#include "RadialDeploy.h"

#include "Globals.h"   // APIDefs (+ Windows.h), g_RadialsDir
#include "Logging.h"

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

RadialDeployResult DeployToRadialMenus() {
    RadialDeployResult res;
    res.available = true;

    const std::string rmDir = RadialMenusDir();
    if (rmDir.empty() || !DirExists(rmDir)) {
        res.error = "RadialMenus not detected";
        return res;
    }
    if (g_RadialsDir.empty()) { res.error = "no staged wheels"; return res; }

    const std::string packsDir = rmDir + "\\packs";
    const std::string iconsDir = rmDir + "\\icons";
    CreateDirectoryA(packsDir.c_str(), nullptr);
    CreateDirectoryA(iconsDir.c_str(), nullptr);

    // Walk each staged radials/<slug>/ subfolder.
    std::string pattern = g_RadialsDir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::string slug = fd.cFileName;
            if (slug == "." || slug == "..") continue;
            std::string wheelDir = g_RadialsDir + "\\" + slug;

            // pack: <slug>/<slug>.json -> packs/<slug>.json
            std::string srcPack = wheelDir + "\\" + slug + ".json";
            std::string dstPack = packsDir + "\\" + slug + ".json";
            if (CopyFileA(srcPack.c_str(), dstPack.c_str(), /*bFailIfExists=*/FALSE))
                ++res.packs;
            else
                LOG_WARNING("radials: deploy failed to copy pack %s", srcPack.c_str());

            // icons: <slug>/icons/* -> icons/* (slug-named, so no cross-wheel collision)
            std::string iconPattern = wheelDir + "\\icons\\*";
            WIN32_FIND_DATAA ifd;
            HANDLE ih = FindFirstFileA(iconPattern.c_str(), &ifd);
            if (ih != INVALID_HANDLE_VALUE) {
                do {
                    if (ifd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    std::string fn  = ifd.cFileName;
                    std::string src = wheelDir + "\\icons\\" + fn;
                    std::string dst = iconsDir + "\\" + fn;
                    if (CopyFileA(src.c_str(), dst.c_str(), FALSE)) ++res.icons;
                } while (FindNextFileA(ih, &ifd));
                FindClose(ih);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    res.ok = res.packs > 0;
    if (!res.ok && res.error.empty()) res.error = "no wheels to deploy";
    LOG_INFO("radials: deployed %d pack(s) + %d icon(s) to RadialMenus",
             res.packs, res.icons);
    return res;
}

#else  // ---- base build: deploy not compiled in ----

RadialDeployResult DeployToRadialMenus() {
    RadialDeployResult res;
    res.available = false;  // UI shows the manual-copy hint instead
    return res;
}

#endif  // EMOT3_PLUS
