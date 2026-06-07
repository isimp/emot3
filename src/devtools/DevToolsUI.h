#pragma once
// =====================================================================
//  Shared dev-tools UI helpers - small building blocks reused across the
//  diagnostic overlays (perf, memory, airborne tuner, ...): auto-unit byte
//  formatting, a boolean state dot, a plot threshold line, and a right-aligned
//  numeric table cell.
//
//  Gated by EMOT3_DEVTOOLS like the rest of the dev tools; in the shipped
//  Distribution/Plus builds the whole header is empty. Only dev TUs include it,
//  so no non-dev stub is needed. Consolidates copies that previously lived in
//  MemoryMonitor.cpp and AirborneTuner.h. Intentionally depends only on ImGui
//  (NOT Profiling.h) so Profiling.h can include it for NumCell without a cycle;
//  the ring-buffer flatten helper lives in Profiling.h next to prof::Ring.
// =====================================================================

#ifdef EMOT3_DEVTOOLS

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

#include "imgui/imgui.h"

namespace devui {

// Human byte size with an auto KB/MB unit. Threshold at 10 MB: the addon's own
// heap sits at ~50-200 KB, so KB precision below 10 MB keeps small deltas
// readable; above it MB reads tidier. e.g. "182.3 KB" / "12.40 MB".
inline std::string FormatBytesAuto(uint64_t bytes) {
    char buf[32];
    if (bytes < 10ull * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%.2f MB", bytes / (1024.0 * 1024.0));
    return buf;
}

// Coloured dot for boolean live readouts: bright red when on, dim grey off.
// Advances the cursor by one text line (square) so it sits inline before a label.
inline void StateDot(bool on) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float  h = ImGui::GetTextLineHeight();
    float  r = h * 0.35f;
    ImU32  col = on ? IM_COL32(220, 70, 70, 255) : IM_COL32(100, 100, 100, 160);
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + h * 0.5f), r, col);
    ImGui::Dummy(ImVec2(h, h));
}

// Draw a horizontal threshold line across the last plotted item's rect.
// `valueRange` is the Y span used by PlotLines (scaleMax - scaleMin),
// `valueMin` is its scaleMin. col is IM_COL32.
inline void DrawThresholdOnLastPlot(float value, float valueMin, float valueRange,
                                    ImU32 col) {
    ImVec2 a = ImGui::GetItemRectMin();
    ImVec2 b = ImGui::GetItemRectMax();
    if (valueRange <= 0.f) return;
    float tNorm = (value - valueMin) / valueRange;  // 0 = bottom, 1 = top
    if (tNorm < 0.f || tNorm > 1.f) return;          // off-plot
    float y = b.y - tNorm * (b.y - a.y);             // ImGui plots top->bottom = max->min
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), col);
}

// Right-aligned formatted text in the CURRENT table cell - numbers read better
// flush-right. Call after TableSetColumnIndex / TableNextColumn.
inline void NumCell(const char* fmt, ...) {
    char buf[64];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    float w     = ImGui::CalcTextSize(buf).x;
    float avail = ImGui::GetContentRegionAvail().x;   // width left in the cell
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w));
    ImGui::TextUnformatted(buf);
}

}  // namespace devui

#endif  // EMOT3_DEVTOOLS
