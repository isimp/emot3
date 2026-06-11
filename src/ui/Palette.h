#pragma once

// Emote palette: a keybound, spotlight-style popup - type to filter the
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

// True while the palette window is open. Other surfaces step back from it:
// the Quickbar feeds this into its unusable presentation (grey / hide /
// untouched per the user's setting), and the Nexus shortcut menu flips its
// open/close label on it.
bool IsPaletteOpen();

// Options-preview hooks, called per frame by OptionsGeneral's palette section
// (interacting with those settings necessarily un-focuses the palette, which
// would otherwise auto-close it - "hard to preview"):
//  - GhostPulse: while a geometry control (rows / size / position) is hovered
//    or dragged, a CLOSED palette renders as a non-interactive ghost at the
//    configured spot, so the sliders preview live without opening it.
//  - KeepAlivePulse: while the section is engaged, an OPEN palette's
//    focus-loss / click-away auto-close stands down, so tuning it live works.
void PaletteGhostPulse();
void PaletteKeepAlivePulse();

// WndProc seam (entry.cpp): observe-only mouse-down hook for click-away
// closing. Clicks on the game world never reach ImGui's focus bookkeeping
// under Nexus, so the palette hit-tests them itself against its last
// published window rect. xClient/yClient are client-space coords from the
// message lParam. Safe to call from the window thread (atomics inside);
// cheap no-op while the palette is closed.
void PaletteNoteMouseDown(int xClient, int yClient);

// Reset transient state (open flag + query). Called from AddonUnload so a
// Nexus reload doesn't resurrect a stale palette (load-time state must be
// re-initialisable - see the registry-reset note in entry.cpp).
void ResetPalette();
