#include "gear.hpp"
#include "game.hpp"
#include "log.hpp"
#include "nn/fs.hpp"
#include <cstdlib>
#include <cstring>

namespace nn::fs {
    void SetEnabledAutoAbort(bool enabled);
    Result DeleteFile(char const* path);
}

namespace mk11::gear {

    namespace {
        // FItemDefinitionHandle: the definition's FGuid.
        struct DefinitionHandle {
            u64 lo, hi;
        };

        bool ParseGuid(const char* s, DefinitionHandle* out) {
            if (strlen(s) != 32) return false;
            u32 part[4];
            for (int i = 0; i < 4; i++) {
                char text[9];
                memcpy(text, s + i * 8, 8);
                text[8] = 0;
                char* end;
                part[i] = (u32)strtoul(text, &end, 16);
                if (*end) return false;
            }
            out->lo = part[0] | (u64)part[1] << 32;
            out->hi = part[2] | (u64)part[3] << 32;
            return true;
        }

        bool Resolve(const char* id, DefinitionHandle* out) {
            if (ParseGuid(id, out)) return true;
            game::NRSString name(id);
            *out = game::Fn<DefinitionHandle (*)(const game::NRSString&)>(off::InventoryManager_FindItemDefinitionHandleByName)(name);
            return out->lo || out->hi;
        }

        // The level new gear gets: the owning character's experience level, as the game does.
        int CharacterLevel(void* user, const void* def) {
            u64 character = game::Fn<u64 (*)(const void*)>(off::ItemDefinition_GetCharacterKey)(def);
            int type = game::Fn<int (*)(const u64*)>(off::GetCharacterExperienceType)(&character);
            void* manager = *reinterpret_cast<void**>(exl::util::modules::GetTargetStart() + off::Got_ExperienceManager);
            auto stats = game::Fn<const u8* (*)(void*, int, const void*)>(off::ExperienceManager_GetExperienceStats)(manager, type, user);
            return stats ? *reinterpret_cast<const s32*>(stats + 60) : 1;
        }

        void DestroyGearItem(u8* gear) {
            *reinterpret_cast<s32*>(gear + 0xc8) = 0;
            game::Fn<void (*)(void*)>(off::TArrayBase_Dtor)(gear + 0xc0);
            game::Fn<void (*)(void*)>(off::AttributeDefinitionInstanceArray_Dtor)(gear + 0xb0);
            *reinterpret_cast<s32*>(gear + 0xa8) = 0;
            game::Fn<void (*)(void*)>(off::TArrayBase_Dtor)(gear + 0xa0);
            *reinterpret_cast<s32*>(gear + 0x88) = 0;
            game::Fn<void (*)(void*)>(off::TArrayBase_Dtor)(gear + 0x80);
            game::Fn<void (*)(void*)>(off::GearItemArray_Dtor)(gear + 0x70);
        }
    }

    bool Add(void* user, const char* id) {
        DefinitionHandle handle;
        if (!user || !Resolve(id, &handle)) {
            Log("gear: %s: unknown item", id);
            return false;
        }
        const void* def = game::Fn<const void* (*)(u64, u64)>(off::InventoryManager_FindItemDefinition)(handle.lo, handle.hi);
        if (!def) {
            Log("gear: %s: no item definition", id);
            return false;
        }
        int level = CharacterLevel(user, def);

        alignas(16) u8 gear[0x100] = {};
        game::Fn<void (*)(void*, u64, u64, int)>(off::GearItem_Ctor)(gear, handle.lo, handle.hi, level);

        // EquipmentRefType { vtable, FGUserEntityHandle (null), flag }
        alignas(16) u8 ref[0x20] = {};
        *reinterpret_cast<u64*>(ref) = *reinterpret_cast<u64*>(exl::util::modules::GetTargetStart() + off::Got_EquipmentRefTypeVtable) + 0x10;
        game::Fn<void (*)(void*)>(off::FGUserEntityHandle_Ctor)(ref + 8);
        game::Fn<void (*)(void*, const void*)>(off::FGUserEntityHandle_SetPointer)(ref + 8, nullptr);
        ref[12] = 0;

        // InventoryManager::AddItem never reads its BILootDropSourceData argument.
        alignas(16) u8 source[0x30] = {};
        bool ok = game::Fn<bool (*)(void*, void*, void*, void*)>(off::InventoryManager_AddItem)(user, gear, source, ref);
        DestroyGearItem(gear);
        Log("gear: %s (level %d) -> %s", id, level, ok ? "added" : "NOT added");
        return ok;
    }

    void ApplyGiveList(void* user) {
        constexpr const char* Path = "mk11sd:/mk11offline/give.txt";
        if (!user || !MountSd()) return;
        char* buf = nullptr;
        nn::fs::SetEnabledAutoAbort(false);
        nn::fs::FileHandle h;
        if (R_SUCCEEDED(nn::fs::OpenFile(&h, Path, nn::fs::OpenMode_Read))) {
            s64 size = 0;
            nn::fs::GetFileSize(&size, h);
            buf = static_cast<char*>(game::Malloc(size + 1));
            if (buf) {
                nn::fs::ReadFile(h, 0, buf, size);
                buf[size] = 0;
            }
            nn::fs::CloseFile(h);
            nn::fs::DeleteFile(Path);
        }
        nn::fs::SetEnabledAutoAbort(true);
        if (!buf) return;

        int added = 0, failed = 0;
        for (char* line = buf; line && *line;) {
            char* next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (char* hash = strchr(line, '#')) *hash = 0;
            while (*line == ' ' || *line == '\t') line++;
            for (char* e = line + strlen(line); e > line && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
            if (*line) (Add(user, line) ? added : failed)++;
            line = next;
        }
        game::Free(buf);
        Log("gear: give list done, %d added, %d failed", added, failed);
    }
}
