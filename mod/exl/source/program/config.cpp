#include "config.hpp"
#include "lib.hpp"
#include "log.hpp"
#include "nn/fs.hpp"
#include <cstdlib>
#include <cstring>

namespace nn::fs {
    void SetEnabledAutoAbort(bool enabled);
}

namespace mk11::config {

    namespace {
        bool s_Loaded = false;
        int s_TopUp = 0;
        bool s_ApplyGear = false;
        bool s_SkipTowerCredits = true;
        bool s_OfflineStore = true;
        int s_ChestKrystals = 10;
        bool s_P1Cpu = false;
        bool s_DailyChallenges = false;

        void Apply(const char* key, const char* value) {
            if (strcmp(key, "krypt_currency_topup") == 0) s_TopUp = atoi(value);
            else if (strcmp(key, "apply_gear_rewards") == 0) s_ApplyGear = atoi(value) != 0;
            else if (strcmp(key, "skip_tower_credits") == 0) s_SkipTowerCredits = atoi(value) != 0;
            else if (strcmp(key, "offline_store") == 0) s_OfflineStore = atoi(value) != 0;
            else if (strcmp(key, "krypt_chest_krystals") == 0) s_ChestKrystals = atoi(value);
            else if (strcmp(key, "p1_cpu") == 0) s_P1Cpu = atoi(value) != 0;
            else if (strcmp(key, "daily_challenges") == 0) s_DailyChallenges = atoi(value) != 0;
        }

        void Load() {
            s_Loaded = true;
            if (!MountSd()) return;
            char buf[2048];
            u64 n = 0;
            nn::fs::FileHandle h;
            nn::fs::SetEnabledAutoAbort(false);
            if (R_SUCCEEDED(nn::fs::OpenFile(&h, "mk11sd:/mk11offline/config.ini", nn::fs::OpenMode_Read))) {
                s64 size = 0;
                nn::fs::GetFileSize(&size, h);
                n = size < (s64)sizeof(buf) - 1 ? size : sizeof(buf) - 1;
                nn::fs::ReadFile(h, 0, buf, n);
                nn::fs::CloseFile(h);
            }
            nn::fs::SetEnabledAutoAbort(true);
            buf[n] = 0;

            for (char* line = buf; line && *line;) {
                char* next = strchr(line, '\n');
                if (next) *next++ = 0;
                char* eq = strchr(line, '=');
                if (*line != '#' && eq) {
                    *eq = 0;
                    char* v = eq + 1;
                    for (char* e = v + strlen(v); e > v && (e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
                    Apply(line, v);
                }
                line = next;
            }
            Log("config: krypt_currency_topup=%d apply_gear_rewards=%d skip_tower_credits=%d offline_store=%d krypt_chest_krystals=%d p1_cpu=%d", s_TopUp,
                s_ApplyGear, s_SkipTowerCredits, s_OfflineStore, s_ChestKrystals, s_P1Cpu);
        }
    }

    bool ApplyGearRewards() {
        if (!s_Loaded) Load();
        return s_ApplyGear;
    }

    int KryptCurrencyTopUp() {
        if (!s_Loaded) Load();
        return s_TopUp;
    }

    bool OfflineStore() {
        if (!s_Loaded) Load();
        return s_OfflineStore;
    }

    bool DailyChallenges() {
        if (!s_Loaded) Load();
        return s_DailyChallenges;
    }

    bool P1Cpu() {
        if (!s_Loaded) Load();
        return s_P1Cpu;
    }

    int KryptChestKrystals() {
        if (!s_Loaded) Load();
        return s_ChestKrystals;
    }

    bool SkipTowerCredits() {
        if (!s_Loaded) Load();
        return s_SkipTowerCredits;
    }
}
