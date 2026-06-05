#pragma once
// =====================================================================
//  Dev-only perf overlay. A dev tool, registered into the dev-tools
//  framework (DevTools.h / RegisterBuiltinDevTools).
//
//  Gated by EMOT3_DEVTOOLS - present ONLY when that macro is defined (the
//  +devtools flavor: the DevTools, PlusDevTools and Debug build configs). The
//  public emot3.dll (Distribution) and emot3_plus.dll (Plus) builds don't define
//  it, so the whole overlay compiles away. This is a separate flavor axis from
//  EMOT3_PLUS (which adds the AV-sensitive input-swallows) - see DevTools.h.
//
//  When EMOT3_DEVTOOLS is NOT defined, PROFILE_SCOPE() and
//  RenderProfilerOverlay() become no-ops, so PROFILE_SCOPE(...) call sites
//  stay unconditional and nothing of the overlay ships.
//
//  Touch points: #include "Profiling.h"; PROFILE_SCOPE(...); the
//  RegisterDevTool entry in DevTools.cpp (which wires the toggle + render).
// =====================================================================

#ifndef EMOT3_DEVTOOLS

// Non-dev build: everything compiles away.
#define PROFILE_SCOPE(name) ((void)0)
namespace prof { inline bool& Enabled() { static bool b = false; return b; } }
inline void RenderProfilerOverlay() {}

#else  // ---- dev build: full implementation ----

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "imgui/imgui.h"

namespace prof {

// Length of the per-section / whole-frame rolling history, in frames. ~2s at
// 60fps - long enough that the average/median read steady, short enough that a
// transient spike ages back out of the windowed peak within a couple seconds.
static constexpr int kHistLen = 120;

// Shared singletons via inline-function statics (one instance across all
// translation units, C++14-safe - avoids needing a .cpp for the globals).
inline bool& Enabled() { static bool b = false; return b; }
// Freeze: when set, the overlay stops promoting live samples into the display,
// so every number holds still for reading while the game keeps rendering. Live
// scope timings are discarded each frame (see NewFrameIfNeeded). Dev-only;
// referenced solely by RenderProfilerOverlay below, so no non-dev stub is needed.
inline bool& Paused()  { static bool b = false; return b; }

// Fixed-size ring of recent samples (ms). Backs both the per-section stats and
// the whole-frame line. avg/median/peak reduce only the valid samples, so a
// freshly-reset ring reports honest numbers before it fills.
struct Ring {
    float v[kHistLen] = {};
    int   head  = 0;   // next write slot
    int   count = 0;   // valid samples so far (<= kHistLen)
    void  push(float x) { v[head] = x; head = (head + 1) % kHistLen;
                          if (count < kHistLen) ++count; }
    void  clear() { head = 0; count = 0; for (float& f : v) f = 0.f; }
    float last() const { return count ? v[(head - 1 + kHistLen) % kHistLen] : 0.f; }
    double avg() const {
        if (!count) return 0.0;
        double s = 0.0; for (int i = 0; i < count; ++i) s += v[i];
        return s / count;
    }
    double peak() const {  // windowed max - a one-off spike ages out after kHistLen frames
        float m = 0.f; for (int i = 0; i < count; ++i) if (v[i] > m) m = v[i];
        return m;
    }
    double median() const {
        if (!count) return 0.0;
        std::vector<float> tmp(v, v + count);
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        return tmp[tmp.size() / 2];  // upper-middle for even counts - fine for a dev tool
    }
};

// Live, this-frame accumulation per label. calls counts how many scopes with
// this name closed this frame (so the same label in MainPanel + Quickbar shows
// 2 calls and their summed ms).
struct Accum { double ms = 0.0; int calls = 0; };
// Promoted, displayed stats per label: last frame's summed ms + call count, plus
// the rolling history feeding avg/median/peak.
struct SectionStat { double cur = 0.0; int calls = 0; Ring hist; };

inline std::map<std::string, Accum>&       accumMap()   { static std::map<std::string, Accum> m;       return m; }
inline std::map<std::string, SectionStat>& displayMap() { static std::map<std::string, SectionStat> m; return m; }
inline Ring& frameHist() { static Ring r; return r; }  // whole-frame ms (io.DeltaTime)
inline int&  lastFrame() { static int f = -1; return f; }

// Promote accum -> display once per ImGui frame, so the overlay shows stable,
// whole-frame totals regardless of which render callback runs first. The
// displayed numbers lag the live frame by one - fine for a profiler. While
// Paused(), promotion is skipped (display frozen) but accum is still cleared so
// the next live frame starts clean.
inline void NewFrameIfNeeded() {
    int f = ImGui::GetFrameCount();
    if (f == lastFrame()) return;
    if (!Paused()) {
        // Sections that ran this frame: record their summed ms + call count and
        // push the frame total into their history.
        for (const auto& kv : accumMap()) {
            SectionStat& s = displayMap()[kv.first];
            s.cur   = kv.second.ms;
            s.calls = kv.second.calls;
            s.hist.push((float)kv.second.ms);
        }
        // Sections we've seen before but that didn't run this frame: push 0 so
        // the windowed avg/median/peak decay instead of freezing at a stale
        // value (a section that runs every other frame then reads ~half its
        // on-frame cost - the honest per-frame budget).
        for (auto& kv : displayMap()) {
            if (accumMap().find(kv.first) == accumMap().end()) {
                kv.second.cur   = 0.0;
                kv.second.calls = 0;
                kv.second.hist.push(0.f);
            }
        }
        frameHist().push((float)(ImGui::GetIO().DeltaTime * 1000.0));
    }
    accumMap().clear();
    lastFrame() = f;
}

struct Scope {
    const char*                           name;
    std::chrono::steady_clock::time_point t0;
    bool                                  active;
    explicit Scope(const char* n) : name(n), active(Enabled()) {
        if (active) { NewFrameIfNeeded(); t0 = std::chrono::steady_clock::now(); }
    }
    ~Scope() {
        if (!active) return;
        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        Accum& a = accumMap()[name];
        a.ms    += ms;
        a.calls += 1;
    }
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace prof

#define PROFILE_CONCAT_(a, b) a##b
#define PROFILE_CONCAT(a, b)  PROFILE_CONCAT_(a, b)
// Times the enclosing scope under `name`; sums across all scopes sharing a
// name within one frame (so the same label in MainPanel + Quickbar adds up).
#define PROFILE_SCOPE(name)   ::prof::Scope PROFILE_CONCAT(profScope_, __LINE__)(name)

// Draw the overlay window when enabled. Call once per frame from a dedicated
// render callback so it shows independently of the main window's visibility.
inline void RenderProfilerOverlay() {
    if (!::prof::Enabled()) return;
    ::prof::NewFrameIfNeeded();
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("emot3 perf##profiler", &::prof::Enabled(),
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav)) {
        // Controls: Freeze holds the numbers still for reading; Reset wipes the
        // accumulated history (and frame line) so a fresh measurement starts.
        ImGui::Checkbox("Freeze", &::prof::Paused());
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            ::prof::displayMap().clear();
            ::prof::frameHist().clear();
        }
        ImGui::Separator();

        // Whole-frame context - the budget the marked sections are spent against
        // (the per-section rows below cover addon code only, never the full frame).
        double frMs  = ::prof::frameHist().last();
        double frAvg = ::prof::frameHist().avg();
        double fps   = frMs > 0.0 ? 1000.0 / frMs : 0.0;
        ImGui::Text("frame %8.3f ms  (%5.1f fps)  avg %8.3f ms", frMs, fps, frAvg);
        ImGui::Separator();

        ImGui::Text("%-14s %8s %8s %8s %8s %7s %6s",
                    "section", "cur", "avg", "med", "peak", "%frame", "calls");
        double total = 0.0;
        for (const auto& kv : ::prof::displayMap()) {
            const ::prof::SectionStat& s = kv.second;
            total += s.cur;
            // Share of the whole-frame budget, averaged so it doesn't jitter.
            double pct = frAvg > 0.0 ? (s.hist.avg() / frAvg * 100.0) : 0.0;
            ImGui::Text("%-14s %8.3f %8.3f %8.3f %8.3f %6.1f%% %6d",
                        kv.first.c_str(), s.cur, s.hist.avg(), s.hist.median(),
                        s.hist.peak(), pct, s.calls);
        }
        ImGui::Separator();
        ImGui::Text("%-14s %8.3f ms", "(sum marked)", total);
        ImGui::TextDisabled("addon sections only - not whole frame");
    }
    ImGui::End();
}

#endif // EMOT3_DEVTOOLS
