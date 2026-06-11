#include "NexusShortcut.h"
#include "Globals.h"
#include "I18n.h"
#include "Keybinds.h"
#include "Logging.h"
#include "Settings.h"
#include "SaveScheduler.h" // RequestSave (debounced settings persist)
#include "Palette.h"        // TogglePalette / IsPaletteOpen (quick-send menu item)
#include "QuickbarPresets.h"
#include "Options.h"        // ApplyQbCloseOnEsc (preset apply re-registers it)
#include "UpdateCheck.h"    // Plus update hint (PlusUpdateAvailable / OpenReleasesPage; stubs otherwise)

#include "imgui/imgui.h"

#include <cstdio>
#include <string>

namespace {

// Identifiers passed to Nexus' QuickAccess.* API.
constexpr const char* kShortcutId    = "emot3_shortcut";
constexpr const char* kShortcutCtxId = "emot3_shortcut_ctx";

// Nexus-internal inputbind that opens the main window on the Addons tab.
// Not part of the documented addon API - invoked by string id (Nexus dispatches
// any registered bind by identifier). Kept file-local; it's not one of OUR binds
// (those live in Keybinds.h).
constexpr const char* kNexusKbAddons = "KB_ADDONS";

// Texture identifiers — match the cache keys LoadUiIconOverrides
// populates (see MainPanel.cpp). Two distinct textures so Nexus has
// something to blend toward on hover; without that the hover slot is
// a no-op and the icon stays static.
constexpr const char* kIconTex      = "EMOT3_UI_SHORTCUT";        // active / resting state
constexpr const char* kIconHoverTex = "EMOT3_UI_SHORTCUT_HOVER";  // hovered state

// Tracks whether we've called QuickAccess.Add since the last Remove, so
// repeated ApplyNexusShortcut() calls don't try to Remove an entry that
// doesn't exist (Nexus is forgiving about it, but cleaner is better).
bool s_registered = false;

// Right-click context menu rendered when the user opens the shortcut's
// popup. Three items: toggle main, toggle Quickbar, and an inactive
// "Settings..." pointer aimed at Nexus' addon Options panel (which has
// no programmatic open API).
void ShortcutContextMenu() {
    // Plus-only nudge: when a newer release exists, a green attention item that
    // copies the GitHub releases link to the clipboard (the Plus build doesn't
    // auto-update; we copy rather than ShellExecute a browser, which would alt-tab
    // / minimize a fullscreen client). The shortcut icon is already badged via
    // QuickAccess.Notify. PlusUpdateAvailable() is a false stub in non-Plus builds,
    // so this compiles away there.
    if (PlusUpdateAvailable()) {
        char label[128];
        std::snprintf(label, sizeof(label), L("shortcut.update_available"),
                      PlusLatestVersion().c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.80f, 0.45f, 1.f));
        bool go = ImGui::MenuItem(label);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            TooltipText("shortcut.update_copy_tooltip");
        if (go) ImGui::SetClipboardText(ReleasesUrl());
        ImGui::Separator();
    }

    const char* mainLabel = g_Settings.ShowWindow
        ? L("opt.gen.hide_main")
        : L("opt.gen.show_main");
    const char* qbLabel = g_Settings.ShowQuickbar
        ? L("opt.qb.hide")
        : L("opt.qb.show");

    if (ImGui::MenuItem(mainLabel)) {
        g_Settings.ShowWindow = !g_Settings.ShowWindow;
        LOG_DEBUG("Shortcut menu > Library %s",
                  g_Settings.ShowWindow ? "shown" : "hidden");
        // Visibility is ride-along nav state — persisted by the unload flush, like
        // the Esc-close hook (which flips the bool with no save). No eager write.
    }
    if (ImGui::MenuItem(qbLabel)) {
        g_Settings.ShowQuickbar = !g_Settings.ShowQuickbar;
        LOG_DEBUG("Shortcut menu > Quickbar %s",
                  g_Settings.ShowQuickbar ? "shown" : "hidden");
        // Ride-along nav state (see Library above) — no eager write.
    }
    // Quick-send palette — same open/close label flip as the two windows
    // above, but a transient popup: nothing persisted. The selecting click
    // can't immediately click-away-close it (the palette's hit rect is only
    // armed after its first rendered frame).
    const char* palLabel = IsPaletteOpen()
        ? L("shortcut.pal_close")
        : L("shortcut.pal_open");
    if (ImGui::MenuItem(palLabel)) {
        TogglePalette();
        LOG_DEBUG("Shortcut menu > quick send %s",
                  IsPaletteOpen() ? "opened" : "closed");
    }

    // Quickbar presets submenu — apply one by name. The active preset (the one
    // whose settings + geometry currently match) gets a check mark. Applying
    // mirrors the Options "Apply" button: copy into g_Settings, re-register the
    // side-effectful close-on-Esc hook, queue the window-geometry one-shot, and
    // persist. Visibility is intentionally left untouched (presets don't carry
    // it), matching the Options behaviour.
    if (ImGui::BeginMenu(L("shortcut.presets"))) {
        if (g_QuickbarPresets.empty()) {
            ImGui::MenuItem(L("opt.qb.preset.none"), nullptr, false, /*enabled=*/false);
        } else {
            for (size_t i = 0; i < g_QuickbarPresets.size(); ++i) {
                const QuickbarPreset& p = g_QuickbarPresets[i];
                bool active = QuickbarPresetMatchesCurrent(p);
                ImGui::PushID((int)i);  // names are unique, but guard id collisions
                if (ImGui::MenuItem(p.Name.c_str(), nullptr, active)) {
                    ApplyQuickbarPreset(p);
                    ApplyQbCloseOnEsc();
                    LOG_DEBUG("Shortcut menu > applied preset '%s'", p.Name.c_str());
                    RequestSave(SaveKind::Settings);  // deliberate change — debounced persist
                }
                ImGui::PopID();
            }
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();

    // "Settings..." keeps its muted placeholder look (disabled text colour) but
    // is now actually clickable - a quiet surprise that it jumps to the Nexus
    // addon menu where emot3's Options live. Fire-and-forget: Invoke opens the
    // Addons tab via Nexus' internal KB_ADDONS bind, or silently no-ops if a
    // future Nexus build lacks it (it looks the id up and returns with no effect
    // - no crash). Nexus exposes no way to pre-select emot3's row, so this lands
    // on the Addons list; the tooltip names the Nexus > Addons > emot3 path. No
    // checks/notices by design.
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    bool openNexus = ImGui::MenuItem(L("shortcut.settings"));
    ImGui::PopStyleColor();
    if (openNexus)
        APIDefs->InputBinds.Invoke(kNexusKbAddons, false);
    if (ImGui::IsItemHovered())
        TooltipText("shortcut.settings_tooltip");
}

} // namespace

void ApplyNexusShortcut() {
    if (!APIDefs) return;

    // Always clean up first so the function is idempotent.
    if (s_registered) {
        APIDefs->QuickAccess.RemoveContextMenu(kShortcutCtxId);
        APIDefs->QuickAccess.Remove(kShortcutId);
        s_registered = false;
    }

    if (!g_Settings.ShowNexusShortcut) {
        LOG_DEBUG("Nexus shortcut: disabled");
        return;
    }

    // Left-click triggers a keybind — pick which one based on the swap
    // setting. Right-click is handled by AddContextMenu below.
    const char* leftKb = g_Settings.SwapShortcutClickActions
                             ? KB_TOGGLE_QB
                             : KB_TOGGLE_MAIN;
    const char* tooltip = "emot3 - clickable emote panel";

    APIDefs->QuickAccess.Add(kShortcutId,
                             kIconTex,
                             kIconHoverTex,
                             leftKb,
                             tooltip);
    APIDefs->QuickAccess.AddContextMenu(kShortcutCtxId,
                                         kShortcutId,
                                         ShortcutContextMenu);
    s_registered = true;
    LOG_DEBUG("Nexus shortcut: registered (left-click > %s)", leftKb);
}

void RemoveNexusShortcut() {
    if (!APIDefs || !s_registered) return;
    APIDefs->QuickAccess.RemoveContextMenu(kShortcutCtxId);
    APIDefs->QuickAccess.Remove(kShortcutId);
    s_registered = false;
}
