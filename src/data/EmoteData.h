#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// emotes.json schema version this build writes. Lower-versioned files are migrated
// up on load (see RunEmoteCatalogMigrations in EmoteData.cpp); a fresh file is
// stamped with this on first save.
constexpr int kEmotesSchemaVersion = 2;

struct Emote {
    // Id is the stable, language-INDEPENDENT key across the codebase
    // (favorites, ManuallyUnlocked, texture cache key, bundled-icon
    // filename, drag payload). For bundled emotes it's the English
    // command stem ("bow"); for user-added emotes it's derived from the
    // command they typed. Uniqueness is enforced by the Options UI.
    std::string Id;            // e.g. "bow"

    // Command is the slash command sent to chat. GW2 accepts several
    // command variants per emote (and offers a set per language); which
    // variant we store and send is the user's preference, picked via the
    // emote language - NOT dictated by the game client's language. Id
    // stays put regardless of what Command is.
    std::string Command;       // e.g. "/verbeugen"
    std::string Name;          // display label, e.g. "Verbeugen"

    // IsCore == true:  always unlocked, no lock state. Shipped with the
    //                  game's base emote set.
    // IsCore == false: unlock state is user-managed via ManuallyUnlocked.
    bool        IsCore       = false;

    bool        IsTargetable = false;  // supports "/cmd @" current-target syntax

    // Part of the "Your Mad King Says..." Halloween event emote set. Drives the
    // optional Mad King Quickbar category.
    bool        IsMadKing    = false;

    std::string IconPath;      // user override. Empty = derive from Id
                               // (see ResolveIconPath in Icons.cpp).

    // Alternate slash commands that ALSO identify this emote (normalized like
    // Command: leading '/', lowercase). Used by the GW2-API unlock sync to
    // recognize the emote, and by the main-panel search. NOT used for sending
    // (an emote always sends its single Command). Bundled emotes seed these
    // from the GW2 wiki's command variants; users can edit them in the Catalog.
    std::vector<std::string> Aliases;

    // Opt-in: register a Nexus InputBind for this emote so the user can bind a
    // key to fire it directly (toggle in Options > Catalog), and so it's
    // eligible for a RadialMenus wheel. Persisted as "keybind" in emotes.json.
    // See core/EmoteBinds.
    bool UserKeybind = false;

    // Auto-motes (v1.5): chat trigger words. When auto-motes is enabled
    // (g_Settings.AutoMotesEnabled) and ANY of these appears WHOLE-WORD,
    // case-insensitive in one of the user's OWN sent chat lines on a watched
    // channel, this emote auto-fires through the normal send gate. Empty (the
    // default) = this emote never auto-fires. Stored lowercased + trimmed at
    // ingress; serialized as "auto_keywords" in emotes.json (omitted when empty).
    // Distinct from Aliases (which are search/unlock slash-command variants) so
    // search aliases don't silently become chat triggers. See core/ChatWatch.
    std::vector<std::string> AutoKeywords;

    // Unlock provenance for bundled unlockable emotes (seeded from the bundled
    // table; empty/0 for core and user-added emotes). Reference data for the UI -
    // UnlockItem is the GW2 API item id of the unlock tome; WikiSlug is the wiki
    // URL path segment of its unlock page (append to <wiki host>/wiki/; powers the
    // "Copy wiki link" menu). Serialized as "unlock_item"/"wiki_slug" in
    // emotes.json (omitted when 0/empty), mirroring the bundled emotes_i18n.json.
    int         UnlockItem = 0;
    std::string WikiSlug;
};

// Runtime emote catalog. All emotes (core + unlockable) live here, and
// every entry is serialized to emotes.json.
extern std::vector<Emote> g_Emotes;
extern std::mutex         g_EmotesMutex;

// The language the current catalog is seeded in (e.g. "de"). Read from
// emotes.json "emote_language", written back on save, and set by the
// first-run dialog / Options "Emote language" control. Empty when the
// catalog is empty / language unknown.
extern std::string        g_EmoteLanguage;

// Clears g_Emotes. Called at addon load before LoadEmotesJson runs.
void ClearEmotes();

// Seed g_Emotes from the bundled emote-localization table for `lang`
// (e.g. "de"). Idempotent by Id: an emote whose Id already exists is
// skipped. Per-emote English fallback when `lang` has no data for that
// emote. Also sets g_EmoteLanguage to `lang`. Returns the count added.
// Seed in `lang`. If `secondaryLang` is a different language the bundled entry
// has data for, that language's command + aliases are folded into each new
// emote's Aliases (search + unlock matching only; never sent). Only affects
// newly added emotes (existing ids are skipped).
int SeedDefaultEmotes(const std::string& lang,
                      const std::string& secondaryLang = "");

// Add ONLY the bundled emotes whose ids are in `ids` and aren't already in the
// catalog (the notifier's "add the new ones"). Unlike SeedDefaultEmotes this
// never touches emotes outside `ids` - so a bundled emote the user deleted is
// not resurrected - and does not change g_EmoteLanguage. Returns the count
// added. `lang`/`secondaryLang` control how each added emote is localized.
int AddBundledEmotesByIds(const std::vector<std::string>& ids,
                          const std::string& lang,
                          const std::string& secondaryLang = "");

// Language codes the bundled table carries (its "languages" array),
// e.g. {"en","de","fr","es"}. The first-run dialog enumerates these.
std::vector<std::string> AvailableEmoteLanguages();

// Every emote id in the bundled table (core AND unlockable) - unlike
// GetBundledUnlockInfo().unlockableIds, which is unlockables-only. Used by the
// new-bundled-emote notifier's snapshot diff (see emot3.md "Bundled-emote
// notifier").
std::vector<std::string> AllBundledEmoteIds();

// True if `id` is a reserved bundled emote id (core OR unlockable). Blocks users
// from creating a custom emote that would shadow a bundled one. Cached set built
// from the bundled table on first call.
bool IsBundledEmoteId(const std::string& id);

// Bundle ids that are NEW relative to `known` (a snapshot of bundled ids seen on
// a prior run) AND not already present in the live catalog. Drives the notifier:
// a non-empty result is the set of emotes to offer the user. Deliberately-
// deleted bundled emotes stay in `known`, so they never reappear here. Takes
// g_EmotesMutex internally to read the catalog.
std::vector<std::string> ComputeNewBundledEmotes(const std::vector<std::string>& known);

// emotes.json persistence (schema v1: id/command/name/is_core/targetable
// /mad_king/icon, plus a top-level emote_language). Tolerant of missing keys.
bool LoadEmotesJson(const std::string& path);
void SaveEmotesJson(const std::string& path);
// Serialize the catalog to its on-disk JSON string (holds g_EmotesMutex while
// iterating) without touching disk — see SerializeSettings.
std::string SerializeEmotesJson();

// Delete an emote from the catalog by Id and cascade the cleanup: drop it from
// favorites + the manual-unlock list, persist emotes.json + settings.json, and
// bump the catalog epoch. Self-contained data operation (the Options UI just
// calls it, then clears its own row buffers).
void DeleteEmote(const std::string& id);

// Lookup by stable Id. Returns nullptr if not found.
const Emote* FindEmote(const std::string& id);

// Per-frame catalog lookup tables, rebuilt every frame under g_EmotesMutex — never
// cached across frames (see emot3.md "Things we tried and rolled back"). byId
// resolves an Id -> emote in O(1); unlockedIds holds the manually-unlocked Ids for
// O(1) state checks. unlocked() folds in the always-unlocked core rule so callers
// don't re-spell `e.IsCore || unlockedIds.count(...)`. Built by BuildCatalogIndex,
// shared by the main-panel and Quickbar render builds so their indexing can't drift.
struct CatalogIndex {
    std::unordered_map<std::string, const Emote*> byId;
    std::unordered_set<std::string>               unlockedIds;
    bool unlocked(const Emote& e) const {
        return e.IsCore || unlockedIds.count(e.Id) != 0;
    }
};

// Fill `out` from g_Emotes (byId) + `manuallyUnlocked` (unlockedIds). PRECONDITION:
// the caller already holds g_EmotesMutex (the lock both render builds take while
// reading g_Emotes). The unlocked list is passed in rather than read from g_Settings
// so this data module stays independent of Settings.
void BuildCatalogIndex(const std::vector<std::string>& manuallyUnlocked,
                       CatalogIndex& out);

// Normalized matching key: trim, lowercase, and STRIP a leading '/'. Unlike
// NormalizeEmoteCommand (which keeps the '/'), this collapses an account-API id
// ("Rock"), a slash command ("/rock") and an alias ("/Rock") onto one key
// ("rock") so the unlock sync can match across case + the slash.
std::string NormalizeUnlockKey(std::string s);

// Bundled-table-derived data for the GW2-API unlock sync (core/UnlockScan):
// the set of bundled UNLOCKABLE ids (is_core == false) — our authoritative
// "emotes the account API can speak about" — and a NormalizeUnlockKey() index
// over every unlockable's id + every language's command AND aliases, so an API
// id ("Rock"), a localized command ("/abrocken") or an alias ("/bbq") all
// resolve to the stable id. Built once from emotes_i18n.json (then cached).
struct BundledUnlockInfo {
    std::unordered_set<std::string>              unlockableIds;
    std::unordered_map<std::string, std::string> normKeyToId;    // norm(key) -> id
    std::unordered_map<std::string, std::string> wikiSlugById;   // id -> wiki URL path slug
    std::unordered_map<std::string, int>         unlockItemById; // id -> unlock item id
};
const BundledUnlockInfo& GetBundledUnlockInfo();

// Build a full GW2 wiki URL from a stored wiki_slug (Emote::WikiSlug), or "" if the
// slug is empty. The slug already carries the page's canonical URL encoding. The
// slug belongs to the emote (seeded / loaded), so callers pass e.WikiSlug rather
// than re-deriving by id.
std::string WikiUrl(const std::string& slug);

// Dev-tool (MemoryMonitor) accessors: estimated entry count + heap footprint of the two
// bundled startup tables (the emote-i18n table behind seeding/search, and the unlock
// match index). Within ~2x; report 0 until loaded/built. Only called under EMOT3_DEVTOOLS.
void BundledEmoteTableStats(size_t& count, size_t& bytes);
void BundledUnlockIndexStats(size_t& count, size_t& bytes);

// Normalize a slash command for the send path: trim, force a leading '/',
// lowercase. The send path (SendOrFillEmote in EmoteAction.cpp) skips index 0
// assuming it's the '/', so a command without one loses its first character.
// This is the single invariant applied at EVERY command ingress - emotes.json
// load and both the Options add and per-row edit commits - so the property the
// send path relies on is guaranteed wherever a command enters. Returns the
// input unchanged when it trims to empty.
std::string NormalizeEmoteCommand(std::string command);

// Canonical form for a user-typed emote Id (the stable catalog key): lowercase,
// keep [a-z0-9_], collapse any other run to a single '_', drop leading/trailing
// '_'. Mirrors NormalizeMeMoteId so emote + /me-mote ids share one safe-key rule.
std::string NormalizeEmoteId(std::string s);
