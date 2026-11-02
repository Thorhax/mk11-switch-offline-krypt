#include "lib.hpp"
#include "game.hpp"
#include "log.hpp"
#include "endings.hpp"
#include "nn/fs.hpp"
#include <cstdio>
#include <cstring>
#include <strings.h>

// Tower endings for Cyrax, Sektor and Kronika.
//
// A tower's character ending is a movie, ../MOVIES/<KEY>_Ending.mp4 (rom:/Movies/, e.g. SCO_Ending.mp4: H.264
// 1280x720 29.97 fps, six AAC language tracks before the video). Its name comes from
// PLAYER_INFO::GetLadderEnding (character key table + "_Ending"; Cyrax already gets CYR_Ending there, the
// hooks make sure of CYR/SEK/KRO_Ending for all three when that movie is there). The movies are added to the romfs with Atmosphere's LayeredFS (atmosphere/contents/0100F2200C984000/romfs/Movies/,
// made with tools/make_ending.py). The game's own Ladder_Ending (Kronika's defeat) plays first, then this.

using namespace mk11;

namespace nn::fs {
    void SetEnabledAutoAbort(bool enabled);
}

namespace {
    struct Extra {
        const char* name;  // character definition name
        const char* key;
    };
    constexpr Extra Extras[] = {
        { "CHAR_Cyrax", "CYR" },
        { "CHAR_Sektor", "SEK" },
        { "CHAR_Kronika", "KRO" },
    };

    // The size of rom:/Movies/<ending>.mp4, or 0 if it is not there.
    s64 MovieSize(const char* ending) {
        char path[64];
        snprintf(path, sizeof(path), "rom:/Movies/%s.mp4", ending);
        nn::fs::FileHandle h;
        nn::fs::SetEnabledAutoAbort(false);
        bool ok = R_SUCCEEDED(nn::fs::OpenFile(&h, path, nn::fs::OpenMode_Read));
        nn::fs::SetEnabledAutoAbort(true);
        s64 size = 0;
        if (ok) {
            nn::fs::GetFileSize(&size, h);
            nn::fs::CloseFile(h);
        }
        return size;
    }

    // "<KEY>_Ending" for an extra fighter whose movie is there, else null.
    const char* EndingFor(const char* character) {
        static char s_Name[32];
        for (const Extra& e : Extras)
            if (character && strcasecmp(character, e.name) == 0) {
                snprintf(s_Name, sizeof(s_Name), "%s_Ending", e.key);
                if (MovieSize(s_Name)) return s_Name;
                Log("endings: no rom:/Movies/%s.mp4, %s keeps the game's ending", s_Name, character);
                return nullptr;
            }
        return nullptr;
    }
}

// The engine only opens files listed in its table of contents (rom:/NX64TOC.txt: "<size> <chunk> <path> <flags>"),
// parsed once at boot; entries added later are not enough for movies. The text gets a line for each extra
// fighter's ending found in rom:/Movies/, like the game's own endings.
HOOK_DEFINE_TRAMPOLINE(ParseTOC) {
    static void Callback(void* toc, char* text) {
        size_t n = text && strncmp(text, "NumLaunchChunks", 15) == 0 ? strlen(text) : 0;  // the main table only
        char* grown = n ? static_cast<char*>(game::Malloc(n + 256)) : nullptr;
        if (!grown) return Orig(toc, text);
        memcpy(grown, text, n + 1);
        for (const Extra& e : Extras) {
            char ending[32];
            snprintf(ending, sizeof(ending), "%s_Ending", e.key);
            if (s64 size = MovieSize(ending)) {
                size_t at = strlen(grown);
                snprintf(grown + at, n + 256 - at, "%ld 21 ..\\Movies\\%s.mp4 55\r\n", size, ending);
                Log("endings: %s.mp4 (%ld bytes) listed in the table of contents", ending, size);
            }
        }
        Orig(toc, grown);  // the copy is kept: the table may refer to it
    }
};

HOOK_DEFINE_TRAMPOLINE(LadderEnding) {
    static bool Callback(u8* info, char* out, s32 size) {
        bool ok = Orig(info, out, size);
        void* def = game::Fn<void* (*)(u8*)>(off::PLAYER_INFO_GetCharacterDefinition)(info);
        const char* character = def ? static_cast<const game::NRSString*>(def)->c_str() : "";
        const char* ending = EndingFor(character);
        Log("endings: %s tower ending %s%s%s", character, out ? out : "", ending ? " -> " : "", ending ? ending : "");
        if (ending && out && size > 0) snprintf(out, size, "%s", ending);
        return ok;
    }
};

HOOK_DEFINE_TRAMPOLINE(CharacterLadderEnding) {
    static void Callback(const char* character, char* out, s32 size) {
        Orig(character, out, size);
        const char* ending = EndingFor(character);
        LogVerbose("endings: %s ending %s%s%s", character ? character : "", out ? out : "", ending ? " -> " : "", ending ? ending : "");
        if (ending && out && size > 0) snprintf(out, size, "%s", ending);
    }
};

namespace mk11::endings {
    void Install() {
        ParseTOC::InstallAtOffset(off::FTableOfContents_ParseFromBuffer);
        LadderEnding::InstallAtOffset(off::PLAYER_INFO_GetLadderEnding);
        CharacterLadderEnding::InstallAtOffset(off::GetCharacterLadderEnding);
    }
}
