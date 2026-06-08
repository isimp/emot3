#include "RadialDeploy.h"

#include "Globals.h"       // APIDefs (+ Windows.h), g_RadialsDir
#include "RadialExports.h" // RadialPacksDir / RadialIconsDir (the staged layout)
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

    const std::string dstPacks = rmDir + "\\packs";
    const std::string dstIcons = rmDir + "\\icons";
    CreateDirectoryA(dstPacks.c_str(), nullptr);
    CreateDirectoryA(dstIcons.c_str(), nullptr);

    // The staged layout already mirrors RadialMenus (radials/packs + radials/icons),
    // so deploy is just two flat folder copies (everything is slug-named, so nothing
    // collides in RadialMenus' shared folders).
    auto copyDir = [](const std::string& srcDir, const std::string& dstDir) -> int {
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
    res.packs = copyDir(RadialPacksDir(), dstPacks);
    res.icons = copyDir(RadialIconsDir(), dstIcons);

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
