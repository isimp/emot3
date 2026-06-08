#include "IconDrawing.h"
#include "Globals.h"   // APIDefs, Texture (EMOT3_UI_* texture lookups)

#include "imgui/imgui.h"

#include <algorithm>   // std::min / std::max

// All UI icons (star / paperclip / lock / target_dot) are now sourced
// from bundled PNGs in resources/ui/, loaded into Nexus' texture cache
// at addon load time (see LoadUiIconOverrides). The hand-drawn
// fallbacks that used to live here have been removed: the bundle is
// shipped with the DLL so it's always available, and a missing PNG
// would now indicate a build problem rather than a normal case worth
// catching. If a texture isn't present, we draw nothing rather than
// falling back to an inferior look - the absence is more obvious and
// the build problem gets noticed instead of papered over.

void DrawStarIcon(ImVec2 c, float r, ImU32 col) {
    Texture* tex = APIDefs ? APIDefs->Textures.Get("EMOT3_UI_STAR") : nullptr;
    if (!tex || !tex->Resource) return;
    int alpha = (col >> IM_COL32_A_SHIFT) & 0xFF;
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex->Resource,
        ImVec2(c.x - r, c.y - r),
        ImVec2(c.x + r, c.y + r),
        ImVec2(0, 0), ImVec2(1, 1),
        IM_COL32(255, 255, 255, alpha));
}

void DrawPaperclipIcon(ImVec2 c, float r, ImU32 col) {
    Texture* tex = APIDefs ? APIDefs->Textures.Get("EMOT3_UI_PAPERCLIP") : nullptr;
    if (!tex || !tex->Resource) return;
    int alpha = (col >> IM_COL32_A_SHIFT) & 0xFF;
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex->Resource,
        ImVec2(c.x - r, c.y - r),
        ImVec2(c.x + r, c.y + r),
        ImVec2(0, 0), ImVec2(1, 1),
        IM_COL32(255, 255, 255, alpha));
}

void DrawCollapseArrow(ImVec2 c, float r, bool collapsed, ImU32 col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (collapsed) {
        // Right-pointing (>): content is hidden.
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.5f, c.y - r),
                              ImVec2(c.x - r * 0.5f, c.y + r),
                              ImVec2(c.x + r * 0.7f, c.y), col);
    } else {
        // Down-pointing (v): content is shown.
        dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.5f),
                              ImVec2(c.x + r, c.y - r * 0.5f),
                              ImVec2(c.x, c.y + r * 0.7f), col);
    }
}

void DrawTrashIcon(ImVec2 c, float r, ImU32 col, ImDrawList* dl) {
    if (!dl) dl = ImGui::GetWindowDrawList();
    float w = r * 1.5f, h = r * 1.8f;
    float t = std::max(1.f, r * 0.18f);     // stroke thickness
    ImVec2 b0(c.x - w * 0.5f, c.y - h * 0.30f);   // bin body top-left
    ImVec2 b1(c.x + w * 0.5f, c.y + h * 0.55f);   // bin body bottom-right
    dl->AddRect(b0, b1, col, 1.5f, 0, t);                                   // body
    float lidY = b0.y - t * 1.2f;
    dl->AddLine(ImVec2(c.x - w * 0.66f, lidY), ImVec2(c.x + w * 0.66f, lidY), col, t);  // lid
    dl->AddLine(ImVec2(c.x - r * 0.30f, lidY - t * 1.8f),
                ImVec2(c.x + r * 0.30f, lidY - t * 1.8f), col, t);          // handle
    dl->AddLine(ImVec2(c.x - w * 0.16f, b0.y + t * 1.5f),
                ImVec2(c.x - w * 0.16f, b1.y - t * 1.5f), col, t);          // ribs
    dl->AddLine(ImVec2(c.x + w * 0.16f, b0.y + t * 1.5f),
                ImVec2(c.x + w * 0.16f, b1.y - t * 1.5f), col, t);
}

void DrawLockOverlay() {
    Texture* tex = APIDefs ? APIDefs->Textures.Get("EMOT3_UI_LOCK") : nullptr;
    if (!tex || !tex->Resource) return;
    ImVec2 imin = ImGui::GetItemRectMin();
    ImVec2 imax = ImGui::GetItemRectMax();
    float w = imax.x - imin.x;
    float h = imax.y - imin.y;
    if (w < 8.f || h < 8.f) return;
    ImVec2 center((imin.x + imax.x) * 0.5f, (imin.y + imax.y) * 0.5f);
    float sz = std::min(w, h) * 0.55f;
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex->Resource,
        ImVec2(center.x - sz * 0.5f, center.y - sz * 0.5f),
        ImVec2(center.x + sz * 0.5f, center.y + sz * 0.5f));
}

void DrawTargetableDot(float dotSz, float alphaMul) {
    Texture* tex = APIDefs ? APIDefs->Textures.Get("EMOT3_UI_TARGET") : nullptr;
    if (!tex || !tex->Resource) return;
    if (dotSz < 4.f) return;  // below this it's an unreadable speck - skip

    // dotSz is computed by the caller (RenderEmoteCell) from a per-mode
    // reference, so the dot looks the same size in every view mode and tracks
    // the icon-scale slider. The dot is anchored to the top-right corner of
    // the last submitted item (the emote button).
    //
    // Pad is the gap from the item's top/right edge: a fraction of the dot,
    // with a small floor so the dot never hugs the border at small sizes (the
    // old fraction-only pad glued tiny dots to the corner).
    ImVec2 imin = ImGui::GetItemRectMin();
    ImVec2 imax = ImGui::GetItemRectMax();
    float pad = std::max(2.5f, dotSz * 0.3f);

    ImVec2 mn(imax.x - dotSz - pad, imin.y + pad);
    ImVec2 mx(mn.x + dotSz, mn.y + dotSz);
    int a = (int)(255 * alphaMul);
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex->Resource, mn, mx,
                 ImVec2(0, 0), ImVec2(1, 1),
                 IM_COL32(255, 255, 255, a));
}

void DrawMeMoteIndicator(float indSz, float alphaMul) {
    // /me-motes and IsTargetable are mutually exclusive (the Emote-only
    // target dot draws via DrawTargetableDot above, which never runs on a
    // /me-mote cell), so the two indicators share the same top-right anchor.
    // Sourced from the bundled (or user-overridden) UI icon "me_mote_dot.png"
    // - drop a PNG under addons/emot3/icons/ui/ to replace it, same path as
    // every other UI override (see entry.cpp's icons/ui/README.txt).
    Texture* tex = APIDefs ? APIDefs->Textures.Get("EMOT3_UI_ME_MOTE") : nullptr;
    if (!tex || !tex->Resource) return;
    if (indSz < 4.f) return;

    ImVec2 imin = ImGui::GetItemRectMin();
    ImVec2 imax = ImGui::GetItemRectMax();
    float pad = std::max(2.5f, indSz * 0.3f);

    ImVec2 mn(imax.x - indSz - pad, imin.y + pad);
    ImVec2 mx(mn.x + indSz, mn.y + indSz);
    int a = (int)(255 * alphaMul);
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex->Resource, mn, mx,
                 ImVec2(0, 0), ImVec2(1, 1),
                 IM_COL32(255, 255, 255, a));
}
