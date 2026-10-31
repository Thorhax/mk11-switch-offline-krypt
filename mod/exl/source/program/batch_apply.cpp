#include "batch_apply.hpp"
#include "config.hpp"
#include "game.hpp"
#include "gear.hpp"
#include "krypt_store.hpp"
#include "log.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mk11::wallet {

    namespace {
        void Key(char* out, size_t size, const char* slug) { snprintf(out, size, "wallet/%s", slug); }
    }

    bool Manages(const char* slug) {
        return strcmp(slug, "Exp_Koins") == 0 || strcmp(slug, "Exp_SoulFragments") == 0 ||
               strcmp(slug, "Exp_BrutalityHearts") == 0;
    }

    bool ManagesSpendable(const char* slug) { return strncmp(slug, "Exp_", 4) != 0; }

    int Get(const char* slug) {
        char key[64];
        Key(key, sizeof(key), slug);
        const char* v = store::Get(key);
        return v ? atoi(v) : 0;
    }

    void Set(const char* slug, int value) {
        if (value < 0) value = 0;
        char key[64], text[16];
        Key(key, sizeof(key), slug);
        snprintf(text, sizeof(text), "%d", value);
        store::Put(key, text);
        store::Flush();
        Log("wallet: %s = %d", slug, value);
    }
}

namespace mk11::batch {

    namespace {
        // Minimal scanning over the compact JSON the game produces for a batch:
        // finds `"key":[ {...}, {...} ]` and hands each object's text to `fn`.
        template<typename F>
        void ForEachObject(const char* json, const char* key, F fn) {
            char pat[64];
            snprintf(pat, sizeof(pat), "\"%s\":[", key);
            const char* p = strstr(json, pat);
            if (!p) return;
            p += strlen(pat);
            while (*p && *p != ']') {
                if (*p != '{') { p++; continue; }
                const char* start = p;
                int depth = 0;
                bool inStr = false;
                for (; *p; p++) {
                    if (inStr) { if (*p == '\\' && p[1]) p++; else if (*p == '"') inStr = false; continue; }
                    if (*p == '"') inStr = true;
                    else if (*p == '{') depth++;
                    else if (*p == '}' && --depth == 0) { p++; break; }
                }
                fn(start, p);
            }
        }

        bool StringField(const char* obj, const char* end, const char* name, char* out, size_t size) {
            char pat[64];
            snprintf(pat, sizeof(pat), "\"%s\":\"", name);
            const char* p = strstr(obj, pat);
            if (!p || p >= end) return false;
            p += strlen(pat);
            size_t n = 0;
            while (p < end && *p != '"' && n + 1 < size) out[n++] = *p++;
            out[n] = 0;
            return true;
        }

        bool IntField(const char* obj, const char* end, const char* name, long long* out) {
            char pat[64];
            snprintf(pat, sizeof(pat), "\"%s\":", name);
            const char* p = strstr(obj, pat);
            if (!p || p >= end) return false;
            *out = strtoll(p + strlen(pat), nullptr, 10);
            return true;
        }

        bool IsEmptyArray(const char* json, const char* key) {
            char pat[64];
            snprintf(pat, sizeof(pat), "\"%s\":[]", key);
            return strstr(json, pat) != nullptr;
        }
    }

    void Apply(const char* json) {
        ForEachObject(json, "inc_consumable", [](const char* o, const char* e) {
            char slug[64];
            long long amount = 0;
            if (!StringField(o, e, "item_slug", slug, sizeof(slug)) || !IntField(o, e, "inc_amount", &amount)) return;
            if (!wallet::Manages(slug) && !wallet::ManagesSpendable(slug)) {
                Log("batch: consumable %s %+lld left to the game", slug, amount);
                return;
            }
            wallet::Set(slug, wallet::Get(slug) + (int)amount);
        });

        void* user = game::Fn<void* (*)(int, unsigned)>(off::GetActiveModeUser)(0, 1);
        void* unlockables = reinterpret_cast<void*>(exl::util::modules::GetTargetStart() + off::GMK11Unlockables);
        ForEachObject(json, "game_unlockables", [&](const char* o, const char* e) {
            long long page = 0, bit = 0;
            if (!user || !IntField(o, e, "page_num", &page) || !IntField(o, e, "unlock_bit", &bit)) return;
            bool changed = game::Fn<bool (*)(void*, void*, unsigned, unsigned)>(off::MK11Unlockables_UnlockPageBit)(
                unlockables, user, (unsigned)bit, (unsigned)page);
            Log("batch: unlock page %lld bit %#llx -> %s", page, bit, changed ? "unlocked" : "already set");
        });

        ForEachObject(json, "add_game_item", [&](const char* o, const char* e) {
            char slug[64];
            if (!user || !StringField(o, e, "item_slug", slug, sizeof(slug))) return;
            if (!config::ApplyGearRewards()) {
                Log("batch: gear %s kept for later (apply_gear_rewards=0)", slug);
                return;
            }
            gear::Add(user, slug);
        });

        for (const char* kind : { "unlock_item", "lock_item", "create_instance", "update_data" })
            if (!IsEmptyArray(json, kind)) Log("batch: '%s' entries not applied yet (kept in inventory_batches.jsonl)", kind);
    }
}
