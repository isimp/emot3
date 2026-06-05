#include "UpdateCheck.h"

// +plus builds only (Plus / PlusDevTools / Debug); base builds use Nexus' native
// updater - see UpdateCheck.h for the gate rationale.
#ifdef EMOT3_PLUS

#include "Globals.h"     // APIDefs, g_Unloading, InflightWorkerScope (+ Windows.h, Nexus.h)
#include "Logging.h"

#include <nlohmann/json.hpp>

#include <winhttp.h>

#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "winhttp.lib")

// The addon definition (with the live Version) is a global owned by entry.cpp.
extern AddonDefinition AddonDef;

namespace {

constexpr const char* kReleasesUrl =
    "https://github.com/isimp/emot3/releases/latest";

// State shared between the check worker and the render-thread drain. All access
// guarded by s_mx. Reset by InitUpdateCheck so a Nexus reload re-checks.
std::mutex  s_mx;
bool        s_started   = false;  // worker launched (render thread only)
bool        s_done      = false;  // worker finished
bool        s_available = false;  // a newer release was found
std::string s_latest;             // newer version string (no leading 'v')
bool        s_notified  = false;  // QuickAccess badge already sent (render thread only)
unsigned long long s_firstTick = 0;

constexpr unsigned long long kDelayMs = 4000;  // let things settle after load

// ---- HTTP (mirrors core/UnlockScan.cpp HttpsGet) ----------------------
struct HttpResult { bool ok = false; int status = 0; std::string body; };

// Secure GET to api.github.com. GitHub requires a User-Agent (supplied via
// WinHttpOpen) and we send the documented Accept header.
HttpResult GithubGet(const wchar_t* path) {
    HttpResult r;
    HINTERNET hS = WinHttpOpen(L"emot3",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return r;
    WinHttpSetTimeouts(hS, 5000, 5000, 8000, 8000);
    HINTERNET hC = WinHttpConnect(hS, L"api.github.com",
                                  INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hC) { WinHttpCloseHandle(hS); return r; }
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return r; }

    const wchar_t* headers = L"Accept: application/vnd.github+json";
    BOOL ok = WinHttpSendRequest(hR, headers, (DWORD)-1L,
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

// Strip a leading 'v'/'V' from a tag for display ("v1.0.1" -> "1.0.1").
std::string StripLeadingV(const std::string& tag) {
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) return tag.substr(1);
    return tag;
}

// Parse up to 4 dotted integers from a release tag (ignoring a leading 'v' and
// any non-numeric suffix like "-beta") and compare against AddonDef.Version.
// Returns true when the tag is strictly newer than the running build.
bool TagIsNewer(const std::string& tag) {
    int v[4] = { 0, 0, 0, 0 };
    const char* p = tag.c_str();
    if (*p == 'v' || *p == 'V') ++p;
    int idx = 0;
    while (*p && idx < 4) {
        if (*p >= '0' && *p <= '9') { v[idx] = v[idx] * 10 + (*p - '0'); ++p; }
        else if (*p == '.')         { ++idx; ++p; }
        else break;  // stop at a pre-release suffix etc.
    }
    const int cur[4] = {
        AddonDef.Version.Major, AddonDef.Version.Minor,
        AddonDef.Version.Build, AddonDef.Version.Revision
    };
    for (int i = 0; i < 4; ++i)
        if (v[i] != cur[i]) return v[i] > cur[i];
    return false;
}

}  // namespace

void InitUpdateCheck() {
    std::lock_guard<std::mutex> lk(s_mx);
    s_started = false; s_done = false; s_available = false;
    s_latest.clear(); s_notified = false; s_firstTick = 0;
}

void DrainUpdateCheck() {
    if (!APIDefs) return;

    unsigned long long now = GetTickCount64();
    if (s_firstTick == 0) s_firstTick = now;

    // Launch the check exactly once, a few seconds after the first tick.
    if (!s_started && now - s_firstTick >= kDelayMs) {
        s_started = true;
        std::thread([]() {
            InflightWorkerScope scope;  // bump/drain g_InflightWorkers (Globals.h)
            if (g_Unloading.load()) return;
            HttpResult res = GithubGet(L"/repos/isimp/emot3/releases/latest");
            if (g_Unloading.load()) return;

            bool newer = false; std::string latest;
            if (res.ok && res.status == 200) {
                try {
                    nlohmann::json j = nlohmann::json::parse(res.body);
                    std::string tag = j.value("tag_name", std::string());
                    if (!tag.empty() && TagIsNewer(tag)) {
                        newer = true; latest = StripLeadingV(tag);
                    }
                } catch (...) { /* malformed -> silent no-op */ }
            } else {
                LOG_DEBUG("update: GitHub check failed (ok=%d status=%d)",
                          (int)res.ok, res.status);
            }
            std::lock_guard<std::mutex> lk(s_mx);
            s_done = true; s_available = newer; s_latest = latest;
        }).detach();
    }

    // Apply the result once: badge the shortcut icon via the Nexus builtin.
    bool fire = false;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (s_done && s_available && !s_notified) { s_notified = true; fire = true; }
    }
    if (fire) {
        APIDefs->QuickAccess.Notify("emot3_shortcut");
        LOG_INFO("update: newer Plus release available (%s)",
                 PlusLatestVersion().c_str());
    }
}

bool PlusUpdateAvailable() {
    std::lock_guard<std::mutex> lk(s_mx);
    return s_done && s_available;
}

std::string PlusLatestVersion() {
    std::lock_guard<std::mutex> lk(s_mx);
    return s_latest;
}

const char* ReleasesUrl() { return kReleasesUrl; }

#ifdef EMOT3_DEVTOOLS
// ---- Dev-tool hooks (the "[debug] Update check" tool) -----------------
// Present only in plus-flavored DEV builds (Dev/Debug). They drive the real
// pipeline above so the tool tests the shipping code path, not a mock.

void RunUpdateCheckNow() {
    {   // re-arm so DrainUpdateCheck launches the worker again
        std::lock_guard<std::mutex> lk(s_mx);
        s_started = false; s_done = false; s_available = false;
        s_latest.clear(); s_notified = false;
    }
    // Non-zero + far in the past so DrainUpdateCheck's delay gate is already
    // satisfied -> it launches on the very next tick (no 4s wait).
    s_firstTick = 1;
}

void ForceUpdateAvailable(const std::string& ver) {
    std::lock_guard<std::mutex> lk(s_mx);
    s_started = true; s_done = true; s_available = true;
    s_latest = ver.empty() ? std::string("0.0.0-test") : ver;
    s_notified = false;  // let DrainUpdateCheck re-badge the icon
}

void ResetUpdateCheck() { InitUpdateCheck(); }
#endif  // EMOT3_DEVTOOLS

#endif  // EMOT3_PLUS
