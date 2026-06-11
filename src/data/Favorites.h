#pragma once

// Favorites + categories. Pure data manipulation against
// g_Settings.FavoriteCategories — no rendering. (Name trimming now lives in
// the shared core/StringUtil.h TrimWhitespace.)

#include <string>
#include "Settings.h"   // EFavoriteRefType for the type-tagged ref API

// Generic, type-tagged API. Favorites mix Emote-typed refs and /me-mote-typed
// refs in the same FavoriteCategory.Refs vector; every lookup keys on the
// (type, id) pair so a /me-mote with id "wave" and an Emote with id "wave"
// are distinct entries.
int  FindCategoryContaining(EFavoriteRefType type, const std::string& id);
bool IsFavorited           (EFavoriteRefType type, const std::string& id);
// Move (type, id) into category catIdx. If already present in another
// category, it's removed there first (each ref lives in exactly one
// category). isLockedSource is the defensive locked-emote guard from the
// click site (Emote-only concept; ignored for /me-mote refs).
void AddRefToCategory      (int catIdx, EFavoriteRefType type, const std::string& id,
                            bool isLockedSource = false);
void RemoveRefFromCategories(EFavoriteRefType type, const std::string& id);

// Emote convenience wrappers — preserve the historical call surface so
// existing Emote-only call sites stay unchanged. Each routes to the
// type-tagged form above with EFavoriteRefType::Emote.
inline int  FindCategoryContaining(const std::string& emoteId) {
    return FindCategoryContaining(EFavoriteRefType::Emote, emoteId);
}
inline bool IsFavorited(const std::string& emoteId) {
    return IsFavorited(EFavoriteRefType::Emote, emoteId);
}
inline void AddEmoteToCategory(int catIdx, const std::string& emoteId,
                               bool isLockedSource = false) {
    AddRefToCategory(catIdx, EFavoriteRefType::Emote, emoteId, isLockedSource);
}
inline void RemoveEmoteFromCategories(const std::string& emoteId) {
    RemoveRefFromCategories(EFavoriteRefType::Emote, emoteId);
}

// Returns true if any (other) category has the given display name.
// excludeIdx lets the rename UI ignore the row it's editing.
bool CategoryNameExists(const std::string& name, int excludeIdx = -1);

// Delete favorites category `idx`, fixing up g_Settings.QuickbarCategoryIdx so
// the active Quickbar category still points at the same (or a valid) entry, and
// saving. No-op for an out-of-range index. One source of truth for the
// index-adjust dance the Library header delete + Options used to each re-spell.
void DeleteFavoriteCategory(int idx);

// Move favorites category `from` to final index `to` (arbitrary distance), with
// the same QuickbarCategoryIdx remap + save. Backs the Library's drag-reorder;
// a swap-by-one is just MoveFavoriteCategory(i, i+/-1). No-op when either index
// is out of range or from == to.
void MoveFavoriteCategory(int from, int to);

// Make sure FavoriteCategories has at least one entry. Called on startup
// (no UI surface for "delete the last category" — but if a saved file
// happens to be empty we want a place to drop new favorites).
void EnsureDefaultCategory();

// Log (do NOT remove) any favorite / manually-unlocked id that isn't in the
// current emote catalog. Call at AddonLoad AFTER the catalog is loaded. Stale
// ids are kept deliberately: the catalog can be temporarily empty (user cleared
// it, or a failed emotes.json) and a later re-seed restores the referenced
// emotes — dropping them here would lose favorites permanently. Skips entirely
// when the catalog is empty (nothing meaningful to validate against).
void ReconcileFavoritesWithCatalog();

