#include "log.hpp"
#include "lib.hpp"
#include "nn/fs.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace nn::fs {
    Result CreateDirectory(char const* path);
    Result DeleteFile(char const* path);
    void SetEnabledAutoAbort(bool enabled);
}

namespace mk11 {

    namespace {
        constexpr const char* Dir = "mk11sd:/mk11offline";
        constexpr const char* Path = "mk11sd:/mk11offline/log.txt";

        std::atomic_flag s_Lock = ATOMIC_FLAG_INIT;
        s64 s_Offset = 0;
        bool s_Ready = false;
        bool s_Tried = false;

        struct Guard {
            Guard()  { while (s_Lock.test_and_set(std::memory_order_acquire)) {} }
            ~Guard() { s_Lock.clear(std::memory_order_release); }
        };
    }

    // nn::fs is not usable during module init (it runs before the SDK has set up its heap and
    // file system), so the SD card is mounted on first use instead.
    bool MountSd() {
        static std::atomic_flag lock = ATOMIC_FLAG_INIT;
        static bool tried = false, mounted = false;
        while (lock.test_and_set(std::memory_order_acquire)) {}
        if (!tried) {
            tried = true;
            // A failed fs call must not abort the game.
            nn::fs::SetEnabledAutoAbort(false);
            mounted = R_SUCCEEDED(nn::fs::MountSdCardForDebug("mk11sd"));
            if (mounted) nn::fs::CreateDirectory(Dir);
            nn::fs::SetEnabledAutoAbort(true);
        }
        lock.clear(std::memory_order_release);
        return mounted;
    }

    void LogInit() {
        s_Tried = true;
        if (!MountSd()) return;
        nn::fs::SetEnabledAutoAbort(false);
        nn::fs::DeleteFile(Path);
        s_Ready = R_SUCCEEDED(nn::fs::CreateFile(Path, 0));
        nn::fs::SetEnabledAutoAbort(true);
    }

    void Log(const char* fmt, ...) {
        char buf[512];
        va_list a;
        va_start(a, fmt);
        int n = vsnprintf(buf, sizeof(buf) - 1, fmt, a);
        va_end(a);
        if (n < 0) return;
        if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
        buf[n++] = '\n';

        Guard g;
        if (!s_Tried) LogInit();
        if (!s_Ready) return;
        // Open per line so the file can be read over FTP while the game runs.
        nn::fs::FileHandle h;
        nn::fs::SetEnabledAutoAbort(false);
        if (R_SUCCEEDED(nn::fs::OpenFile(&h, Path, nn::fs::OpenMode_Write | nn::fs::OpenMode_Append))) {
            nn::fs::WriteFile(h, s_Offset, buf, n, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
            nn::fs::CloseFile(h);
            s_Offset += n;
        }
        nn::fs::SetEnabledAutoAbort(true);
    }
}
