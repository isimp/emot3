#pragma once

#include <string>

// GW2-API emote-unlock sync.
//
// Reads the account's unlocked emotes from GW2's official API
// (GET /v2/account/emotes) and reconciles the user's per-emote unlock state
// (g_Settings.ManuallyUnlocked) - never the catalog. The unlocked list is
// matched to our bundled unlockable emotes by id, or by command via the
// 4-language index (GetBundledUnlockInfo, EmoteData.h), so user-added /
// localized commands still resolve. Emotes we can't resolve are left untouched.
//
// Two key sources (user choice, g_Settings.UnlockApiKeySource):
//   - Hoard & Seek proxy (PieOrCake/hoard_and_seek): emot3 never sees the key;
//     H&S calls the endpoint after a one-time permission grant.
//   - The user's own account-scope API key (g_Settings.Gw2ApiKey): direct
//     WinHTTP call.
// Both are optional - absent source => the sync just reports an error.

// Subscribe to the H&S response event. Call from AddonLoad.
void InitUnlockScan();
// Unsubscribe. Call from AddonUnload.
void ShutdownUnlockScan();

// Begin a sync from current settings (mode/source/key). No-op if one is already
// running. Call from the render thread (the Options "Sync now" button).
void StartUnlockSync();

// Apply any completed fetch and drive H&S permission retries / timeouts. Call
// once per frame from the render thread (AddonOptions). All g_Settings mutation
// happens here, render-side.
void DrainUnlockSync();

// ---- UI status ----
enum class UnlockSyncPhase { Idle, Fetching, PendingPermission, Done, Failed };

struct UnlockSyncStatus {
    UnlockSyncPhase phase = UnlockSyncPhase::Idle;
    // Result tallies, valid when phase == Done.
    int apiCount   = 0;  // ids the account API returned
    int unlocked   = 0;  // matched emotes the account has unlocked (marked unlocked)
    int custom     = 0;  // catalog unlockables we couldn't match (left as-is)
    int unknownApi = 0;  // API ids with no catalog match
    // i18n key for the failure reason, valid when phase == Failed.
    std::string errorKey;
};

const UnlockSyncStatus& UnlockSyncStatusInfo();
bool UnlockSyncBusy();
