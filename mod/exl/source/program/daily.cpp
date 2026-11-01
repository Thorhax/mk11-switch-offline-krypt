#include "lib.hpp"
#include "game.hpp"
#include "log.hpp"
#include "daily.hpp"
#include "batch_apply.hpp"
#include "hydra_reply.hpp"
#include "json.hpp"
#include "krypt_store.hpp"
#include "config.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Daily Challenges offline. Hydra sends the day's challenges ("daily_challenge_get"): entries {id, data:
// {type, key, current, goal, data: {goal, currencyType, currencyAmount, stars}}}, type being the challenge class
// (a fighting stat, a profile stat or playing as a character), key the stat (or character). The game tracks
// them (DailyChallengeObject), uploads progress ("daily_challenge_update", {challenges: [...]}) and asks for
// the reward when one is done ("daily_challenge_reward", {challengeId, reward: <inventory batch>}).
// Offline the module picks three challenges a day (console clock) and hands them to
// DailyChallengeSystem::RefreshDailyChallenges on the main menu; progress is kept in krypt.dat
// (daily/<id>) and the reward batch is applied to the wallet.
//
// At the end of a match with a human player the game checks each challenge (DailyChallengeSystem::
// UpdateChallenges -> DailyChallengeObject::DailyChallengeUpdate -> ChallengeInfoUpdate), which only queues an
// increment (OnlineDailyChallengeInfo::QueueIncrementCommand) for the next upload; offline nothing uploads, so
// the queued increments are applied here: progress (info +32, goal +36) is kept in krypt.dat and a challenge
// that reaches its goal pays its Koins once.

using namespace mk11;

extern "C" u32 SdkUserClockGetCurrentTime(s64* posixTime) asm("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE");

namespace {
    struct Challenge {
        const char* type;
        const char* key;
        int goal;
        int koins;
    };
    constexpr const char* Fighting = "DailyOnlineFightingStatChallengeInfo";
    // Stats whose challenge text the game has (others read "BUG_ME": TOTAL_WINS, TOTAL_FLAWLESS_ROUNDS, TOTAL_COMBOS,
    // TOTAL_JUMP_ATTACKS, TOTAL_PROJECTILES_FIRED).
    constexpr Challenge Pool[] = {
        { Fighting, "TOTAL_SPECIALS_USED", 25, 1000 },
        { Fighting, "KRUSHING_BLOWS_PERFORMED", 3, 1500 },
        { Fighting, "TOTAL_SUPERMOVES", 2, 1500 },
        { Fighting, "TOTAL_THROWS", 5, 1000 },
        { Fighting, "TOTAL_UPPERCUTS", 5, 1000 },
        { Fighting, "TOTAL_BRUTALITIES", 2, 2000 },
        { Fighting, "TOTAL_FATALITIES", 2, 2000 },
    };
    constexpr int PoolSize = sizeof(Pool) / sizeof(Pool[0]);
    constexpr int PerDay = 3;
    constexpr int KoinsType = 1;  // EExperienceType (research: logged below)

    s64 Today() {
        s64 seconds = 0;
        if (SdkUserClockGetCurrentTime(&seconds) != 0 || seconds < 0) seconds = 0;
        return seconds / 86400;
    }

    // The day's challenges: PerDay different pool entries.
    void Pick(s64 day, int* out) {
        u64 x = (u64)day * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
        int n = 0;
        while (n < PerDay) {
            x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
            int pick = (int)(x % PoolSize);
            bool seen = false;
            for (int i = 0; i < n; i++) seen |= out[i] == pick;
            if (!seen) out[n++] = pick;
        }
    }

    void Key(char* out, size_t size, const char* id, const char* field) { snprintf(out, size, "daily/%s%s", id, field); }

    // The day's challenge entries ({id, data}), one JSON object each; `fn` gets each.
    template<typename F>
    void ForEachEntry(s64 day, F fn) {
        int picks[PerDay];
        Pick(day, picks);
        for (int i = 0; i < PerDay; i++) {
            const Challenge& c = Pool[picks[i]];
            char id[48], key[80], entry[512];
            snprintf(id, sizeof(id), "offline-daily-%lld-%d", (long long)day, i);
            Key(key, sizeof(key), id, "/done");
            if (store::Get(key)) continue;  // rewarded today
            Key(key, sizeof(key), id, "");
            const char* saved = store::Get(key);
            int current = saved ? atoi(saved) : 0;
            snprintf(entry, sizeof(entry),
                     "{\"id\":\"%s\",\"data\":{\"type\":\"%s\",\"key\":\"%s\",\"current\":%d,\"goal\":%d,"
                     "\"data\":{\"goal\":%d,\"currencyType\":%d,\"currencyAmount\":%d,\"stars\":1}}}",
                     id, c.type, c.key, current, c.goal, c.goal, KoinsType, c.koins);
            fn(entry);
        }
    }

    bool Offline(void* user) { return user && !game::Fn<bool (*)(void*)>(off::FGUserEntity_IsConnectedToHydra)(user); }

    // As the "daily_challenge_get" reply handler does: the user's challenge objects are replaced by one per entry,
    // each made from a DailyChallengeGOSData (vtable +0, id +8, DailyChallengeSSCData +0x18) read from the entry.
    constexpr ptrdiff_t GosDataVtableRef = 0xabb6298;  // -> DailyChallengeGOSData vtable (- 0x10)
    constexpr ptrdiff_t GosDataDtor = 0x1e57e94;       // DailyChallengeGOSData::~DailyChallengeGOSData() (complete)

    void Refresh(void* user, s64 day) {
        u8* challenges = static_cast<u8*>(game::Fn<void* (*)(void*)>(off::FGUserEntity_GetDailyChallengeArray)(user));
        void* system = game::Fn<void* (*)()>(off::GetDailyChallengeSystem)();
        if (!challenges || !system) return;
        s32& count = *reinterpret_cast<s32*>(challenges + 32);
        u8* items = *reinterpret_cast<u8**>(challenges + 24);
        for (s32 i = 0; i < count; i++) game::Fn<void (*)(u8*)>(off::DailyChallengeObjectPtr_Destroy)(items + i * 16);
        count = 0;

        uintptr_t base = exl::util::modules::GetTargetStart();
        int made = 0;
        ForEachEntry(day, [&](const char* entry) {
            alignas(16) u8 gos[0x80] = {};
            *reinterpret_cast<uintptr_t*>(gos) = *reinterpret_cast<const uintptr_t*>(base + GosDataVtableRef) + 0x10;
            game::Fn<void (*)(u8*)>(off::DailyChallengeSSCData_Ctor)(gos + 0x18);
            {
                game::NRSString text(entry);
                game::Fn<bool (*)(u8*, const game::NRSString&)>(off::DailyChallengeGOSData_FromJsonString)(gos, text);
            }
            game::Fn<void (*)(void*, void*, u8*)>(off::DailyChallengeSystem_CreateChallengesForUser)(system, user, gos);
            game::Fn<void (*)(u8*)>(GosDataDtor)(gos);
            Log("daily: %s", entry);
            made++;
        });
        Log("daily: day %lld, %d challenge(s), user has %d", (long long)day, made, count);
        static bool s_Logged = false;
        if (!s_Logged) {
            s_Logged = true;
            for (int t = 0; t <= 6; t++)
                Log("daily: experience type %d = %s", t, game::Fn<const char* (*)(int)>(off::GetExperienceSlugFromType)(t));
        }
    }
}

namespace {
    // DailyChallengeObject: challenge info at +24, DailyChallengeGOSData (pimpl) at +80 with the id string at +8.
    // OnlineDailyChallengeInfo: current +32, goal +36, queued increments (TArray<int>) at +56 / count +64.
    const char* ChallengeId(u8* object) {
        u8* gos = *reinterpret_cast<u8**>(object + 80);
        return gos ? reinterpret_cast<const game::NRSString*>(gos + 8)->c_str() : "";
    }

    // The Koins for challenge "offline-daily-<day>-<slot>".
    int Reward(const char* id) {
        if (strncmp(id, "offline-daily-", 14) != 0) return 0;
        char* end = nullptr;
        long long day = strtoll(id + 14, &end, 10);
        if (!end || *end != '-') return 0;
        long slot = strtol(end + 1, nullptr, 10);
        if (slot < 0 || slot >= PerDay) return 0;
        int picks[PerDay];
        Pick(day, picks);
        return Pool[picks[slot]].koins;
    }
}

HOOK_DEFINE_TRAMPOLINE(ChallengeUpdate) {
    static void Callback(u8* object, void* player) {
        Orig(object, player);
        u8* info = *reinterpret_cast<u8**>(object + 24);
        const char* id = ChallengeId(object);
        if (!info || strncmp(id, "offline-daily-", 14) != 0) return;
        s32& queued = *reinterpret_cast<s32*>(info + 64);
        const s32* increments = *reinterpret_cast<s32**>(info + 56);
        s32& current = *reinterpret_cast<s32*>(info + 32);
        s32 goal = *reinterpret_cast<s32*>(info + 36);
        if (queued <= 0 || !increments) return;
        int before = current;
        for (s32 i = 0; i < queued; i++) current += increments[i];
        queued = 0;
        if (current > goal) current = goal;
        char key[80], value[24];
        store::Batch storeBatch;
        Key(key, sizeof(key), id, "");
        snprintf(value, sizeof(value), "%d", current);
        store::Put(key, value);
        Log("daily: %s %d -> %d / %d", id, before, current, goal);
        Key(key, sizeof(key), id, "/done");
        if (current >= goal && !store::Get(key)) {
            int koins = Reward(id);
            store::Put(key, "1");
            wallet::Set("Exp_Koins", wallet::Get("Exp_Koins") + koins);
            Log("daily: %s completed, %d Koins", id, koins);
        }
    }
};

// The main menu loop runs whenever the main menu is up: the challenges are (re)loaded once a day.
HOOK_DEFINE_TRAMPOLINE(MainMenuLoop) {
    static void Callback(void* vm) {
        static s64 s_Day = -1;
        s64 day = Today();
        void* user = game::Fn<void* (*)(int, unsigned)>(off::GetActiveModeUser)(0, 1);
        if (day != s_Day && Offline(user) && config::DailyChallenges()) {
            s_Day = day;
            Refresh(user, day);
        }
        Orig(vm);
    }
};

namespace mk11::daily {
    bool AnswerPost(const char* endpoint, const game::OnlineValueHandle* body, const void* done, game::HydraHandle* reply) {
        bool update = strcmp(endpoint, "daily_challenge_update") == 0, reward = strcmp(endpoint, "daily_challenge_reward") == 0;
        if (!update && !reward) return false;
        void* user = game::Fn<void* (*)(int, unsigned)>(off::GetActiveModeUser)(0, 1);
        if (!Offline(user)) return false;
        char* text = nullptr;
        if (body && body->value) {
            game::NRSString s = game::ToJsonString(body->value);
            text = game::StrDup(s.c_str());
        }
        Log("daily: %s %.400s", endpoint, text ? text : "");
        json::Value* root = text ? json::Parse(text) : nullptr;
        store::Batch storeBatch;
        if (update) {
            json::Value* list = json::Find(root, "challenges");
            for (size_t i = 0; i < json::Count(list); i++) {
                json::Value* e = json::At(list, i);
                const char* id = json::String(json::Find(e, "id"));
                long long current = 0;
                if (!id || !json::Integer(json::Find(e, "data.current"), &current)) continue;
                char key[80], value[24];
                Key(key, sizeof(key), id, "");
                snprintf(value, sizeof(value), "%lld", current);
                store::Put(key, value);
            }
        } else {
            const char* id = json::String(json::Find(root, "challengeId"));
            if (json::Value* batch = json::Find(root, "reward")) {
                char* batchText = json::Serialize(batch);
                batch::Apply(batchText);
                std::free(batchText);
            }
            if (id) {
                char key[80];
                Key(key, sizeof(key), id, "/done");
                store::Put(key, "1");
            }
        }
        json::Destroy(root);
        game::Free(text);
        hydra::CompletedReply(reply, "{}");
        hydra::InvokeCompletion(done, reply);
        return true;
    }

    void Install() {
        MainMenuLoop::InstallAtOffset(off::MKS_MainMenu_MainMenuLoop);
        ChallengeUpdate::InstallAtOffset(off::DailyChallengeObject_DailyChallengeUpdate);
        // The Kombat Kard (ZL, where the challenges are) loads the Nintendo network service account data first
        // (OnlineNX64UserData::LoadNSAData), which fails offline: "You must be connected..." and the Kard
        // closes. Offline it carries on as if the data had loaded.
        constexpr ptrdiff_t KombatKardNsaCheck = 0x20cb494;  // PlayerProfileModalDriver::RunInputLoop: tbz w0, #0
        const u32* code = reinterpret_cast<const u32*>(exl::util::modules::GetTargetStart() + KombatKardNsaCheck);
        if (*code == 0x360004c0) exl::patch::CodePatcher(KombatKardNsaCheck).Write<u32>(0xd503201f);  // nop
        else Log("daily: unexpected code at the Kombat Kard check, left alone");
        // ...then waits on "Connecting to server..." until OnlineHydra::IsConnected; the first check reads connected.
        constexpr ptrdiff_t KombatKardConnected = 0x20cb620;  // mov w22, w0 (IsConnected's result)
        code = reinterpret_cast<const u32*>(exl::util::modules::GetTargetStart() + KombatKardConnected);
        if (*code == 0x2a0003f6) exl::patch::CodePatcher(KombatKardConnected).Write<u32>(0x52800036);  // mov w22, #1
        else Log("daily: unexpected code at the Kombat Kard connection check, left alone");
    }
}
