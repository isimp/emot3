#pragma once
// =====================================================================
//  Dev-only memory monitor. Surfaces "how much memory the plugin uses"
//  and helps spot leaks during development. Gated by EMOT3_DEVTOOLS so
//  it compiles to nothing in the public Distribution + Plus DLLs.
//
//  Three signals, ordered by precision:
//
//   1. DLL heap bytes (the precise primary). Global operator new/delete
//      overrides in MemoryMonitor.cpp bump g_DllBytesAllocated /
//      g_DllAllocCount on every allocation made via `::operator new`
//      from code linked into this DLL. Catches: our STL containers,
//      nlohmann_json, every `new` we write. Does NOT catch ImGui (Nexus
//      owns the ImGui allocator at entry.cpp:142-145 - deliberately not
//      replaced, since ImGui is shared Nexus infrastructure, not addon
//      memory). Also does not catch direct malloc/free in CRT internals
//      (printf, std::ofstream temps - bounded, transient, not a leak
//      surface). Cost: one LOCK XADD per alloc / dealloc (~1-2 ns).
//
//   2. Per-subsystem counters (the structural detail). One row per
//      tracked container - g_Emotes, g_QbIconRects, TextCache, I18n
//      cache, Favorites, ManuallyUnlocked, Notifier pending, inflight
//      workers, Profiler displayMap. Each row carries a 120-sample
//      rolling history (prof::Ring) for trend + windowed peak. Counts
//      are exact (.size()); byte estimators are within ~2x (see
//      memmon::mem_est in the .cpp). The point is the SHAPE over time,
//      not the absolute number - a row that grows monotonically points
//      straight at the leak.
//
//   3. Process working set / private bytes (sanity context). Read via
//      GetProcessMemoryInfo on PROCESS_MEMORY_COUNTERS_EX. Includes the
//      GW2 process - useful only as a third-tier backstop ("tracked
//      rows are flat but WS is growing - there's an alloc neither
//      caught").
//
//  Baseline + monotonic-tail check: [Set baseline] captures every value.
//  A row goes red when it has grown vs. baseline AND the last 30 history
//  samples are monotonically non-decreasing - the cheap leak heuristic
//  that ignores expected one-time fills (TextCache plateauing after a
//  scroll burst).
//
//  Touch points: #include "MemoryMonitor.h"; the RegisterDevTool entry in
//  DevTools.cpp; that's it. No entry.cpp edit, no per-call instrumentation
//  anywhere - the operator new override and Sample() do all the work.
// =====================================================================

#ifndef EMOT3_DEVTOOLS

// Non-dev build: the tool is absent. Stub Enabled() so the DevTools.h
// stub in DevTools.cpp can still take its address (though in practice
// RegisterDevTool is itself stubbed away in non-dev builds).
namespace memmon { inline bool& Enabled() { static bool b = false; return b; } }
inline void RenderMemoryMonitor() {}

#else  // ---- dev build: full implementation ----

#include <atomic>
#include <cstddef>

namespace memmon {

// Live byte / allocation counters, bumped by the global operator new and
// delete overrides defined in MemoryMonitor.cpp. Read here (and from the
// overlay) on any thread via .load(memory_order_relaxed) - the counter is
// monotonic-ish; we don't need synchronization with other data, only safe
// reads of the raw value.
extern std::atomic<size_t> g_DllBytesAllocated;
extern std::atomic<size_t> g_DllAllocCount;

// Function-local statics so the addresses are stable across re-registration.
// Not persisted - reset to defaults at each addon launch, matching the
// other dev-tools' pattern (Profiling.h, QuickbarDebug.h).
inline bool& Enabled() { static bool b = false; return b; }
inline bool& Paused()  { static bool b = false; return b; }

}  // namespace memmon

// Overlay window. Registered into the dev-tools framework from
// DevTools.cpp::RegisterBuiltinDevTools(). Called once per frame by
// RenderDevToolOverlays when memmon::Enabled() is true.
void RenderMemoryMonitor();

#endif  // EMOT3_DEVTOOLS
