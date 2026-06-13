#include "EmoteData.h"
#include "IconPath.h"    // SanitizeIconPath (IconPath ingress heal) - data-layer, no ui/
#include "JsonUtil.h"
#include "StringUtil.h"  // TruncateUtf8 / kMaxNameBytes (name cap at load)
#include "AtomicFile.h"  // AtomicWriteFile (crash-safe save - no live-file truncation)
#include "Logging.h"
#include "Resources.h"   // kEmoteData bundled localization table
#include "Settings.h"    // g_Settings.ManuallyUnlocked (DeleteEmote cascade)
#include "SaveScheduler.h"  // RequestSave (debounced, off-thread catalog/settings writes)
#include "Favorites.h"   // RemoveEmoteFromCategories (DeleteEmote cascade)
#include "Globals.h"     // g_*Path + MarkEmotesDirty (DeleteEmote)
#include "Profiling.h"   // PROFILE_SCOPE (no-op without EMOT3_DEVTOOLS) - "save.catalog"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <map>

std::vector<Emote> g_Emotes;
std::mutex         g_EmotesMutex;
std::string        g_EmoteLanguage;

using json = nlohmann::json;

void ClearEmotes() {
    std::lock_guard<std::mutex> lk(g_EmotesMutex);
    g_Emotes.clear();
}

// --- Bundled emote-localization table -----------------------------------
//
// The seed list used to be a hardcoded SEED_EMOTES[] array. It now lives
// in resources/emote_data/emotes_i18n.json, bundled into the DLL, so the
// command/name data for all four GW2 client languages is data, not code
// (and so the only non-ASCII text sits in a UTF-8 file, never in source
// literals). Parsed lazily on first use and cached for the addon's life.

namespace {

struct LangEntry {              // one language's data for a bundled emote
    std::string              command;
    std::string              name;     // may be empty (derive from command)
    std::vector<std::string> aliases;  // alternate commands (GW2 wiki variants)
};

struct LocaleEntry {            // one emote in the bundled table
    std::string id;
    bool        isCore     = false;
    bool        targetable = false;
    bool        madKing    = false;   // "Your Mad King Says..." event set
    std::map<std::string, LangEntry> byLang;  // lang code -> data
};

bool                     s_tableLoaded = false;
std::vector<LocaleEntry> s_table;
std::vector<std::string> s_tableLangs;

// Title-case a command stem for use as a derived display name when a
// language carries a command but no curated name: "/danse" -> "Danse".
// Only touches the first character; multi-word concatenated commands
// stay single-token (acceptable for the FR/ES "prepared" names).
std::string DeriveName(const std::string& command) {
    std::string s = command;
    if (!s.empty() && s.front() == '/') s.erase(0, 1);
    if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}

void EnsureTableLoaded() {
    if (s_tableLoaded) return;
    s_tableLoaded = true;  // even on failure - don't retry every call

    const void* data = nullptr; size_t size = 0;
    if (!TryLoadBundledData(kEmoteData, kEmoteDataCount, "emotes_i18n",
                            data, size)) {
        LOG_CRITICAL("emotes_i18n bundled table missing - no emotes to seed");
        return;
    }
    json j;
    try {
        j = json::parse(static_cast<const char*>(data),
                        static_cast<const char*>(data) + size);
    } catch (const json::parse_error& e) {
        LOG_CRITICAL("emotes_i18n parse error at byte %zu: %s",
                     (size_t)e.byte, e.what());
        return;
    }
    if (j.contains("languages") && j["languages"].is_array()) {
        for (const auto& l : j["languages"])
            if (l.is_string()) s_tableLangs.push_back(l.get<std::string>());
    }
    if (!j.contains("emotes") || !j["emotes"].is_array()) {
        LOG_CRITICAL("emotes_i18n has no \"emotes\" array");
        return;
    }
    for (const auto& item : j["emotes"]) {
        if (!item.is_object()) continue;
        LocaleEntry e;
        e.id         = jsonutil::GetString(item, "id", std::string());
        e.isCore     = jsonutil::GetBool(item, "is_core", false);
        e.targetable = jsonutil::GetBool(item, "targetable", false);
        e.madKing    = jsonutil::GetBool(item, "mad_king", false);
        if (e.id.empty()) continue;
        for (auto it = item.begin(); it != item.end(); ++it) {
            if (!it.value().is_object()) continue;  // skip id/flags scalars
            const std::string& code = it.key();
            LangEntry le;
            le.command = jsonutil::GetString(it.value(), "command", std::string());
            le.name    = jsonutil::GetString(it.value(), "name",    std::string());
            if (le.command.empty()) continue;
            const json& al = jsonutil::GetArray(it.value(), "aliases");
            for (const auto& a : al)
                if (a.is_string()) le.aliases.push_back(a.get<std::string>());
            e.byLang[code] = std::move(le);
        }
        s_table.push_back(std::move(e));
    }
    LOG_INFO("emotes_i18n: %d emote(s), %d language(s)",
             (int)s_table.size(), (int)s_tableLangs.size());
}

// Resolve (command, name) for `entry` in `lang`, applying the fallback
// rule: no data for lang -> use English entirely; command but no name ->
// derive the name from the command stem.
// Resolve a language's data with the fallback rule: no data for lang -> use
// English entirely; command but no name -> derive name from the command stem.
// Aliases follow the same fallback (the chosen language's, or English's).
LangEntry ResolveForLang(const LocaleEntry& entry, const std::string& lang) {
    const LangEntry* src = nullptr;
    auto it = entry.byLang.find(lang);
    if (it != entry.byLang.end()) src = &it->second;
    else { auto en = entry.byLang.find("en"); if (en != entry.byLang.end()) src = &en->second; }

    LangEntry out;
    if (!src) {  // last resort: no data at all
        out.command = "/" + entry.id;
        out.name    = DeriveName(entry.id);
        return out;
    }
    out.command = src->command;
    out.name    = src->name.empty() ? DeriveName(src->command) : src->name;
    out.aliases = src->aliases;
    return out;
}

// Build a catalog Emote from a bundled table entry, resolving the localized
// command/name for `lang` (English fallback) and folding in `lang`'s aliases
// plus, when given, the secondary language's command + aliases. Shared by the
// full reseed (SeedDefaultEmotes) and the id-targeted add (AddBundledEmotesByIds)
// so the two can't drift in how they construct an emote.
Emote BuildSeedEmote(const LocaleEntry& entry, const std::string& lang,
                     const std::string& secondaryLang) {
    LangEntry cn = ResolveForLang(entry, lang);
    Emote e;
    e.Id           = entry.id;
    e.Command      = cn.command;
    e.Name         = cn.name;
    e.IsCore       = entry.isCore;
    e.IsTargetable = entry.targetable;
    e.IsMadKing    = entry.madKing;
    // Seed aliases (normalized like Command; drop empties / the primary
    // command / duplicates).
    auto addAlias = [&](const std::string& raw) {
        std::string na = NormalizeEmoteCommand(raw);
        if (na.empty() || na == e.Command) return;
        if (std::find(e.Aliases.begin(), e.Aliases.end(), na) == e.Aliases.end())
            e.Aliases.push_back(na);
    };
    for (const auto& a : cn.aliases) addAlias(a);
    // Optional secondary language: fold its command + aliases in as aliases
    // (only when the entry explicitly has that language - no English fallback,
    // so English-only emotes don't gain stray ones).
    if (!secondaryLang.empty() && secondaryLang != lang) {
        auto it = entry.byLang.find(secondaryLang);
        if (it != entry.byLang.end()) {
            addAlias(it->second.command);
            for (const auto& a : it->second.aliases) addAlias(a);
        }
    }
    return e;
}

} // namespace

std::vector<std::string> AvailableEmoteLanguages() {
    EnsureTableLoaded();
    return s_tableLangs;
}

std::vector<std::string> AllBundledEmoteIds() {
    EnsureTableLoaded();
    std::vector<std::string> ids;
    ids.reserve(s_table.size());
    for (const auto& entry : s_table) ids.push_back(entry.id);
    return ids;
}

std::vector<std::string> ComputeNewBundledEmotes(const std::vector<std::string>& known) {
    // "New" = present in the bundled table but NOT in the caller's snapshot of
    // previously-known bundled ids. We further drop any id already in the live
    // catalog: those need no "add" offer (the user already has them). A bundled
    // emote the user deliberately deleted is still in `known` (snapshots only
    // ever grow to the full bundle), so it never resurfaces here. See the
    // notifier section in emot3.md.
    std::unordered_set<std::string> knownSet(known.begin(), known.end());
    std::unordered_set<std::string> inCatalog;
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        for (const auto& e : g_Emotes) inCatalog.insert(e.Id);
    }
    std::vector<std::string> out;
    for (const auto& id : AllBundledEmoteIds()) {
        if (knownSet.count(id)) continue;       // already seen in a prior version
        if (inCatalog.count(id)) continue;      // already present, nothing to add
        out.push_back(id);
    }
    return out;
}

int SeedDefaultEmotes(const std::string& lang, const std::string& secondaryLang) {
    EnsureTableLoaded();
    int added = 0, skipped = 0;
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        for (const auto& entry : s_table) {
            bool exists = false;
            for (const auto& e : g_Emotes)
                if (e.Id == entry.id) { exists = true; break; }
            // Already in the catalog (matched by language-independent id):
            // leave it untouched, don't re-add or duplicate. Counted so the
            // log shows how many were kept vs added.
            if (exists) { ++skipped; continue; }
            g_Emotes.push_back(BuildSeedEmote(entry, lang, secondaryLang));
            ++added;
        }
    }
    if (!lang.empty()) g_EmoteLanguage = lang;
    LOG_INFO("Built-in emotes: %d added, %d skipped (id already present), language '%s'",
             added, skipped, lang.c_str());
    return added;
}

int AddBundledEmotesByIds(const std::vector<std::string>& ids,
                          const std::string& lang,
                          const std::string& secondaryLang) {
    EnsureTableLoaded();
    if (ids.empty()) return 0;
    std::unordered_set<std::string> want(ids.begin(), ids.end());
    int added = 0;
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        std::unordered_set<std::string> present;
        for (const auto& e : g_Emotes) present.insert(e.Id);
        for (const auto& entry : s_table) {
            if (!want.count(entry.id)) continue;     // only the requested ids
            if (present.count(entry.id)) continue;   // never duplicate
            g_Emotes.push_back(BuildSeedEmote(entry, lang, secondaryLang));
            ++added;
        }
    }
    // Unlike SeedDefaultEmotes this is a targeted top-up (the notifier's "add the
    // new ones"), so it does NOT change g_EmoteLanguage - the catalog's language
    // preference is unrelated to which subset got added.
    LOG_INFO("Built-in emotes: %d added by id (notifier top-up), language '%s'",
             added, lang.c_str());
    return added;
}

// --- emotes.json (schema v1) --------------------------------------------
//
// { "version": 1, "emote_language": "de",
//   "emotes": [ { "id": "bow", "command": "/verbeugen", "name": "Verbeugen",
//                 "is_core": true, "targetable": true, "mad_king": true,
//                 "icon": "" }, ... ] }
//
// id is the stable key; command/name are localized. Tolerant of missing
// keys. No migration: an entry without an "id" is dropped (the catalog keys on
// id), matching the project's "small userbase, reset on bump" policy.

std::string NormalizeEmoteCommand(std::string command) {
    // Trim spaces/tabs (inline so this stays free of the Favorites dependency).
    auto isws = [](char c) { return c == ' ' || c == '\t'; };
    while (!command.empty() && isws(command.front())) command.erase(command.begin());
    while (!command.empty() && isws(command.back()))  command.pop_back();
    if (command.empty()) return command;
    if (command.front() != '/') command.insert(command.begin(), '/');
    // Per-byte tolower: ASCII letters fold; UTF-8 lead/continuation bytes
    // (>=0x80) pass through unchanged in the C locale, so accented commands
    // like "/grübeln" survive intact. Matches the Options add-path behaviour.
    std::transform(command.begin(), command.end(), command.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return command;
}

std::string NormalizeUnlockKey(std::string s) {
    auto isws = [](char c) { return c == ' ' || c == '\t'; };
    while (!s.empty() && isws(s.front())) s.erase(s.begin());
    while (!s.empty() && isws(s.back()))  s.pop_back();
    if (!s.empty() && s.front() == '/') s.erase(s.begin());
    // Per-byte tolower (ASCII folds; UTF-8 bytes >=0x80 pass through), matching
    // NormalizeEmoteCommand's accent-safe behaviour.
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

const BundledUnlockInfo& GetBundledUnlockInfo() {
    static BundledUnlockInfo info;
    static bool built = false;
    if (built) return info;
    EnsureTableLoaded();  // populates s_table from the bundled emotes_i18n.json
    for (const auto& entry : s_table) {
        if (entry.isCore) continue;  // only unlockables have an account-API state
        info.unlockableIds.insert(entry.id);
        // The id itself is a key so a PascalCase account-API id ("Rock") resolves.
        info.normKeyToId[NormalizeUnlockKey(entry.id)] = entry.id;
        for (const auto& kv : entry.byLang) {
            const LangEntry& le = kv.second;
            std::string k = NormalizeUnlockKey(le.command);
            if (!k.empty()) info.normKeyToId[k] = entry.id;
            for (const auto& a : le.aliases) {
                std::string ka = NormalizeUnlockKey(a);
                if (!ka.empty()) info.normKeyToId[ka] = entry.id;
            }
        }
    }
    built = true;
    LOG_INFO("Unlock index: %d unlockable id(s), %d match key(s)",
             (int)info.unlockableIds.size(), (int)info.normKeyToId.size());
    return info;
}

bool LoadEmotesJson(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_DEBUG("emotes.json not present at %s", path.c_str());
        return false;
    }

    json j;
    try {
        f >> j;
    } catch (const json::parse_error& e) {
        LOG_WARNING("emotes.json parse error at byte %zu: %s",
                    (size_t)e.byte, e.what());
        return false;
    }

    if (!j.is_object() || !j.contains("emotes") || !j["emotes"].is_array()) {
        LOG_WARNING("emotes.json has no \"emotes\" array");
        return false;
    }

    // changed tracks whether sanitization altered the file's content (dropped an
    // entry, normalized a command, derived a name, fixed the language). When set,
    // we re-save at the end so the on-disk file heals.
    bool changed = false;

    std::string lang = jsonutil::GetString(j, "emote_language", std::string());
    if (!lang.empty()) {
        const auto& langs = AvailableEmoteLanguages();
        if (std::find(langs.begin(), langs.end(), lang) == langs.end()) {
            LOG_WARNING("emotes.json: unknown emote_language '%s' - using 'en'",
                        lang.c_str());
            lang = "en";
            changed = true;
        }
    }

    std::vector<Emote> parsed;
    for (const auto& item : j["emotes"]) {
        if (!item.is_object()) { changed = true; continue; }
        Emote e;
        e.Id           = jsonutil::GetString(item, "id",         std::string());
        e.Command      = jsonutil::GetString(item, "command",    std::string());
        e.Name         = TruncateUtf8(jsonutil::GetString(item, "name", std::string()), kMaxNameBytes);
        // Sanitize IconPath at ingress: locks the value to addons/emot3/icons
        // (subfolders allowed; ui/ subfolder excluded) and passes bundled refs
        // through untouched. A hand-edit pointing outside the icons folder
        // heals to empty + logs the original so the user sees what we dropped.
        {
            std::string rawIcon = jsonutil::GetString(item, "icon", std::string());
            bool iconChanged = false;
            e.IconPath = SanitizeIconPath(rawIcon, &iconChanged);
            if (iconChanged) {
                LOG_WARNING("emotes[%s].icon: rejected '%s' (must live under "
                            "addons/emot3/icons; bundled: refs OK)",
                            e.Id.c_str(), rawIcon.c_str());
                changed = true;
            }
        }
        e.IsTargetable = jsonutil::GetBool  (item, "targetable", false);
        e.IsCore       = jsonutil::GetBool  (item, "is_core",    false);
        e.IsMadKing    = jsonutil::GetBool  (item, "mad_king",   false);
        e.UserKeybind  = jsonutil::GetBool  (item, "keybind",    false);

        // Every command ingress runs the same normalization (leading '/' etc.)
        // so the send path's "skip index 0" assumption always holds.
        std::string norm = NormalizeEmoteCommand(e.Command);
        if (norm != e.Command) { e.Command = norm; changed = true; }

        if (e.Id.empty() || e.Command.empty()) { changed = true; continue; }  // drops entries without an id

        // A valid emote with no display name gets one derived from its command
        // stem, mirroring the seed path (so a blank "name" never renders empty).
        if (e.Name.empty()) { e.Name = DeriveName(e.Command); changed = true; }

        // Optional aliases (alternate commands). Normalized like Command; drop
        // empties, the primary command itself, and duplicates. Missing key is
        // fine (older files / emotes without aliases).
        {
            const json& al = jsonutil::GetArray(item, "aliases");
            for (const auto& a : al) {
                if (!a.is_string()) continue;
                std::string na = NormalizeEmoteCommand(a.get<std::string>());
                if (na.empty() || na == e.Command) continue;
                if (std::find(e.Aliases.begin(), e.Aliases.end(), na) == e.Aliases.end())
                    e.Aliases.push_back(na);
            }
        }

        // Optional auto-mote chat triggers (v1.5). Lowercased + trimmed at
        // ingress (KeywordMatches expects them pre-lowercased); empties dropped,
        // dedup first-wins. Distinct from aliases — these are chat-line triggers,
        // not slash-command variants. Missing key is fine (the common case).
        {
            const json& kws = jsonutil::GetArray(item, "auto_keywords");
            for (const auto& k : kws) {
                if (!k.is_string()) continue;
                std::string kw = ToLower(TrimWhitespace(k.get<std::string>()));
                if (kw.empty()) { changed = true; continue; }
                if (std::find(e.AutoKeywords.begin(), e.AutoKeywords.end(), kw) == e.AutoKeywords.end())
                    e.AutoKeywords.push_back(std::move(kw));
                else
                    changed = true;
            }
        }

        bool dup = false;
        for (const auto& p : parsed)
            if (p.Id == e.Id) { dup = true; break; }
        if (!dup) parsed.push_back(std::move(e));
        else { changed = true; LOG_WARNING("emotes.json: dropped duplicate emote id \"%s\"", e.Id.c_str()); }
    }

    int coreCount = 0, unlockCount = 0;
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        g_Emotes = std::move(parsed);
        for (const auto& e : g_Emotes) (e.IsCore ? coreCount : unlockCount)++;
    }
    g_EmoteLanguage = lang;
    LOG_INFO("Loaded %d emote(s) from emotes.json (%d core, %d unlockable, lang '%s')",
             coreCount + unlockCount, coreCount, unlockCount, lang.c_str());

    // Heal the file in place when sanitization corrected something. Self-
    // contained (unlike settings, which reports back to AddonLoad) because
    // emotes.json has a single loader and a single writer.
    if (changed && !path.empty()) {
        LOG_INFO("emotes.json: rewriting to heal sanitized content");
        SaveEmotesJson(path);
    }
    return true;
}

void DeleteEmote(const std::string& id) {
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        auto it = std::find_if(g_Emotes.begin(), g_Emotes.end(),
                               [&](const Emote& e){ return e.Id == id; });
        if (it != g_Emotes.end()) g_Emotes.erase(it);
    }
    // Cascade: drop it from favorites and from the manual-unlock list (its own
    // settings-backed state), then persist both files + bump the catalog epoch.
    RemoveEmoteFromCategories(id);
    auto& u = g_Settings.ManuallyUnlocked;
    u.erase(std::remove(u.begin(), u.end(), id), u.end());
    RequestSave(SaveKind::Emotes);
    RequestSave(SaveKind::Settings);
    MarkEmotesDirty();
    LOG_INFO("Deleted emote %s", id.c_str());
}

std::string SerializeEmotesJson() {
    PROFILE_SCOPE("save.catalog");  // dev perf overlay - catalog JSON serialize cost
    std::ostringstream f;   // build in memory; the caller writes it atomically (temp + rename)

    // Hand-rolled writer (mirrors Settings.cpp's SaveSettings) so the on-disk
    // layout is controlled: per-emote keys in a logical, stable order instead of
    // nlohmann's alphabetical default, and the aliases array stays inline. We
    // still lean on nlohmann for string escaping via json(v).dump().
    // error_handler_t::replace: a string with invalid UTF-8 (e.g. an ANSI-codepage
    // filename picked into IconPath) yields U+FFFD instead of THROWING - an
    // uncaught dump() exception here would terminate the game.
    auto quoted = [](const std::string& v) {
        return json(v).dump(-1, ' ', false, json::error_handler_t::replace);
    };
    auto B = [](bool v) { return v ? "true" : "false"; };

    f << "{\n";
    f << "  \"version\": 1,\n";
    f << "  \"emote_language\": " << quoted(g_EmoteLanguage) << ",\n";
    f << "  \"emotes\": [";

    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        for (size_t i = 0; i < g_Emotes.size(); ++i) {
            const Emote& e = g_Emotes[i];
            if (i) f << ",";
            f << "\n    {\n";
            f << "      \"id\": "         << quoted(e.Id)       << ",\n";
            f << "      \"command\": "    << quoted(e.Command)  << ",\n";
            f << "      \"name\": "       << quoted(e.Name)     << ",\n";
            f << "      \"is_core\": "    << B(e.IsCore)        << ",\n";
            f << "      \"targetable\": " << B(e.IsTargetable)  << ",\n";
            f << "      \"mad_king\": "   << B(e.IsMadKing)     << ",\n";
            f << "      \"keybind\": "    << B(e.UserKeybind)   << ",\n";
            f << "      \"icon\": "       << quoted(e.IconPath);
            // aliases omitted entirely when empty (matches prior behaviour);
            // when present, written inline so the file stays compact.
            if (!e.Aliases.empty()) {
                f << ",\n      \"aliases\": [";
                for (size_t k = 0; k < e.Aliases.size(); ++k) {
                    if (k) f << ", ";
                    f << quoted(e.Aliases[k]);
                }
                f << "]";
            }
            // auto-mote chat triggers — inline, omitted entirely when empty (the
            // common case), so a catalog without auto-motes stays unchanged.
            if (!e.AutoKeywords.empty()) {
                f << ",\n      \"auto_keywords\": [";
                for (size_t k = 0; k < e.AutoKeywords.size(); ++k) {
                    if (k) f << ", ";
                    f << quoted(e.AutoKeywords[k]);
                }
                f << "]";
            }
            f << "\n    }";
        }
        if (!g_Emotes.empty()) f << "\n  ";
    }

    f << "]\n";
    f << "}\n";
    return f.str();
}

void SaveEmotesJson(const std::string& path) {
    // Thin disk-writing wrapper; catalog edits route through the SaveScheduler
    // (debounced, off-thread). Crash-safe via temp+rename.
    AtomicWriteFile(path, SerializeEmotesJson());
}

const Emote* FindEmote(const std::string& id) {
    for (const auto& e : g_Emotes)
        if (e.Id == id) return &e;
    return nullptr;
}

void BuildCatalogIndex(const std::vector<std::string>& manuallyUnlocked,
                       CatalogIndex& out) {
    // Caller holds g_EmotesMutex; pointers into g_Emotes stay valid as long as
    // it does (and the index is rebuilt every frame, so nothing outlives the lock).
    out.byId.clear();
    out.byId.reserve(g_Emotes.size());
    for (const auto& e : g_Emotes) out.byId.emplace(e.Id, &e);
    out.unlockedIds.clear();
    out.unlockedIds.insert(manuallyUnlocked.begin(), manuallyUnlocked.end());
}
