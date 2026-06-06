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

struct EKey {
    std::string id;
    EViewMode   mode;
    int         w;   // maxW rounded to nearest pixel (sub-pixel drift = noise)
    bool operator==(const EKey& o) const {
        return w == o.w && mode == o.mode && id == o.id;
    }
};
struct EKeyHash {
    size_t operator()(const EKey& k) const noexcept {
        // Boost-style hash combine. The id hash carries the bulk of the entropy;
        // mode + w are 2- and ~6-bit inputs, so a simple xor-shift mix is enough.
        size_t h = std::hash<std::string>{}(k.id);
        h ^= (static_cast<size_t>(k.mode) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
        h ^= (static_cast<size_t>(k.w)    + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
        return h;
    }
};

struct FKey {
    std::string id;
    int         w;
    bool operator==(const FKey& o) const { return w == o.w && id == o.id; }
};
struct FKeyHash {
    size_t operator()(const FKey& k) const noexcept {
        size_t h = std::hash<std::string>{}(k.id);
        h ^= (static_cast<size_t>(k.w) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
        return h;
    }
};

// Main-thread-only state - never touched from a worker (workers bump the atomic
// version counter, the cache reads it). No mutex needed.
std::unordered_map<EKey, TextCache::EllipsizedEntry, EKeyHash> s_emap;
std::unordered_map<FKey, TextCache::FitEntry,        FKeyHash> s_fmap;
uint64_t s_lastVersion  = 0;
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
// public *Cached function so callers can't forget.
inline void MaintainEpoch() {
    const uint64_t v = g_EmoteCatalogVersion.load(std::memory_order_relaxed);
    const float    f = ImGui::GetFontSize();
    if (v != s_lastVersion || f != s_lastFontSize) {
        s_emap.clear();
        s_fmap.clear();
        s_lastVersion  = v;
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
                                       EViewMode mode, float maxW) {
    MaintainEpoch();
    EKey k{ emoteId, mode, Quantize(maxW) };
    auto it = s_emap.find(k);
    if (it != s_emap.end()) return it->second;
    auto built = BuildEllipsized(name, maxW);
    auto ins = s_emap.emplace(std::move(k), std::move(built));
    return ins.first->second;
}

const FitEntry& FitNameCached(const std::string& emoteId,
                              const std::string& name,
                              float maxW) {
    MaintainEpoch();
    FKey k{ emoteId, Quantize(maxW) };
    auto it = s_fmap.find(k);
    if (it != s_fmap.end()) return it->second;
    auto built = BuildFit(name, maxW);
    auto ins = s_fmap.emplace(std::move(k), std::move(built));
    return ins.first->second;
}

}  // namespace TextCache
