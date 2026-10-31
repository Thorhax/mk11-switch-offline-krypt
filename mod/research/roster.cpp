#include "lib.hpp"
#include "game.hpp"
#include "log.hpp"
#include "roster.hpp"
#include <cstdio>

// Temporary: logs the UIGridSelectionNavTable the menu script hands the player select screen, to see
// how the roster grid is described.

using namespace mk11;

namespace {
    constexpr size_t EntrySize = 40;  // UIGridSelection allocates 40 bytes per entry
}

// The cells copied from PlayerSelect.mko's charTable (UICharacterSelectNavigation +344, count +352, 0x30 each).
HOOK_DEFINE_TRAMPOLINE(NavigationSetCells) {
    static void Callback(u8* nav) {
        Orig(nav);
        static int s_Logged = 0;
        if (s_Logged++) return;
        const u8* cells = *reinterpret_cast<u8* const*>(nav + 344);
        s32 count = *reinterpret_cast<const s32*>(nav + 352);
        Log("roster: %d character select cells", count);
        for (s32 i = 0; cells && i < count && i < 64; i++) {
            const u8* c = cells + i * 0x30;
            const u64* q = reinterpret_cast<const u64*>(c);
            const char* name = reinterpret_cast<const char*>(q[1]);
            Log("roster:   (%d,%d) %s | %016lx %016lx %016lx %016lx", reinterpret_cast<const s32*>(c)[0], reinterpret_cast<const s32*>(c)[1],
                name ? name : "(null)", q[2], q[3], q[4], q[5]);
        }
    }
};

// Dumps PlayerSelect.mko's charTable (0x30-byte rows) once.
namespace {
    bool LooksLikeString(u64 p) { return p > 0x1000000 && p < 0x8000000000ull && !(p & 0); }

    void DumpCharTable() {
        static bool done = false;
        if (done) return;
        done = true;
        void* script = game::Fn<void* (*)(const char*, bool, u32)>(off::LoadScriptFromSystemArt)("PlayerSelect.mko", true, 1);
        if (!script) { Log("roster: PlayerSelect.mko not loaded"); return; }
        const u8* table = game::Fn<const u8* (*)(void*, const char*, bool)>(off::MKS_ScriptObj_GetVariable)(script, "charTable", true);
        s32 rows = table ? game::Fn<s32 (*)(void*, const void*)>(off::MKS_ScriptObj_GetRowCount)(script, table) : -1;
        Log("roster: charTable %p, %d rows", table, rows);
        for (s32 i = 0; table && i < rows && i < 64; i++) {
            const u8* r = table + i * 0x30;
            const u64* q = reinterpret_cast<const u64*>(r);
            char fields[200];
            size_t n = 0;
            for (int f = 1; f < 6; f++) {
                if (LooksLikeString(q[f])) n += snprintf(fields + n, sizeof(fields) - n, " [%d]'%.24s'", f, reinterpret_cast<const char*>(q[f]));
                else n += snprintf(fields + n, sizeof(fields) - n, " [%d]%lx", f, q[f]);
                if (n >= sizeof(fields)) break;
            }
            Log("roster:   row %d (%d,%d)%s", i, reinterpret_cast<const s32*>(r)[0], reinterpret_cast<const s32*>(r)[1], fields);
        }
    }
}

HOOK_DEFINE_TRAMPOLINE(CellImage) {
    static void Callback(void* screen, s32 a, s32 b, const char* image, const char* extra) {
        DumpCharTable();
        Log("roster: SetCellImage(%d, %d, %s, %s)", a, b, image ? image : "(null)", extra ? extra : "(null)");
        Orig(screen, a, b, image, extra);
    }
};

HOOK_DEFINE_TRAMPOLINE(CellHidden) {
    static void Callback(void* screen, s32 a, s32 b) {
        Log("roster: SetCellHidden(%d, %d)", a, b);
        Orig(screen, a, b);
    }
};

HOOK_DEFINE_TRAMPOLINE(HiddenCharacter) {
    static bool Callback(const char* name) {
        bool hidden = Orig(name);
        Log("roster: IsHiddenCharacter(%s) = %d", name ? name : "(null)", hidden);
        return hidden;
    }
};

namespace mk11::roster {
    void Install() {
        NavigationSetCells::InstallAtOffset(off::UICharacterSelectNavigation_SetCells);
        HiddenCharacter::InstallAtOffset(off::FGGameInfo_IsHiddenCharacter);
        CellImage::InstallAtOffset(off::UIPlayerSelectModuleScreen_SetCellImage);
        CellHidden::InstallAtOffset(off::UIPlayerSelectModuleScreen_SetCellHidden);
    }
}
