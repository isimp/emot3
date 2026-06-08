#include "MeMotes.h"
#include "Globals.h"     // g_MeMotesVersion (DevStateRegistrar reads it)
#include "IconPath.h"    // SanitizeIconPath (IconPath ingress heal) - data-layer, no ui/
#include "JsonUtil.h"
#include "Logging.h"
#include "StringUtil.h"  // TrimWhitespace (shared helper)
#include "Resources.h"   // kMeMoteData bundled seed table
#include "Favorites.h"   // RemoveRefFromCategories + EFavoriteRefType (DeleteMeMote cascade)
#include "Profiling.h"   // PROFILE_SCOPE (no-op without EMOT3_DEVTOOLS) - "save.memotes"

#ifdef EMOT3_DEVTOOLS
#include "DevStateInspector.h"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <unordered_set>

std::vector<MeMote> g_MeMotes;
std::mutex          g_MeMotesMutex;
std::string         g_MeMoteLanguage;

using json = nlohmann::json;

void ClearMeMotes() {
    std::lock_guard<std::mutex> lk(g_MeMotesMutex);
    g_MeMotes.clear();
}

namespace {

// Strip a leading "/me " if the user accidentally pasted the full command into
// a variant-text field. TextDefault/You/All carry the variant BODY only — the
// "/me " prefix is added once at send time in SendOrFillMeMote. Idempotent
// (no-op for already-stripped strings). Case-insensitive on the "me" so
// "/ME hugs" also strips cleanly.
std::string StripMePrefix(std::string s) {
    s = TrimWhitespace(s);
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
        s = TrimWhitespace(s);
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
        std::string t = TrimWhitespace(tok);
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

// --- Bundled /me-mote seed table -----------------------------------------
//
// Lives in resources/me_mote_data/me_motes_i18n.json, bundled into the DLL,
// so the per-language sample bodies (Name, aliases, text_default/you/all)
// are data and not code. Same shape philosophy as the bundled
// emote-localization table in EmoteData.cpp: parse once on first use, cache
// in a file-static, look up by id + language with per-entry English
// fallback. The seeder writes entries into g_MeMotes that aren't already
// present (idempotent by Id) so a user-edited sample stays edited and a
// deleted sample reappears on Restore.

struct SeedLangEntry {                 // one language's data for a sample
    std::string              name;
    std::string              aliases;  // raw whitespace-separated form (TokenizeAliases applies)
    std::string              textDefault;
    std::string              textYou;
    std::string              textAll;
};

struct SeedEntry {                     // one /me-mote in the bundled table
    std::string id;
    std::map<std::string, SeedLangEntry> byLang;  // lang code -> data
};

bool                     s_seedLoaded = false;
std::vector<SeedEntry>   s_seed;
std::vector<std::string> s_seedLangs;

void LoadBundledMeMotesTable() {
    if (s_seedLoaded) return;
    s_seedLoaded = true;  // even on failure — don't retry every call

    const void* data = nullptr; size_t size = 0;
    if (!TryLoadBundledData(kMeMoteData, kMeMoteDataCount, "me_motes_i18n",
                            data, size)) {
        // Non-critical: the user can still hand-create /me-motes via the
        // editor, and a missing bundle just means no first-run samples.
        // Logged at WARNING so a packaging slip is visible without scaring
        // users who never run with the bundle (Distribution always ships it).
        LOG_WARNING("me_motes_i18n bundled table missing — no /me-mote samples to seed");
        return;
    }
    json j;
    try {
        j = json::parse(static_cast<const char*>(data),
                        static_cast<const char*>(data) + size);
    } catch (const json::parse_error& e) {
        LOG_WARNING("me_motes_i18n parse error at byte %zu: %s",
                    (size_t)e.byte, e.what());
        return;
    }
    if (j.contains("languages") && j["languages"].is_array()) {
        for (const auto& l : j["languages"])
            if (l.is_string()) s_seedLangs.push_back(l.get<std::string>());
    }
    if (!j.contains("me_motes") || !j["me_motes"].is_array()) {
        LOG_WARNING("me_motes_i18n has no \"me_motes\" array");
        return;
    }
    // Bundle is author-controlled (lives in resources/me_mote_data/), so
    // dup detection here is belt-and-braces — but a future edit slipping
    // a duplicate id past review would silently double-seed otherwise.
    // Mirrors the user-loader's droppedDup pattern; first-wins keeps the
    // accepted entry deterministic.
    std::unordered_set<std::string> seenIds;
    for (const auto& item : j["me_motes"]) {
        if (!item.is_object()) continue;
        SeedEntry e;
        e.id = jsonutil::GetString(item, "id", std::string());
        // Normalize once at ingress (matches the LoadMeMotesJson discipline)
        // so a hand-edited bundle id with mixed case still matches the
        // catalog's id format.
        e.id = NormalizeMeMoteId(e.id);
        if (e.id.empty()) continue;
        if (!seenIds.insert(e.id).second) {
            LOG_WARNING("me_motes_i18n: duplicate id '%s' skipped (first-wins)",
                        e.id.c_str());
            continue;
        }
        if (!item.contains("by_lang") || !item["by_lang"].is_object()) continue;
        const json& byLang = item["by_lang"];
        for (auto it = byLang.begin(); it != byLang.end(); ++it) {
            if (!it.value().is_object()) continue;
            const std::string& code = it.key();
            SeedLangEntry le;
            le.name        = jsonutil::GetString(it.value(), "name",         std::string());
            le.aliases     = jsonutil::GetString(it.value(), "aliases",      std::string());
            le.textDefault = jsonutil::GetString(it.value(), "text_default", std::string());
            le.textYou     = jsonutil::GetString(it.value(), "text_you",     std::string());
            le.textAll     = jsonutil::GetString(it.value(), "text_all",     std::string());
            // text_default is required per the runtime invariant (left-click
            // sends it). An entry missing it is unrenderable, so skip it
            // here rather than seeding a broken sample.
            if (le.textDefault.empty()) continue;
            e.byLang[code] = std::move(le);
        }
        if (e.byLang.empty()) continue;
        s_seed.push_back(std::move(e));
    }
    LOG_INFO("me_motes_i18n: %d /me-mote sample(s), %d language(s)",
             (int)s_seed.size(), (int)s_seedLangs.size());
}

// Resolve one entry to the chosen language with per-entry English fallback:
// if the entry has no data for `lang`, return its English data; if it has
// neither, return nullptr (skipped at seed time). Matches the EmoteData
// fallback rule — keeps FR/ES players functional with EN samples until
// translations land.
const SeedLangEntry* ResolveSeedForLang(const SeedEntry& e, const std::string& lang) {
    auto it = e.byLang.find(lang);
    if (it != e.byLang.end()) return &it->second;
    auto en = e.byLang.find("en");
    if (en != e.byLang.end()) return &en->second;
    return nullptr;
}

} // namespace

std::vector<std::string> AvailableMeMoteLanguages() {
    LoadBundledMeMotesTable();
    return s_seedLangs;
}

std::string ClampMeMoteLanguage(const std::string& lang) {
    if (lang.empty()) return std::string("en");
    LoadBundledMeMotesTable();
    for (const auto& l : s_seedLangs) if (l == lang) return lang;
    return std::string("en");
}

size_t MeMotesBundledSeedCount() {
    LoadBundledMeMotesTable();
    return s_seed.size();
}

size_t MeMotesBundledSeedBytes() {
    // Approximate heap bytes the bundled seed table holds: per SeedEntry,
    // the id string + every per-language { name, aliases, text_default,
    // text_you, text_all } string. Matches the string_heap() shape
    // MemoryMonitor uses for the g_MeMotes catalog row — caller-side this
    // is "out-of-line bytes the field owns", SSO-sized strings count as 0.
    LoadBundledMeMotesTable();
    auto heap = [](const std::string& s) -> size_t {
        // 23-byte libc++ / 16-byte MSVC short-string boundary varies, but
        // counting .capacity() when it exceeds 16 is the same heuristic
        // MemoryMonitor's string_heap uses; consistency matters more than
        // a perfectly tight estimate.
        return s.capacity() > 16 ? s.capacity() : 0;
    };
    size_t bytes = 0;
    for (const auto& e : s_seed) {
        bytes += heap(e.id);
        bytes += e.byLang.size() * sizeof(std::pair<const std::string, SeedLangEntry>);
        for (const auto& kv : e.byLang) {
            bytes += heap(kv.first);
            bytes += heap(kv.second.name);
            bytes += heap(kv.second.aliases);
            bytes += heap(kv.second.textDefault);
            bytes += heap(kv.second.textYou);
            bytes += heap(kv.second.textAll);
        }
    }
    bytes += s_seedLangs.capacity() * sizeof(std::string);
    for (const auto& l : s_seedLangs) bytes += heap(l);
    return bytes;
}

int SeedBundledMeMotes(const std::string& lang) {
    LoadBundledMeMotesTable();
    if (s_seed.empty()) return 0;
    int added = 0, skipped = 0;
    {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        // Build a quick id set for the existence check — N is small (a dozen
        // samples, maybe a few user-authored ones) but the unordered_set
        // keeps the inner loop O(1) instead of O(N^2).
        std::unordered_set<std::string> present;
        present.reserve(g_MeMotes.size());
        for (const auto& m : g_MeMotes) present.insert(m.Id);

        for (const auto& entry : s_seed) {
            if (present.count(entry.id)) { ++skipped; continue; }
            const SeedLangEntry* src = ResolveSeedForLang(entry, lang);
            if (!src) { ++skipped; continue; }   // no EN either — skip
            MeMote m;
            m.Id          = entry.id;
            m.Name        = src->name;
            m.TextDefault = src->textDefault;
            m.TextYou     = src->textYou;
            m.TextAll     = src->textAll;
            // Tokenize the aliases string at ingress (matches the editor's
            // ParseAliases output form, so the on-disk file looks the same
            // whether the entry came from a seed or a hand-add).
            TokenizeAliases(src->aliases, m.Aliases);
            g_MeMotes.push_back(std::move(m));
            present.insert(entry.id);
            ++added;
        }
    }
    LOG_INFO("Bundled /me-motes: %d added, %d skipped (id already present or no data), language '%s'",
             added, skipped, lang.c_str());
    return added;
}

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

    // Language field — trim whitespace first (so a hand-edited " de " or
    // "de\n" doesn't fall through the exact-match Clamp and silently heal
    // to "en"), then clamp to AvailableMeMoteLanguages() so any value the
    // bundle doesn't carry still heals to a working choice. Parallels
    // emotes.json's "emote_language" field. One WARNING covers both
    // corrections (trim + clamp) so the heal log surfaces exactly what
    // happened.
    {
        std::string rawLang = jsonutil::GetString(j, "language", std::string());
        std::string trimmedLang = TrimWhitespace(rawLang);
        std::string normLang = ClampMeMoteLanguage(trimmedLang);
        if (!rawLang.empty() && normLang != rawLang) {
            LOG_WARNING("me_motes.json: language '%s' normalized to '%s' (trim + bundle clamp)",
                        rawLang.c_str(), normLang.c_str());
        }
        if (normLang != rawLang) changed = true;
        g_MeMoteLanguage = normLang;
    }

    std::vector<MeMote> parsed;
    int skippedNonObj = 0, droppedNoId = 0, droppedNoText = 0, droppedDup = 0;
    for (const auto& item : j["me_motes"]) {
        if (!item.is_object()) { ++skippedNonObj; changed = true; continue; }
        MeMote m;
        m.Id          = jsonutil::GetString(item, "id",       std::string());
        m.Name        = jsonutil::GetString(item, "name",     std::string());
        // Sanitize IconPath at ingress: locks the value to addons/emot3/icons
        // (subfolders allowed; ui/ subfolder excluded) and passes bundled refs
        // through untouched. A hand-edit pointing outside the icons folder
        // heals to empty + logs the original so the user sees what we dropped.
        {
            std::string rawIcon = jsonutil::GetString(item, "icon", std::string());
            bool iconChanged = false;
            m.IconPath = SanitizeIconPath(rawIcon, &iconChanged);
            if (iconChanged) {
                LOG_WARNING("me_motes[%s].icon: rejected '%s' (must live under "
                            "addons/emot3/icons; bundled: refs OK)",
                            m.Id.c_str(), rawIcon.c_str());
                changed = true;
            }
        }
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
            std::string trimmedName = TrimWhitespace(m.Name);
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

void DeleteMeMote(const std::string& id) {
    std::string nameForLog;
    {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        auto it = std::find_if(g_MeMotes.begin(), g_MeMotes.end(),
                               [&](const MeMote& m){ return m.Id == id; });
        if (it != g_MeMotes.end()) { nameForLog = it->Name; g_MeMotes.erase(it); }
    }
    LOG_DEBUG("/me-mote deleted: id=%s (name='%s')", id.c_str(), nameForLog.c_str());
    RemoveRefFromCategories(EFavoriteRefType::MeMote, id);   // favorites cascade
    MarkMeMotesDirty();
    if (!g_MeMotesJsonPath.empty()) SaveMeMotesJson(g_MeMotesJsonPath);
}

void SaveMeMotesJson(const std::string& path) {
    PROFILE_SCOPE("save.memotes");  // dev perf overlay - /me-mote JSON serialize + write
    std::ofstream f(path);
    if (!f.is_open()) {
        LOG_WARNING("Could not open %s for writing", path.c_str());
        return;
    }

    // Hand-rolled writer (mirrors SaveEmotesJson) so the on-disk layout is
    // controlled: per-/me-mote keys in a logical order, aliases inline.
    // error_handler_t::replace: invalid UTF-8 (e.g. an ANSI-codepage filename in
    // IconPath) becomes U+FFFD instead of THROWING and crashing the host.
    auto quoted = [](const std::string& v) {
        return json(v).dump(-1, ' ', false, json::error_handler_t::replace);
    };

    f << "{\n";
    f << "  \"version\": 1,\n";
    f << "  \"language\": "
      << quoted(g_MeMoteLanguage.empty() ? std::string("en") : g_MeMoteLanguage)
      << ",\n";
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
//
// Also surfaces the bundled-seed side: the seed table parses lazily, so the
// LoadBundledMeMotesTable() call here forces it once if it hasn't loaded
// (cheap on subsequent registrations since s_seedLoaded short-circuits).
static DevStateRegistrar s_meMotesState(DevStateCat::Content, "/me-motes catalog", [] {
    DevStateRow("count",   "%zu", g_MeMotes.size());
    DevStateRow("version", "%llu",
                (unsigned long long)g_MeMotesVersion.load(std::memory_order_relaxed));
    DevStateRow("path",    "%s",  g_MeMotesJsonPath.empty()
                                  ? "(unset)" : g_MeMotesJsonPath.c_str());
    DevStateRow("language","%s",  g_MeMoteLanguage.empty()
                                  ? "(unset)" : g_MeMoteLanguage.c_str());
    LoadBundledMeMotesTable();
    DevStateRow("bundled samples", "%zu", s_seed.size());
    {
        // Comma-join s_seedLangs for a one-line read. Bundle ships en + de
        // today; keeping this dynamic so a future translation drop shows up
        // without a code edit here.
        std::string langs;
        for (size_t i = 0; i < s_seedLangs.size(); ++i) {
            if (i) langs += ", ";
            langs += s_seedLangs[i];
        }
        DevStateRow("bundle languages", "%s",
                    langs.empty() ? "(none)" : langs.c_str());
    }
});
#endif
