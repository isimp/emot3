#include "Cells.h"
#include "Globals.h"
#include "QuickbarGeometry.h"  // g_QbStep* / g_QbMaxScroll* / ... (grid layout)
#include "QbHitRects.h"        // g_QbIconRects (register cell hit-rects)
#include "I18n.h"
#include "OptionsCommon.h"  // InputFieldWithHint (shared text-field standard)
#include "Settings.h"
#include "SaveScheduler.h"  // RequestSave (debounced, off-thread settings writes)
#include "EmoteData.h"
#include "EmoteAction.h"
#include "Favorites.h"
#include "RadialExport.h"  // RetargetExportsCategory (keep exported wheels linked on rename)
#include "StringUtil.h"      // TrimWhitespace (category rename)
#include "Icons.h"
#include "IconDrawing.h"     // DrawStarIcon / DrawTrashIcon / cell overlays
#include "Layout.h"
#include "MeMotes.h"         // /me-mote struct + lookups (RenderMeMoteCellBody)
#include "TextCache.h"       // EllipsizeCached / FitNameCached (per-cell label memo)
#include "CharacterState.h"  // g_QbUnusableKey (reason-aware block UI)
#include "Feedback.h"        // ShowFeedback - in-window refusal line (replaces SendAlert)
#include "Logging.h"         // LOG_DEBUG (category rename)
#include "Profiling.h"       // PROFILE_SCOPE (no-op without EMOT3_DEVTOOLS)

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"  // ImGuiWindow::ScrollbarY for the QB anticipation

// ---------------------------------------------------------------------------
// Drag-drop "insertion gap" helpers (used by the grid-wide drop zone in
// RenderEmoteSection). A "gap" g is a slot in the destination vector in the
// range [0, N]: g==0 is before the first cell, g==N is after the last. The
// drop zone shows a vertical line at gap g and inserts there - no cell ever
// gets highlighted, which reads much clearer than "is this to the left or
// the right of the icon I'm hovering?".
// ---------------------------------------------------------------------------

// Result of a drop hit-test. `gap` is the insertion slot [0, N] used for the
// mutation. `anchorCell` + `anchorRight` record WHICH cell the mouse picked
// and on which side - the line is drawn relative to that cell so it follows
// the mouse's row. (gap == anchorCell + anchorRight, but the gap alone loses
// the row: the slot between the last cell of a row and the first of the next
// is one gap with two valid visual positions, and we want the one the mouse
// is actually over.)
struct DropHit { int gap; int anchorCell; bool anchorRight; };

// Computed grid geometry for one RenderEmoteSection pass: where cells land and how
// many fit. Populated AFTER the fit/scroll math chooses cols/rows (that math is left
// untouched) and threaded into the hit-test, drop zone, cell loop and preview so
// they share one source instead of ~10 loose locals. cellColRow folds the
// fill-order math that was previously copy-pasted in three places.
struct GridLayout {                    // aggregate (no member initializers) — always
    float baseX, baseY;                // brace-initialized whole at its one call site
    float stepX, stepY;                // grid origin / cell pitch (cell + spacing)
    float cellW, cellH;                // cell size
    float spacingX, spacingY;          // item spacing
    int   cols, rows;                  // grid laid out this frame
    bool  horizontal;                  // true = column-major fill (horizontal Quickbar)

    // (col,row) of cell index j under the active fill order.
    void cellColRow(int j, int& col, int& row) const {
        if (horizontal) { col = j / rows; row = j % rows; }
        else            { col = j % cols; row = j / cols; }
    }
    // Full grid bounding box (cols x rows, incl. the empty tail of a partial last
    // row) - the drop-zone span. cols*stepX - spacingX <= avail by construction.
    float gridW() const { return std::max(1.f, cols * stepX - spacingX); }
    float gridH() const { return std::max(1.f, rows * stepY - spacingY); }
};

// Hit-test the mouse against the grid. Walks every cell, finds the one in the
// mouse's row band whose centre is closest in X, and picks the side (left or
// right of that cell). Falls back to nearest-centre-in-2D if the mouse sits
// outside every row band (shouldn't happen while hovering the grid-sized drop
// zone, but cheap). Assumes row-major layout (the only layout the drop zone
// runs in - reordering is main-panel-favorites only, never the horizontal
// Quickbar), so `horizontal` only affects the cell-position math here.
static DropHit ComputeDropHit(const std::vector<CellInfo>& items, const GridLayout& g) {
    ImVec2 winPos = ImGui::GetWindowPos();
    float  sx = ImGui::GetScrollX(), sy = ImGui::GetScrollY();
    ImVec2 m  = ImGui::GetMousePos();
    int    N  = (int)items.size();

    int  bandCell = N - 1; bool bandRight = true; float bestBand = FLT_MAX;
    int  c2       = N - 1; bool r2        = true; float best2D   = FLT_MAX;
    for (int j = 0; j < N; ++j) {
        int col, row;
        g.cellColRow(j, col, row);
        float cx     = winPos.x - sx + g.baseX + col * g.stepX;
        float cy     = winPos.y - sy + g.baseY + row * g.stepY;
        float center = cx + g.cellW * 0.5f;
        bool  right  = (m.x >= center);
        float dx     = fabsf(m.x - center);
        if (m.y >= cy - g.spacingY * 0.5f && m.y <= cy + g.cellH + g.spacingY * 0.5f) {
            if (dx < bestBand) { bestBand = dx; bandCell = j; bandRight = right; }
        }
        float cyc = cy + g.cellH * 0.5f;
        float d2  = (m.x - center) * (m.x - center) + (m.y - cyc) * (m.y - cyc);
        if (d2 < best2D) { best2D = d2; c2 = j; r2 = right; }
    }
    DropHit h;
    if (bestBand < FLT_MAX) { h.anchorCell = bandCell; h.anchorRight = bandRight; }
    else                    { h.anchorCell = c2;       h.anchorRight = r2;        }
    h.gap = h.anchorCell + (h.anchorRight ? 1 : 0);
    return h;
}

// Cross-talk between a drop *target* that refuses the drag and the drag
// *source*, which owns the only floating tooltip while a drag is in flight.
// The built-in Core / Unlockable sections never accept a drop; when one is
// hovered it stamps the current frame here, and the source reads it to append
// a "can't drop here" line - so a refused drag explains itself instead of just
// doing nothing. Stamp-and-stale self-clears (no per-frame reset needed) and
// tolerates one frame of lag for the case where the hovered section renders
// after the source cell, which is imperceptible mid-drag.
static int s_dragRejectFrame = -10;
static void StampDragReject() { s_dragRejectFrame = ImGui::GetFrameCount(); }
static bool DragRejectFresh() { return ImGui::GetFrameCount() - s_dragRejectFrame <= 1; }

// Drag-reject feedback for a target that refuses an emote drop: the tooltip
// line (StampDragReject) plus a red outline around the refusing rect (screen
// space). Shared by the built-in Core/Unlockable grid and their collapsed
// section headers, so the refusal looks the same wherever it happens.
static void StampDropReject(ImVec2 mn, ImVec2 mx) {
    StampDragReject();
    ImGui::GetWindowDrawList()->AddRect(mn, mx, IM_COL32(220, 90, 80, 200), 4.f, 0, 2.f);
}

void DrawDropInsertionLine(float x, float top, float bottom) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 col = IM_COL32(110, 180, 255, 255);  // addon blue (cf. Layout.cpp)
    dl->AddRectFilled(ImVec2(x - 1.5f, top), ImVec2(x + 1.5f, bottom), col, 1.5f);
    // End caps so it reads as an insertion marker, not a stray bar.
    dl->AddRectFilled(ImVec2(x - 4.f, top - 2.f),    ImVec2(x + 4.f, top + 2.f),    col, 1.f);
    dl->AddRectFilled(ImVec2(x - 4.f, bottom - 2.f), ImVec2(x + 4.f, bottom + 2.f), col, 1.f);
}

// Horizontal category-reorder indicator: a full-width (inset) blue bar at screen
// Y `yScreen`, drawn in the inter-category gap. Used by both the per-header
// "insert before" target and the "move to end" zone, so they look identical.
void DrawCategoryDropLine(float yScreen) {
    float x0 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x + 6.f;
    float x1 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - 6.f;
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(x0, yScreen - 1.5f), ImVec2(x1, yScreen + 1.5f),
        IM_COL32(110, 180, 255, 255), 1.5f);  // addon blue, rounded caps
}

// A drop that lands immediately before or after the dragged item's own slot,
// within its own category, changes nothing - suppress the line and skip the
// mutation. Cross-category and catalog drags are always meaningful here
// (catalog de-dup is handled in ApplyEmoteDrop).
static bool IsNoopSelfMove(const EmoteDragPayload& src, int dstCat, int gap) {
    if (src.categoryIdx != dstCat) return false;
    return gap == src.emoteIdx || gap == src.emoteIdx + 1;
}

// Apply a drop at gap g. Mirrors the three old per-cell cases (catalog add,
// same-category reorder, cross-category move) but keyed on an explicit gap
// instead of a left/right-of-cell guess. All index math is clamped.
void ApplyEmoteDrop(const EmoteDragPayload& src, int dstCat, int gap) {
    int N = (int)g_Settings.FavoriteCategories.size();
    if (dstCat < 0 || dstCat >= N) return;

    // The drag payload carries its kind via src.type — Emote drags from the
    // Core / Unlockable built-in sections or any favorites category, /me-mote
    // drags from the built-in /me-motes section or any favorites category.
    // Catalog-source drags construct the FavoriteRef with that type; within-
    // favorites moves keep the moved ref's existing type intact (the type
    // doesn't change on reorder).
    if (src.categoryIdx < 0) {
        // Built-in catalog source: a NEW favorite. Locked emotes can't be
        // favorited (blocked at the source, guarded again here; /me-motes carry
        // isLocked=false). InsertRefAt dedups within dstCat, saves, and bumps the
        // cached catalog view (the favorited-id union grew).
        if (src.isLocked) return;
        InsertRefAt(dstCat, gap, src.type, std::string(src.id));
    } else {
        int srcCat = src.categoryIdx, srcIdx = src.emoteIdx;
        // Locked emotes can reorder within their category but not move across.
        bool blocked = src.isLocked && srcCat != dstCat;
        if (blocked || srcCat < 0 || srcCat >= N) return;
        // A move keeps the id favorited (union unchanged): the helpers save but do
        // NOT bump the view; the per-category ORDER is read live by the build.
        if (srcCat == dstCat) MoveRefWithinCategory(srcCat, srcIdx, gap);
        else                  MoveRefAcrossCategories(srcCat, srcIdx, dstCat, gap);
    }
}

// Accept a hovering FAV_DRAG emote drop into category `dstCat` at a fixed `gap`,
// applying it on delivery via ApplyEmoteDrop (which enforces the locked / de-dup
// / cross-category rules). Returns true while a meaningful (non-no-op) payload is
// hovering, so the caller can draw its own "drop here" cue. The caller must have
// already opened a drag-drop target. Reused by the empty-category placeholder and
// the collapsed-category header (the grid uses ComputeDropHit for its per-gap
// preview, so it keeps its own accept).
bool AcceptEmoteDropInto(int dstCat, int gap) {
    const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(
        "FAV_DRAG", ImGuiDragDropFlags_AcceptPeekOnly);
    if (!pl) return false;
    EmoteDragPayload src = *(const EmoteDragPayload*)pl->Data;
    if (IsNoopSelfMove(src, dstCat, gap)) return false;
    if (pl->IsDelivery()) ApplyEmoteDrop(src, dstCat, gap);
    return true;
}

// The execute-variant menu items, shared by the Quickbar and main-panel
// context menus (identical set, so muscle memory transfers). The plain-vs-
// target item flips with SendTargetableOnTarget: normally left-click sends
// plain so the menu offers "Send on target"; when the setting auto-targets a
// targetable emote on click, the menu instead offers "Send normally" and drops
// the now-redundant "Send on target". The two sync items state their modifiers
// explicitly, so they're unchanged either way. Caller handles the locked case.
bool RenderSendVariants(const Emote& e) {
    bool sent = false;
    bool autoTarget = e.IsTargetable && g_Settings.SendTargetableOnTarget;
    if (autoTarget) {
        if (ImGui::MenuItem(L("cells.send_normal")))
            sent = SendOrFillEmote(e, false, false) || sent;
    } else {
        if (ImGui::MenuItem(L("cells.send_target"), "@", false, e.IsTargetable))
            sent = SendOrFillEmote(e, true, false) || sent;
    }
    if (ImGui::MenuItem(L("cells.send_sync"), "*"))
        sent = SendOrFillEmote(e, false, true) || sent;
    if (ImGui::MenuItem(L("cells.send_target_sync"), "@ *", false, e.IsTargetable))
        sent = SendOrFillEmote(e, true, true) || sent;
    return sent;
}

// /me-mote cell renderer. Same signature as RenderEmoteCell so RenderEmoteSection
// can dispatch without knowing the kind. Significantly shorter than the Emote
// path because /me-motes carry no IsCore / IsTargetable / lock state — no
// targetable dot, no lock overlay, no unlock toggle in the right-click menu.
// The icon comes from EnsureMeMoteTexture (the shared content cache: a custom
// PNG, an icons/<id>.png drop-in, or bundled AI - see ResolveMeMoteIconSource),
// else the styled letter fallback. Drag-drop is wired: a drag source
// in the Library + favorites sections (never the Quickbar) emits a FAV_DRAG
// payload tagged EFavoriteRefType::MeMote, which ApplyEmoteDrop routes into the
// /me-mote namespace. Click + right-click variants + Quickbar click-rect
// tracking are all wired here too.
static void RenderMeMoteCellBody(const CellInfo& ci, int sectionRow,
                                 float cellX, float cellY,
                                 float cellW, float cellH, float iconSz,
                                 EViewMode mode,
                                 bool allowReorder,
                                 int  categoryIdx,
                                 bool isQuickbar)
{
    (void)sectionRow;   // allowReorder IS read below (gates the drag source)
    const MeMote& m = *ci.m;

    // Quickbar-only "can't send right now" dim, same as the Emote path —
    // /me-motes use the same chat-injection gates (combat, textbox, held key,
    // airborne, ...) so the dim semantics apply identically.
    bool blocked   = isQuickbar && g_QbUnusableKey != nullptr;
    float alphaMul = blocked ? 0.40f : 1.f;
    bool dimmed    = blocked;

    // /me-mote indicator (top-right accent). Sized like the target dot so it
    // tracks the icon-scale slider; same per-mode reference so it reads the
    // same in every view mode.
    const bool showIndicator = g_Settings.ShowMeMoteIndicator;
    float indSz = 0.f;
    if (showIndicator) {
        float indRef = (mode == EViewMode::TextOnly) ? cellH : iconSz;
        indSz = indRef * (mode == EViewMode::TextOnly ? 0.45f : 0.24f);
    }

    // Namespace the ImGui ID stack BEFORE pushing the Id. An Emote and a
    // /me-mote can share an Id (independent namespaces), and the Emote path
    // pushes the bare Id at the same scope — without this extra level the two
    // cells' buttons AND their "##ctx" context-menu popups hash to the same
    // ImGui ID, so right-clicking one opens a merged menu rendering BOTH cells'
    // items. Mirrors the EMOT3_MM_ texture-cache + meMote TextCache split.
    ImGui::PushID("memote");
    ImGui::PushID(m.Id.c_str());
    if (dimmed) ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                     ImGui::GetStyle().Alpha * alphaMul);

    bool clicked = false;
    if (mode == EViewMode::TextOnly) {
        // Reserve label space for the indicator the same way the Emote path
        // reserves it for the target dot.
        float labelMaxW = cellW - 12.f;
        if (showIndicator) labelMaxW -= indSz + 6.f;
        const auto& cached = TextCache::EllipsizeCached(m.Id, m.Name, mode, labelMaxW, /*meMote=*/true);
        ImGui::SetCursorPos(ImVec2(cellX, cellY));
        int hiContrastPushes = 0;
        if (isQuickbar && g_Settings.QuickbarHighContrast)
            hiContrastPushes = PushHighContrastButtonStyles(/*includeFrameBg=*/false);
        clicked = ImGui::Button(cached.label.c_str(), ImVec2(cellW, cellH));
        if (hiContrastPushes > 0) ImGui::PopStyleColor(hiContrastPushes);
    } else {
        const int pad = 2;
        float btnTotal = iconSz + pad * 2;
        float btnX     = cellX + (cellW - btnTotal) * 0.5f;
        ImGui::SetCursorPos(ImVec2(btnX, cellY));
        // Custom icon if loaded, else styled letter button. /me-motes don't
        // have bundled art or AI-fallback layers — it's the user's PNG (set
        // via Options > /me-motes > Browse) or nothing.
        Texture* tex = EnsureMeMoteTexture(m);  // lazy: loads on first show
        if (tex && tex->Resource) {
            clicked = ImGui::ImageButton((ImTextureID)tex->Resource,
                ImVec2(iconSz, iconSz), ImVec2(0, 0), ImVec2(1, 1), pad);
        } else {
            clicked = RenderStyledFallback("##fb", m.Name.c_str(), btnTotal, alphaMul);
        }

        // Compact mode strip — same dark band as the Emote path, with the
        // /me-mote's Name ellipsized inside it.
        if (mode == EViewMode::Compact) {
            ImVec2 itemMin = ImGui::GetItemRectMin();
            ImVec2 itemMax = ImGui::GetItemRectMax();
            float  fontH   = ImGui::GetFontSize();
            float  stripPadY = 2.f;
            float  stripH    = fontH + stripPadY * 2.f;
            ImVec2 stripMin(itemMin.x, itemMax.y - stripH);
            ImVec2 stripMax(itemMax.x, itemMax.y);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            int bgA = (int)(180.f * alphaMul);
            dl->AddRectFilled(stripMin, stripMax, IM_COL32(0, 0, 0, bgA));
            float stripPadX = 3.f;
            float maxTextW  = (stripMax.x - stripMin.x) - stripPadX * 2.f;
            const auto& cached = TextCache::EllipsizeCached(m.Id, m.Name, mode, maxTextW, /*meMote=*/true);
            float tx = stripMin.x + (stripMax.x - stripMin.x - cached.size.x) * 0.5f;
            float ty = stripMin.y + stripPadY;
            int textA = (int)(245.f * alphaMul);
            dl->AddText(ImVec2(tx, ty), IM_COL32(245, 245, 245, textA),
                        cached.label.c_str());
        }
    }

    // /me-mote indicator overlay — anchored to the last submitted item (the
    // button), so it scales with the button automatically.
    if (showIndicator) DrawMeMoteIndicator(indSz, alphaMul);

    if (isQuickbar) {
        g_QbIconRects.emplace_back(ImGui::GetItemRectMin(),
                                   ImGui::GetItemRectMax());
    }

    // Drag source — mirrors RenderEmoteCell's source block. Enabled from
    // user-favorites cells (for reorder / cross-category move) AND from the
    // Library's built-in /me-motes section (for "add to favorites by
    // dragging"). Quickbar cells are never sources — drag-drop is a main-
    // panel interaction. /me-motes have no lock concept so isLocked is
    // always false; payload.type is MeMote so ApplyEmoteDrop constructs the
    // FavoriteRef in the right namespace.
    bool dragSourceEnabled = (categoryIdx >= 0) ? allowReorder : (!isQuickbar);
    if (dragSourceEnabled) {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            EmoteDragPayload payload {};
            payload.categoryIdx = categoryIdx;
            payload.emoteIdx    = ci.favIdx;
            payload.isLocked    = false;
            payload.type        = EFavoriteRefType::MeMote;
            strncpy_s(payload.id, sizeof(payload.id),
                      m.Id.c_str(), _TRUNCATE);
            ImGui::SetDragDropPayload("FAV_DRAG", &payload, sizeof(payload));
            ImGui::Text(L("cells.moving"), m.Name.c_str());
            // Source-aware reject hint, same shape as the Emote path: a
            // favorite dragged off into a non-target tells the user where the
            // real "Remove from favorites" lives; a catalog drag mis-aimed
            // gets the "drop onto a category" cue.
            if (DragRejectFresh()) {
                const char* why = (categoryIdx >= 0) ? L("cells.no_drop_remove")
                                                     : L("cells.no_drop_here");
                ImGui::TextColored(ImVec4(1.f, 0.45f, 0.40f, 1.f), "%s", why);
            }
            ImGui::EndDragDropSource();
        }
    }

    // Right-click context menu. Quickbar shows execute-variants only;
    // main panel adds favorites management. (The drag SOURCE is wired above;
    // drop TARGETING is handled at the section level by RenderEmoteSection's
    // grid-wide zone, type-agnostically — no per-cell target wiring needed.)
    if (dimmed) ImGui::PopStyleVar();
    if (ImGui::BeginPopupContextItem("##ctx")) {
        // Send variants — You / All are visible always but disabled when
        // their body is empty (so users see the concept exists), matching the
        // plan's "discoverable" gray state. Plain Send is omitted (left-click
        // is the default).
        auto sendVariantItem = [&](EMeMoteVariant variant, const char* labelKey,
                                   bool enabled) {
            if (!enabled) {
                ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                    ImGui::GetStyle().Alpha * 0.5f);
            }
            if (ImGui::MenuItem(L(labelKey)) && enabled)
                SendOrFillMeMote(m, variant);
            if (!enabled) {
                ImGui::PopStyleVar();
                ImGui::PopItemFlag();
            }
        };
        if (isQuickbar) {
            if (blocked) {
                ImGui::TextDisabled("%s", L(g_QbUnusableKey));
            } else {
                sendVariantItem(EMeMoteVariant::You, "cells.send_you", !m.TextYou.empty());
                sendVariantItem(EMeMoteVariant::All, "cells.send_all", !m.TextAll.empty());
            }
        } else {
            // Favorites management (mirrors the emote path but operates on
            // MeMote-typed refs via the generic FavoriteRef API).
            int currentCat = FindCategoryContaining(EFavoriteRefType::MeMote, m.Id);
            int catCount   = (int)g_Settings.FavoriteCategories.size();
            if (catCount == 0) {
                ImGui::TextDisabled("%s", L("cells.no_categories"));
                ImGui::Separator();
                if (ImGui::MenuItem(L("cells.create_and_add"))) {
                    EnsureDefaultCategory();
                    AddRefToCategory(0, EFavoriteRefType::MeMote, m.Id, false);
                }
            } else if (currentCat < 0) {
                if (catCount == 1) {
                    char lbl[96];
                    std::snprintf(lbl, sizeof(lbl), L("cells.add_to"),
                                  g_Settings.FavoriteCategories[0].Name.c_str());
                    if (ImGui::MenuItem(lbl))
                        AddRefToCategory(0, EFavoriteRefType::MeMote, m.Id, false);
                } else if (catCount > 1) {
                    if (ImGui::BeginMenu(L("cells.add_to_category"))) {
                        for (int i = 0; i < catCount; ++i) {
                            if (ImGui::MenuItem(g_Settings.FavoriteCategories[i].Name.c_str()))
                                AddRefToCategory(i, EFavoriteRefType::MeMote, m.Id, false);
                        }
                        ImGui::EndMenu();
                    }
                }
            } else {
                if (catCount > 1) {
                    if (ImGui::BeginMenu(L("cells.move_to_category"))) {
                        for (int i = 0; i < catCount; ++i) {
                            if (i == currentCat) continue;
                            if (ImGui::MenuItem(g_Settings.FavoriteCategories[i].Name.c_str()))
                                AddRefToCategory(i, EFavoriteRefType::MeMote, m.Id, false);
                        }
                        ImGui::EndMenu();
                    }
                }
                if (ImGui::MenuItem(L("cells.remove_from_fav")))
                    RemoveRefFromCategories(EFavoriteRefType::MeMote, m.Id);
            }
            ImGui::Separator();
            sendVariantItem(EMeMoteVariant::You, "cells.send_you", !m.TextYou.empty());
            sendVariantItem(EMeMoteVariant::All, "cells.send_all", !m.TextAll.empty());
        }
        ImGui::EndPopup();
    }

    // Tooltip — Name + (optional) search-match note + right-click hint. No
    // Command (doesn't exist) and no body preview (could be sentence-long).
    // ci.searchNote explains an alias-only search hit (same shape Emote
    // cells use). Suppressed by the same Quickbar opt-out emotes use.
    bool suppressTip = isQuickbar && !g_Settings.ShowQuickbarTooltips;
    if (!suppressTip &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        std::string t = m.Name;
        if (!ci.searchNote.empty()) { t += '\n'; t += ci.searchNote; }
        t += '\n'; t += L("cells.rightclick");
        TooltipTextRaw(t.c_str());
    }

    if (dimmed) ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                     ImGui::GetStyle().Alpha * alphaMul);

    // Full mode label below the button.
    if (mode == EViewMode::Full) {
        const int pad = 2;
        float btnTotal = iconSz + pad * 2;
        float labelY   = cellY + btnTotal + 4.f;
        const auto& cached = TextCache::FitNameCached(m.Id, m.Name, cellW, /*meMote=*/true);
        ImGui::SetCursorPos(ImVec2(cellX + (cellW - cached.size1.x) * 0.5f, labelY));
        ImGui::TextUnformatted(cached.line1.c_str());
        if (!cached.line2.empty()) {
            float ly2 = labelY + cached.size1.y;
            ImGui::SetCursorPos(ImVec2(cellX + (cellW - cached.size2.x) * 0.5f, ly2));
            ImGui::TextUnformatted(cached.line2.c_str());
        }
    }

    if (dimmed) ImGui::PopStyleVar();
    if (clicked) {
        if (blocked) {
            ShowFeedback(L(g_QbUnusableKey));
        } else {
            SendOrFillMeMote(m, EMeMoteVariant::Default);
        }
    }
    ImGui::PopID();   // m.Id
    ImGui::PopID();   // "memote" namespace
}

void RenderEmoteCell(const CellInfo& ci, int sectionRow,
                     float cellX, float cellY,
                     float cellW, float cellH, float iconSz,
                     EViewMode mode,
                     bool allowReorder,
                     int  categoryIdx,
                     bool isQuickbar)
{
    PROFILE_SCOPE("cells");  // dev perf overlay - per visible cell (calls = cells/frame)
    // Dispatch: /me-mote cells take the simpler path (no lock/target/IsCore
    // concepts, no drag-drop yet). Emote cells fall through to the existing
    // renderer below — its behavior is unchanged for the existing Emote path.
    if (ci.m) {
        RenderMeMoteCellBody(ci, sectionRow, cellX, cellY, cellW, cellH,
                             iconSz, mode, allowReorder, categoryIdx, isQuickbar);
        return;
    }
    const Emote& e   = *ci.e;
    // Unlocked state is precomputed once per frame at build time (see
    // CellInfo.unlocked) - cores always count as unlocked, non-cores are
    // managed manually via the right-click menu. We used to call
    // IsEmoteUnlocked here, which linear-scans g_Emotes (FindEmote) per cell;
    // reading the cached flag keeps a grid of N cells from costing O(N^2).
    bool unlocked    = ci.unlocked;
    bool isLocked    = !e.IsCore && !unlocked;
    // Quickbar-only "can't use right now" state - a game-state block (mounted, or
    // with the RealTime API swimming/downed/etc.) OR, when the user opted into
    // "grey while typing or moving", the addon's transient send refusal. Dims +
    // blocks the cell like locked, but for a transient reason. g_QbUnusableKey is
    // the single reason key set once per frame in QuickbarRender (nullptr=usable).
    bool blocked     = isQuickbar && g_QbUnusableKey != nullptr;
    float alphaMul   = blocked ? 0.40f : (isLocked ? 0.45f : 1.f);
    bool dimmed      = isLocked || blocked;

    // Targetable-dot size, computed once so the label-width reservation and
    // the draw below agree. Sized from a per-mode reference picked so the dot
    // looks the same across modes (~11 px at 1x): the icon for icon-bearing
    // modes, the (short) button height for text-only, which has no icon. This
    // fixes the old inconsistency where the dot was derived from min(w,h) of
    // the item rect - tiny on the wide/short text button, large on the square
    // icon button. Pure proportional (no fixed floor) so it tracks the scale
    // slider; the user can turn the dot off entirely (ShowTargetDot).
    const bool showDot = e.IsTargetable && g_Settings.ShowTargetDot;
    float dotSz = 0.f;
    if (showDot) {
        float dotRef = (mode == EViewMode::TextOnly) ? cellH : iconSz;
        dotSz = dotRef * (mode == EViewMode::TextOnly ? 0.45f : 0.24f);
    }

    // PushID needs to be stable & unique within the section. Use the Id.
    ImGui::PushID(e.Id.c_str());
    if (dimmed) ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                     ImGui::GetStyle().Alpha * alphaMul);

    bool clicked = false;

    if (mode == EViewMode::TextOnly) {
        // Reserve space for the target dot so it doesn't overlap the centered
        // label (dot width + its corner gap + a little breathing room).
        float labelMaxW = cellW - 12.f;
        if (showDot) labelMaxW -= dotSz + 6.f;
        const auto& cached = TextCache::EllipsizeCached(e.Id, e.Name, mode, labelMaxW);

        ImGui::SetCursorPos(ImVec2(cellX, cellY));
        // Quickbar's high-contrast option keeps text-only buttons
        // visible when the QB background is hidden, AND keeps the
        // hover state visibly distinct. Mirrors the chrome push
        // around the category bar in Quickbar.cpp: resting state gets
        // the theme colour darkened (dim but solid); hover / active
        // keep the theme colour unchanged at full alpha (bright pop).
        // The hover effect is the RGB jump, not the alpha jump - an
        // alpha-only delta over a translucent game world barely
        // registers visually. See Quickbar.cpp for the longer note.
        // Main-panel cells aren't affected - their window background
        // is always opaque, so the theme defaults work fine there.
        int hiContrastPushes = 0;
        if (isQuickbar && g_Settings.QuickbarHighContrast) {
            hiContrastPushes = PushHighContrastButtonStyles(/*includeFrameBg=*/false);
        }
        clicked = ImGui::Button(cached.label.c_str(), ImVec2(cellW, cellH));
        if (hiContrastPushes > 0) ImGui::PopStyleColor(hiContrastPushes);
    } else {
        const int pad = 2;
        float btnTotal = iconSz + pad * 2;
        float btnX     = cellX + (cellW - btnTotal) * 0.5f;
        ImGui::SetCursorPos(ImVec2(btnX, cellY));

        Texture* tex = EnsureEmoteTexture(e);  // lazy: loads on first show
        if (tex && tex->Resource) {
            clicked = ImGui::ImageButton((ImTextureID)tex->Resource,
                ImVec2(iconSz, iconSz), ImVec2(0, 0), ImVec2(1, 1), pad);
        } else {
            clicked = RenderStyledFallback("##fb", e.Name.c_str(), btnTotal, alphaMul);
        }

        // Compact mode: lay a small alpha strip across the bottom of the
        // icon button and ellipsize the emote's name inside it. Anchored
        // to the button's screen rect (GetItemRectMin/Max) so it stays in
        // sync even when ImGui's frame padding shifts. The strip uses a
        // dark fill with high alpha so light icons stay readable beneath
        // the text without us needing per-icon contrast picking.
        if (mode == EViewMode::Compact) {
            ImVec2 itemMin = ImGui::GetItemRectMin();
            ImVec2 itemMax = ImGui::GetItemRectMax();
            float  fontH   = ImGui::GetFontSize();
            // Vertical padding above + below the glyph row. Kept small so
            // the strip leaves most of the icon visible.
            float  stripPadY = 2.f;
            float  stripH    = fontH + stripPadY * 2.f;
            ImVec2 stripMin(itemMin.x, itemMax.y - stripH);
            ImVec2 stripMax(itemMax.x, itemMax.y);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            // Background: dark, ~70% alpha. High enough that white text on
            // top stays legible over icons of any palette; low enough that
            // the bottom of the icon shows through and the cell still
            // reads as "an icon with a label," not "an icon and a chip."
            int bgA = (int)(180.f * alphaMul);
            dl->AddRectFilled(stripMin, stripMax,
                              IM_COL32(0, 0, 0, bgA));

            // Inner horizontal padding so glyphs don't kiss the edges
            // of the strip; doubles as the ellipsize budget.
            float stripPadX = 3.f;
            float maxTextW  = (stripMax.x - stripMin.x) - stripPadX * 2.f;
            // The targetable dot lives in the top-right corner of the icon,
            // not in the strip, so we don't reserve space for it here.
            // Cached ellipsize: same string + measured size as Ellipsize +
            // CalcTextSize, but memoized by (id, mode, maxW) — see TextCache.
            const auto& cached = TextCache::EllipsizeCached(e.Id, e.Name, mode, maxTextW);
            float  tx = stripMin.x + (stripMax.x - stripMin.x - cached.size.x) * 0.5f;
            float  ty = stripMin.y + stripPadY;
            // Slightly off-white so it doesn't scream against the dark
            // strip; alpha follows the cell so locked emotes dim cleanly.
            int textA = (int)(245.f * alphaMul);
            dl->AddText(ImVec2(tx, ty),
                        IM_COL32(245, 245, 245, textA),
                        cached.label.c_str());
        }
    }

    // Target indicator (all view modes — anchored to the last item, the
    // button). Size computed above so it stays consistent across modes.
    if (showDot) DrawTargetableDot(dotSz, alphaMul);

    // Lock overlay for locked emotes (drawn after dot, on top).
    if (isLocked) DrawLockOverlay();

    // Quickbar click-through: remember this button's screen rect so the next
    // frame can decide whether the QB window should claim mouse input.
    if (isQuickbar) {
        g_QbIconRects.emplace_back(ImGui::GetItemRectMin(),
                                   ImGui::GetItemRectMax());
    }

    // Drag source: enabled in user-favorites cells (for reorder /
    // cross-category move) AND in the main panel's built-in Core /
    // Unlockable sections (for "add to favorites by dragging"). The
    // Quickbar's cells never act as sources - drag-drop is a main-
    // panel interaction. Locked emotes in built-in sections can't
    // start a drag because they can't be favorited anyway; the
    // right-click menu already gates them the same way, so this
    // matches user expectation.
    //
    // Drop target: stays user-favorites-only. Dragging an emote back
    // onto Core / Unlockable does nothing - the built-in catalog
    // isn't a list we mutate. Removing from favorites is the
    // right-click "Remove from favorites" path, on purpose; dropping
    // would make "drag emote slightly to reorder" silently delete it
    // if the user undershot and landed in the wrong section.
    bool dragSourceEnabled = (categoryIdx >= 0)
                             ? allowReorder
                             : (!isQuickbar && !isLocked);
    if (dragSourceEnabled) {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            EmoteDragPayload payload {};
            payload.categoryIdx = categoryIdx;
            payload.emoteIdx    = ci.favIdx;
            payload.isLocked    = isLocked;
            strncpy_s(payload.id, sizeof(payload.id),
                      e.Id.c_str(), _TRUNCATE);
            ImGui::SetDragDropPayload("FAV_DRAG", &payload, sizeof(payload));
            ImGui::Text(L("cells.moving"), e.Name.c_str());
            // When the cursor is over a section that refuses the drop (the
            // built-in Core / Unlockable lists), explain why on the same drag
            // tooltip rather than letting the drop silently do nothing. The
            // message is source-aware: a favorite dragged out of a category is
            // almost always a "get rid of this" gesture, so point at the real
            // removal path (right-click) instead of telling them to drop on a
            // favorites category they're trying to leave. A catalog-sourced
            // drag really is just mis-aimed, so keep the "drag onto a category"
            // hint. (StampDragReject only fires once the cursor has left the
            // source section - see the built-in drop target below - so this
            // never flashes on grab.)
            if (DragRejectFresh()) {
                const char* why = (categoryIdx >= 0) ? L("cells.no_drop_remove")
                                                     : L("cells.no_drop_here");
                ImGui::TextColored(ImVec4(1.f, 0.45f, 0.40f, 1.f), "%s", why);
            }
            ImGui::EndDragDropSource();
        }
    }
    // NOTE: the drop *target* used to live here, per-cell. It now lives in
    // RenderEmoteSection as a single grid-wide drop zone so we can show an
    // insertion line *between* cells (rather than highlighting a cell) and
    // compute the gap once. Cells remain drag *sources* only. See the
    // "Drop zone" block in RenderEmoteSection.

    // Right-click context menu — favoriting works from ANY section.
    // Temporarily restore full alpha so the popup contents don't inherit the
    // dimmed style applied to the cell (locked / non-targetable emotes).
    if (dimmed) ImGui::PopStyleVar();
    if (ImGui::BeginPopupContextItem("##ctx")) {
        if (isQuickbar) {
            // Quickbar context menu is execute-variants only — favorites and
            // lock state are managed from the main panel. Plain "Send" is
            // omitted because that's the left-click default. Target-bound
            // items are disabled when the emote isn't targetable. Locked
            // emotes can still be left-click-sent (the game just won't run
            // the animation), but the modifier variants are hidden because
            // none of them work either — same pattern as locked-can't-favorite.
            if (isLocked) {
                ImGui::TextDisabled("%s", L("cells.locked_no_modifiers"));
            } else if (blocked) {
                // Unusable right now (mounted/swimming/... or typing/moving): the
                // send variants are pointless, so show the reason instead - the
                // same "why" hint mounted cells give, now for every blocked case.
                ImGui::TextDisabled("%s", L(g_QbUnusableKey));
            } else {
                RenderSendVariants(e);
            }
        } else {
            int currentCat = FindCategoryContaining(e.Id);
            int catCount   = (int)g_Settings.FavoriteCategories.size();

            // Lock/Unlock toggle for non-core emotes. Managed manually
            // because the GW2 API can't reliably report unlock state.
            // MarkEmoteLocked also auto-evicts the emote from favorites.
            if (!e.IsCore) {
                if (unlocked) {
                    if (ImGui::MenuItem(L("cells.mark_locked"))) MarkEmoteLocked(e.Id);
                } else {
                    if (ImGui::MenuItem(L("cells.mark_unlocked"))) MarkEmoteUnlocked(e.Id);
                }
                // Wiki link to the emote's unlock (tome / unlock-item) page. Clipboard
                // only - deliberately no ShellExecute, which can minimise an
                // exclusive-fullscreen client (same rule as the update link).
                std::string wikiUrl = WikiUrlForEmote(e.Id);
                if (!wikiUrl.empty() && ImGui::MenuItem(L("cells.copy_wiki"))) {
                    ImGui::SetClipboardText(wikiUrl.c_str());
                    ShowFeedback(L("cells.wiki_copied"));
                }
                ImGui::Separator();
            }

            if (catCount == 0) {
                ImGui::TextDisabled("%s", L("cells.no_categories"));
                ImGui::Separator();
                if (isLocked) {
                    ImGui::TextDisabled("%s", L("cells.locked_cant_fav"));
                } else if (ImGui::MenuItem(L("cells.create_and_add"))) {
                    EnsureDefaultCategory();
                    AddEmoteToCategory(0, e.Id, /*isLockedSource=*/false);
                }
            } else if (currentCat < 0) {
                if (isLocked) {
                    ImGui::TextDisabled("%s", L("cells.locked_cant_fav"));
                } else if (catCount == 1) {
                    char lbl[96];
                    std::snprintf(lbl, sizeof(lbl), L("cells.add_to"),
                                  g_Settings.FavoriteCategories[0].Name.c_str());
                    if (ImGui::MenuItem(lbl))
                        AddEmoteToCategory(0, e.Id, isLocked);
                } else if (catCount > 1) {
                    if (ImGui::BeginMenu(L("cells.add_to_category"))) {
                        for (int i = 0; i < catCount; ++i) {
                            if (ImGui::MenuItem(g_Settings.FavoriteCategories[i].Name.c_str()))
                                AddEmoteToCategory(i, e.Id, isLocked);
                        }
                        ImGui::EndMenu();
                    }
                }
            } else {
                if (catCount > 1 && !isLocked) {
                    if (ImGui::BeginMenu(L("cells.move_to_category"))) {
                        for (int i = 0; i < catCount; ++i) {
                            if (i == currentCat) continue;
                            if (ImGui::MenuItem(g_Settings.FavoriteCategories[i].Name.c_str()))
                                AddEmoteToCategory(i, e.Id, isLocked);
                        }
                        ImGui::EndMenu();
                    }
                }
                if (ImGui::MenuItem(L("cells.remove_from_fav")))
                    RemoveEmoteFromCategories(e.Id);
            }

            // Execute variants at the bottom — same set as the Quickbar menu
            // so muscle memory transfers. Plain "Send" is omitted because
            // that's the left-click default. Locked emotes get an explainer
            // here too — same pattern as locked-can't-favorite above.
            ImGui::Separator();
            if (isLocked) {
                ImGui::TextDisabled("%s", L("cells.locked_no_modifiers"));
            } else {
                RenderSendVariants(e);
            }
        }
        ImGui::EndPopup();
    }

    // Tooltip — also rendered with full alpha (still outside the dim push).
    // Generic "right-click for options" line works for both unlocked emotes
    // (favorites / modifiers / lock toggle) and locked ones (only lock
    // toggle is offered) — no need to vary copy by state.
    // The Quickbar has its own opt-out for tooltips since experienced users
    // know their icons by heart and a tooltip would clutter a HUD.
    bool suppressTip = isQuickbar && !g_Settings.ShowQuickbarTooltips;
    if (!suppressTip &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        const char* lockTag = isLocked ? L("cells.locked_tag") : "";
        // Built up line-by-line so optional rows (search-match note, auto-mote
        // triggers) slot in cleanly between the command and the right-click hint.
        std::string tip = e.Command;
        tip += lockTag;
        // ci.searchNote (main-panel search only) explains a non-obvious match -
        // e.g. an alias hit, which shows nowhere else.
        if (!ci.searchNote.empty()) { tip += '\n'; tip += ci.searchNote; }
        // Auto-mote chat triggers, when set — surfaces the wiring where you see
        // the emote (neutral wording: describes the config, not a promise it's on).
        if (!e.AutoKeywords.empty()) {
            std::string words;
            for (size_t i = 0; i < e.AutoKeywords.size(); ++i) {
                if (i) words += ", ";
                words += e.AutoKeywords[i];
            }
            tip += '\n'; tip += L("cells.auto_triggers"); tip += ' '; tip += words;
        }
        tip += '\n'; tip += L("cells.rightclick");
        TooltipTextRaw(tip.c_str());
    }
    // Re-apply the dim alpha for the label drawn below the button (Full mode)
    // and balance the PopStyleVar at the end of the function.
    if (dimmed) ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                     ImGui::GetStyle().Alpha * alphaMul);

    // Label below the button in Full mode (wraps to 2 lines or ellipsizes).
    if (mode == EViewMode::Full) {
        const int pad = 2;
        float btnTotal = iconSz + pad * 2;
        float labelY   = cellY + btnTotal + 4.f;

        // Cached fit: same two-line / ellipsized result as FitName, but with
        // both line sizes pre-measured so the centering math skips a second
        // CalcTextSize per line — FitName's split test already measured them
        // and the original code threw the measurements away.
        const auto& cached = TextCache::FitNameCached(e.Id, e.Name, cellW);
        ImGui::SetCursorPos(ImVec2(cellX + (cellW - cached.size1.x) * 0.5f, labelY));
        ImGui::TextUnformatted(cached.line1.c_str());

        if (!cached.line2.empty()) {
            float ly2 = labelY + cached.size1.y;
            ImGui::SetCursorPos(ImVec2(cellX + (cellW - cached.size2.x) * 0.5f, ly2));
            ImGui::TextUnformatted(cached.line2.c_str());
        }
    }

    if (dimmed) ImGui::PopStyleVar();
    if (clicked) {
        if (blocked) {
            // Unusable right now (mounted/swimming/... or - opt-in - typing/
            // moving). Don't silently swallow the click - show the in-window reason
            // line naming why nothing happened, same key as the right-click hint.
            // blocked is Quickbar-only; the main panel's equivalent refusal comes
            // from the send gate (EmoteAction, also via ShowFeedback).
            ShowFeedback(L(g_QbUnusableKey));
        } else {
            // Plain send, unless the user opted into auto-targeting and this
            // emote is targetable - then left-click sends on the current target
            // (@). Other modifier variants (*, @ *) live in the right-click menu.
            bool autoTarget = e.IsTargetable && g_Settings.SendTargetableOnTarget;
            SendOrFillEmote(e, autoTarget, false);
        }
    }
    ImGui::PopID();

    (void)sectionRow; // unused but kept for clarity at call site
}

// Small constant slack so ImGui's integer window rounding (at fractional icon
// scales) can't leave childAvail a sub-pixel under N*step and make the floor
// below drop a column/row. 1px is invisible (cells carry internal padding) and
// never rounds N up to N+1 since 1/step < 1.
static const float kGridFitEps = 1.0f;

GridFit ComputeQuickbarGridFit(int itemCount, ImVec2 avail,
                                ImVec2 cellSize, ImVec2 spacing,
                                bool horizontal) {
    const float stepX = cellSize.x + spacing.x;
    const float stepY = cellSize.y + spacing.y;
    GridFit g;
    g.visCols = std::max(1, (int)((avail.x + spacing.x + kGridFitEps) / stepX));
    g.visRows = std::max(1, (int)((avail.y + spacing.y + kGridFitEps) / stepY));

    // Pages mode: round the scroll-axis dimension UP to a whole-page multiple,
    // so the last partial page gets its own dedicated viewport (with empty
    // cells filling out the page) instead of overlapping with the previous
    // page's content. Empty padded cells are never rendered - the cell loop
    // in RenderEmoteSection clamps jEnd to items.size(); the drop zone covers
    // them as an "append at end" target; the end Dummy extends to the padded
    // ContentSize so ImGui's scrollMax matches g_QbMaxScroll*. Bonus: with
    // pad on, maxScroll is an exact multiple of pageStep, so page boundaries
    // (incl. the end) all land cleanly. Off / Cells are unchanged.
    const bool padToPage = (g_Settings.QuickbarSnapScroll == EQbScrollSnap::Pages);

    if (horizontal) {
        // Column-major: rows are the constraint (capacity vertically),
        // cells flow rightward into additional columns when items > rows.
        g.rows = g.visRows;
        g.cols = std::max(1, (itemCount + g.rows - 1) / g.rows);
        if (padToPage)
            g.cols = ((g.cols + g.visCols - 1) / g.visCols) * g.visCols;
        g.overflowsX = g.cols > g.visCols;
        g.maxScrollX = std::max(0, g.cols - g.visCols) * stepX;
    } else {
        // Row-major: cols are the constraint (capacity horizontally),
        // cells flow downward into additional rows when items > cols.
        g.cols = g.visCols;
        g.rows = std::max(1, (itemCount + g.cols - 1) / g.cols);
        if (padToPage)
            g.rows = ((g.rows + g.visRows - 1) / g.visRows) * g.visRows;
        g.overflowsY = g.rows > g.visRows;
        g.maxScrollY = std::max(0, g.rows - g.visRows) * stepY;
    }
    return g;
}

float EmoteGridTopPad(EViewMode mode) {
    // Breathing room above the grid - icons in Full mode hugged the header /
    // window top. Compact/Icon get a hair; Text-only stays flush.
    switch (mode) {
        case EViewMode::Full:     return 2.f;
        case EViewMode::Icon:     return 1.f;
        case EViewMode::Compact:  return 1.f;
        case EViewMode::TextOnly: return 0.f;
    }
    return 0.f;
}

void EmoteCellSize(EViewMode mode, float scale,
                   float& cellW, float& cellH, float& iconSz) {
    switch (mode) {
        case EViewMode::TextOnly: iconSz =  0.f; cellW = 96.f; cellH = 24.f; break;
        case EViewMode::Icon:     iconSz = 48.f; cellW = 56.f; cellH = 56.f; break;
        // Compact shares Icon's footprint — the label is overlaid on the
        // icon rather than added below it, so we don't grow the cell.
        case EViewMode::Compact:  iconSz = 48.f; cellW = 56.f; cellH = 56.f; break;
        default: /* Full */       iconSz = 48.f; cellW = 80.f; cellH = 94.f; break;
    }
    if (scale > 0.f && scale != 1.f) {
        cellW  *= scale;
        cellH  *= scale;
        iconSz *= scale;
    }
    // Full mode draws a two-line label; ensure cellH reserves space for it
    // at the current font size so labels never overflow into the next row.
    if (mode == EViewMode::Full) {
        const int   pad        = 2;
        float       fontH      = ImGui::GetFontSize();
        float       btnTotal   = iconSz + pad * 2;
        float       gap        = 4.f;
        float       labelAreaH = fontH * 2.f + 4.f;
        float       neededH    = btnTotal + gap + labelAreaH;
        if (neededH > cellH) cellH = neededH;
    }
}

// Grid-wide drop zone: one invisible button spanning the whole grid, submitted
// UNDER the cells. SetItemAllowOverlap lets the cells (drawn afterwards, on top)
// keep hover / click / drag-source behaviour, while this single grid-wide target
// catches the drop anywhere - including the dead space between cells, where per-cell
// targets would make the insertion line flicker out. The gap math lives in one place
// (ComputeDropHit), and AcceptNoDrawDefaultRect suppresses ImGui's default
// cell-rectangle highlight so we draw our own between-cells line instead. Spans the
// FULL box (g.gridW()/g.gridH(), incl. a partial last row's empty tail) so the open
// space past the last cell is a valid append drop. Reports the hover preview back via
// outPreview / outShowPreview (drawn after the cells by DrawInsertionPreview).
static void HandleGridDropZone(const std::vector<CellInfo>& items, const GridLayout& g,
                               bool allowReorder, int categoryIdx, bool isQuickbar,
                               DropHit& outPreview, bool& outShowPreview) {
    if (allowReorder && categoryIdx >= 0) {
        ImGui::PushID(categoryIdx);
        ImGui::SetCursorPos(ImVec2(g.baseX, g.baseY));
        ImGui::InvisibleButton("##emote_dropzone", ImVec2(g.gridW(), g.gridH()));
        ImGui::SetItemAllowOverlap();
        if (ImGui::BeginDragDropTarget()) {
            // PeekOnly = AcceptBeforeDelivery | AcceptNoDrawDefaultRect:
            // returns the payload while merely hovering (so we can preview the
            // line every frame) and suppresses ImGui's default cell highlight.
            const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(
                "FAV_DRAG", ImGuiDragDropFlags_AcceptPeekOnly);
            if (pl) {
                EmoteDragPayload src = *(const EmoteDragPayload*)pl->Data;
                DropHit hit = ComputeDropHit(items, g);
                // No-op self-drops draw nothing and do nothing.
                if (!IsNoopSelfMove(src, categoryIdx, hit.gap)) {
                    outPreview = hit; outShowPreview = true;  // hover preview
                    if (pl->IsDelivery()) ApplyEmoteDrop(src, categoryIdx, hit.gap);
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    } else if (categoryIdx < 0 &&
               (!isQuickbar || ImGui::GetDragDropPayload() != nullptr)) {
        // Built-in catalog section (Core / Unlockable). Favoriting by drag only
        // flows INTO user categories, so this section never accepts a drop -
        // but with no target at all, dragging an emote over it gave zero
        // feedback and read as a broken drag. Lay a passive peek-only target so
        // the rejection is visible: a red outline around the section here, plus
        // (via StampDragReject) an explanatory line on the drag tooltip. We
        // never accept delivery, so dropping still does nothing.
        //
        // categoryIdx is -1 for ALL built-in sections (Core / Unlockable /
        // /me-motes), so key the ImGui id off the first cell's address (e or m,
        // distinct per section, stable per frame) to avoid an id collision
        // between their invisible buttons. The /me-mote section has e == null,
        // so fall back to m — never PushID(nullptr). items is non-empty here -
        // the function returns early when it is.
        //
        // Quickbar gate (the `!isQuickbar || GetDragDropPayload()` above): this
        // grid-spanning button sets HoveredId over the whole body, which blocks
        // ImGui from starting a window-move from the empty body ("Allow move").
        // The QB has no drag sources and never accepts a drop, so it only needs
        // this reject target WHILE a drag is in flight - when idle we skip it so
        // the QB body stays plain window void (movable / click-through). The
        // main panel (!isQuickbar) submits it every frame as before.
        ImGui::PushID(items[0].e ? (const void*)items[0].e
                                 : (const void*)items[0].m);
        ImGui::SetCursorPos(ImVec2(g.baseX, g.baseY));
        ImGui::InvisibleButton("##builtin_noreorder", ImVec2(g.gridW(), g.gridH()));
        ImGui::SetItemAllowOverlap();
        if (ImGui::BeginDragDropTarget()) {
            const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(
                "FAV_DRAG", ImGuiDragDropFlags_AcceptPeekOnly);
            if (pl) {
                // Suppress the rejection while the cursor is still over the
                // section the drag STARTED in. Catalog-sourced drags begin with
                // the cursor over this very section, so stamping on frame 1 read
                // as an instant "can't drop here" error before the user moved.
                // The source section is the one still showing the dragged cell
                // (a drag doesn't remove it until drop), so detect it by
                // (type, Id) membership. Type matters: an Emote and a /me-mote
                // can share an Id, so an Id-only test would mis-identify the
                // source — suppressing the reject on a built-in section that
                // should refuse, or (for a /me-mote section, where it.e is null)
                // failing to recognize the true source so it instantly rejects.
                // Once the cursor leaves onto a different no-drop section, the
                // reject (tooltip + outline) appears as before. Each cell is
                // exactly one kind (it.e XOR it.m).
                const EmoteDragPayload* src = (const EmoteDragPayload*)pl->Data;
                bool isSourceSection = false;
                for (const auto& it : items) {
                    const std::string* itId = it.e ? &it.e->Id
                                                   : (it.m ? &it.m->Id : nullptr);
                    EFavoriteRefType itType = it.e ? EFavoriteRefType::Emote
                                                   : EFavoriteRefType::MeMote;
                    if (itId && itType == src->type && *itId == src->id) {
                        isSourceSection = true;
                        break;
                    }
                }
                if (!isSourceSection) {
                    // Anchor the refusal on the whole section (screen space:
                    // window pos minus scroll, matching the insertion-line math),
                    // not just at the cursor. Same feedback as a collapsed
                    // built-in header via the shared helper.
                    ImVec2 winPos = ImGui::GetWindowPos();
                    float  sx = ImGui::GetScrollX(), sy = ImGui::GetScrollY();
                    ImVec2 a(winPos.x - sx + g.baseX, winPos.y - sy + g.baseY);
                    ImVec2 b(a.x + g.gridW(), a.y + g.gridH());
                    StampDropReject(a, b);
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }
}

// Insertion-gap preview line. Drawn after the cells (so it sits on top) and on the
// window draw list (so it clips to the section like the cells do). We draw relative
// to the anchor cell + side the hit-test picked, so the bar follows the mouse's row.
// Two margin fixes vs. deriving position from the gap index alone:
//   - at a row start (left of the first cell in a row) the bar would sit half-a-
//     spacing into the left padding and clip; inset it to hug the cell's left edge.
//   - at a row end (right of the last cell before a wrap) the gap index points at the
//     NEXT row's first cell, so deriving from the gap drew the bar at the next row's
//     start. Anchoring on the picked cell keeps it on the right edge where the mouse is.
static void DrawInsertionPreview(const GridLayout& g, const DropHit& hit) {
    ImVec2 winPos = ImGui::GetWindowPos();
    float  sx = ImGui::GetScrollX(), sy = ImGui::GetScrollY();
    int    col, row;
    g.cellColRow(hit.anchorCell, col, row);
    float cellL = winPos.x - sx + g.baseX + col * g.stepX;
    float cellR = cellL + g.cellW;
    float top   = winPos.y - sy + g.baseY + row * g.stepY;
    float bot   = top + g.cellH;

    float lineX;
    if (hit.anchorRight) {
        // Draw in the gap to the right of the cell, except when the cell is in the
        // rightmost column (content edge) - there +spacing/2 would clip, so hug its
        // right edge instead. A partial last row's final cell is NOT in the rightmost
        // column, so its bar sits in the open space to the right, where the drop lands.
        bool atContentRight = (col == g.cols - 1);
        lineX = atContentRight ? (cellR - 1.5f) : (cellR + g.spacingX * 0.5f);
    } else {
        bool atRowStart = (col == 0);
        lineX = atRowStart ? (cellL + 1.5f) : (cellL - g.spacingX * 0.5f);
    }
    DrawDropInsertionLine(lineX, top, bot);
}

void RenderEmoteSection(const std::vector<CellInfo>& items, bool allowReorder,
                        int categoryIdx, EViewMode mode, float scale,
                        bool isQuickbar, bool horizontal) {
    if (items.empty()) return;

    float topPad = EmoteGridTopPad(mode);
    if (topPad > 0.f) ImGui::Dummy(ImVec2(0.f, topPad));

    float cellW, cellH, iconSz;
    EmoteCellSize(mode, scale, cellW, cellH, iconSz);

    float spacingX = ImGui::GetStyle().ItemSpacing.x;
    float spacingY = ImGui::GetStyle().ItemSpacing.y;
    ImVec2 avail   = ImGui::GetContentRegionAvail();
    int   cols, rows;
    if (horizontal) {
        // Column-major: rows fit available height; columns extend rightward
        // until all items are placed → horizontal scroll picks up the rest.
        //
        // Tricky bit: if content will overflow X *and* the user has the X
        // scrollbar visible, ImGui will draw it at the bottom on the *next*
        // frame, which shrinks GetContentRegionAvail().y under our feet.
        // Without anticipating that, the row count we computed against the
        // pre-scrollbar avail.y would suddenly be one too many, content
        // would spill past the bottom, and ImGui would add a vertical
        // scrollbar to compensate — the dreaded "both bars at once".
        //
        // Fix: peek at whether overflow is coming AND whether a bar will
        // actually be drawn, then reserve the bar's height exactly once.
        // Work from the bar-independent inner height (add the X scrollbar
        // back when it's currently shown, since avail.y already excludes it)
        // so we never double-subtract it - the mirror of the row-major
        // branch's fullX handling. Double-subtracting clipped the last row.
        // Owned-scroll mode (either SnapWindow or SnapScroll) reserves its own
        // gutter by shrinking the child, so skip the ImGui-scrollbar
        // reservation here - otherwise it double-counts and drops a row. Only
        // pure free mode (neither snap) anticipates ImGui's bar.
        bool         fit   = isQuickbar && (g_Settings.QuickbarSnapWindow ||
                                            g_Settings.QuickbarSnapScroll != EQbScrollSnap::Off);
        if (fit) {
            // Owned-scroll Quickbar: shared canonical math via the helper, so
            // Quickbar.cpp's pre-BeginChild overflow prediction (which calls the
            // same helper with the same avail) is guaranteed to agree with what
            // we lay out here. Without that agreement, the gutter reservation
            // decision could disagree with the actual cell layout and produce
            // the boundary flicker the old s_qbScrollable lag had.
            GridFit gf = ComputeQuickbarGridFit((int)items.size(), avail,
                                                 ImVec2(cellW, cellH),
                                                 ImVec2(spacingX, spacingY),
                                                 /*horizontal=*/true);
            cols = gf.cols; rows = gf.rows;
        } else {
            // Free mode: anticipate ImGui's bar so the row count stays stable
            // as the auto-bar toggles.
            float        sb    = ImGui::GetStyle().ScrollbarSize;
            ImGuiWindow* w     = ImGui::GetCurrentWindow();
            float        fullY = avail.y + (w && w->ScrollbarX ? sb : 0.f);
            int  rowsFull   = std::max(1, (int)((fullY + spacingY + kGridFitEps) / (cellH + spacingY)));
            bool willOverflowX  = (int)items.size() > rowsFull;
            bool willDrawXScrl  = willOverflowX &&
                g_Settings.QuickbarScrollIndicator == EQbScrollIndicator::Scrollbar;
            float effectiveY = willDrawXScrl ? std::max(cellH, fullY - sb) : fullY;
            rows = std::max(1, (int)((effectiveY + spacingY + kGridFitEps) / (cellH + spacingY)));
            cols = ((int)items.size() + rows - 1) / rows;
        }
    } else {
        // Row-major (default): cols fit available width; rows extend down.
        // For the Quickbar, anticipate the vertical scrollbar so the column
        // count stays stable as the bar toggles - otherwise the bar appears,
        // steals width, drops a column, grows the height, and stays (the
        // "scrollbar too early" feel when fitting the window). Work from the
        // bar-independent inner width (add the bar back when it's currently
        // shown) so its width is reserved exactly once, never twice. The main
        // panel always scrolls a long catalog, so its bar is effectively
        // permanent and avail.x is already stable - keep its plain fit.
        auto fitCols = [&](float width) {
            return std::max(1, (int)((width + spacingX + kGridFitEps) / (cellW + spacingX)));
        };
        const bool qbOwnScroll = g_Settings.QuickbarSnapWindow ||
                                  g_Settings.QuickbarSnapScroll != EQbScrollSnap::Off;
        if (isQuickbar && qbOwnScroll) {
            // Owned-scroll Quickbar: shared canonical math via the helper, so
            // Quickbar.cpp's pre-BeginChild overflow prediction (same helper,
            // same avail) is guaranteed to agree with this layout. Without
            // that agreement, the boundary flicker the old s_qbScrollable lag
            // produced returns.
            GridFit gf = ComputeQuickbarGridFit((int)items.size(), avail,
                                                 ImVec2(cellW, cellH),
                                                 ImVec2(spacingX, spacingY),
                                                 /*horizontal=*/false);
            cols = gf.cols; rows = gf.rows;
        } else if (isQuickbar) {
            // Free-mode Quickbar: anticipate ImGui's vertical scrollbar so the
            // column count stays stable as the bar toggles. The main panel
            // always scrolls so its bar is effectively permanent; main-panel
            // path falls through to the simple fitCols(avail.x) below.
            float        sb    = ImGui::GetStyle().ScrollbarSize;
            ImGuiWindow* w     = ImGui::GetCurrentWindow();
            float        fullX = avail.x + (w && w->ScrollbarY ? sb : 0.f);
            int   colsFull  = fitCols(fullX);
            int   rowsFull  = ((int)items.size() + colsFull - 1) / colsFull;
            float gridHFull = rowsFull * (cellH + spacingY) - spacingY;
            bool  willY     = (gridHFull > avail.y) &&
                g_Settings.QuickbarScrollIndicator == EQbScrollIndicator::Scrollbar;
            cols = fitCols(willY ? std::max(cellW, fullX - sb) : fullX);
            rows = ((int)items.size() + cols - 1) / cols;
        } else {
            // Main panel: plain fit.
            cols = fitCols(avail.x);
            rows = ((int)items.size() + cols - 1) / cols;
        }
    }

    float baseX = ImGui::GetCursorPosX();
    float baseY = ImGui::GetCursorPosY();
    float stepX = cellW + spacingX;
    float stepY = cellH + spacingY;
    // Export the cell step for the Quickbar window drag-snap (read one frame
    // later in Quickbar.cpp). Only the QB pass; the main panel doesn't snap.
    if (isQuickbar) {
        g_QbStepX = stepX; g_QbStepY = stepY;
        g_QbCols = cols; g_QbRows = rows;
        // Overflow + cell-aligned max scroll come from the same helper that
        // Quickbar.cpp uses for its pre-BeginChild prediction (and that the
        // ownScroll branches above use to choose cols/rows). One canonical
        // computation, no chance of divergence. Immune to ImGui's sub-pixel
        // ContentSize rounding by construction.
        GridFit gf = ComputeQuickbarGridFit((int)items.size(), avail,
                                             ImVec2(cellW, cellH),
                                             ImVec2(spacingX, spacingY),
                                             horizontal);
        if (horizontal) {
            g_QbOverflow   = gf.overflowsX;
            g_QbMaxScrollX = gf.maxScrollX;
            g_QbMaxScrollY = 0.f;
        } else {
            g_QbOverflow   = gf.overflowsY;
            g_QbMaxScrollY = gf.maxScrollY;
            g_QbMaxScrollX = 0.f;
        }
    }

    // Bundle the geometry the grid helpers need (fill order, cell pitch, origin,
    // grid box) into one value object so they share a single source instead of a
    // fistful of loose locals. The fit math above that chose cols/rows is untouched.
    GridLayout g{ baseX, baseY, stepX, stepY, cellW, cellH,
                  spacingX, spacingY, cols, rows, horizontal };

    // Grid-wide drop zone: reorder target on favorites, "can't drop here" feedback
    // on the built-in sections. Reports the hover preview back for drawing below.
    DropHit preview{}; bool showPreview = false;
    HandleGridDropZone(items, g, allowReorder, categoryIdx, isQuickbar,
                       preview, showPreview);

    // Visible-range cull. RenderEmoteCell is the per-frame render hot path
    // (texture lookup, label ellipsize/shaping, draw-list strip in Compact/Full,
    // a full Button/ImageButton). ImGui clips the *output* of off-screen cells
    // but still pays their *build* cost, so for a long scrolled catalog most of
    // mp.draw/qb.draw is wasted on cells nobody can see. Cells are uniform-pitch
    // (stepX/stepY) and absolutely positioned, so the scroll viewport maps to a
    // CONTIGUOUS index range under either fill order (GridLayout::cellColRow: a
    // row holds [r*cols,(r+1)*cols); a column holds [c*rows,(c+1)*rows)). We skip
    // everything outside it. This is leaner than ImGuiListClipper here: the grid
    // is absolutely positioned (clipper assumes cursor-advance flow) and the
    // horizontal Quickbar is column-major (clipper is row-only). Safe because the
    // full-extent end Dummy below still sets ContentSize (scrollbar range
    // unchanged), and the drop zone + insertion preview are grid-math, not these
    // cells (reorder unaffected). When nothing overflows the range resolves to
    // all cells (a no-op). +/-1 band of overscan keeps a partial edge row/col
    // drawn so nothing pops in mid-scroll. Stacked main-panel sections each have
    // their own baseY, so a section wholly above/below the fold yields an empty
    // range and is skipped entirely.
    int jStart = 0, jEnd = (int)items.size();
    {
        const ImGuiWindow* win  = ImGui::GetCurrentWindow();
        const ImRect&      clip = win->InnerClipRect;   // visible content, screen space
        constexpr int      kOverscan = 1;
        if (horizontal) {                                // column-major: cull X band
            const float viewL = clip.Min.x - win->Pos.x + win->Scroll.x;
            const float viewR = clip.Max.x - win->Pos.x + win->Scroll.x;
            int firstCol = (int)std::floor((viewL - baseX) / stepX) - kOverscan;
            int lastCol  = (int)std::floor((viewR - baseX) / stepX) + kOverscan;
            firstCol = std::max(0, firstCol);
            lastCol  = std::min(cols - 1, lastCol);
            if (firstCol > lastCol) { jStart = jEnd = 0; }            // off-screen section
            else { jStart = firstCol * rows;
                   jEnd   = std::min((int)items.size(), (lastCol + 1) * rows); }
        } else {                                         // row-major: cull Y band
            const float viewT = clip.Min.y - win->Pos.y + win->Scroll.y;
            const float viewB = clip.Max.y - win->Pos.y + win->Scroll.y;
            int firstRow = (int)std::floor((viewT - baseY) / stepY) - kOverscan;
            int lastRow  = (int)std::floor((viewB - baseY) / stepY) + kOverscan;
            firstRow = std::max(0, firstRow);
            lastRow  = std::min(rows - 1, lastRow);
            if (firstRow > lastRow) { jStart = jEnd = 0; }            // off-screen section
            else { jStart = firstRow * cols;
                   jEnd   = std::min((int)items.size(), (lastRow + 1) * cols); }
        }
    }

    for (int j = jStart; j < jEnd; ++j) {
        int col, row;
        g.cellColRow(j, col, row);
        float cellX = baseX + col * stepX;
        float cellY = baseY + row * stepY;

        RenderEmoteCell(items[j], j, cellX, cellY, cellW, cellH, iconSz,
                        mode, allowReorder, categoryIdx, isQuickbar);
    }

    // Insertion-gap preview line, drawn after the cells so it sits on top of them.
    if (showPreview) DrawInsertionPreview(g, preview);

    // Don't trail a spacing-step after the last row/column — that extra strip
    // makes parent BeginChilds think content extends further than it does and
    // shows a scrollbar one gap earlier than necessary.
    int   usedCols = horizontal ? cols : std::min(cols, (int)items.size());
    int   usedRows = horizontal ? std::min(rows, (int)items.size()) : rows;
    float endX = (usedCols > 0) ? (baseX + usedCols * stepX - spacingX) : baseX;
    float endY = (usedRows > 0) ? (baseY + usedRows * stepY - spacingY) : baseY;
    // Position the dummy at the bottom-right so ImGui's auto-content-size
    // picks up the full extent in both axes; it matters in horizontal mode.
    ImGui::SetCursorPos(ImVec2(endX, endY));
    ImGui::Dummy(ImVec2(1, 1));
}

// Inline-rename state for the Library category headers. Only one header is ever
// renaming at a time, so a single index + buffer suffices (no per-id map). The
// index is fine because reorder can't happen mid-rename (drag needs the header).
static int  s_renamingCat      = -1;
static bool s_renameFocus      = false;
static char s_catRenameBuf[64] = {};

bool RenderCategoryHeader(int categoryIdx, const char* name, bool searchActive) {
    auto& cats = g_Settings.FavoriteCategories;
    if (categoryIdx < 0 || categoryIdx >= (int)cats.size()) {
        SectionHeader(name, /*isUser=*/true);  // defensive fallback
        return false;
    }
    ImGui::Spacing();
    ImGui::PushID(categoryIdx);

    // Star marking a user-created category (same look as the old header).
    const float iconBox = 16.f;
    ImVec2 iconPos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(iconBox, iconBox));
    ImVec2 iconCenter(iconPos.x + iconBox * 0.5f, iconPos.y + iconBox * 0.5f);
    DrawStarIcon(iconCenter, iconBox * 0.42f, IM_COL32(240, 205, 70, 255));
    ImGui::SameLine();

    // Stored flag vs the effective state: an active search forces every category
    // open so its matches show, without touching the persisted Collapsed.
    bool stored    = cats[categoryIdx].Collapsed;
    bool collapsed = stored && !searchActive;

    if (s_renamingCat == categoryIdx) {
        // Inline rename - same red-border + tooltip validation as every other
        // text input (empty or duplicate name both flag the box).
        if (s_renameFocus) { ImGui::SetKeyboardFocusHere(); s_renameFocus = false; }
        std::string trimmed = TrimWhitespace(s_catRenameBuf);
        bool empty   = trimmed.empty();
        bool dup     = !empty && CategoryNameExists(trimmed, categoryIdx);
        bool invalid = empty || dup;
        bool enter = false, active = false;
        {
            InputFieldOpts o; o.invalid = invalid;   // width 0 = fill
            o.flags = ImGuiInputTextFlags_EnterReturnsTrue;
            bool hov = false;
            enter  = InputFieldWithHint("##catrename", nullptr, s_catRenameBuf,
                                        sizeof(s_catRenameBuf), o, nullptr, &hov);
            active = ImGui::IsItemActive();   // last item = the InputText
            if (invalid && hov) {
                if (dup) {
                    char m[160]; std::snprintf(m, sizeof m, L("mp.cat_exists"), trimmed.c_str());
                    TooltipTextRaw(m);
                } else {
                    TooltipText("common.name_empty");
                }
            }
        }
        auto commit = [&]() {
            std::string oldName = cats[categoryIdx].Name;
            LOG_DEBUG("favorites: renamed category \"%s\" -> \"%s\"",
                      oldName.c_str(), trimmed.c_str());
            cats[categoryIdx].Name = trimmed;
            RequestSave(SaveKind::Settings);
            // Keep any exported RadialMenus wheels linked to the renamed category.
            RetargetExportsCategory(oldName, trimmed);
        };
        if (enter && !invalid) { commit(); s_renamingCat = -1; }
        else if (!active && ImGui::IsItemDeactivated()) {
            // Lost focus (clicked away / Escape): commit a valid change, else cancel.
            if (!invalid && trimmed != cats[categoryIdx].Name) commit();
            s_renamingCat = -1;
        }
    } else {
        // One interactive item for the whole header: click toggles collapse,
        // drag reorders, right-click renames/deletes. A drawn disclosure triangle
        // sits in the label's leading pad (kept inside the Selectable so it's part
        // of the click target); the leading spaces reserve room for it.
        std::string lbl = std::string("   ") + name + "###cathdr";
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.78f, 0.32f, 1.f));
        bool clicked = ImGui::Selectable(lbl.c_str());
        ImGui::PopStyleColor();
        {
            ImVec2 aMin = ImGui::GetItemRectMin(), aMax = ImGui::GetItemRectMax();
            float  ah   = ImGui::GetFontSize() * 0.26f;
            DrawCollapseArrow(ImVec2(aMin.x + ah + 2.f, (aMin.y + aMax.y) * 0.5f),
                              ah, collapsed, IM_COL32(235, 200, 90, 255));
        }

        // Drag source: reorder the whole category. Distinct payload type so it
        // can never be confused with the cell-level FAV_DRAG emote drag.
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("CAT_DRAG", &categoryIdx, sizeof(int));
            ImGui::Text(L("cells.moving"), name);
            ImGui::EndDragDropSource();
        }
        // Drop target: FAV_DRAG (emote), ONLY while collapsed - the body is hidden
        // so there's no grid to catch the drop, and the header receives it
        // (appends to this category) with a highlight cue. Expanded, the grid
        // handles emote drops. Category REORDER (CAT_DRAG) is NOT handled here: the
        // whole-section zone in MainPanel (CategoryReorderZone) is a much bigger,
        // easier target. The header keeps only the CAT_DRAG *source* above.
        if (collapsed && ImGui::BeginDragDropTarget()) {
            if (AcceptEmoteDropInto(categoryIdx, (int)cats[categoryIdx].Refs.size())) {
                // Highlight the whole header so it reads as "drops into here".
                ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(mn, mx, IM_COL32(110, 180, 255, 38), 3.f);
                dl->AddRect(mn, mx, IM_COL32(110, 180, 255, 200), 3.f, 0, 1.5f);
            }
            ImGui::EndDragDropTarget();
        }

        if (clicked) {
            cats[categoryIdx].Collapsed = !stored;       // toggle the stored flag
            stored    = cats[categoryIdx].Collapsed;
            collapsed = stored && !searchActive;         // effective for this frame
            // Collapse is navigation state — updated in memory only; it rides along
            // the next real settings write and the unload flush (see SaveScheduler.h).
        }
        if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            TooltipText("mp.cat_header_tooltip");

        if (ImGui::BeginPopupContextItem("##cathdr_ctx")) {
            if (ImGui::MenuItem(L("cells.rename_category"))) {
                s_renamingCat = categoryIdx;
                s_renameFocus = true;
                strncpy_s(s_catRenameBuf, sizeof(s_catRenameBuf), name, _TRUNCATE);
            }
            if (ImGui::MenuItem(L("cells.delete_category")))
                DeleteFavoriteCategory(categoryIdx);  // also fixes QuickbarCategoryIdx
            ImGui::EndPopup();
        }
    }

    ImGui::PopID();
    ImGui::Separator();
    // Return the live collapse state (a delete above leaves categoryIdx invalid,
    // so read the captured local, not cats[categoryIdx]).
    return collapsed;
}

bool SectionHeader(const char* label, bool isUser, bool* collapsed, bool searchActive) {
    ImGui::Spacing();

    // Reserve a small icon slot inline with the header text.
    const float iconBox = 16.f;
    ImVec2 iconPos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(iconBox, iconBox));
    ImGui::SameLine();

    // Star for user-created categories, paperclip for built-in sections.
    ImVec2 iconCenter(iconPos.x + iconBox * 0.5f, iconPos.y + iconBox * 0.5f);
    if (isUser)
        DrawStarIcon(iconCenter, iconBox * 0.42f, IM_COL32(240, 205, 70, 255));
    else
        DrawPaperclipIcon(iconCenter, iconBox * 0.45f, IM_COL32(180, 180, 180, 255));

    // Text colour: gold for user-created, muted for built-in.
    const ImVec4 textCol = isUser
        ? ImVec4(0.92f, 0.78f, 0.32f, 1.f)
        : ImVec4(0.72f, 0.72f, 0.72f, 1.f);

    bool effective = false;
    if (collapsed) {
        // Collapsible built-in section: a disclosure arrow + a clickable row that
        // toggles the caller's flag. An active search forces it open (effective),
        // without touching the stored flag. The label doubles as the item ID
        // (Core / Unlockable differ), so the two never collide.
        effective = *collapsed && !searchActive;
        std::string lbl = std::string("   ") + label;
        ImGui::PushStyleColor(ImGuiCol_Text, textCol);
        bool clicked = ImGui::Selectable(lbl.c_str());
        ImGui::PopStyleColor();
        {
            ImVec2 aMin = ImGui::GetItemRectMin(), aMax = ImGui::GetItemRectMax();
            float  ah   = ImGui::GetFontSize() * 0.26f;
            ImU32  acol = isUser ? IM_COL32(235, 200, 90, 255)
                                 : IM_COL32(185, 185, 185, 255);
            DrawCollapseArrow(ImVec2(aMin.x + ah + 2.f, (aMin.y + aMax.y) * 0.5f),
                              ah, effective, acol);
        }
        if (clicked) {
            *collapsed = !*collapsed;
            effective  = *collapsed && !searchActive;
            // Collapse is navigation state — updated in memory only; it rides along
            // the next real settings write and the unload flush (see SaveScheduler.h).
        }
        if (ImGui::IsItemHovered())
            TooltipText("mp.section_header_tooltip");
        // While collapsed (grid hidden), the header stands in for the section as a
        // drop target. Built-in sections never accept favorites, so reject an emote
        // drop here with the same red feedback the expanded grid gives. (You can't
        // drag FROM a collapsed section, so there's no frame-1 self-reject to guard.)
        if (effective && !isUser && ImGui::BeginDragDropTarget()) {
            const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(
                "FAV_DRAG", ImGuiDragDropFlags_AcceptPeekOnly);
            if (pl) StampDropReject(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::EndDragDropTarget();
        }
    } else {
        // Plain visual header (the defensive fallback path).
        ImGui::TextColored(textCol, "%s", label);
    }

    ImGui::Separator();
    return effective;
}
