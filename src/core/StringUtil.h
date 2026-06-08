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
