#include "RadialExports.h"

#include "Globals.h"      // g_RadialsDir (+ Windows.h)
#include "AtomicFile.h"   // DirExists (shared)
#include "EmoteBinds.h"   // ParseEmoteBindIdentifier - recover refs from item Actions
#include "StringUtil.h"   // ToLower / TruncateUtf8 / kMaxNameBytes (name cap at load)
#include "JsonUtil.h"     // never-throw typed getters (no type_error escaping the loader)
#include "Logging.h"
#include "Profiling.h"    // PROFILE_SCOPE (no-op without EMOT3_DEVTOOLS)

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;
using namespace jsonutil;

std::vector<RadialExport> g_RadialExports;
std::mutex                g_RadialExportsMutex;

const char* const kRadialPackNamePrefix = "emot3: ";

namespace {

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

    // All field reads go through the never-throw jsonutil getters: nlohmann's own
    // j.value(key, default) THROWS type_error on a present-but-wrong-type field
    // (a hand-made / legacy / RadialMenus-generated pack with "ID":"x" or
    // "Scale":"1.0"), which would escape this loader into AddonLoad uncaught.
    w.Id             = GetInt(j, "ID", 0);
    w.SourceCategory = GetString(j, "emot3_source_category", std::string());
    w.Partial        = GetBool(j, "emot3_partial", false);
    w.Group          = GetString(j, "emot3_group", w.Slug);  // legacy/hand-made -> own group
    w.Page           = GetInt(j, "emot3_page", 1);
    w.SourceRefs.clear();
    for (const auto& s : GetArray(j, "emot3_source_refs"))
        if (s.is_string()) w.SourceRefs.push_back(s.get<std::string>());

    // Logical export name: prefer the emot3_name marker; fall back to the pack "Name"
    // minus the "emot3: " prefix (legacy / hand-made packs).
    w.Name = GetString(j, "emot3_name", std::string());
    if (w.Name.empty()) {
        std::string packName = GetString(j, "Name", std::string());
        const size_t pfx = std::strlen(kRadialPackNamePrefix);
        w.Name = (packName.rfind(kRadialPackNamePrefix, 0) == 0) ? packName.substr(pfx)
                                                                 : packName;
    }
    if (w.Name.empty()) w.Name = w.Slug;
    w.Name = TruncateUtf8(w.Name, kMaxNameBytes);  // cap hand-edited/legacy names, like the other loaders

    // Native RadialMenus options (recoverable directly); emot3-namespaced markers
    // for what isn't (the source category above + the gate flag below).
    w.Options.Small               = (GetInt(j, "Type", 1) == 2);  // 1 Normal / 2 Small
    w.Options.SelectionMode       = GetInt(j, "SelectionMode", 3);
    w.Options.Scale               = GetFloat(j, "Scale", 1.0f);
    w.Options.IconScale           = GetFloat(j, "IconScale", 1.0f);
    w.Options.ShowItemNameTooltip = GetBool(j, "ShowItemNameTooltip", false);

    // Recover refs (and detect gating) from the items' Action identifiers.
    bool anyVisibility = false;
    for (const auto& it : GetArray(j, "Items")) {
        if (!it.is_object()) continue;
        if (it.contains("Visibility")) anyVisibility = true;
        const json& actions = GetArray(it, "Actions");
        if (actions.empty() || !actions[0].is_object()) continue;
        std::string idn = GetString(actions[0], "Identifier", std::string());
        RadialItemRef ref;
        if (ParseEmoteBindIdentifier(idn, ref.Type, ref.Id, ref.Variant))
            w.Items.push_back(std::move(ref));
    }
    // Gate flag: explicit marker if present, else inferred from any item carrying a
    // Visibility object (older/hand-edited packs).
    w.Options.GateByState = GetBool(j, "emot3_gate", anyVisibility);
    return true;
}

}  // namespace

std::string RadialPacksDir() { return g_RadialsDir.empty() ? std::string() : g_RadialsDir + "\\packs"; }
std::string RadialIconsDir() { return g_RadialsDir.empty() ? std::string() : g_RadialsDir + "\\icons"; }

void LoadRadialExports() {
    PROFILE_SCOPE("radials.scan");
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
            if (file.size() <= 5) continue;                  // need a stem before ".json"
            std::string slug = file.substr(0, file.size() - 5);  // drop ".json"
            RadialExport w;
            w.Slug = slug;
            // ReadPack uses never-throw jsonutil getters, but guard the call anyway:
            // a single malformed/legacy pack must never throw out of AddonLoad's bare
            // LoadRadialExports() - flag the row as a parse error and keep scanning.
            bool ok = false;
            try { ok = ReadPack(packsDir + "\\" + file, w); }
            catch (const std::exception& e) {
                LOG_WARNING("radials: %s threw on load (%s); skipping", file.c_str(), e.what());
            }
            if (!ok) {
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
    LOG_INFO("radials: loaded %d wheel(s) from %s", (int)g_RadialExports.size(),
             packsDir.c_str());
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

std::vector<std::string> ExportedSourceCategories() {
    std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
    std::vector<std::string> out;
    for (const auto& w : g_RadialExports) {
        if (w.SourceCategory.empty()) continue;
        bool dup = false;
        for (const auto& s : out) if (s == w.SourceCategory) { dup = true; break; }
        if (!dup) out.push_back(w.SourceCategory);
    }
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

bool RadialContainsRef(EFavoriteRefType type, const std::string& id,
                       EMeMoteVariant variant) {
    std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
    for (const auto& w : g_RadialExports)
        for (const auto& it : w.Items)
            if (it.Type == type && it.Id == id && it.Variant == variant) return true;
    return false;
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

// --- dev-tool introspection ----------------------------------------------------
int RadialExportWheelCount() {
    std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
    return (int)g_RadialExports.size();
}
int RadialExportItemCount() {
    std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
    int n = 0;
    for (const auto& w : g_RadialExports) n += (int)w.Items.size();
    return n;
}
int RadialExportParseErrors() {
    std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
    int n = 0;
    for (const auto& w : g_RadialExports) if (w.ParseError) ++n;
    return n;
}
size_t RadialExportApproxBytes() {
    std::lock_guard<std::mutex> lk(g_RadialExportsMutex);
    size_t b = 0;
    for (const auto& w : g_RadialExports) {
        b += sizeof(RadialExport) + w.Slug.capacity() + w.Name.capacity() +
             w.SourceCategory.capacity() + w.Group.capacity();
        for (const auto& s : w.SourceRefs) b += sizeof(std::string) + s.capacity();
        for (const auto& it : w.Items)     b += sizeof(RadialItemRef) + it.Id.capacity();
    }
    return b;
}

#ifdef EMOT3_DEVTOOLS
#include "DevStateInspector.h"
static DevStateRegistrar s_radialState(DevStateCat::Content, "Radial exports", [] {
    DevStateRow("wheels", "%d", RadialExportWheelCount());
    DevStateRow("items (total)", "%d", RadialExportItemCount());
    DevStateRow("parse errors", "%d", RadialExportParseErrors());
    DevStateRow("approx bytes", "%d", (int)RadialExportApproxBytes());
});
#endif
