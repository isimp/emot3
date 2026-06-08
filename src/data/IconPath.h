#pragma once

// Pure IconPath value logic (no imgui, no Nexus), extracted from ui/Icons so the
// data layer can validate IconPath at JSON-load ingress without depending on the
// UI layer: the "bundled:<bucket>:<name>" scheme + SanitizeIconPath. Depends only
// on core/StringUtil (IsAbsolutePath) and resources/Resources (bundled tables).

#include <string>

struct BundledIcon;   // resources/Resources.h - ParseBundledIconRef yields a table ptr

// --- "bundled:" IconPath scheme (icon picker) ----------------------------
// Lets an emote OR a /me-mote reference a bundled icon by name, instead of a
// filesystem path, so the in-app icon picker can assign any embedded icon to
// any entry. Stored in IconPath as "bundled:<bucket>:<name>"; the three buckets
// map to the three bundled tables. Safe vs. real paths (the path isAbs check
// keys on a colon at index 1; "bundled:" has its first colon at index 7), and
// additive/back-compat (older files simply never contain it). An explicitly
// chosen bundled icon resolves regardless of UseAIIconFallback.
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

// Sanitize an IconPath value at ingress (loader heal + picker write). Locks the
// field to the addons/emot3/icons folder and its subfolders; returns empty when
// the input names something outside that policy. Rules:
//   - Empty input -> empty output.
//   - "bundled:<bucket>:<name>" refs -> returned unchanged (bucket/name validity
//     is checked later in ParseBundledIconRef).
//   - Absolute paths (drive-letter "X:\..." or UNC "\\...") -> rejected.
//   - Any '..' path segment -> rejected (no folder-escape).
//   - Leading "ui\" / "ui/" at the top level -> rejected (that subfolder holds
//     UI overrides, not emote/me-mote icons; see entry.cpp's icons/ui README).
//   - Forward slashes normalized to backslashes; leading separators stripped;
//     ASCII whitespace trimmed.
// `outChanged` (optional) is set true iff the returned value differs from `raw` -
// drives the loader's heal-on-load WARNING + re-save.
std::string SanitizeIconPath(const std::string& raw, bool* outChanged = nullptr);
