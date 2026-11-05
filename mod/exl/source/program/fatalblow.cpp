#include "lib.hpp"
#include "game.hpp"
#include "log.hpp"
#include "config.hpp"
#include "fatalblow.hpp"

// Cyrax and Sektor have no Fatal Blow: every regular fighter's script calls SetupSupermoveAttack(fighter, script,
// pFuncSupermove, 992608951), which registers the L+R command gComSupermove for its attack; theirs is never called
// and they have no pFuncSupermove. Their scripts do contain SetupSupermoveAttack, with their own script-global
// gComSupermove and condition set, so it is done here the same way after their RegisterUniversalSpecialMoveCombos,
// with one of their own attacks: Cyrax's amplified Bomb Smash, Sektor's Teleport Attack.
// A real Fatal Blow ends in a cinema whose script disables the command (GenericCommand::SetForceDisabled, as their
// ForceDisableSupermove does) and deals its damage there. Here, once L+R is accepted
// (PLAYER_INFO::CheckSuperMoveCommandInternal) the fighter is armed for a few seconds, and the first hit on the
// opponent in that time (ReactionInfo::CalculateDamage, victim at +16; Cyrax's bombs are the attacker, not him),
// unless blocked, uses the Fatal Blow up for the match and deals FatalBlowBonus of the victim's maximum life on top of the
// attack's own damage, the way a Fatal Blow cinema does (DamageCharacter, damage type 3, no attacker). The bonus
// leaves the victim alive; a knockout comes from the attack's own hits. The registration carries no rules, so
// L+R is only accepted at FatalBlowLife or less (life +3104, maximum +3108, as FGCharacterObj::GetNormalizedLife).

using namespace mk11;

namespace {
    constexpr u32 SupermoveValue = 992608951;  // passed by every regular fighter's script

    struct Extra {
        const char* name;
        u64 command;     // script global gComSupermove (MKS::RegisterID, from their SetupSupermoveAttack)
        u64 conditions;  // script global SWITCH_BITS_INDEX_FORM
        u32 attack;      // FNV-1 of the attack function's name
    };
    constexpr Extra Cyrax = { "Cyrax", 0x28100000000078e8, 0x24100000000063b8, 0xbd6ab51a };   // pFuncBombSmashMB
    constexpr Extra Sektor = { "Sektor", 0x2810000000007288, 0x2410000000005d58, 0xd4a4531c };  // pFuncTeleportAttack

    // The fighter's script, as its own registrations pass it while RegisterUniversalSpecialMoveCombos runs
    // (that function's second argument is a number, not the script).
    void* s_Fighter = nullptr;
    void* s_Script = nullptr;

    constexpr float FatalBlowBonus = 0.28f;  // of the victim's maximum life; the attack's own hits add a few percent
    constexpr float FatalBlowLife = 0.30f;   // available at or below this share of life, like everyone's
    constexpr u64 ArmedTicks = 19200000ull * 4;  // 4 s of the 19.2 MHz system counter

    struct Registered { void* fighter; void* command; };
    Registered s_Registered[4];  // the fighters of the current matches (both players can be Cyrax or Sektor)
    int s_NextSlot = 0;
    void* s_Armed = nullptr;   // fighter whose Fatal Blow is under way
    void* s_ArmedCommand = nullptr;
    void* s_Opponent = nullptr;
    u64 s_ArmedAt = 0;
    bool s_Landed = false;

    float Life(const void* fighter) {
        const u8* f = static_cast<const u8*>(fighter);
        float max = *reinterpret_cast<const float*>(f + 3108);
        return max > 0.0f ? *reinterpret_cast<const float*>(f + 3104) / max : 1.0f;
    }

    u64 Ticks() {
        u64 t;
        asm volatile("mrs %0, cntpct_el0" : "=r"(t));
        return t;
    }

    void Register(void* vm, void* fighter, void* script, const Extra& who) {
        if (!fighter || !script || !config::FatalBlows()) return;
        void* command = game::Fn<void* (*)(void*, u64)>(off::MKS_VM_GetStackAddr)(vm, who.command);
        void* conditions = game::Fn<void* (*)(void*, u64)>(off::MKS_VM_GetStackAddr)(vm, who.conditions);
        if (!command || !conditions) return;
        game::Fn<void (*)(void*, void*, void*, u32, void*, void*, u32, void*)>(off::FGCharacterObj_RegisterAttackCommand)(
            fighter, command, script, who.attack, conditions, conditions, 0x8000003b, nullptr);
        game::Fn<void (*)(void*, float, u32)>(off::GenericCommand_AutoCheckAsSuperMoveCommand)(command, 1.0f, SupermoveValue);
        game::Fn<void (*)(void*, u32, u32, u32, u32, u32, float, u32)>(off::GenericCommand_SetSpecialMoveComboSettings)(
            command, 0x20, 18, 15, 12, 0, 1.0f, SupermoveValue);
        Registered* slot = nullptr;
        for (Registered& r : s_Registered)
            if (r.fighter == fighter) slot = &r;
        if (!slot) slot = &s_Registered[s_NextSlot++ % 4];
        *slot = { fighter, command };
        LogVerbose("fatalblow: %s Fatal Blow registered (script %p)", who.name, script);
    }
}

#define EXTRA_SETUP(NAME)                                                                  \
    HOOK_DEFINE_TRAMPOLINE(NAME##Combos) {                                                 \
        static u64 Callback(void* vm) {                                                    \
            void* const* args = *reinterpret_cast<void* const* const*>(static_cast<u8*>(vm) + 112); \
            s_Fighter = args ? args[0] : nullptr;                                          \
            s_Script = nullptr;                                                            \
            u64 result = Orig(vm);                                                         \
            Register(vm, s_Fighter, s_Script, NAME);                                       \
            s_Fighter = nullptr;                                                           \
            return result;                                                                 \
        }                                                                                  \
    };
HOOK_DEFINE_TRAMPOLINE(AttackCommand) {
    static u64 Callback(void* fighter, void* command, void* script, u32 attack, void* a, void* b, u32 flags, void* check) {
        if (fighter && fighter == s_Fighter && !s_Script) s_Script = script;
        return Orig(fighter, command, script, attack, a, b, flags, check);
    }
};

HOOK_DEFINE_TRAMPOLINE(SupermoveInput) {
    static bool Callback(void* player, void* command, void* fighter, const void* opponent) {
        // The rules a regular Fatal Blow's script adds are not there: only at low life here.
        for (const Registered& r : s_Registered)
            if (r.command == command && r.fighter == fighter && Life(fighter) > FatalBlowLife) return false;
        bool accepted = Orig(player, command, fighter, opponent);
        if (accepted)
            for (const Registered& r : s_Registered)
                if (r.command == command && r.fighter == fighter) {
                    s_Armed = fighter;
                    s_ArmedCommand = command;
                    s_Opponent = const_cast<void*>(opponent);
                    s_ArmedAt = Ticks();
                    s_Landed = false;
                    LogVerbose("fatalblow: L+R accepted");
                }
        return accepted;
    }
};

HOOK_DEFINE_TRAMPOLINE(HitDamage) {
    static void Callback(u8* reaction) {
        Orig(reaction);
        u8* victim = *reinterpret_cast<u8**>(reaction + 16);
        if (!s_Armed || s_Landed || !victim || victim != s_Opponent) return;
        if (Ticks() - s_ArmedAt > ArmedTicks) {
            s_Armed = nullptr;
            return;
        }
        if (game::Fn<bool (*)(void*)>(off::ReactionInfo_IsHitBlocked)(reaction)) {
            s_Armed = nullptr;  // blocked: not used up, as for everyone
            LogVerbose("fatalblow: blocked");
            return;
        }
        s_Landed = true;
        game::Fn<void (*)(void*, bool)>(off::GenericCommand_SetForceDisabled)(s_ArmedCommand, true);
        float life = *reinterpret_cast<float*>(victim + 3104), maxLife = *reinterpret_cast<float*>(victim + 3108);
        float bonus = maxLife * FatalBlowBonus;
        if (bonus > life - maxLife * 0.01f) bonus = life - maxLife * 0.01f;
        LogVerbose("fatalblow: landed, life %d/1000 of max %d/1000, bonus %d/1000", (int)(life * 1000), (int)(maxLife * 1000), (int)(bonus * 1000));
        if (bonus <= 0.0f) return;
        alignas(16) u8 move[64] = {};
        game::Fn<void (*)(void*)>(off::MoveID_Ctor)(move);
        game::Fn<void (*)(void*, float, u32, u32, u32, u32, u32, void*, void*, u32)>(off::FGCharacterObj_DamageCharacter)(
            victim, bonus, 0, 0, 3, 0, 0, nullptr, move, 1);
    }
};

EXTRA_SETUP(Cyrax)
EXTRA_SETUP(Sektor)

namespace mk11::fatalblow {
    void Install() {
        AttackCommand::InstallAtOffset(off::FGCharacterObj_RegisterAttackCommand);
        SupermoveInput::InstallAtOffset(off::PLAYER_INFO_CheckSuperMoveCommandInternal);
        HitDamage::InstallAtOffset(off::ReactionInfo_CalculateDamage);
        CyraxCombos::InstallAtOffset(off::MKS_CHAR_Cyrax_RegisterUniversalSpecialMoveCombos);
        SektorCombos::InstallAtOffset(off::MKS_CHAR_Sektor_RegisterUniversalSpecialMoveCombos);
    }
}
