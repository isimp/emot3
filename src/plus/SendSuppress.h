#pragma once
// =====================================================================
//  Keyboard-swallow during emote injection - the +plus "send while moving".
//
//  Recovers the original "click whenever" send mode: while an emote
//  command is being injected, the user's real keystrokes are CONSUMED in
//  the addon WndProc (return 0) so held movement keys (WASD, etc.) can't
//  interleave with our WM_CHAR stream and garble the command.
//
//  Returning 0 to swallow keyboard input is the pattern that tripped
//  Microsoft Defender's ML heuristic (Wacatac.B!ml), so the whole thing is
//  compiled in only for the +plus flavor (EMOT3_PLUS) - exactly like the
//  click-through wheel routing (QuickbarWheel.h). In base builds these are
//  inert no-op stubs and the send falls back to the refuse-when-unsafe gate
//  (ShouldSkipEmoteSend). Gated at runtime by the persisted Plus setting
//  g_PlusSettings.SwallowInputOnSend (PlusSettings.h).
//
//  Touch points: the guarded call in entry.cpp's WndProc; the
//  SendSuppressScope around the injection worker in EmoteAction.cpp; the
//  "Send while moving" toggle in Options > General > Sending.
// =====================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// RAII: while alive AND constructed with active=true, the WndProc swallows
// the user's keyboard messages (see SendSuppressConsume). Spans the emote
// injection window; cleaned up on every worker early-return. Constructing
// with active=false (refuse mode) is a no-op. Inert in base builds regardless.
// A nesting counter backs this, so overlapping sends behave.
struct SendSuppressScope {
    explicit SendSuppressScope(bool active);
    ~SendSuppressScope();
    SendSuppressScope(const SendSuppressScope&)            = delete;
    SendSuppressScope& operator=(const SendSuppressScope&) = delete;
private:
    bool active_;
};

// WndProc: returns true if it consumed this keyboard message (caller returns 0
// so the game never sees it). True only while a SendSuppressScope(active=true)
// is alive. Always false in base builds.
bool SendSuppressConsume(UINT msg);
