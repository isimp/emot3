#pragma once

// Quick-send palette: a keybound, spotlight-style popup - type to filter the
// catalog (same predicate as the Library search, data/SearchMatch.h), Enter
// sends the selected entry. Esc closes in one press (handled internally - the
// Nexus CloseOnEscape hook only fired after the query field ate one Esc to
// deactivate-and-revert), and losing window focus in any way closes it too.
// With an empty query it lists the frequently-used emotes (usage::Frequent),
// so it's useful before you type.
//
// Deliberately settings-free: the feature is opt-in by nature (the keybind
// ships unassigned; it surfaces in Nexus' per-addon keybind list like Toggle
// Library / Toggle Quickbar). Sends route through SendOrFill* unchanged, so
// every gate (competitive, game state, moving, textbox) applies - on a
// refusal the palette stays open and shows the reason in-window; on success
// it closes and the send feeds Recently/Frequently used at the choke point.

// The exact window-name string passed to ImGui::Begin.
extern const char* const PALETTE_WND_NAME;

// ERenderType_Render callback (registered in entry.cpp after QuickbarRender).
// No-op while closed.
void PaletteRender();

// Keybind action: open (focusing the search field) or close.
void TogglePalette();

// Reset transient state (open flag + query). Called from AddonUnload so a
// Nexus reload doesn't resurrect a stale palette (load-time state must be
// re-initialisable - see the registry-reset note in entry.cpp).
void ResetPalette();
