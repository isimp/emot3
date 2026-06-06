#pragma once

// Options > Text Emotes tab — editor for the /me-mote catalog (see
// src/data/MeMotes.h). Separate from OptionsEmotes (the official emote
// catalog) because /me-motes carry a different schema (Name/Text/TextYou/
// TextAll, no Command, no IsCore/IsTargetable/IsMadKing). Both tabs share
// the same row-based collapsible-editor UX so users don't have to relearn.
//
// Registered in Options.cpp (AddonOptions) directly after the Emotes tab.

void RenderMeMotesTab();
