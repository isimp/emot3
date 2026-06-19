#pragma once

#include <string>
#include <vector>

struct Emote;  // data/EmoteData.h

// =====================================================================
// Emote catalog migration  (emotes.json — record transforms)
// =====================================================================
// emot3's SECOND migration system (see SettingsMigrations.* for the first).
// Unlike the settings framework — a PRE-parse, DOM-only transform of
// settings.json — the emote catalog migrates POST-parse: it works on the
// already-built std::vector<Emote> and compares each emote against the bundled
// tables by id. Because it needs the bundle (and the frozen v1 snapshot) it
// can't be a settings-style DOM step; it lives here as a parallel runner.
//
// Only the per-field upgrade POLICY lives in this submodule. The bundle/table
// machinery stays in EmoteData.cpp (where the tables already live); this runner
// reaches it through the ResolveBundledById() seam declared in EmoteData.h.
//
// See emot3.md "Schema version + the v1->v2 migration" and nexus-addon-dev.md
// "Migrating a bundled catalog (records, not settings)".

namespace emot3::migrations {

// Bring `emotes` (freshly parsed from emotes.json, whose stored schema is
// `fromVersion`) up to kEmotesSchemaVersion. For each bundled emote still
// holding its v1 default (matched by id, resolved in `lang` with English
// fallback) it adopts the current bundle value; fields the user customized are
// left untouched. unlock_item/wiki_slug (absent in v1) are filled here. Aliases
// are intentionally NOT migrated. Custom emotes (id not in the bundle) and
// brand-new bundled emotes (added by the notifier) are skipped.
//
// No-op returning false when already at/above kEmotesSchemaVersion; returns true
// if it changed anything, so the caller can re-save to stamp the new version.
// Idempotent once the file is stamped current.
bool RunEmoteCatalogMigrations(std::vector<Emote>& emotes, int fromVersion,
                               const std::string& lang);

}  // namespace emot3::migrations
