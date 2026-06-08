#pragma once

// Small shared string / path helpers, consolidated from copies that had
// drifted across several TUs (ASCII lowercase, whitespace trim, absolute-path
// detection). Header-only inline, mirroring data/JsonUtil.h. ASCII-only by
// design: the addon's identifiers, file paths and command stems are ASCII;
// user-facing localized text lives in UTF-8 data files, never here.

#include <cctype>
#include <string>

// ASCII-lowercase a copy.
inline std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Trim leading/trailing ASCII whitespace (space, tab, newline, CR). The
// permissive set - the name-field call sites only ever see single-line input,
// so stripping newlines too is harmless there, and it's correct for the
// paste-safe /me-mote text fields that supersede the old narrower copies.
inline std::string TrimWhitespace(std::string s) {
    auto isws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    while (!s.empty() && isws((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && isws((unsigned char)s.back()))  s.pop_back();
    return s;
}

// True if `p` is an absolute Windows path: a drive-letter root ("X:\" or
// "X:/") or a UNC root ("\\server\..."). A relative path (the icons/ folder
// convention) returns false. The single home for the check the icon path
// resolvers and SanitizeIconPath share.
inline bool IsAbsolutePath(const std::string& p) {
    return (p.size() >= 3 && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) ||
           (p.size() >= 2 && p[0] == '\\' && p[1] == '\\');
}

// Sanitize a user string into a filesystem-safe path COMPONENT (a single name,
// NOT a path with separators) valid on Windows, Linux, and Wine. Strict ASCII
// slug:
//   - ASCII alphanumerics kept (lowercased);
//   - space / '-' / '_' collapse to a single '_';
//   - everything else dropped - so '/', '.', '..', ASCII control chars, the
//     Windows-illegal set <>:"/\|?*, and all non-ASCII vanish. That makes the
//     result inherently traversal-proof, illegal-char-proof, and (being all
//     lowercase) case-collision-proof on Windows/Wine;
//   - leading/trailing '_' trimmed; empty result -> `fallback`;
//   - a reserved Windows device name (con/prn/aux/nul/com0-9/lpt0-9, reserved
//     even WITH an extension) -> prefixed with '_';
//   - capped to a safe byte length (well under the 255 filesystem limit).
// For a name that becomes a file, the caller adds the extension AFTER this.
inline std::string SanitizeFilename(const std::string& name,
                                    const char* fallback = "untitled") {
    constexpr size_t kMaxFilenameComponent = 96;  // bytes; << the 255 FS limit
    const std::string fb = (fallback && *fallback) ? std::string(fallback)
                                                   : std::string("untitled");

    std::string s;
    bool lastUnderscore = false;
    for (char c : name) {
        unsigned char u = (unsigned char)c;
        if (u < 0x80 && std::isalnum(u)) {
            s += (char)std::tolower(u);
            lastUnderscore = false;
        } else if (c == ' ' || c == '-' || c == '_') {
            if (!lastUnderscore) { s += '_'; lastUnderscore = true; }
        }
        // anything else (punctuation, control, path separators, UTF-8 >= 0x80)
        // is dropped entirely
    }
    // Trim leading/trailing '_'.
    size_t a = s.find_first_not_of('_');
    size_t b = s.find_last_not_of('_');
    s = (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
    if (s.empty()) return fb;

    // Reserved device names (our stem is already lowercase [a-z0-9_], so compare
    // the whole stem). COM/LPT are reserved with a single trailing digit 0-9.
    auto isReserved = [](const std::string& v) {
        if (v == "con" || v == "prn" || v == "aux" || v == "nul") return true;
        if (v.size() == 4 && v[3] >= '0' && v[3] <= '9' &&
            (v.compare(0, 3, "com") == 0 || v.compare(0, 3, "lpt") == 0))
            return true;
        return false;
    };
    if (isReserved(s)) s.insert(s.begin(), '_');

    if (s.size() > kMaxFilenameComponent) {
        s.resize(kMaxFilenameComponent);
        while (!s.empty() && s.back() == '_') s.pop_back();  // re-trim if the cut split
        if (s.empty()) return fb;
    }
    return s;
}

// Make `base` unique by appending _2, _3, ... until `isTaken(candidate)` is
// false. `isTaken` is a caller-supplied predicate (e.g. "this file/folder exists
// or a sibling already uses this name"). Template so it needs no <functional>.
template <typename TakenFn>
inline std::string MakeUniqueSlug(const std::string& base, TakenFn isTaken) {
    if (!isTaken(base)) return base;
    for (int n = 2; n < 100000; ++n) {
        std::string cand = base + "_" + std::to_string(n);
        if (!isTaken(cand)) return cand;
    }
    return base;  // pathological; effectively unreachable
}

// True if `s` is well-formed UTF-8 - the encoding JSON serialization (nlohmann
// json.dump THROWS on invalid UTF-8) and ImGui expect. The ANSI Win32 APIs
// (FindFirstFileA) return filenames in the system codepage (e.g. CP1252), whose
// high bytes are NOT valid UTF-8; validate before such a string reaches a JSON
// writer or is stored. Rejects overlong forms, UTF-16 surrogates, and >U+10FFFF.
inline bool IsValidUtf8(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { ++i; continue; }                 // ASCII
        size_t extra;
        unsigned char lo = 0x80, hi = 0xBF;              // 1st-continuation range
        if      ((c & 0xE0) == 0xC0) { if (c < 0xC2) return false; extra = 1; }        // 2-byte (no overlong)
        else if ((c & 0xF0) == 0xE0) { extra = 2; if (c == 0xE0) lo = 0xA0;            // 3-byte
                                                  if (c == 0xED) hi = 0x9F; }          //   (no overlong / surrogate)
        else if ((c & 0xF8) == 0xF0) { if (c > 0xF4) return false; extra = 3;          // 4-byte
                                       if (c == 0xF0) lo = 0x90;                        //   (no overlong)
                                       if (c == 0xF4) hi = 0x8F; }                      //   (<= U+10FFFF)
        else return false;                               // invalid lead byte
        if (i + extra >= n) return false;                // truncated
        for (size_t k = 1; k <= extra; ++k) {
            unsigned char cc = (unsigned char)s[i + k];
            unsigned char l = (k == 1) ? lo : 0x80;
            unsigned char h = (k == 1) ? hi : 0xBF;
            if (cc < l || cc > h) return false;
        }
        i += extra + 1;
    }
    return true;
}
