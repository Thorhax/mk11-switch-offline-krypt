#include "lib.hpp"
#include "game.hpp"
#include "log.hpp"
#include "startup.hpp"

// Pressing A at the title runs MKS_MainMenu::StartMainMenuFromAttract, which opens "Updating Game Data...", calls
// Tweakvars::GetLatest(true) and, with an Agora connection, WaitForInstallingDLC. GetLatest(true) waits in
// Tweakvars::WaitForJobToComplete for the tweakvar download job started at boot (AttractModeLegalScreen), sleeping
// until the job reports complete, with no time limit; the job asks the server for the tweakvar version and file.
// It is the one wait on that path without a time limit (a tester without internet reported being stuck at
// "Updating Game Data"; the EULA download before it times out). The wait now gives up after WaitLimit; ApplyLatestData then finds
// the job incomplete and applies nothing (as when there is no job).
// WaitForJobToComplete itself can't be hooked: it starts 8 bytes before a page boundary, and exlaunch's far patch
// (5 instructions) across it aborts the boot; its call in GetLatestFromServer is replaced instead.

using namespace mk11;

namespace {
    constexpr u64 TicksPerSecond = 19200000ull;  // system counter
    constexpr u64 WaitLimit = TicksPerSecond * 15;

    u64 Ticks() {
        u64 t;
        asm volatile("mrs %0, cntpct_el0" : "=r"(t));
        return t;
    }

    float Seconds(u64 ticks) { return static_cast<float>(ticks) / TicksPerSecond; }
}

// GetLatestFromServer's bl WaitForJobToComplete is a nop now; this waits in its place, at most WaitLimit.
HOOK_DEFINE_INLINE(TweakvarWait) {
    static void Callback(exl::hook::InlineCtx* ctx) {
        void* tweakvars = reinterpret_cast<void*>(ctx->X[0]);
        const void* name = reinterpret_cast<const void*>(ctx->X[1]);
        const u64 start = Ticks();
        for (;;) {
            void* job = game::Fn<void* (*)(void*, const void*)>(off::Tweakvars_GetJob)(tweakvars, name);
            if (!job || game::Fn<bool (*)(void*)>(off::TweakvarDownloadJob_IsComplete)(job)) break;
            if (Ticks() - start > WaitLimit) {
                Log("startup: tweakvar download still running after %.0f s, not waiting for it", Seconds(WaitLimit));
                return;
            }
            game::Fn<void (*)(u32)>(off::MKProcSleep)(1);
        }
        LogVerbose("startup: tweakvar wait %.1f s", Seconds(Ticks() - start));
    }
};

namespace mk11::startup {
    void Install() {
        const u32* call = reinterpret_cast<const u32*>(exl::util::modules::GetTargetStart() + off::Tweakvars_GetLatestFromServer_WaitCall);
        if (*call != 0x940000b0) {  // bl Tweakvars::WaitForJobToComplete
            Log("startup: unexpected code at the tweakvar wait, left alone");
            return;
        }
        exl::patch::CodePatcher(off::Tweakvars_GetLatestFromServer_WaitCall).Write<u32>(0xd503201f);  // nop
        TweakvarWait::InstallAtOffset(off::Tweakvars_GetLatestFromServer_WaitCall);
    }
}
