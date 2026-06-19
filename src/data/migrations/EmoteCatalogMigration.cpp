#include "EmoteCatalogMigration.h"

#include "EmoteData.h"   // Emote, kEmotesSchemaVersion, NormalizeEmoteCommand,
                         // ResolvedBundleEmote + ResolveBundledById (the seam)
#include "Logging.h"

namespace emot3::migrations {

// v1->v2: for each bundled emote still holding its v1 default (matched by id, in
// the catalog's seed language), adopt the current bundle value - leaving
// user-customized fields untouched. unlock_item/wiki_slug (absent in v1) are
// filled here, REPLACING the former always-on backfill. Aliases are intentionally
// NOT migrated. Custom emotes (id not bundled) and the two NEW emotes (added by
// the new-bundled-emote notifier) are out of scope. Idempotent once stamped v2.
//
// The bundle/table machinery (the current table + the frozen v1 snapshot + the
// per-language resolver) stays in EmoteData.cpp; we reach both versions of an
// emote's defaults through ResolveBundledById(), so this file is pure policy.
bool RunEmoteCatalogMigrations(std::vector<Emote>& emotes, int fromVersion,
                               const std::string& lang) {
    if (fromVersion >= kEmotesSchemaVersion) return false;
    const std::string L = lang.empty() ? std::string("en") : lang;
    for (Emote& e : emotes) {
        const ResolvedBundleEmote oldd = ResolveBundledById(e.Id, L, /*fromV1=*/true);
        const ResolvedBundleEmote newd = ResolveBundledById(e.Id, L, /*fromV1=*/false);
        if (!oldd.found || !newd.found) continue;  // custom / brand-new id: skip
        // command/name: replace only if still the v1 default (normalize both
        // command sides so the compare mirrors load-time normalization).
        if (e.Command == NormalizeEmoteCommand(oldd.command))
            e.Command = NormalizeEmoteCommand(newd.command);
        if (e.Name == oldd.name) e.Name = newd.name;
        // flags (per-id): adopt v2 only when the user still has the v1 value.
        if (e.IsTargetable == oldd.targetable) e.IsTargetable = newd.targetable;
        if (e.IsCore       == oldd.isCore)     e.IsCore       = newd.isCore;
        if (e.IsMadKing    == oldd.madKing)    e.IsMadKing    = newd.madKing;
        // unlock provenance: absent in v1 -> fill from v2 (one-time).
        if (e.WikiSlug.empty()) e.WikiSlug   = newd.wikiSlug;
        if (e.UnlockItem == 0)  e.UnlockItem = newd.unlockItem;
    }
    LOG_INFO("emote catalog: migrated schema v%d -> v%d", fromVersion, kEmotesSchemaVersion);
    return true;
}

}  // namespace emot3::migrations
