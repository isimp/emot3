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
#include <unordered_set>
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
    std::string packs = g_RadialsDir + "\\packs";
    std::string icons = g_RadialsDir + "\\icons";
    if (!DirExists(packs)) CreateDirectoryA(packs.c_str(), nullptr);
    if (!DirExists(icons)) CreateDirectoryA(icons.c_str(), nullptr);
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
    w.Partial        = j.value("emot3_partial", false);
    w.Group          = j.value("emot3_group", w.Slug);  // legacy/hand-made -> own group
    w.Page           = j.value("emot3_page", 1);

    // Logical export name: prefer the emot3_name marker; fall back to the pack "Name"
    // minus the "emot3: " prefix (legacy / hand-made packs).
    w.Name = j.value("emot3_name", std::string());
    if (w.Name.empty()) {
        std::string packName = j.value("Name", std::string());
        const size_t pfx = std::strlen(kRadialPackNamePrefix);
        w.Name = (packName.rfind(kRadialPackNamePrefix, 0) == 0) ? packName.substr(pfx)
                                                                 : packName;
    }
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

std::string RadialPacksDir() { return g_RadialsDir.empty() ? std::string() : g_RadialsDir + "\\packs"; }
std::string RadialIconsDir() { return g_RadialsDir.empty() ? std::string() : g_RadialsDir + "\\icons"; }

void LoadRadialExports() {
    std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
    g_RadialExports.clear();
    if (g_RadialsDir.empty()) return;
    EnsureRadialsDir();

    const std::string packsDir = RadialPacksDir();
    std::string pattern = packsDir + "\\*.json";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string file = fd.cFileName;                 // "<slug>.json"
            std::string slug = file.substr(0, file.size() - 5);  // drop ".json"
            RadialExport w;
            w.Slug = slug;
            if (!ReadPack(packsDir + "\\" + file, w)) {
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

std::vector<RadialExport> WheelsInGroup(const std::string& group) {
    std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
    std::vector<RadialExport> out;
    for (const auto& w : g_RadialExports)
        if (w.Group == group) out.push_back(w);
    std::sort(out.begin(), out.end(),
              [](const RadialExport& a, const RadialExport& b) { return a.Page < b.Page; });
    return out;
}

std::vector<std::string> RadialWheelsContaining(EFavoriteRefType type,
                                                const std::string& id) {
    std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
    std::vector<std::string> out;
    std::unordered_set<std::string> seenGroups;  // one entry per logical export
    for (const auto& w : g_RadialExports) {
        for (const auto& it : w.Items) {
            if (it.Type == type && it.Id == id) {
                if (seenGroups.insert(w.Group).second) out.push_back(w.Name);
                break;
            }
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
    std::string pack = RadialPacksDir() + "\\" + slug + ".json";
    return GetFileAttributesA(pack.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool RadialGroupInUse(const std::string& group) {
    if (group.empty()) return false;
    std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
    for (const auto& w : g_RadialExports) if (w.Group == group) return true;
    return false;
}

bool RemoveRadialFiles(const std::string& slug) {
    if (g_RadialsDir.empty() || slug.empty()) return false;
    bool ok = true;

    // pack: radials/packs/<slug>.json
    std::string pack = RadialPacksDir() + "\\" + slug + ".json";
    if (!DeleteFileA(pack.c_str()) &&
        GetFileAttributesA(pack.c_str()) != INVALID_FILE_ATTRIBUTES) {
        LOG_WARNING("radials: failed to remove %s", pack.c_str());
        ok = false;
    }

    // icons: radials/icons/emot3_<slug>_*  (slug-prefixed, so the match is exact)
    const std::string iconsDir = RadialIconsDir();
    std::string pattern = iconsDir + "\\emot3_" + slug + "_*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            DeleteFileA((iconsDir + "\\" + fd.cFileName).c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    LOG_INFO("radials: removed staged files for wheel %s", slug.c_str());
    return ok;
}

bool RemoveRadialGroup(const std::string& group) {
    if (group.empty()) return false;
    // Snapshot the group's page slugs under the lock; delete files outside it
    // (RemoveRadialFiles takes no lock).
    std::vector<std::string> slugs;
    {
        std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
        for (const auto& w : g_RadialExports)
            if (w.Group == group) slugs.push_back(w.Slug);
    }
    bool ok = true;
    for (const auto& s : slugs) if (!RemoveRadialFiles(s)) ok = false;
    return ok;
}
