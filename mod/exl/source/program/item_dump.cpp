#include "item_dump.hpp"
#include "game.hpp"
#include "log.hpp"
#include "nn/fs.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace nn::fs {
    void SetEnabledAutoAbort(bool enabled);
    Result GetEntryType(DirectoryEntryType* type, const char* path);
    Result DeleteFile(char const* path);
}

namespace mk11::items {

    namespace {
        constexpr const char* Path = "mk11sd:/mk11offline/items.txt";
        constexpr size_t DefinitionSize = 0x100;   // sizeof(FItemDefinition)

        // FItemObject, the base of every definition: vtable, FGuid at +8, internal name at +0x18.
        struct ItemObject {
            void* vtable;
            u32 guid[4];
            game::NRSString name;
        };

        // TArray<wchar_t> with 32-bit wchar_t, as returned by GetBaseLocalizedName.
        struct UniString {
            u32* data = nullptr;
            s32 num = 0;
            s32 max = 0;

            UniString() = default;
            UniString(const UniString&) = delete;
            ~UniString() {
                num = 0;
                game::Fn<void (*)(UniString*)>(off::TArrayBase_Dtor)(this);
            }
        };

        struct DefinitionArray {
            u8* data;
            s64 count;
        };

        struct Buffer {
            char* data = nullptr;
            size_t size = 0, cap = 0;

            ~Buffer() { if (data) game::Free(data); }

            void Printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
                char line[512];
                va_list va;
                va_start(va, fmt);
                int n = vsnprintf(line, sizeof(line), fmt, va);
                va_end(va);
                if (n <= 0) return;
                if ((size_t)n >= sizeof(line)) n = sizeof(line) - 1;
                if (size + n > cap) {
                    size_t newCap = cap ? cap * 2 : 1 << 20;
                    while (newCap < size + n) newCap *= 2;
                    char* p = static_cast<char*>(game::Malloc(newCap));
                    if (!p) return;
                    if (data) { memcpy(p, data, size); game::Free(data); }
                    data = p;
                    cap = newCap;
                }
                memcpy(data + size, line, n);
                size += n;
            }
        };

        void ToUtf8(const u32* s, s32 num, char* out, size_t size) {
            size_t n = 0;
            for (s32 i = 0; s && i < num && s[i] && n + 4 < size; i++) {
                u32 c = s[i];
                if (c == '\t' || c == '\n' || c == '\r') c = ' ';
                if (c < 0x80) out[n++] = c;
                else if (c < 0x800) { out[n++] = 0xC0 | c >> 6; out[n++] = 0x80 | (c & 0x3F); }
                else if (c < 0x10000) { out[n++] = 0xE0 | c >> 12; out[n++] = 0x80 | (c >> 6 & 0x3F); out[n++] = 0x80 | (c & 0x3F); }
                else { out[n++] = 0xF0 | c >> 18; out[n++] = 0x80 | (c >> 12 & 0x3F); out[n++] = 0x80 | (c >> 6 & 0x3F); out[n++] = 0x80 | (c & 0x3F); }
            }
            out[n] = 0;
        }

        // FName -> its text, read from the name table the way GetLocalizationSection does.
        const char* NameText(u64 name) {
            const uintptr_t base = exl::util::modules::GetTargetStart();
            auto entries = **reinterpret_cast<u8***>(base + off::Got_FNameEntries);
            u8* entry = entries ? reinterpret_cast<u8**>(entries)[static_cast<u32>(name)] : nullptr;
            return entry ? reinterpret_cast<const char*>(entry + 0x10) : "";
        }

        bool Exists() {
            nn::fs::DirectoryEntryType type;
            return R_SUCCEEDED(nn::fs::GetEntryType(&type, Path));
        }

        void Write(const Buffer& buf) {
            nn::fs::CreateFile(Path, 0);
            nn::fs::FileHandle h;
            if (R_SUCCEEDED(nn::fs::OpenFile(&h, Path, nn::fs::OpenMode_Write | nn::fs::OpenMode_Append))) {
                nn::fs::WriteFile(h, 0, buf.data, buf.size, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
                nn::fs::CloseFile(h);
            }
        }
    }

    void DumpIfMissing() {
        static bool s_Done = false;
        if (s_Done || !MountSd()) return;
        s_Done = true;
        nn::fs::SetEnabledAutoAbort(false);
        bool exists = Exists();
        nn::fs::SetEnabledAutoAbort(true);
        if (exists) return;

        auto defs = game::Fn<DefinitionArray (*)()>(off::InventoryManager_GetItemDefinitions)();
        Log("items: %lld definitions", (long long)defs.count);
        if (!defs.data || defs.count <= 0) return;

        Buffer buf;
        buf.Printf("# guid\tname\tcharacter\tslot\ttype\taugment\tdisabled\tdisplay name\n");
        for (s64 i = 0; i < defs.count; i++) {
            u8* def = defs.data + i * DefinitionSize;
            auto obj = reinterpret_cast<const ItemObject*>(def);

            const char* character = NameText(game::Fn<u64 (*)(const void*)>(off::ItemDefinition_GetCharacterKey)(def));
            auto slot = game::Fn<const ItemObject* (*)(const void*)>(off::SlotDefinitionHandle_GetReference)(def + 0x48);
            int type = game::Fn<u8 (*)(const void*)>(off::ItemDefinition_GetInventoryItemType)(def);
            bool augment = game::Fn<bool (*)(const void*)>(off::ItemDefinition_IsAugment)(def);
            bool disabled = game::Fn<bool (*)(const void*)>(off::ItemDefinition_IsDisabled)(def);

            char display[256];
            {
                UniString s = game::Fn<UniString (*)(const void*)>(off::ItemDefinition_GetBaseLocalizedName)(def);
                ToUtf8(s.data, s.num, display, sizeof(display));
            }

            buf.Printf("%08X%08X%08X%08X\t%s\t%s\t%s\t%d\t%d\t%d\t%s\n", obj->guid[0], obj->guid[1], obj->guid[2],
                       obj->guid[3], obj->name.c_str(), character, slot ? slot->name.c_str() : "", type, augment,
                       disabled, display);
        }

        nn::fs::SetEnabledAutoAbort(false);
        Write(buf);
        nn::fs::SetEnabledAutoAbort(true);
        Log("items: wrote %zu bytes to items.txt", buf.size);
    }

    void DumpInventory(void* user) {
        if (!user || !MountSd()) return;
        auto inv = game::Fn<u8* (*)(void*)>(off::FGUserEntity_GetInventoryData)(user);
        if (!inv) return;

        // TSet<TPair<HydraID, GearItemSaveData>>: element data at +0, count at +8, allocation
        // bit array at +0x20 (inline storage at +0x30 when that pointer is null).
        u8* set = inv + 0x18;
        u8* data = *reinterpret_cast<u8**>(set);
        s32 num = *reinterpret_cast<s32*>(set + 8);
        auto flags = *reinterpret_cast<const u32**>(set + 0x20);
        if (!flags) flags = reinterpret_cast<const u32*>(set + 0x30);

        Buffer buf;
        buf.Printf("# hydra id\tguid\tname\tcharacter\tdisplay name\n");
        int count = 0;
        for (s32 i = 0; data && i < num; i++) {
            if (!(flags[i >> 5] & (1u << (i & 31)))) continue;
            const u8* elem = data + i * 0xb8;
            const u32* guid = reinterpret_cast<const u32*>(elem + 0x44);
            char hydra[33];
            memcpy(hydra, elem, 32);
            hydra[32] = 0;
            const u8* def = game::Fn<const u8* (*)(u64, u64)>(off::InventoryManager_FindItemDefinition)(
                guid[0] | (u64)guid[1] << 32, guid[2] | (u64)guid[3] << 32);
            char display[256] = "";
            const char* name = "?";
            const char* character = "";
            if (def) {
                name = reinterpret_cast<const ItemObject*>(def)->name.c_str();
                character = NameText(game::Fn<u64 (*)(const void*)>(off::ItemDefinition_GetCharacterKey)(def));
                UniString s = game::Fn<UniString (*)(const void*)>(off::ItemDefinition_GetBaseLocalizedName)(def);
                ToUtf8(s.data, s.num, display, sizeof(display));
            }
            buf.Printf("%s\t%08X%08X%08X%08X\t%s\t%s\t%s\n", hydra, guid[0], guid[1], guid[2], guid[3], name, character, display);
            count++;
        }

        constexpr const char* InvPath = "mk11sd:/mk11offline/inventory.txt";
        nn::fs::SetEnabledAutoAbort(false);
        nn::fs::DeleteFile(InvPath);
        nn::fs::CreateFile(InvPath, 0);
        nn::fs::FileHandle h;
        if (R_SUCCEEDED(nn::fs::OpenFile(&h, InvPath, nn::fs::OpenMode_Write | nn::fs::OpenMode_Append))) {
            nn::fs::WriteFile(h, 0, buf.data, buf.size, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
            nn::fs::CloseFile(h);
        }
        nn::fs::SetEnabledAutoAbort(true);
        Log("items: %d inventory entries written to inventory.txt", count);
    }
}
