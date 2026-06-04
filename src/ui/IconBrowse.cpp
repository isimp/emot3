#include "IconBrowse.h"
#include "Globals.h"
#include "Logging.h"
#include "Icons.h"
#include "EmoteData.h"

#include <Windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

#include <algorithm>
#include <cctype>
#include <thread>

IconBrowseState g_IconBrowse;

void StartIconBrowse(const std::string& emoteCommand,
                     const std::string& currentValue)
{
    if (g_IconBrowse.active.load() || g_IconBrowse.ready.load()) {
        LOG_DEBUG("Browse dialog already in flight - ignoring new request");
        return;
    }

    LOG_DEBUG("Opening icon-browse dialog for %s", emoteCommand.c_str());
    {
        std::lock_guard<std::mutex> lk(g_IconBrowse.m);
        g_IconBrowse.targetId = emoteCommand;
        g_IconBrowse.result.clear();
    }
    g_IconBrowse.active.store(true);

    // Captures by value — the worker outlives this stack frame. Seed the
    // dialog's initial selection from the current Icon Path (if any).
    std::string seed    = currentValue;
    std::string initDir = g_IconsDir;
    Emote       seedForResolve;
    seedForResolve.IconPath = seed;
    // Id is only consulted by ResolveIconPath when IconPath is empty
    // (and we guard that case below), so any non-empty placeholder is
    // fine here - we just need a valid Emote to resolve the seed path.
    seedForResolve.Id       = "_browse";
    std::string seedAbs = seed.empty() ? std::string() : ResolveIconPath(seedForResolve);

    std::thread([seedAbs, initDir]() {
        // Bump the shared inflight-worker counter so AddonUnload can
        // briefly wait for us to drain. Unlike SendOrFillEmote's worker,
        // this one doesn't deref APIDefs/MumbleLink inside the lambda
        // (only g_IconBrowse static state + Win32 directly), so it can't
        // crash on a mid-Sleep unload. The remaining failure mode is the
        // rare "user opened the file picker AND disabled the addon
        // before closing the picker" - we accept that edge case rather
        // than block AddonUnload on a dialog the user controls.
        struct CounterScope {
            CounterScope()  { g_InflightWorkers.fetch_add(1); }
            ~CounterScope() { g_InflightWorkers.fetch_sub(1); }
        } scope;

        // CoInitializeEx is required by the modern file dialog code path
        // inside comdlg32. S_FALSE on subsequent calls is fine — the dialog
        // still works.
        (void)CoInitializeEx(nullptr,
            COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

        char buf[MAX_PATH] = {};
        if (!seedAbs.empty() && seedAbs.size() < sizeof(buf)) {
            strncpy_s(buf, sizeof(buf), seedAbs.c_str(), _TRUNCATE);
        }

        OPENFILENAMEA ofn = {};
        ofn.lStructSize     = sizeof(ofn);
        ofn.hwndOwner       = nullptr;   // NOT parented to the game window
        ofn.lpstrFile       = buf;
        ofn.nMaxFile        = sizeof(buf);
        // Only the image formats the texture loader can actually decode.
        // "All files" is deliberately omitted - offering it just invites
        // picks we'd reject anyway. (The filter restricts the browse list,
        // not manual path entry, so we re-check the extension after the pick.)
        ofn.lpstrFilter     = "Images (*.png;*.jpg;*.jpeg)\0*.png;*.jpg;*.jpeg\0"
                              "PNG (*.png)\0*.png\0"
                              "JPEG (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0";
        ofn.nFilterIndex    = 1;
        ofn.lpstrTitle      = "Select emote icon";
        ofn.lpstrInitialDir = initDir.empty() ? nullptr : initDir.c_str();
        ofn.Flags           = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

        std::string picked;
        if (GetOpenFileNameA(&ofn)) picked = buf;

        // Enforce the supported types even if the user typed an extension the
        // filter wouldn't have shown (e.g. a hand-entered path). An
        // unsupported pick is dropped, which DrainIconBrowse treats the same
        // as a cancel.
        if (!picked.empty()) {
            std::string ext;
            size_t dot = picked.find_last_of('.');
            if (dot != std::string::npos) ext = picked.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });
            if (ext != "png" && ext != "jpg" && ext != "jpeg") {
                LOG_WARNING("Icon pick rejected: unsupported type '.%s' "
                            "(png/jpg/jpeg only)", ext.c_str());
                picked.clear();
            }
        }

        // Collapse to a path relative to <icons>/ when the pick lives there.
        if (!picked.empty() && !initDir.empty() &&
            picked.size() > initDir.size() + 1)
        {
            std::string lhs = picked.substr(0, initDir.size());
            std::string rhs = initDir;
            std::transform(lhs.begin(), lhs.end(), lhs.begin(), ::tolower);
            std::transform(rhs.begin(), rhs.end(), rhs.begin(), ::tolower);
            if (lhs == rhs) {
                std::string rel = picked.substr(initDir.size());
                while (!rel.empty() &&
                       (rel.front() == '\\' || rel.front() == '/'))
                    rel.erase(rel.begin());
                picked = rel;
            }
        }

        {
            std::lock_guard<std::mutex> lk(g_IconBrowse.m);
            g_IconBrowse.result = picked;
        }
        g_IconBrowse.ready.store(true);
        g_IconBrowse.active.store(false);

        CoUninitialize();
    }).detach();
}

bool DrainIconBrowse() {
    if (!g_IconBrowse.ready.load()) return false;

    std::string targetId, result;
    {
        std::lock_guard<std::mutex> lk(g_IconBrowse.m);
        targetId = g_IconBrowse.targetId;
        result   = g_IconBrowse.result;
        g_IconBrowse.targetId.clear();
        g_IconBrowse.result.clear();
    }
    g_IconBrowse.ready.store(false);

    if (targetId.empty() || result.empty()) {
        // Cancel or stale — also covers the "dialog opened but user closed
        // without picking anything" path, which isn't a problem.
        return false;
    }

    bool applied = false;
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        for (auto& e : g_Emotes) {
            if (e.Id == targetId) { e.IconPath = result; applied = true; break; }
        }
    }
    if (applied) {
        LOG_INFO("Icon for %s set to: %s", targetId.c_str(), result.c_str());
    } else {
        LOG_WARNING("Browse result discarded - emote %s no longer exists",
                    targetId.c_str());
    }
    return applied;
}
