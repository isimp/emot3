#include "IconBrowse.h"
#include "Globals.h"
#include "Logging.h"
#include "EmoteData.h"
#include "MeMotes.h"
#include "Icons.h"       // SanitizeIconPath (heal at the write boundary)

// Historically this TU also wired a Win32 OPENFILENAME "From file..." picker
// on a detached worker thread (StartIconBrowse / DrainIconBrowse / the
// g_IconBrowse state machine). The in-app icon picker (ui/IconPicker.cpp)
// replaced that affordance once it landed — it covers every bundled bucket
// AND the user's icons/ folder in one place, so an OS-side dialog adds no
// affordance the picker doesn't already have. Browse-side machinery was
// removed; the surviving entry point is ApplyIconPathToTarget, the shared
// routing helper the picker (and any future writer) calls to land an
// IconPath in the right catalog with the right persistence + dirty bump.

bool ApplyIconPathToTarget(EIconTargetKind kind, const std::string& targetId,
                           const std::string& path) {
    // Heal the path at the write boundary - the same SanitizeIconPath the
    // JSON-load ingress (EmoteData/MeMotes) runs. Bundled refs and valid
    // relatives pass through untouched; an absolute / folder-escape / malformed
    // path from any caller is neutralized before it's persisted (and before the
    // cache derives a content key from it). The picker only ever emits valid
    // paths, so this is a no-op for it today - it hardens the boundary for any
    // future writer.
    const std::string clean = SanitizeIconPath(path);

    // Route an icon choice to the correct catalog. Each branch persists + marks
    // dirty internally so callers don't have to know which catalog was touched.
    // Stale targets (entry deleted between request and apply) log + drop.
    bool applied = false;
    if (kind == EIconTargetKind::Emote) {
        {
            std::lock_guard<std::mutex> lk(g_EmotesMutex);
            for (auto& e : g_Emotes)
                if (e.Id == targetId) { e.IconPath = clean; applied = true; break; }
        }
        if (applied) {
            LOG_INFO("Icon for emote %s set to: %s", targetId.c_str(), clean.c_str());
            if (!g_EmotesJsonPath.empty()) SaveEmotesJson(g_EmotesJsonPath);
            MarkEmotesDirty();
        } else {
            LOG_WARNING("Icon choice discarded - emote %s no longer exists",
                        targetId.c_str());
        }
    } else {  // MeMote
        {
            std::lock_guard<std::mutex> lk(g_MeMotesMutex);
            for (auto& m : g_MeMotes)
                if (m.Id == targetId) { m.IconPath = clean; applied = true; break; }
        }
        if (applied) {
            LOG_INFO("Icon for /me-mote %s set to: %s",
                     targetId.c_str(), clean.c_str());
            if (!g_MeMotesJsonPath.empty()) SaveMeMotesJson(g_MeMotesJsonPath);
            MarkMeMotesDirty();
        } else {
            LOG_WARNING("Icon choice discarded - /me-mote %s no longer exists",
                        targetId.c_str());
        }
    }
    return applied;
}
