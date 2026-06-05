#pragma once
// =====================================================================
//  Dev-tools framework (Layer 1) - the registry that hosts every
//  diagnostic overlay.
//
//  Gated by EMOT3_DEVTOOLS, the +devtools flavor (the DevTools, PlusDevTools
//  and Debug build configs - see src/emot3.vcxproj). When it is NOT defined the
//  whole framework compiles to no-op stubs, so nothing of the dev tools ships
//  in the public emot3.dll (Distribution) or the emot3_plus.dll (Plus)
//  builds. This is a SEPARATE flavor axis from EMOT3_PLUS, which adds the
//  AV-sensitive input-swallows (QuickbarWheel / SendSuppress); a build
//  can have either, both, or neither.
//
//  Two layers:
//    Layer 1 (this file) - a registry of whole overlay windows. Each
//      tool contributes { id, label, enabled-bool, render-fn }. One
//      Nexus render callback (RenderDevToolOverlays) draws every enabled
//      tool; one Options block (RenderDevToolToggles) draws a
//      "[debug] <label>" checkbox per tool.
//    Layer 2 (DevStateInspector.h) - a registry of state SECTIONS
//      inside the one "Runtime state" tool, so modules can surface their
//      own live state without anyone editing a central inspector.
//
//  ---------------------------------------------------------------------
//  HOW TO ADD A NEW DEV TOOL (a whole overlay window):
//    1. Write `Enabled()` returning a `bool&` and a `Render()` that draws
//       your ImGui window (guard it with `if (!Enabled()) return;`, like
//       Profiling.h does), both inside `#ifndef EMOT3_DEVTOOLS #else`.
//    2. Add ONE line to RegisterBuiltinDevTools() in DevTools.cpp:
//         RegisterDevTool({ "myid", "My label", &mytool::Enabled(),
//                           mytool::Render });
//    It then appears automatically in Options (a "[debug] My label"
//    checkbox) and is drawn every frame when enabled. No entry.cpp or
//    Options.cpp edits needed.
//
//  If all you want is a new block of read-only runtime numbers, you do
//  NOT need a whole tool - add a state SECTION instead (see
//  DevStateInspector.h). That is the common case.
// =====================================================================

#ifndef EMOT3_DEVTOOLS

// Non-dev builds: the framework is absent. These no-op stubs let the
// call sites in entry.cpp / Options.cpp stay unconditional.
inline void RegisterBuiltinDevTools() {}
inline void RenderDevToolOverlays()   {}
inline void RenderDevToolToggles()    {}

#else  // ---- dev build: full implementation ----

// One registered overlay. `enabled` points at the tool's own runtime bool
// (typically the function-local static behind its Enabled() accessor, which
// has a stable address); `render` draws the tool's window when enabled.
struct DevToolDef {
    const char* id;       // stable id (used for the checkbox's ImGui ##id)
    const char* label;    // shown as "[debug] <label>" in Options
    bool*       enabled;  // runtime on/off (not persisted; reset each launch)
    void      (*render)();// draws the tool's window; called only while enabled
};

// Append a tool to the registry. Call from RegisterBuiltinDevTools().
void RegisterDevTool(const DevToolDef& d);

// Register every built-in dev tool. Called once at AddonLoad (entry.cpp).
void RegisterBuiltinDevTools();

// Nexus render callback (registered once in entry.cpp): draws each enabled
// tool's window. Safe to call before any tool registered (draws nothing).
void RenderDevToolOverlays();

// Options: draws a "[debug] <label>" checkbox per registered tool.
void RenderDevToolToggles();

#endif  // EMOT3_DEVTOOLS
