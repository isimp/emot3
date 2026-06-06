#include "Options.h"
#include "OptionsCommon.h"  // RenderGeneral/Quickbar/EmotesOptionsTab
#include "Globals.h"
#include "Logging.h"
#include "I18n.h"
#include "Settings.h"
#include "EmoteData.h"      // SaveEmotesJson
#include "IconBrowse.h"     // DrainIconBrowse
#include "UnlockScan.h"     // DrainUnlockSync (GW2-API unlock sync)
#include "UpdateCheck.h"    // DrainUpdateCheck (Plus update hint; no-op stub otherwise)
#include "Profiling.h"      // PROFILE_SCOPE macro (no-op without EMOT3_DEVTOOLS)
#include "DevTools.h"       // dev-tools framework toggles (only in EMOT3_DEVTOOLS builds)

#include "imgui/imgui.h"

#include <string>
// ---- Lifecycle helper -------------------------------------------------

void ApplyQbCloseOnEsc() {
    if (!APIDefs) return;
    if (g_Settings.QuickbarCloseOnEsc) {
        APIDefs->UI.RegisterCloseOnEscape("emot3 Quickbar##qb",
                                           &g_Settings.ShowQuickbar);
        LOG_DEBUG("Quickbar close-on-Escape: ON");
    } else {
        APIDefs->UI.DeregisterCloseOnEscape("emot3 Quickbar##qb");
        LOG_DEBUG("Quickbar close-on-Escape: OFF");
    }
}

// ---- Top-level dispatcher ---------------------------------------------

void AddonOptions() {
    PROFILE_SCOPE("opt.frame");  // dev perf overlay
    // Pick up any completed file-browse result from the worker thread
    // before drawing this frame.
    if (DrainIconBrowse()) {
        if (!g_EmotesJsonPath.empty()) SaveEmotesJson(g_EmotesJsonPath);
        MarkEmotesDirty();
    }
    // Apply any completed GW2-API unlock sync + drive H&S retries (render-side).
    DrainUnlockSync();
    // Plus-only: tick the "newer release available" check (no-op stub otherwise).
    DrainUpdateCheck();

    // Developer section - the dev-tool overlay toggles ("[debug] ..."), grouped
    // under one collapsed header (a "dropdown") at the top of the addon options,
    // just below Nexus' keybind list. Only the +devtools flavor compiles this in,
    // so a public base build shows nothing here. Raw English labels on purpose -
    // dev-only, never translated. See DevTools.h. (The +plus input-swallow
    // toggles are NOT here - they're shipped, user-facing settings: wheel routing
    // lives in the Quickbar tab, "send while moving" in General > Sending.)
#ifdef EMOT3_DEVTOOLS
    if (ImGui::CollapsingHeader("Developer##emot3dev")) {
        ImGui::Indent();
        // One "[debug] <label>" checkbox per registered tool. Not persisted.
        RenderDevToolToggles();
        ImGui::Unindent();
    }
    ImGui::Separator();
    ImGui::Spacing();
#endif

    if (ImGui::BeginTabBar("##emot3tabs")) {
        // "label###stableid" - the visible label is translated, but the
        // ID after ### stays fixed, so switching UI language doesn't make
        // ImGui lose track of which tab is open and jump to another one.
        std::string tGeneral  = std::string(L("opt.tab.general"))  + "###tab_general";
        std::string tQuickbar = std::string(L("opt.tab.quickbar")) + "###tab_quickbar";
        std::string tEmotes   = std::string(L("opt.tab.emotes"))   + "###tab_emotes";
        std::string tUnlocks  = std::string(L("opt.tab.unlocks"))  + "###tab_unlocks";
        if (ImGui::BeginTabItem(tGeneral.c_str())) {
            RenderGeneralOptionsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tQuickbar.c_str())) {
            RenderQuickbarOptionsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tUnlocks.c_str())) {
            RenderUnlocksTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tEmotes.c_str())) {
            RenderEmotesTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}