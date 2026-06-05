#include "UpdateCheckDebug.h"

#ifdef EMOT3_DEVTOOLS

#include "Globals.h"      // APIDefs (+ Windows.h, nexus/Nexus.h -> AddonDefinition)
#include "Logging.h"

#include "imgui/imgui.h"

#ifndef EMOT3_DIST
#include "UpdateCheck.h"  // custom update check + dev hooks (plus-flavored builds)
#endif

// The addon definition (Name / Version / Signature / Provider / UpdateLink) is a
// global owned by entry.cpp; read it so the readout reflects the live build.
extern AddonDefinition AddonDef;

// Tester for the update flow. Behaves per build flavor so a plus-flavored dev
// build drives the custom GitHub check and a dist-flavored one drives Nexus'
// native updater - see UpdateCheckDebug.h.
void RenderUpdateCheckDebug() {
    if (!updchkdbg::Enabled()) return;

    ImGui::SetNextWindowBgAlpha(0.9f);
    if (ImGui::Begin("emot3 Update check##updchkdbg", &updchkdbg::Enabled(),
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav)) {
        ImGui::Text("addon  : %s  v%d.%d.%d.%d",
                    AddonDef.Name ? AddonDef.Name : "?",
                    AddonDef.Version.Major, AddonDef.Version.Minor,
                    AddonDef.Version.Build, AddonDef.Version.Revision);
        ImGui::Text("sig    : %d   provider: %d", AddonDef.Signature,
                    (int)AddonDef.Provider);
        ImGui::Separator();

#ifndef EMOT3_DIST
        // ---- Plus-flavored: the custom GitHub update check ----
        ImGui::TextUnformatted("flavor : plus  (custom GitHub update check)");
        ImGui::Text("available: %s", PlusUpdateAvailable() ? "yes" : "no");
        {
            std::string latest = PlusLatestVersion();
            ImGui::Text("latest   : %s", latest.empty() ? "(none)" : latest.c_str());
        }
        if (ImGui::Button("Run check now")) {
            RunUpdateCheckNow();
            LOG_INFO("updcheck[dev]: manual GitHub check kicked");
        }
        if (ImGui::Button("Force available (sample)")) {
            ForceUpdateAvailable("9.9.9");
            LOG_INFO("updcheck[dev]: forced available 9.9.9");
        }
        if (ImGui::Button("Reset")) {
            ResetUpdateCheck();
            LOG_INFO("updcheck[dev]: state reset");
        }
        ImGui::TextDisabled("Run/Force light the shortcut icon badge, the Options banner,");
        ImGui::TextDisabled("and the shortcut right-click \"update\" item.");
#else
        // ---- Dist-flavored: Nexus' native updater ----
        ImGui::TextUnformatted("flavor : dist  (Nexus native auto-update)");
        ImGui::Text("update link: %s",
                    AddonDef.UpdateLink ? AddonDef.UpdateLink : "(none)");
        ImGui::TextWrapped("The public Distribution build auto-updates via Nexus' GitHub "
                           "provider. This forces Nexus to fetch the latest release DLL now "
                           "(no version check).");
        if (ImGui::Button("Request update via Nexus")) {
            APIDefs->RequestUpdate(
                AddonDef.Signature,
                "https://github.com/isimp/emot3/releases/latest/download/emot3.dll");
            LOG_INFO("updcheck[dev]: RequestUpdate fired");
        }
#endif
    }
    ImGui::End();
}

#endif  // EMOT3_DEVTOOLS
