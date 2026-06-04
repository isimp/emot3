#pragma once
// =====================================================================
//  Runtime state inspector (Layer 2 of the dev-tools framework).
//
//  A single dev tool (registered into Layer 1 - see DevTools.h) whose
//  window is a stack of collapsible SECTIONS. The point is extensibility:
//  any module surfaces its own live runtime state by self-registering a
//  section near its own globals, so nobody has to edit a central
//  inspector and new state shows up automatically.
//
//  Gated by EMOT3_DEVTOOLS: in non-dev builds this header is empty and the
//  registrars below simply don't exist - which is fine because every
//  registration site is itself wrapped in `#ifdef EMOT3_DEVTOOLS`.
//
//  ---------------------------------------------------------------------
//  HOW TO ADD A RUNTIME-STATE SECTION (the common case):
//    In the .cpp that owns the state, at file scope:
//
//      #ifdef EMOT3_DEVTOOLS
//      #include "DevStateInspector.h"
//      static DevStateRegistrar s_myState("My section", [] {
//          DevStateRow("some flag", "%d", g_SomeFlag);
//          DevStateRow("a string",  "%s", g_Name.c_str());
//      });
//      #endif
//
//    The lambda must be captureless (it converts to a void(*)()). It runs
//    every frame the section is open; keep it read-only and cheap (no disk
//    I/O - this is a dev tool, but the no-blocking-I/O-in-render rule still
//    applies). It appears in the inspector window automatically, in
//    registration order.
// =====================================================================

#ifdef EMOT3_DEVTOOLS

// Render one "label .... value" row inside a section's emit callback.
// printf-style; value is right-of a fixed label column.
void DevStateRow(const char* label, const char* fmt, ...);

// Self-registering section. Construct one static instance per section (in
// the owning module's .cpp). `emit` draws the section's rows when it's open.
struct DevStateRegistrar {
    DevStateRegistrar(const char* title, void (*emit)());
};

namespace devstate {
// Runtime on/off for the inspector window (the Layer-1 tool's bool).
bool& Enabled();
// Draws the inspector window; registered into Layer 1 by RegisterBuiltinDevTools.
void  Render();
}  // namespace devstate

#endif  // EMOT3_DEVTOOLS
