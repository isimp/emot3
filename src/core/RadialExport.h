#pragma once

// RadialMenus pack writer — turns page-assigned emote/me-mote refs into a staged
// logical export (one or more page packs sharing a group id) under
// addons/emot3/radials/packs + radials/icons, then rescans the record
// (data/RadialExports) and re-syncs the Nexus binds the wheel items invoke. This is
// the ONLY place that knows the RadialMenus on-disk format; the enum ints are named
// constants with a version pin (see RadialExport.cpp), so a format change is a
// single-file fix. We only ever WRITE packs (never read RadialMenus runtime state),
// so a format drift degrades a wheel, never crashes.
//
// A logical export is a GROUP: `pages` is one inner vector of refs per page (the
// wizard does the per-item page assignment + capacity check up front). One page ->
// one simple wheel; N pages -> a split, all sharing emot3_group.

#include <string>
#include <vector>

#include "RadialExports.h"   // RadialItemRef, RadialWheelOptions, RadialExport

// Outcome of an export, for the wizard's "done" panel. One entry per page written.
struct RadialExportResult {
    bool                     ok = false;
    int                      wheelsWritten = 0;  // pages written
    std::vector<int>         ids;    // assigned menu ids (>= 90001), one per page
    std::vector<std::string> names;  // pack names ("emot3: <name>") for the KB_RADIAL hint
    std::string              error;  // non-empty on failure (short text)
};

// Create a NEW logical export from favorites category `sourceCategory`, named
// `baseName`, with `pages` already partitioned by the wizard (each inner vector <=
// capacity). Allocates a fresh group id + one menu id per page, writes each page pack
// + its icons, then rescans + SyncEmoteBinds. The subset/"partial" flag (for the drift
// check) is derived from whether the pages' union is the full default category.
RadialExportResult ExportGroup(const std::string& sourceCategory,
                               const std::string& baseName,
                               const std::vector<std::vector<RadialItemRef>>& pages,
                               const RadialWheelOptions& options);

// Rewrite an existing logical export in place, reusing its `group` id: deletes the
// group's current packs/icons, then writes `pages` fresh (page menu ids reassigned).
// Used by the wizard's Edit/Save. Rescans + SyncEmoteBinds.
RadialExportResult EditGroup(const std::string& group, const std::string& sourceCategory,
                             const std::string& baseName,
                             const std::vector<std::vector<RadialItemRef>>& pages,
                             const RadialWheelOptions& options);

// Rename a logical export: rewrite every page's pack "Name" under a re-slugged group
// (writing the new packs, deleting the old) so stale files never linger. Items / page
// layout / options preserved. Rescans + SyncEmoteBinds. False on I/O error.
bool RenameGroup(const std::string& group, const std::string& newName);

// Remove a logical export entirely (all page packs + icons), then rescan +
// SyncEmoteBinds so its radial-only binds drop (personal binds for the same entries
// persist).
bool RemoveGroup(const std::string& group);

// A favorites category was renamed: update the emot3_source_category marker of every
// staged pack that pointed at the old name, so exported wheels stay linked to it
// (drift/edit keep working). The marker is emot3-only, so deployed copies need no
// change. No-op when nothing references the old name. Call from the rename site.
void RetargetExportsCategory(const std::string& oldCategory,
                             const std::string& newCategory);
