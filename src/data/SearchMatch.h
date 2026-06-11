#pragma once

// Shared catalog text-match for search surfaces (the Library search box; the
// quick-send palette). Pure matching only - class/section filtering stays with
// each surface, and an alias-only hit is reported as data so the caller
// decides how to surface it (the Library formats its "matched via alias"
// tooltip line from it).
//
// Extracted from MainPanel.cpp's passes()/passesMeMote() lambdas so the
// surfaces can't drift apart on what a search hit IS. Header-only inline,
// mirroring core/StringUtil.h / data/JsonUtil.h.

#include <string>
#include "EmoteData.h"   // Emote
#include "MeMotes.h"     // MeMote
#include "StringUtil.h"  // ToLower

// One match result. aliasOnly is non-null when the entry matched ONLY via that
// alias (name and command both missed) - the confusing case, since an alias
// shows nowhere in the UI. It points into the entry's Aliases vector, so it is
// valid exactly as long as the entry is (same locking rules as the entry;
// don't stash it across frames).
struct SearchHit {
    bool hit = false;
    const std::string* aliasOnly = nullptr;
};

// needleLower must be ASCII-lowercased already (ToLower once per query, not
// per entry) and non-empty - the "is the search active at all" threshold (the
// Library's 2-char rule) stays a caller concern. Substring match, ASCII
// case-insensitive: the semantics the Library search has always had.
inline SearchHit MatchEmoteSearch(const Emote& e, const std::string& needleLower) {
    SearchHit r;
    if (ToLower(e.Name).find(needleLower)    != std::string::npos ||
        ToLower(e.Command).find(needleLower) != std::string::npos) {
        r.hit = true;
        return r;
    }
    for (const auto& al : e.Aliases) {
        if (ToLower(al).find(needleLower) != std::string::npos) {
            r.hit = true;
            r.aliasOnly = &al;
            return r;
        }
    }
    return r;
}

// /me-motes have no Command field; otherwise the same shape.
inline SearchHit MatchMeMoteSearch(const MeMote& m, const std::string& needleLower) {
    SearchHit r;
    if (ToLower(m.Name).find(needleLower) != std::string::npos) {
        r.hit = true;
        return r;
    }
    for (const auto& al : m.Aliases) {
        if (ToLower(al).find(needleLower) != std::string::npos) {
            r.hit = true;
            r.aliasOnly = &al;
            return r;
        }
    }
    return r;
}
