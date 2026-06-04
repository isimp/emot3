#include "Logging.h"
#include "Globals.h"

#include <cstdarg>
#include <cstdio>

void EmotLog(ELogLevel level, const char* fmt, ...) {
    // Pre-init or pre-load → drop the message silently. We don't have a
    // logger to forward to yet.
    if (!APIDefs || !APIDefs->Log) return;

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) return;  // formatting error — nothing useful to forward

    APIDefs->Log(level, "emot3", buf);
}
