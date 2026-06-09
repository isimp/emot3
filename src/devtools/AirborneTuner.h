#pragma once
// =====================================================================
//  Dev-only "Airborne tuner" overlay - investigate + tune the airborne /
//  movement thresholds in CharacterState (kAirSpeed / kFallEngageTicks /
//  kReleaseSec / kMoveSpeed / kMoveReleaseSec).
//
//  The existing "Game state" inspector row reads s_vertVel as one number
//  that swings +/-5 in under a second during a jump - unreadable. This
//  tool puts the same signal on a 120-sample sparkline next to a
//  threshold line and live sliders for all 5 constants, so the user can
//  jump / walk / fall in-game and see the decision land.
//
//  Gated by EMOT3_DEVTOOLS - non-dev builds get no-op stubs and the call
//  site in TickCharacterState compiles out. The 5 constants live in
//  cs_constants (CharacterState.h) as constexpr-returning getters in
//  shipped builds and function-local-static-returning-by-reference in
//  dev builds, so the slider's "= newValue" path only exists in dev.
//
//  Touch points: #include "AirborneTuner.h"; the airtuner::OnSample(...)
//  call at the end of TickCharacterState (dev-gated); the
//  RegisterDevTool line in DevTools.cpp. No Options.cpp edit.
// =====================================================================

#ifndef EMOT3_DEVTOOLS

namespace airtuner { inline bool& Enabled() { static bool b = false; return b; } }
struct AirtunerSample {};  // dummy so the OnSample call type-checks
namespace airtuner { inline void OnSample(const AirtunerSample&) {} }
inline void RenderAirborneTuner() {}

#else  // ---- dev build: full implementation ----

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui/imgui.h"

#include "Profiling.h"        // prof::Ring (120-sample rolling buffer reused here)
#include "DevToolsUI.h"       // devui::StateDot / DrawThresholdOnLastPlot
#include "CharacterState.h"   // cs_constants::AirSpeed() etc. - the tunable knobs

// All the data TickCharacterState computes for one fresh game tick, packaged
// for the tuner. Defined at file scope so the call site in CharacterState.cpp
// is `airtuner::OnSample({ ... })` aggregate-init.
struct AirtunerSample {
    float    vertVel;      // m/s, + up
    float    horizSpeed;   // m/s, horizontal
    float    avatarY;      // current avatar Y
    float    dt;           // seconds since last fresh tick
    unsigned uiTickDelta;  // UITick - prev UITick (>= 1)
    bool     airborne;     // s_airborne after this tick
    bool     moving;       // s_moving    after this tick
    int      downRun;      // consecutive fast-descending ticks (legacy)
    int      upRun;        // consecutive unexplained fast-rising ticks (legacy)
    double   groundSince;  // -1 if not running, else when descent began (legacy)
    double   stillSince;   // -1 if not running, else when stillness began
    double   now;          // steady_clock seconds at this sample (CharacterState's clock)
    float    aUp;          // vertical acceleration m/s^2 (ballistic detector signal)
    bool     airborneLegacy;     // detector A result this tick
    bool     airborneBallistic;  // detector B result this tick
    int      ballFall;     // consecutive free-fall (aUp ~ -g) ticks
};

namespace airtuner {

// ---- toggles --------------------------------------------------------
inline bool& Enabled() { static bool b = false; return b; }
inline bool& Paused()  { static bool b = false; return b; }

// ---- per-tick state for the rings + event log -----------------------
// Function-local statics so we don't need a .cpp file. Read/written only on
// the render thread (OnSample is called from TickCharacterState in
// AddonRender, before any other consumer reads). No locks needed.

inline prof::Ring& VertVelHist()    { static prof::Ring r; return r; }
inline prof::Ring& HorizSpeedHist() { static prof::Ring r; return r; }
inline prof::Ring& AvatarYHist()    { static prof::Ring r; return r; }
inline prof::Ring& AUpHist()        { static prof::Ring r; return r; }  // vertical accel

inline AirtunerSample& LastSample() {
    static AirtunerSample s{};
    return s;
}
inline bool& HaveSample() { static bool b = false; return b; }

// Event log: last N transitions of s_airborne and s_moving. Each entry
// records the timestamp + a one-number summary (peak |vUp| during the
// airborne span; peak horizSpeed during the moving span).
struct Event {
    double t;          // ImGui::GetTime() when the transition happened
    bool   isAirborne; // true = airborne edge; false = moving edge
    bool   on;         // true = went ON, false = went OFF
    float  peak;       // |peak vUp| (airborne) or peak horizSpeed (moving), captured at the OFF edge
};
static constexpr int kEventLogSize = 10;
struct EventLog {
    Event ev[kEventLogSize];
    int   head  = 0;  // next write slot
    int   count = 0;
    void  push(const Event& e) {
        ev[head] = e;
        head = (head + 1) % kEventLogSize;
        if (count < kEventLogSize) ++count;
    }
    void  clear() { head = 0; count = 0; }
};
inline EventLog& Events() { static EventLog l; return l; }

// Edge-detection + peak-during-span tracking. Updated by OnSample.
inline bool&  PrevAirborne()    { static bool  b = false; return b; }
inline bool&  PrevMoving()      { static bool  b = false; return b; }
inline float& AirbornePeakAbsVUp() { static float v = 0.f; return v; }  // |vUp| max while airborne
inline float& MovingPeakSpeed()    { static float v = 0.f; return v; }

// ---- calibration aids (cleared by "Reset history") ------------------
// The raw aUp trace is jerky; reading kGravity/kLaunchAccel off it by eye is hard.
// Instead we MEASURE: AUpSmooth is a cosmetic EMA for the plot/live number (the
// detector still uses raw aUp). The rest are running stats since the last reset:
// fall g = avg -aUp while ballistic-descending; takeoff/ground peaks = max aUp seen
// airborne vs grounded - kLaunchAccel wants a value between those two clusters.
inline float&  AUpSmooth()        { static float  v = 0.f; return v; }
inline float&  GroundAccelPeak()  { static float  v = 0.f; return v; }
inline float&  TakeoffAccelPeak() { static float  v = 0.f; return v; }
inline double& FallGSum()         { static double v = 0.0; return v; }
inline int&    FallGCount()       { static int    v = 0;   return v; }
inline float   MeasuredFallG()    { return FallGCount() ? (float)(FallGSum() / FallGCount()) : 0.f; }

// ---- the per-tick callback (called from TickCharacterState) ---------

inline void OnSample(const AirtunerSample& s) {
    if (Paused()) {
        LastSample() = s;       // keep the live readouts current even when paused
        HaveSample() = true;
        return;
    }

    VertVelHist().push(s.vertVel);
    HorizSpeedHist().push(s.horizSpeed);
    AvatarYHist().push(s.avatarY);
    // Cosmetic EMA so the accel plot/readout is legible (detector uses raw aUp).
    AUpSmooth() += 0.3f * (s.aUp - AUpSmooth());
    AUpHist().push(AUpSmooth());
    // Calibration stats from RAW aUp (what the detector tests against).
    if (s.airborneBallistic) {
        if (s.aUp > TakeoffAccelPeak()) TakeoffAccelPeak() = s.aUp;
        if (s.vertVel < -0.5f) { FallGSum() += -s.aUp; FallGCount() += 1; }  // descending: aUp ~ -g
    } else {
        if (s.aUp > GroundAccelPeak()) GroundAccelPeak() = s.aUp;
    }
    LastSample() = s;
    HaveSample() = true;

    // Edge-detect + log. Peak captured at the OFF edge.
    if (s.airborne) {
        float absV = std::fabs(s.vertVel);
        if (absV > AirbornePeakAbsVUp()) AirbornePeakAbsVUp() = absV;
    }
    if (s.moving) {
        if (s.horizSpeed > MovingPeakSpeed()) MovingPeakSpeed() = s.horizSpeed;
    }

    if (s.airborne != PrevAirborne()) {
        Event e{ s.now, /*isAirborne=*/true, /*on=*/s.airborne,
                 s.airborne ? std::fabs(s.vertVel) : AirbornePeakAbsVUp() };
        Events().push(e);
        if (s.airborne) AirbornePeakAbsVUp() = std::fabs(s.vertVel);
        else            AirbornePeakAbsVUp() = 0.f;
        PrevAirborne() = s.airborne;
    }
    if (s.moving != PrevMoving()) {
        Event e{ s.now, /*isAirborne=*/false, /*on=*/s.moving,
                 s.moving ? s.horizSpeed : MovingPeakSpeed() };
        Events().push(e);
        if (s.moving) MovingPeakSpeed() = s.horizSpeed;
        else          MovingPeakSpeed() = 0.f;
        PrevMoving() = s.moving;
    }
}

// ---- rendering helpers ----------------------------------------------
// DrawThresholdOnLastPlot / StateDot now live in DevToolsUI.h (namespace devui),
// and FlattenRing in Profiling.h (namespace prof) - both shared with the memory
// monitor. Call sites below use those.

// Build the "constexpr float kAirSpeed = X.XXf; ..." text from current slider
// values, ready to paste back into CharacterState.h.
inline std::string CurrentValuesAsCpp() {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "// Paste into CharacterState.h's cs_constants getter bodies:\n"
        "//   AirSpeed:        %.2ff\n"
        "//   FallEngageTicks: %d\n"
        "//   RiseEngageTicks: %d\n"
        "//   ClimbSlopeMax:   %.2ff\n"
        "//   ReleaseSec:      %.3f\n"
        "//   MoveSpeed:       %.2ff\n"
        "//   MoveReleaseSec:  %.3f\n"
        "//   LaunchAccel:     %.1ff\n"
        "//   Gravity:         %.1ff\n"
        "//   GravityTol:      %.2ff\n",
        cs_constants::AirSpeed(),
        cs_constants::FallEngageTicks(),
        cs_constants::RiseEngageTicks(),
        cs_constants::ClimbSlopeMax(),
        cs_constants::ReleaseSec(),
        cs_constants::MoveSpeed(),
        cs_constants::MoveReleaseSec(),
        cs_constants::LaunchAccel(),
        cs_constants::Gravity(),
        cs_constants::GravityTol());
    return std::string(buf);
}

// Reset all knobs to their shipped defaults (and the detector back to legacy).
inline void ResetDefaults() {
    cs_constants::AirSpeed()        = 3.5f;
    cs_constants::FallEngageTicks() = 2;
    cs_constants::RiseEngageTicks() = 2;
    cs_constants::ClimbSlopeMax()   = 1.2f;
    cs_constants::ReleaseSec()      = 0.25;
    cs_constants::MoveSpeed()       = 1.0f;
    cs_constants::MoveReleaseSec()  = 0.15;
    cs_constants::DetectorMode()    = 0;
    cs_constants::LaunchAccel()     = 150.f;
    cs_constants::Gravity()         = 20.f;
    cs_constants::GravityTol()      = 0.5f;
}

}  // namespace airtuner

// ---- the overlay window ---------------------------------------------

inline void RenderAirborneTuner() {
    if (!airtuner::Enabled()) return;

    ImGui::SetNextWindowBgAlpha(0.88f);
    if (!ImGui::Begin("emot3 airborne tuner##airtuner", &airtuner::Enabled(),
                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav)) {
        ImGui::End();
        return;
    }

    // ---- top controls -----------------------------------------------
    ImGui::Checkbox("Freeze", &airtuner::Paused());
    ImGui::SameLine();
    if (ImGui::Button("Reset history")) {
        airtuner::VertVelHist().clear();
        airtuner::HorizSpeedHist().clear();
        airtuner::AvatarYHist().clear();
        airtuner::AUpHist().clear();
        airtuner::Events().clear();
        airtuner::AirbornePeakAbsVUp() = 0.f;
        airtuner::MovingPeakSpeed()    = 0.f;
        airtuner::AUpSmooth()        = 0.f;
        airtuner::GroundAccelPeak()  = 0.f;
        airtuner::TakeoffAccelPeak() = 0.f;
        airtuner::FallGSum()         = 0.0;
        airtuner::FallGCount()       = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults")) airtuner::ResetDefaults();
    ImGui::SameLine();
    if (ImGui::Button("Copy as C++")) {
        ImGui::SetClipboardText(airtuner::CurrentValuesAsCpp().c_str());
    }

    // Active detector: which one drives the real airborne gate (s_airborne).
    // Both always compute so the readouts/dots below compare them side by side.
    {
        int& mode = cs_constants::DetectorMode();
        ImGui::TextUnformatted("Active gate:"); ImGui::SameLine();
        ImGui::RadioButton("legacy", &mode, 0);    ImGui::SameLine();
        ImGui::RadioButton("ballistic", &mode, 1);
    }

    ImGui::TextDisabled("history ~2s @ 60Hz; long falls scroll off the left edge");
    ImGui::Separator();

    // ---- sliders ----------------------------------------------------
    {
        float& air  = cs_constants::AirSpeed();
        int&   fet  = cs_constants::FallEngageTicks();
        int&   ret  = cs_constants::RiseEngageTicks();
        float& csm  = cs_constants::ClimbSlopeMax();
        double& rs  = cs_constants::ReleaseSec();
        float& mvs  = cs_constants::MoveSpeed();
        double& mrs = cs_constants::MoveReleaseSec();
        float& lac  = cs_constants::LaunchAccel();
        float& grav = cs_constants::Gravity();
        float& gtol = cs_constants::GravityTol();

        // ImGui::SliderScalar wants &min / &max as void*. Stable storage so we
        // can take addresses; static const because the bounds don't change.
        static const double kDoubleMin = 0.0;
        static const double kDoubleMax = 1.0;

        ImGui::PushItemWidth(180.f);
        ImGui::TextDisabled("shared / legacy detector");
        ImGui::SliderFloat("kAirSpeed (|vUp| m/s)",   &air, 0.5f, 10.0f, "%.2f");
        ImGui::SliderInt  ("kFallEngageTicks",        &fet, 1,    10);
        ImGui::SliderInt  ("kRiseEngageTicks",        &ret, 1,    10);   // 1 = no debounce
        ImGui::SliderFloat("kClimbSlopeMax (vUp/horiz)", &csm, 0.0f, 3.0f, "%.2f"); // 0 = gate off
        ImGui::SliderScalar("kReleaseSec (s)",         ImGuiDataType_Double, &rs,
                             &kDoubleMin, &kDoubleMax, "%.3f");
        ImGui::SliderFloat("kMoveSpeed (horiz m/s)",  &mvs, 0.1f, 5.0f, "%.2f");
        ImGui::SliderScalar("kMoveReleaseSec (s)",     ImGuiDataType_Double, &mrs,
                             &kDoubleMin, &kDoubleMax, "%.3f");
        ImGui::Spacing();
        ImGui::TextDisabled("ballistic detector (aUp ~ -g)");
        ImGui::SliderFloat("kLaunchAccel (m/s^2)",    &lac, 20.f, 600.f, "%.0f");
        ImGui::SliderFloat("kGravity (m/s^2)",        &grav, 5.f, 60.f, "%.1f");
        ImGui::SliderFloat("kGravityTol (frac)",      &gtol, 0.05f, 1.0f, "%.2f");
        ImGui::PopItemWidth();
    }

    ImGui::Separator();

    // ---- measured calibration aids (so you don't read the graph) ----
    // Fall once, jump a few times, then click Apply. Numbers accumulate since the
    // last "Reset history"; kLaunchAccel wants to sit between ground & takeoff peaks.
    {
        const float fg     = airtuner::MeasuredFallG();
        const float gPeak  = airtuner::GroundAccelPeak();
        const float tPeak  = airtuner::TakeoffAccelPeak();
        const float suggLA = (tPeak > gPeak) ? 0.5f * (tPeak + gPeak) : 0.f;

        ImGui::TextDisabled("measured (since reset) - calibrate, then Apply:");
        ImGui::Text("fall g       %5.1f m/s^2", fg);
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply -> kGravity") && fg > 1.f)
            cs_constants::Gravity() = fg;

        ImGui::Text("ground peak  %5.0f   takeoff peak %5.0f m/s^2", gPeak, tPeak);
        ImGui::Text("suggest kLaunchAccel %5.0f", suggLA);
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply -> kLaunchAccel") && suggLA > 1.f)
            cs_constants::LaunchAccel() = suggLA;
    }

    ImGui::Separator();

    // ---- live readouts ----------------------------------------------
    const AirtunerSample& s = airtuner::LastSample();
    const bool have = airtuner::HaveSample();
    // Use the SAMPLE's clock as "now", not ImGui::GetTime(): groundSince/stillSince
    // and the event timestamps come from CharacterState's std::chrono::steady_clock
    // (kept imgui-free), whose epoch is unrelated to ImGui's - mixing them made the
    // elapsed readouts show huge values. s.now is the steady_clock stamp of the
    // latest tick (kept current even while paused).
    const double now = s.now;

    if (ImGui::BeginTable("##airtuner_live", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextRow();

        // Left column: scalar signals
        ImGui::TableSetColumnIndex(0);
        if (have) {
            ImGui::Text("vert vel     %+7.2f m/s",  s.vertVel);
            ImGui::Text("horiz spd    %7.2f m/s",   s.horizSpeed);
            ImGui::Text("vert accel   %+8.1f m/s2", airtuner::AUpSmooth());
            ImGui::Text("Avatar Y     %7.2f",       s.avatarY);
            ImGui::Text("dt           %7.2f ms",    s.dt * 1000.0);
            ImGui::Text("UITick Δ     %u",           s.uiTickDelta);
        } else {
            ImGui::TextDisabled("(no sample yet - waiting for fresh UITick)");
        }

        // Right column: flags + timers. Show BOTH detectors' results; mark the one
        // DetectorMode currently routes to s_airborne.
        ImGui::TableSetColumnIndex(1);
        const int mode = cs_constants::DetectorMode();
        devui::StateDot(s.airborneLegacy);    ImGui::SameLine();
        ImGui::Text("airborne: legacy%s",    mode == 0 ? " (active)" : "");
        devui::StateDot(s.airborneBallistic); ImGui::SameLine();
        ImGui::Text("airborne: ballistic%s", mode == 1 ? " (active)" : "");
        devui::StateDot(s.moving);   ImGui::SameLine();
        ImGui::Text("moving");
        ImGui::Text("downRun      %d / %d", s.downRun, cs_constants::FallEngageTicks());
        ImGui::Text("upRun        %d / %d", s.upRun,   cs_constants::RiseEngageTicks());
        ImGui::Text("ballFall     %d / %d", s.ballFall, cs_constants::FallEngageTicks());
        {
            // Rise "explained by climbing" ratio: a launch needs vUp/horiz above
            // kClimbSlopeMax (stairs/ramps stay at/under the surface slope).
            float ratio = s.horizSpeed > 0.01f ? s.vertVel / s.horizSpeed : 0.f;
            ImGui::Text("rise ratio   %+6.2f / %.2f", ratio, cs_constants::ClimbSlopeMax());
        }
        if (have && s.groundSince >= 0.0) {
            float frac = (float)((now - s.groundSince) / cs_constants::ReleaseSec());
            if (frac > 1.f) frac = 1.f;
            char buf[48];
            std::snprintf(buf, sizeof(buf), "groundSince  %.2fs / %.2f",
                          now - s.groundSince, cs_constants::ReleaseSec());
            ImGui::ProgressBar(frac, ImVec2(160.f, 0), buf);
        } else {
            ImGui::TextDisabled("groundSince  -");
        }
        if (have && s.stillSince >= 0.0) {
            float frac = (float)((now - s.stillSince) / cs_constants::MoveReleaseSec());
            if (frac > 1.f) frac = 1.f;
            char buf[48];
            std::snprintf(buf, sizeof(buf), "stillSince   %.2fs / %.2f",
                          now - s.stillSince, cs_constants::MoveReleaseSec());
            ImGui::ProgressBar(frac, ImVec2(160.f, 0), buf);
        } else {
            ImGui::TextDisabled("stillSince   -");
        }
        ImGui::EndTable();
    }

    ImGui::Separator();

    // ---- sparklines -------------------------------------------------
    const ImVec2 plotSize(360, 56);

    // Vertical velocity. Symmetric scale, ±max(kAirSpeed*2, recent peak).
    {
        float buf[prof::kHistLen];
        int n = prof::FlattenRing(airtuner::VertVelHist(), buf);
        const float air = cs_constants::AirSpeed();
        // Symmetric scale: ±max(2*threshold, |buffer extreme|). prof::Ring's
        // .peak() returns the positive windowed max; vUp goes negative on a
        // fall, so scan the buffer directly for the absolute extreme.
        float absMax = air * 2.f;
        for (int i = 0; i < n; ++i) {
            float a = std::fabs(buf[i]);
            if (a > absMax) absMax = a;
        }
        const float sMin = -absMax, sMax = absMax;
        char overlay[48]; std::snprintf(overlay, sizeof(overlay),
            "vert vel  cur %+.2f  range \xc2\xb1%.1f m/s", n ? buf[n-1] : 0.f, absMax);
        ImGui::PlotLines("##vvplot", buf, n, 0, overlay, sMin, sMax, plotSize);
        // ±kAirSpeed threshold lines (red so they pop)
        devui::DrawThresholdOnLastPlot( air, sMin, sMax - sMin, IM_COL32(220, 70, 70, 200));
        devui::DrawThresholdOnLastPlot(-air, sMin, sMax - sMin, IM_COL32(220, 70, 70, 200));
    }

    // Vertical acceleration (ballistic signal). Symmetric scale; -kGravity line is
    // where free-fall sits (calibrate kGravity to the descending plateau / "min").
    // The windowed min ~= the real free-fall accel = the gravity to dial in.
    {
        float buf[prof::kHistLen];
        int n = prof::FlattenRing(airtuner::AUpHist(), buf);
        const float g = cs_constants::Gravity();
        float absMax = g * 1.5f, vmin = 0.f;
        for (int i = 0; i < n; ++i) {
            float a = std::fabs(buf[i]);
            if (a > absMax) absMax = a;
            if (buf[i] < vmin) vmin = buf[i];
        }
        const float sMin = -absMax, sMax = absMax;
        char overlay[64]; std::snprintf(overlay, sizeof(overlay),
            "vert accel  cur %+.0f  min %+.0f m/s2 (~ -g)", n ? buf[n-1] : 0.f, vmin);
        ImGui::PlotLines("##auplot", buf, n, 0, overlay, sMin, sMax, plotSize);
        // -g (free-fall) line in green; +g mirrored in faint red for context.
        devui::DrawThresholdOnLastPlot(-g, sMin, sMax - sMin, IM_COL32(70, 200, 90, 220));
        devui::DrawThresholdOnLastPlot( g, sMin, sMax - sMin, IM_COL32(220, 70, 70, 120));
    }

    // Horizontal speed. 0..max(kMoveSpeed*2, recent peak).
    {
        float buf[prof::kHistLen];
        int n = prof::FlattenRing(airtuner::HorizSpeedHist(), buf);
        const float mvs = cs_constants::MoveSpeed();
        float peak = mvs * 2.f;
        for (int i = 0; i < n; ++i) if (buf[i] > peak) peak = buf[i];
        const float sMin = 0.f, sMax = peak;
        char overlay[48]; std::snprintf(overlay, sizeof(overlay),
            "horiz spd  cur %.2f  range 0..%.1f m/s", n ? buf[n-1] : 0.f, peak);
        ImGui::PlotLines("##hsplot", buf, n, 0, overlay, sMin, sMax, plotSize);
        devui::DrawThresholdOnLastPlot(mvs, sMin, sMax - sMin,
                                           IM_COL32(220, 70, 70, 200));
    }

    // Avatar Y. Auto-scaled (FLT_MAX) - spatial context for the velocity plot.
    {
        float buf[prof::kHistLen];
        int n = prof::FlattenRing(airtuner::AvatarYHist(), buf);
        char overlay[48]; std::snprintf(overlay, sizeof(overlay),
            "Avatar Y  cur %.2f", n ? buf[n-1] : 0.f);
        ImGui::PlotLines("##yplot", buf, n, 0, overlay, FLT_MAX, FLT_MAX, plotSize);
    }

    ImGui::Separator();

    // ---- event log --------------------------------------------------
    ImGui::Text("recent transitions (newest at top)");
    const airtuner::EventLog& log = airtuner::Events();
    if (log.count == 0) {
        ImGui::TextDisabled("  (none yet - jump or walk to log a transition)");
    } else if (ImGui::BeginTable("##airtuner_events", 3,
                                  ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("when");
        ImGui::TableSetupColumn("event");
        ImGui::TableSetupColumn("peak");
        ImGui::TableHeadersRow();
        // Iterate newest-first.
        for (int i = 0; i < log.count; ++i) {
            int idx = (log.head - 1 - i + airtuner::kEventLogSize) % airtuner::kEventLogSize;
            const airtuner::Event& e = log.ev[idx];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("-%5.2fs", (float)(now - e.t));
            ImGui::TableSetColumnIndex(1);
            const char* k = e.isAirborne ? "airborne" : "moving";
            ImVec4 col = e.on ? ImVec4(1.f, 0.6f, 0.6f, 1.f) : ImVec4(0.7f, 0.7f, 0.7f, 1.f);
            ImGui::TextColored(col, "%s %s", k, e.on ? "ON" : "OFF");
            ImGui::TableSetColumnIndex(2);
            if (e.isAirborne) ImGui::Text("|vUp| %.2f", e.peak);
            else              ImGui::Text("%.2f m/s",  e.peak);
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

#endif  // EMOT3_DEVTOOLS
