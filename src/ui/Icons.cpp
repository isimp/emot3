#include "Icons.h"
#include "Globals.h"
#include "EmoteData.h"
#include "MeMotes.h"      // MeMote struct for ResolveMeMoteIconPath
#include "Settings.h"     // g_Settings.UseAIIconFallback (AI fallback gate)
#include "Resources.h"    // LookupBundledResource + kOfficialIcons / kAIIcons / kMeMoteAIIcons

#include "imgui/imgui.h"
#include "IconCacheConfig.h"   // g_IconCache.maxIconDim (user-icon dimension cap)

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <vector>

std::string ResolveIconPath(const Emote& e) {
    std::string p = e.IconPath;
    if (p.empty()) {
        // Derive from the stable Id (English stem), NOT the localized
        // Command - bundled artwork is named in English (bow.png), so a
        // German catalog's "/verbeugen" must still resolve to "bow.png".
        std::string base = e.Id;
        if (!base.empty() && base.front() == '/') base.erase(0, 1);
        std::transform(base.begin(), base.end(), base.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        p = base + ".png";
    }
    bool isAbs = (p.size() >= 3 && p[1] == ':' &&
                  (p[2] == '\\' || p[2] == '/')) ||
                 (p.size() >= 2 && p[0] == '\\' && p[1] == '\\');
    if (isAbs) return p;
    if (g_IconsDir.empty()) return p;
    return g_IconsDir + "\\" + p;
}

// --- "bundled:" scheme helpers (see Icons.h) -----------------------------
std::string MakeBundledIconRef(BundledBucket bucket, const std::string& name) {
    const char* b = bucket == BundledBucket::Official ? "official"
                  : bucket == BundledBucket::AI       ? "ai"
                                                      : "memote_ai";
    return std::string("bundled:") + b + ":" + name;
}

bool ParseBundledIconRef(const std::string& iconPath,
                         const BundledIcon*& outTable, int& outCount,
                         std::string& outName) {
    static const std::string kPfx = "bundled:";
    if (iconPath.size() <= kPfx.size() ||
        iconPath.compare(0, kPfx.size(), kPfx) != 0)
        return false;
    const size_t bstart = kPfx.size();
    const size_t colon  = iconPath.find(':', bstart);
    if (colon == std::string::npos) return false;
    const std::string bucket = iconPath.substr(bstart, colon - bstart);
    std::string name = iconPath.substr(colon + 1);
    if (name.empty()) return false;

    const BundledIcon* tbl = nullptr; int cnt = 0;
    if      (bucket == "official")  { tbl = kOfficialIcons; cnt = kOfficialIconsCount; }
    else if (bucket == "ai")        { tbl = kAIIcons;       cnt = kAIIconsCount; }
    else if (bucket == "memote_ai") { tbl = kMeMoteAIIcons; cnt = kMeMoteAIIconsCount; }
    else return false;

    // Only a ref whose name actually exists is "resolvable"; a stale/typo'd ref
    // returns false so the caller falls back to the normal resolution chain.
    if (LookupBundledResource(tbl, cnt, name) == 0) return false;
    outTable = tbl; outCount = cnt; outName = std::move(name);
    return true;
}

bool IsBundledIconRef(const std::string& iconPath) {
    const BundledIcon* t = nullptr; int c = 0; std::string n;
    return ParseBundledIconRef(iconPath, t, c, n);
}

std::string SanitizeIconPath(const std::string& raw, bool* outChanged) {
    auto mark = [&](const std::string& v) {
        if (outChanged) *outChanged = (v != raw);
        return v;
    };

    // 1. Empty -> empty.
    if (raw.empty()) return mark(std::string());

    // 2. Bundled refs pass through untouched. Their validity is checked
    //    later by ParseBundledIconRef (stale bucket/name falls through to
    //    the regular resolution chain). The prefix check at byte 0 makes
    //    them distinguishable from disk paths (drive-letter colons live
    //    at byte 1).
    static const std::string kBundledPfx = "bundled:";
    if (raw.size() >= kBundledPfx.size() &&
        raw.compare(0, kBundledPfx.size(), kBundledPfx) == 0)
        return mark(raw);

    // 3. Trim ASCII whitespace, normalize '/' -> '\\', strip leading
    //    separators. After this, the value is either an absolute path
    //    (rejected below) or a relative path under g_IconsDir.
    std::string s = raw;
    auto isws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!s.empty() && isws(s.front())) s.erase(s.begin());
    while (!s.empty() && isws(s.back()))  s.pop_back();
    if (s.empty()) return mark(std::string());
    for (auto& ch : s) if (ch == '/') ch = '\\';

    // 4. Absolute paths -> rejected. Drive-letter "X:\..." and UNC
    //    "\\server\share\..." both. After this rule + the leading-strip
    //    below, anything that survives is a path RELATIVE to g_IconsDir.
    bool isAbs = (s.size() >= 3 && s[1] == ':' &&
                  (s[2] == '\\' || s[2] == '/')) ||
                 (s.size() >= 2 && s[0] == '\\' && s[1] == '\\');
    if (isAbs) return mark(std::string());

    while (!s.empty() && s.front() == '\\') s.erase(s.begin());
    if (s.empty()) return mark(std::string());

    // 5. Reject '..' segments anywhere in the path (no folder-escape).
    //    Conservative — treats both literal ".." between separators and a
    //    bare ".." filename the same way.
    {
        size_t pos = 0;
        while (pos < s.size()) {
            size_t sep = s.find('\\', pos);
            std::string seg = (sep == std::string::npos)
                                ? s.substr(pos)
                                : s.substr(pos, sep - pos);
            if (seg == "..") return mark(std::string());
            if (sep == std::string::npos) break;
            pos = sep + 1;
        }
    }

    // 6. Reject the top-level "ui\" subfolder — that's where UI overrides
    //    live (star/paperclip/lock/target_dot/me_mote_dot), not emote icons.
    //    Case-insensitive on the literal "ui" prefix. A deeper "ui" folder
    //    (e.g. "themes\\ui\\cool.png") is allowed — only the top-level
    //    boundary is policed.
    if (s.size() >= 3) {
        char a = (char)std::tolower((unsigned char)s[0]);
        char b = (char)std::tolower((unsigned char)s[1]);
        if (a == 'u' && b == 'i' && s[2] == '\\')
            return mark(std::string());
    }

    return mark(s);
}

IconSource ResolveIconSource(const Emote& e) {
    // 0. Icon picker: an explicit "bundled:<bucket>:<name>" ref wins over the
    //    whole chain and ignores UseAIIconFallback (an explicit pick, not the
    //    auto fallback). Loaded by LoadEmoteTextures' BundledChosen case.
    if (IsBundledIconRef(e.IconPath)) return IconSource::BundledChosen;
    // 1/2. A PNG on disk at the resolved path - either an explicit IconPath or
    //      the icons/<id>.png folder drop-in (ResolveIconPath returns the latter
    //      when IconPath is empty). The loader treats both the same; we only
    //      split them so the status line can name which it is.
    std::string path = ResolveIconPath(e);
    DWORD attr = GetFileAttributesA(path.c_str());
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

std::string EmoteCacheKey(const std::string& command) {
    std::string s = command;
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return std::string("EMOT3_") + s;
}

Texture* GetEmoteTexture(const std::string& command) {
    return APIDefs->Textures.Get(EmoteCacheKey(command).c_str());
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
        std::transform(base.begin(), base.end(), base.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        p = base + ".png";
    }
    bool isAbs = (p.size() >= 3 && p[1] == ':' &&
                  (p[2] == '\\' || p[2] == '/')) ||
                 (p.size() >= 2 && p[0] == '\\' && p[1] == '\\');
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
    DWORD attr = GetFileAttributesA(path.c_str());
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

std::string MeMoteCacheKey(const std::string& id) {
    // Separate namespace from Emotes (EMOT3_<id>) so an Emote "wave" and a
    // /me-mote "wave" don't fight over the same Nexus texture cache slot.
    std::string s = id;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return std::string("EMOT3_MM_") + s;
}

Texture* GetMeMoteTexture(const std::string& id) {
    if (!APIDefs) return nullptr;
    return APIDefs->Textures.Get(MeMoteCacheKey(id).c_str());
}

// --- user-icon dimension cap (header-only, no decode) --------------------
IconProbe ProbeIconFile(const std::string& path, int& outW, int& outH) {
    outW = 0; outH = 0;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return IconProbe::Unreadable;
    // A JPEG's SOF can sit behind a large EXIF/APPn block, so read a bounded
    // header window (PNG dims live in the first 24 bytes; this covers the vast
    // majority of JPEGs). If SOF isn't within the window we report Unreadable.
    constexpr size_t kMaxHeader = 64 * 1024;
    std::vector<unsigned char> b(kMaxHeader);
    f.read(reinterpret_cast<char*>(b.data()), (std::streamsize)kMaxHeader);
    const size_t n = (size_t)f.gcount();
    b.resize(n);

    auto be16 = [&](size_t i) -> int { return (b[i] << 8) | b[i + 1]; };
    auto be32 = [&](size_t i) -> uint32_t {
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

// All UI icons (star / paperclip / lock / target_dot) are now sourced
// from bundled PNGs in resources/ui/, loaded into Nexus' texture cache
// at addon load time (see LoadUiIconOverrides). The hand-drawn
// fallbacks that used to live here have been removed: the bundle is
// shipped with the DLL so it's always available, and a missing PNG
// would now indicate a build problem rather than a normal case worth
// catching. If a texture isn't present, we draw nothing rather than
// falling back to an inferior look - the absence is more obvious and
// the build problem gets noticed instead of papered over.

void DrawStarIcon(ImVec2 c, float r, ImU32 col) {
    Texture* tex = APIDefs ? APIDefs->Textures.Get("EMOT3_UI_STAR") : nullptr;
    if (!tex || !tex->Resource) return;
    int alpha = (col >> IM_COL32_A_SHIFT) & 0xFF;
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex->Resource,
        ImVec2(c.x - r, c.y - r),
        ImVec2(c.x + r, c.y + r),
        ImVec2(0, 0), ImVec2(1, 1),
        IM_COL32(255, 255, 255, alpha));
}

void DrawPaperclipIcon(ImVec2 c, float r, ImU32 col) {
    Texture* tex = APIDefs ? APIDefs->Textures.Get("EMOT3_UI_PAPERCLIP") : nullptr;
    if (!tex || !tex->Resource) return;
    int alpha = (col >> IM_COL32_A_SHIFT) & 0xFF;
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex->Resource,
        ImVec2(c.x - r, c.y - r),
        ImVec2(c.x + r, c.y + r),
        ImVec2(0, 0), ImVec2(1, 1),
        IM_COL32(255, 255, 255, alpha));
}

void DrawCollapseArrow(ImVec2 c, float r, bool collapsed, ImU32 col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (collapsed) {
        // Right-pointing (>): content is hidden.
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.5f, c.y - r),
                              ImVec2(c.x - r * 0.5f, c.y + r),
                              ImVec2(c.x + r * 0.7f, c.y), col);
    } else {
        // Down-pointing (v): content is shown.
        dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.5f),
                              ImVec2(c.x + r, c.y - r * 0.5f),
                              ImVec2(c.x, c.y + r * 0.7f), col);
    }
}

void DrawTrashIcon(ImVec2 c, float r, ImU32 col, ImDrawList* dl) {
    if (!dl) dl = ImGui::GetWindowDrawList();
    float w = r * 1.5f, h = r * 1.8f;
    float t = std::max(1.f, r * 0.18f);     // stroke thickness
    ImVec2 b0(c.x - w * 0.5f, c.y - h * 0.30f);   // bin body top-left
    ImVec2 b1(c.x + w * 0.5f, c.y + h * 0.55f);   // bin body bottom-right
    dl->AddRect(b0, b1, col, 1.5f, 0, t);                                   // body
    float lidY = b0.y - t * 1.2f;
    dl->AddLine(ImVec2(c.x - w * 0.66f, lidY), ImVec2(c.x + w * 0.66f, lidY), col, t);  // lid
    dl->AddLine(ImVec2(c.x - r * 0.30f, lidY - t * 1.8f),
                ImVec2(c.x + r * 0.30f, lidY - t * 1.8f), col, t);          // handle
    dl->AddLine(ImVec2(c.x - w * 0.16f, b0.y + t * 1.5f),
                ImVec2(c.x - w * 0.16f, b1.y - t * 1.5f), col, t);          // ribs
    dl->AddLine(ImVec2(c.x + w * 0.16f, b0.y + t * 1.5f),
                ImVec2(c.x + w * 0.16f, b1.y - t * 1.5f), col, t);
}

void DrawLockOverlay() {
    Texture* tex = APIDefs ? APIDefs->Textures.Get("EMOT3_UI_LOCK") : nullptr;
    if (!tex || !tex->Resource) return;
    ImVec2 imin = ImGui::GetItemRectMin();
    ImVec2 imax = ImGui::GetItemRectMax();
    float w = imax.x - imin.x;
    float h = imax.y - imin.y;
    if (w < 8.f || h < 8.f) return;
    ImVec2 center((imin.x + imax.x) * 0.5f, (imin.y + imax.y) * 0.5f);
    float sz = std::min(w, h) * 0.55f;
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex->Resource,
        ImVec2(center.x - sz * 0.5f, center.y - sz * 0.5f),
        ImVec2(center.x + sz * 0.5f, center.y + sz * 0.5f));
}

void DrawTargetableDot(float dotSz, float alphaMul) {
    Texture* tex = APIDefs ? APIDefs->Textures.Get("EMOT3_UI_TARGET") : nullptr;
    if (!tex || !tex->Resource) return;
    if (dotSz < 4.f) return;  // below this it's an unreadable speck - skip

    // dotSz is computed by the caller (RenderEmoteCell) from a per-mode
    // reference, so the dot looks the same size in every view mode and tracks
    // the icon-scale slider. The dot is anchored to the top-right corner of
    // the last submitted item (the emote button).
    //
    // Pad is the gap from the item's top/right edge: a fraction of the dot,
    // with a small floor so the dot never hugs the border at small sizes (the
    // old fraction-only pad glued tiny dots to the corner).
    ImVec2 imin = ImGui::GetItemRectMin();
    ImVec2 imax = ImGui::GetItemRectMax();
    float pad = std::max(2.5f, dotSz * 0.3f);

    ImVec2 mn(imax.x - dotSz - pad, imin.y + pad);
    ImVec2 mx(mn.x + dotSz, mn.y + dotSz);
    int a = (int)(255 * alphaMul);
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex->Resource, mn, mx,
                 ImVec2(0, 0), ImVec2(1, 1),
                 IM_COL32(255, 255, 255, a));
}

void DrawMeMoteIndicator(float indSz, float alphaMul) {
    // /me-motes and IsTargetable are mutually exclusive (the Emote-only
    // target dot draws via DrawTargetableDot above, which never runs on a
    // /me-mote cell), so the two indicators share the same top-right anchor.
    // Sourced from the bundled (or user-overridden) UI icon "me_mote_dot.png"
    // - drop a PNG under addons/emot3/icons/ui/ to replace it, same path as
    // every other UI override (see entry.cpp's icons/ui/README.txt).
    Texture* tex = APIDefs ? APIDefs->Textures.Get("EMOT3_UI_ME_MOTE") : nullptr;
    if (!tex || !tex->Resource) return;
    if (indSz < 4.f) return;

    ImVec2 imin = ImGui::GetItemRectMin();
    ImVec2 imax = ImGui::GetItemRectMax();
    float pad = std::max(2.5f, indSz * 0.3f);

    ImVec2 mn(imax.x - indSz - pad, imin.y + pad);
    ImVec2 mx(mn.x + indSz, mn.y + indSz);
    int a = (int)(255 * alphaMul);
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex->Resource, mn, mx,
                 ImVec2(0, 0), ImVec2(1, 1),
                 IM_COL32(255, 255, 255, a));
}
