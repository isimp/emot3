#include "DevTools.h"

#ifdef EMOT3_DEVTOOLS

#include "Profiling.h"          // prof::Enabled / RenderProfilerOverlay
#include "QuickbarDebug.h"      // qbdbg::Enabled / RenderQbSizingOverlay
#include "DevStateInspector.h"  // devstate::Enabled / devstate::Render
#include "Simulators.h"         // simdbg::Enabled / RenderSimulators (notifier + update-check)
#include "MemoryMonitor.h"      // memmon::Enabled / RenderMemoryMonitor
#include "AirborneTuner.h"      // airtuner::Enabled / RenderAirborneTuner
#include "IconCacheTuner.h"     // iconcachetuner::Enabled / RenderIconCacheTuner

#include "imgui/imgui.h"

#include <string>
#include <vector>

namespace {
// Function-local static so it's safe to touch from any init order. One
// process-wide registry of dev-tool overlays.
std::vector<DevToolDef>& Registry() {
    static std::vector<DevToolDef> r;
    return r;
}
}  // namespace

void RegisterDevTool(const DevToolDef& d) { Registry().push_back(d); }

void RegisterBuiltinDevTools() {
    // Idempotent: Nexus calls AddonLoad again on every disable/enable while the
    // DLL stays in the process, so the function-local-static Registry() survives.
    // Without this clear, each reload appended another copy of every builtin and
    // the "[debug]" toggles (and their overlays) rendered twice, thrice, ...
    Registry().clear();
    // The full roster of dev tools. Add new tools here (one line each); see
    // the how-to in DevTools.h. Grouped by theme - the cat drives the headings
    // in the Options toggle list (registry order within a cat = display order).
    // --- Inspect (live, read-only) ---
    RegisterDevTool({ "profiler", "Performance overlay", DevCat::Inspect,
                      &prof::Enabled(),     RenderProfilerOverlay });
    RegisterDevTool({ "memory",   "Memory monitor", DevCat::Inspect,
                      &memmon::Enabled(),   RenderMemoryMonitor });
    RegisterDevTool({ "state",    "Runtime state inspector", DevCat::Inspect,
                      &devstate::Enabled(), devstate::Render });
    RegisterDevTool({ "qbsizing", "Quickbar", DevCat::Inspect,
                      &qbdbg::Enabled(),    RenderQbSizingOverlay });
    // --- Tune (runtime knobs) ---
    RegisterDevTool({ "airborne", "Airborne tuner", DevCat::Tune,
                      &airtuner::Enabled(), RenderAirborneTuner });
    RegisterDevTool({ "iconcache", "Icon cache tuner", DevCat::Tune,
                      &iconcachetuner::Enabled(), RenderIconCacheTuner });
    // --- Simulate (trigger prod-only paths) ---
    RegisterDevTool({ "simulators", "Simulators", DevCat::Simulate,
                      &simdbg::Enabled(),   RenderSimulators });
}

void RenderDevToolOverlays() {
    for (const DevToolDef& t : Registry())
        if (t.enabled && *t.enabled) t.render();
}

void RenderDevToolToggles() {
    // One "[debug] <label>" checkbox per tool, grouped under a dim heading per
    // theme. The ##id keeps the ImGui id stable/unique even if two tools shared
    // a label. Groups with no registered tool are skipped.
    struct Group { DevCat cat; const char* title; };
    static const Group kGroups[] = {
        { DevCat::Inspect,  "Inspect" },
        { DevCat::Tune,     "Tune" },
        { DevCat::Simulate, "Simulate" },
    };
    for (const Group& g : kGroups) {
        bool any = false;
        for (const DevToolDef& t : Registry()) if (t.cat == g.cat) { any = true; break; }
        if (!any) continue;
        ImGui::TextDisabled("%s", g.title);
        for (const DevToolDef& t : Registry()) {
            if (t.cat != g.cat) continue;
            std::string lbl = std::string("[debug] ") + t.label + "##devtool_" + t.id;
            ImGui::Checkbox(lbl.c_str(), t.enabled);
        }
        ImGui::Spacing();
    }
}

#endif  // EMOT3_DEVTOOLS
