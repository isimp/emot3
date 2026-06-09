#pragma once
// =====================================================================
//  Dev-only "Airborne tuner" overlay - investigate + tune the airborne /
//  movement detector in CharacterState (cs_constants).
//
//  The plain "Game state" row reads s_vertVel as one number that swings
//  +/-5 in under a second during a jump - unreadable. This tool puts the
//  signals on sparklines next to their threshold lines, MEASURES the
//  clusters you care about (so you don't read the graph by eye), and gives
//  one-click "set" buttons + live sliders for every knob. Sections are
//  collapsible and the window scrolls, so it never overflows the screen.
//
//  Gated by EMOT3_DEVTOOLS - non-dev builds get no-op stubs and the call
//  site in TickCharacterState compiles out. The knobs live in cs_constants
//  (CharacterState.h) as ref-returning getters in dev builds (sliders write
//  them) and constexpr in shipped builds.
//
//  Touch points: #include "AirborneTuner.h"; airtuner::OnSample(...) at the
//  end of TickCharacterState (dev-gated); RegisterDevTool in DevTools.cpp.
// =====================================================================

#ifndef EMOT3_DEVTOOLS

namespace airtuner { inline bool& Enabled() { static bool b = false; return b; } }
struct AirtunerSample {};  // dummy so the OnSample call type-checks
namespace airtuner { inline void OnSample(const AirtunerSample&) {} }
inline void RenderAirborneTuner() {}

#else  // ---- dev build: full implementation ----

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui/imgui.h"

#include "Profiling.h"        // prof::Ring (120-sample rolling buffer reused here)
#include "DevToolsUI.h"       // devui::StateDot / DrawThresholdOnLastPlot
#include "CharacterState.h"   // cs_constants::AirSpeed() etc. - the tunable knobs

// One fresh game tick's data, packaged for the tuner. File scope so the call
// site in CharacterState.cpp is `airtuner::OnSample({ ... })` aggregate-init.
struct AirtunerSample {
    float    vertVel;      // m/s, + up (raw, single-tick)
    float    vertVelEMA;   // m/s, smoothed (the marginal-decision signal)
    float    horizSpeed;   // m/s, horizontal (smoothed; the moving signal)
    float    avatarY;      // current avatar Y
    float    dt;           // seconds since last fresh tick
    unsigned uiTickDelta;  // UITick - prev UITick (>= 1)
    bool     airborne;     // s_airborne after this tick
    bool     moving;       // s_moving    after this tick
    double   riseSince;    // -1 if not running, else when the marginal rise began
    double   fallSince;    // -1 if not running, else when the marginal fall began
    double   groundSince;  // -1 if not running, else when descent/settle began
    double   stillSince;   // -1 if not running, else when stillness began
    double   now;          // steady_clock seconds at this sample (CharacterState's clock)
};

namespace airtuner {

// ---- toggles --------------------------------------------------------
inline bool& Enabled() { static bool b = false; return b; }
inline bool& Paused()  { static bool b = false; return b; }

// ---- rolling history (render-thread only; no locks) -----------------
inline prof::Ring& VertVelHist()    { static prof::Ring r; return r; }  // smoothed vUp
inline prof::Ring& HorizSpeedHist() { static prof::Ring r; return r; }

inline AirtunerSample& LastSample() { static AirtunerSample s{}; return s; }
inline bool& HaveSample() { static bool b = false; return b; }

// Event log: last N airborne/moving transitions, with the peak captured at OFF.
struct Event { double t; bool isAirborne; bool on; float peak; };
static constexpr int kEventLogSize = 10;
struct EventLog {
    Event ev[kEventLogSize];
    int   head = 0, count = 0;
    void  push(const Event& e) { ev[head] = e; head = (head + 1) % kEventLogSize;
                                 if (count < kEventLogSize) ++count; }
    void  clear() { head = 0; count = 0; }
};
inline EventLog& Events() { static EventLog l; return l; }
inline bool&  PrevAirborne()       { static bool  b = false; return b; }
inline bool&  PrevMoving()         { static bool  b = false; return b; }
inline float& AirbornePeakAbsVUp() { static float v = 0.f; return v; }
inline float& MovingPeakSpeed()    { static float v = 0.f; return v; }

// ---- calibration aids (cleared by "Reset history") ------------------
// Peak |vUp| / horiz split into grounded-vs-airborne / standing-vs-moving clusters
// (since the last reset). kAirSpeed/kMoveSpeed want a value BETWEEN their two
// clusters; measured from the SMOOTHED signals the thresholds actually compare.
inline float& VUpPeakAir()     { static float v = 0.f; return v; }
inline float& VUpPeakGround()  { static float v = 0.f; return v; }
inline float& HorizPeakMove()  { static float v = 0.f; return v; }
inline float& HorizPeakStand() { static float v = 0.f; return v; }

inline void ClearHistory() {
    VertVelHist().clear(); HorizSpeedHist().clear(); Events().clear();
    AirbornePeakAbsVUp() = 0.f; MovingPeakSpeed() = 0.f;
    VUpPeakAir() = 0.f; VUpPeakGround() = 0.f; HorizPeakMove() = 0.f; HorizPeakStand() = 0.f;
}

// ---- per-tick callback (from TickCharacterState) --------------------
inline void OnSample(const AirtunerSample& s) {
    if (Paused()) { LastSample() = s; HaveSample() = true; return; }

    VertVelHist().push(s.vertVelEMA);   // plot the signal the detector decides on
    HorizSpeedHist().push(s.horizSpeed);
    LastSample() = s;
    HaveSample() = true;

    // Calibration clusters (smoothed signals).
    const float av = std::fabs(s.vertVelEMA);
    if (s.airborne) { if (av > VUpPeakAir())    VUpPeakAir()    = av; }
    else            { if (av > VUpPeakGround()) VUpPeakGround() = av; }
    if (s.moving)   { if (s.horizSpeed > HorizPeakMove())  HorizPeakMove()  = s.horizSpeed; }
    else            { if (s.horizSpeed > HorizPeakStand()) HorizPeakStand() = s.horizSpeed; }

    // Event log: edge-detect, peak captured at the OFF edge.
    if (s.airborne) { float a = std::fabs(s.vertVel); if (a > AirbornePeakAbsVUp()) AirbornePeakAbsVUp() = a; }
    if (s.moving)   { if (s.horizSpeed > MovingPeakSpeed()) MovingPeakSpeed() = s.horizSpeed; }
    if (s.airborne != PrevAirborne()) {
        Events().push({ s.now, true, s.airborne,
                        s.airborne ? std::fabs(s.vertVel) : AirbornePeakAbsVUp() });
        AirbornePeakAbsVUp() = s.airborne ? std::fabs(s.vertVel) : 0.f;
        PrevAirborne() = s.airborne;
    }
    if (s.moving != PrevMoving()) {
        Events().push({ s.now, false, s.moving,
                        s.moving ? s.horizSpeed : MovingPeakSpeed() });
        MovingPeakSpeed() = s.moving ? s.horizSpeed : 0.f;
        PrevMoving() = s.moving;
    }
}

// Paste-back text for the current knob values.
inline std::string CurrentValuesAsCpp() {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "// Paste into CharacterState.h's cs_constants getter bodies:\n"
        "//   AirSpeed:        %.2ff\n"
        "//   FastLaunchMult:  %.2ff\n"
        "//   ClimbSlopeMax:   %.2ff\n"
        "//   VelWindowSec:    %.3f\n"
        "//   RiseEngageSec:   %.3f\n"
        "//   FallEngageSec:   %.3f\n"
        "//   ReleaseSec:      %.3f\n"
        "//   MoveSpeed:       %.2ff\n"
        "//   MoveReleaseSec:  %.3f\n",
        cs_constants::AirSpeed(), cs_constants::FastLaunchMult(), cs_constants::ClimbSlopeMax(),
        cs_constants::VelWindowSec(), cs_constants::RiseEngageSec(), cs_constants::FallEngageSec(),
        cs_constants::ReleaseSec(), cs_constants::MoveSpeed(), cs_constants::MoveReleaseSec());
    return std::string(buf);
}

inline void ResetDefaults() {
    cs_constants::AirSpeed()       = 3.5f;
    cs_constants::FastLaunchMult() = 2.0f;
    cs_constants::ClimbSlopeMax()  = 1.2f;
    cs_constants::VelWindowSec()   = 0.04;
    cs_constants::RiseEngageSec()  = 0.05;
    cs_constants::FallEngageSec()  = 0.05;
    cs_constants::ReleaseSec()     = 0.25;
    cs_constants::MoveSpeed()      = 1.0f;
    cs_constants::MoveReleaseSec() = 0.15;
}

}  // namespace airtuner

// ---- the overlay window ---------------------------------------------

inline void RenderAirborneTuner() {
    if (!airtuner::Enabled()) return;

    // Not AlwaysAutoResize: cap height to the viewport and SCROLL, so it never runs
    // off-screen. Sections collapse to keep it short by default.
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::SetNextWindowSize(ImVec2(360.f, 480.f), ImGuiCond_FirstUseEver);
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.f, 140.f), ImVec2(FLT_MAX, ds.y * 0.92f));
    if (!ImGui::Begin("emot3 airborne tuner##airtuner", &airtuner::Enabled(),
                       ImGuiWindowFlags_NoNav)) {
        ImGui::End();
        return;
    }

    const AirtunerSample& s = airtuner::LastSample();
    const bool   have = airtuner::HaveSample();
    const double now  = s.now;   // steady_clock stamp (NOT ImGui::GetTime)

    // ---- controls (always visible) ----------------------------------
    ImGui::Checkbox("Freeze", &airtuner::Paused());
    ImGui::SameLine(); if (ImGui::Button("Reset history"))     airtuner::ClearHistory();
    ImGui::SameLine(); if (ImGui::Button("Reset to defaults")) airtuner::ResetDefaults();
    ImGui::SameLine(); if (ImGui::Button("Copy as C++"))
        ImGui::SetClipboardText(airtuner::CurrentValuesAsCpp().c_str());
    ImGui::Separator();

    // ---- live state (always visible) --------------------------------
    devui::StateDot(s.airborne); ImGui::SameLine(); ImGui::Text("airborne");
    ImGui::SameLine(150.f);
    devui::StateDot(s.moving);   ImGui::SameLine(); ImGui::Text("moving");
    if (have)
        ImGui::Text("vUp %+.2f (sm %+.2f)   horiz %.2f m/s",
                    s.vertVel, s.vertVelEMA, s.horizSpeed);
    else
        ImGui::TextDisabled("(no sample yet - waiting for a fresh UITick)");

    // ---- Calibrate: measure, then set (default open) ----------------
    if (ImGui::CollapsingHeader("Calibrate  (move around, then set)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const float vG = airtuner::VUpPeakGround(), vA = airtuner::VUpPeakAir();
        const float hS = airtuner::HorizPeakStand(), hM = airtuner::HorizPeakMove();
        const float sugAir = (vA > vG) ? 0.5f * (vA + vG) : 0.f;
        const float sugMv  = (hM > hS) ? 0.5f * (hM + hS) : 0.f;
        ImGui::Text("|vUp| ground %.2f  air %.2f  ->  kAirSpeed %.2f", vG, vA, sugAir);
        ImGui::SameLine(); if (ImGui::SmallButton("set##air") && sugAir > 0.1f)
            cs_constants::AirSpeed() = sugAir;
        ImGui::Text("horiz stand %.2f  move %.2f  ->  kMoveSpeed %.2f", hS, hM, sugMv);
        ImGui::SameLine(); if (ImGui::SmallButton("set##mv") && sugMv > 0.05f)
            cs_constants::MoveSpeed() = sugMv;
        ImGui::TextDisabled("(clusters use the smoothed signal; Reset history to re-measure)");
    }

    // ---- Knobs (default closed) -------------------------------------
    if (ImGui::CollapsingHeader("Knobs (sliders)")) {
        float& air  = cs_constants::AirSpeed();
        float& flm  = cs_constants::FastLaunchMult();
        float& csm  = cs_constants::ClimbSlopeMax();
        double& vw  = cs_constants::VelWindowSec();
        double& res = cs_constants::RiseEngageSec();
        double& fes = cs_constants::FallEngageSec();
        double& rls = cs_constants::ReleaseSec();
        float& mvs  = cs_constants::MoveSpeed();
        double& mrs = cs_constants::MoveReleaseSec();
        static const double kSecMin = 0.0, kSecMax = 0.5;

        ImGui::PushItemWidth(160.f);
        ImGui::SliderFloat("kAirSpeed (|vUp| m/s)",      &air, 0.5f, 10.0f, "%.2f");
        ImGui::SliderFloat("kFastLaunchMult (x air)",    &flm, 1.0f, 4.0f, "%.2f");
        ImGui::SliderFloat("kClimbSlopeMax (vUp/horiz)", &csm, 0.0f, 3.0f, "%.2f"); // 0 = gate off
        ImGui::SliderScalar("kVelWindowSec (s)",  ImGuiDataType_Double, &vw,  &kSecMin, &kSecMax, "%.3f");
        ImGui::SliderScalar("kRiseEngageSec (s)", ImGuiDataType_Double, &res, &kSecMin, &kSecMax, "%.3f");
        ImGui::SliderScalar("kFallEngageSec (s)", ImGuiDataType_Double, &fes, &kSecMin, &kSecMax, "%.3f");
        ImGui::SliderScalar("kReleaseSec (s)",    ImGuiDataType_Double, &rls, &kSecMin, &kSecMax, "%.3f");
        ImGui::SliderFloat("kMoveSpeed (horiz m/s)",     &mvs, 0.1f, 5.0f, "%.2f");
        ImGui::SliderScalar("kMoveReleaseSec (s)", ImGuiDataType_Double, &mrs, &kSecMin, &kSecMax, "%.3f");
        ImGui::PopItemWidth();
    }

    // ---- Timers (default closed) ------------------------------------
    if (ImGui::CollapsingHeader("Timers")) {
        const float ratio = s.horizSpeed > 0.01f ? s.vertVelEMA / s.horizSpeed : 0.f;
        ImGui::Text("rise ratio %+.2f / %.2f   (dt %.1f ms, UITick d %u)",
                    ratio, cs_constants::ClimbSlopeMax(), s.dt * 1000.0, s.uiTickDelta);
        auto bar = [&](const char* label, double since, double span) {
            if (have && since >= 0.0) {
                float frac = span > 0.0 ? (float)((now - since) / span) : 1.f;
                if (frac > 1.f) frac = 1.f;
                char b[56]; std::snprintf(b, sizeof(b), "%s %.2f / %.2f s", label, now - since, span);
                ImGui::ProgressBar(frac, ImVec2(190.f, 0), b);
            } else ImGui::TextDisabled("%s -", label);
        };
        bar("riseSince  ", s.riseSince,   cs_constants::RiseEngageSec());
        bar("fallSince  ", s.fallSince,   cs_constants::FallEngageSec());
        bar("groundSince", s.groundSince, cs_constants::ReleaseSec());
        bar("stillSince ", s.stillSince,  cs_constants::MoveReleaseSec());
    }

    // ---- Graphs (default closed) ------------------------------------
    if (ImGui::CollapsingHeader("Graphs")) {
        ImGui::TextDisabled("smoothed signals; history ~2s @ 60Hz");
        const ImVec2 plotSize(330, 52);
        // Vertical velocity (smoothed), ±kAirSpeed lines.
        {
            float buf[prof::kHistLen];
            int n = prof::FlattenRing(airtuner::VertVelHist(), buf);
            const float air = cs_constants::AirSpeed();
            float absMax = air * 2.f;
            for (int i = 0; i < n; ++i) { float a = std::fabs(buf[i]); if (a > absMax) absMax = a; }
            const float sMin = -absMax, sMax = absMax;
            char ov[48]; std::snprintf(ov, sizeof(ov), "vUp(sm)  cur %+.2f  \xc2\xb1%.1f m/s",
                                       n ? buf[n-1] : 0.f, absMax);
            ImGui::PlotLines("##vvplot", buf, n, 0, ov, sMin, sMax, plotSize);
            devui::DrawThresholdOnLastPlot( air, sMin, sMax - sMin, IM_COL32(220, 70, 70, 200));
            devui::DrawThresholdOnLastPlot(-air, sMin, sMax - sMin, IM_COL32(220, 70, 70, 200));
        }
        // Horizontal speed (smoothed), kMoveSpeed line.
        {
            float buf[prof::kHistLen];
            int n = prof::FlattenRing(airtuner::HorizSpeedHist(), buf);
            const float mvs = cs_constants::MoveSpeed();
            float peak = mvs * 2.f;
            for (int i = 0; i < n; ++i) if (buf[i] > peak) peak = buf[i];
            char ov[48]; std::snprintf(ov, sizeof(ov), "horiz(sm)  cur %.2f  0..%.1f m/s",
                                       n ? buf[n-1] : 0.f, peak);
            ImGui::PlotLines("##hsplot", buf, n, 0, ov, 0.f, peak, plotSize);
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
