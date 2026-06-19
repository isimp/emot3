#include "UpdateCheckDebug.h"

#ifdef EMOT3_DEVTOOLS

#include "Globals.h"            // APIDefs (+ Windows.h, nexus/Nexus.h -> AddonDefinition)
#include "Logging.h"
#include "DevStateInspector.h"  // DevStateRow (aligned key/value rows)

#include "imgui/imgui.h"

#ifdef EMOT3_PLUS
#include "UpdateCheck.h"   // custom update check + dev hooks (+plus builds)
#include "PlusSettings.h"  // g_PlusSettings.NotifyPrereleases (channel readout)
#endif

// The addon definition (Name / Version / Signature / Provider / UpdateLink) is a
// global owned by entry.cpp; read it so the readout reflects the live build.
extern AddonDefinition AddonDef;

// Tester for the update flow. Behaves per build flavor so a +plus dev build
// drives the custom GitHub check and a base one drives Nexus' native updater -
// see UpdateCheckDebug.h.
void RenderUpdateCheckBody() {
    {
        DevStateRow("addon", "%s  v%d.%d.%d.%d",
                    AddonDef.Name ? AddonDef.Name : "?",
                    AddonDef.Version.Major, AddonDef.Version.Minor,
                    AddonDef.Version.Build, AddonDef.Version.Revision);
        DevStateRow("sig", "%d   provider: %d", AddonDef.Signature,
                    (int)AddonDef.Provider);
        ImGui::Separator();

#ifdef EMOT3_PLUS
        // ---- +plus: the custom GitHub update check ----
        DevStateRow("flavor", "plus  (custom GitHub update check)");
        DevStateRow("available", "%s", PlusUpdateAvailable() ? "yes" : "no");
        {
            std::string latest = PlusLatestVersion();
            DevStateRow("latest", "%s", latest.empty() ? "(none)" : latest.c_str());
        }
        DevStateRow("notify prereleases", "%s",
                    g_PlusSettings.NotifyPrereleases ? "on (/releases)" : "off (/releases/latest)");
        if (ImGui::Button("Run check now")) {
            RunUpdateCheckNow();   // uses the live NotifyPrereleases channel
            LOG_INFO("updcheck[dev]: manual GitHub check kicked");
        }
        // Force the prerelease channel for one run regardless of the setting -
        // exercises the new /releases array path + prerelease ranking end-to-end.
        if (ImGui::Button("Run check (incl. prereleases)")) {
            RunUpdateCheckNowChannel(/*prereleases=*/true);
            LOG_INFO("updcheck[dev]: manual GitHub check kicked (prereleases forced)");
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
        // ---- base: Nexus' native updater ----
        DevStateRow("flavor", "base  (Nexus native auto-update)");
        DevStateRow("update link", "%s",
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
}

#endif  // EMOT3_DEVTOOLS
