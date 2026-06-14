#pragma once

#include <nlohmann/json.hpp>

// =====================================================================
// Settings schema migrations
// =====================================================================
// emot3's FIRST versioned-migration framework. settings.json carries an
// integer "version". On load (data/Settings.cpp LoadSettings), BEFORE the
// field-by-field parse, RunSettingsMigrations() walks the stored version up to
// kCurrentSchemaVersion, applying each step's JSON-DOM transform in order.
//
// Every transform mutates the parsed DOM in place, so the downstream parser
// only ever sees the CURRENT schema and stays free of legacy / tolerant-reader
// branches. Before this framework, those branches lived inline in LoadSettings
// (snap_scroll bool->int, left_click_opens_quickbar, favorites emotes->refs) -
// M001 folds them in here.
//
// Adding a migration (one file each under src/data/migrations/, named
// MNNN_*.cpp, in step order):
//   1. bump kCurrentSchemaVersion by one,
//   2. add MNNN_*.cpp defining void Migrate_NNN_*(nlohmann::json&),
//   3. declare it below and extend the dispatch switch in Migrations.cpp.
//
// Rules every migration MUST follow:
//   - Idempotent / guarded: a DOM already partly in the new shape (e.g. written
//     by a build that emitted new keys under an old version number) must survive
//     a re-run untouched. Guard on "is the old form actually present".
//   - DOM-only: operate on the json argument; never touch g_Settings, the
//     filesystem, or other globals. Cross-file follow-ups (e.g. presets) are
//     handled where that data is parsed - see QuickbarPresets ParsePresetJson.
//   - Never throw: tolerate missing / wrong-typed keys.

namespace emot3::migrations {

// The schema version this build writes. settings.json files at a lower version
// are migrated up on load; a fresh file is stamped with this on first save.
constexpr int kCurrentSchemaVersion = 3;

// Bring `dom` (a parsed, object-typed settings.json) from its stored "version"
// up to kCurrentSchemaVersion, applying each step in order. Mutates dom in
// place, including stamping dom["version"]. Safe on a DOM with a missing /
// out-of-range version (treated as the v1 baseline) and on an already-current
// or newer DOM (no-op). Returns true if any migration ran, so the caller can
// force a re-save to persist the new on-disk shape.
bool RunSettingsMigrations(nlohmann::json& dom);

// --- individual steps (one per file; invoked by the dispatch) --------------
void Migrate_001_NormalizeLegacyKeys(nlohmann::json& dom);  // v1 -> v2
void Migrate_002_UnusableSplit(nlohmann::json& dom);        // v2 -> v3

}  // namespace emot3::migrations
