#pragma once

// /me-motes — user-defined free-form text chat emotes that send "/me <text>".
// A SEPARATE domain from the official Emote catalog: own struct, own JSON file
// (addons/emot3/me_motes.json), own Options tab. The Library, Quickbar, and
// favorites systems surface them alongside official emotes via the unified
// CellItem adapter in src/ui/Cells.cpp.
//
// Why separate from Emote: /me-motes carry no Command, no IsCore / IsTargetable
// / IsMadKing, no per-language localization (user-authored in whatever language
// they play in). Lumping them into the Emote struct would either pollute the
// official-emote schema with unused fields or sprinkle "if (IsMeMote) ..."
// branches across every catalog code path.
//
// Send pipeline: each /me-mote ships up to three pre-written variants — Default
// (left-click), You (right-click "Send (you)"), All (right-click "Send (all)").
// GW2 has no addon-facing target API, so target-aware auto-switching isn't
// possible (verified: MumbleLink + RTAPI + Nexus AddonAPI all silent on
// target); pre-written variants give the user contextual variety without it.
// See emot3.md "/me-motes".

#include <string>
#include <vector>
#include <mutex>

// Which of a /me-mote's three text bodies to send. Default fires on left-click;
// You and All are the right-click context-menu entries (each grayed out when
// the corresponding text is empty). Lives here (the /me-mote domain) because
// MeMote stores a KeybindVariant of this type; core/EmoteAction.h opaque-
// forward-declares it for SendOrFillMeMote.
enum class EMeMoteVariant { Default, You, All };

struct MeMote {
    // Stable, language-INDEPENDENT key across the codebase (favorites refs,
    // serialization, texture cache key for the icon path). Derived from the
    // Name on create; uniqueness is enforced by the Options > /me-motes UI.
    std::string Id;

    // Display label shown in the Library /me-motes section, the Quickbar
    // /me-motes category, and favorites cells. Free-form, user-authored.
    std::string Name;

    // Optional user-supplied icon path override. Resolves through the same
    // chain Emotes use, minus the BundledOfficial tier (ArenaNet doesn't
    // ship art for /me-motes since they're user-content): explicit IconPath
    // → icons/<id>.png drop-in → bundled AI artwork (opt-in via
    // UseAIIconFallback) → styled letter button. See ResolveMeMoteIconPath
    // + ResolveMeMoteIconSource in Icons.cpp.
    std::string IconPath;

    // Search hints. The Library's search bar matches Name + Aliases (not the
    // body texts — sentence-long content would yield noisy hits). Same
    // convention as Emote.Aliases but unstored slash semantics: user-authored
    // free-form words ("party", "join", "thinking").
    std::vector<std::string> Aliases;

    // The three contextual text bodies. TextDefault is REQUIRED (load
    // sanitize drops any /me-mote with empty TextDefault; the Options editor
    // blocks save on empty). TextYou / TextAll are optional; their
    // corresponding right-click context-menu entries gray out when empty
    // (the entries always show — discoverable as a concept even when not
    // configured for this particular /me-mote).
    //
    // Send-time: "/me " is prepended exactly once in SendOrFillMeMote — NEVER
    // store it in these strings. (A user pasting "/me hugs" into the field
    // gets sanitized to "hugs" on save.)
    std::string TextDefault;
    std::string TextYou;
    std::string TextAll;

    // Opt-in Nexus InputBinds — bind ANY subset of the three variants, each as
    // its own bind (its own identifier), so a hotkey or a RadialMenus wheel item
    // can target a specific body. You/All are only bindable when that body is
    // non-empty. Persisted as "keybinds": ["default","you","all"]. See
    // core/EmoteBinds. (KeybindDefault doubles as the legacy single-bind opt-in.)
    bool KeybindDefault = false;
    bool KeybindYou     = false;
    bool KeybindAll     = false;
};

// True if any variant of this /me-mote is bound (used by the editor + binds sync).
inline bool MeMoteHasAnyKeybind(const MeMote& m) {
    return m.KeybindDefault || m.KeybindYou || m.KeybindAll;
}

// Runtime /me-mote catalog. All /me-motes live here; serialized to me_motes.json.
extern std::vector<MeMote> g_MeMotes;
extern std::mutex          g_MeMotesMutex;

// The language the bundled /me-mote samples seed in (e.g. "de"). Defaults to
// "en" at first creation; the first-run dialog and AddonLoad upgrade hook
// stamp it to match the picked emote language (clamped to
// AvailableMeMoteLanguages() so the value is always one the bundle ships).
// Read by SeedBundledMeMotes for the Restore path; the Options > /me-motes
// language combo writes it. Saved to me_motes.json as "language". Parallel
// to g_EmoteLanguage in EmoteData.h — separate so the user can keep their
// emote catalog in one language and re-seed /me-motes in another.
extern std::string         g_MeMoteLanguage;

// Clear the in-memory catalog (held under g_MeMotesMutex). Called at AddonLoad
// before LoadMeMotesJson runs, so a hand-cleared file maps to an empty catalog
// rather than retaining stale entries.
void ClearMeMotes();

// me_motes.json persistence. Schema v1:
//   { "version": 1,
//     "me_motes": [ { "id", "name", "icon", "aliases": [...],
//                     "text", "text_you", "text_all" }, ... ] }
// Tolerant of missing keys; sanitize drops an entry with empty Id or empty
// TextDefault, dedupes by Id, and strips leading "/me " from any body field if
// the user accidentally typed it in (TextDefault et al. carry the variant text
// only — the prefix is added at send time).
bool LoadMeMotesJson(const std::string& path);
void SaveMeMotesJson(const std::string& path);
// Serialize the /me-mote catalog to its on-disk JSON string (holds g_MeMotesMutex
// while iterating) without touching disk — see SerializeSettings.
std::string SerializeMeMotesJson();

// Delete a /me-mote from the catalog by Id and cascade: drop it from favorites,
// bump the epoch, and persist me_motes.json. Self-contained (the Options UI just
// calls it, then clears its own row buffers).
void DeleteMeMote(const std::string& id);

// Lookup by stable Id. Returns nullptr if not found. Caller should hold
// g_MeMotesMutex if the catalog could mutate concurrently (the per-frame
// renderer takes it; background mutators take it too — same discipline as
// FindEmote).
const MeMote* FindMeMote(const std::string& id);

// A /me-mote is renderable in the Library / Quickbar / favorites cells only
// when it carries both a display label (Name) and a body to send on click
// (TextDefault). Empty Name renders an unreadable letter-fallback cell with
// no tooltip text; empty TextDefault means a click sends nothing. A
// half-filled entry (the user just created one and only typed the Id) is
// invisible until both required fields land, but stays in the editor so the
// user can finish it. Inline so callers don't need a separate TU dep.
inline bool IsMeMoteRenderable(const MeMote& m) {
    return !m.Name.empty() && !m.TextDefault.empty();
}

// Normalize a free-form string into a /me-mote Id: trim, lowercase ASCII,
// replace anything that isn't [a-z0-9_] with '_', collapse runs of '_', and
// trim leading/trailing '_'. Mirrors the NormalizeEmoteCommand invariant of
// "every Id entry normalized at every ingress" so the catalog's Id field is
// consistent across hand-edits + UI adds. Stable across runs (a given Name
// always produces the same Id). Returns "" on empty/all-invalid input — the
// caller (Options UI) appends a uniqueness suffix and falls back to a
// "me_mote_N" placeholder when the source string normalizes to nothing.
std::string NormalizeMeMoteId(std::string s);

// --- Bundled /me-mote seed table -----------------------------------------
//
// resources/me_mote_data/me_motes_i18n.json bundles a handful of "I'd type
// this in chat anyway, no animation matches" /me-motes (LFG, Ready check,
// Need healer, ...) so the first-run experience isn't an empty section.
// The first-run language dialog seeds BOTH catalogs: SeedDefaultEmotes for
// the official catalog, then SeedBundledMeMotes(lang) for the /me-motes.
// The Options > /me-motes tab also exposes a "Restore bundled" button that
// re-runs the same idempotent-by-Id seeder, mirroring the OptionsEmotes
// "Restore built-in emotes" affordance.

// Seed g_MeMotes from the bundled seed table for `lang` (e.g. "de").
// Idempotent by Id: a /me-mote whose Id already exists is skipped (so a user
// rename of a sample stays renamed; a deleted sample reappears on Restore).
// Per-entry English fallback when `lang` has no data for that entry, so
// FR/ES players still get usable samples until those translations land.
// Returns the count added.
int SeedBundledMeMotes(const std::string& lang);

// Language codes the bundled /me-mote table carries (its "languages" array),
// e.g. {"en","de"}. Sibling to AvailableEmoteLanguages(); lets a caller
// reason about whether the user's chosen language has localized samples or
// will fall back to English.
std::vector<std::string> AvailableMeMoteLanguages();

// Map an arbitrary language code to one the /me-mote bundle actually carries:
// returns `lang` when it's in AvailableMeMoteLanguages(), else "en". Used at
// every seed-language ingress (first-run dialog, AddonLoad upgrade hook,
// Options combo write) so g_MeMoteLanguage is always a value the seeder can
// resolve without a per-entry fallback. Empty input returns "en".
std::string ClampMeMoteLanguage(const std::string& lang);

// GW2 chat caps each transmitted line at ~199 characters; "/me " (4 chars) is
// prepended at send time, so the usable BODY budget per /me-mote variant is
// 195. Lives next to the schema so the editor warning + any send-time clip
// guard reference the same constant. The Options editor uses this to color a
// live char counter under each body input (gray <=80%, amber >80%, red >cap)
// — long bodies still save and send, but the editor surfaces the truncation
// risk to the author rather than letting it bite in-game.
constexpr int kMeMoteBodyCharBudget = 195;

// Accessors over the file-static bundled seed table for the MemoryMonitor
// subsystem row. Mirror the shape of TextCache::ApproxBytes() /
// TranslationCacheApproxBytes() so MemoryMonitor can reach the data without
// touching the anonymous-namespace statics directly. Both force the lazy
// LoadBundledMeMotesTable() call on first read.
size_t MeMotesBundledSeedCount();
size_t MeMotesBundledSeedBytes();
