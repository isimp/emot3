#pragma once

// Per-cell + grid-section rendering, shared by the main panel, the
// Quickbar, and (indirectly via the favorites lists) the section header
// utility used in both windows.

#include <string>
#include <vector>
#include "imgui/imgui.h"   // ImVec2 (helper API)
#include "Settings.h"      // EViewMode

struct Emote;
struct MeMote;

// One cell to draw. Exactly one of `e` (Emote) or `m` (/me-mote) is non-null;
// RenderEmoteCell dispatches on which is set. Keeping both pointers in the
// same struct (rather than forking the vector type) means RenderEmoteSection
// + its drop-zone + grid-fit code stay type-agnostic and unchanged — adding
// the second kind only touches the per-cell render path.
struct CellInfo {
    const Emote*  e = nullptr;
    const MeMote* m = nullptr;
    int           favIdx;          // index into FavoriteCategory.Refs when reorderable, else -1
    bool          unlocked = false; // precomputed at build time (e->IsCore ||
                                    // in ManuallyUnlocked) so the render loop and
                                    // the unlockables sort don't each re-run the
                                    // O(N) IsEmoteUnlocked/FindEmote per cell.
                                    // For /me-mote cells always true (no unlock concept).
    // Why this emote matched the active search, when the reason isn't visible in
    // the cell (an alias hit - aliases show nowhere else). Empty otherwise. Shown
    // as an extra tooltip line. Main-panel only; the Quickbar leaves it empty.
    std::string   searchNote;
};

// Drag payload identifies a single Library cell being dragged — either an
// Emote or a /me-mote, distinguished by `type`. categoryIdx distinguishes
// the source:
//   >= 0 - a favorites category at index categoryIdx; emoteIdx is the
//          slot within that category (used for in-place reorder and
//          for the erase-then-insert dance on a cross-category move).
//   <  0 - one of the built-in catalog sections (Core / Unlockable
//          / /me-motes in the main panel). emoteIdx isn't meaningful in
//          this mode; the destination just inserts the ref without
//          erasing from any source list.
// id is populated for every drag - it's the stable Id (Emote or /me-mote)
// the destination uses when the source isn't a real list it can erase from
// (catalog drags). isLocked is captured at drag start so the target can
// refuse cross-category moves of a locked emote (locked emotes can't sit
// in a new category but can be reordered within their current one);
// /me-mote drags always carry isLocked=false (no lock concept exists).
// type is the EFavoriteRefType the destination uses to construct the
// FavoriteRef on insert into a favorites category — Emote routes to
// g_Emotes, MeMote routes to g_MeMotes. Default Emote so any legacy code
// that initializes the struct with brace-init stays correct.
struct EmoteDragPayload {
    int              categoryIdx;
    int              emoteIdx;
    bool             isLocked;
    char             id[64];
    EFavoriteRefType type = EFavoriteRefType::Emote;
};

// One cell of the grid — icon button + optional label + overlays + the
// right-click context menu. isQuickbar switches the context menu over to
// the execute-variant set instead of favorites management.
void RenderEmoteCell(const CellInfo& ci, int sectionRow,
                     float cellX, float cellY,
                     float cellW, float cellH, float iconSz,
                     EViewMode mode,
                     bool allowReorder,
                     int  categoryIdx,
                     bool isQuickbar);

// Apply a drag-drop at gap `g` (a slot in FavoriteCategories[dstCat].Emotes,
// 0..size) for the given payload: catalog add, same-category reorder, or
// cross-category move. All index math is clamped; locked-emote and de-dup
// rules are enforced inside. Saves settings on a successful change. Shared by
// the grid drop zone and the empty-category target.
void ApplyEmoteDrop(const EmoteDragPayload& src, int dstCat, int gap);

// Draw the addon-blue insertion marker (vertical bar + end caps) on the
// current window's draw list, from `top` to `bottom` at x = `x`. Used for the
// "drop goes here" preview in both the grid and the empty-category line.
void DrawDropInsertionLine(float x, float top, float bottom);

// Horizontal category-reorder indicator: a full-width (edge-inset) blue bar at
// screen-space Y `yScreen`, in the gap between category sections. Shared by the
// per-header "insert before" target (Cells.cpp) and the "move to end" zone
// (MainPanel.cpp) so both render identically.
void DrawCategoryDropLine(float yScreen);

// Accept a hovering FAV_DRAG emote drop into category `dstCat` at insertion index
// `gap` (call inside an open BeginDragDropTarget). Applies on delivery via
// ApplyEmoteDrop; returns true while a meaningful payload hovers so the caller can
// draw its cue. Shared by the empty-category line and the collapsed-category header.
bool AcceptEmoteDropInto(int dstCat, int gap);

// Cell geometry for a view mode at a given (already-clamped) icon scale.
// Single source of truth for the grid layout AND the Quickbar window
// drag-snap (which needs the cell step before the grid renders). Full mode's
// height depends on the current font, so this reads ImGui::GetFontSize().
void  EmoteCellSize(EViewMode mode, float scale,
                    float& cellW, float& cellH, float& iconSz);
// Breathing room drawn above the grid for a given mode (0 for Text-only).
float EmoteGridTopPad(EViewMode mode);

// Canonical grid-fit computation: given an item count and the available
// pixel space the grid will fill, return the columns / rows the layout
// will use plus the visible-count and overflow flags. SHARED between
// Cells.cpp (actual layout) and Quickbar.cpp (same-frame overflow
// prediction so the bar gutter reservation decision matches the cell
// renderer's eventual decision - eliminates the one-frame-lag flicker at
// the overflow boundary). The two callers MUST agree, so they call this.
//
//   horizontal=false: row-major. cols are determined by avail.x (cells
//                     flow across, then wrap down). Overflow is on Y.
//   horizontal=true : column-major. rows are determined by avail.y
//                     (cells flow down, then wrap right). Overflow is on X.
//
// `avail` is the pixel space the grid OCCUPIES - already excluding any
// chrome (gutter, top-pad, etc.) the caller has reserved. The helper
// applies kGridFitEps internally so both call sites are byte-identical.
struct GridFit {
    int   cols       = 1;
    int   rows       = 1;
    int   visCols    = 1;       // columns that fit in avail.x
    int   visRows    = 1;       // rows that fit in avail.y
    bool  overflowsX = false;   // cols > visCols (horizontal/column-major)
    bool  overflowsY = false;   // rows > visRows (row-major)
    float maxScrollX = 0.f;     // cell-aligned: (cols - visCols) * stepX
    float maxScrollY = 0.f;     //               (rows - visRows) * stepY
};
GridFit ComputeQuickbarGridFit(int itemCount, ImVec2 avail,
                                ImVec2 cellSize, ImVec2 spacing,
                                bool horizontal);

// A whole section's manual flow-layout grid.
//   horizontal=false (default) — row-major: cols fit available width,
//                                grid grows downward (vertical scroll).
//   horizontal=true            — column-major: rows fit available height,
//                                grid grows rightward (horizontal scroll).
// Both modes reflow on resize.
void RenderEmoteSection(const std::vector<CellInfo>& items, bool allowReorder,
                        int categoryIdx, EViewMode mode, float scale = 1.f,
                        bool isQuickbar = false, bool horizontal = false);

// Section banner with an icon (star for user, paperclip for built-in) and a
// coloured title - used for the built-in Core / Unlockable sections. When
// `collapsed` is non-null the header gains a disclosure arrow and toggles that
// flag on click (saving), returning the EFFECTIVE collapsed state (forced open
// while `searchActive`). With `collapsed` null it's a plain, non-interactive
// header. User favorites categories use RenderCategoryHeader instead.
bool SectionHeader(const char* label, bool isUser = false,
                   bool* collapsed = nullptr, bool searchActive = false);

// Header for a user favorites category in the Library: a gold, arrow-prefixed
// row that toggles collapse on click, reorders on drag (drop line in the gap,
// CAT_DRAG payload), and offers Rename / Delete on right-click. Returns the
// EFFECTIVE collapsed state (always expanded while `searchActive`, without
// touching the stored flag), so the caller skips the grid when it's true.
bool RenderCategoryHeader(int categoryIdx, const char* name, bool searchActive = false);
