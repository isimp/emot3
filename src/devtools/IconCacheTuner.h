#pragma once
// =====================================================================
//  Dev-only "Icon cache" tuner - view + edit the icon-cache config
//  (icon_cache.json: max_icon_dim / max_folder_icons / pool_budget_mb)
//  at runtime. Committing a value writes icon_cache.json (created on first
//  write - the file isn't seeded otherwise) and bumps the catalog epochs so
//  the new caps re-apply (picker re-scans on next open; content keys
//  re-resolve). Read-only readout of the resolved file path.
//
//  Gated by EMOT3_DEVTOOLS. Only touch point besides this file is the one
//  RegisterDevTool line in DevTools.cpp - no Options.cpp edit (the toggle
//  auto-appears under Developer).
// =====================================================================

#ifndef EMOT3_DEVTOOLS

namespace iconcachetuner { inline bool& Enabled() { static bool b = false; return b; } }
inline void RenderIconCacheTuner() {}

#else  // ---- dev build: full implementation ----

#include "imgui/imgui.h"

#include "Globals.h"          // MarkEmotesDirty / MarkMeMotesDirty
#include "IconCacheConfig.h"  // g_IconCache, g_IconCacheConfigPath, SaveIconCacheConfig

namespace iconcachetuner {
inline bool& Enabled() { static bool b = false; return b; }
}

inline void RenderIconCacheTuner() {
    if (!iconcachetuner::Enabled()) return;
    ImGui::SetNextWindowBgAlpha(0.88f);
    if (!ImGui::Begin("emot3 icon cache##iconcachetuner", &iconcachetuner::Enabled(),
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Icon-cache tuning. A change writes icon_cache.json");
    ImGui::TextDisabled("(created on first edit) and re-applies the caps.");
    ImGui::Spacing();

    // InputInt for precise entry - type an exact value or use the +/- step
    // buttons. We clamp to range + persist + re-resolve only when an edit
    // FINISHES (IsItemDeactivatedAfterEdit), so a half-typed value isn't clamped
    // mid-entry (typing "512" wouldn't survive a per-keystroke clamp) and a quick
    // edit doesn't thrash the disk or the texture cache.
    auto clampi = [](int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); };
    bool committed = false;
    ImGui::PushItemWidth(210.f);

    ImGui::InputInt("max icon dim (px)", &g_IconCache.maxIconDim, 16, 64);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("User PNG/JPEG icons larger than this (per side) are skipped "
                          "(letter fallback) + logged. Bundled icons are exempt. Range 16-512.");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        g_IconCache.maxIconDim = clampi(g_IconCache.maxIconDim, 16, 512);
        committed = true;
    }

    ImGui::InputInt("max folder icons", &g_IconCache.maxFolderIcons, 64, 256);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How many PNGs the icon picker lists from the icons/ folder. Range 0-2048.");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        g_IconCache.maxFolderIcons = clampi(g_IconCache.maxFolderIcons, 0, 2048);
        committed = true;
    }

    ImGui::InputInt("pool budget (MB, 0=off)", &g_IconCache.poolBudgetMB, 16, 64);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Ceiling on total icon VRAM; past it, new icons use the "
                          "letter fallback until restart. 0 = unlimited. Range 0-512.");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        g_IconCache.poolBudgetMB = clampi(g_IconCache.poolBudgetMB, 0, 512);
        committed = true;
    }

    ImGui::PopItemWidth();

    if (committed) {
        SaveIconCacheConfig(g_IconCacheConfigPath);   // creates the file on first write
        MarkEmotesDirty();                            // re-resolve so the caps re-apply
        MarkMeMotesDirty();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("file: %s",
        g_IconCacheConfigPath.empty() ? "(path unset)" : g_IconCacheConfigPath.c_str());

    ImGui::End();
}

#endif  // EMOT3_DEVTOOLS
