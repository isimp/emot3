#include "IconCacheConfig.h"

#include "JsonUtil.h"   // jsonutil::GetInt (never-throw, type-checked)
#include "Logging.h"
#include "AtomicFile.h" // AtomicWriteFile (shared crash-safe temp+rename)

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

using json = nlohmann::json;
using namespace jsonutil;

IconCacheConfig g_IconCache;
std::string     g_IconCacheConfigPath;

namespace {
int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
}

void LoadIconCacheConfig(const std::string& path) {
    // Absent file -> keep the coded defaults (the common case; nothing written).
    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_DEBUG("icon_cache.json not present; using coded icon-cache defaults");
        return;
    }
    json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        LOG_WARNING("icon_cache.json parse error (%s); using icon-cache defaults", e.what());
        return;
    }
    // Each field independent + clamped; a missing/mistyped key keeps its default
    // (GetInt is never-throw and type-checked).
    g_IconCache.maxIconDim     = Clamp(GetInt(j, "max_icon_dim",     g_IconCache.maxIconDim),     16, 512);
    g_IconCache.maxFolderIcons = Clamp(GetInt(j, "max_folder_icons", g_IconCache.maxFolderIcons), 0,  2048);
    g_IconCache.poolBudgetMB   = Clamp(GetInt(j, "pool_budget_mb",   g_IconCache.poolBudgetMB),   0,  512);
    LOG_INFO("icon_cache.json loaded: max_icon_dim=%d max_folder_icons=%d pool_budget_mb=%d",
             g_IconCache.maxIconDim, g_IconCache.maxFolderIcons, g_IconCache.poolBudgetMB);
}

void SaveIconCacheConfig(const std::string& path) {
    if (path.empty()) return;
    // Hand-rolled writer (3 ints) - keeps the on-disk shape stable + matches the
    // keys LoadIconCacheConfig reads. Only the dev tuner calls this.
    std::ostringstream f;
    f << "{\n";
    f << "  \"max_icon_dim\": "     << g_IconCache.maxIconDim     << ",\n";
    f << "  \"max_folder_icons\": " << g_IconCache.maxFolderIcons << ",\n";
    f << "  \"pool_budget_mb\": "   << g_IconCache.poolBudgetMB   << "\n";
    f << "}\n";
    // Crash-safe temp+rename so a mid-write crash can't leave a half-truncated
    // icon_cache.json that the next load would discard (dropping the user's settings).
    if (!AtomicWriteFile(path, f.str())) return;
    LOG_INFO("wrote icon_cache.json: max_icon_dim=%d max_folder_icons=%d pool_budget_mb=%d",
             g_IconCache.maxIconDim, g_IconCache.maxFolderIcons, g_IconCache.poolBudgetMB);
}
