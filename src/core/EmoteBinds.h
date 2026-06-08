#pragma once

// Per-emote / per-/me-mote Nexus InputBinds.
//
// An opt-in "Keybind" toggle in the catalog editors registers a Nexus InputBind
// for that entry, so the user can bind a key (in Nexus' keybind UI) to fire the
// emote directly — and so it's eligible to drive a RadialMenus wheel item
// (a radial item Invokes a Nexus InputBind by name; see the v1.3 plan / P3).
//
// The bind identifier is the contract shared with the RadialMenus pack writer,
// so it lives in one helper. Stable (keyed on the immutable Id, not the editable
// Name) and catalog-namespaced (an emote and a /me-mote with the same Id get
// distinct binds). For /me-motes the variant is part of the identifier — the
// radial can only reference a bind by name, so which body it sends has to be in
// the name.
//
// Everything's gated through SyncEmoteBinds(): it diffs the desired set against
// what's currently registered and registers/deregisters the delta, so it's safe
// to call any time the opt-in set changes (toggle, variant change, catalog edit)
// and once at AddonLoad. AddonUnload calls DeregisterAllEmoteBinds().

#include <string>

#include "Settings.h"   // EFavoriteRefType
#include "MeMotes.h"     // EMeMoteVariant

// "emot3 Emote: <id>"  /  "emot3 Me-mote: <id> [default|you|all]"
std::string EmoteBindIdentifier(EFavoriteRefType type, const std::string& id,
                                EMeMoteVariant variant = EMeMoteVariant::Default);

// Reconcile the registered Nexus InputBinds with the catalogs' opt-in flags
// (Emote.UserKeybind / MeMote.UserKeybind + KeybindVariant). Diff-based:
// registers newly-opted-in binds, deregisters removed ones, leaves the rest
// (so a user's key assignment survives). Render-thread; AddonLoad + every
// opt-in/variant change call it. (P3 will also union in RadialMenus wheel refs.)
void SyncEmoteBinds();

// Deregister every emote InputBind we registered (AddonUnload; also clears the
// tracking set so an AddonLoad re-sync starts clean on a Nexus reload).
void DeregisterAllEmoteBinds();
