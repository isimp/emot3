#include "NotifierDebug.h"

#ifdef EMOT3_DEVTOOLS

#include "Globals.h"
#include "Settings.h"
#include "EmoteData.h"
#include "MainPanel.h"          // DetectNewBundledEmotes (the real, setting-gated path)
#include "Logging.h"
#include "DevStateInspector.h"  // DevStateRow (aligned key/value rows)

#include "imgui/imgui.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// Tester for the new-bundled-emote notifier. Drives the production helpers +
// globals so it validates the real pipeline (snapshot diff, catalog filter,
// dialog, add-by-id), not a mock. See NotifierDebug.h.
void RenderNotifierBody() {
    {
        std::vector<std::string> bundle = AllBundledEmoteIds();
        int catalog = 0;
        { std::lock_guard<std::mutex> lk(g_EmotesMutex); catalog = (int)g_Emotes.size(); }

        DevStateRow("notify setting", "%s", g_Settings.NotifyNewBundledEmotes ? "on" : "off");
        DevStateRow("known snapshot", "%d", (int)g_Settings.KnownBundledEmotes.size());
        DevStateRow("bundle total",   "%d", (int)bundle.size());
        DevStateRow("catalog size",   "%d", catalog);
        DevStateRow("prompt pending", "%s", g_PromptNewBundledEmotes ? "yes" : "no");
        DevStateRow("staged new ids", "%d", (int)g_NewBundledEmoteIds.size());
        ImGui::Separator();

        // 1) Eyeball the dialog: force it open with a few real ids, IGNORING the
        //    notify setting (always shows). (These ids are usually already in the
        //    catalog, so the dialog's "Add them" adds 0 - it's a UI check.)
        if (ImGui::Button("Force prompt (ignores setting)")) {
            g_NewBundledEmoteIds.clear();
            for (size_t i = 0; i < bundle.size() && i < 5; ++i)
                g_NewBundledEmoteIds.push_back(bundle[i]);
            g_PromptNewBundledEmotes = !g_NewBundledEmoteIds.empty();
            LOG_INFO("notifier[dev]: forced prompt with %d sample id(s)",
                     (int)g_NewBundledEmoteIds.size());
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Forces the dialog open regardless of the\n"
                              "\"Tell me about new built-in emotes\" setting.");

        // 2) Realistic end-to-end, RESPECTS the notify setting: drop one bundled
        //    emote from BOTH the catalog and the snapshot, then run the real
        //    DetectNewBundledEmotes() - the same path AddonLoad uses. With the
        //    setting on, the dropped emote is genuinely "new" and the prompt
        //    stages it (the dialog's "Add them" restores it - a clean round trip);
        //    with the setting off, nothing shows. This is how to test the gate.
        if (ImGui::Button("Simulate update (respects setting)")) {
            std::unordered_set<std::string> bset(bundle.begin(), bundle.end());
            std::string victim;
            {
                std::lock_guard<std::mutex> lk(g_EmotesMutex);
                for (auto it = g_Emotes.rbegin(); it != g_Emotes.rend(); ++it)
                    if (bset.count(it->Id)) { victim = it->Id; break; }
                if (!victim.empty())
                    g_Emotes.erase(std::remove_if(g_Emotes.begin(), g_Emotes.end(),
                        [&](const Emote& e){ return e.Id == victim; }), g_Emotes.end());
            }
            if (!victim.empty()) {
                auto& k = g_Settings.KnownBundledEmotes;
                k.erase(std::remove(k.begin(), k.end(), victim), k.end());
                if (!g_EmotesJsonPath.empty()) SaveEmotesJson(g_EmotesJsonPath);
                if (!g_SettingsPath.empty())   SaveSettings(g_SettingsPath);
                MarkEmotesDirty();
                // The real, setting-gated detection - so "notify off" => no prompt.
                DetectNewBundledEmotes();
                LOG_INFO("notifier[dev]: dropped '%s'; detect staged %d, prompt=%s",
                         victim.c_str(), (int)g_NewBundledEmoteIds.size(),
                         g_PromptNewBundledEmotes ? "yes" : "no");
            } else {
                LOG_WARNING("notifier[dev]: no bundled emote in the catalog to drop");
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drops one emote from the catalog + snapshot, then runs\n"
                              "the real detection. Honors the notify setting:\n"
                              "off => no prompt. \"Add them\" restores the emote.");

        // 3) Back to baseline: snapshot = full bundle, nothing pending.
        if (ImGui::Button("Reset snapshot to full bundle")) {
            g_Settings.KnownBundledEmotes = bundle;
            if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
            g_NewBundledEmoteIds.clear();
            g_PromptNewBundledEmotes = false;
            LOG_INFO("notifier[dev]: snapshot reset to full bundle (%d)", (int)bundle.size());
        }
    }
}

#endif  // EMOT3_DEVTOOLS
