#include "MeMotes.h"
#include "JsonUtil.h"
#include "Logging.h"

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
    if (s.size() >= 4 && s[0] == '/' &&
        (s[1] == 'm' || s[1] == 'M') &&
        (s[2] == 'e' || s[2] == 'E') &&
        (s[3] == ' ' || s[3] == '\t')) {
        s.erase(0, 4);
        s = Trim(s);
    }
    return s;
}

// Sanitize one alias: trim, drop empty. (Unlike Emote.Aliases, /me-mote
// aliases are NOT slash commands — they're free-form search words, so the
// command-normalization rules don't apply.)
bool SanitizeAlias(std::string& a) {
    a = Trim(a);
    return !a.empty();
}

} // namespace

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

    bool changed = false;
    std::vector<MeMote> parsed;
    for (const auto& item : j["me_motes"]) {
        if (!item.is_object()) { changed = true; continue; }
        MeMote m;
        m.Id          = jsonutil::GetString(item, "id",       std::string());
        m.Name        = jsonutil::GetString(item, "name",     std::string());
        m.IconPath    = jsonutil::GetString(item, "icon",     std::string());
        m.TextDefault = jsonutil::GetString(item, "text",     std::string());
        m.TextYou     = jsonutil::GetString(item, "text_you", std::string());
        m.TextAll     = jsonutil::GetString(item, "text_all", std::string());

        // Sanitize the bodies: trim + strip an accidental leading "/me ". The
        // schema invariant is "TextDefault carries the variant text only" so
        // the send pipeline can always prepend "/me " exactly once. A user who
        // typed the full command in survives sanitization rather than getting
        // a doubled prefix on send.
        std::string normDef = StripMePrefix(m.TextDefault);
        std::string normYou = StripMePrefix(m.TextYou);
        std::string normAll = StripMePrefix(m.TextAll);
        if (normDef != m.TextDefault) { m.TextDefault = normDef; changed = true; }
        if (normYou != m.TextYou)     { m.TextYou     = normYou; changed = true; }
        if (normAll != m.TextAll)     { m.TextAll     = normAll; changed = true; }

        // Drop entries with empty Id (no way to reference them) or empty
        // TextDefault (no body to send on left-click — the You/All variants
        // alone aren't enough, since the cell-click handler always uses
        // Default).
        if (m.Id.empty() || m.TextDefault.empty()) { changed = true; continue; }

        // Optional aliases (free-form search words). Trim + drop empties +
        // dedupe. Missing key is fine.
        {
            const json& al = jsonutil::GetArray(item, "aliases");
            for (const auto& a : al) {
                if (!a.is_string()) continue;
                std::string na = a.get<std::string>();
                if (!SanitizeAlias(na)) continue;
                if (std::find(m.Aliases.begin(), m.Aliases.end(), na) == m.Aliases.end())
                    m.Aliases.push_back(std::move(na));
            }
        }

        // Dedupe by Id (last writer wins — but heal the file).
        bool dup = false;
        for (const auto& p : parsed)
            if (p.Id == m.Id) { dup = true; break; }
        if (!dup) parsed.push_back(std::move(m));
        else      changed = true;
    }

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
