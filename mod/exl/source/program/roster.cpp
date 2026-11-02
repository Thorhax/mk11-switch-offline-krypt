#include "lib.hpp"
#include "game.hpp"
#include "log.hpp"
#include "roster.hpp"
#include "selectart.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <utility>

// Character select (versus, local modes): Cyrax, Sektor and Kronika.
//
// The select screen is PlayerSelect.mko, compiled to native code (MKS_PlayerSelect::*). Its roster is the
// script global charTable: 37 rows of 0x30 bytes {s32 row, s32 col, "CHAR_<Name>", portrait, key, ...},
// reached through MKS::VM::GetStackAddr(CharTableId); every loop and bounds check uses the constant 37
// (or 36). The fighters themselves load and fight like any other (tested as CPU opponents), so the
// screen gets a copy of charTable with three more rows in free grid cells, and the 37s become 40.

using namespace mk11;

namespace {
    constexpr u64 CharTableId = 0x2810000000001288;
    constexpr s32 BaseRows = 37;

    struct Row {
        s32 row, col;
        const char* name;      // CHAR_<Name>
        const char* portrait;  // texture in ui_ps_CharacterImages
        const char* key;       // three-letter character key
        u64 rest[2];           // copied from a base-game row
    };
    static_assert(sizeof(Row) == 0x30);

    // The free grid cells they go in are hidden in the select screen's movie, and the game has no select
    // screen art for them (ui_ps_CharacterImages / ui_ps_Versus_Jpegs); selectart.cpp shows the cells with
    // borrowed art.
    struct Extra {
        s32 row, col;
        const char* name;
        const char* portrait;
        const char* key;
    };
    constexpr Extra Extras[] = {
        { 0, 0, "CHAR_Cyrax",   "Cyrax",   "CYR" },
        { 0, 5, "CHAR_Sektor",  "Sektor",  "SEK" },
        { 6, 5, "CHAR_Kronika", "Kronika", "KRO" },
    };
    constexpr s32 Rows = BaseRows + sizeof(Extras) / sizeof(Extras[0]);

    // The row-count constants in MKS_PlayerSelect (cmp #36 / #37, and 37 as "no character in this cell").
    struct Patch {
        ptrdiff_t offset;
        u32 original, patched;
    };
    constexpr Patch Patches[] = {
        { 0x49c0e10, 0x7100911f, 0x71009d1f },  // PlayerSelect_InitCellHeads
        { 0x49c11a8, 0x7100951f, 0x7100a11f },
        { 0x49c2664, 0x7100911f, 0x71009d1f },  // PlayerSelectLogic_LockCells
        { 0x49c3870, 0x7100911f, 0x71009d1f },  // GetNumAvailablePlayerSelectChars
        { 0x49c3c98, 0x7100911f, 0x71009d1f },  // GetRandomPlayerSelectCharacter
        { 0x49d40d0, 0x7100951f, 0x7100a11f },  // GetCellByIndex
        { 0x49d42b0, 0x7100951f, 0x7100a11f },  // GetCharacterNameByIndex
        { 0x49d43d0, 0x7100911f, 0x71009d1f },  // GetCharacterIndexByName
        { 0x49d4414, 0x7100951f, 0x7100a11f },
        { 0x49d45e8, 0x7100951f, 0x7100a11f },  // GetCharacterBaseImageNameByIndex
        { 0x49d46dc, 0x7100951f, 0x7100a11f },  // IsCharacterIndexDLC
        { 0x49d47dc, 0x7100951f, 0x7100a11f },  // GetCharacterUnlockPrice
        { 0x49d4b4c, 0x7100951f, 0x7100a11f },  // IsCharacterFighterPackEarlyAccess
        { 0x49d4d14, 0x7100951f, 0x7100a11f },  // IsCharacterAftermathExpansionEarlyAccess
        { 0x49d5d98, 0x7100911f, 0x71009d1f },  // GetCharacterSelectionIndex_int_int
        { 0x49d5dcc, 0x528004a8, 0x52800508 },  //   (not found = row count)
        { 0x49e0e0c, 0x7100911f, 0x71009d1f },  // AIFighter_GetPlayerSelectSelectionOverride
        { 0x49e0e50, 0x7100951f, 0x7100a11f },
        { 0x49e0e74, 0x7100951f, 0x7100a11f },
        { 0x49e15ac, 0x7100951f, 0x7100a11f },  // DebugSetCharacterFromStartupScript
        { 0x49e15d0, 0x7100951f, 0x7100a11f },
        { 0x49f8cf4, 0x7100951f, 0x7100a11f },  // GetLateJoinCharacterNameByIndex
        { 0x49f8de8, 0x7100951f, 0x7100a11f },  // GetLateJoinCharacterBaseImageNameByIndex
    };

    const void* s_Original = nullptr;  // the script's charTable
    Row s_Table[Rows];
    bool s_Patched = false;

    void ApplyPatches() {
        if (s_Patched) return;
        s_Patched = true;
        const u8* main = reinterpret_cast<const u8*>(exl::util::modules::GetTargetStart());
        for (const Patch& p : Patches)
            if (*reinterpret_cast<const u32*>(main + p.offset) != p.original) {
                Log("roster: unexpected code at main+%#lx, roster left at %d", p.offset, BaseRows);
                return;
            }
        for (const Patch& p : Patches) exl::patch::CodePatcher(p.offset).Write<u32>(p.patched);
        Log("roster: %d character select rows", Rows);
    }

    // Copies charTable (when `table` is it) with the extra rows; false when it is not charTable.
    bool Build(void* vm, const void* table) {
        void* script = game::Fn<void* (*)(void*, bool)>(off::MKS_VM_GetScript)(vm, true);
        if (!script || game::Fn<const void* (*)(void*, const char*, bool)>(off::MKS_ScriptObj_GetVariable)(script, "charTable", true) != table)
            return false;
        s32 rows = game::Fn<s32 (*)(void*, const void*)>(off::MKS_ScriptObj_GetRowCount)(script, table);
        if (rows != BaseRows) {
            Log("roster: charTable has %d rows, expected %d; left alone", rows, BaseRows);
            return false;
        }
        const Row* base = static_cast<const Row*>(table);
        const Row* tmpl = base;
        for (s32 i = 0; i < BaseRows; i++)
            if (base[i].name && strcmp(base[i].name, "CHAR_Scorpion") == 0) tmpl = &base[i];
        memcpy(s_Table, base, sizeof(Row) * BaseRows);
        for (size_t i = 0; i < sizeof(Extras) / sizeof(Extras[0]); i++) {
            Row& r = s_Table[BaseRows + i];
            r = *tmpl;
            r.row = Extras[i].row;
            r.col = Extras[i].col;
            r.name = Extras[i].name;
            r.portrait = Extras[i].portrait;
            r.key = Extras[i].key;
        }
        s_Original = table;
        return true;
    }
}

HOOK_DEFINE_TRAMPOLINE(StackAddr) {
    static void* Callback(void* vm, u64 id) {
        void* addr = Orig(vm, id);
        if (id != CharTableId || !addr) return addr;
        if (addr != s_Original && !Build(vm, addr)) return addr;
        ApplyPatches();
        return s_Table;
    }
};
// The grid's navigation table (UIGridSelectionNavTable, from the script): one entry per cell, 6 columns x 7
// rows, {cell, left, right, up, down} as (row, col) pairs. The free cells have entries but nothing leads
// to them, so the screen gets a copy that links the extra fighters in.
namespace {
    struct NavCell {
        s32 row, col;
    };
    struct NavEntry {
        NavCell cell, left, right, up, down;
    };
    static_assert(sizeof(NavEntry) == 40);

    constexpr s32 GridCells = 42;
    NavEntry s_Nav[GridCells];

    NavEntry* Find(s32 row, s32 col) {
        for (NavEntry& e : s_Nav)
            if (e.cell.row == row && e.cell.col == col) return &e;
        return nullptr;
    }

    // Sets an entry's neighbours; a {-1, -1} neighbour is left as it is.
    bool Link(s32 row, s32 col, NavCell left, NavCell right, NavCell up, NavCell down) {
        NavEntry* e = Find(row, col);
        if (!e) return false;
        for (auto [to, from] : { std::pair{&e->left, left}, {&e->right, right}, {&e->up, up}, {&e->down, down} })
            if (from.row >= 0) *to = from;
        return true;
    }

    // Cyrax (0,0) and Sektor (0,5) complete the top row, Kronika (6,5) ends the bottom one.
    bool LinkExtras() {
        constexpr NavCell Keep = { -1, -1 };
        return Link(0, 0, {0, 5}, {0, 1}, {5, 0}, {1, 0}) &&  // Cyrax
               Link(0, 5, {0, 4}, {0, 0}, {6, 5}, {1, 5}) &&  // Sektor
               Link(6, 5, {6, 4}, {6, 2}, {5, 5}, {0, 5}) &&  // Kronika
               Link(0, 1, {0, 0}, Keep, Keep, Keep) && Link(0, 4, Keep, {0, 5}, Keep, Keep) &&
               Link(1, 0, Keep, Keep, {0, 0}, Keep) && Link(5, 0, Keep, Keep, Keep, {0, 0}) &&
               Link(1, 5, Keep, Keep, {0, 5}, Keep) && Link(5, 5, Keep, Keep, Keep, {6, 5}) &&
               Link(6, 4, Keep, {6, 5}, Keep, Keep) && Link(6, 2, {6, 5}, Keep, Keep, Keep);
    }

    bool IsExtra(const char* name) {
        for (const Extra& e : Extras)
            if (strcasecmp(name, e.name) == 0) return true;
        return false;
    }

    bool Online() { return game::Fn<bool (*)()>(off::MKOnline_IsAnyUserHydraConnected)(); }
}

HOOK_DEFINE_TRAMPOLINE(ScreenCtor) {
    static void Callback(void* screen, const NavEntry* nav, s32 cells, s32 columns) {
        if (nav && cells == GridCells && columns == 6) {
            memcpy(s_Nav, nav, sizeof(s_Nav));
            if (LinkExtras()) nav = s_Nav;
            else Log("roster: unexpected navigation table, extra fighters not linked");
        }
        Orig(screen, nav, cells, columns);
    }
};

// The extra fighters have no character content entry (UFGGameSettings::GetCharacterInfo), which reads as
// locked. Offline they are available; with a Hydra connection they stay locked (local modes only).
HOOK_DEFINE_TRAMPOLINE(CharacterLocked) {
    static bool Callback(void* user, const char* name) {
        if (name && IsExtra(name) && !Online()) return false;
        return Orig(user, name);
    }
};

// Finishers: the game has no victim data for the extra fighters (they were only ever fought in Story Mode),
// e.g. a Brutality on Cyrax closed the game while setting up his ragdoll. Fatalities, stage fatalities
// and Brutalities are therefore off in fights with one of them; Fatal Blows work.
namespace {
    constexpr ptrdiff_t GameInfoRef = 0xab81c18;  // -> FGGameInfo*

    bool ExtraInFight() {
        void* gameInfo = **reinterpret_cast<void** const*>(exl::util::modules::GetTargetStart() + GameInfoRef);
        if (!gameInfo) return false;
        for (s32 player = 0; player < 2; player++) {
            void* info = game::Fn<void* (*)(void*, s32)>(off::FGGameInfo_GetPlayerInfo)(gameInfo, player);
            void* fighter = info ? game::Fn<void* (*)(void*, u32)>(off::PLAYER_INFO_GetActiveFGCharacterObj)(info, 1) : nullptr;
            const void* def = fighter ? game::Fn<const void* (*)(void*)>(off::FGCharacterObj_GetCurrentCharacterDefinition)(fighter) : nullptr;
            if (def && IsExtra(static_cast<const game::NRSString*>(def)->c_str())) return true;
        }
        return false;
    }

    void LogBlocked(const char* what) {
        static int s_Logged = 0;
        if (s_Logged++ < 20) LogVerbose("roster: %s blocked (extra fighter in the fight)", what);
    }
}

HOOK_DEFINE_TRAMPOLINE(BrutalityAllowed) {
    static bool Callback(const void* attacker, const void* victim) {
        if (!Orig(attacker, victim)) return false;
        if (!ExtraInFight()) return true;
        LogBlocked("Brutality");
        return false;
    }
};

HOOK_DEFINE_TRAMPOLINE(FatalityAvailable) {
    static bool Callback() {
        if (!Orig()) return false;
        if (!ExtraInFight()) return true;
        LogBlocked("Fatality");
        return false;
    }
};

HOOK_DEFINE_TRAMPOLINE(StageFatalityAvailable) {
    static bool Callback(void* fighter) {
        if (!Orig(fighter)) return false;
        if (!ExtraInFight()) return true;
        LogBlocked("Stage fatality");
        return false;
    }
};

// UI images are texture objects found by name (UIImageElement::SetTextureDecodeAsync -> StaticFindObject)
// and decompressed without a null check, so an image that does not exist (the extra fighters have no
// versus art) closes the game. Missing images are skipped instead.
namespace {
    constexpr ptrdiff_t ImageClassRef = 0xab67b80;  // -> UClass* used by SetTextureDecodeAsync

    bool ImageExists(const char* name) {
        game::NRSString path(name);
        game::NRSString base = game::Fn<game::NRSString (*)(const game::NRSString&, int)>(off::PathUtils_GetBaseFilename)(path, 1);
        void* const* classRef = *reinterpret_cast<void* const* const*>(exl::util::modules::GetTargetStart() + ImageClassRef);
        return game::Fn<void* (*)(const void*, void*, const char*, int)>(off::UObject_StaticFindObject)(*classRef, nullptr, base.c_str(), 0);
    }
}

HOOK_DEFINE_TRAMPOLINE(ImageDecode) {
    static void Callback(void* element, const char* name, u32 flags) {
        selectart::ClearVersusArt(element);
        if (name && *name && !ImageExists(name)) {
            // The extra fighters' versus art (selectart.cpp): shown already, or a stand-in to decode.
            const char* standIn = selectart::VersusArt(element, name);
            if (standIn && !*standIn) return;
            if (standIn && ImageExists(standIn)) return Orig(element, standIn, flags);
            Log("roster: no image %s, skipped", name);
            return;
        }
        Orig(element, name, flags);
    }
};

// Logs the message of every failed assert (e.g. a missing import while loading a fighter's packages).
HOOK_DEFINE_TRAMPOLINE(FailAssert) {
    static void Callback(const char* expr, const char* file, int line, const char* fmt, ...) {
        char buf[384];
        va_list a;
        va_start(a, fmt);
        vsnprintf(buf, sizeof(buf), fmt ? fmt : "", a);
        va_end(a);
        Log("assert: %s:%d %s: %s", file ? file : "?", line, expr ? expr : "", buf);
        Orig(expr, file, line, "%s", buf);
    }
};

namespace mk11::roster {
    void Install() {
        StackAddr::InstallAtOffset(off::MKS_VM_GetStackAddr);
        ScreenCtor::InstallAtOffset(off::UIPlayerSelectModuleScreen_Ctor);
        CharacterLocked::InstallAtOffset(off::FGGameInfo_GetIsCharacterLocked);
        ImageDecode::InstallAtOffset(off::UIImageElement_SetTextureDecodeAsync);
        BrutalityAllowed::InstallAtOffset(off::IsBrutalityAllowed);
        FatalityAvailable::InstallAtOffset(off::IsFatalityAvailable);
        StageFatalityAvailable::InstallAtOffset(off::IsBackgroundFatalityAvailable);
        FailAssert::InstallAtOffset(off::NRS_FailAssert);
    }
}
