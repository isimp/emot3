#include "Icons.h"
#include "Globals.h"
#include "EmoteData.h"
#include "MeMotes.h"      // MeMote struct for ResolveMeMoteIconPath
#include "Settings.h"     // g_Settings.UseAIIconFallback (AI fallback gate)
#include "Resources.h"    // LookupBundledResource + kOfficialIcons / kAIIcons / kMeMoteAIIcons
#include "StringUtil.h"   // ToLower / IsAbsolutePath (shared helpers)
#include "WinEncoding.h"  // Utf8ToWide / Utf8ToAcp (Unicode icon paths)

#include "imgui/imgui.h"
#include "IconCacheConfig.h"   // g_IconCache.maxIconDim (user-icon dimension cap)

#include "Logging.h"      // LOG_DEBUG / LOG_WARNING (size cap + pool budget)

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Profiling.h"   // PROFILE_SCOPE on the cold texture-load branch

namespace {
// Tag prefix per bundled table - disambiguates official vs emote-AI vs
// me-mote-AI, which can share a name with DIFFERENT art (must not collide).
const char* BundledTagPrefix(const BundledIcon* tbl) {
    if (tbl == kOfficialIcons) return "o:";
    if (tbl == kAIIcons)       return "ea:";
    if (tbl == kMeMoteAIIcons) return "ma:";
    return "x:";
}
// Build the DiskFile ResolvedIcon for a user PNG/JPEG: cap-probe the header
// (over-cap / unreadable -> key "" so the cell shows a letter + a logged note),
// and fold mtime+size into the content key so an in-place edit reloads on the
// next re-resolve (Refresh button) while an unchanged file keeps its key (no
// churn). Key example: EMOT3IC_f:c:\...\bow.png:1d9f...:4a2.
ResolvedIcon MakeDiskIcon(const std::string& path) {
    ResolvedIcon r;
    int w = 0, h = 0;
    IconProbe pv = ProbeIconFile(path, w, h);
    if (pv != IconProbe::Ok) {
        LOG_DEBUG("icon skipped (%s): %s",
                  pv == IconProbe::TooLarge ? "over the size cap" : "unreadable",
                  path.c_str());
        return r;   // key "" -> letter fallback
    }
    unsigned long long mtime = 0, size = 0;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(Utf8ToWide(path).c_str(), GetFileExInfoStandard, &fad)) {
        mtime = ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) |
                 (unsigned long long)fad.ftLastWriteTime.dwLowDateTime;
        size  = ((unsigned long long)fad.nFileSizeHigh << 32) |
                 (unsigned long long)fad.nFileSizeLow;
    }
    char suf[48];
    std::snprintf(suf, sizeof suf, ":%llx:%llx", mtime, size);
    r.from = ResolvedIcon::From::DiskFile;
    r.path = path;
    r.key  = std::string("EMOT3IC_f:") + ToLower(path) + suf;
    return r;
}
}  // namespace

// Public builders, so the icon picker keys its thumbnails into the SAME content
// pool as cells (a folder icon used by an emote and shown in the picker is ONE
// texture). ResolveEmoteIcon/ResolveMeMoteIcon route through these too, so the
// tags can never drift between the picker and the cell path.
ResolvedIcon MakeBundledResolved(const BundledIcon* tbl, int count, const std::string& name) {
    ResolvedIcon r;
    if (!tbl || name.empty()) return r;
    r.from  = ResolvedIcon::From::BundledMem;
    r.table = tbl; r.count = count; r.name = name;   // raw name; the loader slash-strips
    // Key on a normalized stem (leading slash stripped, lowercased) so a
    // hand-authored Id like "/wave" dedups with "wave" and with a
    // bundled:official:wave ref - matching LookupBundledResource's normalization.
    std::string stem = name;
    if (!stem.empty() && stem.front() == '/') stem.erase(0, 1);
    r.key   = std::string("EMOT3IC_") + BundledTagPrefix(tbl) + ToLower(stem);
    return r;
}
ResolvedIcon MakeFileResolved(const std::string& fullPath) { return MakeDiskIcon(fullPath); }

std::string ResolveIconPath(const Emote& e) {
    std::string p = e.IconPath;
    if (p.empty()) {
        // Derive from the stable Id (English stem), NOT the localized
        // Command - bundled artwork is named in English (bow.png), so a
        // German catalog's "/verbeugen" must still resolve to "bow.png".
        std::string base = e.Id;
        if (!base.empty() && base.front() == '/') base.erase(0, 1);
        // Containment: an emote id can be a user/imported value (NormalizeEmoteCommand
        // keeps UTF-8 + interior slashes), so a crafted emotes.json id like
        // "/../../x" could escape icons/. Reject any path separator or drive/ADS
        // colon here -> no folder lookup (the resolver falls through to the
        // bundled/letter fallback). A normal id ("bow", "grübeln") has none and
        // still resolves. (/me-mote ids are NormalizeMeMoteId'd to [a-z0-9_], safe.)
        if (base.find_first_of("/\\:") != std::string::npos) return std::string();
        p = ToLower(base) + ".png";
    }
    bool isAbs = IsAbsolutePath(p);
    if (isAbs) return p;
    if (g_IconsDir.empty()) return p;
    return g_IconsDir + "\\" + p;
}

// The "bundled:" scheme helpers (Make/Parse/IsBundledIconRef) + SanitizeIconPath
// moved to data/IconPath.cpp — pure value logic, so the data layer sanitizes
// IconPath at JSON ingress without a ui/ dependency. Declared in data/IconPath.h,
// reached here via Icons.h.

IconSource ResolveIconSource(const Emote& e) {
    // 0. Icon picker: an explicit "bundled:<bucket>:<name>" ref wins over the
    //    whole chain and ignores UseAIIconFallback (an explicit pick, not the
    //    auto fallback). Loaded via MakeBundledResolved -> EnsureResolved.
    if (IsBundledIconRef(e.IconPath)) return IconSource::BundledChosen;
    // 1/2. A PNG on disk at the resolved path - either an explicit IconPath or
    //      the icons/<id>.png folder drop-in (ResolveIconPath returns the latter
    //      when IconPath is empty). The loader treats both the same; we only
    //      split them so the status line can name which it is.
    std::string path = ResolveIconPath(e);
    DWORD attr = GetFileAttributesW(Utf8ToWide(path).c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return e.IconPath.empty() ? IconSource::FolderOverride : IconSource::Custom;
    // 3. Bundled official artwork (keyed on the English Id).
    if (LookupBundledResource(kOfficialIcons, kOfficialIconsCount, e.Id) != 0)
        return IconSource::BundledOfficial;
    // 4. Bundled AI fallback, only when the user opted in.
    if (g_Settings.UseAIIconFallback &&
        LookupBundledResource(kAIIcons, kAIIconsCount, e.Id) != 0)
        return IconSource::BundledAI;
    // 5. Styled letter button.
    return IconSource::TextFallback;
}

// Resolve an emote to its content key + how to load it. Switches on the SAME
// ResolveIconSource chain the status line uses (single source of resolution
// order), then tags by tier so identical images share a key (dedup) and
// different-art tiers never collide. Does the cold-path disk stat (via
// MakeDiskIcon) for the file tiers.
ResolvedIcon ResolveEmoteIcon(const Emote& e) {
    switch (ResolveIconSource(e)) {
        case IconSource::BundledChosen: {
            const BundledIcon* tbl = nullptr; int cnt = 0; std::string nm;
            if (ParseBundledIconRef(e.IconPath, tbl, cnt, nm))
                return MakeBundledResolved(tbl, cnt, nm);
            return ResolvedIcon{};
        }
        case IconSource::Custom:
        case IconSource::FolderOverride:
            return MakeFileResolved(ResolveIconPath(e));   // cap-probed; key "" if over-cap/unreadable
        case IconSource::BundledOfficial:
            return MakeBundledResolved(kOfficialIcons, kOfficialIconsCount, e.Id);
        case IconSource::BundledAI:
            return MakeBundledResolved(kAIIcons, kAIIconsCount, e.Id);
        case IconSource::TextFallback:
        default:
            return ResolvedIcon{};  // key "" -> styled letter
    }
}

std::string ResolveMeMoteIconPath(const MeMote& m) {
    // Mirrors the Emote pattern: always returns a disk path. Explicit
    // IconPath wins (absolute returned as-is, relative joined to g_IconsDir);
    // otherwise derives "<id>.png" under g_IconsDir so the FolderOverride
    // tier in ResolveMeMoteIconSource has a path to stat. The returned path
    // may or may not exist — the resolver does the stat to decide which
    // tier fires.
    std::string p = m.IconPath;
    if (p.empty()) {
        // Derive from the stable Id (lowercased; strip any leading slash for
        // symmetry with Emote, though /me-mote Ids never carry one). Result
        // is a filename, joined to g_IconsDir below.
        std::string base = m.Id;
        if (!base.empty() && base.front() == '/') base.erase(0, 1);
        p = ToLower(base) + ".png";
    }
    bool isAbs = IsAbsolutePath(p);
    if (isAbs) return p;
    if (g_IconsDir.empty()) return p;
    return g_IconsDir + "\\" + p;
}

MeMoteIconSource ResolveMeMoteIconSource(const MeMote& m) {
    // 0. Icon picker: a "bundled:<bucket>:<name>" ref wins over the whole chain
    //    and ignores UseAIIconFallback (explicit pick). Checked before
    //    ResolveMeMoteIconPath so the ref is never mangled into a disk path.
    if (IsBundledIconRef(m.IconPath)) return MeMoteIconSource::BundledChosen;
    // 1/2. A PNG on disk at the resolved path — either an explicit IconPath
    //      (Custom) or the icons/<id>.png drop-in (FolderOverride). The
    //      loader treats both the same; we only split them so the status
    //      line can name which it is. A missing explicit IconPath falls
    //      through to the bundled AI tier — same fallthrough behaviour
    //      ResolveIconSource has for emotes.
    std::string path = ResolveMeMoteIconPath(m);
    DWORD attr = GetFileAttributesW(Utf8ToWide(path).c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return m.IconPath.empty() ? MeMoteIconSource::FolderOverride
                                  : MeMoteIconSource::Custom;
    // 3. Bundled AI fallback, only when the user opted in via the same
    //    UseAIIconFallback setting that governs Emote AI artwork. Keys on
    //    the stable /me-mote Id (lfg.png, brb.png), case-insensitively.
    if (g_Settings.UseAIIconFallback &&
        LookupBundledResource(kMeMoteAIIcons, kMeMoteAIIconsCount, m.Id) != 0)
        return MeMoteIconSource::BundledAI;
    // 4. Styled letter button (drawn by RenderMeMoteCellBody when no
    //    texture is loaded for the cache key).
    return MeMoteIconSource::TextFallback;
}

// Resolve a /me-mote to its content key + load descriptor (mirrors
// ResolveEmoteIcon; shorter chain - no BundledOfficial tier). A disk icon
// shared with an emote (same file) dedups to one texture: the `f:` tag is
// path-based, not catalog-prefixed.
ResolvedIcon ResolveMeMoteIcon(const MeMote& m) {
    switch (ResolveMeMoteIconSource(m)) {
        case MeMoteIconSource::BundledChosen: {
            const BundledIcon* tbl = nullptr; int cnt = 0; std::string nm;
            if (ParseBundledIconRef(m.IconPath, tbl, cnt, nm))
                return MakeBundledResolved(tbl, cnt, nm);
            return ResolvedIcon{};
        }
        case MeMoteIconSource::Custom:
        case MeMoteIconSource::FolderOverride:
            return MakeFileResolved(ResolveMeMoteIconPath(m));
        case MeMoteIconSource::BundledAI:
            return MakeBundledResolved(kMeMoteAIIcons, kMeMoteAIIconsCount, m.Id);
        case MeMoteIconSource::TextFallback:
        default:
            return ResolvedIcon{};
    }
}

// --- content-addressed lazy texture cache --------------------------------
// Every distinct image loads at most once, under a CONTENT key (ResolveEmoteIcon
// / ResolveMeMoteIcon), so two entries with the same icon share one Nexus
// texture (dedup) and re-selecting a prior image is free. Nexus never evicts, so
// a content key, once loaded, stays for the process - we lean into that. Disk
// icons fold mtime+size into the key, so an in-place edit yields a new key (the
// Refresh button re-resolves to pick it up) while an unchanged file keeps its
// key (nothing reloads, nothing churns). Main-thread-only state, so no mutex:
// callers pass the entity by reference and own its lifetime; we never lock the
// catalog mutex (safe from a cell/row loop that already holds it).
namespace {
// Per-id memo: the resolved content key + a latched Texture* (cached once the
// async load finishes, so the steady-state hot path skips the per-frame
// Textures.Get on the long content key). Epoch-scoped: MaintainTexEpoch clears
// these on a catalog bump; InvalidateEmoteIcon/MeMoteIcon drop one entry.
struct MemoEntry { std::string key; Texture* tex = nullptr; };
std::unordered_map<std::string, MemoEntry> s_emoteKey;   // emote Id    -> {key, tex} (epoch-scoped)
std::unordered_map<std::string, MemoEntry> s_memoteKey;  // /me-mote Id -> {key, tex} (epoch-scoped)
std::unordered_set<std::string> s_attemptedKeys;           // content keys load-issued (permanent: key == content)
uint64_t s_lastEmoteTexV   = 0;
uint64_t s_lastMeMoteTexV  = 0;
bool     s_poolBudgetLogged = false;

#ifdef EMOT3_DEVTOOLS
// content key -> last frame requested, for the memmon in-use vs idle split.
std::unordered_map<std::string, uint32_t> s_keyLastFrame;
void TouchKey(const std::string& key) {
    if (!key.empty()) s_keyLastFrame[key] = (uint32_t)ImGui::GetFrameCount();
}
#else
inline void TouchKey(const std::string&) {}
#endif

// Clear the id->key memos when a catalog version moves (an edit re-resolves
// entries, possibly to new content keys). s_attemptedKeys is NOT cleared - a
// content key is permanent (it already encodes the content + mtime).
void MaintainTexEpoch() {
    const uint64_t ev = g_EmoteCatalogVersion.load(std::memory_order_relaxed);
    const uint64_t mv = g_MeMotesVersion.load(std::memory_order_relaxed);
    if (ev != s_lastEmoteTexV)  { s_emoteKey.clear();  s_lastEmoteTexV  = ev; }
    if (mv != s_lastMeMoteTexV) { s_memoteKey.clear(); s_lastMeMoteTexV = mv; }
}

// Accurate CURRENT pool size, maintained INCREMENTALLY: each distinct loaded
// content texture (deduped) is summed exactly once, the first frame we observe it
// ready, into s_poolBytes (tracked in s_countedKeys). Keys are immutable and Nexus
// never evicts, so a counted texture's bytes never change - the total can't drift.
// The per-frame guard runs the reconcile at most once a frame, over only the
// not-yet-counted keys, so it's O(newly-loaded)/frame and O(1) once all loads
// settle. The budget enforces against this; the dev "used" readout (IconPoolStats)
// sums the same loaded keys, so the two agree once loads settle. (A key whose load
// never completes stays re-checked each frame; that set is tiny in practice.)
size_t IconPoolUsedBytes() {
    if (!APIDefs) return 0;
    static int    s_poolFrame = -1;
    static size_t s_poolBytes = 0;
    static std::unordered_set<std::string> s_countedKeys;
    const int now = ImGui::GetFrameCount();
    if (now == s_poolFrame) return s_poolBytes;
    s_poolFrame = now;
    for (const std::string& key : s_attemptedKeys) {
        if (s_countedKeys.count(key)) continue;        // already summed (immutable bytes)
        Texture* t = APIDefs->Textures.Get(key.c_str());
        if (t && t->Resource) {
            s_poolBytes += (size_t)t->Width * (size_t)t->Height * 4u;
            s_countedKeys.insert(key);
        }
    }
    return s_poolBytes;
}

// Optional ceiling (icon_cache.json pool_budget_mb; 0 = off). SOFT per frame:
// it checks the accurate current pool size (memoized per frame), so a burst of
// cold loads in one frame can overshoot a little before it trips - fine for a
// backstop, and it agrees with the memmon "used" readout.
bool PoolBudgetReached() {
    if (g_IconCache.poolBudgetMB <= 0) return false;
    return IconPoolUsedBytes() >= (size_t)g_IconCache.poolBudgetMB * 1024u * 1024u;
}
}  // namespace

// Load (once, dedup-aware) a resolved icon into the Nexus cache + return it.
// Shared by the cell render path (via Ensure*Texture) and the lazy picker grid,
// so picker thumbnails and cell icons reuse the same content textures. The disk
// path is already cap-validated by MakeDiskIcon (its key is "" otherwise).
Texture* EnsureResolved(const ResolvedIcon& r) {
    if (!APIDefs || r.from == ResolvedIcon::From::None || r.key.empty()) return nullptr;
    TouchKey(r.key);
    Texture* t = APIDefs->Textures.Get(r.key.c_str());
    if (t && t->Resource) {                      // ready: fresh load, a dedup hit, OR a texture that
        s_attemptedKeys.insert(r.key);           // survived an addon hot-reload - Nexus keeps textures
        return t;                                // until game restart, but our statics reset, so record
    }                                            // it here too or the pool stats + budget miss every
                                                 // icon already resident when this instance started.
    if (s_attemptedKeys.count(r.key)) return t;  // already issued (loading); don't re-load
    if (PoolBudgetReached()) {                   // optional opt-in soft cap
        if (!s_poolBudgetLogged) {
            s_poolBudgetLogged = true;
            LOG_WARNING("icon pool hit the %d MB budget; new icons use the letter "
                        "fallback until restart (icon_cache.json pool_budget_mb)",
                        g_IconCache.poolBudgetMB);
        }
        return t;
    }
    PROFILE_SCOPE("tex.load");                    // cold path only
    if (r.from == ResolvedIcon::From::BundledMem) {
        const void* data = nullptr; size_t size = 0;
        if (TryLoadBundledIconBytes(r.table, r.count, r.name, data, size))
            APIDefs->Textures.GetOrCreateFromMemory(r.key.c_str(), const_cast<void*>(data), size);
    } else {  // DiskFile
        // Nexus' GetOrCreateFromFile takes a const char* (no wide variant), so hand
        // it the path in the system codepage. A name not representable there (lossy)
        // can't be opened, so skip the load -> letter fallback (vs. a doomed/garbage
        // texture entry). Our own stat/probe above used the wide path, so existence
        // + header probe already succeeded for any Unicode name.
        bool lossy = false;
        const std::string acp = Utf8ToAcp(r.path, &lossy);
        if (!lossy && !acp.empty())
            APIDefs->Textures.GetOrCreateFromFile(r.key.c_str(), acp.c_str());
    }
    s_attemptedKeys.insert(r.key);
    return APIDefs->Textures.Get(r.key.c_str());  // may be null this frame if async
}

Texture* EnsureEmoteTexture(const Emote& e) {
    if (!APIDefs) return nullptr;
    MaintainTexEpoch();
    auto it = s_emoteKey.find(e.Id);
    if (it == s_emoteKey.end()) {                 // cold: resolve once per epoch (the one stat)
        ResolvedIcon r = ResolveEmoteIcon(e);
        Texture* t = EnsureResolved(r);           // loads + returns it (null this frame if still async)
        it = s_emoteKey.emplace(e.Id, MemoEntry{ r.key, t }).first;
    }
    MemoEntry& m = it->second;
    TouchKey(m.key);                               // mark in-use even on the memo-hit path
    if (m.tex) return m.tex;                        // latched: hot path, no Textures.Get
    if (m.key.empty()) return nullptr;             // text fallback (no texture)
    return (m.tex = APIDefs->Textures.Get(m.key.c_str()));  // still loading: re-check, latch when ready
}

Texture* EnsureMeMoteTexture(const MeMote& m) {
    if (!APIDefs) return nullptr;
    MaintainTexEpoch();
    auto it = s_memoteKey.find(m.Id);
    if (it == s_memoteKey.end()) {
        ResolvedIcon r = ResolveMeMoteIcon(m);
        Texture* t = EnsureResolved(r);
        it = s_memoteKey.emplace(m.Id, MemoEntry{ r.key, t }).first;
    }
    MemoEntry& me = it->second;
    TouchKey(me.key);
    if (me.tex) return me.tex;                      // latched: hot path, no Textures.Get
    if (me.key.empty()) return nullptr;             // text fallback (no texture)
    return (me.tex = APIDefs->Textures.Get(me.key.c_str()));  // still loading: re-check, latch when ready
}

// Refresh button (Options editors): drop one entry's memo so the next render
// re-resolves it - re-stats the file, so a changed mtime/size yields a new key
// and reloads; an unchanged file keeps its key (no reload, no churn).
void InvalidateEmoteIcon(const std::string& id)  { s_emoteKey.erase(id); }
void InvalidateMeMoteIcon(const std::string& id) { s_memoteKey.erase(id); }

#ifdef EMOT3_DEVTOOLS
// Deduped pool accounting for the memory monitor: every distinct content texture
// we've loaded, split into in-use (drawn this/last frame) vs idle (off-screen or
// orphaned by an edit). Each key counts ONCE (shared textures aren't double
// counted), and iterating s_attemptedKeys surfaces orphaned/churn slots the old
// per-catalog count couldn't see.
void IconPoolStats(IconPoolUsage& out) {
    out = IconPoolUsage{};
    if (!APIDefs) return;
    const uint32_t now = (uint32_t)ImGui::GetFrameCount();
    for (const std::string& key : s_attemptedKeys) {
        Texture* t = APIDefs->Textures.Get(key.c_str());
        if (!t || !t->Resource) continue;          // never-loaded / failed / async-pending key
        const size_t bytes = (size_t)t->Width * (size_t)t->Height * 4u;
        out.totalCount++; out.totalBytes += bytes;
        auto it = s_keyLastFrame.find(key);
        const bool inUse = (it != s_keyLastFrame.end()) && (now - it->second <= 1u);
        if (inUse) { out.inUseCount++; out.inUseBytes += bytes; }
        else       { out.idleCount++;  out.idleBytes  += bytes; }
    }
}

// Footprint of the lazy cache's BOOKKEEPING containers (the id->key memos + the
// permanent attempted-keys set + the dev per-key frame map) - NOT the textures,
// which IconPoolStats counts. These string-key maps/sets are invisible to the
// pool readout; s_attemptedKeys in particular grows monotonically (keys are
// immutable) so it's worth a memory-monitor row. Rough (~2x) - node overhead is
// an estimate. count = total entries across the containers.
void IconCacheKeyStats(size_t& count, size_t& bytes) {
    auto strHeap = [](const std::string& s) -> size_t {
        return s.capacity() > 15 ? s.capacity() + 1 : 0;
    };
    constexpr size_t kNode = 40;  // unordered_map/set node + bucket slot, rough
    count = 0; bytes = 0;
    for (const auto& kv : s_emoteKey) {
        ++count; bytes += kNode + sizeof(MemoEntry) + strHeap(kv.first) + strHeap(kv.second.key);
    }
    for (const auto& kv : s_memoteKey) {
        ++count; bytes += kNode + sizeof(MemoEntry) + strHeap(kv.first) + strHeap(kv.second.key);
    }
    for (const auto& k : s_attemptedKeys) {
        ++count; bytes += kNode + sizeof(std::string) + strHeap(k);
    }
    for (const auto& kv : s_keyLastFrame) {
        ++count; bytes += kNode + sizeof(std::string) + sizeof(uint32_t) + strHeap(kv.first);
    }
}
// One-shot (per addon load) sweep that records content textures ALREADY resident
// in Nexus but not yet in our lazy set. Per-cell recording only sees on-screen
// cells, so after a hot-reload (Nexus keeps textures until game restart while our
// statics reset) the off-screen icons that survived go uncounted until scrolled
// into view. This derives every catalog entry's key and records the resident
// ones. PROBE-ONLY (Textures.Get, never GetOrCreate): on a cold start nothing is
// resident, so it records nothing and never eager-loads - the lazy cell path
// stays the only loader.
void ReconcileResidentPool() {
    if (!APIDefs) return;
    auto probe = [](const std::string& key) {
        if (key.empty()) return;
        Texture* t = APIDefs->Textures.Get(key.c_str());
        if (t && t->Resource) s_attemptedKeys.insert(key);
    };
    { std::lock_guard<std::mutex> lk(g_EmotesMutex);  for (const Emote&  e : g_Emotes)  probe(ResolveEmoteIcon(e).key);  }
    { std::lock_guard<std::mutex> lk(g_MeMotesMutex); for (const MeMote& m : g_MeMotes) probe(ResolveMeMoteIcon(m).key); }
}
#endif

// --- user-icon dimension cap (header-only, no decode) --------------------
IconProbe ProbeIconFile(const std::string& path, int& outW, int& outH) {
    outW = 0; outH = 0;
    // Wide open for any Unicode name. std::filesystem::path is natively wide on
    // Windows and its fstream ctor is portable (MSVC + libstdc++/MinGW), unlike
    // the MSVC-only std::ifstream(const wchar_t*) extension.
    const std::filesystem::path fp(Utf8ToWide(path));
    std::ifstream f(fp, std::ios::binary);
    if (!f.is_open()) return IconProbe::Unreadable;
    // A JPEG's SOF can sit behind a large EXIF/APPn block, so read a bounded
    // header window (PNG dims live in the first 24 bytes; this covers the vast
    // majority of JPEGs). If SOF isn't within the window we report Unreadable.
    constexpr size_t kMaxHeader = 64 * 1024;
    std::vector<unsigned char> b(kMaxHeader);
    f.read(reinterpret_cast<char*>(b.data()), (std::streamsize)kMaxHeader);
    const size_t n = (size_t)f.gcount();
    b.resize(n);

    // Bounds-guarded big-endian reads: return 0 if the read would run past the
    // buffer, so a truncated/malformed header can't OOB-read regardless of how the
    // marker-walk below indexes (the preceding `if`s already keep callers in range;
    // this is defense-in-depth against a future call site forgetting that).
    auto be16 = [&](size_t i) -> int {
        if (i + 1 >= b.size()) return 0;
        return (b[i] << 8) | b[i + 1];
    };
    auto be32 = [&](size_t i) -> uint32_t {
        if (i + 3 >= b.size()) return 0;
        return ((uint32_t)b[i] << 24) | ((uint32_t)b[i + 1] << 16) |
               ((uint32_t)b[i + 2] << 8) | (uint32_t)b[i + 3];
    };
    auto classify = [&](long long w, long long h) -> IconProbe {
        if (w <= 0 || h <= 0) return IconProbe::Unreadable;   // bogus dims
        outW = (int)w; outH = (int)h;
        const long long cap = g_IconCache.maxIconDim;
        return (w > cap || h > cap) ? IconProbe::TooLarge : IconProbe::Ok;
    };

    // PNG: 8-byte signature, then the IHDR chunk (length+"IHDR") with width@16,
    // height@20 (big-endian).
    static const unsigned char kPng[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (n >= 24 && std::equal(kPng, kPng + 8, b.begin()) &&
        b[12] == 'I' && b[13] == 'H' && b[14] == 'D' && b[15] == 'R') {
        return classify((long long)be32(16), (long long)be32(20));
    }

    // JPEG: SOI (FFD8), then walk segments to the first SOF (FFC0..FFCF except
    // C4=DHT, C8=JPG, CC=DAC). SOF body: [len:2][prec:1][height:2][width:2].
    if (n >= 4 && b[0] == 0xFF && b[1] == 0xD8) {
        size_t i = 2;
        while (i + 3 < n) {
            if (b[i] != 0xFF)   { ++i; continue; }              // resync to next marker
            unsigned char marker = b[i + 1];
            if (marker == 0xFF) { ++i; continue; }              // run of 0xFF padding
            // Standalone markers carry no length: RSTn (D0-D7), SOI (D8),
            // EOI (D9), TEM (01).
            if ((marker >= 0xD0 && marker <= 0xD9) || marker == 0x01) { i += 2; continue; }
            int seg = be16(i + 2);                              // length incl. these 2 bytes
            if (seg < 2) break;                                  // malformed
            const bool isSOF = (marker >= 0xC0 && marker <= 0xCF) &&
                               marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
            if (isSOF) {
                if (i + 8 >= n) break;                           // SOF body not fully buffered
                return classify(be16(i + 7), be16(i + 5));       // width, height
            }
            i += 2 + (size_t)seg;                                // skip this segment
        }
        return IconProbe::Unreadable;                            // no SOF in the window
    }

    return IconProbe::Unreadable;                                // unrecognized format
}

bool IconFileWithinCap(const std::string& path) {
    int w = 0, h = 0;
    return ProbeIconFile(path, w, h) == IconProbe::Ok;
}

// The UI-decoration draw helpers (DrawStarIcon / DrawPaperclipIcon /
// DrawCollapseArrow / DrawTrashIcon / DrawLockOverlay / DrawTargetableDot /
// DrawMeMoteIndicator) moved to ui/IconDrawing.{h,cpp}. They were pure ImGui
// drawing over the EMOT3_UI_* textures with no tie to this file's resolution /
// texture-cache core.
