#include "CatalogView.h"

#include <atomic>
#include <cstdint>
#include <mutex>

#include "Globals.h"     // g_EmoteCatalogVersion / g_MeMotesVersion / g_UiViewRevision
#include "Settings.h"    // g_Settings (ManuallyUnlocked, FavoriteCategories), EFavoriteRefType
#include "EmoteData.h"   // g_Emotes / g_EmotesMutex / BuildCatalogIndex
#include "MeMotes.h"     // g_MeMotes / g_MeMotesMutex / MeMote

const CatalogView& GetCatalogView() {
    static CatalogView v;
    static uint64_t builtEmote = (uint64_t)-1, builtMe = (uint64_t)-1, builtUi = (uint64_t)-1;

    const uint64_t e  = g_EmoteCatalogVersion.load(std::memory_order_relaxed);
    const uint64_t m  = g_MeMotesVersion.load(std::memory_order_relaxed);
    const uint64_t ui = g_UiViewRevision.load(std::memory_order_relaxed);
    if (e == builtEmote && m == builtMe && ui == builtUi) return v;   // hot path: nothing changed
    builtEmote = e; builtMe = m; builtUi = ui;

    // Emotes half: byId + unlockedIds (BuildCatalogIndex clears + fills) and the
    // favorited-id sets. Favorites live in g_Settings (render-thread owned), but we
    // fold them into this locked block for one consistent snapshot point.
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        BuildCatalogIndex(g_Settings.ManuallyUnlocked, v.idx);
        v.favoritedIds.clear();
        v.favoritedMeMoteIds.clear();
        for (const auto& c : g_Settings.FavoriteCategories)
            for (const auto& r : c.Refs) {
                if (r.Type == EFavoriteRefType::Emote)       v.favoritedIds.insert(r.Id);
                else if (r.Type == EFavoriteRefType::MeMote) v.favoritedMeMoteIds.insert(r.Id);
            }
    }
    // /me-motes half: Id -> ptr. Separate lock so we never nest the two mutexes.
    {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        v.meMotesById.clear();
        v.meMotesById.reserve(g_MeMotes.size());
        for (const auto& mm : g_MeMotes) v.meMotesById[mm.Id] = &mm;
    }
    return v;
}
