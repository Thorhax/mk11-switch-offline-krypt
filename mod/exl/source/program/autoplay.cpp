#include "lib.hpp"
#include "game.hpp"
#include "log.hpp"
#include "config.hpp"
#include "autoplay.hpp"

// p1_cpu=1: player 1 is CPU-controlled. Fight scripts call FGAIGameInterface::CheckForDrones at the start of
// each round, which turns every player whose PLAYER_INFO +56 bit 0 (the CPU flag) is set into an AI drone;
// player 1 gets that flag first. Player 1 gets the controller back when the end of match menu is shown
// (UIFightHUD::BuildOnEndOfMatchFade), or that menu would not answer it; earlier, during the match's ending
// choreography, the match stalls. Becoming a drone binds the player to a spare virtual gamepad and forgets the
// physical one (AIGameInterface::TurnPlayerIntoDrone), so that one is remembered and bound again.

using namespace mk11;

namespace {
    constexpr ptrdiff_t GameInfoRef = 0xab81c18;  // -> FGGameInfo*
    constexpr ptrdiff_t PlayerCpuFlags = 56;
    constexpr ptrdiff_t PlayerGamepad = 48;  // GAMEPAD*
    void* s_Gamepad = nullptr;              // player 1's physical gamepad

    u8* Player1() {
        void* gameInfo = **reinterpret_cast<void** const*>(exl::util::modules::GetTargetStart() + GameInfoRef);
        return gameInfo ? game::Fn<u8* (*)(void*, s32)>(off::FGGameInfo_GetPlayerInfo)(gameInfo, 0) : nullptr;
    }
}

HOOK_DEFINE_TRAMPOLINE(CheckForDrones) {
    static void Callback() {
        if (config::P1Cpu()) {
            u8* player = Player1();
            void* pad = player ? *reinterpret_cast<void**>(player + PlayerGamepad) : nullptr;
            if (pad && game::Fn<bool (*)(void*)>(off::GAMEPAD_IsPhysicalGamepad)(pad)) s_Gamepad = pad;
            if (player && !(player[PlayerCpuFlags] & 1)) {
                player[PlayerCpuFlags] |= 1;
                Log("autoplay: player 1 is CPU-controlled (p1_cpu=1)");
            }
        }
        Orig();
    }
};

HOOK_DEFINE_TRAMPOLINE(EndOfMatchMenu) {
    static void Callback(void* self) {
        Orig(self);
        if (!config::P1Cpu()) return;
        u8* player = Player1();
        if (!player) return;
        player[PlayerCpuFlags] &= ~1;
        if (game::Fn<void* (*)(u8*)>(off::PLAYER_INFO_GetAIDrone)(player)) {
            game::Fn<void (*)(u8*)>(off::FGAIGameInterface_TurnDroneIntoPlayer)(player);
            if (s_Gamepad && !*reinterpret_cast<void**>(player + PlayerGamepad))
                game::Fn<void (*)(u8*, void*)>(off::Input_BindCharToGamepad)(player, s_Gamepad);
            Log("autoplay: player 1 has the controller back (gamepad %p)", *reinterpret_cast<void**>(player + PlayerGamepad));
        }
    }
};

namespace mk11::autoplay {
    void Install() {
        CheckForDrones::InstallAtOffset(off::FGAIGameInterface_CheckForDrones);
        EndOfMatchMenu::InstallAtOffset(off::UIFightHUD_BuildOnEndOfMatchFade);
    }
}
