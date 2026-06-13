// ChatApi.h — minimal ABI mirror of the "Events: Chat" Nexus addon's public
// chat-message event, so emot3 can subscribe to it as an OPTIONAL dependency.
//
// The upstream addon is jsantorek/GW2-Chat (https://github.com/jsantorek/GW2-Chat).
// Its src/Chat.h carries NO license, so we do NOT copy it. This is an independent
// re-declaration of only the wire ABI we consume (the event name + the Message
// struct it broadcasts), matching the upstream layout so the payload casts
// correctly. Same pattern as vendor/hoardandseek. If the upstream ABI changes,
// re-verify against its Chat.h and update here.
//
// Verified against GW2-Chat src/Chat.h @ 2026-06 (plain C, no #pragma pack).
#ifndef EMOT3_GW2CHAT_API_H
#define EMOT3_GW2CHAT_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The Nexus event raised once per chat line. Subscribe via
// APIDefs->Events.Subscribe(EMOT3_GW2CHAT_EVENT, cb); the callback receives a
// `const gw2chat::Message*` (cast from void*). The addon's own signature, for
// EV_ADDON_LOADED/UNLOADED gating.
#define EMOT3_GW2CHAT_EVENT     "EV_CHAT:Message"
#define EMOT3_GW2CHAT_SIGNATURE 0x67770263

// All strings are raw, null-terminated, UTF-8, owned by the publisher and valid
// ONLY for the duration of the synchronous event callback — copy before returning.
typedef char* gw2chat_StringUTF8;

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} gw2chat_Guid;  // Windows GUID

typedef struct {
    uint32_t Low;
    uint32_t High;
} gw2chat_Timestamp;  // Windows FILETIME

typedef enum {
    GW2CHAT_Error = 0,
    GW2CHAT_Guild,
    GW2CHAT_GuildMotD,
    GW2CHAT_Local,
    GW2CHAT_Map,
    GW2CHAT_Party,
    GW2CHAT_Squad,
    GW2CHAT_SquadMessage,
    GW2CHAT_SquadBroadcast,
    GW2CHAT_TeamPvP,
    GW2CHAT_TeamWvW,
    GW2CHAT_Whisper,
    GW2CHAT_Emote,
    GW2CHAT_EmoteCustom
} gw2chat_MessageType;

typedef enum {
    GW2CHAT_FlagNone                 = 0,
    GW2CHAT_Guild_IsRepresented      = 1 << 0,
    GW2CHAT_Squad_IsFromCommander    = 1 << 1,
    GW2CHAT_Party_IsFromCommander    = 1 << 1,
    GW2CHAT_Local_IsFromMentor       = 1 << 2,
    GW2CHAT_Map_IsFromMentor         = 1 << 2,
    GW2CHAT_SquadMessage_IsBroadcast = 1 << 3,
    GW2CHAT_Whisper_IsFromMe         = 1 << 4   // only set for whispers
} gw2chat_MetadataFlags;

typedef enum {
    GW2CHAT_EternalBattlegrounds  = 38,
    GW2CHAT_GreenAlpineBorderlands = 95,
    GW2CHAT_BlueAlpineBorderlands  = 96,
    GW2CHAT_ObsidianSanctum        = 899,
    GW2CHAT_EdgeOfTheMists         = 968,
    GW2CHAT_RedDesertBorderlands   = 1099,
    GW2CHAT_ArmisticeBastion       = 1315
} gw2chat_MapId;

typedef enum {
    GW2CHAT_EmoteBless  = 0x81213e71,
    GW2CHAT_EmoteBeckon = 0x81011505,
    GW2CHAT_EmoteDance  = 0x81074004,
    GW2CHAT_EmoteSit    = 0x810E0C2B,
    GW2CHAT_EmoteYes    = 0x810d23c5,
    GW2CHAT_EmoteNo     = 0x45c9010d,
    GW2CHAT_EmoteCower  = 0x81067f49,
    GW2CHAT_EmoteLaugh  = 0x45cf010d
} gw2chat_EmoteType;

typedef struct {
    gw2chat_Guid      Account;
    gw2chat_StringUTF8 CharacterName;
    gw2chat_StringUTF8 AccountName;   // nullptr if unavailable
    gw2chat_StringUTF8 Content;
} gw2chat_GenericMessage;

typedef struct {
    gw2chat_GenericMessage Base;
    uint32_t               GuildIndex;
} gw2chat_GuildMessage;

typedef struct {
    gw2chat_StringUTF8 Content;
    uint32_t           GuildIndex;
} gw2chat_GuildMessageOfTheDay;

typedef struct {
    gw2chat_GenericMessage Base;
    gw2chat_MapId          Map;
} gw2chat_WvWTeamMessage;

typedef struct {
    gw2chat_StringUTF8 CharacterName;
    gw2chat_EmoteType  ActionTaken;
} gw2chat_EmoteMessage;

typedef struct {
    gw2chat_StringUTF8 CharacterName;
    gw2chat_StringUTF8 ActionTaken;
} gw2chat_CustomEmoteMessage;

typedef struct {
    gw2chat_Timestamp     DateTime;
    gw2chat_MessageType   Type;
    gw2chat_MetadataFlags Flags;
    union {
        gw2chat_GuildMessage         Guild;
        gw2chat_GuildMessageOfTheDay GuildMotD;
        gw2chat_GenericMessage       Local;
        gw2chat_GenericMessage       Map;
        gw2chat_GenericMessage       Party;
        gw2chat_GenericMessage       Squad;
        gw2chat_StringUTF8           SquadMessage;
        gw2chat_GenericMessage       TeamPvP;
        gw2chat_WvWTeamMessage       TeamWvW;
        gw2chat_GenericMessage       Whisper;
        gw2chat_EmoteMessage         Emote;
        gw2chat_CustomEmoteMessage   EmoteCustom;
    };
} gw2chat_Message;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // EMOT3_GW2CHAT_API_H
