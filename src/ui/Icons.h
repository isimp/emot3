#pragma once

// Icon-related plumbing: path resolution, texture-cache keys, and the
// decorative draw helpers used by section headers and emote cells.

#include <string>
#include "imgui/imgui.h"

struct Emote;
struct MeMote;
struct Texture;
struct BundledIcon;   // Resources.h — ParseBundledIconRef returns a table pointer

// Resolve the on-disk path an emote PNG loads from. Empty IconPath →
// derive from the stable Id (`bow` → `bow.png`, lowercased) under
// g_IconsDir. Absolute path → used as-is. Relative → joined to g_IconsDir.
// Keyed on Id, not the localized Command, so bundled English-named art
// resolves regardless of the catalog's emote language.
std::string ResolveIconPath(const Emote& e);

// /me-mote icon path resolution — always returns a disk path: the explicit
// `m.IconPath` if set, otherwise the derived `icons/<id>.png` under
// g_IconsDir (matches the Emote pattern; the file may or may not exist —
// ResolveMeMoteIconSource stats it to decide which tier fires). Relative
// paths resolve against g_IconsDir; absolute paths pass through.
std::string ResolveMeMoteIconPath(const struct MeMote& m);

// Where a /me-mote's icon resolves from, in priority order. Single source
// of truth for that order, shared by the lazy texture loader
// (EnsureMeMoteTexture) and the /me-motes Options tab's status line (which labels it)
// so the two can never disagree. Shorter than the Emote chain by ONE tier:
// there's no BundledOfficial (the catalog is user-content, ArenaNet doesn't
// ship art for it). The remaining four tiers parallel the Emote chain so
// users get consistent affordances across both catalogs — including the
// `icons/<id>.png` drop-in convention and the opt-in BundledAI fallback.
enum class MeMoteIconSource {
    BundledChosen,    // IconPath is a `bundled:<bucket>:<name>` ref (icon picker)
    Custom,           // explicit IconPath on disk
    FolderOverride,   // no IconPath, but icons/<id>.png exists on disk
    BundledAI,        // bundled AI fallback (only when UseAIIconFallback is on)
    TextFallback      // no icon — styled letter button
};

// Resolve which source actually supplies m's icon, in the same order the
// loader loads. Does a disk stat, so it's for load / settings-screen use,
// not a per-frame render path. A missing explicit IconPath falls through
// to the next tier (Custom is returned only when the file exists on disk);
// the Options status line surfaces the missing-path case separately, same
// as DescribeIconSource does for Emotes.
MeMoteIconSource ResolveMeMoteIconSource(const struct MeMote& m);

// Where an emote's icon resolves from, in priority order. Single source of
// truth for that order, shared by the lazy texture loader (EnsureEmoteTexture,
// which loads on demand) and the Catalog tab's status line (DescribeIconSource,
// which labels it) so the two can never disagree. Custom and FolderOverride
// both mean "a PNG on disk at ResolveIconPath" - the loader treats them
// identically; only the status line distinguishes them (an explicit IconPath vs
// the icons/<id>.png drop-in).
enum class IconSource {
    BundledChosen,    // IconPath is a `bundled:<bucket>:<name>` ref (icon picker)
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

// --- "bundled:" IconPath scheme (icon picker) ----------------------------
// Lets an emote OR a /me-mote reference a bundled icon by name, instead of a
// filesystem path, so the in-app icon picker can assign any embedded icon to
// any entry. Stored in IconPath as "bundled:<bucket>:<name>"; the three
// buckets map to the three bundled tables. Safe vs. real paths (the path
// `isAbs` check keys on a colon at index 1; "bundled:" has its first colon at
// index 7), and additive/back-compat (older files simply never contain it).
// An explicitly chosen bundled icon resolves regardless of UseAIIconFallback
// (the setting only gates the auto AI fallback + the picker's AI buckets).
enum class BundledBucket { Official, AI, MeMoteAI };

// Build a ref string for a picked bundled icon.
std::string MakeBundledIconRef(BundledBucket bucket, const std::string& name);

// Parse a ref. Returns true ONLY for a well-formed ref whose <name> actually
// exists in the referenced table (a typo'd/stale ref returns false, so callers
// fall through to the normal resolution chain + letter fallback). On success
// yields the bundled table + count + bare name for TryLoadBundledIconBytes.
bool ParseBundledIconRef(const std::string& iconPath,
                         const BundledIcon*& outTable, int& outCount,
                         std::string& outName);

// Cheap predicate: true iff iconPath is a valid, resolvable bundled ref.
bool IsBundledIconRef(const std::string& iconPath);

// Sanitize an IconPath value at ingress (loader heal + picker write).
// Locks the field to the addons/emot3/icons folder and its subfolders;
// returns empty when the input names something outside that policy. Rules:
//   - Empty input -> empty output.
//   - "bundled:<bucket>:<name>" refs -> returned unchanged (validation of
//     the bucket/name happens later in ParseBundledIconRef).
//   - Absolute paths (drive-letter "X:\..." or UNC "\\...") -> rejected.
//   - Any '..' path segment -> rejected (no folder-escape).
//   - Leading "ui\" / "ui/" at the top level -> rejected (the ui/ subfolder
//     holds UI overrides drawn by DrawStarIcon / DrawTargetableDot / etc.,
//     not emote/me-mote icons; see entry.cpp's icons/ui/README.txt).
//   - Forward slashes normalized to backslashes; leading separators stripped;
//     ASCII whitespace trimmed.
// `outChanged` (optional) is set to true iff the returned value differs
// from `raw` — drives the loader's heal-on-load WARNING + re-save.
std::string SanitizeIconPath(const std::string& raw, bool* outChanged = nullptr);

// --- content-addressed texture cache --------------------------------------
// A resolved icon: its content cache key + how to load it. Two entries that
// resolve to the SAME image share a key -> one Nexus texture (dedup); a disk
// icon's key folds in mtime+size so an in-place edit reloads. key == "" means no
// texture (the cell draws a styled letter). Tag scheme (key = "EMOT3IC_" + tag):
// o:/ea:/ma: + name for bundled official/emote-AI/me-mote-AI; f:<lowerpath>:mtime:size
// for a disk file.
struct ResolvedIcon {
    std::string key;                       // content key, or "" for none
    enum class From { None, BundledMem, DiskFile } from = From::None;
    const BundledIcon* table = nullptr;    // BundledMem: table + name for TryLoadBundledIconBytes
    int                count = 0;
    std::string        name;
    std::string        path;               // DiskFile: absolute path for GetOrCreateFromFile
};

// Resolve an entry to its content key + load descriptor, switching on the shared
// ResolveIconSource / ResolveMeMoteIconSource order. Does the cold-path disk stat
// (cap-probe + mtime/size) for the file tiers. Also used by the picker to key its
// thumbnails into the SAME shared pool.
ResolvedIcon ResolveEmoteIcon(const Emote& e);
ResolvedIcon ResolveMeMoteIcon(const MeMote& m);

// Build a ResolvedIcon directly from a bundled (table + name) or disk (full
// path) source, so the icon picker keys its thumbnails into the SAME content
// pool as cells. MakeFileResolved cap-probes the header (key "" if over-cap or
// unreadable). ResolveEmoteIcon / ResolveMeMoteIcon route through these.
ResolvedIcon MakeBundledResolved(const BundledIcon* table, int count, const std::string& name);
ResolvedIcon MakeFileResolved(const std::string& fullPath);

// Load (once, dedup-aware) a resolved icon into the Nexus cache + return it
// (null if None / over-cap / still loading). Used by the lazy picker grid; the
// cell path goes through Ensure*Texture below.
Texture* EnsureResolved(const ResolvedIcon& r);

// Lazy on-demand load + lookup for the render path: returns the entry's texture
// (loading it on first show), memoized by Id per catalog epoch so the hot path
// is a map lookup + Textures.Get with no per-frame disk I/O. The off-screen cull
// keeps these to visible cells. An edit (MarkEmotesDirty / MarkMeMotesDirty)
// clears the memo so changed/added entries re-resolve. No locking: pass the
// entity by reference; safe from a cell/row loop holding the catalog mutex.
Texture* EnsureEmoteTexture(const Emote& e);
Texture* EnsureMeMoteTexture(const MeMote& m);

// Drop one entry's memo so the next render re-resolves it (the Refresh button):
// re-stats the file -> a changed mtime/size reloads, an unchanged file is a no-op.
void InvalidateEmoteIcon(const std::string& id);
void InvalidateMeMoteIcon(const std::string& id);

// --- user-icon dimension cap (icon_cache.json: max_icon_dim) --------------
// Header-only image probe for USER-supplied icon files (bundled icons are
// exempt). Reads JUST the PNG/JPEG header off disk (no decode) to get the
// pixel width/height, so an oversized file can be skipped before it reserves a
// big GPU texture. A 4096x4096 PNG is 64 MB of VRAM drawn at thumbnail size.
enum class IconProbe {
    Ok,          // a recognized header within the cap -> safe to load
    TooLarge,    // recognized, but width or height exceeds g_IconCache.maxIconDim
    Unreadable   // not a PNG/JPEG header we can parse (truncated / wrong format)
};
// Probe `path`; on a recognized header fills outW/outH (0 otherwise). Callers
// apply policy: the picker (PNG-only folder) skips+logs Unreadable; a generic
// disk loader may treat Unreadable as "let the decoder try" (best-effort).
IconProbe ProbeIconFile(const std::string& path, int& outW, int& outH);
// Convenience: true iff ProbeIconFile(path) == Ok (within the cap).
bool IconFileWithinCap(const std::string& path);

// --- dev: deduped icon-pool accounting (memory monitor) -------------------
// The full content pool we've loaded, split by current use. Each distinct
// texture counts ONCE (shared icons aren't double-counted), and orphaned/churn
// slots are visible (the old per-catalog count couldn't see them).
struct IconPoolUsage {
    size_t totalCount = 0, totalBytes = 0;   // every distinct loaded content texture
    size_t inUseCount = 0, inUseBytes = 0;   // drawn this / last frame
    size_t idleCount  = 0, idleBytes  = 0;   // loaded but off-screen or orphaned
};
#ifdef EMOT3_DEVTOOLS
void IconPoolStats(IconPoolUsage& out);
// One-shot pool reconcile: probe each catalog entry's content key and record the
// ones already resident in Nexus (e.g. textures that survived an addon hot-reload
// while their cells are off-screen). Probe-only - never loads. Call once per load.
void ReconcileResidentPool();
#endif

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
//   DrawMeMoteIndicator: small upper-right corner accent marking a /me-mote
//     cell. Sources the bundled (or icons/ui/-overridden) "me_mote_dot.png"
//     UI texture. Shares the same anchor as DrawTargetableDot — they never
//     co-occur (/me-mote cells are never IsTargetable). indSz is the
//     diameter in pixels (sized like the target dot so it tracks the
//     icon-scale slider).
void DrawLockOverlay();
void DrawTargetableDot(float dotSz, float alphaMul);
void DrawMeMoteIndicator(float indSz, float alphaMul);
