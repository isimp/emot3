#pragma once

// Screen-space rects of every interactive widget rendered in the Quickbar
// last frame (icon buttons, title bar, category selector). The QB's
// click-through logic hit-tests the cursor against this list before
// deciding whether to add ImGuiWindowFlags_NoInputs to the window - over
// any rect -> keep input; off -> pass through to the game.
//
// This is the one ImVec2-typed Quickbar global, so it lives in a ui/ header
// (not core/Globals.h) to keep imgui out of the data/ and core/ layers.
// Defined in ui/Quickbar.cpp; written by the Quickbar / cell render, read by
// the click-through check and the dev memory monitor.

#include <utility>
#include <vector>

#include "imgui/imgui.h"   // ImVec2

extern std::vector<std::pair<ImVec2, ImVec2>> g_QbIconRects;
