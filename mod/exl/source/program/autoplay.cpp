#include "lib.hpp"
#include "game.hpp"
#include "log.hpp"
#include "config.hpp"
#include "autoplay.hpp"

// p1_cpu=1: player 1 is CPU-controlled. Fight scripts call FGAIGameInterface::CheckForDrones at the start of
// each round, which turns every player whose PLAYER_INFO +56 bit 0 (the CPU flag) is set into an AI drone;
// player 1 gets that flag first.

using namespace mk11;

namespace {
    constexpr ptrdiff_t GameInfoRef = 0xab81c18;  // -> FGGameInfo*
    constexpr ptrdiff_t PlayerCpuFlags = 56;
}

HOOK_DEFINE_TRAMPOLINE(CheckForDrones) {
    static void Callback() {
        if (config::P1Cpu()) {
            void* gameInfo = **reinterpret_cast<void** const*>(exl::util::modules::GetTargetStart() + GameInfoRef);
            u8* player = gameInfo ? game::Fn<u8* (*)(void*, s32)>(off::FGGameInfo_GetPlayerInfo)(gameInfo, 0) : nullptr;
            if (player && !(player[PlayerCpuFlags] & 1)) {
                player[PlayerCpuFlags] |= 1;
                Log("autoplay: player 1 is CPU-controlled (p1_cpu=1)");
            }
        }
        Orig();
    }
};

namespace mk11::autoplay {
    void Install() {
        CheckForDrones::InstallAtOffset(off::FGAIGameInterface_CheckForDrones);
    }
}
