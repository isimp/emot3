#include "UnlockScan.h"

#include "Globals.h"      // APIDefs, g_Unloading, g_InflightWorkers
#include "Settings.h"     // g_Settings, SaveSettings
#include "EmoteData.h"    // g_Emotes, GetBundledUnlockInfo, NormalizeEmoteCommand
#include "EmoteAction.h"  // IsEmoteUnlocked / MarkEmoteUnlocked / MarkEmoteLocked
#include "Logging.h"

#include <nlohmann/json.hpp>

#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include "hoardandseek/HoardAndSeekAPI.h"  // EV_HOARD_QUERY_API + structs

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

namespace {

// Our private response-event name for the Hoard & Seek proxy round-trip.
constexpr const char* kHsResponseEvent = "EV_EMOT3_UNLOCKS";

// Render-thread-only UI status (read by the Options tab, written in Start/Drain).
UnlockSyncStatus s_status;

// Cross-thread handoff: the worker thread (own-key) or the H&S response handler
// fill s_pending; DrainUnlockSync (render thread) consumes it and applies.
std::mutex s_mx;
struct Pending {
    bool        ready   = false;  // a result is waiting for the render thread
    bool        pending = false;  // H&S permission popup (retry, don't fail)
    bool        ok      = false;  // got a JSON body
    std::string json;             // raw /v2/account/emotes body
    std::string failKey;          // i18n key when !ok && !pending
};
Pending s_pending;

std::atomic<bool> s_busy{false};          // a sync is in flight
unsigned long long s_startTick   = 0;     // GetTickCount64 at StartUnlockSync
unsigned long long s_lastRaiseTick = 0;   // last H&S query raise (retry pacing)
bool s_usingHs = false;                   // current sync goes through H&S

// Once-per-session auto-sync state. File scope (not function-local statics in
// DrainUnlockSync) so InitUnlockScan can reset them on reload - the DLL persists
// across a Nexus reload, so a function-local static would never re-arm.
bool s_autoFired = false;                 // auto-sync already fired this session
unsigned long long s_autoFirstTick = 0;   // first DrainUnlockSync tick (auto-sync delay base)

constexpr unsigned long long kNoResponseMs = 8000;   // H&S silent => assume absent
constexpr unsigned long long kPendingRetryMs = 3000; // re-ask while popup open
constexpr unsigned long long kPendingCapMs = 90000;  // give up waiting for the user

// ---- HTTP -------------------------------------------------------------

struct HttpResult { bool ok = false; int status = 0; std::string body; };

// Secure GET to api.guildwars2.com. `bearer` empty => no auth header.
HttpResult HttpsGet(const wchar_t* path, const std::string& bearer) {
    HttpResult r;
    HINTERNET hS = WinHttpOpen(L"emot3",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return r;
    WinHttpSetTimeouts(hS, 5000, 5000, 8000, 8000);
    HINTERNET hC = WinHttpConnect(hS, L"api.guildwars2.com",
                                  INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hC) { WinHttpCloseHandle(hS); return r; }
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return r; }

    std::wstring headers;
    if (!bearer.empty()) {
        // GW2 API keys are ASCII, so a widening copy is lossless.
        std::wstring wkey(bearer.begin(), bearer.end());
        headers = L"Authorization: Bearer " + wkey;
    }
    BOOL ok = WinHttpSendRequest(hR,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : (DWORD)-1L,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hR, nullptr);
    if (ok) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(hR,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        r.status = (int)code;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hR, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(hR, &chunk[0], avail, &read)) break;
            chunk.resize(read);
            r.body += chunk;
            if (read == 0) break;
        }
        r.ok = true;
    }
    WinHttpCloseHandle(hR);
    WinHttpCloseHandle(hC);
    WinHttpCloseHandle(hS);
    return r;
}

// ---- parsing + apply --------------------------------------------------

// Parse the /v2/account/emotes JSON array of id strings.
bool ParseEmoteIds(const std::string& body, std::vector<std::string>& out) {
    try {
        json j = json::parse(body);
        if (!j.is_array()) return false;
        for (const auto& v : j)
            if (v.is_string()) out.push_back(v.get<std::string>());
        return true;
    } catch (...) {
        return false;
    }
}

void Fail(const char* errorKey) {
    s_status.phase    = UnlockSyncPhase::Failed;
    s_status.errorKey = errorKey;
    s_busy.store(false);
    LOG_WARNING("Unlock sync failed: %s", errorKey);
}

// Apply a successful account-emotes body to the unlock state. Render thread.
void ApplyResult(const std::string& body) {
    // Log the raw body (truncated) so a parse failure is diagnosable.
    LOG_INFO("Unlock sync: received %d byte(s): %.200s",
             (int)body.size(), body.c_str());
    std::vector<std::string> apiIds;
    if (!ParseEmoteIds(body, apiIds)) { Fail("opt.unl.err_parse"); return; }

    const BundledUnlockInfo& bi = GetBundledUnlockInfo();

    // Log exactly what the account API exposed (ground truth, the observability
    // the user asked for), and resolve each id to a bundled id via the
    // normalized index (case-insensitive, slash-insensitive, alias-aware).
    {
        std::string list;
        for (const auto& id : apiIds) { list += id; list += ' '; }
        LOG_INFO("Unlock sync: account API returned %d id(s): %s",
                 (int)apiIds.size(), list.c_str());
    }
    std::unordered_set<std::string> unlockedBundled;  // bundled ids the account has
    int unknownApi = 0;
    for (const auto& apiId : apiIds) {
        auto it = bi.normKeyToId.find(NormalizeUnlockKey(apiId));
        if (it != bi.normKeyToId.end()) unlockedBundled.insert(it->second);
        else {
            ++unknownApi;
            LOG_INFO("Unlock sync: API id '%s' not recognized (no catalog match)",
                     apiId.c_str());
        }
    }

    // Additive only: resolve each catalog unlockable to a bundled id (by its id,
    // command, or any alias); if the account has it unlocked, collect it to mark
    // unlocked. We NEVER lock - the API covers only a subset, so we don't trust
    // it to manage the full state. Resolve under the catalog lock; apply (which
    // saves settings) outside it, since the mutators read g_Emotes via FindEmote
    // without locking.
    std::vector<std::string> toUnlock;  // catalog ids
    int custom = 0;
    {
        std::lock_guard<std::mutex> lk(g_EmotesMutex);
        for (const auto& e : g_Emotes) {
            if (e.IsCore) continue;
            std::string gid;
            auto tryKey = [&](const std::string& raw) {
                if (!gid.empty()) return;
                auto it = bi.normKeyToId.find(NormalizeUnlockKey(raw));
                if (it != bi.normKeyToId.end()) gid = it->second;
            };
            tryKey(e.Id);
            tryKey(e.Command);
            for (const auto& a : e.Aliases) tryKey(a);
            if (gid.empty()) {
                ++custom;
                LOG_DEBUG("Unlock sync: '%s' unmatched - left as-is", e.Id.c_str());
                continue;
            }
            if (unlockedBundled.count(gid)) toUnlock.push_back(e.Id);
        }
    }

    int unlocked = 0;
    for (const auto& id : toUnlock) {
        ++unlocked;
        if (!IsEmoteUnlocked(id)) MarkEmoteUnlocked(id);  // additive; never locks
    }

    s_status.phase      = UnlockSyncPhase::Done;
    s_status.apiCount   = (int)apiIds.size();
    s_status.unlocked   = unlocked;
    s_status.custom     = custom;
    s_status.unknownApi = unknownApi;
    s_busy.store(false);
    MarkEmotesDirty();  // refresh panels' unlock-derived state
    LOG_INFO("Unlock sync done: %d unlocked, %d custom-skipped, "
             "%d API id(s) unknown to catalog",
             unlocked, custom, unknownApi);
}

// ---- own-key worker ---------------------------------------------------

void StartOwnKeyFetch() {
    std::string key = g_Settings.Gw2ApiKey;
    std::thread([key]() {
        InflightWorkerScope scope;  // bump/drain g_InflightWorkers (Globals.h)
        if (g_Unloading.load()) return;

        HttpResult res = HttpsGet(L"/v2/account/emotes", key);

        std::lock_guard<std::mutex> lk(s_mx);
        s_pending.ready = true;
        if (!res.ok) {
            s_pending.ok = false; s_pending.failKey = "opt.unl.err_http";
        } else if (res.status == 401 || res.status == 403) {
            s_pending.ok = false; s_pending.failKey = "opt.unl.err_key";
        } else if (res.status != 200) {
            s_pending.ok = false; s_pending.failKey = "opt.unl.err_http";
        } else {
            s_pending.ok = true; s_pending.json = res.body;
        }
    }).detach();
}

// ---- Hoard & Seek -----------------------------------------------------

void RaiseHsQuery() {
    HoardQueryApiRequest req;
    std::memset(&req, 0, sizeof(req));
    req.api_version = HOARD_API_VERSION;
    strncpy_s(req.requester,      sizeof(req.requester),      "emot3", _TRUNCATE);
    strncpy_s(req.endpoint,       sizeof(req.endpoint),       "/v2/account/emotes", _TRUNCATE);
    strncpy_s(req.response_event, sizeof(req.response_event), kHsResponseEvent, _TRUNCATE);
    req.account_name[0] = '\0';  // first configured account
    s_lastRaiseTick = GetTickCount64();
    // Must NOT hold any lock here - H&S may invoke our handler synchronously.
    APIDefs->Events.Raise(EV_HOARD_QUERY_API, &req);
}

// EVENT_CONSUME for kHsResponseEvent. May run on H&S's thread (or synchronously
// inside Raise) - only touches s_pending under s_mx.
void OnHoardResponse(void* aEventArgs) {
    if (!aEventArgs) return;
    const HoardQueryApiResponse* resp =
        static_cast<const HoardQueryApiResponse*>(aEventArgs);
    std::lock_guard<std::mutex> lk(s_mx);
    s_pending.ready = true;
    s_pending.pending = false;
    if (resp->status == HOARD_STATUS_OK) {
        s_pending.ok = true;
        s_pending.json.assign(resp->json);  // null-terminated; tiny payload
    } else if (resp->status == HOARD_STATUS_PENDING) {
        s_pending.ok = false;
        s_pending.pending = true;
    } else {
        s_pending.ok = false;
        s_pending.failKey = "opt.unl.err_denied";
    }
}

bool s_subscribed = false;

}  // namespace

void InitUnlockScan() {
    if (!APIDefs) return;

    // Reload-clean. The DLL persists across a Nexus reload (disable -> enable),
    // so this module's session statics survive the previous run. Reset them here
    // or: a sync whose worker bailed on g_Unloading at unload leaves s_busy stuck
    // true - blocking ALL future syncs forever - and once-per-session auto-sync
    // never re-arms. Workers have drained by now (AddonUnload waits) and the H&S
    // path is event-driven, so nothing mutates these concurrently; the lock just
    // covers the last-gasp race with a worker that hadn't checked g_Unloading yet.
    s_busy.store(false);
    s_startTick = s_lastRaiseTick = 0;
    s_usingHs       = false;
    s_autoFired     = false;
    s_autoFirstTick = 0;
    { std::lock_guard<std::mutex> lk(s_mx); s_pending = Pending{}; }

    // s_subscribed is paired with ShutdownUnlockScan's unsubscribe, so a normal
    // reload arrives here unsubscribed; guard only the subscribe.
    if (!s_subscribed) {
        APIDefs->Events.Subscribe(kHsResponseEvent, OnHoardResponse);
        s_subscribed = true;
    }
}

void ShutdownUnlockScan() {
    if (!APIDefs || !s_subscribed) return;
    APIDefs->Events.Unsubscribe(kHsResponseEvent, OnHoardResponse);
    s_subscribed = false;
}

void StartUnlockSync() {
    if (s_busy.load()) return;
    if (!APIDefs) return;
    { std::lock_guard<std::mutex> lk(s_mx); s_pending = Pending{}; }
    s_status = UnlockSyncStatus{};
    s_status.phase = UnlockSyncPhase::Fetching;
    s_busy.store(true);
    s_startTick = GetTickCount64();
    s_usingHs = (g_Settings.UnlockApiKeySource == 0);  // 0 = H&S, 1 = own key

    LOG_INFO("Unlock sync start: source=%s (additive)",
             s_usingHs ? "Hoard&Seek" : "own-key");

    if (s_usingHs) {
        RaiseHsQuery();
    } else {
        if (g_Settings.Gw2ApiKey.empty()) { Fail("opt.unl.err_nokey"); return; }
        StartOwnKeyFetch();
    }
}

void DrainUnlockSync() {
    // Auto-sync: once per session, a few seconds after the first tick (which is
    // roughly addon/game start), if enabled and a usable source exists. The
    // delay lets an out-of-order Hoard & Seek finish loading. The sync is always
    // additive (adds found unlocks, never locks).
    {
        // s_autoFired / s_autoFirstTick are file-scope (reset by InitUnlockScan on
        // reload) so auto-sync re-arms after a Nexus reload - see the decls above.
        const unsigned long long now = GetTickCount64();
        if (s_autoFirstTick == 0) s_autoFirstTick = now;
        if (!s_autoFired && g_Settings.UnlockAutoSync && !s_busy.load() &&
            now - s_autoFirstTick > 4000) {
            bool usable = (g_Settings.UnlockApiKeySource == 0) ||           // H&S
                          (g_Settings.UnlockApiKeySource == 1 &&            // own key
                           !g_Settings.Gw2ApiKey.empty());
            if (usable) {
                s_autoFired = true;
                LOG_INFO("Unlock auto-sync: triggering on session start");
                StartUnlockSync();
            }
        }
    }

    if (!s_busy.load()) return;

    // Pull any handoff the worker / H&S handler left us.
    bool haveResult = false, isPending = false, ok = false;
    std::string json, failKey;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (s_pending.ready) {
            haveResult = true;
            isPending  = s_pending.pending;
            ok         = s_pending.ok;
            json       = std::move(s_pending.json);
            failKey    = s_pending.failKey;
            s_pending  = Pending{};
        }
    }

    const unsigned long long now = GetTickCount64();

    if (haveResult) {
        if (ok) {
            ApplyResult(json);
        } else if (isPending) {
            s_status.phase = UnlockSyncPhase::PendingPermission;  // popup shown
        } else {
            Fail(failKey.empty() ? "opt.unl.err_http" : failKey.c_str());
        }
        return;
    }

    // No result yet: drive H&S retry / timeouts (own-key always resolves via
    // the worker, so these only matter for the H&S path).
    if (s_usingHs) {
        if (s_status.phase == UnlockSyncPhase::PendingPermission) {
            if (now - s_startTick > kPendingCapMs) { Fail("opt.unl.err_denied"); return; }
            if (now - s_lastRaiseTick > kPendingRetryMs) RaiseHsQuery();
        } else if (now - s_startTick > kNoResponseMs) {
            Fail("opt.unl.err_noresp");  // H&S silent => probably not installed
        }
    } else if (now - s_startTick > 20000) {
        Fail("opt.unl.err_http");  // safety net; WinHTTP timeouts are shorter
    }
}

const UnlockSyncStatus& UnlockSyncStatusInfo() { return s_status; }
bool UnlockSyncBusy() { return s_busy.load(); }

#ifdef EMOT3_DEVTOOLS
#include "DevStateInspector.h"
// Runtime-inspector section: GW2-API unlock-sync status (phase + last result), so a
// "did my sync actually work?" can be checked without re-running it.
static DevStateRegistrar s_unlockSection(DevStateCat::Config, "Unlock sync", [] {
    const UnlockSyncStatus& st = UnlockSyncStatusInfo();
    const char* phase = "?";
    switch (st.phase) {
        case UnlockSyncPhase::Idle:              phase = "idle";     break;
        case UnlockSyncPhase::Fetching:          phase = "fetching"; break;
        case UnlockSyncPhase::PendingPermission: phase = "pending H&S permission"; break;
        case UnlockSyncPhase::Done:              phase = "done";     break;
        case UnlockSyncPhase::Failed:            phase = "failed";   break;
    }
    DevStateRow("phase",      "%s%s", phase, UnlockSyncBusy() ? " (busy)" : "");
    DevStateRow("key source", "%s", g_Settings.UnlockApiKeySource == 0 ? "Hoard & Seek" : "own key");
    DevStateRow("auto-sync",  "%s", g_Settings.UnlockAutoSync ? "on" : "off");
    DevStateRow("api count",  "%d", st.apiCount);
    DevStateRow("unlocked",   "%d", st.unlocked);
    DevStateRow("custom (unmatched)", "%d", st.custom);
    DevStateRow("unknown api ids",    "%d", st.unknownApi);
    if (st.phase == UnlockSyncPhase::Failed)
        DevStateRow("error key", "%s", st.errorKey.empty() ? "(none)" : st.errorKey.c_str());
});
#endif  // EMOT3_DEVTOOLS
