#pragma once

// Shared "apply an icon path to a catalog entry" helper. Historically lived
// alongside the OS "From file..." file-dialog plumbing (StartIconBrowse /
// DrainIconBrowse on a detached worker), which got removed once the in-app
// icon picker (ui/IconPicker.cpp) replaced that affordance. The routing
// helper survived because the picker calls it directly — and any future
// "set this entry's IconPath to X" writer can do the same.
//
// The kind enum stays as the cross-catalog routing tag (Emote vs MeMote);
// adding a third kind later means appending a value + extending the
// ApplyIconPathToTarget branch.

#include <string>

// Which catalog the icon write targets. The picker passes one of these so
// ApplyIconPathToTarget routes to the right catalog's IconPath + persistence
// (Emote -> emotes.json + MarkEmotesDirty; MeMote -> me_motes.json +
// MarkMeMotesDirty).
enum class EIconTargetKind { Emote, MeMote };

// Apply an icon choice (a file path OR a "bundled:<bucket>:<name>" ref) to the
// catalog entry of the given kind+Id: set its IconPath, persist that catalog's
// JSON, and bump its dirty flag so the texture loader reloads next frame.
// Returns true if the target still existed. Used by the in-app icon picker;
// render-thread only.
bool ApplyIconPathToTarget(EIconTargetKind kind, const std::string& targetId,
                           const std::string& path);
