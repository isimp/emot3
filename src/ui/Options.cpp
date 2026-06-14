#include "Options.h"
#include "OptionsCommon.h"  // RenderGeneral/Quickbar/EmotesOptionsTab
#include "OptionsMeMotes.h" // RenderMeMotesTab — /me-motes editor
#include "OptionsRadial.h"  // RenderRadialTab — RadialMenus wheel export
#include "Globals.h"
#include "Logging.h"
#include "I18n.h"
#include "Settings.h"
#include "EmoteData.h"      // SaveEmotesJson
#include "IconPicker.h"     // RenderIconPicker (in-app icon grid)
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
    // Apply any completed GW2-API unlock sync + drive H&S retries (render-side).
    DrainUnlockSync();
    // Plus-only: tick the "newer release available" check (no-op stub otherwise).
    DrainUpdateCheck();
    // Render the in-app icon picker modal (no-op until opened by a "Library..."
    // button in an editor). Done here at the AddonOptions root so its open +
    // render ID context is stable regardless of which tab opened it.
    RenderIconPicker();

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
        std::string tGeneral  = std::string(L("opt.tab.general"))   + "###tab_general";
        std::string tUnlocks  = std::string(L("opt.tab.unlocks"))   + "###tab_unlocks";
        std::string tQuickbar = std::string(L("opt.tab.quickbar"))  + "###tab_quickbar";
        std::string tPalette  = std::string(L("opt.tab.palette"))   + "###tab_palette";
        std::string tRadial   = std::string(L("opt.tab.radial"))    + "###tab_radial";
        std::string tAutoMote = std::string(L("opt.tab.automotes")) + "###tab_automotes";
        std::string tEmotes   = std::string(L("opt.tab.emotes"))    + "###tab_emotes";
        std::string tMeMotes  = std::string(L("opt.tab.me_motes"))  + "###tab_me_motes";
        // Tab order (v1.5): General, Unlocks, then the send surfaces (Quickbar,
        // Palette, RadialMenus), then Auto-motes, then the editors (Catalog,
        // /me-motes).
        if (ImGui::BeginTabItem(tGeneral.c_str())) {
            RenderGeneralOptionsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tUnlocks.c_str())) {
            RenderUnlocksTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tQuickbar.c_str())) {
            RenderQuickbarOptionsTab();
            ImGui::EndTabItem();
        }
        // Palette — its own tab (started as a General section and outgrew it). Sits
        // next to Quickbar: both are send-surface windows.
        if (ImGui::BeginTabItem(tPalette.c_str())) {
            RenderPaletteOptionsTab();
            ImGui::EndTabItem();
        }
        // RadialMenus — export favorites categories as wheels. See ui/OptionsRadial.cpp.
        if (ImGui::BeginTabItem(tRadial.c_str())) {
            RenderRadialTab();
            ImGui::EndTabItem();
        }
        // Auto-motes — chat-keyword auto-fire (was a General section). See
        // RenderAutoMotesTab in ui/OptionsGeneral.cpp.
        if (ImGui::BeginTabItem(tAutoMote.c_str())) {
            RenderAutoMotesTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tEmotes.c_str())) {
            RenderEmotesTab();
            ImGui::EndTabItem();
        }
        // /me-motes — separate editor, separate JSON file. See
        // ui/OptionsMeMotes.cpp + data/MeMotes.h.
        if (ImGui::BeginTabItem(tMeMotes.c_str())) {
            RenderMeMotesTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}