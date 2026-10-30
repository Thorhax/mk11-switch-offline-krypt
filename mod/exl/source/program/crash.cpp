#include "crash.hpp"
#include "lib.hpp"
#include "log.hpp"

namespace nn::os {
    struct UserExceptionInfo;
    void SetUserExceptionHandler(void (*handler)(UserExceptionInfo*), void* stack, unsigned long stackSize, UserExceptionInfo* info);
}

namespace mk11::crash {

    namespace {
        alignas(0x1000) u8 s_Stack[0x10000];
        alignas(0x10) u8 s_Info[0x200];

        uintptr_t s_MainStart, s_MainEnd;

        bool InMain(u64 v) { return v >= s_MainStart && v < s_MainEnd; }

        void Handler(nn::os::UserExceptionInfo* info) {
            const u64* w = reinterpret_cast<const u64*>(info);
            Log("=== CRASH (user exception) ===");
            // Raw dump first: the exact UserExceptionInfo layout is not assumed.
            for (int i = 0; i < 0x140 / 8; i += 4)
                Log("info+%03x: %016lx %016lx %016lx %016lx", i * 8, w[i], w[i + 1], w[i + 2], w[i + 3]);
            for (int i = 0; i < 0x140 / 8; i++)
                if (InMain(w[i])) Log("  info+%03x -> main+%#lx", i * 8, w[i] - s_MainStart);

            // Frame-pointer walk from every stack-looking value that holds a plausible frame record.
            for (int i = 0; i < 0x140 / 8; i++) {
                u64 fp = w[i];
                if (fp < 0x1000 || (fp & 0xf) || InMain(fp)) continue;
                int depth = 0;
                while (fp && !(fp & 0xf) && depth < 24) {
                    const u64* frame = reinterpret_cast<const u64*>(fp);
                    u64 lr = frame[1];
                    if (!InMain(lr)) break;
                    if (depth == 0) Log("  backtrace from info+%03x:", i * 8);
                    Log("    #%d main+%#lx", depth, lr - s_MainStart);
                    if (frame[0] <= fp) break;
                    fp = frame[0];
                    depth++;
                }
                if (depth > 2) break;
            }
            Log("=== end of crash report ===");
        }
    }

    // Keeps the game from replacing our handler with its own.
    HOOK_DEFINE_REPLACE(BlockHandlerChange) {
        static void Callback(void (*)(nn::os::UserExceptionInfo*), void*, unsigned long, nn::os::UserExceptionInfo*) {
            Log("crash: game tried to install its own exception handler (ignored)");
        }
    };

    void Install() {
        auto& main = exl::util::GetMainModuleInfo();
        s_MainStart = main.m_Total.m_Start;
        s_MainEnd = main.m_Total.GetEnd();
        nn::os::SetUserExceptionHandler(Handler, s_Stack, sizeof(s_Stack), reinterpret_cast<nn::os::UserExceptionInfo*>(s_Info));
        BlockHandlerChange::InstallAtFuncPtr(nn::os::SetUserExceptionHandler);
    }
}
