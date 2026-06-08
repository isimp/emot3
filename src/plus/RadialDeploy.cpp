#include "RadialDeploy.h"

#include "Globals.h"       // APIDefs (+ Windows.h), g_RadialsDir
#include "RadialExports.h" // RadialPacksDir / RadialIconsDir (the staged layout)
#include "Logging.h"

#include <fstream>
#include <iterator>
#include <string>

namespace {

bool DirExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

// --- detection: available in every build ------------------------------------

std::string RadialMenusDir() {
    if (!APIDefs || !APIDefs->Paths.GetAddonDirectory) return std::string();
    const char* dir = APIDefs->Paths.GetAddonDirectory("RadialMenus");
    return dir ? std::string(dir) : std::string();
}

bool IsRadialMenusInstalled() {
    std::string dir = RadialMenusDir();
    return !dir.empty() && DirExists(dir);
}

// --- deploy: +plus only -----------------------------------------------------

#ifdef EMOT3_PLUS

#include <nlohmann/json.hpp>
#include <set>

namespace {

using json = nlohmann::json;

bool ReadFileBytes(const std::string& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool ParseJson(const std::string& text, json& out) {
    out = json::parse(text, nullptr, /*allow_exceptions=*/false);
    return !out.is_discarded();
}

// emot3's "content fingerprint": the set of bound emote/me-mote identifiers a pack
// fires (Items[].Actions[].Identifier), order-independent. This is the ONLY thing the
// sync status compares - everything else in the pack (Scale, SelectionMode, color,
// position, ...) is RadialMenus' to own once the wheel is deployed.
std::set<std::string> PackIdentifierSet(const json& j) {
    std::set<std::string> ids;
    auto items = j.find("Items");
    if (items == j.end() || !items->is_array()) return ids;
    for (const auto& it : *items) {
        auto acts = it.find("Actions");
        if (acts == it.end() || !acts->is_array()) continue;
        for (const auto& a : *acts) {
            auto id = a.find("Identifier");
            if (id != a.end() && id->is_string()) ids.insert(id->get<std::string>());
        }
    }
    return ids;
}

// Keys emot3 authoritatively owns in a deployed pack. On a re-deploy these are taken
// from the staged pack; EVERY other key (RadialMenus presentation: Scale, IconScale,
// SelectionMode, HoverTimeout, InnerRadius, colors, position, ItemRotation, ... plus
// any key RadialMenus adds that we don't know about) is preserved from the deployed
// copy, so the user's in-RadialMenus customization survives a content re-sync.
const char* const kEmot3OwnedKeys[] = {
    "ID", "Name", "FormatRevision", "Items",
    "emot3_source_category", "emot3_gate", "emot3_group", "emot3_page",
    "emot3_name", "emot3_partial", "emot3_source_refs",
};

// Merge-deploy one staged pack into RadialMenus. First deploy (no parseable deployed
// pack) copies the staged pack verbatim (the wizard's presentation defaults go through).
// Re-deploy starts from the DEPLOYED json and overwrites only the emot3-owned keys, so
// RadialMenus presentation is kept. Returns true on success.
bool MergeDeployPack(const std::string& stagedPath, const std::string& deployedPath) {
    std::string stagedText;
    if (!ReadFileBytes(stagedPath, stagedText)) return false;  // nothing staged

    json staged;
    if (!ParseJson(stagedText, staged) || !staged.is_object())  // staged unreadable
        return CopyFileA(stagedPath.c_str(), deployedPath.c_str(), FALSE) != 0;

    std::string deployedText;
    json dep;
    const bool haveDep = ReadFileBytes(deployedPath, deployedText) &&
                         ParseJson(deployedText, dep) && dep.is_object();
    if (!haveDep)  // first deploy: staged pack verbatim (incl. wizard presentation)
        return CopyFileA(stagedPath.c_str(), deployedPath.c_str(), FALSE) != 0;

    for (const char* k : kEmot3OwnedKeys) {
        auto sv = staged.find(k);
        if (sv != staged.end()) dep[k] = *sv;
        else                    dep.erase(k);
    }
    std::string body;
    try { body = dep.dump(2, ' ', false, json::error_handler_t::replace) + "\n"; }
    catch (const std::exception&) { return false; }
    std::ofstream f(deployedPath, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f.write(body.data(), (std::streamsize)body.size());
    return f.good();
}

// Delete one deployed slug's pack + its emot3_<slug>_* icons from RadialMenus.
// Returns the number of files removed.
int DeleteDeployedSlug(const std::string& rmDir, const std::string& slug) {
    if (slug.empty()) return 0;
    const std::string packsDir = rmDir + "\\packs";
    const std::string iconsDir = rmDir + "\\icons";
    int removed = 0;
    if (DeleteFileA((packsDir + "\\" + slug + ".json").c_str())) ++removed;
    std::string pattern = iconsDir + "\\emot3_" + slug + "_*";  // slug-prefixed icons
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (DeleteFileA((iconsDir + "\\" + fd.cFileName).c_str())) ++removed;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return removed;
}

// Copy every emot3_<slug>_* icon for one slug from staged into RadialMenus' icons.
int CopySlugIcons(const std::string& srcIcons, const std::string& dstIcons,
                  const std::string& slug) {
    int n = 0;
    std::string pattern = srcIcons + "\\emot3_" + slug + "_*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string fn = fd.cFileName;
            if (CopyFileA((srcIcons + "\\" + fn).c_str(),
                          (dstIcons + "\\" + fn).c_str(), FALSE)) ++n;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return n;
}

}  // namespace

RadialDeployResult DeployToRadialMenus() {
    RadialDeployResult res;
    res.available = true;

    const std::string rmDir = RadialMenusDir();
    if (rmDir.empty() || !DirExists(rmDir)) {
        res.error = "RadialMenus not detected";
        return res;
    }
    if (g_RadialsDir.empty()) { res.error = "no staged wheels"; return res; }

    const std::string dstPacks = rmDir + "\\packs";
    const std::string dstIcons = rmDir + "\\icons";
    CreateDirectoryA(dstPacks.c_str(), nullptr);
    CreateDirectoryA(dstIcons.c_str(), nullptr);

    const std::string srcPacks = RadialPacksDir();
    const std::string srcIcons = RadialIconsDir();

    // Packs are MERGE-deployed (keep each deployed wheel's RadialMenus presentation,
    // overwrite only the emote content) so a bulk re-deploy doesn't wipe the user's
    // in-RadialMenus customizations. Icons are emot3-owned content - a flat copy.
    auto copyIcons = [&](const std::string& srcDir, const std::string& dstDir) -> int {
        int n = 0;
        std::string pattern = srcDir + "\\*";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string fn = fd.cFileName;
                if (CopyFileA((srcDir + "\\" + fn).c_str(),
                              (dstDir + "\\" + fn).c_str(), /*bFailIfExists=*/FALSE))
                    ++n;
                else
                    LOG_WARNING("radials: deploy failed to copy %s", fn.c_str());
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        return n;
    };
    // Merge each staged pack (*.json) in turn.
    {
        std::string pattern = srcPacks + "\\*.json";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string fn = fd.cFileName;
                if (MergeDeployPack(srcPacks + "\\" + fn, dstPacks + "\\" + fn)) ++res.packs;
                else LOG_WARNING("radials: deploy failed to merge %s", fn.c_str());
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    res.icons = copyIcons(srcIcons, dstIcons);

    res.ok = res.packs > 0;
    if (!res.ok && res.error.empty()) res.error = "no wheels to deploy";
    LOG_INFO("radials: deployed %d pack(s) + %d icon(s) to RadialMenus",
             res.packs, res.icons);
    return res;
}

bool RadialMenusHasPack(const std::string& slug) {
    const std::string rmDir = RadialMenusDir();
    if (rmDir.empty() || slug.empty()) return false;
    std::string pack = rmDir + "\\packs\\" + slug + ".json";
    return GetFileAttributesA(pack.c_str()) != INVALID_FILE_ATTRIBUTES;
}

int RemoveFromRadialMenus(const std::vector<std::string>& slugs) {
    const std::string rmDir = RadialMenusDir();
    if (rmDir.empty()) return 0;
    int removed = 0;
    for (const auto& slug : slugs) removed += DeleteDeployedSlug(rmDir, slug);
    LOG_INFO("radials: removed %d file(s) from RadialMenus", removed);
    return removed;
}

int PruneDeployedGroupOrphans(const std::string& group,
                              const std::vector<std::string>& keepSlugs) {
    const std::string rmDir = RadialMenusDir();
    if (rmDir.empty() || !DirExists(rmDir) || group.empty()) return 0;
    const std::string packsDir = rmDir + "\\packs";
    // Find deployed packs tagged with THIS group (emot3_group marker) whose slug is no
    // longer one of the group's current pages - these are pages a shrink left behind.
    // Collect first; don't delete while the find handle is open. Match by marker (not a
    // slug pattern) so we never touch another group's or a hand-made pack.
    std::vector<std::string> orphans;
    std::string pattern = packsDir + "\\*.json";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string fn = fd.cFileName;                       // "<slug>.json"
            if (fn.size() <= 5) continue;
            std::string slug = fn.substr(0, fn.size() - 5);      // strip ".json"
            bool keep = false;
            for (const auto& k : keepSlugs) if (k == slug) { keep = true; break; }
            if (keep) continue;
            std::string text; json j;
            if (!ReadFileBytes(packsDir + "\\" + fn, text) || !ParseJson(text, j) ||
                !j.is_object())
                continue;
            auto g = j.find("emot3_group");
            if (g != j.end() && g->is_string() && g->get<std::string>() == group)
                orphans.push_back(slug);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    int removed = 0;
    for (const auto& slug : orphans) removed += DeleteDeployedSlug(rmDir, slug);
    if (removed)
        LOG_INFO("radials: pruned %d orphan file(s) for group %s", removed, group.c_str());
    return removed;
}

int DeployGroupToRadialMenus(const std::vector<std::string>& slugs) {
    const std::string rmDir = RadialMenusDir();
    if (rmDir.empty() || !DirExists(rmDir)) return 0;
    const std::string dstPacks = rmDir + "\\packs";
    const std::string dstIcons = rmDir + "\\icons";
    CreateDirectoryA(dstPacks.c_str(), nullptr);
    CreateDirectoryA(dstIcons.c_str(), nullptr);
    const std::string srcPacks = RadialPacksDir();
    const std::string srcIcons = RadialIconsDir();
    int n = 0;
    for (const auto& slug : slugs) {
        if (slug.empty()) continue;
        // Merge the pack (keep RadialMenus presentation, overwrite only emot3 content),
        // then refresh the slug's icons (emot3-owned).
        if (MergeDeployPack(srcPacks + "\\" + slug + ".json",
                            dstPacks + "\\" + slug + ".json")) ++n;
        n += CopySlugIcons(srcIcons, dstIcons, slug);
    }
    LOG_INFO("radials: synced %d file(s) to RadialMenus", n);
    return n;
}

RadialSyncState RadialMenusSyncState(const std::vector<std::string>& slugs) {
    const std::string rmDir = RadialMenusDir();
    if (rmDir.empty() || slugs.empty()) return RadialSyncState::NotDeployed;
    const std::string stagedDir = RadialPacksDir();
    const std::string rmPacks   = rmDir + "\\packs";
    int deployed = 0, diff = 0;
    for (const auto& slug : slugs) {
        std::string depText;
        if (!ReadFileBytes(rmPacks + "\\" + slug + ".json", depText)) continue;  // not deployed
        ++deployed;
        std::string stagedText;
        json dep, staged;
        const bool depOk    = ParseJson(depText, dep);
        const bool stagedOk = ReadFileBytes(stagedDir + "\\" + slug + ".json", stagedText) &&
                              ParseJson(stagedText, staged);
        // Compare ONLY the emote/me-mote content (PackIdentifierSet), so the user's
        // RadialMenus presentation tweaks never read as out of date. Treat a parse
        // failure on either side as differing (safest - prompts a re-sync).
        if (!depOk || !stagedOk || PackIdentifierSet(dep) != PackIdentifierSet(staged))
            ++diff;
    }
    if (deployed == 0)                                  return RadialSyncState::NotDeployed;
    if (diff > 0 || deployed != (int)slugs.size())      return RadialSyncState::OutOfDate;
    return RadialSyncState::InSync;
}

#else  // ---- base build: nothing outside emot3's own folder ----

RadialDeployResult DeployToRadialMenus() {
    RadialDeployResult res;
    res.available = false;  // UI shows the manual-copy hint instead
    return res;
}
bool RadialMenusHasPack(const std::string&) { return false; }
int  RemoveFromRadialMenus(const std::vector<std::string>&) { return 0; }
int  PruneDeployedGroupOrphans(const std::string&, const std::vector<std::string>&) { return 0; }
int  DeployGroupToRadialMenus(const std::vector<std::string>&) { return 0; }
RadialSyncState RadialMenusSyncState(const std::vector<std::string>&) {
    return RadialSyncState::NotDeployed;
}

#endif  // EMOT3_PLUS
