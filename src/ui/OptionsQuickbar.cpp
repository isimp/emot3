#include "Options.h"
#include "OptionsCommon.h"
#include "Globals.h"
#include "I18n.h"
#include "Settings.h"
#include "QuickbarPresets.h"
#include "Favorites.h"      // TrimName for the preset-name entry
#include "Layout.h"         // ToggleButton, PushInvalidInputStyle
#include "Logging.h"        // LOG_TRACE / LOG_DEBUG (setting + auto-disable)
#include "Profiling.h"      // PROFILE_SCOPE macro (no-op without EMOT3_DEVTOOLS)

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"  // PushItemFlag / ImGuiItemFlags_Disabled

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
// ---- Tab: Quickbar ----------------------------------------------------

// ---- Tab: Quickbar - Presets section ----------------------------------
//
// Named snapshots of the Quickbar settings + window geometry, one JSON file
// each under presets/ (see QuickbarPresets.cpp). The live g_Settings IS the
// edit buffer - the controls below this section are the editor, with live
// preview - so there's no separate per-preset form: Apply loads one, tweak
// the controls, Update saves the current state back. New/Rename/Delete manage
// the set. Update is guarded by a modified check (some setting or the window
// geometry diverged), so it's a no-op-free, deliberate save.
static void RenderQuickbarPresetsSection() {
    OptionsSection(L("opt.sec.presets"));

    int count = (int)g_QuickbarPresets.size();

    static int  s_sel           = 0;      // index into g_QuickbarPresets
    static int  s_mode          = 0;      // 0 = none, 1 = new, 2 = rename
    static char s_nameBuf[64]   = {};
    static bool s_confirmDelete = false;

    if (s_sel >= count) s_sel = count - 1;
    if (s_sel < 0)      s_sel = count > 0 ? 0 : -1;
    bool hasSel = s_sel >= 0 && s_sel < count;

    // Enabled/disabled button helper: ItemFlags_Disabled gates the click (and
    // dims via alpha) but still lets IsItemHovered fire so we can explain why
    // it's disabled. Returns true only on an enabled click.
    auto actionButton = [](const char* label, bool enabled) -> bool {
        if (!enabled) {
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
        }
        bool clicked = ImGui::Button(label);
        if (!enabled) { ImGui::PopStyleVar(); ImGui::PopItemFlag(); }
        return clicked && enabled;
    };
    auto selectByName = [&](const std::string& nm) {
        for (int i = 0; i < (int)g_QuickbarPresets.size(); ++i)
            if (g_QuickbarPresets[i].Name == nm) { s_sel = i; return; }
    };

    // --- Row 1: preset combo + Apply (+ modified hint) ---
    ImGui::SetNextItemWidth(200.f);
    const char* preview = hasSel ? g_QuickbarPresets[s_sel].Name.c_str()
                                 : L("opt.qb.preset.none");
    if (ImGui::BeginCombo("##qbpreset", preview)) {
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            bool sel = i == s_sel;
            if (ImGui::Selectable(g_QuickbarPresets[i].Name.c_str(), sel)) {
                s_sel = i;
                s_mode = 0; s_confirmDelete = false;   // cancel any pending edit
            }
            if (sel) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // Computed after the combo so a selection change this frame isn't stale.
    bool modified = hasSel &&
                    !QuickbarPresetMatchesCurrent(g_QuickbarPresets[s_sel]);

    ImGui::SameLine();
    if (actionButton(L("opt.qb.preset.apply"), hasSel)) {
        ApplyQuickbarPreset(g_QuickbarPresets[s_sel]);
        ApplyQbCloseOnEsc();   // CloseOnEsc registration is side-effectful
        s_mode = 0; s_confirmDelete = false;
        if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", hasSel ? L("opt.qb.preset.apply_tooltip")
                                       : L("opt.qb.preset.select_first"));
    if (modified) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", L("opt.qb.preset.modified"));
    }

    // --- Row 2: New / Update / Rename / Delete ---
    if (actionButton(L("opt.qb.preset.new"), true)) {
        s_mode = 1; s_confirmDelete = false; s_nameBuf[0] = '\0';
    }
    if (ImGui::IsItemHovered())
        TooltipText("opt.qb.preset.new_tooltip");

    ImGui::SameLine();
    if (actionButton(L("opt.qb.preset.update"), hasSel && modified)) {
        CaptureQuickbarPreset(g_QuickbarPresets[s_sel]);   // mutates in place
        WriteQuickbarPreset(g_QuickbarPresets[s_sel]);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", !hasSel ? L("opt.qb.preset.select_first")
                                : !modified ? L("opt.qb.preset.update_clean")
                                            : L("opt.qb.preset.update_tooltip"));

    ImGui::SameLine();
    if (actionButton(L("opt.qb.preset.rename"), hasSel)) {
        s_mode = 2; s_confirmDelete = false;
        snprintf(s_nameBuf, sizeof(s_nameBuf), "%s",
                 g_QuickbarPresets[s_sel].Name.c_str());
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", hasSel ? L("opt.qb.preset.rename_tooltip")
                                       : L("opt.qb.preset.select_first"));

    ImGui::SameLine();
    if (actionButton(L("common.delete"), hasSel)) {
        s_confirmDelete = true; s_mode = 0;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", hasSel ? L("opt.qb.preset.delete_tooltip")
                                       : L("opt.qb.preset.select_first"));

    // --- Inline name entry (New / Rename) ---
    if (s_mode != 0) {
        std::string trimmed = TrimName(s_nameBuf);
        int  excludeIdx = s_mode == 2 ? s_sel : -1;
        bool empty   = trimmed.empty();
        bool dup     = !empty && QuickbarPresetNameExists(trimmed, excludeIdx);
        // Both empty and duplicate flag the box itself (red border + tooltip),
        // matching every other validated input - no muted hint written below.
        bool invalid = empty || dup;

        ImGui::SetNextItemWidth(200.f);
        if (invalid) PushInvalidInputStyle();
        bool enter = ImGui::InputText("##presetname", s_nameBuf, sizeof(s_nameBuf),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        if (invalid) { PopInvalidInputStyle(); DrawInvalidInputBorder(); }
        if (invalid && ImGui::IsItemHovered()) {
            if (dup) ImGui::SetTooltip(L("opt.qb.preset.name_dup"), trimmed.c_str());
            else     TooltipText("common.name_empty");
        }

        ImGui::SameLine();
        bool save = ImGui::Button((std::string(L("opt.qb.preset.save")) + "##psave").c_str());
        ImGui::SameLine();
        if (ImGui::Button((std::string(L("common.cancel")) + "##pcancel").c_str())) {
            s_mode = 0; s_nameBuf[0] = '\0';
        } else if ((save || enter) && !invalid) {
            if (s_mode == 1) {
                QuickbarPreset p;
                p.Name = trimmed;
                CaptureQuickbarPreset(p);
                WriteQuickbarPreset(p);
            } else {
                RenameQuickbarPreset(g_QuickbarPresets[s_sel], trimmed);
            }
            LoadQuickbarPresets();     // re-scan + re-sort to match disk
            selectByName(trimmed);
            s_mode = 0; s_nameBuf[0] = '\0';
        }
    }

    // --- Inline delete confirmation ---
    if (s_confirmDelete && hasSel) {
        ImGui::TextDisabled(L("opt.qb.preset.delete_confirm"),
                            g_QuickbarPresets[s_sel].Name.c_str());
        ImGui::SameLine();
        if (ImGui::Button((std::string(L("common.delete")) + "##pdelyes").c_str())) {
            DeleteQuickbarPresetFile(g_QuickbarPresets[s_sel]);
            LoadQuickbarPresets();
            if (s_sel >= (int)g_QuickbarPresets.size())
                s_sel = (int)g_QuickbarPresets.size() - 1;
            s_confirmDelete = false;
        }
        ImGui::SameLine();
        if (ImGui::Button((std::string(L("common.cancel")) + "##pdelno").c_str()))
            s_confirmDelete = false;
    }

    // The single place we tell the user geometry travels with a preset.
    ImGui::TextDisabled("%s", L("opt.qb.preset.help"));
}

void RenderQuickbarOptionsTab() {
    PROFILE_SCOPE("opt.quickbar");  // dev perf overlay
    // Primary visibility toggle at the top of the tab. The label
    // flips between "Show Quickbar" and "Hide Quickbar" so it always
    // describes the action the click will take, not the current
    // state - same pattern as the Nexus shortcut's context menu, and
    // a fix for "Show ..." reading as a lie when the window is
    // already open. Bumped frame padding + Spacing on each side +
    // the inline status label give it the weight of a primary
    // action; the default-sized button crammed against the tab
    // header read as cramped and indistinct from the checkboxes
    // below.
    ImGui::Spacing();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.f, 6.f));
    const char* qbToggleLabel = g_Settings.ShowQuickbar
                                ? L("opt.qb.hide")
                                : L("opt.qb.show");
    if (ToggleButton(qbToggleLabel, &g_Settings.ShowQuickbar)) {
        if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
    }
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered())
        TooltipText("opt.qb.toggle_tooltip");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_Settings.ShowQuickbar ? L("opt.currently_visible")
                                                      : L("opt.currently_hidden"));
    ImGui::Spacing();

    // ===== Presets ===== (top of the tab; the controls below are its editor)
    RenderQuickbarPresetsSection();

    // ===== Layout =====
    OptionsSection(L("opt.sec.layout"));

    ImGui::Text("%s", L("opt.qb.view_mode"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.f);
    // Display order — descending information density (Full → Compact →
    // Icon → Text only). Decoupled from the numeric enum values via the
    // lookup below, so we can reorder the dropdown without breaking
    // existing settings.json files (which store the raw enum integers).
    static const EViewMode kQbViewOrder[] = {
        EViewMode::Full,
        EViewMode::Compact,
        EViewMode::Icon,
        EViewMode::TextOnly,
    };
    const char* qbViewNames[] = { L("view.full"), L("view.compact"),
                                  L("view.icon"), L("view.textonly") };
    int qbDisplayIdx = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kQbViewOrder); ++i) {
        if (kQbViewOrder[i] == g_Settings.QuickbarViewMode) { qbDisplayIdx = i; break; }
    }
    if (ImGui::Combo("##qbvm", &qbDisplayIdx, qbViewNames, IM_ARRAYSIZE(qbViewNames))) {
        g_Settings.QuickbarViewMode = kQbViewOrder[qbDisplayIdx];
        LOG_TRACE("setting quickbar.view_mode = %d", (int)g_Settings.QuickbarViewMode);
        if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
    }
    if (ImGui::IsItemHovered()) {
        static const TooltipOption kQbViewOpts[] = {
            { "view.full",     "view.full.desc",     false },
            { "view.compact",  "view.compact.desc",  false },
            { "view.icon",     "view.icon.desc",     false },
            { "view.textonly", "view.textonly.desc", false },
        };
        TooltipOptions(nullptr, kQbViewOpts, IM_ARRAYSIZE(kQbViewOpts));
    }

    // Per-mode floor — Full/Text-only/Compact need 1× for label space, Icon
    // scales from 0.5×; the 2.5× cap is uniform. See MinIconScaleForMode.
    float scaleMin = MinIconScaleForMode(g_Settings.QuickbarViewMode);
    if (g_Settings.QuickbarIconScale < scaleMin) g_Settings.QuickbarIconScale = scaleMin;

    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderFloat(L("opt.qb.icon_scale"), &g_Settings.QuickbarIconScale, scaleMin, 2.5f, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        LOG_TRACE("setting quickbar.icon_scale = %.2f", g_Settings.QuickbarIconScale);
        if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        g_Settings.QuickbarIconScale = 1.0f;
        LOG_TRACE("setting quickbar.icon_scale = %.2f (reset)", g_Settings.QuickbarIconScale);
        if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
    }
    if (ImGui::IsItemHovered())
        TooltipText("opt.qb.scale_tooltip");

    CheckboxWithSaveAndTooltip("opt.qb.show_cat_bar", &g_Settings.ShowQuickbarCategoryBar, /*defaultIsOn=*/true);

    // Dropdown style indented under the category bar — gated on the bar
    // being shown (a dropdown style is meaningless without the bar).
    ImGui::Indent();
    DisabledCheckbox("opt.qb.use_dropdown", &g_Settings.QuickbarUseDropdown,
                     /*enabled=*/g_Settings.ShowQuickbarCategoryBar,
                     /*defaultIsOn=*/false, "opt.qb.use_dropdown_disabled");
    ImGui::Unindent();

    // ===== Scrolling ===== (what content does inside the window)
    OptionsSection(L("opt.sec.scrolling"));

    // What signals "there's more": off / edge hints / scrollbar - mutually
    // exclusive so the bar and the hints never share an edge. Enum value maps
    // straight to the item index (0/1/2), same as the wheel/combat combos.
    // Sits at the top of the section as its headline choice.
    {
        const char* indNames[] = { L("opt.qb.scroll_ind_off"),
                                   L("opt.qb.scroll_ind_hints"),
                                   L("opt.qb.scroll_ind_bar") };
        int indIdx = (int)g_Settings.QuickbarScrollIndicator;
        ImGui::Text("%s", L("opt.qb.scroll_indicator"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.f);
        if (ImGui::Combo("##qbscrollind", &indIdx, indNames, IM_ARRAYSIZE(indNames))) {
            g_Settings.QuickbarScrollIndicator = (EQbScrollIndicator)indIdx;
            LOG_TRACE("setting quickbar.scroll_indicator = %d", (int)g_Settings.QuickbarScrollIndicator);
            if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
        }
        if (ImGui::IsItemHovered()) {
            static const TooltipOption kIndOpts[] = {
                { "opt.qb.scroll_ind_off",   "opt.qb.scroll_ind_off.desc",   false },
                { "opt.qb.scroll_ind_hints", "opt.qb.scroll_ind_hints.desc", false },
                { "opt.qb.scroll_ind_bar",   "opt.qb.scroll_ind_bar.desc",   true  },
            };
            TooltipOptions("opt.qb.scroll_indicator.intro", kIndOpts, IM_ARRAYSIZE(kIndOpts));
        }
    }

    CheckboxWithSaveAndTooltip("opt.qb.hscroll", &g_Settings.QuickbarHorizontalScroll, /*defaultIsOn=*/false);

    CheckboxWithSaveAndTooltip("opt.qb.snap_scroll", &g_Settings.QuickbarSnapScroll, /*defaultIsOn=*/true);

    CheckboxWithSaveAndTooltip("opt.qb.scroll_wrap", &g_Settings.QuickbarScrollWrap, /*defaultIsOn=*/false);

    // ===== Window ===== (the frame: size, move, chrome)
    OptionsSection(L("opt.sec.window"));

    CheckboxWithSaveAndTooltip("opt.qb.allow_resize", &g_Settings.QuickbarAllowResize, /*defaultIsOn=*/true);

    CheckboxWithSaveAndTooltip("opt.qb.allow_move", &g_Settings.QuickbarAllowMove, /*defaultIsOn=*/true);

    // "Fit window to grid" snaps resize to whole cells - a resize behavior, so
    // it sits with the frame controls (not the content-scroll group above).
    CheckboxWithSaveAndTooltip("opt.qb.snap_window", &g_Settings.QuickbarSnapWindow, /*defaultIsOn=*/true);

    CheckboxWithSaveAndTooltip("opt.qb.show_bg", &g_Settings.ShowQuickbarBg, /*defaultIsOn=*/true);

    // Click-through requires (a) the background already be hidden and
    // (b) some way to still drag the window if move is allowed -
    // either the title bar visible, OR "Allow move" off. Otherwise
    // the user could create a transparent, click-through, no-title
    // window that's impossible to drag.
    //
    // If the user is already in click-through mode and then toggles
    // another setting that lands the combination in an invalid
    // state (e.g. they had Title on + Move on + bg off + Click-
    // through on, then turn Title off), we don't want to leave the
    // click-through flag stuck at true while the checkbox sits
    // greyed out. The runtime would still honor it, since the only
    // gate there is bg-off, leaving the user with an unrecoverable
    // window. Auto-correct: when ctEnabled is false but the stored
    // flag is true, force it false and persist before we render the
    // checkbox so the UI immediately reflects the corrected state.
    ImGui::Indent();
    bool ctNoDragHandle = !g_Settings.ShowQuickbarTitle && g_Settings.QuickbarAllowMove;
    bool ctEnabled = !g_Settings.ShowQuickbarBg && !ctNoDragHandle;
    if (!ctEnabled && g_Settings.QuickbarClickThrough) {
        g_Settings.QuickbarClickThrough = false;
        LOG_DEBUG("quickbar: click-through auto-disabled (%s)",
                  g_Settings.ShowQuickbarBg ? "background shown" : "no drag handle");
        if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
    }
    if (!ctEnabled) {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    }
    bool clickChanged = ImGui::Checkbox(
        L("opt.qb.clickthrough"),
        &g_Settings.QuickbarClickThrough);
    if (!ctEnabled) {
        ImGui::PopStyleVar();
        ImGui::PopItemFlag();
        if (clickChanged) {
            g_Settings.QuickbarClickThrough = !g_Settings.QuickbarClickThrough;
            clickChanged = false;
        }
    }
    if (clickChanged && !g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
    if (ImGui::IsItemHovered()) {
        if (g_Settings.ShowQuickbarBg) {
            TooltipText("opt.qb.ct_need_bg");
        } else if (ctNoDragHandle) {
            TooltipText("opt.qb.ct_no_drag");
        } else {
            TooltipOnOff("opt.qb.ct.on", "opt.qb.ct.off", /*defaultIsOn=*/false);
        }
    }
    ImGui::Unindent();

    CheckboxWithSaveAndTooltip("opt.qb.show_title", &g_Settings.ShowQuickbarTitle, /*defaultIsOn=*/true);

    // ===== Look =====
    OptionsSection(L("opt.sec.look"));

    CheckboxWithSaveAndTooltip("opt.qb.high_contrast", &g_Settings.QuickbarHighContrast, /*defaultIsOn=*/false);

    CheckboxWithSaveAndTooltip("opt.qb.show_tooltips", &g_Settings.ShowQuickbarTooltips, /*defaultIsOn=*/true);

    // ===== Interaction =====
    OptionsSection(L("opt.sec.interaction"));

    // Single 3-way control for "when does the wheel cycle the category"
    // (Off / Over the category bar / Anywhere). Replaces the old pair of
    // booleans where "anywhere" silently superseded "over bar" - the combo
    // makes the three states mutually exclusive and self-evident. Enum
    // value maps straight to the item index (0/1/2), no lookup table.
    {
        const char* wheelNames[] = { L("opt.qb.wheel_off"),
                                     L("opt.qb.wheel_overbar"),
                                     L("opt.qb.wheel_anywhere") };
        int wheelIdx = (int)g_Settings.QuickbarWheelCycle;
        // Label-then-control, matching the View mode combo above (an ImGui
        // combo's own label renders to its right, which read as misplaced).
        ImGui::Text("%s", L("opt.qb.wheel_cycle"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.f);
        if (ImGui::Combo("##qbwheel", &wheelIdx, wheelNames,
                         IM_ARRAYSIZE(wheelNames))) {
            g_Settings.QuickbarWheelCycle = (EWheelCycle)wheelIdx;
            LOG_TRACE("setting quickbar.wheel_cycle = %d", (int)g_Settings.QuickbarWheelCycle);
            if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
        }
        if (ImGui::IsItemHovered()) {
            static const TooltipOption kWheelOpts[] = {
                { "opt.qb.wheel_off",      "opt.qb.wheel_off.desc",      false },
                { "opt.qb.wheel_overbar",  "opt.qb.wheel_overbar.desc",  true  },
                { "opt.qb.wheel_anywhere", "opt.qb.wheel_anywhere.desc", false },
            };
            TooltipOptions("opt.qb.wheel_cycle.intro", kWheelOpts, IM_ARRAYSIZE(kWheelOpts));
        }
        // Wrap-around is a modifier of the cycle behaviour, so it rides on the
        // same line as the dropdown (only meaningful while cycling is enabled).
        if (g_Settings.QuickbarWheelCycle != EWheelCycle::Off) {
            ImGui::SameLine();
            CheckboxWithSaveAndTooltip("opt.qb.wheel_cycle_wrap", &g_Settings.QuickbarWheelCycleWrap, /*defaultIsOn=*/true);
        }
    }

    // In combat: off / grey out / hide. Its own Quickbar setting (not the
    // General-tab "can't be used" dropdown) - emotes still work in combat, so
    // this is a Quickbar-only presentation choice. Enum value maps straight to
    // the item index (0/1/2), same as the wheel combo above.
    {
        const char* combatNames[] = { L("opt.qb.combat_off"),
                                      L("opt.qb.combat_grey"),
                                      L("opt.qb.combat_hide") };
        int combatIdx = (int)g_Settings.QuickbarCombatBehavior;
        ImGui::Text("%s", L("opt.qb.combat"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.f);
        if (ImGui::Combo("##qbcombat", &combatIdx, combatNames, IM_ARRAYSIZE(combatNames))) {
            g_Settings.QuickbarCombatBehavior = (EQbCombat)combatIdx;
            LOG_TRACE("setting quickbar.combat_behavior = %d", (int)g_Settings.QuickbarCombatBehavior);
            if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
        }
        if (ImGui::IsItemHovered()) {
            static const TooltipOption kCombatOpts[] = {
                { "opt.qb.combat_off",  "opt.qb.combat_off.desc",  true  },
                { "opt.qb.combat_grey", "opt.qb.combat_grey.desc", false },
                { "opt.qb.combat_hide", "opt.qb.combat_hide.desc", false },
            };
            TooltipOptions("opt.qb.combat.intro", kCombatOpts, IM_ARRAYSIZE(kCombatOpts));
        }
    }

    if (CheckboxWithSaveAndTooltip("opt.qb.close_esc", &g_Settings.QuickbarCloseOnEsc, /*defaultIsOn=*/false)) {
        ApplyQbCloseOnEsc();   // register / deregister Escape hook
    }
}