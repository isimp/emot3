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

    bool palGeo = false;  // a geometry control (rows / size / position) is engaged

    // Primary visibility toggle at the top - same affordance + styling as
    // the General tab's Library toggle and the Quickbar tab's main toggle.
    // The palette's open state is transient (nothing persisted), so the
    // button drives Toggle/IsPaletteOpen instead of a settings bool.
    // DELIBERATELY OUTSIDE the keep-alive group below: if the toggle fed the
    // pulse, a closing click would be vetoed on its mouse-DOWN (keepAlive
    // can't tell "close toggle" from "slider") and only the RELEASE would
    // close - the palette sat deactivated-but-open for the click's duration
    // (read as a flicker). Outside the group, the mouse-down closes the
    // palette instantly via the WndProc click-away and the release's
    // toggle-fire is swallowed by the same-click suppression - the exact
    // Nexus-icon flow.
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

    // Everything from here down is one group: hovering it keeps an open
    // palette alive, the geometry controls additionally show the ghost.
    // (See the toggle comment above for why the toggle is NOT part of it.)
    ImGui::BeginGroup();

    // Discoverability: the palette's everyday entry points are its Nexus
    // keybind (ships unassigned) and the quick-access icon.
    ImGui::TextDisabled("%s", L("opt.pal.bind_hint"));
    ImGui::Spacing();

    // ---- Layout: row cap, size, position, grow direction ----
    OptionsSection(L("opt.sec.layout"));

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

    // Vertical anchor (fraction of screen height). Which edge it places -
    // and so its legal range, default, and tooltip - follows the grow
    // direction below: top edge 0.0..0.8 growing down, bottom edge 0.2..1.0
    // (the mirrored range) growing up. Same calibration both ways: the
    // smallest palette at scale 1 still fits on the growing side.
    const bool palUp = g_Settings.PaletteGrowUp;
    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderFloat(L("opt.pal.y_pos"), &g_Settings.PaletteYPos,
                       palUp ? 0.2f : 0.f, palUp ? 1.f : 0.8f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        LOG_TRACE("setting palette.y_pos = %.2f", g_Settings.PaletteYPos);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        g_Settings.PaletteYPos = palUp ? 0.8f : 0.2f;
        LOG_TRACE("setting palette.y_pos = %.2f (reset)", g_Settings.PaletteYPos);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered())
        TooltipText(palUp ? "opt.pal.y_pos_tip_up" : "opt.pal.y_pos_tip");
    palGeo |= ImGui::IsItemHovered() || ImGui::IsItemActive();

    // Grow direction: top edge anchored (list grows downward, default) or
    // bottom edge anchored (grows upward - the query field stays put near
    // where the user looks). The vertical-position slider above places
    // whichever edge is anchored.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(L("opt.pal.grow"));
    ImGui::SameLine();
    int gd = g_Settings.PaletteGrowUp ? 1 : 0;
    const char* gdItems[] = { L("opt.pal.grow_down"), L("opt.pal.grow_up") };
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::Combo("##palgrow", &gd, gdItems, IM_ARRAYSIZE(gdItems))) {
        g_Settings.PaletteGrowUp = (gd == 1);
        // The anchor flips edges (top <-> bottom): MIRROR the stored value so
        // the palette stays in the same screen region - and the mirror maps
        // the two ranges onto each other exactly (0.0..0.8 <-> 0.2..1.0), so
        // the result is always in the new mode's clamps.
        g_Settings.PaletteYPos = 1.0f - g_Settings.PaletteYPos;
        LOG_TRACE("setting palette.grow_up = %d (y_pos mirrored to %.2f)",
                  gd, g_Settings.PaletteYPos);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered()) {
        static const TooltipOption kPalGrowOpts[] = {
            { "opt.pal.grow_down", "opt.pal.grow_down.desc", true  },
            { "opt.pal.grow_up",   "opt.pal.grow_up.desc",   false },
        };
        TooltipOptions("opt.pal.grow_tip", kPalGrowOpts,
                       IM_ARRAYSIZE(kPalGrowOpts));
    }
    palGeo |= ImGui::IsItemHovered() || ImGui::IsItemActive();

    // ---- Look: background, row style, footer ----
    OptionsSection(L("opt.sec.look"));

    // Background opacity (1.0 = the theme's own background).
    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderFloat(L("opt.pal.bg_alpha"), &g_Settings.PaletteBgAlpha,
                       0.2f, 1.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        LOG_TRACE("setting palette.bg_alpha = %.2f", g_Settings.PaletteBgAlpha);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        g_Settings.PaletteBgAlpha = 1.0f;
        LOG_TRACE("setting palette.bg_alpha = %.2f (reset)", g_Settings.PaletteBgAlpha);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered()) TooltipText("opt.pal.bg_alpha_tip");
    palGeo |= ImGui::IsItemHovered() || ImGui::IsItemActive();

    // Row appearance: icon column (off = compact text-only rows) and the
    // footer key-help line. Both count as geometry engagement so the ghost
    // previews them live.
    CheckboxWithSaveAndTooltip("opt.pal.show_icons", &g_Settings.PaletteShowIcons,
                               /*defaultIsOn=*/true);
    palGeo |= ImGui::IsItemHovered() || ImGui::IsItemActive();
    CheckboxWithSaveAndTooltip("opt.pal.show_footer", &g_Settings.PaletteShowFooter,
                               /*defaultIsOn=*/true);
    palGeo |= ImGui::IsItemHovered() || ImGui::IsItemActive();

    // ---- Interaction: suggestions, search and send behavior ----
    OptionsSection(L("opt.sec.interaction"));

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

    // Tab cycling is keyboard-only and self-revealing ONLY once you know it
    // exists (the palette footer mentions it, but the footer is optional) -
    // so the options tab carries the one-line explainer.
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", L("opt.pal.tab_hint"));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    CheckboxWithSaveAndTooltip("opt.pal.clear_on_open", &g_Settings.PaletteClearOnOpen,
                               /*defaultIsOn=*/true);

    // First Esc clears a non-empty search, second Esc closes.
    CheckboxWithSaveAndTooltip("opt.pal.esc_clear", &g_Settings.PaletteEscClearsFirst,
                               /*defaultIsOn=*/true);

    // What Enter / a row click does: follow the global send-on-click settings
    // (emotes and /me-motes each have one) or force auto-send / fill-chat for
    // every palette send, context-menu variants included.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(L("opt.pal.enter_mode"));
    ImGui::SameLine();
    int em = (int)g_Settings.PaletteEnterMode;
    const char* emItems[] = { L("opt.pal.enter_global"), L("opt.pal.enter_send"),
                              L("opt.pal.enter_fill") };
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::Combo("##palenter", &em, emItems, IM_ARRAYSIZE(emItems))) {
        g_Settings.PaletteEnterMode = (EPaletteEnterMode)em;
        LOG_TRACE("setting palette.enter_mode = %d", em);
        RequestSave(SaveKind::Settings);
    }
    if (ImGui::IsItemHovered()) {
        static const TooltipOption kPalEnterOpts[] = {
            { "opt.pal.enter_global", "opt.pal.enter_global.desc", true  },
            { "opt.pal.enter_send",   "opt.pal.enter_send.desc",   false },
            { "opt.pal.enter_fill",   "opt.pal.enter_fill.desc",   false },
        };
        TooltipOptions("opt.pal.enter_mode_tip", kPalEnterOpts,
                       IM_ARRAYSIZE(kPalEnterOpts));
    }

    ImGui::EndGroup();
    // Engagement -> preview pulses (see Palette.h). IsItemActive keeps a
    // slider drag counted even when the mouse leaves the tab rect; the group
    // hover keeps an OPEN palette alive while the user is anywhere in here.
    const bool tabHovered = ImGui::IsItemHovered();
    if (palGeo) PaletteGhostPulse();
    if (palGeo || tabHovered) PaletteKeepAlivePulse();
}
