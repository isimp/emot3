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
#include "DevSettings.h"    // persisted swallow toggles (+plus flavor only)

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
        g_EmotesDirty = true;
    }
    // Apply any completed GW2-API unlock sync + drive H&S retries (render-side).
    DrainUnlockSync();
    // Plus-only: tick the "newer release available" check (no-op stub otherwise).
    DrainUpdateCheck();

    // Developer section - the dev-tool overlay toggles + persisted swallow
    // toggles, grouped under one collapsed header (a "dropdown") at the top of
    // the addon options, just below Nexus' keybind list. Two independent flavor
    // axes live here: the [debug] overlay toggles (+devtools, EMOT3_DEVTOOLS) and
    // the [dev] input-swallow toggles (+plus, EMOT3_PLUS). The header + its
    // trailing separator only exist when at least one is compiled in, so a public
    // base build (neither flavor) shows nothing here. Raw English labels
    // on purpose - dev-only, never translated. See DevTools.h / DevSettings.h.
#if defined(EMOT3_DEVTOOLS) || defined(EMOT3_PLUS)
    if (ImGui::CollapsingHeader("Developer##emot3dev")) {
        ImGui::Indent();
#ifdef EMOT3_DEVTOOLS
        // Dev-tool overlay toggles ("[debug] ..."), one per registered tool.
        // Not persisted. See DevTools.h.
        RenderDevToolToggles();
#endif
#ifdef EMOT3_PLUS
        // Persisted dev SWALLOW toggles (dev.json) - a separate axis from the
        // overlays above. The input-swallow routing they control consumes input
        // in the WndProc (AV-sensitive), so they're compiled in only for the
        // +plus flavor along with their settings.
        if (ImGui::Checkbox("[dev] Click-through wheel routing",
                            &g_DevSettings.QbClickThroughWheel))
            SaveDevSettings();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "On: in click-through mode, the mouse wheel over the Quickbar\n"
                "scrolls/cycles it and the game camera doesn't zoom.\n"
                "Off: the wheel passes through to the game like clicks do.\n"
                "Dev-only (consumes WM_MOUSEWHEEL); stripped from release builds.");
        if (ImGui::Checkbox("[dev] Swallow input during emote send",
                            &g_DevSettings.SwallowInputOnSend))
            SaveDevSettings();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "On: after you click an emote, your keyboard is swallowed for the\n"
                "~150ms injection so held keys (WASD) can't garble the command -\n"
                "you can click while moving. Still refuses if GW2 chat is focused.\n"
                "Off (default): instead of swallowing, the send is refused when a\n"
                "printable key is held (the safe, release-build behavior).\n"
                "Dev-only (consumes keyboard in the WndProc); stripped from releases.");
#endif
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