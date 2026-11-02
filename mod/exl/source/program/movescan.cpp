#include "lib.hpp"
#include "game.hpp"
#include "log.hpp"
#include "movescan.hpp"
#include <cstdio>

// Research logging (verbose_log=1): the special move commands each fighter registers, with their motion index
// (SPECIAL_MOVE_COMBO_INDEX) and pad flags, to read the extra fighters' inputs.

using namespace mk11;

namespace {
    const char* NameOf(const void* cmd) {
        const char* name = cmd ? *static_cast<const char* const*>(cmd) : nullptr;
        return name ? name : "?";
    }
}

HOOK_DEFINE_TRAMPOLINE(RegSpecial) {
    static void Callback(void* fighter, void* cmd, s32 index) {
        LogVerbose("movescan: %p special %s motion %d", fighter, NameOf(cmd), index);
        Orig(fighter, cmd, index);
    }
};

HOOK_DEFINE_TRAMPOLINE(RegEXSpecial) {
    static void Callback(void* fighter, void* cmd, s32 index, u32 extra) {
        LogVerbose("movescan: %p ex %s motion %d (%u)", fighter, NameOf(cmd), index, extra);
        Orig(fighter, cmd, index, extra);
    }
};

HOOK_DEFINE_TRAMPOLINE(RegSpecialN) {
    static void Callback(void* fighter, void* cmd, void* array) {
        const s32* data = array ? *static_cast<const s32* const*>(array) : nullptr;
        s32 num = array ? *reinterpret_cast<const s32*>(static_cast<const u8*>(array) + 8) : 0;
        char list[64] = "";
        for (s32 i = 0, at = 0; data && i < num && i < 8 && at < 56; i++) at += snprintf(list + at, sizeof(list) - at, "%d ", data[i]);
        LogVerbose("movescan: %p specialN %s motions %s", fighter, NameOf(cmd), list);
        Orig(fighter, cmd, array);
    }
};

HOOK_DEFINE_TRAMPOLINE(AutoGround) {
    static void Callback(void* cmd, u64 pad, u32 a, u32 b, u32 c, u32 d, float f, u32 e, u32 g) {
        LogVerbose("movescan: %s ground pad %#llx %u %u %u %u %.2f %u %u", NameOf(cmd), (unsigned long long)pad, a, b, c, d, f, e, g);
        Orig(cmd, pad, a, b, c, d, f, e, g);
    }
};

HOOK_DEFINE_TRAMPOLINE(CommandFlags) {
    static void Callback(void* cmd, u64 pad, u32 a, u32 b, u32 c, u32 d) {
        LogVerbose("movescan: %s flags pad %#llx %u %u %u %u", NameOf(cmd), (unsigned long long)pad, a, b, c, d);
        Orig(cmd, pad, a, b, c, d);
    }
};

HOOK_DEFINE_TRAMPOLINE(RegFatality) {
    static void Callback(void* fighter, void* cmd, void* script, u32 a, const u32* pads, const u32* pads2, u32 count, void* rules) {
        char text[160] = "";
        int at = 0;
        for (int i = 0; pads && i < 8 && at < 70; i++) at += snprintf(text + at, sizeof(text) - at, "%x ", pads[i]);
        at += snprintf(text + at, sizeof(text) - at, "| ");
        for (int i = 0; pads2 && i < 8 && at < 150; i++) at += snprintf(text + at, sizeof(text) - at, "%x ", pads2[i]);
        u64 flags = cmd ? *reinterpret_cast<const u64*>(static_cast<const u8*>(cmd) + 56) : 0;
        LogVerbose("movescan: %p fatality %s a %u count %u flags %#llx pads %s", fighter, NameOf(cmd), a, count, (unsigned long long)flags, text);
        Orig(fighter, cmd, script, a, pads, pads2, count, rules);
    }
};

HOOK_DEFINE_TRAMPOLINE(AutoFatality) {
    static void Callback(void* cmd, float near, float far, u64 pad, u32 a) {
        LogVerbose("movescan: %s fatality range %.2f-%.2f pad %#llx %u", NameOf(cmd), near, far, (unsigned long long)pad, a);
        Orig(cmd, near, far, pad, a);
    }
};

namespace mk11::movescan {
    void Install() {
        RegSpecial::InstallAtOffset(off::FGCharacterObj_RegisterSpecialMoveCombo);
        RegEXSpecial::InstallAtOffset(off::FGCharacterObj_RegisterEXSpecialMoveCombo);
        RegSpecialN::InstallAtOffset(off::FGCharacterObj_RegisterSpecialMoveComboN);
        AutoGround::InstallAtOffset(off::GenericCommand_AutoCheckAsGroundSpecialCommand);
        CommandFlags::InstallAtOffset(off::GenericCommand_SetCommandFlags);
        RegFatality::InstallAtOffset(off::FGCharacterObj_RegisterFatalityCommand);
        AutoFatality::InstallAtOffset(off::GenericCommand_AutoCheckAsFatalityCommand);
    }
}
