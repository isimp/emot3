#include "TextCache.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>

#include "Globals.h"   // g_EmoteCatalogVersion

// Per-mode keying: TextOnly + Compact both use Ellipsize, but their maxW comes
// from different per-mode derivations (TextOnly = cellW - dot reservation;
// Compact = strip width minus its inner padding). Keeping mode in the key
// separates their slots so a coincidentally-equal maxW never crosses streams.
// Full uses its own FitName map below.
namespace {

// `meMote` namespaces the key by catalog, mirroring the texture cache's
// EMOT3_MM_ prefix (Icons.cpp): an Emote and a /me-mote can share an Id (their
// namespaces are independent), and without this discriminator the second cell
// to render in a frame would read the first's cached label by key collision.
struct EKey {
    std::string id;
    EViewMode   mode;
    int         w;   // maxW rounded to nearest pixel (sub-pixel drift = noise)
    bool        meMote;
    bool operator==(const EKey& o) const {
        return w == o.w && mode == o.mode && meMote == o.meMote && id == o.id;
    }
};
struct EKeyHash {
    size_t operator()(const EKey& k) const noexcept {
        // Boost-style hash combine. The id hash carries the bulk of the entropy;
        // mode + w are 2- and ~6-bit inputs, so a simple xor-shift mix is enough.
        size_t h = std::hash<std::string>{}(k.id);
        h ^= (static_cast<size_t>(k.mode)   + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
        h ^= (static_cast<size_t>(k.w)      + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
        h ^= (static_cast<size_t>(k.meMote) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
        return h;
    }
};

struct FKey {
    std::string id;
    int         w;
    bool        meMote;
    bool operator==(const FKey& o) const { return w == o.w && meMote == o.meMote && id == o.id; }
};
struct FKeyHash {
    size_t operator()(const FKey& k) const noexcept {
        size_t h = std::hash<std::string>{}(k.id);
        h ^= (static_cast<size_t>(k.w)      + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
        h ^= (static_cast<size_t>(k.meMote) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
        return h;
    }
};

// Main-thread-only state - never touched from a worker (workers bump the atomic
// version counter, the cache reads it). No mutex needed.
std::unordered_map<EKey, TextCache::EllipsizedEntry, EKeyHash> s_emap;
std::unordered_map<FKey, TextCache::FitEntry,        FKeyHash> s_fmap;
// Two version counters because the cache is shared by Emote and /me-mote cells
// (CellInfo carries either e or m; RenderEmoteCell/RenderMeMoteCellBody both
// call EllipsizeCached + FitNameCached with their own Ids). A Name edit in
// either catalog must invalidate the cached labels — observe both versions and
// nuke the maps when either changes. Adding a third catalog later means one
// more `s_lastFooV` field + branch here, nothing else.
uint64_t s_lastEmoteV  = 0;
uint64_t s_lastMeMoteV = 0;
float    s_lastFontSize = 0.f;

// Quantize maxW to the nearest integer pixel. The original Ellipsize compared
// `<= maxW`; rounding can at worst pick a label one glyph shorter than the
// live maxW would have allowed, which is below noticeable. Floor would
// systematically under-fit; round is the symmetric choice.
inline int Quantize(float maxW) {
    return (int)std::lround(maxW);
}

// Clear both maps + reseed the version/font epoch when an invalidation event
// fired between this lookup and the previous one. Called at the top of each
// public *Cached function so callers can't forget. The epoch is "either
// catalog's version OR the font" — any change blows the maps so labels
// re-shape with current data.
inline void MaintainEpoch() {
    const uint64_t ev = g_EmoteCatalogVersion.load(std::memory_order_relaxed);
    const uint64_t mv = g_MeMotesVersion.load(std::memory_order_relaxed);
    const float    f  = ImGui::GetFontSize();
    if (ev != s_lastEmoteV || mv != s_lastMeMoteV || f != s_lastFontSize) {
        s_emap.clear();
        s_fmap.clear();
        s_lastEmoteV   = ev;
        s_lastMeMoteV  = mv;
        s_lastFontSize = f;
    }
}

// Slow paths: the original Ellipsize / FitName bodies, but capturing the
// measured ImVec2 size of the chosen result before returning so the caller can
// skip the final CalcTextSize.

TextCache::EllipsizedEntry BuildEllipsized(const std::string& name, float maxW) {
    TextCache::EllipsizedEntry out;
    ImVec2 sz = ImGui::CalcTextSize(name.c_str());
    if (sz.x <= maxW) { out.label = name; out.size = sz; return out; }

    std::string s = name;
    while (!s.empty()) {
        std::string trial = s + "..";
        ImVec2 ts = ImGui::CalcTextSize(trial.c_str());
        if (ts.x <= maxW) { out.label = std::move(trial); out.size = ts; return out; }
        s.pop_back();
    }
    out.label = "..";
    out.size  = ImGui::CalcTextSize(out.label.c_str());
    return out;
}

TextCache::FitEntry BuildFit(const std::string& name, float maxW) {
    TextCache::FitEntry out;
    ImVec2 sz = ImGui::CalcTextSize(name.c_str());
    if (sz.x <= maxW) {
        out.line1 = name;
        out.size1 = sz;
        // line2 stays empty; size2 zero-initialized.
        return out;
    }

    // Last space whose split keeps both halves under maxW wins (matches the
    // original FitName behaviour byte-for-byte: longest-second-line variant).
    size_t bestSplit = std::string::npos;
    ImVec2 bestS1{}, bestS2{};
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] != ' ') continue;
        std::string l1 = name.substr(0, i);
        std::string l2 = name.substr(i + 1);
        ImVec2 ts1 = ImGui::CalcTextSize(l1.c_str());
        ImVec2 ts2 = ImGui::CalcTextSize(l2.c_str());
        if (ts1.x <= maxW && ts2.x <= maxW) {
            bestSplit = i;
            bestS1 = ts1; bestS2 = ts2;
        }
    }
    if (bestSplit != std::string::npos) {
        out.line1 = name.substr(0, bestSplit);
        out.line2 = name.substr(bestSplit + 1);
        out.size1 = bestS1;
        out.size2 = bestS2;
        return out;
    }

    // No two-line split fits - fall back to single-line ellipsis.
    std::string s = name;
    while (!s.empty()) {
        std::string trial = s + "..";
        ImVec2 ts = ImGui::CalcTextSize(trial.c_str());
        if (ts.x <= maxW) {
            out.line1 = std::move(trial);
            out.size1 = ts;
            return out;
        }
        s.pop_back();
    }
    out.line1 = "..";
    out.size1 = ImGui::CalcTextSize(out.line1.c_str());
    return out;
}

}  // namespace

namespace TextCache {

const EllipsizedEntry& EllipsizeCached(const std::string& emoteId,
                                       const std::string& name,
                                       EViewMode mode, float maxW, bool meMote) {
    MaintainEpoch();
    EKey k{ emoteId, mode, Quantize(maxW), meMote };
    auto it = s_emap.find(k);
    if (it != s_emap.end()) return it->second;
    auto built = BuildEllipsized(name, maxW);
    auto ins = s_emap.emplace(std::move(k), std::move(built));
    return ins.first->second;
}

const FitEntry& FitNameCached(const std::string& emoteId,
                              const std::string& name,
                              float maxW, bool meMote) {
    MaintainEpoch();
    FKey k{ emoteId, Quantize(maxW), meMote };
    auto it = s_fmap.find(k);
    if (it != s_fmap.end()) return it->second;
    auto built = BuildFit(name, maxW);
    auto ins = s_fmap.emplace(std::move(k), std::move(built));
    return ins.first->second;
}

// --- Dev-tool introspection -------------------------------------------------
// Implementations live in this TU because the key/value types (EKey/FKey/
// EllipsizedEntry/FitEntry) are file-private. sizeof() is taken here so the
// header doesn't have to expose them.

size_t EllipsizeMapSize() { return s_emap.size(); }
size_t FitMapSize()       { return s_fmap.size(); }

size_t ApproxBytes() {
    // MSVC SSO threshold: 15 chars in-struct. Beyond that, std::string holds
    // capacity+1 heap bytes (capacity chars + null terminator). Allocator may
    // round up; the estimate is within ~2x by design.
    auto strHeap = [](const std::string& s) -> size_t {
        return s.capacity() > 15 ? s.capacity() + 1 : 0;
    };

    size_t bytes = 0;

    // Ellipsize map: bucket array + per-node overhead + per-entry strings.
    // 24 = rough MSVC x64 unordered_map node header (next ptr + cached hash +
    // padding). EllipsizedEntry holds one std::string (label) inline.
    bytes += s_emap.bucket_count() * sizeof(void*);
    bytes += s_emap.size() * (sizeof(EKey) + sizeof(EllipsizedEntry) + 24);
    for (const auto& kv : s_emap) {
        bytes += strHeap(kv.first.id);
        bytes += strHeap(kv.second.label);
    }

    // FitName map: same shape, but FitEntry holds two strings (line1, line2).
    bytes += s_fmap.bucket_count() * sizeof(void*);
    bytes += s_fmap.size() * (sizeof(FKey) + sizeof(FitEntry) + 24);
    for (const auto& kv : s_fmap) {
        bytes += strHeap(kv.first.id);
        bytes += strHeap(kv.second.line1);
        bytes += strHeap(kv.second.line2);
    }
    return bytes;
}

}  // namespace TextCache
