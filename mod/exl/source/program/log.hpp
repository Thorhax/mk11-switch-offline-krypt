#pragma once
// Minimal thread-safe logger writing to sdmc:/mk11offline/log.txt through the game's own nn::fs.

#include <cstdarg>
#include <cstddef>

namespace mk11 {
    // Mounts the SD card as "mk11sd:" once; safe to call from any thread.
    bool MountSd();
    void LogInit();
    void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
}
