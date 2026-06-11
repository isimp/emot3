#pragma once

// Nexus quick-access shortcut — the small icon row at the top of the
// screen. emot3 adds a single shortcut there that:
//
//   - Left-click  → toggles the main window. The General Options
//                   tab has a checkbox to swap that to the Quickbar
//                   for users who primarily live in the HUD.
//   - Right-click → opens a Nexus-style context menu with entries to
//                   show / hide either window plus a (disabled)
//                   "Settings..." pointer aimed at the addon's full
//                   Options panel (which Nexus has no API to open
//                   programmatically).
//
// Hover effect: Nexus' QuickAccess.Add wires hover by blending the
// "active" texture into the "hover" texture. We ship two PNGs —
// resources/ui/icon.png (active) and resources/ui/icon_hover.png
// (hover) — bundled and loaded through the same disk-first /
// bundle-fallback path as the rest of the UI artwork. To change how
// hover looks, edit icon_hover.png and rebuild; LoadUiIconOverrides
// picks it up automatically via the gen_rc.py manifest.
//
// Aside on the API: Nexus' QuickAccessVT struct only exposes Add
// (dual-texture), Remove, Notify, AddContextMenu and
// RemoveContextMenu — there's no AddSimple custom-render path even
// though QUICKACCESS_ADDSIMPLE is declared as a typedef in Nexus.h.
// That's why the hover effect has to come from a real second texture
// rather than from our own draw callback.

// (Re)apply the shortcut based on current settings. Safe to call any
// number of times: cleans up the previous registration before adding
// a new one, and is a no-op when ShowNexusShortcut is off.
//
// Called once from AddonLoad after the textures are primed, and again
// whenever the user flips ShowNexusShortcut or changes ShortcutClickAction
// in the General Options tab.
void ApplyNexusShortcut();

// Cleanup hook for AddonUnload. Removes the shortcut and the context
// menu callback so Nexus stops calling into our (about-to-be-unloaded)
// code.
void RemoveNexusShortcut();
