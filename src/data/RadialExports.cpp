#include "RadialExports.h"

#include "Globals.h"      // g_RadialsDir (+ Windows.h)
#include "EmoteBinds.h"   // ParseEmoteBindIdentifier - recover refs from item Actions
#include "StringUtil.h"   // ToLower
#include "Logging.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using json = nlohmann::json;

std::vector<RadialExport> g_RadialExports;
std::mutex                g_RadialExportsMutex;

const char* const kRadialPackNamePrefix = "emot3: ";

namespace {

bool DirExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

void EnsureRadialsDir() {
    if (g_RadialsDir.empty()) return;
    if (!DirExists(g_RadialsDir)) CreateDirectoryA(g_RadialsDir.c_str(), nullptr);
}

// Recursively delete a directory tree (the staged radials/<slug>/ folder: a flat
// dir + an icons/ subdir of PNGs, so two levels deep at most). ASCII paths under
// addons/emot3/ — same assumption the rest of the addon's file code makes.
bool RemoveDirRecursive(const std::string& dir) {
    std::string pattern = dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string name = fd.cFileName;
            if (name == "." || name == "..") continue;
            std::string full = dir + "\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirRecursive(full);
            else                                                DeleteFileA(full.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(dir.c_str()) != 0;
}

// Parse one staged pack into w (w.Slug already set). Returns false on missing /
// unreadable / non-object JSON, leaving the caller to flag a broken row.
bool ReadPack(const std::string& packPath, RadialExport& w) {
    std::ifstream f(packPath, std::ios::binary);
    if (!f.is_open()) return false;
    json j;
    try { f >> j; }
    catch (const json::parse_error& e) {
        LOG_WARNING("radials: %s parse error at byte %zu: %s",
                    packPath.c_str(), (size_t)e.byte, e.what());
        return false;
    }
    if (!j.is_object()) return false;

    w.Id             = j.value("ID", 0);
    w.SourceCategory = j.value("emot3_source_category", std::string());

    std::string packName = j.value("Name", std::string());
    const size_t pfx = std::strlen(kRadialPackNamePrefix);
    w.Name = (packName.rfind(kRadialPackNamePrefix, 0) == 0) ? packName.substr(pfx)
                                                             : packName;
    if (w.Name.empty()) w.Name = w.Slug;

    // Native RadialMenus options (recoverable directly); emot3-namespaced markers
    // for what isn't (the source category above + the gate flag below).
    w.Options.Small               = (j.value("Type", 1) == 2);  // 1 Normal / 2 Small
    w.Options.SelectionMode       = j.value("SelectionMode", 3);
    w.Options.Scale               = j.value("Scale", 1.0f);
    w.Options.IconScale           = j.value("IconScale", 1.0f);
    w.Options.ShowItemNameTooltip = j.value("ShowItemNameTooltip", false);

    // Recover refs (and detect gating) from the items' Action identifiers.
    bool anyVisibility = false;
    if (j.contains("Items") && j["Items"].is_array()) {
        for (const auto& it : j["Items"]) {
            if (!it.is_object()) continue;
            if (it.contains("Visibility")) anyVisibility = true;
            if (!it.contains("Actions") || !it["Actions"].is_array() ||
                it["Actions"].empty())
                continue;
            const auto& a0 = it["Actions"][0];
            if (!a0.is_object()) continue;
            std::string idn = a0.value("Identifier", std::string());
            RadialItemRef ref;
            if (ParseEmoteBindIdentifier(idn, ref.Type, ref.Id, ref.Variant))
                w.Items.push_back(std::move(ref));
        }
    }
    // Gate flag: explicit marker if present, else inferred from any item carrying a
    // Visibility object (older/hand-edited packs).
    w.Options.GateByState = j.value("emot3_gate", anyVisibility);
    return true;
}

}  // namespace

void LoadRadialExports() {
    std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
    g_RadialExports.clear();
    if (g_RadialsDir.empty()) return;
    EnsureRadialsDir();

    std::string pattern = g_RadialsDir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::string slug = fd.cFileName;
            if (slug == "." || slug == "..") continue;
            std::string packPath = g_RadialsDir + "\\" + slug + "\\" + slug + ".json";
            RadialExport w;
            w.Slug = slug;
            if (!ReadPack(packPath, w)) {
                w.ParseError = true;
                if (w.Name.empty()) w.Name = slug;
            }
            g_RadialExports.push_back(std::move(w));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    std::sort(g_RadialExports.begin(), g_RadialExports.end(),
              [](const RadialExport& a, const RadialExport& b) {
                  return ToLower(a.Name) < ToLower(b.Name);
              });
    LOG_INFO("radials: loaded %d wheel(s)", (int)g_RadialExports.size());
}

std::vector<std::string> ExportedSourceCategories() {
    std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
    std::vector<std::string> out;
    for (const auto& w : g_RadialExports)
        if (!w.SourceCategory.empty()) out.push_back(w.SourceCategory);
    return out;
}

std::vector<std::string> RadialWheelsContaining(EFavoriteRefType type,
                                                const std::string& id) {
    std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
    std::vector<std::string> out;
    for (const auto& w : g_RadialExports) {
        for (const auto& it : w.Items) {
            if (it.Type == type && it.Id == id) { out.push_back(w.Name); break; }
        }
    }
    return out;
}

int NextFreeRadialId(const std::vector<int>& alsoReserved) {
    std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
    int id = 90001;
    auto taken = [&](int candidate) {
        for (const auto& w : g_RadialExports) if (w.Id == candidate) return true;
        for (int r : alsoReserved)            if (r == candidate)    return true;
        return false;
    };
    while (taken(id)) ++id;
    return id;
}

bool RadialSlugInUse(const std::string& slug) {
    if (g_RadialsDir.empty() || slug.empty()) return false;
    return DirExists(g_RadialsDir + "\\" + slug);
}

bool RemoveRadialDir(const std::string& slug) {
    if (g_RadialsDir.empty() || slug.empty()) return false;
    std::string dir = g_RadialsDir + "\\" + slug;
    if (!DirExists(dir)) return true;  // already gone counts as success
    if (!RemoveDirRecursive(dir)) {
        LOG_WARNING("radials: failed to remove %s", dir.c_str());
        return false;
    }
    LOG_INFO("radials: removed wheel subfolder %s", slug.c_str());
    return true;
}
