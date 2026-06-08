#pragma once

// RadialMenus pack writer — turns a set of emote/me-mote refs into one or more
// staged wheels under addons/emot3/radials/<slug>/ (pack JSON + icons), then
// rescans the record (data/RadialExports) and re-syncs the Nexus binds the wheel
// items invoke. This is the ONLY place that knows the RadialMenus on-disk format;
// the enum ints are named constants with a version pin (see RadialExport.cpp), so a
// format change is a single-file fix. We only ever WRITE packs (never read
// RadialMenus runtime state), so a format drift degrades a wheel, never crashes.
//
// Capacity (12 Normal / 8 Small) is enforced here: an over-capacity export either
// splits into multiple wheels (autoSplit) or is truncated defensively (the wizard
// prevents the latter by requiring a subset pick or split up front).

#include <string>
#include <vector>

#include "RadialExports.h"   // RadialItemRef, RadialWheelOptions, RadialExport

// Outcome of an export, for the wizard's "done" panel.
struct RadialExportResult {
    bool                     ok = false;
    int                      wheelsWritten = 0;
    std::vector<int>         ids;    // assigned menu ids (>= 90001)
    std::vector<std::string> names;  // pack names ("emot3: <name>") for the KB_RADIAL hint
    std::string              error;  // non-empty on failure (i18n key or short text)
};

// Export `items` (already subset-selected, in display order) from favorites
// category `sourceCategory` as one or more staged wheels named `baseName`. Splits
// into multiple wheels (baseName "(1)", "(2)", … / unique slugs + ids) when the
// count exceeds the chosen Type's capacity AND autoSplit is true; otherwise a single
// wheel (truncated to capacity defensively). Rescans + SyncEmoteBinds before
// returning. Does NOT deploy into RadialMenus' folder (base build hints the manual
// copy; +plus deploys via src/plus/RadialDeploy).
RadialExportResult ExportCategoryAsWheels(const std::string& sourceCategory,
                                          const std::string& baseName,
                                          const std::vector<RadialItemRef>& items,
                                          const RadialWheelOptions& options,
                                          bool autoSplit);

// Rewrite one existing wheel in place (same slug + id): the "Re-export" action
// (re-snapshot the source category) and any options edit. Clears the wheel's stale
// icons first. Rescans + SyncEmoteBinds.
bool ReExportWheel(const std::string& slug, int id, const std::string& name,
                   const std::string& sourceCategory, bool partial,
                   const std::vector<RadialItemRef>& items,
                   const RadialWheelOptions& options);

// Rename a wheel: rewrite the pack "Name" and re-slug the subfolder (writing the new
// one, deleting the old) so stale art never lingers. Items/id/options preserved.
// Rescans (+ SyncEmoteBinds when the slug changed). Returns false on I/O error.
bool RenameWheel(const std::string& oldSlug, const std::string& newName);

// Remove a wheel entirely (delete its subfolder), then rescan + SyncEmoteBinds so
// its radial-only binds drop (user binds for the same entries persist).
bool RemoveWheel(const std::string& slug);
