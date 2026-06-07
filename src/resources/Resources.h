#pragma once
//
// Resource bundling — interface.
//
// PNG artwork is bundled into the addon DLL via Win32 RCDATA resources
// (emot3.rc + ResourcesGenerated.cpp, both produced by
// tools/gen_rc.py). Nothing is written to disk by us; the icons/ folder
// stays empty unless the user deliberately drops a PNG into it as an
// override. The Emote texture loader consults bundles in priority order:
//
//   1. icons/<command>.png        — user override (file-based)
//   2. kOfficialIcons             — bundled official artwork (memory)
//   3. kAIIcons                   — bundled AI artwork, opt-in via
//                                   g_Settings.UseAIIconFallback (memory)
//   4. RenderStyledFallback       — letter button
//
// The /me-mote texture loader uses a SHORTER chain (see Icons.cpp's
// ResolveMeMoteIconSource); the catalog is user-content so there's no
// BundledOfficial tier, but everything else parallels the Emote chain:
//
//   1. m.IconPath                 — user-supplied path (file-based)
//   2. icons/<id>.png             — folder drop-in matching the stable Id
//                                   (file-based, mirrors the Emote convention)
//   3. kMeMoteAIIcons             — bundled AI artwork keyed on /me-mote
//                                   Id, opt-in via the same setting (memory)
//   4. RenderStyledFallback       — letter button (drawn by
//                                   RenderMeMoteCellBody)
//
// This module exposes the manifest tables and a single byte-loading
// helper. Callers feed those bytes to Nexus' GetOrCreateFromMemory.

#include <cstddef>
#include <string>

struct BundledIcon {
    const char* command;     // lowercase command stem (no leading slash)
    int         resourceId;  // Win32 RCDATA id within the addon DLL
};

// Same layout as BundledIcon, distinct type so call sites read clearly:
// this maps a *file stem* (e.g. "de", "emotes_i18n") to its RCDATA id.
// Used for the bundled JSON data files (UI translations + emote-locale
// table) rather than PNG artwork.
struct BundledData {
    const char* name;        // lowercase file stem (no extension)
    int         resourceId;  // Win32 RCDATA id within the addon DLL
};

// Generated tables (defined in ResourcesGenerated.cpp).
extern const BundledIcon kOfficialIcons[];
extern const int         kOfficialIconsCount;
extern const BundledIcon kAIIcons[];
extern const int         kAIIconsCount;
extern const BundledIcon kUiIcons[];
extern const int         kUiIconsCount;
// UI translation tables (resources/i18n/<code>.json). The stems double
// as the set of available UI language codes — I18n discovery iterates
// this table rather than hardcoding a language list.
extern const BundledData kI18nFiles[];
extern const int         kI18nFilesCount;
// Emote-localization table (resources/emote_data/*.json). Currently a
// single file with stem "emotes_i18n".
extern const BundledData kEmoteData[];
extern const int         kEmoteDataCount;
// Default Quickbar presets (resources/presets/*.json). Seeded to the user's
// presets/ folder on first run (see QuickbarPresets.cpp BuildDefaultPresets).
extern const BundledData kPresets[];
extern const int         kPresetsCount;
// /me-mote seed table (resources/me_mote_data/*.json). Currently a single
// file with stem "me_motes_i18n". Consumed by data/MeMotes.cpp's
// LoadBundledMeMotesTable + SeedBundledMeMotes (id-keyed first-run seed,
// language-aware, EN fallback). Mirrors the kEmoteData consumer pattern.
extern const BundledData kMeMoteData[];
extern const int         kMeMoteDataCount;
// /me-mote AI artwork (resources/me_motes_ai/*.png). Mirrors kAIIcons but
// keyed on /me-mote stable Ids (lfg.png, brb.png, ...). Opt-in via the
// same g_Settings.UseAIIconFallback gate the Emote AI tier uses — keeps
// the AI-provenance opt-in policy consistent across the two catalogs.
// Skipped tier vs Emotes: there's NO BundledOfficial equivalent for
// /me-motes (the catalog is user-content; ArenaNet doesn't ship art for
// it), so the chain is icons/<id>.png → BundledAI → letter.
//
// Invariant: PNG stems MUST be valid normalized /me-mote ids ([a-z0-9_]).
// The fallback matches the user's m.Id via LookupBundledResource (which
// lowercases + slash-strips its input but does NOT collapse other
// punctuation), so a mis-named drop-in like "Clap.png" or
// "ready-check.png" silently never resolves. New drops should run through
// NormalizeMeMoteId mentally first.
extern const BundledIcon kMeMoteAIIcons[];
extern const int         kMeMoteAIIconsCount;

// Return the Win32 RCDATA id for `command` in the given table, or 0 if
// the command isn't bundled. Accepts both "/wave" and "wave" forms;
// matching is case-insensitive against the lowercase manifest stem.
int LookupBundledResource(const BundledIcon* table, int count,
                          const std::string& command);

// Lock-and-return a bundled PNG's raw bytes. The pointer is into the
// addon DLL's read-only resource section and lives for the addon's
// lifetime — safe to hand off to Nexus' Textures.GetOrCreateFromMemory.
//
// We expose this (rather than calling Nexus' GetOrCreateFromResource
// directly) because Nexus' implementation appears to look up resources
// by string type "RCDATA" rather than the predefined integer RT_RCDATA,
// which fails silently against our RC-compiled entries. Going through
// our own loader and handing Nexus raw bytes side-steps the issue.
bool TryLoadBundledIconBytes(const BundledIcon* table, int count,
                             const std::string& command,
                             const void*& outData, size_t& outSize);

// Data-file variants of the two helpers above, keyed by file stem
// (case-insensitive). Same RCDATA byte-loading path; the returned
// pointer is into the DLL's read-only resource section and lives for
// the addon's lifetime. Callers parse the bytes as UTF-8 JSON.
int LookupBundledData(const BundledData* table, int count,
                      const std::string& name);
bool TryLoadBundledData(const BundledData* table, int count,
                        const std::string& name,
                        const void*& outData, size_t& outSize);
