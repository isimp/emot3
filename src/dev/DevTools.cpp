#include "DevTools.h"

#ifdef EMOT3_DEVTOOLS

#include "Profiling.h"          // prof::Enabled / RenderProfilerOverlay
#include "QuickbarDebug.h"      // qbdbg::Enabled / RenderQbSizingOverlay
#include "DevStateInspector.h"  // devstate::Enabled / devstate::Render
#include "NotifierDebug.h"      // notifierdbg::Enabled / RenderNotifierDebug
#include "UpdateCheckDebug.h"   // updchkdbg::Enabled / RenderUpdateCheckDebug

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
    // the how-to in DevTools.h. Order = display order in Options.
    RegisterDevTool({ "profiler", "Performance overlay",
                      &prof::Enabled(),    RenderProfilerOverlay });
    RegisterDevTool({ "qbsizing", "Quickbar sizing",
                      &qbdbg::Enabled(),   RenderQbSizingOverlay });
    RegisterDevTool({ "state",    "Runtime state inspector",
                      &devstate::Enabled(), devstate::Render });
    RegisterDevTool({ "notifier", "Notifier",
                      &notifierdbg::Enabled(), RenderNotifierDebug });
    RegisterDevTool({ "updatecheck", "Update check",
                      &updchkdbg::Enabled(), RenderUpdateCheckDebug });
}

void RenderDevToolOverlays() {
    for (const DevToolDef& t : Registry())
        if (t.enabled && *t.enabled) t.render();
}

void RenderDevToolToggles() {
    // One "[debug] <label>" checkbox per tool, one per line. The ##id keeps
    // the ImGui id stable and unique even if two tools shared a label.
    for (const DevToolDef& t : Registry()) {
        std::string lbl = std::string("[debug] ") + t.label + "##devtool_" + t.id;
        ImGui::Checkbox(lbl.c_str(), t.enabled);
    }
}

#endif  // EMOT3_DEVTOOLS
