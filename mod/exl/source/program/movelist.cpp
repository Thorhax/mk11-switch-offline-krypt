#include "lib.hpp"
#include "game.hpp"
#include "log.hpp"
#include "movelist.hpp"
#include <cctype>
#include <cstring>
#include <strings.h>

// The pause menu move list for Cyrax, Sektor and Kronika, whose move list data is a "PLACEHOLDER" line.
//
// The move list script (MKS_MoveListScreen) fills one page at a time: UIMoveListScreen::SetTotalNumMoves(rows),
// SetLocalizedHeader(0, title), then SetLocalizedMove(row, name, input) per move, the input being a wide
// (4-byte) string. For these fighters the page gets the module's own rows instead. Pages: 0 basic attacks,
// 1 kombo attacks, 2 special moves, 3 finishers. The moves are the user's list (Switch button layout).
// Move names go through the localisation table (unknown keys read "BUG_ME"), so the name label is set to the
// text afterwards. The script's own headers and blank lines for the rows written here are dropped; its page
// lines are extended so the cursor reaches every row (ExtendPages), and the move/frame data numbers read "-".
// The pause menu's Quick Reference box (the quick move list, SetLocalizedQuickMove per row and player) shows
// their special moves the same way.

using namespace mk11;

namespace {
    struct Move {
        const wchar_t* name;
        const wchar_t* input;
    };
    struct Page {
        const Move* moves;
        int count;
    };
    struct Fighter {
        const char* name;  // as shown on the move list (upper case)
        Page pages[4];
    };

    constexpr Move Basic[] = { { L"See Controls in the pause menu", L"" } };
    constexpr Move Finishers[] = { { L"Fatalities and Brutalities", L"Not available" } };

    constexpr Move CyraxKombos[] = {
        { L"Easy String", L"X, X, X" },
        { L"Easy String", L"Y, Y, Y" },
        { L"Slide", L"Forward + B" },
        { L"Easy String", L"Forward + A, B" },
        { L"Down-Back Special", L"Down, Back + B (Amplify: R)" },
    };
    constexpr Move CyraxSpecials[] = {
        { L"Energy Net", L"Back, Forward + Y" },
        { L"Explosive (Near)", L"Down, Back + X" },
        { L"Explosive (Medium)", L"Down, Forward + X" },
        { L"Explosive (Far)", L"Back, Forward + X" },
    };
    constexpr Move SektorKombos[] = {
        { L"Easy String", L"X, Y, X" },
        { L"Easy String", L"Back + Y, X" },
        { L"Easy String", L"Y, Y, A" },
        { L"Easy String", L"Back + B, A" },
    };
    constexpr Move SektorSpecials[] = {
        { L"Flamethrower", L"Down, Forward + B" },
        { L"Teleport", L"Down, Back + A" },
        { L"Homing Missiles", L"Back, Forward + Y" },
    };
    constexpr Move KronikaKombos[] = {
        { L"Basic String", L"Y, Y, Y" },
        { L"Basic String", L"X, X, X" },
        { L"Basic String", L"B, B, B" },
    };
    constexpr Move KronikaSpecials[] = {
        { L"Teleport", L"Down, Back + Y" },
        { L"Mid Projectile", L"Back, Forward + Y" },
        { L"Orbs", L"Down, Back + X" },
        { L"Tracking Move", L"Back, Forward + X" },
        { L"Healing/Damaging Dome", L"Down, Forward + X" },
        { L"Launcher", L"Down, Back + B" },
        { L"Combo Cancel", L"Down, Forward + B" },
        { L"Ground Crystals", L"Back, Forward + B" },
        { L"Low Attack", L"Down, Forward + A" },
    };

    template<size_t N> constexpr Page P(const Move (&m)[N]) { return { m, (int)N }; }

    constexpr Fighter Fighters[] = {
        { "CYRAX",   { P(Basic), P(CyraxKombos),   P(CyraxSpecials),   P(Finishers) } },
        { "SEKTOR",  { P(Basic), P(SektorKombos),  P(SektorSpecials),  P(Finishers) } },
        { "KRONIKA", { P(Basic), P(KronikaKombos), P(KronikaSpecials), P(Finishers) } },
    };

    const Fighter* s_Fighter = nullptr;  // the move list being shown is one of them

    constexpr ptrdiff_t GameInfoRef = 0xab81c18;  // -> FGGameInfo*

    // The fighter player `player` is playing, when it is one of them.
    const Fighter* FighterOf(u32 player) {
        void* gameInfo = **reinterpret_cast<void** const*>(exl::util::modules::GetTargetStart() + GameInfoRef);
        void* info = gameInfo ? game::Fn<void* (*)(void*, s32)>(off::FGGameInfo_GetPlayerInfo)(gameInfo, player) : nullptr;
        void* obj = info ? game::Fn<void* (*)(void*, u32)>(off::PLAYER_INFO_GetActiveFGCharacterObj)(info, 1) : nullptr;
        const void* def = obj ? game::Fn<const void* (*)(void*)>(off::FGCharacterObj_GetCurrentCharacterDefinition)(obj) : nullptr;
        if (!def) return nullptr;
        const char* name = static_cast<const game::NRSString*>(def)->c_str();  // "char_cyrax"
        if (strncasecmp(name, "char_", 5) != 0) return nullptr;
        for (const Fighter& f : Fighters)
            if (strcasecmp(name + 5, f.name) == 0) return &f;
        return nullptr;
    }

    // UIMoveListScreen: move grid widget handle at +0xa8; element paths (NRS strings) of a move's name label at
    // +728 and of a header's label at +760.
    constexpr ptrdiff_t MoveNamePath = 728, HeaderPath = 760;
    constexpr int VisibleRows = 12;

    void SetLabel(u8* screen, int row, ptrdiff_t pathOffset, const wchar_t* text) {
        u8* grid = game::Fn<u8* (*)(void*)>(off::GridSelectWidgetHandle_Get)(screen + 0xa8);
        void* cell = grid ? game::Fn<void* (*)(u8*, s32, s32)>(off::UIGridSelectWidget_GetCellElement)(grid, row, 0) : nullptr;
        const auto* path = reinterpret_cast<const game::NRSString*>(screen + pathOffset);
        void* label = cell ? game::Fn<void* (*)(void*, const char*)>(off::UIContainerWidget_GetElement)(cell, path->c_str()) : nullptr;
        if (label) game::Fn<void (*)(void*, const void*)>(off::UITextElement_setText)(label, text);
    }

    const Page* CurrentPage(void* screen) {
        if (!s_Fighter) return nullptr;
        int page = game::Fn<int (*)(void*)>(off::UIMoveListScreen_GetCurrentPageSelection)(screen);
        return page >= 0 && page < 4 ? &s_Fighter->pages[page] : nullptr;
    }
}

// The character name is a wide string ("CYRAX").
HOOK_DEFINE_TRAMPOLINE(CharName) {
    static void Callback(void* screen, const void* name) {
        s_Fighter = nullptr;
        if (name) {
            char ascii[16] = {};
            const wchar_t* w = static_cast<const wchar_t*>(name);
            for (int i = 0; i < 15 && w[i]; i++) ascii[i] = (w[i] < 128) ? toupper((int)w[i]) : '?';
            for (const Fighter& f : Fighters)
                if (strcmp(ascii, f.name) == 0) s_Fighter = &f;
        }
        Orig(screen, name);
    }
};

HOOK_DEFINE_TRAMPOLINE(Total) {
    static void Callback(void* screen, int rows) {
        if (const Page* page = CurrentPage(screen)) rows = 1 + page->count;  // the header row, then the moves
        Orig(screen, rows);
    }
};

// The script's pages (script global 0x2810000000000090): 4 x 824 bytes, each up to 100 line object pointers
// with the line count at +808. A line's type is at +12; types 1, 2 and 4 can take the cursor
// (MoveList_CanSelectMove). For these fighters a page has one selectable placeholder line, so the cursor
// stopped on the first move. Before the script fills a page's cells, rows 1..N of each of their pages point
// at that line and the count becomes 1 + N; the rows' text is written by the hooks above and below.
namespace {
    constexpr u64 PagesRegister = 0x2810000000000090;
    constexpr ptrdiff_t PageSize = 824, PageLineCount = 808, LineType = 12;
    constexpr int MaxLines = 100;

    bool Selectable(const u8* line) {
        u32 type = *reinterpret_cast<const u32*>(line + LineType);
        return type <= 4 && ((1u << type) & 0x16);
    }

    void ExtendPages(void* vm) {
        if (!s_Fighter) return;
        u8* pages = game::Fn<u8* (*)(void*, u64)>(off::MKS_VM_GetStackAddr)(vm, PagesRegister);
        if (!pages) return;
        for (int p = 0; p < 4; p++) {
            u8* page = pages + p * PageSize;
            auto lines = reinterpret_cast<u8**>(page);
            s32& count = *reinterpret_cast<s32*>(page + PageLineCount);
            int want = 1 + s_Fighter->pages[p].count;
            if (count < 1 || count > MaxLines || want > MaxLines) continue;
            u8* move = nullptr;
            for (int i = 0; i < count && !move; i++)
                if (lines[i] && Selectable(lines[i])) move = lines[i];
            if (!move || (count == want && lines[want - 1] == move)) continue;
            LogVerbose("movelist: %s page %d: %d line(s) -> %d", s_Fighter->name, p, count, want);
            for (int i = 1; i < want; i++) lines[i] = move;
            count = want;
        }
    }
}

HOOK_DEFINE_TRAMPOLINE(FillMoveCells) {
    static void Callback(void* vm) {
        ExtendPages(vm);
        Orig(vm);
    }
};

// The move data and frame data numbers are the placeholder move's; show a dash instead.
HOOK_DEFINE_TRAMPOLINE(FrameData) {
    static void Callback(void* screen, int field, const char* text, u32 localized) {
        if (CurrentPage(screen)) return Orig(screen, field, "-", 0);
        Orig(screen, field, text, localized);
    }
};

HOOK_DEFINE_TRAMPOLINE(MoveRow) {
    static void Callback(void* screen, int row, const char* name, const void* input) {
        if (CurrentPage(screen)) return;  // the placeholder line; the rows are written with the header
        Orig(screen, row, name, input);
    }
};

HOOK_DEFINE_TRAMPOLINE(Blank) {
    static void Callback(void* screen, int row) {
        const Page* page = CurrentPage(screen);
        if (page && row >= 1 && row <= page->count) return;
        Orig(screen, row);
    }
};

HOOK_DEFINE_TRAMPOLINE(Header) {
    static void Callback(void* screen, int row, const char* text) {
        const Page* page = CurrentPage(screen);
        if (page && row != 0) return;  // a placeholder section
        Orig(screen, row, text);
        if (!page) return;
        u8* s = static_cast<u8*>(screen);
        if (page == &s_Fighter->pages[3]) SetLabel(s, 0, HeaderPath, L"FINISHERS");  // its title key is missing too
        for (int i = 0; i < page->count; i++) {
            MoveRow::Orig(screen, 1 + i, "", page->moves[i].input);
            SetLabel(s, 1 + i, MoveNamePath, page->moves[i].name);
        }
        for (int row = 1 + page->count; row < VisibleRows; row++) Blank::Orig(screen, row);  // left from the last page
    }
};


// Quick move list rows: widgets at +0x200 (player 0) / +0x250 (others) + row * 8, up to QuickRows; the name is
// the row's "FocusText0.Text" under the element path at +808.
namespace {
    constexpr int QuickRows = 10;
    constexpr ptrdiff_t QuickRowPath = 808;

    void SetQuickName(u8* screen, int row, u32 player, const wchar_t* name) {
        void* widget = *reinterpret_cast<void**>(screen + (player == 0 ? 0x200 : 0x250) + row * 8);
        const auto* path = reinterpret_cast<const game::NRSString*>(screen + QuickRowPath);
        void* element = widget ? game::Fn<void* (*)(void*, const char*)>(off::UIContainerWidget_GetElement)(widget, path->c_str()) : nullptr;
        if (element) game::Fn<void (*)(void*, const char*, const void*)>(off::SetStaticTextMKString)(element, "FocusText0.Text", name);
    }
}

HOOK_DEFINE_TRAMPOLINE(QuickBlank) {
    static void Callback(void* screen, int row, u32 player) {
        const Fighter* f = FighterOf(player);
        if (f && row < f->pages[2].count) return;
        Orig(screen, row, player);
    }
};

HOOK_DEFINE_TRAMPOLINE(QuickMove) {
    static void Callback(void* screen, int row, const char* name, const void* input, u32 player, bool locked, void* pad) {
        const Fighter* f = FighterOf(player);
        if (!f) return Orig(screen, row, name, input, player, locked, pad);
        if (row != 0) return;  // placeholder lines; the whole list is written with row 0
        const Page& specials = f->pages[2];
        for (int i = 0; i < specials.count && i < QuickRows; i++) {
            Orig(screen, i, "", specials.moves[i].input, player, false, pad);
            SetQuickName(static_cast<u8*>(screen), i, player, specials.moves[i].name);
        }
        for (int i = specials.count; i < QuickRows; i++) QuickBlank::Orig(screen, i, player);
    }
};

namespace mk11::movelist {
    void Install() {
        CharName::InstallAtOffset(off::UIMoveListScreen_SetLocalizedCharName);
        Total::InstallAtOffset(off::UIMoveListScreen_SetTotalNumMoves);
        MoveRow::InstallAtOffset(off::UIMoveListScreen_SetLocalizedMove);
        Header::InstallAtOffset(off::UIMoveListScreen_SetLocalizedHeader);
        Blank::InstallAtOffset(off::UIMoveListScreen_SetBlankLine);
        QuickMove::InstallAtOffset(off::UIMoveListScreen_SetLocalizedQuickMove);
        QuickBlank::InstallAtOffset(off::UIMoveListScreen_SetQuickMoveBlankLine);
        FillMoveCells::InstallAtOffset(off::MKS_MoveListScreen_FillMoveCells);
        FrameData::InstallAtOffset(off::UIMoveListScreen_SetLocalizedFrameData);
    }
}
