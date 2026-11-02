#include "lib.hpp"
#include "game.hpp"
#include "log.hpp"
#include "selectart.hpp"
#include <cstdlib>
#include <cstring>

// Select screen cells for Cyrax, Sektor and Kronika.
//
// The player select movie has all 42 grid cells (playerSelectGrid.cells.Cell_<row * 6 + col>); the five the
// base roster leaves free are hidden (visible = false). The script still sets a texture on the extra
// fighters' cells (UIImageElement::setTexture "#MainlineManaged.UI.ui_ps_CharacterImages.cell_<Name>.tga"),
// but the game has no portraits for them. That call shows the cell again and swaps in art that is loaded on
// this screen anyway: Cyrax's and Sektor's assist Konsumable icons and Kronika's announcer voice icon,
// scaled and moved to sit inside the frame.

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

    struct Art {
        s32 cell;             // grid cell number (row * 6 + col), as in roster.cpp
        const char* texture;  // borrowed texture, in the game's setTexture form
        double scale, dy;     // fitted to the cell frame on the console
    };
    constexpr Art Arts[] = {
        { 0, "#MainlineManaged.UI.UI_G_CONSUMABLEICONS.Consumable_CyraxAssist.tga", 1.6, 15 },
        { 5, "#MainlineManaged.UI.UI_G_CONSUMABLEICONS.Consumable_SektorAssist.tga", 1.6, 15 },
        { 41, "#MainlineManaged.UI.ui_g_KosmeticIcons.Announcer.AnnouncerVoice_Kronika.tga", 0.33, 8 },
    };

    constexpr ptrdiff_t Texture2DClass = 0xabdd0d8;  // UTexture2D::PrivateStaticClass

    // The borrowed textures are loaded with other screens' UI (the main menu has them, the select screen
    // after a fight does not). The first time one is found it is rooted, so it stays loaded for the session;
    // until then the cell stays hidden.
    bool Loaded(const Art& art) {
        static bool s_Rooted[sizeof(Arts) / sizeof(Arts[0])];
        size_t i = &art - Arts;
        if (s_Rooted[i]) return true;
        char path[128], name[96];  // full path for the lookup, short name for the log
        const char* end = art.texture + strlen(art.texture) - 4;  // ".tga"
        const char* start = end;
        while (start > art.texture && start[-1] != '.') start--;
        snprintf(path, sizeof(path), "%.*s", (int)(end - art.texture - 1), art.texture + 1);
        snprintf(name, sizeof(name), "%.*s", (int)(end - start), start);
        const void* texture2D = *reinterpret_cast<void* const*>(exl::util::modules::GetTargetStart() + Texture2DClass);
        void* tex = game::Fn<void* (*)(const void*, void*, const char*, int)>(off::UObject_StaticFindObject)(texture2D, nullptr, path, 0);
        if (!tex) return false;
        game::Fn<void (*)(void*)>(off::UObject_AddToRoot)(tex);
        s_Rooted[i] = true;
        Log("selectart: %s kept loaded for Cell_%d", name, art.cell);
        return true;
    }

    const char* PathOf(void* element) {
        const char* name = *reinterpret_cast<const char* const*>(static_cast<u8*>(element) + 8);
        s32 len = *reinterpret_cast<const s32*>(static_cast<u8*>(element) + 16);
        return name && len ? name : "";
    }

    // The art for a "...playerSelectGrid.cells.Cell_<n>.Cell.cellImage" element, if it is an extra fighter's.
    const Art* ArtFor(const char* path) {
        const char* p = strstr(path, "playerSelectGrid.cells.Cell_");
        if (!p) return nullptr;
        const char* rest = strchr(p + 28, '.');
        if (!rest || strcmp(rest, ".Cell.cellImage") != 0) return nullptr;
        s32 n = atoi(p + 28);
        for (const Art& a : Arts)
            if (a.cell == n) return &a;
        return nullptr;
    }

    // Shows the cell (cellImage's grandparent) and fits the art, directly, as the game itself does for
    // elements that are not queued (+32 clear). A second texture on the same image leaves it as it is.
    void Place(void* element, const Art& art) {
        const GValue* image = *reinterpret_cast<GValue* const*>(static_cast<u8*>(element) + 24);
        u32 queued = *reinterpret_cast<const u32*>(static_cast<u8*>(element) + 32);
        if (!image || queued) {
            Log("selectart: Cell_%d left hidden (value %p, queued %u)", art.cell, image, queued);
            return;
        }
        GValue inner, cell;
        if (!Member(*image, "parent", &inner) || !Member(inner, "parent", &cell) || !cell.IsDisplay()) {
            Log("selectart: Cell_%d not found", art.cell);
            return;
        }
        GValue on;
        on.type = 2;
        on.v.b = true;
        SetMember(cell, "visible", on);
        double scale = Number(*image, "scaleX");
        if (scale > art.scale - 0.001 && scale < art.scale + 0.001) return;
        double y = Number(*image, "y");
        SetNumber(*image, "scaleX", art.scale);
        SetNumber(*image, "scaleY", art.scale);
        SetNumber(*image, "y", y + art.dy);
    }
}

HOOK_DEFINE_TRAMPOLINE(SetTexture) {
    static void Callback(void* element, const char* name) {
        const Art* art = element ? ArtFor(PathOf(element)) : nullptr;
        if (!art || !Loaded(*art)) return Orig(element, name);
        Place(element, *art);
        Orig(element, art->texture);
    }
};

namespace mk11::selectart {
    void Install() { SetTexture::InstallAtOffset(off::UIImageElement_setTexture); }
}
