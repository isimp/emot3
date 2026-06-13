#pragma once

// Auto-motes chat watch (v1.5) — subscribes to the optional "Events: Chat"
// Nexus addon (jsantorek/GW2-Chat, vendor/gw2chat/ChatApi.h) and turns the
// user's OWN sent chat lines into auto-fired catalog emotes. The trigger words
// live on the catalog emotes themselves (Emote::AutoKeywords, data/EmoteData.h);
// this module scans them and fires the match.
//
// Optional dependency, mirroring the RTAPI integration in core/CharacterState:
//   - We Subscribe to the chat event + EV_ADDON_LOADED/UNLOADED (sig-gated on
//     the Events:Chat signature) so we can detect the addon arriving/leaving.
//   - Without that addon installed nothing ever raises the event, so the whole
//     feature is inert (no crash); the Options tab shows a "requires Events:
//     Chat" notice driven by ChatWatchAvailable().
//
// Thread model: the chat event can fire on any thread, so OnChat NEVER touches
// ImGui or APIDefs and NEVER sends inline — it only matches a rule and drops the
// resolved Emote.Id into a small mutex-guarded pending slot. The render thread
// drains that slot once per frame (DrainAutoMotes) and routes the fire through
// the existing SendOrFillEmote gate (competitive lockout, can't-emote states,
// textbox/held-key/airborne refusals all apply for free).

void InitChatWatch();      // AddonLoad: subscribe to the chat + addon events
void ShutdownChatWatch();  // AddonUnload: unsubscribe

// True when the "Events: Chat" addon is loaded and publishing. Set by its
// EV_ADDON_LOADED (and, defensively, by the first chat event we receive — it
// may have loaded before us, in which case Nexus didn't replay its load event);
// cleared by its EV_ADDON_UNLOADED. Read by the Options tab for the dependency
// banner. Degrades to "not present" until proven otherwise.
bool ChatWatchAvailable();

// Render-thread, once per gameplay frame (AddonRender): if OnChat queued a
// matched rule's emote, resolve it (FindEmote) and fire it through the normal
// SendOrFillEmote gate. No-op when nothing is pending.
void DrainAutoMotes();
