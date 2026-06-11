#include "Options.h"
#include "OptionsCommon.h"
#include "Globals.h"
#include "I18n.h"
#include "Settings.h"
#include "SaveScheduler.h"   // RequestSave (debounced, off-thread settings writes)
#include "Layout.h"          // ToggleButton / TooltipText
#include "Palette.h"         // Toggle/IsPaletteOpen + the preview pulses
#include "Logging.h"         // LOG_TRACE (setting-change audit trail)
#include "Profiling.h"       // PROFILE_SCOPE macro (no-op without EMOT3_DEVTOOLS)

#include "imgui/imgui.h"

// ---- Tab: Palette -------------------------------------------------------
// The emote palette's settings - its own tab (it started as a General
// section and outgrew it). The whole tab doubles as a live preview surface:
// engagement drives the palette's ghost / keep-alive pulses, so the geometry
// sliders show their effect without juggling focus (interacting with this
// window is otherwise exactly the focus loss that auto-closes the palette -
// see Palette.h).

void RenderPaletteOptionsTab() {
    PROFILE_SCOPE("opt.palette");  // dev perf overlay

    // While this tab is visible, an open palette NEVER auto-closes - the tab
    // is the management/preview surface, so the keep-alive pulses
    // unconditionally every frame the tab renders. (It originally pulsed on
    // the group's IsItemHovered, but ImGui reports hover as FALSE while any
    // item is ACTIVE - so the pulse died mid-click on the very toggle button
    // that closes the palette, the keep-alive expired during the press, and
    // the focus-close raced the click: visible flicker. Closing from here is
    // the button / Esc / keybind, never a click-away.)
    PaletteKeepAlivePulse();

    bool palGeo = false;  // a geometry control (rows / size / position) is engaged

    // Primary visibility toggle at the top - same affordance + styling as
    // the General tab's Library toggle and the Quickbar tab's main toggle.
    // The palette's open state is transient (nothing persisted), so the
    // button drives Toggle/IsPaletteOpen instead of a settings bool.
    ImGui::Spacing();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.f, 6.f));
    bool palOpen = IsPaletteOpen();
    const char* toggleLabel = palOpen ? L("shortcut.pal_close")
                                      : L("shortcut.pal_open");
    if (ToggleButton(toggleLabel, &palOpen)) TogglePalette();
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered()) TooltipText("opt.pal.toggle_tooltip");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", IsPaletteOpen() ? L("opt.currently_visible")
                                              : L("opt.currently_hidden"));
    ImGui::Spacing();

    // Discoverability: the palette's everyday entry points are its Nexus
    // keybind (ships unassigned) and the quick-access icon.
    ImGui::TextDisabled("%s", L("opt.pal.bind_hint"));
    ImGui::Spacing();

    // Result-row cap. The palette never scrolls - more rows = taller window.
    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderInt(L("opt.pal.max_results"), &g_Settings.PaletteMaxResults, 5, 15);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        LOG_TRACE("setting palette.max_results = %d", g_Settings.PaletteMaxResults);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered()) TooltipText("opt.pal.max_results_tip");
    palGeo |= ImGui::IsItemHovered() || ImGui::IsItemActive();

    // Size factor (window width + row height; same slider idiom as the icon
    // scales: save on release, right-click resets).
    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderFloat(L("opt.pal.scale"), &g_Settings.PaletteScale, 0.8f, 1.5f, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        LOG_TRACE("setting palette.scale = %.2f", g_Settings.PaletteScale);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        g_Settings.PaletteScale = 1.0f;
        LOG_TRACE("setting palette.scale = %.2f (reset)", g_Settings.PaletteScale);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered()) TooltipText("opt.pal.scale_tip");
    palGeo |= ImGui::IsItemHovered() || ImGui::IsItemActive();

    // Horizontal anchor (fraction of screen width; 0.50 = centered). Like the
    // vertical slider below, this moves an open palette live (the window
    // positions itself every frame).
    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderFloat(L("opt.pal.x_pos"), &g_Settings.PaletteXPos, 0.1f, 0.9f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        LOG_TRACE("setting palette.x_pos = %.2f", g_Settings.PaletteXPos);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        g_Settings.PaletteXPos = 0.5f;
        LOG_TRACE("setting palette.x_pos = %.2f (reset)", g_Settings.PaletteXPos);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered()) TooltipText("opt.pal.x_pos_tip");
    palGeo |= ImGui::IsItemHovered() || ImGui::IsItemActive();

    // Vertical anchor (fraction of screen height).
    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderFloat(L("opt.pal.y_pos"), &g_Settings.PaletteYPos, 0.f, 0.8f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        LOG_TRACE("setting palette.y_pos = %.2f", g_Settings.PaletteYPos);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        g_Settings.PaletteYPos = 0.2f;
        LOG_TRACE("setting palette.y_pos = %.2f (reset)", g_Settings.PaletteYPos);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered()) TooltipText("opt.pal.y_pos_tip");
    palGeo |= ImGui::IsItemHovered() || ImGui::IsItemActive();

    // Empty-query suggestions: Frequently / Recently used (the usage log's two
    // views, same labels as the Quickbar categories) or nothing.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(L("opt.pal.empty_query"));
    ImGui::SameLine();
    int pq = (int)g_Settings.PaletteEmptyQuery;
    const char* pqItems[] = { L("qb.cat_frequent"), L("qb.cat_recently_used"),
                              L("tt.off") };
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::Combo("##palempty", &pq, pqItems, IM_ARRAYSIZE(pqItems))) {
        g_Settings.PaletteEmptyQuery = (EPaletteEmptyQuery)pq;
        LOG_TRACE("setting palette.empty_query = %d", pq);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered()) {
        static const TooltipOption kPalEmptyOpts[] = {
            { "qb.cat_frequent",      "opt.pal.empty_frequent.desc", true  },
            { "qb.cat_recently_used", "opt.pal.empty_recent.desc",   false },
            { "tt.off",               "opt.pal.empty_off.desc",      false },
        };
        TooltipOptions("opt.pal.empty_query_tip", kPalEmptyOpts,
                       IM_ARRAYSIZE(kPalEmptyOpts));
    }

    CheckboxWithSaveAndTooltip("opt.pal.clear_on_open", &g_Settings.PaletteClearOnOpen,
                               /*defaultIsOn=*/true);

    // Geometry engagement -> ghost preview (see Palette.h). IsItemActive
    // keeps a slider drag counted even when the mouse leaves the tab rect.
    if (palGeo) PaletteGhostPulse();
}
