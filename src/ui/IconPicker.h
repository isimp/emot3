#pragma once

// In-app icon picker: a modal grid of every bundled icon (official + AI +
// /me-mote AI) plus the PNGs in addons/emot3/icons, for assigning an icon to an
// emote or a /me-mote. Picking an embedded icon stores a "bundled:<bucket>:<name>"
// ref; picking a folder PNG stores its relative filename. Both route through
// ApplyIconPathToTarget (IconBrowse.h), which writes the IconPath and bumps the
// catalog dirty epoch.

#include <cstddef>
#include <string>

#include "IconBrowse.h"   // EIconTargetKind

// Request the picker open for a given catalog entry. The next RenderIconPicker
// frame actually opens the modal (and primes/scans the icon thumbnails).
// Safe to call repeatedly; a second call before the first opens just retargets.
//
// `currentIconPath` is the target's IconPath right now (used only to highlight
// the matching thumbnail). The CALLER passes it rather than the picker reading
// the catalog, because this is invoked from the editors WHILE they hold
// g_EmotesMutex / g_MeMotesMutex — the picker must not re-lock (non-recursive
// std::mutex; re-entry = crash).
void OpenIconPicker(EIconTargetKind kind, const std::string& targetId,
                    const std::string& currentIconPath);

// Render the picker modal. Call ONCE per frame from AddonOptions, inside the
// Options window. Renders nothing until opened.
void RenderIconPicker();

// Dev/MemoryMonitor introspection: count + estimated bytes (Width*Height*4) of
// the picker's EMOT3_PICK_* thumbnail textures currently resident in Nexus'
// cache. Zero until the picker is first opened (textures are primed lazily).
void IconPickerTextureStats(std::size_t& outCount, std::size_t& outBytes);
