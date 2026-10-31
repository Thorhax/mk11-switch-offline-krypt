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

        void Apply(const char* key, const char* value) {
            if (strcmp(key, "krypt_currency_topup") == 0) s_TopUp = atoi(value);
            else if (strcmp(key, "apply_gear_rewards") == 0) s_ApplyGear = atoi(value) != 0;
            else if (strcmp(key, "skip_tower_credits") == 0) s_SkipTowerCredits = atoi(value) != 0;
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
            Log("config: krypt_currency_topup=%d apply_gear_rewards=%d skip_tower_credits=%d", s_TopUp, s_ApplyGear, s_SkipTowerCredits);
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

    bool SkipTowerCredits() {
        if (!s_Loaded) Load();
        return s_SkipTowerCredits;
    }
}
