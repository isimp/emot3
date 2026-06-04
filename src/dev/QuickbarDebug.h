#pragma once
// =====================================================================
//  Dev-only Quickbar sizing readout. A dev tool, registered into the
//  dev-tools framework (DevTools.h / RegisterBuiltinDevTools).
//
//  Same gating as Profiling.h: present ONLY when EMOT3_DEVTOOLS is defined
//  (the Debug and Dev configs); compiled away in the public emot3.dll
//  (Distribution) and emot3_plus.dll (Plus) builds. Lets us see the exact
//  pixel numbers behind the fit/snap behaviour (window vs child viewport
//  vs ImGui ContentSize vs ScrollMax vs inset/step) so sizing bugs can be
//  diagnosed from real values instead of guessed.
//
//  Touch points (all guarded): #include "QuickbarDebug.h"; the capture
//  block in Quickbar.cpp; the RegisterDevTool entry in DevTools.cpp (which
//  wires the toggle + render).
// =====================================================================

#ifndef EMOT3_DEVTOOLS

namespace qbdbg { inline bool& Enabled() { static bool b = false; return b; } }
inline void RenderQbSizingOverlay() {}

#else  // ---- dev build ----

#include "imgui/imgui.h"

namespace qbdbg {

inline bool& Enabled() { static bool b = false; return b; }

// Snapshot of one Quickbar render's sizing, captured after EndChild.
struct QbMetrics {
    float winW = 0, winH = 0;            // outer window size
    float availX = 0, availY = 0;        // child content region (the viewport)
    float contentX = 0, contentY = 0;    // ImGui's measured child ContentSize
    float scrollMaxX = 0, scrollMaxY = 0;
    float stepX = 0, stepY = 0;          // cell + spacing
    float insetX = 0, insetY = 0;        // window - childAvail (chrome, snap input)
    float gridTopOffset = 0;             // topPad + ItemSpacing.y
    float gutter = 0;                    // reserved custom-bar gutter (fit)
    int   cols = 0, rows = 0, items = 0;
    bool  fit = false, horiz = false, barActive = false, scrollable = false;
};

inline QbMetrics& M() { static QbMetrics m; return m; }

}  // namespace qbdbg

// Draw the readout when enabled. Registered as its own render callback so it
// shows regardless of which windows are open. The "content - avail" line is
// the smoking gun for spurious overflow (>0 means the content measures bigger
// than the viewport, so a scrollbar appears even when it "should" fit).
inline void RenderQbSizingOverlay() {
    if (!::qbdbg::Enabled()) return;
    const ::qbdbg::QbMetrics& m = ::qbdbg::M();
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (ImGui::Begin("emot3 QB sizing##qbdbg", &::qbdbg::Enabled(),
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav)) {
        ImGui::Text("fit=%d horiz=%d bar=%d scrollable=%d",
                    m.fit, m.horiz, m.barActive, m.scrollable);
        ImGui::Text("items=%d  cols=%d rows=%d", m.items, m.cols, m.rows);
        ImGui::Separator();
        ImGui::Text("win        %8.2f x %8.2f", m.winW, m.winH);
        ImGui::Text("childAvail %8.2f x %8.2f", m.availX, m.availY);
        ImGui::Text("content    %8.2f x %8.2f", m.contentX, m.contentY);
        ImGui::Text("scrollMax  %8.2f x %8.2f", m.scrollMaxX, m.scrollMaxY);
        ImGui::Text("step       %8.2f x %8.2f", m.stepX, m.stepY);
        ImGui::Text("inset      %8.2f x %8.2f", m.insetX, m.insetY);
        ImGui::Text("gridTopOff %8.2f  gutter %6.2f", m.gridTopOffset, m.gutter);
        ImGui::Separator();
        ImGui::Text("content - avail   X %+7.2f   Y %+7.2f",
                    m.contentX - m.availX, m.contentY - m.availY);
    }
    ImGui::End();
}

#endif  // EMOT3_DEVTOOLS
