#pragma once

// Emote / me-mote usage tracking — the data behind the Quickbar's synthetic
// "Recently used" and "Frequently used" categories. Quickbar-only (these
// categories are non-editable by nature, so there's no Library section).
//
// Model: ONE bounded, newest-first event log (last kUsageLog sends) in its own
// addons/emot3/usage.json (separate from settings.json so a send doesn't rewrite
// all settings). Both views derive from the log, so changing behaviour naturally
// ages out of the FIFO window — no forever-counters, no decay clock.
//
//   - Recently used = first-seen-distinct walk of the log (newest first).
//   - Frequently used = frequency tally over the log (tiebreak: most-recent
//     first occurrence). "Frequently", not "Most", because Record() coalesces
//     consecutive duplicates (below), so it ranks by "times switched to" rather
//     than raw key-mashing — mashing one emote can't crown itself or flush the
//     window's diversity.
//
// Threading: Record() can be reached off the render thread (a keybind / radial
// Invoke in later features), so the log is guarded by an internal mutex. The
// derive functions run on the render thread.
//
// Persistence: usage owns its own file and is NOT part of the SaveScheduler.
// Usage is low-stakes, auto-derived convenience data, so a change costs zero disk
// I/O during play — it persists once at AddonUnload via Flush() (which also fires
// on a clean exit and on every addon disable/reload). The one eager exception is
// Reset(), which writes immediately so a manual clear can't be undone by a later
// crash. Trade-off: a hard game crash loses the current session's usage.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Settings.h"  // FavoriteRef, EFavoriteRefType

namespace usage {

// Window size: the last N send events. Bounds storage and is the recency
// horizon — older usage falls out as new (distinct-from-previous) sends arrive.
constexpr size_t kUsageLog = 300;

// Load usage.json (called once at AddonLoad). Remembers the path for the unload
// Flush(). Tolerant of a missing / malformed file (starts empty), validates each
// entry (known type, non-empty id), and caps to kUsageLog. Does NOT mark anything
// dirty (a load is not a change).
void Load(const std::string& path);

// Write usage.json now, but only if the log changed this session (cheap no-op
// otherwise). Call from AddonUnload — that's the normal persist point.
void Flush();

// Record one real send (call AFTER the send gate passes, so only actual
// sends/fills count). Consecutive-coalesced: if (type,id) is already the most
// recent entry it's a no-op — a burst of one emote costs a single slot instead
// of flushing every other emote out of the window. Unconditional: always logs,
// even when both categories are disabled, so a category is pre-populated the
// moment it's enabled. Bumps the version epoch + marks dirty (saved at unload).
void Record(EFavoriteRefType type, const std::string& id);

// Derived views, capped to `max`, memoized on the version epoch (recomputed only
// when the log changes). Returned by const-ref into module-static caches — valid
// until the next call that changes the log; copy if you need to keep it.
const std::vector<FavoriteRef>& RecentlyUsed(size_t max);
const std::vector<FavoriteRef>& Frequent(size_t max);

// Drop every occurrence of one ref (an emote/me-mote was deleted). Bumps the
// version + marks dirty when something changed.
void RemoveRef(EFavoriteRefType type, const std::string& id);

// Drop every ref of one catalog (the "Clear catalog" action wipes all emotes /
// all /me-motes). Bumps the version + marks dirty when something changed.
void RemoveAllOfType(EFavoriteRefType type);

// Drop refs that are live in neither catalog. `isLive` is a caller-supplied free
// function (no capture) so this module stays independent of the emote/me-mote
// catalogs. Called at AddonLoad (after both catalogs load) + after a clear.
void PruneDead(bool (*isLive)(EFavoriteRefType, const std::string&));

// Clear the whole log (the "Reset usage" button). Writes immediately so a manual
// clear can't be silently undone by a later crash (the only eager write here).
void Reset();

// Dev/observability (cheap; safe in any build).
uint64_t Version();
size_t   LogSize();
size_t   ApproxBytes();

}  // namespace usage
