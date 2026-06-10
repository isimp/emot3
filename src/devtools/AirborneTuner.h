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
#include <fstream>           // ofstream (dev-only trace log; core stays fstream-free)
#include <string>
#include <vector>            // buffered session log (retroactive highlight needs in-memory rows)

#include "imgui/imgui.h"

#include "Profiling.h"        // prof::Ring (120-sample rolling buffer reused here)
#include "DevToolsUI.h"       // devui::StateDot / DrawThresholdOnLastPlot
#include "AirborneDetect.h"   // cs_constants::HardFallSpeed() etc. - the tunable knobs

// Addon root (Globals.cpp). Forward-declared so this dev header needn't pull in
// Globals.h (and Windows.h) - we only need the path string for the trace file.
extern std::string g_AddonDir;

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
    bool     rise;         // currently classified rising-airborne (fast or band)
    bool     fall;         // currently classified falling-airborne (fast or band)
    double   landSince;    // -1 if not running, else when surface-consistent landing began
    double   fallSince;    // -1 if not running, else when the ballistic-fall debounce began
    double   groundSince;  // -1 if not running, else when the release backstop began
    double   stillSince;   // -1 if not running, else when stillness began
    double   now;          // steady_clock seconds at this sample (CharacterState's clock)
    // ---- vertical ballistic instrumentation (Step 1; appended - keep in sync with
    //      the OnSample({...}) call site in CharacterState.cpp, positional init) ----
    float    accel;             // change in smoothed vUp over AccelWindowSec (<=0 falling)
    bool     launch;            // raw vUp spike >= LaunchSpeed
    bool     hard;              // +vUpEMA>=HardRiseSpeed or -vUpEMA<=-HardFallSpeed (asymmetric)
    bool     ballistic;         // accelerating downward past FallAccelDrop / FallArmSpeed
    bool     surfaceConsistent; // |vUpEMA| settled AND acceleration stopped (landing band)
    bool     primed;            // inside the post glide/fly-off expect-fall window
    bool     gliding;           // RTAPI CS_IsGliding (false if RTAPI absent)
    bool     flying;            // RTAPI CS_IsFlying
    bool     rtapiLive;         // RTAPI connected this tick
    float    vInst;             // instantaneous vUp (clamped, pre-median) - for the trace
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

// ---- ballistic instrumentation history (render-thread only; no locks) ----
inline prof::Ring& AccelHist()      { static prof::Ring r; return r; }  // windowed vert accel
inline float&      MeasuredGravity(){ static float v = 0.f; return v; } // steepest sustained descent accel-rate (m/s^2)

// ---- per-event scenario recorder -----------------------------------
// Record ONE clean event at a time (Record -> do it -> Save), tagged with the
// scenario you picked. Each row keeps the event's peak signature + what the live
// detector decided, so distinct fall types are compared side by side instead of
// blurred into one "AIR" bucket. Derive separates the labeled GROUND vs AIR events.
enum class Cls { Ground, Air };
struct Scenario { const char* name; Cls cls; };
inline const Scenario* Scenarios(int& n) {
    static const Scenario s[] = {
        { "stand",       Cls::Ground }, { "walk",        Cls::Ground },
        { "stairs-up",   Cls::Ground }, { "stairs-down", Cls::Ground },
        { "ramp",        Cls::Ground },
        { "long-jump",   Cls::Air },    { "ledge-drop",  Cls::Air },
        { "glide-cancel",Cls::Air },    { "mount-hop",   Cls::Air },
        { "jump-drop",   Cls::Air },
    };
    n = (int)(sizeof(s) / sizeof(s[0])); return s;
}
inline int&  ActiveLabel() { static int  i = 5; return i; }   // default "long-jump"
inline bool& Recording()   { static bool b = false; return b; }

struct RecEvent { int label; float vUpPeak, accelPeak, rawUpPeak, minV, durSec, airFrac; };
struct InProgress { float vUpPeak, accelPeak, rawUpPeak, minV; double startT; int nTicks, nAir; bool active; };
inline InProgress& Cur() { static InProgress p{}; return p; }

static constexpr int kMaxEvents = 64;
struct EventList {
    RecEvent ev[kMaxEvents]; int count = 0;
    void push(const RecEvent& e) { if (count < kMaxEvents) ev[count++] = e; }
    void remove(int i) { if (i < 0 || i >= count) return; for (int j = i; j + 1 < count; ++j) ev[j] = ev[j + 1]; --count; }
    void clear() { count = 0; }
};
inline EventList& Recorded() { static EventList l; return l; }

inline void RecStart(double now) {
    InProgress& p = Cur();
    p.vUpPeak = p.accelPeak = p.rawUpPeak = 0.f; p.minV = 0.f;
    p.startT = now; p.nTicks = p.nAir = 0; p.active = true;
    Recording() = true;
}
inline void RecSave(double now) {
    InProgress& p = Cur();
    if (p.active && p.nTicks > 0)
        Recorded().push({ ActiveLabel(), p.vUpPeak, p.accelPeak, p.rawUpPeak, p.minV,
                          (float)(now - p.startT), p.nTicks ? (float)p.nAir / p.nTicks : 0.f });
    p.active = false; Recording() = false;
}
inline void RecDiscard() { Cur().active = false; Recording() = false; }

// ---- raw-signal trace ring (one-click CSV export for offline debugging) ----
// Captures the per-tick signal so the noise / real scenario shapes can be read off a
// CSV instead of guessed from a single number. Always-on while not Frozen; "Copy trace
// CSV" dumps the ring to the clipboard (paste it back for analysis).
struct TraceRow { double now; float dtMs, vInst, vMed, vEMA, accel, horiz; unsigned flags; int label; int hl; };
static constexpr int kTraceCap = 512;   // ~7 s at the game tick rate; small enough to paste
struct TraceRing {
    TraceRow r[kTraceCap]; int head = 0, count = 0;
    void push(const TraceRow& x) { r[head] = x; head = (head + 1) % kTraceCap; if (count < kTraceCap) ++count; }
    void clear() { head = count = 0; }
};
inline TraceRing& Trace() { static TraceRing t; return t; }

inline std::string TraceAsCsv() {
    const TraceRing& t = Trace();
    int nsc; const Scenario* sc = Scenarios(nsc);
    std::string out;
    out.reserve((size_t)t.count * 80 + 128);
    out += "t,dt_ms,vInst,vMed,vEMA,accel,horiz,airborne,ballistic,hard,launch,surface,primed,label,hl\n";
    if (t.count == 0) return out;
    const int first = (t.head - t.count + kTraceCap) % kTraceCap;
    const double t0 = t.r[first].now;
    char line[200];
    for (int i = 0; i < t.count; ++i) {
        const TraceRow& r = t.r[(first + i) % kTraceCap];
        const char* lbl = (r.label >= 0 && r.label < nsc) ? sc[r.label].name : "";
        std::snprintf(line, sizeof(line),
            "%.3f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%s,%d\n",
            r.now - t0, r.dtMs, r.vInst, r.vMed, r.vEMA, r.accel, r.horiz,
            (r.flags & 1) ? 1 : 0, (r.flags & 2) ? 1 : 0, (r.flags & 4) ? 1 : 0,
            (r.flags & 8) ? 1 : 0, (r.flags & 16) ? 1 : 0, (r.flags & 32) ? 1 : 0, lbl, r.hl);
        out += line;
    }
    return out;
}

// ---- whole-session file log (buffered; written on stop) ------------
// Buffers every tick in memory while logging, then writes <addon dir>\airborne_trace.csv
// on stop. Buffered (not streamed) on purpose: the "Highlight" button retroactively tags
// the seconds BEFORE the press, so you flag what just happened - not what's coming up
// (that's what the recorder is for). A session is a few minutes ~= a few MB; trivial.
inline bool&                  LogToFile() { static bool b = false; return b; }
inline std::vector<TraceRow>& LogBuf()    { static std::vector<TraceRow> v; return v; }
inline std::string&           LogPath()   { static std::string p; return p; }
inline double&                LogStartT() { static double t = 0.0; return t; }
inline int&                   LogRows()   { static int n = 0; return n; }
// Highlight count: each press tags the last kHighlightWindowSec of already-captured rows
// with an incrementing number (the `hl` column), so the session has numbered regions you
// can point at after the fact ("what went wrong in highlight 2?"). 0 = unmarked.
inline int&                   Highlight() { static int n = 0; return n; }
static constexpr double kHighlightWindowSec = 3.0;

// Buffer one tick while logging (called from OnSample, regardless of Freeze).
inline void LogTick(const AirtunerSample& s) {
    if (!LogToFile()) return;
    if (LogBuf().empty()) LogStartT() = s.now;
    const unsigned f = (s.airborne ? 1u : 0u) | (s.ballistic ? 2u : 0u) | (s.hard ? 4u : 0u) |
                       (s.launch ? 8u : 0u) | (s.surfaceConsistent ? 16u : 0u) | (s.primed ? 32u : 0u);
    LogBuf().push_back({ s.now, s.dt * 1000.f, s.vInst, s.vertVel, s.vertVelEMA,
                         s.accel, s.horizSpeed, f, Recording() ? ActiveLabel() : -1, 0 });
    LogRows() = (int)LogBuf().size();
}

// Retroactively tag the last kHighlightWindowSec of rows (in LogBuf AND the clipboard
// ring) with the next highlight number - "mark what just happened".
inline void MarkHighlight() {
    const int h = Highlight() + 1;
    bool any = false;
    std::vector<TraceRow>& b = LogBuf();
    if (!b.empty()) {
        const double tEnd = b.back().now;
        for (int i = (int)b.size() - 1; i >= 0; --i) {
            if (tEnd - b[i].now > kHighlightWindowSec) break;
            if (b[i].hl == 0) { b[i].hl = h; any = true; }
        }
    }
    TraceRing& t = Trace();
    for (int k = 0; k < t.count; ++k) {
        const int idx = (t.head - 1 - k + kTraceCap) % kTraceCap;
        if (t.r[(t.head - 1 + kTraceCap) % kTraceCap].now - t.r[idx].now > kHighlightWindowSec) break;
        if (t.r[idx].hl == 0) { t.r[idx].hl = h; any = true; }
    }
    if (any) Highlight() = h;
}

// Write the buffered session to disk (called from the UI when logging stops).
inline void WriteLogFile() {
    LogPath() = (g_AddonDir.empty() ? std::string() : g_AddonDir + "\\") + "airborne_trace.csv";
    std::ofstream f(LogPath().c_str(), std::ios::out | std::ios::trunc);
    if (!f.is_open()) return;
    f << "t,dt_ms,vInst,vMed,vEMA,accel,horiz,airborne,ballistic,hard,launch,surface,primed,label,hl\n";
    int nsc; const Scenario* sc = Scenarios(nsc);
    const double t0 = LogStartT();
    char line[200];
    for (const TraceRow& r : LogBuf()) {
        const char* lbl = (r.label >= 0 && r.label < nsc) ? sc[r.label].name : "";
        std::snprintf(line, sizeof(line),
            "%.3f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%s,%d\n",
            r.now - t0, r.dtMs, r.vInst, r.vMed, r.vEMA, r.accel, r.horiz,
            (r.flags & 1) ? 1 : 0, (r.flags & 2) ? 1 : 0, (r.flags & 4) ? 1 : 0,
            (r.flags & 8) ? 1 : 0, (r.flags & 16) ? 1 : 0, (r.flags & 32) ? 1 : 0, lbl, r.hl);
        f << line;
    }
    LogRows() = (int)LogBuf().size();
}

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

inline void ClearHistory() {
    VertVelHist().clear(); HorizSpeedHist().clear(); AccelHist().clear(); Events().clear();
    Trace().clear();
    MeasuredGravity() = 0.f;
    AirbornePeakAbsVUp() = 0.f; MovingPeakSpeed() = 0.f;
    // Wizard envelopes are managed by the wizard controls, not "Reset history".
}

// ---- per-tick callback (from TickCharacterState) --------------------
inline void OnSample(const AirtunerSample& s) {
    LogTick(s);   // whole-session file log runs regardless of Freeze
    if (Paused()) { LastSample() = s; HaveSample() = true; return; }

    VertVelHist().push(s.vertVelEMA);   // plot the signal the detector decides on
    HorizSpeedHist().push(s.horizSpeed);
    AccelHist().push(s.accel);
    LastSample() = s;
    HaveSample() = true;

    // Measured gravity: steepest sustained downward accel-rate while clearly falling.
    // The robust successor to the reverted instantaneous-aUp aid (slope of the SMOOTHED
    // velocity over the window). Informs FallAccelDrop / AccelWindowSec; no knob ships.
    if (s.vertVelEMA < 0.f && s.accel < 0.f) {
        const double awin = cs_constants::AccelWindowSec();
        if (awin > 0.0) {
            const float rate = -s.accel / (float)awin;   // m/s per s
            if (rate > MeasuredGravity()) MeasuredGravity() = rate;
        }
    }

    // Per-event recorder: accumulate the current event's peak signature while armed.
    if (Recording() && Cur().active) {
        InProgress& p = Cur();
        const float av  = std::fabs(s.vertVelEMA);
        const float aac = std::fabs(s.accel);
        if (av > p.vUpPeak)                    p.vUpPeak   = av;
        if (s.accel < 0.f && aac > p.accelPeak) p.accelPeak = aac;
        if (s.vertVel > p.rawUpPeak)           p.rawUpPeak = s.vertVel;
        if (s.vertVelEMA < p.minV)             p.minV      = s.vertVelEMA;
        ++p.nTicks; if (s.airborne) ++p.nAir;
    }

    // Raw-signal trace ring (for the CSV export).
    {
        const unsigned f = (s.airborne ? 1u : 0u) | (s.ballistic ? 2u : 0u) | (s.hard ? 4u : 0u) |
                           (s.launch ? 8u : 0u) | (s.surfaceConsistent ? 16u : 0u) | (s.primed ? 32u : 0u);
        Trace().push({ s.now, s.dt * 1000.f, s.vInst, s.vertVel, s.vertVelEMA,
                       s.accel, s.horizSpeed, f, Recording() ? ActiveLabel() : -1, 0 });
    }

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
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
        "// Paste into CharacterState.h's cs_constants getter bodies:\n"
        "//   --- vertical ballistic ---\n"
        "//   LaunchSpeed:       %.2ff\n"
        "//   HardRiseSpeed:     %.2ff\n"
        "//   HardFallSpeed:     %.2ff\n"
        "//   FallAccelDrop:     %.2ff\n"
        "//   AccelWindowSec:    %.3f\n"
        "//   FallArmSpeed:      %.2ff\n"
        "//   FallEngageSec:     %.3f\n"
        "//   SettleAccel:       %.2ff\n"
        "//   GroundSettleSpeed: %.2ff\n"
        "//   LandConfirmSec:    %.3f\n"
        "//   ReleaseSec:        %.3f\n"
        "//   PrimeFallSec:      %.3f\n"
        "//   PrimeFallScale:    %.2ff\n"
        "//   --- shared / optional ---\n"
        "//   VelWindowSec:      %.3f\n"
        "//   ClimbSlopeMax:     %.2ff\n"
        "//   MoveSpeed:         %.2ff\n"
        "//   MoveReleaseSec:    %.3f\n",
        cs_constants::LaunchSpeed(), cs_constants::HardRiseSpeed(), cs_constants::HardFallSpeed(),
        cs_constants::FallAccelDrop(), cs_constants::AccelWindowSec(), cs_constants::FallArmSpeed(),
        cs_constants::FallEngageSec(), cs_constants::SettleAccel(), cs_constants::GroundSettleSpeed(),
        cs_constants::LandConfirmSec(), cs_constants::ReleaseSec(), cs_constants::PrimeFallSec(),
        cs_constants::PrimeFallScale(),
        cs_constants::VelWindowSec(), cs_constants::ClimbSlopeMax(),
        cs_constants::MoveSpeed(), cs_constants::MoveReleaseSec());
    return std::string(buf);
}

inline void ResetDefaults() {   // keep in sync with CharacterState.h cs_constants
    cs_constants::ClimbSlopeMax()  = 0.0f;   // optional horiz suppressor: off by default
    cs_constants::VelWindowSec()   = 0.04;
    cs_constants::MoveSpeed()      = 1.0f;
    cs_constants::MoveReleaseSec() = 0.15;
    // vertical ballistic (calibrated from the in-game trace)
    cs_constants::LaunchSpeed()       = 7.0f;
    cs_constants::HardRiseSpeed()     = 6.0f;
    cs_constants::HardFallSpeed()     = 10.0f;
    cs_constants::FallAccelDrop()     = 3.5f;
    cs_constants::AccelWindowSec()    = 0.16;
    cs_constants::FallArmSpeed()      = 2.0f;
    cs_constants::FallEngageSec()     = 0.15;
    cs_constants::SettleAccel()       = 1.5f;
    cs_constants::GroundSettleSpeed() = 3.5f;
    cs_constants::LandConfirmSec()    = 0.08;
    cs_constants::ReleaseSec()        = 0.30;
    cs_constants::PrimeFallSec()      = 0.40;
    cs_constants::PrimeFallScale()    = 0.6f;
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
    if (ImGui::Button("Copy trace CSV")) ImGui::SetClipboardText(airtuner::TraceAsCsv().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%d rows - jump/fall, hit Freeze, then copy", airtuner::Trace().count);
    if (ImGui::Checkbox("Log to file (whole session)", &airtuner::LogToFile())) {
        if (airtuner::LogToFile()) { airtuner::LogBuf().clear(); airtuner::Highlight() = 0; airtuner::LogRows() = 0; }
        else airtuner::WriteLogFile();   // stop -> write the buffered session to disk
    }
    ImGui::SameLine();
    if (ImGui::Button("Highlight last 3s")) airtuner::MarkHighlight();   // tags what JUST happened
    ImGui::SameLine(); ImGui::TextDisabled("(%d marked)", airtuner::Highlight());
    if (airtuner::LogToFile())
        ImGui::TextDisabled("logging... %d rows - uncheck to write the file", airtuner::LogRows());
    else if (!airtuner::LogPath().empty())
        ImGui::TextDisabled("wrote %d rows -> %s", airtuner::LogRows(), airtuner::LogPath().c_str());
    ImGui::Separator();

    // ---- live state (compact; grouped: outputs / why / RTAPI) -------
    // Tight dot+label so each lamp pairs unambiguously with its name (the old wide
    // 150px field made them hard to track). Timer-window lamps moved to "Timers".
    auto lamp = [](bool on, const char* label) {
        devui::StateDot(on); ImGui::SameLine(0.f, 3.f); ImGui::TextUnformatted(label);
    };
    devui::StateDot(s.airborne); ImGui::SameLine(0.f, 4.f);
    ImGui::TextColored(s.airborne ? ImVec4(1, 0.55f, 0.55f, 1) : ImVec4(0.55f, 0.55f, 0.55f, 1), "AIRBORNE");
    ImGui::SameLine(0.f, 28.f); lamp(s.moving, "moving");
    ImGui::TextDisabled("engage:"); ImGui::SameLine();
    lamp(s.launch, "launch");    ImGui::SameLine();
    lamp(s.hard, "hard");        ImGui::SameLine();
    lamp(s.ballistic, "ball");   ImGui::SameLine(0.f, 18.f);
    ImGui::TextDisabled("rel:"); ImGui::SameLine(); lamp(s.surfaceConsistent, "surface");
    if (s.rtapiLive) {
        ImGui::TextDisabled("RTAPI:"); ImGui::SameLine();
        lamp(s.gliding, "glide"); ImGui::SameLine();
        lamp(s.flying, "fly");    ImGui::SameLine();
        lamp(s.primed, "prime");
    }
    if (have)
        ImGui::Text("vUp %+.2f  accel %+.2f  horiz %.2f m/s", s.vertVelEMA, s.accel, s.horizSpeed);
    else
        ImGui::TextDisabled("(no sample yet - waiting for a fresh UITick)");

    // ---- Calibration recorder: per-event capture + derive (default open) ----
    if (ImGui::CollapsingHeader("Calibration recorder", ImGuiTreeNodeFlags_DefaultOpen)) {
        int nsc; const airtuner::Scenario* sc = airtuner::Scenarios(nsc);
        int& lbl = airtuner::ActiveLabel();
        if (lbl < 0 || lbl >= nsc) lbl = 0;

        ImGui::PushItemWidth(150.f);
        if (ImGui::BeginCombo("label", sc[lbl].name)) {
            for (int i = 0; i < nsc; ++i)
                if (ImGui::Selectable(sc[i].name, i == lbl)) lbl = i;
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();

        if (!airtuner::Recording()) {
            if (ImGui::Button("Record")) airtuner::RecStart(now);
            ImGui::SameLine();
            ImGui::TextDisabled("Record -> do ONE event -> Save. Reposition freely while not recording.");
        } else {
            const airtuner::InProgress& p = airtuner::Cur();
            if (ImGui::Button("Save event")) airtuner::RecSave(now);
            ImGui::SameLine(); if (ImGui::Button("Discard")) airtuner::RecDiscard();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "REC %s  |vUp|%.1f acc%.1f up%.1f  %.2fs",
                               sc[lbl].name, p.vUpPeak, p.accelPeak, p.rawUpPeak, (float)(now - p.startT));
        }

        const airtuner::EventList& L = airtuner::Recorded();
        if (L.count == 0) {
            ImGui::TextDisabled("(no events yet)");
        } else {
            int delIdx = -1;
            if (ImGui::BeginTable("##recev", 8,
                       ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("scenario"); ImGui::TableSetupColumn("|vUp|");
                ImGui::TableSetupColumn("accel"); ImGui::TableSetupColumn("rawUp");
                ImGui::TableSetupColumn("minV"); ImGui::TableSetupColumn("dur");
                ImGui::TableSetupColumn("det%"); ImGui::TableSetupColumn("");
                ImGui::TableHeadersRow();
                for (int i = 0; i < L.count; ++i) {
                    const airtuner::RecEvent& e2 = L.ev[i];
                    const airtuner::Scenario& s2 = sc[e2.label];
                    const bool air = s2.cls == airtuner::Cls::Air;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(air ? ImVec4(1, 0.8f, 0.6f, 1) : ImVec4(0.7f, 0.85f, 1, 1), "%s", s2.name);
                    ImGui::TableSetColumnIndex(1); devui::NumCell("%.1f", e2.vUpPeak);
                    ImGui::TableSetColumnIndex(2); devui::NumCell("%.1f", e2.accelPeak);
                    ImGui::TableSetColumnIndex(3); devui::NumCell("%.1f", e2.rawUpPeak);
                    ImGui::TableSetColumnIndex(4); devui::NumCell("%.1f", e2.minV);
                    ImGui::TableSetColumnIndex(5); devui::NumCell("%.2f", e2.durSec);
                    ImGui::TableSetColumnIndex(6);
                    const bool bad = air ? (e2.airFrac < 0.6f) : (e2.airFrac > 0.05f);  // missed air / false ground
                    if (bad) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.4f, 1));
                    devui::NumCell("%.0f%%", e2.airFrac * 100.f);
                    if (bad) ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(7);
                    ImGui::PushID(i); if (ImGui::SmallButton("x")) delIdx = i; ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (delIdx >= 0) airtuner::Recorded().remove(delIdx);   // deferred; safe after the loop
        }

        // Aggregate labeled events -> ground ceiling vs air onset, then derive.
        float gVUp = 0, gAccel = 0, gRawUp = 0, aVUpMin = 1e9f, aAccelMin = 1e9f, aRawUp = 0;
        int nG = 0, nA = 0, gAccelLbl = -1, aAccelLbl = -1;
        for (int i = 0; i < L.count; ++i) {
            const airtuner::RecEvent& e2 = L.ev[i];
            if (sc[e2.label].cls == airtuner::Cls::Ground) {
                ++nG;
                if (e2.vUpPeak  > gVUp)    gVUp   = e2.vUpPeak;
                if (e2.accelPeak > gAccel) { gAccel = e2.accelPeak; gAccelLbl = e2.label; }
                if (e2.rawUpPeak > gRawUp) gRawUp = e2.rawUpPeak;
            } else {
                ++nA;
                if (e2.vUpPeak  < aVUpMin)    aVUpMin   = e2.vUpPeak;
                if (e2.accelPeak < aAccelMin) { aAccelMin = e2.accelPeak; aAccelLbl = e2.label; }
                if (e2.rawUpPeak > aRawUp)    aRawUp    = e2.rawUpPeak;
            }
        }

        if (nG > 0 && nA > 0) {
            ImGui::Separator();
            const float hard   = gVUp < aVUpMin ? 0.5f * (gVUp + aVUpMin) : gVUp * 1.15f;
            const float fad    = 0.5f * (gAccel + aAccelMin);                     // between ground accel & gravity
            const float launch = aRawUp > gRawUp + 0.5f ? 0.5f * (gRawUp + aRawUp)
                                                        : cs_constants::LaunchSpeed();
            const float farm   = gVUp * 0.35f > 1.0f ? gVUp * 0.35f : 1.0f;
            const float gset   = gVUp * 0.70f > 1.5f ? gVUp * 0.70f : 1.5f;
            const float sacc   = fad  * 0.70f;
            const bool ovHard  = gVUp   >= aVUpMin;     // an air event dipped into ground territory
            const bool ovAccel = gAccel >= aAccelMin;   // a ground scenario accelerates like a fall

            auto row = [](const char* name, float val, const char* formula, bool overlap, const char* hint) {
                ImGui::TextColored(overlap ? ImVec4(1, 0.5f, 0.5f, 1) : ImVec4(0.6f, 1, 0.6f, 1),
                                   "%-17s %6.2f", name, val);
                ImGui::SameLine(); ImGui::TextDisabled("<- %s", formula);
                if (overlap) {
                    ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "(!)");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", hint);
                }
            };
            row("HardFallSpeed", hard,   "mid(ground|vUp|max, air|vUp|min)", ovHard,
                "an air event's |vUp| dipped below a ground event - record cleaner/bigger falls");
            row("FallAccelDrop", fad,    "mid(ground accel, air accel)",     ovAccel,
                "a ground scenario accelerates as hard as a fall - see the overlap line below");
            row("LaunchSpeed",   launch, "mid(ground rawUp, air rawUp)",     false, "");
            row("FallArmSpeed",      farm, "ground|vUp| x0.35",   false, "");
            row("GroundSettleSpeed", gset, "ground|vUp| x0.70",   false, "");
            row("SettleAccel",       sacc, "FallAccelDrop x0.70", false, "");

            if (ovAccel && gAccelLbl >= 0 && aAccelLbl >= 0)
                ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1),
                    "(!) accel overlap: ground '%s' (%.1f) >= air '%s' (%.1f) - widen AccelWindowSec or enable ClimbSlopeMax",
                    sc[gAccelLbl].name, gAccel, sc[aAccelLbl].name, aAccelMin);

            if (ImGui::Button("Apply all")) {   // HardRiseSpeed stays on its slider (recorder tracks |vUp|, not sign)
                cs_constants::HardFallSpeed()     = hard;
                cs_constants::FallAccelDrop()     = fad;
                cs_constants::LaunchSpeed()       = launch;
                cs_constants::FallArmSpeed()      = farm;
                cs_constants::GroundSettleSpeed() = gset;
                cs_constants::SettleAccel()       = sacc;
            }
            ImGui::SameLine();
        } else if (L.count > 0) {
            ImGui::TextDisabled("record >=1 GROUND and >=1 AIR event to derive");
        }
        if (ImGui::Button("Clear events")) airtuner::Recorded().clear();
    }

    // ---- Knobs (default closed) -------------------------------------
    if (ImGui::CollapsingHeader("Knobs (sliders)")) {
        static const double kSecMin = 0.0, kSecMax = 0.5;
        ImGui::PushItemWidth(160.f);

        ImGui::TextDisabled("engage (vertical, horizontal-independent)");
        float&  lspd = cs_constants::LaunchSpeed();
        float&  hrsp = cs_constants::HardRiseSpeed();
        float&  hfsp = cs_constants::HardFallSpeed();
        float&  fadk = cs_constants::FallAccelDrop();
        double& awnk = cs_constants::AccelWindowSec();
        float&  fark = cs_constants::FallArmSpeed();
        ImGui::SliderFloat("LaunchSpeed (raw vUp)",   &lspd, 2.0f, 14.0f, "%.2f");
        ImGui::SliderFloat("HardRiseSpeed (+vUpEMA)", &hrsp, 2.0f, 14.0f, "%.2f");
        ImGui::SliderFloat("HardFallSpeed (-vUpEMA)", &hfsp, 2.0f, 14.0f, "%.2f");
        ImGui::SliderFloat("FallAccelDrop",           &fadk, 0.5f, 12.0f, "%.2f");
        ImGui::SliderScalar("AccelWindowSec (s)", ImGuiDataType_Double, &awnk, &kSecMin, &kSecMax, "%.3f");
        ImGui::SliderFloat("FallArmSpeed",          &fark, 0.0f, 5.0f, "%.2f");

        ImGui::Separator();
        ImGui::TextDisabled("release / landing");
        float&  sack = cs_constants::SettleAccel();
        float&  gstk = cs_constants::GroundSettleSpeed();
        double& lcsk = cs_constants::LandConfirmSec();
        double& rls  = cs_constants::ReleaseSec();
        ImGui::SliderFloat("SettleAccel",           &sack, 0.1f, 6.0f, "%.2f");
        ImGui::SliderFloat("GroundSettleSpeed",     &gstk, 0.5f, 6.0f, "%.2f");
        ImGui::SliderScalar("LandConfirmSec (s)",   ImGuiDataType_Double, &lcsk, &kSecMin, &kSecMax, "%.3f");
        ImGui::SliderScalar("ReleaseSec bkstp (s)", ImGuiDataType_Double, &rls,  &kSecMin, &kSecMax, "%.3f");

        ImGui::Separator();
        ImGui::TextDisabled("RTAPI prime / smoothing / move / optional slope");
        double& pfsk = cs_constants::PrimeFallSec();
        float&  pfck = cs_constants::PrimeFallScale();
        double& vw   = cs_constants::VelWindowSec();
        double& fes  = cs_constants::FallEngageSec();
        float&  mvs  = cs_constants::MoveSpeed();
        double& mrs  = cs_constants::MoveReleaseSec();
        float&  csm  = cs_constants::ClimbSlopeMax();
        ImGui::SliderScalar("PrimeFallSec (s)",     ImGuiDataType_Double, &pfsk, &kSecMin, &kSecMax, "%.3f");
        ImGui::SliderFloat("PrimeFallScale (x)",    &pfck, 0.1f, 1.0f, "%.2f");
        ImGui::SliderScalar("VelWindowSec (s)",     ImGuiDataType_Double, &vw,  &kSecMin, &kSecMax, "%.3f");
        ImGui::SliderScalar("FallEngageSec (s)",    ImGuiDataType_Double, &fes, &kSecMin, &kSecMax, "%.3f");
        ImGui::SliderFloat("MoveSpeed (horiz m/s)", &mvs, 0.1f, 5.0f, "%.2f");
        ImGui::SliderScalar("MoveReleaseSec (s)",   ImGuiDataType_Double, &mrs, &kSecMin, &kSecMax, "%.3f");
        ImGui::SliderFloat("ClimbSlopeMax (0=off)", &csm, 0.0f, 3.0f, "%.2f");
        ImGui::PopItemWidth();
    }

    // ---- Timers (default closed) ------------------------------------
    if (ImGui::CollapsingHeader("Timers")) {
        ImGui::Text("accel %+.2f  g~%.1f   (dt %.1f ms, UITick d %u)",
                    s.accel, airtuner::MeasuredGravity(), s.dt * 1000.0, s.uiTickDelta);
        auto bar = [&](const char* label, double since, double span) {
            if (have && since >= 0.0) {
                float frac = span > 0.0 ? (float)((now - since) / span) : 1.f;
                if (frac > 1.f) frac = 1.f;
                char b[56]; std::snprintf(b, sizeof(b), "%s %.2f / %.2f s", label, now - since, span);
                ImGui::ProgressBar(frac, ImVec2(190.f, 0), b);
            } else ImGui::TextDisabled("%s -", label);
        };
        bar("fallSince  ", s.fallSince,   cs_constants::FallEngageSec());
        bar("landSince  ", s.landSince,   cs_constants::LandConfirmSec());
        bar("groundSince", s.groundSince, cs_constants::ReleaseSec());
        bar("stillSince ", s.stillSince,  cs_constants::MoveReleaseSec());
    }

    // ---- Graphs (default closed) ------------------------------------
    if (ImGui::CollapsingHeader("Graphs")) {
        ImGui::TextDisabled("smoothed signals; history ~2s @ 60Hz");
        const ImVec2 plotSize(330, 52);
        // Vertical velocity (smoothed): orange +HardRiseSpeed / -HardFallSpeed (asymmetric),
        // green ±GroundSettleSpeed (the bands the decision uses).
        {
            float buf[prof::kHistLen];
            int n = prof::FlattenRing(airtuner::VertVelHist(), buf);
            const float hrise = cs_constants::HardRiseSpeed();
            const float hfall = cs_constants::HardFallSpeed();
            const float gset = cs_constants::GroundSettleSpeed();
            float absMax = (hfall > hrise ? hfall : hrise) * 1.3f;
            for (int i = 0; i < n; ++i) { float a = std::fabs(buf[i]); if (a > absMax) absMax = a; }
            const float sMin = -absMax, sMax = absMax, span = sMax - sMin;
            char ov[48]; std::snprintf(ov, sizeof(ov), "vUp(sm)  cur %+.2f  \xc2\xb1%.1f m/s",
                                       n ? buf[n-1] : 0.f, absMax);
            ImGui::PlotLines("##vvplot", buf, n, 0, ov, sMin, sMax, plotSize);
            devui::DrawThresholdOnLastPlot( hrise, sMin, span, IM_COL32(240, 150, 40, 220));
            devui::DrawThresholdOnLastPlot(-hfall, sMin, span, IM_COL32(240, 150, 40, 220));
            devui::DrawThresholdOnLastPlot( gset, sMin, span, IM_COL32(120, 200, 120, 140));
            devui::DrawThresholdOnLastPlot(-gset, sMin, span, IM_COL32(120, 200, 120, 140));
        }
        // Vertical accel (windowed): red -FallAccelDrop + green ±SettleAccel; measured-g readout.
        {
            float buf[prof::kHistLen];
            int n = prof::FlattenRing(airtuner::AccelHist(), buf);
            const float fad = cs_constants::FallAccelDrop();
            const float sa  = cs_constants::SettleAccel();
            float absMax = fad * 1.5f;
            for (int i = 0; i < n; ++i) { float a = std::fabs(buf[i]); if (a > absMax) absMax = a; }
            const float sMin = -absMax, sMax = absMax, span = sMax - sMin;
            char ov[64]; std::snprintf(ov, sizeof(ov), "accel(win) cur %+.2f  g~%.1f",
                                       n ? buf[n-1] : 0.f, airtuner::MeasuredGravity());
            ImGui::PlotLines("##accplot", buf, n, 0, ov, sMin, sMax, plotSize);
            devui::DrawThresholdOnLastPlot(-fad, sMin, span, IM_COL32(220, 70, 70, 220));
            devui::DrawThresholdOnLastPlot( sa,  sMin, span, IM_COL32(120, 200, 120, 140));
            devui::DrawThresholdOnLastPlot(-sa,  sMin, span, IM_COL32(120, 200, 120, 140));
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
