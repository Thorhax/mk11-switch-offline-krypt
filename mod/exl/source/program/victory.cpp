#include "lib.hpp"
#include "game.hpp"
#include "log.hpp"
#include "config.hpp"
#include "victory.hpp"
#include <strings.h>

// Cyrax and Sektor have no victory cinema (CharacterDefinition::GetCharacterCinema finds none for them). At the
// end of a match MKS_FightEngine::WinnerWinpose then runs pGenericVictoryCinema, which without a cinema blends
// the winner back to the stance, calls RequestVictoryEnd and ends in SleepForever: they just stood there. After a
// Brutality the game instead runs the winner's BrutalityVictory (PLAYER_INFO::GetCharacterBrutalityVictory), the
// raised fist, which also ends in SleepForever; their scripts have one. For them the stance blend is skipped and
// the final SleepForever becomes their BrutalityVictory.
// Borrowing another fighter's victory package was tried and dropped: its imports (e.g. Disk.Cinematics.Global
// for Terminator_C_Victory) are only loaded when that fighter is in the match, and the game closed.

using namespace mk11;

namespace {
    constexpr u32 SleepForeverHash = 0x3fc4e673;      // FNV-1 of "SleepForever"
    constexpr u32 BrutalityVictoryHash = 0x28b4bc37;  // FNV-1 of "BrutalityVictory"

    void* s_PoseScript = nullptr;  // the winner's script, while its pose is due

    bool HasWinPose(void* fighter) {
        const void* def = fighter ? game::Fn<const void* (*)(void*)>(off::FGCharacterObj_GetCurrentCharacterDefinition)(fighter) : nullptr;
        const char* name = def ? static_cast<const game::NRSString*>(def)->c_str() : "";
        return !strcasecmp(name, "CHAR_Cyrax") || !strcasecmp(name, "CHAR_Sektor");
    }
}

// The call is a nop now; this makes it (BlendToStanceCharacter(winner, 0, 5, 5)) except for Cyrax and Sektor.
HOOK_DEFINE_INLINE(VictoryStance) {
    static void Callback(exl::hook::InlineCtx* ctx) {
        void* fighter = reinterpret_cast<void*>(ctx->X[0]);
        s_PoseScript = nullptr;
        if (config::VictoryPose() && HasWinPose(fighter)) {
            void* script = game::Fn<void* (*)(void*)>(off::GameplaySkeletalObj_GetActivePlayerDataScript)(fighter);
            if (script && game::Fn<u32 (*)(void*, u32, bool)>(off::MKS_ScriptObj_GetFunctionIndex)(script, BrutalityVictoryHash, false)) {
                s_PoseScript = script;
                return;
            }
        }
        game::Fn<void (*)(void*, float, float, float)>(off::BlendToStanceCharacter)(fighter, 0.0f, 5.0f, 5.0f);
    }
};

// pGenericVictoryCinema's last step: VM::CallScriptFunction(vm, x1 = script, w2 = SleepForever, w3 = 0 arguments).
HOOK_DEFINE_INLINE(VictorySleep) {
    static void Callback(exl::hook::InlineCtx* ctx) {
        if (!s_PoseScript || ctx->W[2] != SleepForeverHash) return;
        ctx->X[1] = reinterpret_cast<u64>(s_PoseScript);
        ctx->W[2] = BrutalityVictoryHash;
        s_PoseScript = nullptr;
        LogVerbose("victory: win pose (BrutalityVictory)");
    }
};

namespace mk11::victory {
    void Install() {
        // config.ini is read when the hooks run: the SD card is not mounted yet while the module installs.
        const uintptr_t base = exl::util::modules::GetTargetStart();
        const u32* blend = reinterpret_cast<const u32*>(base + off::GenericVictoryCinema_BlendToStanceCall);
        const u32* sleep = reinterpret_cast<const u32*>(base + off::GenericVictoryCinema_SleepArgs);
        // bl BlendToStanceCharacter; mov/movk w2, SleepForever; mov w3, wzr
        if (*blend != 0x976d32a2 || sleep[-4] != 0x529cce62 || sleep[-3] != 0x72a7f882 || sleep[0] != 0x2a1f03e3) {
            Log("victory: unexpected code at the victory pose, left alone");
            return;
        }
        exl::patch::CodePatcher(off::GenericVictoryCinema_BlendToStanceCall).Write<u32>(0xd503201f);  // nop
        VictoryStance::InstallAtOffset(off::GenericVictoryCinema_BlendToStanceCall);
        VictorySleep::InstallAtOffset(off::GenericVictoryCinema_SleepArgs);
    }
}
