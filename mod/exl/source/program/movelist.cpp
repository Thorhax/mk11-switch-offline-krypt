#include "lib.hpp"
#include "game.hpp"
#include "log.hpp"
#include "movelist.hpp"
#include <cctype>
#include <cstring>

// The pause menu move list for Cyrax, Sektor and Kronika, whose move list data is a "PLACEHOLDER" line.
//
// The move list script (MKS_MoveListScreen) fills one page at a time: UIMoveListScreen::SetTotalNumMoves(rows),
// SetLocalizedHeader(0, title), then SetLocalizedMove(row, name, input) per move, the input being a wide
// (4-byte) string. For these fighters the page gets the module's own rows instead. Pages: 0 basic attacks,
// 1 kombo attacks, 2 special moves, 3 finishers. The moves are the user's list (Switch button layout).
// Move names go through the localisation table (unknown keys read "BUG_ME"), so the name label is set to the
// text afterwards. The script still believes the page has its one placeholder line: its later headers and
// blank lines for the rows written here are dropped, and its cursor stays on the first move.

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


namespace mk11::movelist {
    void Install() {
        CharName::InstallAtOffset(off::UIMoveListScreen_SetLocalizedCharName);
        Total::InstallAtOffset(off::UIMoveListScreen_SetTotalNumMoves);
        MoveRow::InstallAtOffset(off::UIMoveListScreen_SetLocalizedMove);
        Header::InstallAtOffset(off::UIMoveListScreen_SetLocalizedHeader);
        Blank::InstallAtOffset(off::UIMoveListScreen_SetBlankLine);
    }
}
