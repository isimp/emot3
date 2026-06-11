#pragma once

// Quick-send palette: a keybound, spotlight-style popup - type to filter the
// catalog (same predicate as the Library search, data/SearchMatch.h), Enter
// sends the selected entry, Esc closes. With an empty query it lists the
// frequently-used emotes (usage::Frequent), so it's useful before you type.
//
// Deliberately settings-free: the feature is opt-in by nature (the keybind
// ships unassigned; it surfaces in Nexus' per-addon keybind list like Toggle
// Library / Toggle Quickbar). Sends route through SendOrFill* unchanged, so
// every gate (competitive, game state, moving, textbox) applies - on a
// refusal the palette stays open and shows the reason in-window; on success
// it closes and the send feeds Recently/Frequently used at the choke point.

// The exact window-name string passed to ImGui::Begin - entry.cpp registers
// it with Nexus' CloseOnEscape (the strings must match across the API
// boundary, same contract as the Library / Quickbar names).
extern const char* const PALETTE_WND_NAME;

// ERenderType_Render callback (registered in entry.cpp after QuickbarRender).
// No-op while closed.
void PaletteRender();

// Keybind action: open (focusing the search field) or close.
void TogglePalette();

// The open flag, for entry.cpp's RegisterCloseOnEscape wiring.
bool* PaletteOpenFlag();

// Reset transient state (open flag + query). Called from AddonUnload so a
// Nexus reload doesn't resurrect a stale palette (load-time state must be
// re-initialisable - see the registry-reset note in entry.cpp).
void ResetPalette();
