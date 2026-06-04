#include "Favorites.h"
#include "Globals.h"
#include "Logging.h"
#include "Settings.h"
#include "EmoteData.h"   // g_Emotes / g_EmotesMutex, for the catalog reconcile

#include <algorithm>
#include <mutex>
#include <unordered_set>

std::string TrimName(std::string s) {
    auto isws = [](char c) { return c == ' ' || c == '\t'; };
    while (!s.empty() && isws(s.front())) s.erase(s.begin());
    while (!s.empty() && isws(s.back()))  s.pop_back();
    return s;
}

int FindCategoryContaining(const std::string& id) {
    for (size_t i = 0; i < g_Settings.FavoriteCategories.size(); ++i) {
        const auto& cat = g_Settings.FavoriteCategories[i];
        if (std::find(cat.Emotes.begin(), cat.Emotes.end(), id) != cat.Emotes.end())
            return (int)i;
    }
    return -1;
}

bool IsFavorited(const std::string& id) {
    return FindCategoryContaining(id) >= 0;
}

void AddEmoteToCategory(int catIdx, const std::string& id, bool isLockedSource) {
    if (catIdx < 0 || catIdx >= (int)g_Settings.FavoriteCategories.size()) return;
    int curr = FindCategoryContaining(id);
    if (curr == catIdx) return;
    if (isLockedSource) {
        LOG_DEBUG("Refused to favorite locked emote %s", id.c_str());
        return;
    }
    if (curr >= 0) {
        auto& src = g_Settings.FavoriteCategories[curr].Emotes;
        src.erase(std::remove(src.begin(), src.end(), id), src.end());
        LOG_DEBUG("Moved %s from \"%s\" to \"%s\"", id.c_str(),
                  g_Settings.FavoriteCategories[curr].Name.c_str(),
                  g_Settings.FavoriteCategories[catIdx].Name.c_str());
    } else {
        LOG_DEBUG("Added %s to favorites category \"%s\"", id.c_str(),
                  g_Settings.FavoriteCategories[catIdx].Name.c_str());
    }
    g_Settings.FavoriteCategories[catIdx].Emotes.push_back(id);
    if (!g_SettingsPath.empty()) SaveSettings(g_SettingsPath);
}

void RemoveEmoteFromCategories(const std::string& id) {
    bool changed = false;
    for (auto& cat : g_Settings.FavoriteCategories) {
        auto before = cat.Emotes.size();
        cat.Emotes.erase(std::remove(cat.Emotes.begin(), cat.Emotes.end(), id),
                         cat.Emotes.end());
        if (cat.Emotes.size() != before) changed = true;
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
    int  count      = (int)cats[idx].Emotes.size();
    int  prevActive = g_Settings.QuickbarCategoryIdx;
    cats.erase(cats.begin() + idx);
    // Keep the Quickbar's active index valid + pointing at the same category
    // where possible (mirrors the old SectionHeader / Options delete fixup).
    int& active = g_Settings.QuickbarCategoryIdx;
    int  newSz  = (int)cats.size();
    if      (active > idx)  active--;
    else if (active == idx) active = (active < newSz) ? active : newSz - 1;
    if (active < 0) active = 0;
    LOG_DEBUG("favorites: deleted category \"%s\" (%d emote(s))", name.c_str(), count);
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
    std::unordered_set<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        if (g_Emotes.empty()) return;  // empty catalog: nothing to validate against
        ids.reserve(g_Emotes.size());
        for (const auto& e : g_Emotes) ids.insert(e.Id);
    }

    int stale = 0;
    for (const auto& cat : g_Settings.FavoriteCategories) {
        for (const auto& id : cat.Emotes) {
            if (ids.find(id) == ids.end()) {
                LOG_WARNING("favorites: category \"%s\" references unknown emote id "
                            "'%s' (kept; re-seeding the catalog restores it)",
                            cat.Name.c_str(), id.c_str());
                ++stale;
            }
        }
    }
    for (const auto& id : g_Settings.ManuallyUnlocked) {
        if (ids.find(id) == ids.end()) {
            LOG_WARNING("unlocks: unknown emote id '%s' (kept)", id.c_str());
            ++stale;
        }
    }
    if (stale > 0)
        LOG_INFO("Catalog reconcile: %d stale id(s) logged, none removed", stale);
}

