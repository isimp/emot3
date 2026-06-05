#pragma once
#include <cstdint>

// Minimal mirror of the public Hoard & Seek integration header.
//
//   Upstream : https://github.com/PieOrCake/hoard_and_seek
//   File     : include/HoardAndSeekAPI.h
//   Pinned   : release v0.10.1.0  (HOARD_API_VERSION 3)
//
// Upstream's own integration guidance is "just copy the header" - there is no
// link-time dependency; addons communicate purely via Nexus events. Its full
// header declares ~a dozen request/response structs; we deliberately reproduce
// ONLY the pieces emot3 uses (the generic authenticated-API proxy:
// EV_HOARD_QUERY_API + HoardQueryApiRequest/Response) rather than vendor the lot.
//
// The struct layouts below are byte-identical to upstream so the event payloads
// read correctly. If HOARD_API_VERSION ever changes upstream, re-verify these
// layouts against include/HoardAndSeekAPI.h at the new release before bumping.
//
// H&S is an optional dependency: when it isn't installed nothing raises/handles
// these events, so the path is simply inert.

#define HOARD_API_VERSION 3

// HoardQueryApiResponse::status values.
#define HOARD_STATUS_OK       0  // request succeeded
#define HOARD_STATUS_DENIED   1  // user denied permission
#define HOARD_STATUS_PENDING  2  // permission popup shown, not yet decided

// Raise with a HoardQueryApiRequest*; H&S raises your response_event with a
// HoardQueryApiResponse*.
#define EV_HOARD_QUERY_API "EV_HOARD_QUERY_API"

// CRITICAL: upstream declares these inside `#pragma pack(push, 1)`, so the
// structs are byte-packed (no alignment padding). We MUST match that exactly or
// the uint32_t fields after the odd-sized char arrays (json_length, and hence
// the json buffer) land at the wrong offsets and read garbage.
#pragma pack(push, 1)

struct HoardQueryApiRequest {
    uint32_t api_version;       // HOARD_API_VERSION
    char     requester[64];     // addon name (used for permission checks)
    char     endpoint[256];     // GW2 API path, e.g. "/v2/account/emotes"
    char     response_event[64];// event name H&S raises with the response
    char     account_name[64];  // v3+: which account's key (empty = first)
};

struct HoardQueryApiResponse {
    uint32_t api_version;       // HOARD_API_VERSION
    uint8_t  status;            // HOARD_STATUS_OK / DENIED / PENDING
    char     account_name[64];  // echoed account
    char     endpoint[256];     // echoed endpoint
    uint32_t json_length;       // actual JSON length (may exceed buffer)
    uint8_t  truncated;         // 1 if response was truncated to fit
    char     json[65536];       // raw JSON response, null-terminated
};

#pragma pack(pop)
