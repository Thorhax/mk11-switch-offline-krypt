#include "gear.hpp"
#include "batch_apply.hpp"
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

        // FItemSequenceDefinition: +40 its FItemSequenceState array (each starts with the state's
        // FItemDefinitionHandle), +48 the state count.
        constexpr ptrdiff_t SequenceStatesOffset = 40;
        constexpr ptrdiff_t SequenceStateCountOffset = 48;

        const void* FindObject(const DefinitionHandle& h) {
            return game::Fn<const void* (*)(u64, u64)>(off::InventoryManager_FindItemDefinition)(h.lo, h.hi);
        }

        bool IsA(const void* object, ptrdiff_t vtableSymbol) {
            return *static_cast<const uintptr_t*>(object) == exl::util::modules::GetTargetStart() + vtableSymbol + 0x10;
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
        const void* def = FindObject(handle);
        if (def && IsA(def, off::Vtable_FItemSequenceDefinition)) {
            // Krypt chests award an item sequence (a gear piece with its upgrade states); like the
            // server and the unlock pop-up, grant the sequence's first state.
            s32 states = *reinterpret_cast<const s32*>(static_cast<const u8*>(def) + SequenceStateCountOffset);
            auto first = *reinterpret_cast<const DefinitionHandle* const*>(static_cast<const u8*>(def) + SequenceStatesOffset);
            if (states < 1 || !first) {
                Log("gear: %s: item sequence without states", id);
                return false;
            }
            handle = *first;
            def = FindObject(handle);
            LogVerbose("gear: %s is an item sequence, granting its first item", id);
        }
        if (!def || !IsA(def, off::Vtable_FItemDefinition)) {
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
        // FItemObject: vtable, FGuid at +8, internal name (e.g. KOL_GearC18_Level1) at +0x18.
        const char* name = reinterpret_cast<const game::NRSString*>(static_cast<const u8*>(def) + 0x18)->c_str();
        Log("gear: %s %s (level %d) -> %s", id, name, level, ok ? "added" : "NOT added");
        return ok;
    }

    namespace {
        // "<slug> <amount>" adds to a wallet balance: Koins, Souls, Hearts, Time Krystals or a spendable
        // item (crafting material, Kollector relic, key, Konsumable). Any other line is a gear name or GUID.
        bool GiveLine(void* user, char* line) {
            char* space = strpbrk(line, " \t");
            if (!space) return Add(user, line);
            char* end = nullptr;
            long amount = strtol(space + 1, &end, 10);
            if (end == space + 1 || *end) return Add(user, line);
            *space = 0;
            bool known = false;
            if (strncmp(line, "Exp_", 4) == 0) {
                for (const char* currency : { "Exp_Koins", "Exp_SoulFragments", "Exp_BrutalityHearts", "Exp_PremiumCurrency" })
                    if (strcmp(line, currency) == 0) known = true;
            } else {
                game::NRSString slug(line);
                known = game::Fn<const void* (*)(const game::NRSString&)>(off::SpendableItemManager_GetItemDefinitionBySlug)(slug) != nullptr;
            }
            if (!known || amount <= 0 || amount > 100000000) {
                Log("give: %s %ld: unknown currency or item, or a bad amount", line, amount);
                return false;
            }
            long total = (long)wallet::Get(line) + amount;
            wallet::Set(line, total > 0x7fffffff ? 0x7fffffff : (int)total);
            Log("give: %s +%ld", line, amount);
            return true;
        }
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
            if (*line) (GiveLine(user, line) ? added : failed)++;
            line = next;
        }
        game::Free(buf);
        Log("gear: give list done, %d added, %d failed", added, failed);
    }
}
