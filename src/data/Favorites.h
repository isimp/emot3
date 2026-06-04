#pragma once

// Favorites + categories + the small string-trim helper shared between
// the main panel category bar and the Options favorites editor. Pure data
// manipulation against g_Settings.FavoriteCategories — no rendering.

#include <string>

// Trim leading/trailing spaces and tabs from a category/emote name.
std::string TrimName(std::string s);

// Favorites store stable emote ids (not commands) - callers pass e.Id.
// Returns the index of the first category containing `id`, or -1.
int  FindCategoryContaining(const std::string& id);
bool IsFavorited(const std::string& id);

// Move `id` into category `catIdx`. If already present in another
// category, it's removed there first (each emote lives in one place).
//   isLockedSource — defensive guard captured at the click site; locked
//                    emotes refuse to be added even if a UI path slipped
//                    through.
void AddEmoteToCategory(int catIdx, const std::string& id,
                        bool isLockedSource = false);

// Remove `id` from every favorites category that contains it.
void RemoveEmoteFromCategories(const std::string& id);

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

