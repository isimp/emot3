#include "EmoteBinds.h"

#include "Globals.h"      // APIDefs
#include "EmoteData.h"    // g_Emotes, g_EmotesMutex, FindEmote
#include "MeMotes.h"      // g_MeMotes, g_MeMotesMutex, FindMeMote, EMeMoteVariant
#include "EmoteAction.h"  // SendOrFillEmote / SendOrFillMeMote
#include "Settings.h"     // g_Settings (SendTargetableOnTarget)
#include "Logging.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace {

const char* const kEmotePrefix  = "emot3 Emote: ";
const char* const kMeMotePrefix = "emot3 Me-mote: ";

// Identifiers currently registered with Nexus (the diff baseline). Module-static
// so DeregisterAllEmoteBinds can clear it on unload and a reload re-syncs clean.
std::unordered_set<std::string> g_registered;

const char* VariantStr(EMeMoteVariant v) {
    return v == EMeMoteVariant::You ? "you"
         : v == EMeMoteVariant::All ? "all"
                                    : "default";
}
EMeMoteVariant ParseVariant(const std::string& s) {
    return s == "you" ? EMeMoteVariant::You
         : s == "all" ? EMeMoteVariant::All
                      : EMeMoteVariant::Default;
}

// Nexus calls this when the user (or RadialMenus) triggers one of our emote
// binds. Press only. Copy the entry out under its mutex, release, then dispatch
// (SendOrFill* spawns a detached worker — never hold the catalog lock across it).
void OnEmoteBind(const char* identifier, bool isRelease) {
    if (isRelease || !identifier) return;
    const std::string id = identifier;

    if (id.rfind(kEmotePrefix, 0) == 0) {
        const std::string emoteId = id.substr(std::strlen(kEmotePrefix));
        Emote copy;
        {
            std::lock_guard<std::mutex> lk(g_EmotesMutex);
            const Emote* e = FindEmote(emoteId);
            if (!e) return;
            copy = *e;
        }
        // Honor the live "send on target" setting, exactly like a panel click.
        const bool autoTarget = copy.IsTargetable && g_Settings.SendTargetableOnTarget;
        SendOrFillEmote(copy, autoTarget, /*useSync=*/false, /*fromKeybind=*/true);
        return;
    }

    if (id.rfind(kMeMotePrefix, 0) == 0) {
        std::string rest = id.substr(std::strlen(kMeMotePrefix));  // "<id> [variant]"
        EMeMoteVariant variant = EMeMoteVariant::Default;
        std::string meMoteId = rest;
        const size_t br = rest.rfind(" [");
        if (br != std::string::npos && !rest.empty() && rest.back() == ']') {
            meMoteId = rest.substr(0, br);
            variant  = ParseVariant(rest.substr(br + 2, rest.size() - (br + 2) - 1));
        }
        MeMote copy;
        {
            std::lock_guard<std::mutex> lk(g_MeMotesMutex);
            const MeMote* m = FindMeMote(meMoteId);
            if (!m) return;
            copy = *m;
        }
        SendOrFillMeMote(copy, variant, /*fromKeybind=*/true);
        return;
    }
}

}  // namespace

std::string EmoteBindIdentifier(EFavoriteRefType type, const std::string& id,
                                EMeMoteVariant variant) {
    if (type == EFavoriteRefType::Emote) return std::string(kEmotePrefix) + id;
    return std::string(kMeMotePrefix) + id + " [" + VariantStr(variant) + "]";
}

void SyncEmoteBinds() {
    if (!APIDefs) return;

    // Desired set = every opted-in catalog entry. (P3 unions in wheel refs.)
    std::unordered_set<std::string> desired;
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        for (const auto& e : g_Emotes)
            if (e.UserKeybind)
                desired.insert(EmoteBindIdentifier(EFavoriteRefType::Emote, e.Id));
    }
    {
        std::lock_guard<std::mutex> lk(g_MeMotesMutex);
        for (const auto& m : g_MeMotes) {
            if (m.KeybindDefault)
                desired.insert(EmoteBindIdentifier(EFavoriteRefType::MeMote, m.Id, EMeMoteVariant::Default));
            if (m.KeybindYou)
                desired.insert(EmoteBindIdentifier(EFavoriteRefType::MeMote, m.Id, EMeMoteVariant::You));
            if (m.KeybindAll)
                desired.insert(EmoteBindIdentifier(EFavoriteRefType::MeMote, m.Id, EMeMoteVariant::All));
        }
    }

    // Register additions (newly desired identifiers).
    int added = 0;
    for (const auto& idn : desired) {
        if (g_registered.insert(idn).second) {
            APIDefs->InputBinds.RegisterWithString(idn.c_str(), OnEmoteBind, "");
            ++added;
        }
    }
    // Deregister removals (registered but no longer desired).
    int removed = 0;
    for (auto it = g_registered.begin(); it != g_registered.end();) {
        if (desired.count(*it) == 0) {
            APIDefs->InputBinds.Deregister(it->c_str());
            it = g_registered.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (added || removed)
        LOG_DEBUG("keybind: synced (+%d -%d, %d active)", added, removed,
                  (int)g_registered.size());

    // Nexus' Deregister is non-destructive (nulls the handler, keeps the entry +
    // the user's key assignment) and does NOT raise EV_INPUTBIND_UPDATED, so a
    // pure removal leaves a stale row in Nexus' addon keybind list until some
    // later registry change refreshes it. Register raises the event itself, so a
    // refresh is already pending whenever we added; raise it ourselves when we
    // ONLY removed, so an untick drops from Nexus' list immediately (the rebuild
    // regroups by owning handler — our now-handlerless bind falls out of the
    // emot3 group). The key assignment persists, so re-ticking restores it.
    if (removed && !added && APIDefs->Events.Raise)
        APIDefs->Events.Raise("EV_INPUTBIND_UPDATED", nullptr);
}

void DeregisterAllEmoteBinds() {
    if (!APIDefs) return;
    for (const auto& idn : g_registered) APIDefs->InputBinds.Deregister(idn.c_str());
    g_registered.clear();
}
