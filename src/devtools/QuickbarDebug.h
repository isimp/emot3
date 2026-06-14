#pragma once
// =====================================================================
//  Dev-only Quickbar diagnostics (sizing snapshot + live grid globals). Shown
//  as a section ("Quickbar", under "Content & catalogs") in the Runtime state
//  inspector - the DevStateRegistrar lives in Quickbar.cpp and calls
//  EmitQuickbarSection() below. (Was briefly its own overlay window; folded back
//  into the inspector so all dev state lives in one place.)
//
//  Same gating as Profiling.h: present ONLY when EMOT3_DEVTOOLS is defined
//  (the Debug and Dev configs); compiled away in the public emot3.dll
//  (Distribution) and emot3_plus.dll (Plus) builds. Lets us see the exact
//  pixel numbers behind the fit/snap behaviour (window vs child viewport
//  vs ImGui ContentSize vs ScrollMax vs inset/step) so sizing bugs can be
//  diagnosed from real values instead of guessed.
//
//  Touch points (all guarded): #include "QuickbarDebug.h"; the capture block +
//  the DevStateRegistrar in Quickbar.cpp (which wires the inspector section).
// =====================================================================

#ifndef EMOT3_DEVTOOLS

namespace qbdbg { inline bool& Enabled() { static bool b = false; return b; } }

#else  // ---- dev build ----

#include "imgui/imgui.h"
#include "DevStateInspector.h"  // DevStateRow (aligned key/value rows)
#include "QuickbarGeometry.h"   // g_Qb* live grid globals (folded-in grid block)

namespace qbdbg {

inline bool& Enabled() { static bool b = false; return b; }

// Snapshot of one Quickbar render's sizing, captured after EndChild.
struct QbMetrics {
    float winW = 0, winH = 0;            // outer window size
    float availX = 0, availY = 0;        // child content region (the viewport)
    float contentX = 0, contentY = 0;    // ImGui's measured child ContentSize
    float scrollMaxX = 0, scrollMaxY = 0;  // ImGui's pixel scroll max
    float scrollX = 0, scrollY = 0;        // current scroll offset (resting)
    float cellMaxX = 0, cellMaxY = 0;      // cell-aligned max (g_QbMaxScroll*)
    float stepX = 0, stepY = 0;          // cell + spacing
    float insetX = 0, insetY = 0;        // window - childAvail (chrome, snap input)
    float gridTopOffset = 0;             // topPad + ItemSpacing.y
    float gutter = 0;                    // reserved custom-bar gutter (fit)
    int   cols = 0, rows = 0, items = 0;
    bool  fit = false, horiz = false, barActive = false, scrollable = false;
};

inline QbMetrics& M() { static QbMetrics m; return m; }

}  // namespace qbdbg

// Emit the Quickbar sizing rows as a runtime-inspector SECTION (registered from
// Quickbar.cpp under "Content & catalogs"); the inspector owns the window +
// collapsing header, so this draws rows only. The "content - avail" line is the
// smoking gun for spurious overflow (>0 means the content measures bigger than
// the viewport, so a scrollbar appears even when it "should" fit).
inline void EmitQuickbarSection() {
    const ::qbdbg::QbMetrics& m = ::qbdbg::M();
    // ---- Sizing snapshot (captured after EndChild last frame) ----
    DevStateRow("flags", "fit=%d horiz=%d bar=%d scrollable=%d",
                m.fit, m.horiz, m.barActive, m.scrollable);
    DevStateRow("items", "%d  (%d cols x %d rows)", m.items, m.cols, m.rows);
    ImGui::Separator();
    DevStateRow("win",        "%.2f x %.2f", m.winW, m.winH);
    DevStateRow("childAvail", "%.2f x %.2f", m.availX, m.availY);
    DevStateRow("content",    "%.2f x %.2f", m.contentX, m.contentY);
    DevStateRow("scrollMax",  "%.2f x %.2f", m.scrollMaxX, m.scrollMaxY);
    DevStateRow("scroll",     "%.2f x %.2f", m.scrollX, m.scrollY);
    DevStateRow("cellMax",    "%.2f x %.2f", m.cellMaxX, m.cellMaxY);
    DevStateRow("step",       "%.2f x %.2f", m.stepX, m.stepY);
    DevStateRow("inset",      "%.2f x %.2f", m.insetX, m.insetY);
    DevStateRow("gridTopOff", "%.2f  (gutter %.2f)", m.gridTopOffset, m.gutter);
    ImGui::Separator();
    DevStateRow("content - avail", "X %+.2f   Y %+.2f",
                m.contentX - m.availX, m.contentY - m.availY);

    // ---- Live grid globals (window position, geometry-valid, overflow) ----
    ImGui::Separator();
    DevStateRow("window x,y",     "%.0f, %.0f", g_QbWinX, g_QbWinY);
    DevStateRow("geometry valid", "%s", g_QbGeometryValid ? "yes" : "no");
    DevStateRow("overflow",       "%s", g_QbOverflow ? "yes" : "no");
}

#endif  // EMOT3_DEVTOOLS
