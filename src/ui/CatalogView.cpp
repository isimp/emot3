#include "CatalogView.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "Globals.h"     // g_EmoteCatalogVersion / g_MeMotesVersion / g_UiViewRevision
#include "Settings.h"    // g_Settings (ManuallyUnlocked, FavoriteCategories), EFavoriteRefType
#include "EmoteData.h"   // g_Emotes / g_EmotesMutex / BuildCatalogIndex
#include "MeMotes.h"     // g_MeMotes / g_MeMotesMutex / MeMote

namespace {
// The cached view + the versions it was built for. File-scope (not a function-local
// static) so the dev-tool stats accessor below can read the live view. Render-thread
// only - GetCatalogView() and CatalogViewStats() both run there.
CatalogView s_view;
uint64_t    s_builtEmote = (uint64_t)-1;
uint64_t    s_builtMe    = (uint64_t)-1;
uint64_t    s_builtUi    = (uint64_t)-1;
}  // namespace

const CatalogView& GetCatalogView() {
    const uint64_t e  = g_EmoteCatalogVersion.load(std::memory_order_relaxed);
    const uint64_t m  = g_MeMotesVersion.load(std::memory_order_relaxed);
    const uint64_t ui = g_UiViewRevision.load(std::memory_order_relaxed);
    if (e == s_builtEmote && m == s_builtMe && ui == s_builtUi) return s_view;  // hot path: nothing changed
    s_builtEmote = e; s_builtMe = m; s_builtUi = ui;

    // Emotes half: byId + unlockedIds (BuildCatalogIndex clears + fills) and the
    // favorited-id sets. Favorites live in g_Settings (render-thread owned), but we
    // fold them into this locked block for one consistent snapshot point.
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        BuildCatalogIndex(g_Settings.ManuallyUnlocked, s_view.idx);
        s_view.favoritedIds.clear();
        s_view.favoritedMeMoteIds.clear();
        for (const auto& c : g_Settings.FavoriteCategories)
            for (const auto& r : c.Refs) {
                if (r.Type == EFavoriteRefType::Emote)       s_view.favoritedIds.insert(r.Id);
                else if (r.Type == EFavoriteRefType::MeMote) s_view.favoritedMeMoteIds.insert(r.Id);
            }
    }
    // /me-motes half: Id -> ptr. Separate lock so we never nest the two mutexes.
    {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        s_view.meMotesById.clear();
        s_view.meMotesById.reserve(g_MeMotes.size());
        for (const auto& mm : g_MeMotes) s_view.meMotesById[mm.Id] = &mm;
    }
    return s_view;
}

// Dev-tool (MemoryMonitor) accessor: the cached view's total entry count + an estimated
// heap footprint - hash nodes + bucket arrays + any out-of-SSO key strings (the keys are
// COPIES of ids, distinct from g_Emotes' own strings, which is why this is its own row).
// Within ~2x, like the other memmon estimates. Render-thread only; reads the same static
// GetCatalogView() fills. Compiled in all flavors but only called under EMOT3_DEVTOOLS -
// the linker drops it from shipped builds.
void CatalogViewStats(size_t& count, size_t& bytes) {
    count = 0; bytes = 0;
    auto keyHeap = [](const std::string& s) -> size_t {
        return s.capacity() > 15 ? s.capacity() + 1 : 0;   // MSVC SSO holds 15 chars
    };
    // unordered node ~ key string + next-ptr + cached-hash (+ a value ptr for a map).
    auto mapStat = [&](const auto& m) {
        constexpr size_t kNode = sizeof(std::string) + sizeof(void*) + sizeof(void*) + sizeof(size_t);
        count += m.size();
        bytes += m.size() * kNode + m.bucket_count() * sizeof(void*);
        for (const auto& kv : m) bytes += keyHeap(kv.first);
    };
    auto setStat = [&](const auto& s) {
        constexpr size_t kNode = sizeof(std::string) + sizeof(void*) + sizeof(size_t);
        count += s.size();
        bytes += s.size() * kNode + s.bucket_count() * sizeof(void*);
        for (const auto& k : s) bytes += keyHeap(k);
    };
    mapStat(s_view.idx.byId);
    setStat(s_view.idx.unlockedIds);
    mapStat(s_view.meMotesById);
    setStat(s_view.favoritedIds);
    setStat(s_view.favoritedMeMoteIds);
}
