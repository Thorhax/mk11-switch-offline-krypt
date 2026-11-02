#include "lib.hpp"
#include "game.hpp"
#include "log.hpp"
#include "selectart.hpp"
#include "nn/fs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace nn::fs {
    void SetEnabledAutoAbort(bool enabled);
}

// Select screen art for Cyrax, Sektor and Kronika.
//
// Cells: the player select movie has all 42 grid cells (playerSelectGrid.cells.Cell_<row * 6 + col>); the five
// the base roster leaves free are hidden (visible = false). The script still sets a texture on the extra
// fighters' cells (UIImageElement::setTexture "#MainlineManaged.UI.ui_ps_CharacterImages.cell_<Name>.tga"),
// but the game has no portraits for them. That call shows the cell again and swaps in art that is loaded on
// this screen anyway: Cyrax's and Sektor's assist Konsumable icons and Kronika's announcer voice icon, scaled
// and moved to sit inside the frame.
//
// Versus art (the large portrait beside the grid, p1MeshImage / p2MeshImage) comes from the SD card; without it
// the previous fighter's stays.
//
// Art on the SD card: sdmc:/mk11offline/art/<Name>_cell.art (128 x 256, replaces the borrowed cell art) and
// <Name>_large.art (1024 x 1088; mirrored and moved right for player 2), made with tools/make_art.py. Each image request ("img://<element>",
// FGFxImageLoader::LoadProtocolImage) for those elements is answered with a Scaleform RawImage of the file.

using namespace mk11;

namespace {
    // Scaleform::GFx::Value (0x30): list links, object interface, type, value, aux.
    struct GValue {
        void* prev = reinterpret_cast<void*>(-1);
        void* next = reinterpret_cast<void*>(-1);
        void* iface = nullptr;
        u32 type = 0;
        u32 pad = 0;
        union {
            void* data;
            double num;
            s32 i;
            u32 u;
            bool b;
        } v = {};
        u64 aux = 0;

        GValue() = default;
        GValue(const GValue&) = delete;
        ~GValue() { game::Fn<void (*)(GValue*)>(off::GFxValue_Dtor)(this); }
        u32 Kind() const { return type & 0x1f; }
        bool IsObject() const { return Kind() >= 8 && iface; }
        bool IsDisplay() const { return Kind() == 10; }
    };
    static_assert(sizeof(GValue) == 0x30);

    bool Member(const GValue& obj, const char* name, GValue* out) {
        if (!obj.IsObject()) return false;
        return game::Fn<bool (*)(void*, void*, const char*, GValue*, bool)>(off::GFxValue_GetMember)(obj.iface, obj.v.data, name, out, obj.IsDisplay());
    }

    bool SetMember(const GValue& obj, const char* name, const GValue& value) {
        return game::Fn<bool (*)(void*, void*, const char*, const GValue*, bool)>(off::GFxValue_SetMember)(obj.iface, obj.v.data, name, &value,
                                                                                                         obj.IsDisplay());
    }

    double Number(const GValue& obj, const char* name) {
        GValue n;
        if (!Member(obj, name, &n)) return 0;
        return n.Kind() == 5 ? n.v.num : n.Kind() == 3 ? n.v.i : n.Kind() == 4 ? n.v.u : 0;
    }

    bool SetNumber(const GValue& obj, const char* name, double value) {
        GValue n;
        n.type = 5;
        n.v.num = value;
        return SetMember(obj, name, n);
    }

    const char* PathOf(void* element) {
        const char* name = *reinterpret_cast<const char* const*>(static_cast<u8*>(element) + 8);
        s32 len = *reinterpret_cast<const s32*>(static_cast<u8*>(element) + 16);
        return name && len ? name : "";
    }

    // An element's Flash value when the game itself would change it directly (+32 clear: not queued).
    const GValue* DirectValue(void* element) {
        const GValue* value = *reinterpret_cast<GValue* const*>(static_cast<u8*>(element) + 24);
        u32 queued = *reinterpret_cast<const u32*>(static_cast<u8*>(element) + 32);
        return queued ? nullptr : value;
    }

    struct Art {
        const char* name;     // as in the game's asset names
        s32 cell;             // grid cell number (row * 6 + col), as in roster.cpp
        const char* texture;  // borrowed cell texture, in the game's setTexture form
        double scale, dy;     // fitted to the cell frame on the console
    };
    constexpr Art Arts[] = {
        { "Cyrax", 0, "#MainlineManaged.UI.UI_G_CONSUMABLEICONS.Consumable_CyraxAssist.tga", 1.6, 15 },
        { "Sektor", 5, "#MainlineManaged.UI.UI_G_CONSUMABLEICONS.Consumable_SektorAssist.tga", 1.6, 15 },
        { "Kronika", 41, "#MainlineManaged.UI.ui_g_KosmeticIcons.Announcer.AnnouncerVoice_Kronika.tga", 0.33, 8 },
    };
    constexpr s32 ArtCount = sizeof(Arts) / sizeof(Arts[0]);

    const Art* ArtNamed(const char* name, size_t len) {
        for (const Art& a : Arts)
            if (strlen(a.name) == len && strncmp(a.name, name, len) == 0) return &a;
        return nullptr;
    }

    constexpr ptrdiff_t Texture2DClass = 0xabdd0d8;  // UTexture2D::PrivateStaticClass

    // The borrowed textures are loaded with other screens' UI (the main menu has them, the select screen
    // after a fight does not). The first time one is found it is rooted, so it stays loaded for the session;
    // until then the cell stays hidden.
    bool Loaded(const char* texture) {
        static const char* s_Rooted[ArtCount];
        for (const char* r : s_Rooted)
            if (r == texture) return true;
        char path[128];
        snprintf(path, sizeof(path), "%.*s", (int)strlen(texture) - 5, texture + 1);  // without '#' and ".tga"
        const void* texture2D = *reinterpret_cast<void* const*>(exl::util::modules::GetTargetStart() + Texture2DClass);
        void* tex = game::Fn<void* (*)(const void*, void*, const char*, int)>(off::UObject_StaticFindObject)(texture2D, nullptr, path, 0);
        if (!tex) return false;
        game::Fn<void (*)(void*)>(off::UObject_AddToRoot)(tex);
        for (const char*& r : s_Rooted)
            if (!r) {
                r = texture;
                break;
            }
        Log("selectart: %s kept loaded", path);
        return true;
    }
}

// Art files on the SD card, as Scaleform images.
namespace {
    struct ArtHeader {
        char magic[8];  // "MK11ART2"
        u32 width, height;
        u32 pad;  // columns on the left to leave out
    };

    // Player 2's versus image is placed at about x 430 and shows to the right of the grid (x 840 on), where the
    // game's own art has the face; the mirrored picture is moved right by this many (black) columns.
    constexpr u32 Player2Shift = 480;

    // A RawImage (R8G8B8A8) of sdmc:/mk11offline/art/<name>_<kind>.art, or mirrored and moved right for player 2;
    // null if there is none.
    void* LoadArt(const char* name, const char* kind, bool mirror) {
        if (!MountSd()) return nullptr;
        char path[96];
        snprintf(path, sizeof(path), "mk11sd:/mk11offline/art/%s_%s.art", name, kind);
        nn::fs::FileHandle h;
        nn::fs::SetEnabledAutoAbort(false);
        bool open = R_SUCCEEDED(nn::fs::OpenFile(&h, path, nn::fs::OpenMode_Read));
        nn::fs::SetEnabledAutoAbort(true);
        if (!open) return nullptr;
        void* img = nullptr;
        ArtHeader head = {};
        s64 size = 0;
        nn::fs::GetFileSize(&size, h);
        nn::fs::ReadFile(h, 0, &head, sizeof(head));
        if (memcmp(head.magic, "MK11ART2", 8) != 0 || !head.width || !head.height || head.width > 2048 || head.height > 2048 ||
            head.pad >= head.width || size != (s64)(sizeof(head) + (u64)head.width * head.height * 4)) {
            Log("selectart: %s is not a valid art file", path);
        } else {
            u32 skip = head.pad;
            u32 width = head.width - skip;
            u32 shift = mirror ? Player2Shift : 0;
            u32 dims[2] = { width + shift, head.height };
            img = game::Fn<void* (*)(u32, u32, const u32*, u32, void*, void*)>(off::RawImage_Create)(1, 1, dims, 0, nullptr, nullptr);
            u8* planes = img ? *reinterpret_cast<u8**>(static_cast<u8*>(img) + 0x38) : nullptr;  // RawImage ImageData pPlanes
            u64 pitch = planes ? *reinterpret_cast<u64*>(planes + 8) : 0;
            u8* data = planes ? *reinterpret_cast<u8**>(planes + 24) : nullptr;
            if (!data || pitch < (width + shift) * 4u) {
                Log("selectart: no image memory for %s", path);
                img = nullptr;  // a failed RawImage is left to the game's allocator
            } else {
                u64 row = head.width * 4u;
                for (u32 y = 0; y < head.height; y++) {
                    for (u32 x = 0; x < shift; x++) memcpy(data + y * pitch + x * 4, "\0\0\0\xff", 4);
                    u8* line = data + y * pitch + shift * 4;
                    nn::fs::ReadFile(h, sizeof(head) + y * row + skip * 4u, line, width * 4u);
                    if (mirror)
                        for (u32 a = 0, b = width - 1; a < b; a++, b--)
                            for (u32 c = 0; c < 4; c++) {
                                u8 t = line[a * 4 + c];
                                line[a * 4 + c] = line[b * 4 + c];
                                line[b * 4 + c] = t;
                            }
                }
                Log("selectart: %s %ux%u%s", path, width + shift, head.height, mirror ? " (mirrored for player 2)" : "");
            }
        }
        nn::fs::CloseFile(h);
        return img;
    }

    void AddRef(void* img) { game::Fn<void (*)(void*)>(off::ScaleformRefCount_AddRef)(img); }
    void Release(void* img) { game::Fn<void (*)(void*)>(off::ScaleformRefCount_Release)(img); }

    // Images that answer requests for an element's url. Set on the game thread, read on the Scaleform one.
    struct Answer {
        char url[192];
        void* image;
    };
    constexpr s32 CardAnswers = 8;  // variation cards (four per player)
    Answer s_Answers[ArtCount + 2 + CardAnswers];  // the three cells, the two versus images, then variation cards
    s32 s_Shown[2] = { -1, -1 };  // per player: the extra fighter (Arts index) whose versus art is shown, or -1
    volatile u32 s_Lock = 0;

    void Lock() {
        while (__atomic_exchange_n(&s_Lock, 1, __ATOMIC_ACQUIRE)) {}
    }
    void Unlock() { __atomic_store_n(&s_Lock, 0, __ATOMIC_RELEASE); }

    // Takes over one reference to `image`.
    void SetAnswer(Answer& a, const char* url, void* image) {
        Lock();
        void* old = a.image;
        snprintf(a.url, sizeof(a.url), "%s", url);
        a.image = image;
        Unlock();
        if (old) Release(old);
    }

    void* Answered(const char* url) {
        void* img = nullptr;
        Lock();
        for (Answer& a : s_Answers)
            if (a.image && strcmp(a.url, url) == 0) {
                img = a.image;
                AddRef(img);
                break;
            }
        Unlock();
        return img;
    }

    // Cell art from the SD card, read once per name.
    void* CellArt(s32 index) {
        static void* s_Cells[ArtCount];
        static bool s_Tried[ArtCount];
        if (!s_Tried[index]) {
            s_Tried[index] = true;
            s_Cells[index] = LoadArt(Arts[index].name, "cell", false);
        }
        return s_Cells[index];
    }
}

// Variation cards (loadoutSelect ... cardN.cardImageVis.loadoutImage) get no texture for the extra fighters: they
// show the fighter's cell art (the player's fighter is the one whose versus art is shown).
namespace {
    bool VariationCard(void* element, const char* path, const char* name) {
        if (!strstr(path, ".loadoutSelect.") || !strstr(path, ".loadoutImage")) return false;
        s32 player = strstr(path, "P2module") ? 1 : 0;
        s32 i = s_Shown[player];
        if (name && name[0]) return false;
        void* own = i >= 0 ? CellArt(i) : nullptr;
        if (!own || !Loaded(Arts[i].texture)) return false;
        const char* card = strstr(path, ".card");
        s32 slot = ArtCount + 2 + (player * 4 + (card ? atoi(card + 5) / 3 : 0)) % CardAnswers;
        char url[192];
        snprintf(url, sizeof(url), "img://%s", path);
        AddRef(own);
        SetAnswer(s_Answers[slot], url, own);
        return true;
    }
}

// Cells: shows the cell (cellImage's grandparent), then fits the borrowed art or lets the SD card art answer.
HOOK_DEFINE_TRAMPOLINE(SetTexture) {
    static void Callback(void* element, const char* name) {
        const char* path = element ? PathOf(element) : "";
        if (VariationCard(element, path, name)) {
            s32 i = s_Shown[strstr(path, "P2module") ? 1 : 0];
            Orig(element, Arts[i].texture);
            // the card hides its image (cardImageVis) when the loadout has none
            GValue on;
            on.type = 2;
            on.v.b = true;
            GValue vis;
            if (const GValue* image = DirectValue(element)) {
                SetMember(*image, "visible", on);
                if (Member(*image, "parent", &vis) && vis.IsDisplay()) SetMember(vis, "visible", on);
            }
            return;
        }
        const char* p = strstr(path, "playerSelectGrid.cells.Cell_");
        const char* rest = p ? strchr(p + 28, '.') : nullptr;
        if (!rest || strcmp(rest, ".Cell.cellImage") != 0) return Orig(element, name);
        s32 n = atoi(p + 28);
        s32 i = 0;
        while (i < ArtCount && Arts[i].cell != n) i++;
        if (i == ArtCount || !Loaded(Arts[i].texture)) return Orig(element, name);
        const Art& art = Arts[i];

        void* own = CellArt(i);
        if (own) {
            char url[192];
            snprintf(url, sizeof(url), "img://%s", path);
            AddRef(own);
            SetAnswer(s_Answers[i], url, own);
        }
        const GValue* image = DirectValue(element);
        GValue inner, cell;
        if (!image || !Member(*image, "parent", &inner) || !Member(inner, "parent", &cell) || !cell.IsDisplay()) {
            Log("selectart: Cell_%d left hidden", art.cell);
            return Orig(element, name);
        }
        GValue on;
        on.type = 2;
        on.v.b = true;
        SetMember(cell, "visible", on);
        double scale = Number(*image, "scaleX");
        if (!own && !(scale > art.scale - 0.001 && scale < art.scale + 0.001)) {
            double y = Number(*image, "y");
            SetNumber(*image, "scaleX", art.scale);
            SetNumber(*image, "scaleY", art.scale);
            SetNumber(*image, "y", y + art.dy);
        }
        Orig(element, art.texture);
    }
};

HOOK_DEFINE_TRAMPOLINE(LoadProtocolImage) {
    static void* Callback(void* self, const void* info, const void* url) {
        const char* u = reinterpret_cast<const char*>((*reinterpret_cast<const uintptr_t*>(url) & ~uintptr_t(3)) + 12);  // String data
        if (void* img = Answered(u)) return img;
        return Orig(self, info, url);
    }
};

// Versus art.
namespace {
    s32 SlotOf(void* element) { return strstr(PathOf(element), "p2MeshImage") ? 1 : 0; }
}

// The fighter whose variations are opening: CharacterLoadoutKey +0x20 is the character key (a name index; the name
// table entry's text starts at +0x10, as in UILoadoutSelectModule::GetDiskPresetLoadoutImage).
HOOK_DEFINE_TRAMPOLINE(LoadoutSelectStart) {
    static void Callback(void* screen, s32 cursor, void* pad, const void* keys, s32 index, bool flag) {
        const u8* data = keys ? *static_cast<const u8* const*>(keys) : nullptr;
        s32 num = keys ? *reinterpret_cast<const s32*>(static_cast<const u8*>(keys) + 8) : 0;
        const char* name = "";
        if (data && num > 0) {
            u32 key = *reinterpret_cast<const u32*>(data + 0x20);
            auto table = **reinterpret_cast<const u8** const* const*>(exl::util::modules::GetTargetStart() + off::Got_FNameTable);
            if (table && table[key]) name = reinterpret_cast<const char*>(table[key] + 0x10);
        }
        constexpr const char* Keys[ArtCount] = { "CYR", "SEK", "KRO" };  // character keys, in Arts order
        s32 shown = -1;
        for (s32 i = 0; i < ArtCount; i++)
            if (strcasecmp(name, Keys[i]) == 0) shown = i;
        s_Shown[cursor == 1 ? 1 : 0] = shown;
        LogVerbose("selectart: variations of %s (player %d)", name, cursor + 1);
        Orig(screen, cursor, pad, keys, index, flag);
    }
};

namespace mk11::selectart {
    void ClearVersusArt(void* element) {
        s32 slot = SlotOf(element);
        Answer& a = s_Answers[ArtCount + slot];
        char url[192];
        snprintf(url, sizeof(url), "img://%s", PathOf(element));
        if (a.image && strcmp(a.url, url) == 0) SetAnswer(a, "", nullptr);
    }

    const char* VersusArt(void* element, const char* name) {
        const char* base = strstr(name, "ui_ps_Versus_Jpegs.");
        const char* side = base ? strstr(base + 19, "_P") : nullptr;
        const Art* art = side ? ArtNamed(base + 19, side - (base + 19)) : nullptr;
        s32 slot = SlotOf(element);
        if (base) s_Shown[slot] = art ? (s32)(art - Arts) : -1;
        if (!art) return nullptr;

        // Art from the SD card: a stand-in decodes as usual and its image request gets the file's image.
        static void* s_Images[2];
        static const char* s_ImageNames[2];
        if (s_ImageNames[slot] != art->name) {
            if (s_Images[slot]) Release(s_Images[slot]);
            s_Images[slot] = LoadArt(art->name, "large", slot == 1);
            s_ImageNames[slot] = art->name;
        }
        if (void* img = s_Images[slot]) {
            char url[192];
            snprintf(url, sizeof(url), "img://%s", PathOf(element));
            AddRef(img);
            SetAnswer(s_Answers[ArtCount + slot], url, img);
            static char standIn[2][80];
            snprintf(standIn[slot], sizeof(standIn[slot]), "MainlineManaged.UI.ui_ps_Versus_Jpegs.Scorpion%s", side);
            return standIn[slot];
        }

        return nullptr;
    }

    void Install() {
        SetTexture::InstallAtOffset(off::UIImageElement_setTexture);
        LoadProtocolImage::InstallAtOffset(off::FGFxImageLoader_LoadProtocolImage);
        LoadoutSelectStart::InstallAtOffset(off::UIPlayerSelectModuleScreen_LoadoutSelectStart);
    }
}
