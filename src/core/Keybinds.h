#pragma once

// Nexus keybind identifiers. Declared here so entry.cpp (which
// registers them) and NexusShortcut.cpp (which references them when
// wiring up the quick-access icon's left-click action) stay in sync —
// the strings must match exactly across the API boundary.
//
// Defined in entry.cpp via extern linkage rather than as inline /
// constexpr in the header, which keeps the project's C++ standard
// agnostic — namespace-scope constexpr variables changed ODR rules in
// C++17 and we don't want to pin the project to that.

extern const char* const KB_TOGGLE_MAIN;
extern const char* const KB_TOGGLE_QB;
