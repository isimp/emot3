#include "IconPath.h"

#include "Resources.h"   // BundledIcon, kOfficialIcons/..., LookupBundledResource
#include "StringUtil.h"  // IsAbsolutePath

#include <cctype>
#include <string>
#include <utility>

// --- "bundled:" scheme helpers (see IconPath.h) --------------------------
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

    // 1b. Reject non-UTF-8 (e.g. an ANSI-codepage filename from a Win32 file
    //     enumeration): storing it would make the JSON writer emit replacement
    //     chars (and it would never resolve), so drop it -> the resolution chain
    //     falls back to the default icon. Keeps invalid UTF-8 out of the catalog.
    if (!IsValidUtf8(raw)) return mark(std::string());

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
    bool isAbs = IsAbsolutePath(s);
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
