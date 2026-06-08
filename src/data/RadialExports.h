#pragma once

// Exported RadialMenus wheels — the managed record behind the Options "RadialMenus"
// tab. Staging mirrors RadialMenus' own layout: addons/emot3/radials/packs/ holds the
// RadialMenus-format pack <slug>.json files and addons/emot3/radials/icons/ holds
// their PNGs (named emot3_<slug>_*), so the user copies the two folders straight into
// RadialMenus. Each pack carries an `emot3_source_category` marker (ignored by
// RadialMenus), so the staged files double AS emot3's record — there is no separate
// bookkeeping file. This module scans radials/packs/ into an in-memory list at
// AddonLoad and on every mutation, mirroring data/QuickbarPresets.
//
// Writing packs + icons (and rename/remove that rewrite them) lives in
// core/RadialExport.{h,cpp}; this module is the passive record + read helpers + the
// raw folder delete. emot3 only ever WRITES packs and never reads RadialMenus
// runtime state, so a format drift can at worst render a wheel wrong, never crash.

#include <string>
#include <vector>
#include <mutex>

#include "Settings.h"   // EFavoriteRefType
#include "MeMotes.h"    // EMeMoteVariant

// RadialMenus capacity (items displayed at once). We enforce this ourselves — extras
// past the cap are silently dropped by RadialMenus — by requiring a subset pick or an
// auto-split in the export wizard.
constexpr int kRadialCapNormal = 12;
constexpr int kRadialCapSmall  = 8;

// One emote/me-mote slot in a wheel. Variant is meaningful only for /me-motes
// (Default for emotes). On scan it's recovered from the pack item's Action
// Identifier (the single carrier of type/id/variant) via ParseEmoteBindIdentifier.
struct RadialItemRef {
    EFavoriteRefType Type = EFavoriteRefType::Emote;
    std::string      Id;
    EMeMoteVariant   Variant = EMeMoteVariant::Default;
};

inline bool operator==(const RadialItemRef& a, const RadialItemRef& b) {
    return a.Type == b.Type && a.Id == b.Id && a.Variant == b.Variant;
}
inline bool operator!=(const RadialItemRef& a, const RadialItemRef& b) { return !(a == b); }

// Curated RadialMenus wheel options, stored in (and recovered from) the pack.
struct RadialWheelOptions {
    bool  Small               = false;  // menu Type: false = Normal, true = Small
    int   SelectionMode       = 3;      // RadialMenus selection mode int (see RadialExport.cpp)
    float Scale               = 1.0f;
    float IconScale           = 0.8f;   // 0.8 reads better than RadialMenus' 1.0 default
    bool  ShowItemNameTooltip = false;
    bool  GateByState         = true;   // write per-item Visibility can't-emote conditions
};

// One exported wheel == one radials/<Slug>/ subfolder. Rebuilt by scanning, so
// there's no separate persistence of this struct.
struct RadialExport {
    std::string Slug;            // subfolder name == pack file stem
    std::string Name;            // user-facing name (pack "Name" minus the "emot3: " prefix)
    std::string SourceCategory;  // emot3_source_category marker (favorites category of origin)
    int         Id = 0;          // RadialMenus menu ID (>= 90001)
    RadialWheelOptions          Options;
    std::vector<RadialItemRef>  Items;
    bool        Partial    = false;  // a deliberate subset / auto-split (not a full mirror of
                                     // the category) - drift is judged leniently for these
    bool        ParseError = false;  // pack present but unreadable (shown as a broken row)
};

// In-memory mirror of radials/, sorted by Name. Guarded because EmoteBinds reads it
// from SyncEmoteBinds while the UI mutates it on the render thread.
extern std::vector<RadialExport> g_RadialExports;
extern std::mutex                g_RadialExportsMutex;

// Prepended to every exported pack's "Name" so wheels are identifiable in
// RadialMenus' own list ("emot3: Greetings"). The scan strips it to recover Name.
extern const char* const kRadialPackNamePrefix;

// Staging subfolders that mirror RadialMenus' layout: radials/packs and radials/icons
// (empty strings if g_RadialsDir is unset). Creating them on demand is the writer's
// job; these just build the paths.
std::string RadialPacksDir();
std::string RadialIconsDir();

// (Re)scan radials/packs/ into g_RadialExports. Call at AddonLoad (after g_RadialsDir
// is set) and after any mutation. Tolerant of missing/malformed packs.
void LoadRadialExports();

// Favorites categories that already have >= 1 exported wheel (so the wizard's
// category combo can hide them). Case-sensitive on SourceCategory.
std::vector<std::string> ExportedSourceCategories();

// Names of the wheel(s) referencing (type,id) in ANY variant — drives the catalog
// editors' "active via radial 'X'" note. Empty when none.
std::vector<std::string> RadialWheelsContaining(EFavoriteRefType type,
                                                const std::string& id);

// Lowest unused menu ID >= 90001 across current wheels, also skipping any ids in
// `alsoReserved` (for assigning several ids in one multi-wheel split).
int NextFreeRadialId(const std::vector<int>& alsoReserved);

// True if a radials/<slug>/ subfolder already exists (slug-uniqueness probe).
bool RadialSlugInUse(const std::string& slug);

// Delete a wheel's staged files: radials/packs/<slug>.json + every
// radials/icons/emot3_<slug>_* PNG (the slug-prefixed naming makes this exact). Pure
// filesystem; the caller rescans + re-syncs binds. Missing files count as success.
bool RemoveRadialFiles(const std::string& slug);
