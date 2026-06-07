#pragma once
//
// Power-user tuning knobs for the icon cache (picker + texture loading).
//
// Read at AddonLoad from <addon dir>/icon_cache.json IF it exists; otherwise the
// coded defaults below are used. The ONLY writer is the dev-only tuner
// (SaveIconCacheConfig) - no non-dev / Options path writes it, so it stays a
// separate file from settings.json (which the UI rewrites on every change). The
// per-field comments below document the keys, defaults, and ranges.

#include <string>

struct IconCacheConfig {
    // Max width/height (px) for a USER-supplied icon FILE (PNGs under
    // addons/emot3/icons/). A file larger than this is skipped - letter
    // fallback for a cell, dropped from the picker grid - and logged, so one
    // oversized PNG can't reserve a huge GPU texture (a 512x512 icon is already
    // 1 MB). Bundled icons are exempt. Clamped to [16, 512].
    int maxIconDim = 128;

    // Cap on how many PNGs the icon picker lists from the icons/ folder, so a
    // pathological folder can't enumerate unbounded thumbnails. Clamped to
    // [0, 2048].
    int maxFolderIcons = 512;

    // HARD ceiling (MB) on total icon GPU memory: once the loaded icon pool
    // passes this, new icons stop loading (letter fallback) and a one-time log
    // fires. Default 128 MB - with the 128px dimension cap that's ~2048 distinct
    // icons, far beyond any realistic catalog, so it never bites normal use but
    // bounds a runaway (a huge catalog or churn). Set 0 to disable. The per-icon
    // dimension cap + the folder/count caps are the first line; this is the
    // backstop. Clamped to [0, 512].
    int poolBudgetMB = 128;
};

extern IconCacheConfig g_IconCache;

// Path to icon_cache.json (set at AddonLoad). The tuner devtool writes here.
extern std::string g_IconCacheConfigPath;

// Load <path> (icon_cache.json) if it exists, overriding each field above
// independently (clamped to its range); an absent file, a parse error, or a
// missing/mistyped key leaves that field at its coded default.
void LoadIconCacheConfig(const std::string& path);

// Write the current g_IconCache to <path> (created on first write). Called by
// the dev tuner when a value changes - the file isn't seeded otherwise, so it
// exists only once the user has set their own parameters.
void SaveIconCacheConfig(const std::string& path);
