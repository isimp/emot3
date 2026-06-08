#include "Favorites.h"
#include "Globals.h"
#include "Logging.h"
#include "Settings.h"
#include "EmoteData.h"   // g_Emotes / g_EmotesMutex, for the catalog reconcile
#include "MeMotes.h"     // g_MeMotes / g_MeMotesMutex, /me-mote reconcile half
#include "StringUtil.h"  // SanitizeFilename + MakeUniqueSlug (CategoryFolderNames)

#include <algorithm>
#include <mutex>
#include <unordered_set>

namespace {
// One-liner: does this ref match (type, id)? Used by every helper below;
// keeps the comparison consistent and the std::remove_if lambdas terse.
inline bool RefEquals(const FavoriteRef& r, EFavoriteRefType type,
                      const std::string& id) {
    return r.Type == type && r.Id == id;
}
// Human-readable type tag for log messages — "emote" or "me-mote".
inline const char* TypeTag(EFavoriteRefType t) {
    return t == EFavoriteRefType::Emote ? "emote" : "me-mote";
}
} // namespace

int FindCategoryContaining(EFavoriteRefType type, const std::string& id) {
    for (size_t i = 0; i < g_Settings.FavoriteCategories.size(); ++i) {
        const auto& cat = g_Settings.FavoriteCategories[i];
        for (const auto& r : cat.Refs)
            if (RefEquals(r, type, id)) return (int)i;
    }
    return -1;
}

bool IsFavorited(EFavoriteRefType type, const std::string& id) {
    return FindCategoryContaining(type, id) >= 0;
}

void AddRefToCategory(int catIdx, EFavoriteRefType type, const std::string& id,
                      bool isLockedSource) {
    if (catIdx < 0 || catIdx >= (int)g_Settings.FavoriteCategories.size()) return;
    int curr = FindCategoryContaining(type, id);
    if (curr == catIdx) return;
    if (isLockedSource) {
        LOG_DEBUG("Refused to favorite locked emote %s", id.c_str());
        return;
    }
    if (curr >= 0) {
        auto& src = g_Settings.FavoriteCategories[curr].Refs;
        src.erase(std::remove_if(src.begin(), src.end(),
                                 [&](const FavoriteRef& r) { return RefEquals(r, type, id); }),
                  src.end());
        LOG_DEBUG("Moved %s %s from \"%s\" to \"%s\"", TypeTag(type), id.c_str(),
                  g_Settings.FavoriteCategories[curr].Name.c_str(),
                  g_Settings.FavoriteCategories[catIdx].Name.c_str());
    } else {
        LOG_DEBUG("Added %s %s to favorites category \"%s\"",
                  TypeTag(type), id.c_str(),
                  g_Settings.FavoriteCategories[catIdx].Name.c_str());
    }
    g_Settings.FavoriteCategories[catIdx].Refs.push_back(FavoriteRef{ type, id });
    if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
}

void RemoveRefFromCategories(EFavoriteRefType type, const std::string& id) {
    bool changed = false;
    for (auto& cat : g_Settings.FavoriteCategories) {
        auto before = cat.Refs.size();
        cat.Refs.erase(std::remove_if(cat.Refs.begin(), cat.Refs.end(),
                                      [&](const FavoriteRef& r) {
                                          return RefEquals(r, type, id);
                                      }),
                       cat.Refs.end());
        if (cat.Refs.size() != before) changed = true;
    }
    if (changed && !g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
}

bool CategoryNameExists(const std::string& name, int excludeIdx) {
    for (size_t i = 0; i < g_Settings.FavoriteCategories.size(); ++i) {
        if ((int)i == excludeIdx) continue;
        if (g_Settings.FavoriteCategories[i].Name == name) return true;
    }
    return false;
}

void DeleteFavoriteCategory(int idx) {
    auto& cats = g_Settings.FavoriteCategories;
    if (idx < 0 || idx >= (int)cats.size()) return;
    std::string name = cats[idx].Name;
    int  count      = (int)cats[idx].Refs.size();
    int  prevActive = g_Settings.QuickbarCategoryIdx;
    cats.erase(cats.begin() + idx);
    // Keep the Quickbar's active index valid + pointing at the same category
    // where possible (mirrors the old SectionHeader / Options delete fixup).
    int& active = g_Settings.QuickbarCategoryIdx;
    int  newSz  = (int)cats.size();
    if      (active > idx)  active--;
    else if (active == idx) active = (active < newSz) ? active : newSz - 1;
    if (active < 0) active = 0;
    LOG_DEBUG("favorites: deleted category \"%s\" (%d ref(s))", name.c_str(), count);
    if (active != prevActive)
        LOG_DEBUG("favorites: quickbar active category index %d -> %d (after delete)",
                  prevActive, active);
    if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
}

void MoveFavoriteCategory(int from, int to) {
    auto& cats = g_Settings.FavoriteCategories;
    int n = (int)cats.size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    std::string name = cats[from].Name;
    int prevActive   = g_Settings.QuickbarCategoryIdx;
    FavoriteCategory moved = std::move(cats[from]);
    cats.erase(cats.begin() + from);
    cats.insert(cats.begin() + to, std::move(moved));
    // Remap the active Quickbar index so it tracks its category through the
    // move. The moved item itself maps from->to; an index between the two
    // shifts by one in the direction that closes the gap.
    int& a = g_Settings.QuickbarCategoryIdx;
    if      (a == from)               a = to;
    else if (from < a && a <= to)     a -= 1;   // downward move passed under it
    else if (to   <= a && a < from)   a += 1;   // upward move pushed it down
    if (a < 0) a = 0; else if (a >= n) a = n - 1;
    LOG_DEBUG("favorites: moved category \"%s\" %d -> %d", name.c_str(), from, to);
    if (a != prevActive)
        LOG_DEBUG("favorites: quickbar active category index %d -> %d (after move)",
                  prevActive, a);
    if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
}

void EnsureDefaultCategory() {
    if (g_Settings.FavoriteCategories.empty()) {
        FavoriteCategory c;
        c.Name = "Favorites";
        g_Settings.FavoriteCategories.push_back(std::move(c));
    }
}

void ReconcileFavoritesWithCatalog() {
    // Snapshot live Ids by catalog. Skip the per-catalog half when its catalog
    // is empty (load failure / fresh install) — stale ids are kept regardless,
    // but logging them as "unknown" would be noise when the catalog itself is
    // missing.
    std::unordered_set<std::string> emoteIds, meMoteIds;
    bool haveEmotes = false, haveMeMotes = false;
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        if (!g_Emotes.empty()) {
            haveEmotes = true;
            emoteIds.reserve(g_Emotes.size());
            for (const auto& e : g_Emotes) emoteIds.insert(e.Id);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        if (!g_MeMotes.empty()) {
            haveMeMotes = true;
            meMoteIds.reserve(g_MeMotes.size());
            for (const auto& m : g_MeMotes) meMoteIds.insert(m.Id);
        }
    }
    if (!haveEmotes && !haveMeMotes) return;

    int stale = 0;
    for (const auto& cat : g_Settings.FavoriteCategories) {
        for (const auto& r : cat.Refs) {
            const bool isEmote = r.Type == EFavoriteRefType::Emote;
            // Skip the per-type check when the corresponding catalog is empty
            // (no Ids to validate against).
            if (isEmote   && !haveEmotes)   continue;
            if (!isEmote  && !haveMeMotes)  continue;
            const auto& set = isEmote ? emoteIds : meMoteIds;
            if (set.find(r.Id) == set.end()) {
                LOG_WARNING("favorites: category \"%s\" references unknown %s id "
                            "'%s' (kept; re-seeding the catalog restores it)",
                            cat.Name.c_str(), TypeTag(r.Type), r.Id.c_str());
                ++stale;
            }
        }
    }
    if (haveEmotes) {
        for (const auto& id : g_Settings.ManuallyUnlocked) {
            if (emoteIds.find(id) == emoteIds.end()) {
                LOG_WARNING("unlocks: unknown emote id '%s' (kept)", id.c_str());
                ++stale;
            }
        }
    }
    if (stale > 0)
        LOG_INFO("Catalog reconcile: %d stale id(s) logged, none removed", stale);
}

// See Favorites.h. Batch (single used-set pass) so two categories that slug to
// the same stem get distinct folders. No caller yet - scaffolding for the
// upcoming per-category folder feature.
std::vector<std::string> CategoryFolderNames() {
    std::vector<std::string> out;
    std::unordered_set<std::string> used;
    out.reserve(g_Settings.FavoriteCategories.size());
    for (const auto& cat : g_Settings.FavoriteCategories) {
        std::string folder = MakeUniqueSlug(
            SanitizeFilename(cat.Name, "category"),
            [&](const std::string& stem) { return used.count(stem) != 0; });
        used.insert(folder);
        out.push_back(folder);
    }
    return out;
}

