#pragma once

// Icon-related plumbing: path resolution, texture-cache keys, and the
// decorative draw helpers used by section headers and emote cells.

#include <string>
#include "imgui/imgui.h"

struct Emote;
struct Texture;

// Resolve the on-disk path an emote PNG loads from. Empty IconPath →
// derive from the stable Id (`bow` → `bow.png`, lowercased) under
// g_IconsDir. Absolute path → used as-is. Relative → joined to g_IconsDir.
// Keyed on Id, not the localized Command, so bundled English-named art
// resolves regardless of the catalog's emote language.
std::string ResolveIconPath(const Emote& e);

// Where an emote's icon resolves from, in priority order. Single source of
// truth for that order, shared by the texture loader (LoadEmoteTextures, which
// picks what to load) and the Catalog tab's status line (DescribeIconSource,
// which labels it) so the two can never disagree. Custom and FolderOverride
// both mean "a PNG on disk at ResolveIconPath" - the loader treats them
// identically; only the status line distinguishes them (an explicit IconPath vs
// the icons/<id>.png drop-in).
enum class IconSource {
    Custom,           // explicit IconPath on disk
    FolderOverride,   // no IconPath, but icons/<id>.png exists on disk
    BundledOfficial,  // bundled ArenaNet artwork
    BundledAI,        // bundled AI fallback (only when UseAIIconFallback is on)
    TextFallback      // no icon - styled letter button
};

// Resolve which source actually supplies e's icon, in the same order the loader
// loads. Does a disk stat, so it's for load / settings-screen use, not a
// per-frame render path. A missing explicit IconPath falls through to the
// bundled art (Custom is returned only when that file exists), matching the
// loader; the status line surfaces the missing-path case separately.
IconSource ResolveIconSource(const Emote& e);

// `bow` → `EMOT3_bow`. Lowercased + slash-stripped. Pass the emote's
// stable Id so the cache key is language-independent.
std::string EmoteCacheKey(const std::string& id);

// Lookup-only - returns nullptr if the texture isn't loaded yet. Pass Id.
Texture* GetEmoteTexture(const std::string& id);

// Section-header glyphs. Each one prefers the corresponding PNG under
// addons/emot3/icons/ui/ when present; otherwise falls back to the
// hand-drawn version unchanged. Caller-supplied alpha (via `col`) is
// preserved in the texture path so dimming still works.
void DrawStarIcon(ImVec2 c, float r, ImU32 col);
void DrawPaperclipIcon(ImVec2 c, float r, ImU32 col);

// Disclosure triangle for collapsible section headers: right-pointing when
// collapsed, down-pointing when expanded. Drawn directly (no texture) since the
// bundled font has no disclosure glyph. `r` is the half-extent around center `c`.
void DrawCollapseArrow(ImVec2 c, float r, bool collapsed, ImU32 col);

// Trash-can glyph for the "drop to remove from favorites" zone. Drawn directly
// (line art, no texture / no font glyph). `r` is the half-extent around `c`. Pass
// a draw list to target it (e.g. the foreground list for an always-on-top
// overlay); null uses the current window's draw list.
void DrawTrashIcon(ImVec2 c, float r, ImU32 col, ImDrawList* dl = nullptr);

// Cell overlays.
//   DrawLockOverlay: centered over the last-rendered item; opaque enough
//     to stay readable when the cell itself is dimmed.
//   DrawTargetableDot: small upper-right corner indicator. dotSz is the
//     caller-computed diameter (kept consistent across view modes); alpha is
//     caller-supplied so the dot dims with the cell.
//   DrawMeMoteIndicator: small upper-LEFT corner accent (a chat-blue filled
//     circle) marking a /me-mote cell. Different corner from the target dot
//     so the two never visually clash. Drawn procedurally (no texture);
//     indSz is the diameter in pixels.
void DrawLockOverlay();
void DrawTargetableDot(float dotSz, float alphaMul);
void DrawMeMoteIndicator(float indSz, float alphaMul);
