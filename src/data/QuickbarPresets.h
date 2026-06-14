#pragma once

#include <string>
#include <vector>

#include "Settings.h"  // EViewMode

// A saved snapshot of the Quickbar's look/behaviour settings plus its window
// geometry. Persisted one-file-per-preset under g_PresetsDir
// (addons/emot3/presets/<slug>.json). Fully user-editable and deletable;
// included defaults are seeded to that folder on first run, after which they
// behave like any other preset.
struct QuickbarPreset {
    std::string Name;   // display name (stored as the file's "name")
    std::string File;   // on-disk filename incl. ".json"; "" until first written

    // --- 18-field Quickbar settings snapshot (mirrors g_Settings.QuickbarXxx).
    // Adding a Quickbar setting that should travel with presets means editing
    // five spots in QuickbarPresets.cpp: CaptureQuickbarPreset, ApplyQuickbar-
    // Preset, ParsePresetJson, PresetToJson and QuickbarPresetMatchesCurrent.
    // Kept explicit to match the field-list style already used in Settings.cpp.
    EViewMode ViewMode        = EViewMode::Icon;
    float     IconScale       = 1.0f;
    bool      UseDropdown      = false;
    bool      ShowCategoryBar  = true;
    bool      HorizontalScroll = false;
    bool          SnapWindow       = false;
    EQbScrollSnap SnapScroll       = EQbScrollSnap::Cells;
    bool      AllowResize      = true;
    bool      AllowMove        = true;
    bool      ShowBg           = true;
    bool      ShowTitle        = true;
    bool      ClickThrough     = false;
    bool      HighContrast     = false;
    EQbScrollIndicator ScrollIndicator = EQbScrollIndicator::Scrollbar;
    bool      ShowTooltips     = true;
    EWheelCycle WheelCycle     = EWheelCycle::OverBar;
    bool      WheelCycleWrap   = true;
    bool      ScrollWrap       = false;
    bool      CloseOnEsc       = false;
    EQbCombat CombatBehavior   = EQbCombat::Off;
    // How this Quickbar shows an unusable emote (Grey/Hide/Normal). Only acts
    // while the global BlockUnusableEmotes is on. Old presets (no key) inherit
    // the migrated active value - see ParsePresetJson.
    EUnusableBehavior UnusableDisplay = EUnusableBehavior::Grey;

    // --- optional window geometry (screen-space; clamped to the viewport on
    // apply). HasGeometry is false for a preset captured while the Quickbar
    // had never been drawn.
    bool  HasGeometry = false;
    float X = 0.f, Y = 0.f, W = 0.f, H = 0.f;
};

// In-memory list, a mirror of the presets/ folder, sorted by Name.
extern std::vector<QuickbarPreset> g_QuickbarPresets;

// Ensure presets/ exists (seeding the embedded defaults on first run), then
// (re)load every presets/*.json into g_QuickbarPresets. Call once at
// AddonLoad after g_PresetsDir is set.
void LoadQuickbarPresets();

// Copy the live g_Settings Quickbar fields into p; also captures the live
// window geometry when g_QbGeometryValid (otherwise leaves p's geometry as-is,
// so Update on a hidden Quickbar preserves the stored geometry).
void CaptureQuickbarPreset(QuickbarPreset& p);

// Copy p's settings into g_Settings and, when p.HasGeometry, queue the one-shot
// window-geometry apply consumed by the Quickbar's next render. Does NOT touch
// ShowQuickbar or the active category. The caller must re-apply side-effectful
// settings afterwards (ApplyQbCloseOnEsc) and persist g_Settings.
void ApplyQuickbarPreset(const QuickbarPreset& p);

// True when every captured field of p equals the live g_Settings AND - only
// when both p.HasGeometry and g_QbGeometryValid - the live geometry matches
// within ~1px. Drives the "modified" hint / Update button guard.
bool QuickbarPresetMatchesCurrent(const QuickbarPreset& p);

// Write p to disk. Assigns p.File from a unique slug of p.Name when empty.
// Returns false on I/O error (logged). Used by New and Update.
bool WriteQuickbarPreset(QuickbarPreset& p);

// Rename p to newName: updates p.Name and, when the name's slug changes,
// renames the backing file (writes the new one, deletes the old). Returns
// false on I/O error (logged).
bool RenameQuickbarPreset(QuickbarPreset& p, const std::string& newName);

// Delete p's backing file. Returns false on I/O error (logged); an
// already-missing file counts as success.
bool DeleteQuickbarPresetFile(const QuickbarPreset& p);

// True if a preset other than the one at excludeIndex (-1 = none) already uses
// this name (case-insensitive). For New / Rename validation.
bool QuickbarPresetNameExists(const std::string& name, int excludeIndex);
