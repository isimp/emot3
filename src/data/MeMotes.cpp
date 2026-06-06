#include "MeMotes.h"
#include "Globals.h"     // g_MeMotesVersion (DevStateRegistrar reads it)
#include "JsonUtil.h"
#include "Logging.h"

#ifdef EMOT3_DEVTOOLS
#include "DevStateInspector.h"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>

std::vector<MeMote> g_MeMotes;
std::mutex          g_MeMotesMutex;

using json = nlohmann::json;

void ClearMeMotes() {
    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
    g_MeMotes.clear();
}

namespace {

// Trim leading/trailing ASCII whitespace. Same pattern as the trim helper in
// Favorites.cpp (TrimName), local copy here to avoid a cross-module include
// for a 3-line function.
std::string Trim(std::string s) {
    auto issp = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!s.empty() && issp((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && issp((unsigned char)s.back()))  s.pop_back();
    return s;
}

// Strip a leading "/me " if the user accidentally pasted the full command into
// a variant-text field. TextDefault/You/All carry the variant BODY only — the
// "/me " prefix is added once at send time in SendOrFillMeMote. Idempotent
// (no-op for already-stripped strings). Case-insensitive on the "me" so
// "/ME hugs" also strips cleanly.
std::string StripMePrefix(std::string s) {
    s = Trim(s);
    // A body of exactly "/me" with no text is a nonsensical leftover — treat it
    // as empty so the empty-TextDefault drop fires instead of shipping "/me /me"
    // on send. ("/method" etc. are real words and intentionally NOT stripped.)
    if (s.size() == 3 && s[0] == '/' &&
        (s[1] == 'm' || s[1] == 'M') && (s[2] == 'e' || s[2] == 'E'))
        return std::string();
    if (s.size() >= 4 && s[0] == '/' &&
        (s[1] == 'm' || s[1] == 'M') &&
        (s[2] == 'e' || s[2] == 'E') &&
        (s[3] == ' ' || s[3] == '\t')) {
        s.erase(0, 4);
        s = Trim(s);
    }
    return s;
}

// Tokenize an alias string on whitespace + comma into `out`, trimming each
// token, dropping empties, and deduping first-wins against what's already
// there. Mirrors OptionsMeMotes' ParseAliases so a hand-edited multi-word
// alias loads in the SAME tokenized form the editor produces — no silent
// reshape on the first edit. (Unlike Emote.Aliases, /me-mote aliases are NOT
// slash commands, so no command-normalization is applied.)
void TokenizeAliases(const std::string& raw, std::vector<std::string>& out) {
    std::string tok;
    auto flush = [&] {
        std::string t = Trim(tok);
        tok.clear();
        if (t.empty()) return;
        if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(std::move(t));
    };
    for (char ch : raw) {
        if (ch == ' ' || ch == '\t' || ch == ',') flush();
        else                                       tok += ch;
    }
    flush();
}

} // namespace

std::string NormalizeMeMoteId(std::string s) {
    // Step 1: trim ASCII whitespace (same isws as Trim above, inline so the
    // lambda fits the pattern).
    auto isws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!s.empty() && isws(s.front())) s.erase(s.begin());
    while (!s.empty() && isws(s.back()))  s.pop_back();
    if (s.empty()) return s;

    // Step 2: build the normalized form. Lowercase ASCII letters; keep digits
    // and underscores as-is; collapse anything else (spaces, punctuation,
    // accented bytes >=0x80) into single underscores; drop leading runs of
    // underscores at the start. Trailing underscores trimmed at the end. The
    // result lands in [a-z0-9_] which is JSON-key-safe and filename-safe.
    std::string out;
    out.reserve(s.size());
    bool lastUnderscore = true;  // start true: suppress leading underscores
    for (unsigned char uc : s) {
        char nc;
        if (uc >= 'A' && uc <= 'Z')                                   nc = (char)(uc + 32);
        else if ((uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9') || uc == '_') nc = (char)uc;
        else                                                          nc = '_';
        if (nc == '_') {
            if (lastUnderscore) continue;       // collapse / suppress leading
            lastUnderscore = true;
        } else {
            lastUnderscore = false;
        }
        out += nc;
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

bool LoadMeMotesJson(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_DEBUG("me_motes.json not present at %s", path.c_str());
        return false;
    }

    json j;
    try {
        f >> j;
    } catch (const json::parse_error& e) {
        LOG_WARNING("me_motes.json parse error at byte %zu: %s",
                    (size_t)e.byte, e.what());
        return false;
    }

    if (!j.is_object() || !j.contains("me_motes") || !j["me_motes"].is_array()) {
        LOG_WARNING("me_motes.json has no \"me_motes\" array");
        return false;
    }

    // Each correction logs at WARNING (mirroring SanitizeSettings' policy):
    // the file is healed + re-saved on load, so anything we fix up is
    // recorded so the user can see what changed. `changed` drives the
    // re-save.
    bool changed = false;
    std::vector<MeMote> parsed;
    int skippedNonObj = 0, droppedNoId = 0, droppedNoText = 0, droppedDup = 0;
    for (const auto& item : j["me_motes"]) {
        if (!item.is_object()) { ++skippedNonObj; changed = true; continue; }
        MeMote m;
        m.Id          = jsonutil::GetString(item, "id",       std::string());
        m.Name        = jsonutil::GetString(item, "name",     std::string());
        m.IconPath    = jsonutil::GetString(item, "icon",     std::string());
        m.TextDefault = jsonutil::GetString(item, "text",     std::string());
        m.TextYou     = jsonutil::GetString(item, "text_you", std::string());
        m.TextAll     = jsonutil::GetString(item, "text_all", std::string());

        // Normalize the Id at every ingress (mirrors NormalizeEmoteCommand's
        // discipline — every code path that introduces an Id runs the same
        // canonicalization, so on-disk values match what the editor would
        // produce).
        std::string normId = NormalizeMeMoteId(m.Id);
        if (normId != m.Id) {
            LOG_WARNING("me_motes: id '%s' normalized to '%s'",
                        m.Id.c_str(), normId.c_str());
            m.Id = normId; changed = true;
        }

        // Sanitize the bodies: trim + strip an accidental leading "/me ". The
        // schema invariant is "TextDefault carries the variant text only" so
        // the send pipeline can always prepend "/me " exactly once. A user who
        // typed the full command in survives sanitization rather than getting
        // a doubled prefix on send.
        auto trySanitizeBody = [&](std::string& field, const char* label) {
            std::string norm = StripMePrefix(field);
            if (norm != field) {
                LOG_WARNING("me_motes[%s].%s: stripped leading '/me ' / trimmed whitespace",
                            m.Id.c_str(), label);
                field = std::move(norm); changed = true;
            }
        };
        trySanitizeBody(m.TextDefault, "text");
        trySanitizeBody(m.TextYou,     "text_you");
        trySanitizeBody(m.TextAll,     "text_all");

        // Trim the Name. The editor always stores a trimmed Name, so heal the
        // on-disk form to match — a whitespace-only Name would otherwise render
        // as a blank label and never canonicalize. (Empty-after-trim is kept,
        // not dropped: it's a half-filled entry the user can finish in the
        // editor, same as an Id typed with no Name yet.)
        {
            std::string trimmedName = Trim(m.Name);
            if (trimmedName != m.Name) {
                LOG_WARNING("me_motes[%s].name: trimmed whitespace", m.Id.c_str());
                m.Name = std::move(trimmedName); changed = true;
            }
        }

        // Drop entries with empty Id (no way to reference them) or empty
        // TextDefault (no body to send on left-click — the You/All variants
        // alone aren't enough, since the cell-click handler always uses
        // Default).
        if (m.Id.empty())          { ++droppedNoId;   changed = true; continue; }
        if (m.TextDefault.empty()) {
            LOG_WARNING("me_motes[%s]: dropped (text body is empty)", m.Id.c_str());
            ++droppedNoText; changed = true; continue;
        }

        // Optional aliases (free-form search words). Each JSON string is
        // tokenized on whitespace + comma (so a hand-edited "foo bar" lands as
        // two aliases, matching the editor), trimmed, empties dropped, deduped.
        // Missing key is fine.
        {
            const json& al = jsonutil::GetArray(item, "aliases");
            for (const auto& a : al) {
                if (!a.is_string()) continue;
                TokenizeAliases(a.get<std::string>(), m.Aliases);
            }
        }

        // Dedupe by Id (first-wins keeps load deterministic across re-saves).
        bool dup = false;
        for (const auto& p : parsed)
            if (p.Id == m.Id) { dup = true; break; }
        if (!dup) parsed.push_back(std::move(m));
        else {
            LOG_WARNING("me_motes[%s]: dropped (duplicate id)", m.Id.c_str());
            ++droppedDup; changed = true;
        }
    }
    if (skippedNonObj) LOG_WARNING("me_motes: skipped %d non-object entr%s",
                                   skippedNonObj, skippedNonObj == 1 ? "y" : "ies");
    if (droppedNoId)   LOG_WARNING("me_motes: dropped %d entr%s with empty id",
                                   droppedNoId, droppedNoId == 1 ? "y" : "ies");
    if (droppedNoText) LOG_WARNING("me_motes: dropped %d entr%s with empty text body",
                                   droppedNoText, droppedNoText == 1 ? "y" : "ies");
    if (droppedDup)    LOG_WARNING("me_motes: dropped %d duplicate-id entr%s",
                                   droppedDup, droppedDup == 1 ? "y" : "ies");

    int count = 0;
    {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        g_MeMotes = std::move(parsed);
        count = (int)g_MeMotes.size();
    }
    LOG_INFO("Loaded %d /me-mote(s) from me_motes.json", count);

    // Heal the file in place when sanitization corrected something — same
    // self-contained pattern as emotes.json (single loader, single writer).
    if (changed && !path.empty()) {
        LOG_INFO("me_motes.json: rewriting to heal sanitized content");
        SaveMeMotesJson(path);
    }
    return true;
}

void SaveMeMotesJson(const std::string& path) {
    std::ofstream f(path);
    if (!f.is_open()) {
        LOG_WARNING("Could not open %s for writing", path.c_str());
        return;
    }

    // Hand-rolled writer (mirrors SaveEmotesJson) so the on-disk layout is
    // controlled: per-/me-mote keys in a logical order, aliases inline.
    auto quoted = [](const std::string& v) { return json(v).dump(); };

    f << "{\n";
    f << "  \"version\": 1,\n";
    f << "  \"me_motes\": [";

    {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        for (size_t i = 0; i < g_MeMotes.size(); ++i) {
            const MeMote& m = g_MeMotes[i];
            if (i) f << ",";
            f << "\n    {\n";
            f << "      \"id\": "       << quoted(m.Id)          << ",\n";
            f << "      \"name\": "     << quoted(m.Name)        << ",\n";
            f << "      \"icon\": "     << quoted(m.IconPath)    << ",\n";
            f << "      \"text\": "     << quoted(m.TextDefault) << ",\n";
            f << "      \"text_you\": " << quoted(m.TextYou)     << ",\n";
            f << "      \"text_all\": " << quoted(m.TextAll);
            // aliases omitted entirely when empty (matches SaveEmotesJson);
            // when present, written inline so the file stays compact.
            if (!m.Aliases.empty()) {
                f << ",\n      \"aliases\": [";
                for (size_t k = 0; k < m.Aliases.size(); ++k) {
                    if (k) f << ", ";
                    f << quoted(m.Aliases[k]);
                }
                f << "]";
            }
            f << "\n    }";
        }
        if (!g_MeMotes.empty()) f << "\n  ";
    }

    f << "]\n";
    f << "}\n";
}

const MeMote* FindMeMote(const std::string& id) {
    for (const auto& m : g_MeMotes)
        if (m.Id == id) return &m;
    return nullptr;
}

#ifdef EMOT3_DEVTOOLS
// Runtime state inspector section — surfaces the live /me-mote catalog state
// (count + monotonic version + on-disk path) alongside the Emote catalog
// section so a dev can spot when the file failed to load (count stays 0) or
// when edits aren't propagating (version doesn't change). Layer-2 standard;
// no-op in non-devtools builds. See DevStateInspector.h.
static DevStateRegistrar s_meMotesState("/me-motes catalog", [] {
    DevStateRow("count",   "%zu", g_MeMotes.size());
    DevStateRow("version", "%llu",
                (unsigned long long)g_MeMotesVersion.load(std::memory_order_relaxed));
    DevStateRow("path",    "%s",  g_MeMotesJsonPath.empty()
                                  ? "(unset)" : g_MeMotesJsonPath.c_str());
});
#endif
