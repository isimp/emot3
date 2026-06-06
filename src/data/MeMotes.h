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

struct MeMote {
    // Stable, language-INDEPENDENT key across the codebase (favorites refs,
    // serialization, texture cache key for the icon path). Derived from the
    // Name on create; uniqueness is enforced by the Options > /me-motes UI.
    std::string Id;

    // Display label shown in the Library /me-motes section, the Quickbar
    // /me-motes category, and favorites cells. Free-form, user-authored.
    std::string Name;

    // Optional user-supplied icon path override; empty = no icon, the cell
    // falls back to the styled letter button. Unlike Emotes, /me-motes have NO
    // <id>.png folder convention, no bundled art, and no AI fallback — it's the
    // explicit path or the letter (see ResolveMeMoteIconPath in Icons.cpp).
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
};

// Runtime /me-mote catalog. All /me-motes live here; serialized to me_motes.json.
extern std::vector<MeMote> g_MeMotes;
extern std::mutex          g_MeMotesMutex;

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
