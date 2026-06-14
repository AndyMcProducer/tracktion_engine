#pragma once
#include <cstdio>
#include <ctime>

// Avoid full engine include here to prevent circular dependencies.
// Just need basic types and functions for logging.

#if TRACKTION_ENABLE_ARA

/** Low-level file-based logging for ARA events.
    Defined as static inline to avoid linkage issues across the engine.
*/
static inline void engine_ara_log(const char* msg)
{
    if (auto* f = fopen("C:\\Temp\\ara_debug_log.txt", "a"))
    {
        time_t now = time(nullptr);
        char buf[64];
        strftime(buf, sizeof(buf), "%d %b %Y %H:%M:%S", localtime(&now));
        fprintf(f, "%s: [ENGINE] %s\n", buf, msg);
        fclose(f);
    }
}

#else
static inline void engine_ara_log(const char*) {}
#endif
