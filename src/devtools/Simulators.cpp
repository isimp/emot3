#include "Simulators.h"

#ifdef EMOT3_DEVTOOLS

#include "NotifierDebug.h"     // RenderNotifierBody
#include "UpdateCheckDebug.h"  // RenderUpdateCheckBody

#include "imgui/imgui.h"

// One window hosting the two prod-path testers as collapsing sections - was two
// separate one-off windows ("emot3 Notifier" + "emot3 Update check"). Each body
// drives the real production path (see NotifierDebug.cpp / UpdateCheckDebug.cpp);
// only the hosting window changed.
void RenderSimulators() {
    if (!simdbg::Enabled()) return;

    ImGui::SetNextWindowBgAlpha(0.9f);
    if (ImGui::Begin("emot3 Simulators##sim", &simdbg::Enabled(),
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav)) {
        if (ImGui::CollapsingHeader("Notifier", ImGuiTreeNodeFlags_DefaultOpen))
            RenderNotifierBody();
        if (ImGui::CollapsingHeader("Update check", ImGuiTreeNodeFlags_DefaultOpen))
            RenderUpdateCheckBody();
    }
    ImGui::End();
}

#endif  // EMOT3_DEVTOOLS
