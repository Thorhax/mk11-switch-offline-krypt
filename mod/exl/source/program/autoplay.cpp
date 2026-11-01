#include "lib.hpp"
#include "game.hpp"
#include "log.hpp"
#include "config.hpp"
#include "autoplay.hpp"

// p1_cpu=1: player 1 is CPU-controlled. Fight scripts call FGAIGameInterface::CheckForDrones at the start of
// each round. It turns a player whose CharacterDefinition is marked user-owned AI (the game's own AI fighter
// modes) into an AI drone on a virtual gamepad that keeps a link to the physical one (virtual pad +16), and the
// game hands that pad back after the match. Player 1's character is marked that way. Marking the player as a
// plain CPU (PLAYER_INFO +56 bit 0) instead made a drone that forgot the physical pad, and the end of match menu
// then ignored the controller. As a fallback, player 1 still gets the pad back when the end of match menu is
// shown (UIFightHUD::BuildOnEndOfMatchFade).

using namespace mk11;

namespace {
    constexpr ptrdiff_t GameInfoRef = 0xab81c18;  // -> FGGameInfo*
    constexpr ptrdiff_t PlayerCpuFlags = 56;
    constexpr ptrdiff_t PlayerGamepad = 48;  // GAMEPAD*
    constexpr ptrdiff_t UserOwnedAIByte = 81;  // CharacterDefinition flags (GetIsUserOwnedAI: bit 2)
    constexpr u8 UserOwnedAIBit = 0x4;
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
            void* def = player ? game::Fn<void* (*)(u8*)>(off::PLAYER_INFO_GetCharacterDefinition)(player) : nullptr;
            if (def && !(static_cast<u8*>(def)[UserOwnedAIByte] & UserOwnedAIBit)) {
                game::Fn<void (*)(void*, bool)>(off::CharacterDefinition_SetIsUserOwnedAI)(def, true);
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
        if (void* def = game::Fn<void* (*)(u8*)>(off::PLAYER_INFO_GetCharacterDefinition)(player))
            game::Fn<void (*)(void*, bool)>(off::CharacterDefinition_SetIsUserOwnedAI)(def, false);
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
