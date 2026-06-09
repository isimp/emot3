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
// Legacy-detector calibration: peak |vUp| / horiz split by grounded vs airborne/moving
// (kAirSpeed/kMoveSpeed want a value between the two clusters).
inline float&  VUpPeakAir()       { static float  v = 0.f; return v; }
inline float&  VUpPeakGround()    { static float  v = 0.f; return v; }
inline float&  HorizPeakMove()    { static float  v = 0.f; return v; }
inline float&  HorizPeakStand()   { static float  v = 0.f; return v; }

// Reset every calibration accumulator + history ring (the "Reset history" button).
inline void ClearHistory() {
    VertVelHist().clear(); HorizSpeedHist().clear(); AvatarYHist().clear(); AUpHist().clear();
    Events().clear();
    AirbornePeakAbsVUp() = 0.f; MovingPeakSpeed() = 0.f; AUpSmooth() = 0.f;
    GroundAccelPeak() = 0.f; TakeoffAccelPeak() = 0.f; FallGSum() = 0.0; FallGCount() = 0;
    VUpPeakAir() = 0.f; VUpPeakGround() = 0.f; HorizPeakMove() = 0.f; HorizPeakStand() = 0.f;
}

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
    // Legacy-detector clusters: |vUp| and horiz split by the active airborne/moving flags.
    const float absV = std::fabs(s.vertVel);
    if (s.airborne) { if (absV > VUpPeakAir())    VUpPeakAir()    = absV; }
    else            { if (absV > VUpPeakGround()) VUpPeakGround() = absV; }
    if (s.moving)   { if (s.horizSpeed > HorizPeakMove())  HorizPeakMove()  = s.horizSpeed; }
    else            { if (s.horizSpeed > HorizPeakStand()) HorizPeakStand() = s.horizSpeed; }
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

    // Not AlwaysAutoResize: the panel is tall, so cap height to the viewport and let
    // it SCROLL instead of overflowing off-screen. Sections below are collapsible so
    // it stays short by default.
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::SetNextWindowSize(ImVec2(380.f, 520.f), ImGuiCond_FirstUseEver);
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSizeConstraints(ImVec2(320.f, 140.f), ImVec2(FLT_MAX, ds.y * 0.92f));
    if (!ImGui::Begin("emot3 airborne tuner##airtuner", &airtuner::Enabled(),
                       ImGuiWindowFlags_NoNav)) {
        ImGui::End();
        return;
    }

    const AirtunerSample& s   = airtuner::LastSample();
    const bool   have = airtuner::HaveSample();
    // s.now is CharacterState's steady_clock stamp (NOT ImGui::GetTime - unrelated
    // epoch); use it so the elapsed/timer readouts come out in real seconds.
    const double now  = s.now;
    const int    mode = cs_constants::DetectorMode();

    // ---- controls (always visible) ----------------------------------
    ImGui::Checkbox("Freeze", &airtuner::Paused());
    ImGui::SameLine(); if (ImGui::Button("Reset history"))      airtuner::ClearHistory();
    ImGui::SameLine(); if (ImGui::Button("Reset to defaults"))  airtuner::ResetDefaults();
    ImGui::SameLine(); if (ImGui::Button("Copy as C++"))
        ImGui::SetClipboardText(airtuner::CurrentValuesAsCpp().c_str());
    {
        int& m = cs_constants::DetectorMode();
        ImGui::TextUnformatted("Active gate:"); ImGui::SameLine();
        ImGui::RadioButton("legacy", &m, 0);    ImGui::SameLine();
        ImGui::RadioButton("ballistic", &m, 1);
    }
    ImGui::Separator();

    // ---- live state (always visible, compact) -----------------------
    devui::StateDot(s.airborneLegacy);    ImGui::SameLine();
    ImGui::Text("legacy%s", mode == 0 ? " *" : "");   ImGui::SameLine(150.f);
    devui::StateDot(s.airborneBallistic); ImGui::SameLine();
    ImGui::Text("ballistic%s", mode == 1 ? " *" : "");
    devui::StateDot(s.moving); ImGui::SameLine(); ImGui::Text("moving");
    if (have)
        ImGui::Text("vUp %+.2f   horiz %.2f   aUp %+.0f m/s2",
                    s.vertVel, s.horizSpeed, airtuner::AUpSmooth());
    else
        ImGui::TextDisabled("(no sample yet - waiting for a fresh UITick)");

    // ---- Calibrate: measure, then Apply (default open) --------------
    if (ImGui::CollapsingHeader("Calibrate  (move around, then Apply)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        // Legacy: |vUp| and horiz split into grounded vs airborne/moving clusters;
        // each threshold wants a value BETWEEN its two clusters.
        const float vG = airtuner::VUpPeakGround(), vA = airtuner::VUpPeakAir();
        const float hS = airtuner::HorizPeakStand(), hM = airtuner::HorizPeakMove();
        const float sugAir = (vA > vG) ? 0.5f * (vA + vG) : 0.f;
        const float sugMv  = (hM > hS) ? 0.5f * (hM + hS) : 0.f;
        ImGui::TextDisabled("legacy:");
        ImGui::Text("|vUp| ground %.2f  air %.2f  ->  kAirSpeed %.2f", vG, vA, sugAir);
        ImGui::SameLine(); if (ImGui::SmallButton("set##air") && sugAir > 0.1f)
            cs_constants::AirSpeed() = sugAir;
        ImGui::Text("horiz stand %.2f  move %.2f  ->  kMoveSpeed %.2f", hS, hM, sugMv);
        ImGui::SameLine(); if (ImGui::SmallButton("set##mv") && sugMv > 0.05f)
            cs_constants::MoveSpeed() = sugMv;

        // Ballistic: fall g (avg -aUp descending) + the accel clusters for kLaunchAccel.
        const float fg    = airtuner::MeasuredFallG();
        const float gP    = airtuner::GroundAccelPeak(), tP = airtuner::TakeoffAccelPeak();
        const float sugLA = (tP > gP) ? 0.5f * (tP + gP) : 0.f;
        ImGui::Spacing(); ImGui::TextDisabled("ballistic:");
        ImGui::Text("fall g %.1f  ->  kGravity", fg);
        ImGui::SameLine(); if (ImGui::SmallButton("set##g") && fg > 1.f)
            cs_constants::Gravity() = fg;
        ImGui::Text("accel ground %.0f  takeoff %.0f  ->  kLaunchAccel %.0f", gP, tP, sugLA);
        ImGui::SameLine(); if (ImGui::SmallButton("set##la") && sugLA > 1.f)
            cs_constants::LaunchAccel() = sugLA;
    }

    // ---- Knobs (default closed) -------------------------------------
    if (ImGui::CollapsingHeader("Knobs (sliders)")) {
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
        static const double kDoubleMin = 0.0, kDoubleMax = 1.0;

        ImGui::PushItemWidth(170.f);
        ImGui::TextDisabled("shared / legacy");
        ImGui::SliderFloat("kAirSpeed (|vUp| m/s)",      &air, 0.5f, 10.0f, "%.2f");
        ImGui::SliderInt  ("kFallEngageTicks",           &fet, 1,    10);
        ImGui::SliderInt  ("kRiseEngageTicks",           &ret, 1,    10);   // 1 = no debounce
        ImGui::SliderFloat("kClimbSlopeMax (vUp/horiz)", &csm, 0.0f, 3.0f, "%.2f"); // 0 = off
        ImGui::SliderScalar("kReleaseSec (s)",  ImGuiDataType_Double, &rs, &kDoubleMin, &kDoubleMax, "%.3f");
        ImGui::SliderFloat("kMoveSpeed (horiz m/s)",     &mvs, 0.1f, 5.0f, "%.2f");
        ImGui::SliderScalar("kMoveReleaseSec (s)", ImGuiDataType_Double, &mrs, &kDoubleMin, &kDoubleMax, "%.3f");
        ImGui::Spacing(); ImGui::TextDisabled("ballistic (aUp ~ -g)");
        ImGui::SliderFloat("kLaunchAccel (m/s^2)",       &lac, 20.f, 600.f, "%.0f");
        ImGui::SliderFloat("kGravity (m/s^2)",           &grav, 5.f, 60.f, "%.1f");
        ImGui::SliderFloat("kGravityTol (frac)",         &gtol, 0.05f, 1.0f, "%.2f");
        ImGui::PopItemWidth();
    }

    // ---- Timers & counters (default closed) -------------------------
    if (ImGui::CollapsingHeader("Timers & counters")) {
        ImGui::Text("downRun %d/%d   upRun %d/%d   ballFall %d/%d",
                    s.downRun, cs_constants::FallEngageTicks(),
                    s.upRun,   cs_constants::RiseEngageTicks(),
                    s.ballFall, cs_constants::FallEngageTicks());
        const float ratio = s.horizSpeed > 0.01f ? s.vertVel / s.horizSpeed : 0.f;
        ImGui::Text("rise ratio %+.2f / %.2f   (dt %.1f ms, UITick d %u)",
                    ratio, cs_constants::ClimbSlopeMax(), s.dt * 1000.0, s.uiTickDelta);
        if (have && s.groundSince >= 0.0) {
            float frac = (float)((now - s.groundSince) / cs_constants::ReleaseSec());
            if (frac > 1.f) frac = 1.f;
            char buf[48]; std::snprintf(buf, sizeof(buf), "groundSince %.2f / %.2f s",
                          now - s.groundSince, cs_constants::ReleaseSec());
            ImGui::ProgressBar(frac, ImVec2(180.f, 0), buf);
        } else ImGui::TextDisabled("groundSince -");
        if (have && s.stillSince >= 0.0) {
            float frac = (float)((now - s.stillSince) / cs_constants::MoveReleaseSec());
            if (frac > 1.f) frac = 1.f;
            char buf[48]; std::snprintf(buf, sizeof(buf), "stillSince %.2f / %.2f s",
                          now - s.stillSince, cs_constants::MoveReleaseSec());
            ImGui::ProgressBar(frac, ImVec2(180.f, 0), buf);
        } else ImGui::TextDisabled("stillSince -");
    }

    // ---- Graphs (default closed) ------------------------------------
    if (ImGui::CollapsingHeader("Graphs")) {
        ImGui::TextDisabled("history ~2s @ 60Hz; long falls scroll off the left");
        const ImVec2 plotSize(340, 52);

        // Vertical velocity, symmetric, ±kAirSpeed lines.
        {
            float buf[prof::kHistLen];
            int n = prof::FlattenRing(airtuner::VertVelHist(), buf);
            const float air = cs_constants::AirSpeed();
            float absMax = air * 2.f;
            for (int i = 0; i < n; ++i) { float a = std::fabs(buf[i]); if (a > absMax) absMax = a; }
            const float sMin = -absMax, sMax = absMax;
            char overlay[48]; std::snprintf(overlay, sizeof(overlay),
                "vert vel  cur %+.2f  \xc2\xb1%.1f m/s", n ? buf[n-1] : 0.f, absMax);
            ImGui::PlotLines("##vvplot", buf, n, 0, overlay, sMin, sMax, plotSize);
            devui::DrawThresholdOnLastPlot( air, sMin, sMax - sMin, IM_COL32(220, 70, 70, 200));
            devui::DrawThresholdOnLastPlot(-air, sMin, sMax - sMin, IM_COL32(220, 70, 70, 200));
        }
        // Vertical acceleration (smoothed), -g line green / +g faint.
        {
            float buf[prof::kHistLen];
            int n = prof::FlattenRing(airtuner::AUpHist(), buf);
            const float g = cs_constants::Gravity();
            float absMax = g * 1.5f, vmin = 0.f;
            for (int i = 0; i < n; ++i) {
                float a = std::fabs(buf[i]); if (a > absMax) absMax = a;
                if (buf[i] < vmin) vmin = buf[i];
            }
            const float sMin = -absMax, sMax = absMax;
            char overlay[64]; std::snprintf(overlay, sizeof(overlay),
                "vert accel  cur %+.0f  min %+.0f (~ -g)", n ? buf[n-1] : 0.f, vmin);
            ImGui::PlotLines("##auplot", buf, n, 0, overlay, sMin, sMax, plotSize);
            devui::DrawThresholdOnLastPlot(-g, sMin, sMax - sMin, IM_COL32(70, 200, 90, 220));
            devui::DrawThresholdOnLastPlot( g, sMin, sMax - sMin, IM_COL32(220, 70, 70, 120));
        }
        // Horizontal speed, kMoveSpeed line.
        {
            float buf[prof::kHistLen];
            int n = prof::FlattenRing(airtuner::HorizSpeedHist(), buf);
            const float mvs = cs_constants::MoveSpeed();
            float peak = mvs * 2.f;
            for (int i = 0; i < n; ++i) if (buf[i] > peak) peak = buf[i];
            char overlay[48]; std::snprintf(overlay, sizeof(overlay),
                "horiz spd  cur %.2f  0..%.1f m/s", n ? buf[n-1] : 0.f, peak);
            ImGui::PlotLines("##hsplot", buf, n, 0, overlay, 0.f, peak, plotSize);
            devui::DrawThresholdOnLastPlot(mvs, 0.f, peak, IM_COL32(220, 70, 70, 200));
        }
    }

    // ---- Event log (default closed) ---------------------------------
    if (ImGui::CollapsingHeader("Event log")) {
        const airtuner::EventLog& log = airtuner::Events();
        if (log.count == 0) {
            ImGui::TextDisabled("(none yet - jump or walk to log a transition)");
        } else if (ImGui::BeginTable("##airtuner_events", 3,
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("when");
            ImGui::TableSetupColumn("event");
            ImGui::TableSetupColumn("peak");
            ImGui::TableHeadersRow();
            for (int i = 0; i < log.count; ++i) {
                int idx = (log.head - 1 - i + airtuner::kEventLogSize) % airtuner::kEventLogSize;
                const airtuner::Event& e = log.ev[idx];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("-%5.2fs", (float)(now - e.t));
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
    }

    ImGui::End();
}

#endif  // EMOT3_DEVTOOLS
