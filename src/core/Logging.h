#pragma once

// printf-style wrappers around Nexus's APIDefs->Log. Safe to call before
// AddonLoad has wired the Nexus API — they no-op until APIDefs is set.
// Channel name "emot3" is baked in.
//
// Use the macros (not the function directly) so future filtering or
// per-level compile-out is easy to add.

#include "nexus/Nexus.h"

void EmotLog(ELogLevel level, const char* fmt, ...);

#define LOG_CRITICAL(...) EmotLog(ELogLevel_CRITICAL, __VA_ARGS__)
#define LOG_WARNING(...)  EmotLog(ELogLevel_WARNING,  __VA_ARGS__)
#define LOG_INFO(...)     EmotLog(ELogLevel_INFO,     __VA_ARGS__)
#define LOG_DEBUG(...)    EmotLog(ELogLevel_DEBUG,    __VA_ARGS__)
#define LOG_TRACE(...)    EmotLog(ELogLevel_TRACE,    __VA_ARGS__)
