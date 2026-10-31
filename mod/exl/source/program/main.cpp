#include "lib.hpp"
#include "batch_apply.hpp"
#include "config.hpp"
#include "crash.hpp"
#include "game.hpp"
#include "gear.hpp"
#include "hydra_reply.hpp"
#include "item_dump.hpp"
#include "json.hpp"
#include "krypt_store.hpp"
#include "log.hpp"
#include "mk11_offsets.hpp"
#include "portals.hpp"
#include "system_dialogs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// MK11 offline Krypt.
//  Entry: GoingToKrypt's online-restriction, sign-in and inventory-sync checks are patched at
//    their call sites only, so other online features keep their normal checks. The Krypt
//    progression feed is reported as downloaded instead of waiting for Hydra.
//  Save/load: MK11KryptSaveRestore::Save and RestoreKryptData are replaced. Each Krypt save
//    object is serialised with the game's own OnlineValue JSON code into krypt_store, which
//    lives on the SD card, instead of being sent to Hydra.
//  Rewards: the inventory batch each save would have posted to Hydra is applied on the console
//    (batch_apply) and also kept in inventory_batches.jsonl.
//  Wallet: MK11 keeps no offline balance for Koins, Souls, Hearts or player/character/gear XP (their
//    offline profile stats stay 0), so while there is no Hydra connection their ExperienceView value lives in
//    krypt_store. Crafting materials and Konsumables (SpendableItemInventory) are kept only in
//    the Hydra inventory, so offline their counts live in krypt_store as well. Time Krystals
//    (Exp_PremiumCurrency) are never touched.
//  Optional: config.ini krypt_currency_topup=N tops Koins/Souls/Hearts up to N on Krypt start.

using namespace mk11;

namespace {

    // FGFeedObj layout (see FGFeedObj::IsFinishedDownloading / IsSuccessful).
    struct FeedStep { u8 pad[0x18]; s32 state; };
    struct FeedObj {
        u8 pad0[0x14];
        s32 activeStep;
        FeedStep** steps;
        s32 stepCount;
    };
    constexpr s32 FeedStepSucceeded = 3;

    // MK11Krypt caches the Soul balance here; a negative value makes GetCurrentSoulBalance re-read it.
    constexpr ptrdiff_t KryptSoulCacheOffset = 4216;
    // MK11Krypt's UIKryptHUDDriver*; RefreshWallet uses it without a null check.
    constexpr ptrdiff_t KryptHudOffset = 3360;

    void* s_Krypt = nullptr;  // the MK11Krypt instance, set when the Krypt initialises

    // ExperienceView (one per EExperienceType inside ExperienceUser): +0 owning FGUserEntity*,
    // +136 ExperienceInfo* whose +8 is the consumable name, e.g. "Exp_Koins".
    const char* ViewSlug(const void* view) {
        auto info = *reinterpret_cast<u8* const*>(reinterpret_cast<const u8*>(view) + 136);
        return info ? reinterpret_cast<const game::NRSString*>(info + 8)->c_str() : nullptr;
    }

    // The slug when this view's balance is kept in our wallet, else nullptr.
    const char* WalletSlug(const void* view) {
        const char* slug = ViewSlug(view);
        if (!slug || !wallet::Manages(slug)) return nullptr;
        void* user = *reinterpret_cast<void* const*>(view);
        if (!user || game::Fn<bool (*)(void*)>(off::FGUserEntity_IsConnectedToHydra)(user)) return nullptr;
        return slug;
    }

    // Shows wallet changes in the Krypt HUD.
    void RefreshKryptWallet() {
        if (!s_Krypt) return;
        *reinterpret_cast<s32*>(static_cast<u8*>(s_Krypt) + KryptSoulCacheOffset) = -1;
        if (!*reinterpret_cast<void**>(static_cast<u8*>(s_Krypt) + KryptHudOffset)) return;
        game::Fn<void (*)(void*)>(off::MK11Krypt_RefreshWallet)(s_Krypt);
    }

    // The object text that follows `"key":` in `json` ("{...}"), or false.
    bool FindObject(const char* json, const char* key, const char** start, size_t* len) {
        const char* p = strstr(json, key);
        if (!p) return false;
        p += strlen(key);
        if (*p != '{') return false;
        int depth = 0;
        bool inStr = false;
        for (const char* q = p; *q; q++) {
            if (inStr) { if (*q == '\\' && q[1]) q++; else if (*q == '"') inStr = false; continue; }
            if (*q == '"') inStr = true;
            else if (*q == '{') depth++;
            else if (*q == '}' && --depth == 0) { *start = p; *len = q + 1 - p; return true; }
        }
        return false;
    }

    // The Krypt shows chest and vase rewards from the Hydra response to its save
    // (GChestSSCHandle, read by MK11Krypt::ProcessPendingServerRewardRequest) and raises
    // ErrorContactingServer when that request did not succeed. So each save returns a completed,
    // successful OnlineHydraSSC whose response is {"rewards": {"items": [], "user_data": <the batch's
    // user_data>}}. UnlockTracker::AddUnlockMessageFromOnlinePayload reads the pop-up's items from
    // rewards.user_data.data.unlockTracker, as the server echoes them.
    void CompletedResponse(game::HydraHandle* out, const char* batchJson) {
        const char* rewards = nullptr;
        size_t rewardsLen = 0;
        char* text = nullptr;
        if (batchJson && FindObject(batchJson, "\"user_data\":", &rewards, &rewardsLen)) {
            text = static_cast<char*>(game::Malloc(rewardsLen + 48));
            if (text) snprintf(text, rewardsLen + 48, "{\"rewards\":{\"items\":[],\"user_data\":%.*s}}", (int)rewardsLen, rewards);
        }
        hydra::CompletedReply(out, text);
        game::Free(text);
        Log("Krypt save: server reply %p (%s)", out->value, rewardsLen ? "with rewards" : "empty");
    }

    // Store keys are "<background>/<save key>" so DLC backgrounds cannot collide.
    void MakeKey(char* out, size_t size, const game::NRSString& key) {
        snprintf(out, size, "%s/%s", game::KryptBackgroundName().c_str(), key.c_str());
    }
}

HOOK_DEFINE_REPLACE(KryptFeedWait) {
    static bool Callback(FeedObj* feed) {
        Log("Krypt feed: reporting %d step(s) as downloaded (%zu stored keys)", feed->stepCount, store::Count());
        for (s32 i = 0; i < feed->stepCount; i++)
            if (feed->steps[i]) feed->steps[i]->state = FeedStepSucceeded;
        if (feed->stepCount > 0) feed->activeStep = feed->stepCount - 1;
        return true;
    }
};

HOOK_DEFINE_REPLACE(KryptSave) {
    // Returns HydraHandle<OnlineHydraSSC> through x8, like the original.
    static game::HydraHandle Callback(const game::PtrArray<game::KryptSaveData>* objects, void* batch, const void* done) {
        store::Batch storeBatch;  // save objects, rewards and wallet: one write
        char key[256];
        for (s32 i = 0; i < objects->num; i++) {
            game::KryptSaveData* obj = objects->data[i];
            game::OnlineValueHandle value;
            game::NewOnlineValueMap(&value);
            obj->SaveTo(&value);
            game::NRSString json = game::ToJsonString(value.value);
            MakeKey(key, sizeof(key), obj->key);
            store::Put(key, json.c_str());
        }
        store::Flush();
        Log("Krypt save: %d object(s)", objects->num);

        if (char* replay = store::TakeReplayBatch()) {
            Log("Krypt save: applying replay_batch.json");
            batch::Apply(replay);
            game::Free(replay);
            RefreshKryptWallet();
        }

        game::HydraHandle response;
        bool responded = false;
        if (!game::Fn<bool (*)(void*)>(off::InventoryBatch_IsEmpty)(batch)) {
            game::OnlineValueHandle batchValue = game::Fn<game::OnlineValueHandle (*)(void*)>(off::InventoryBatch_ToOnlineValue)(batch);
            if (batchValue.value) {
                game::NRSString json = game::ToJsonString(batchValue.value);
                store::RecordBatch(json.c_str());
                Log("Krypt save: inventory batch %.300s", json.c_str());
                batch::Apply(json.c_str());
                RefreshKryptWallet();
                CompletedResponse(&response, json.c_str());
                responded = true;
            }
        }
        if (!responded) CompletedResponse(&response, nullptr);

        game::InvokeSaveCallback(done, true);
        return response;
    }
};

HOOK_DEFINE_REPLACE(KryptRestore) {
    static void Callback(const game::NRSString& saveKey, game::KryptSaveData& obj, const void* user) {
        char key[256];
        MakeKey(key, sizeof(key), saveKey);
        const char* json = store::Get(key);
        if (!json) return;
        game::NRSString text(json);
        game::OnlineValueHandle value = game::ParseFromJson(text);
        if (value.value) obj.LoadFrom(&value);
        else Log("Krypt restore: %s failed to parse", key);
    }
};

HOOK_DEFINE_TRAMPOLINE(WalletGet) {
    static s32 Callback(const void* view) {
        const char* slug = WalletSlug(view);
        return slug ? wallet::Get(slug) : Orig(view);
    }
};

HOOK_DEFINE_TRAMPOLINE(WalletSet) {
    static void Callback(void* view, s32 value) {
        if (const char* slug = WalletSlug(view)) wallet::Set(slug, value);
        Orig(view, value);
    }
};

// SpendableItemInventory: +0 FGUserEntity*. SpendableItemDefinition: +8 the item name.
HOOK_DEFINE_TRAMPOLINE(SpendableBalance) {
    static s32 Callback(const void* inventory, const void* def) {
        void* user = *reinterpret_cast<void* const*>(inventory);
        const char* slug = reinterpret_cast<const game::NRSString*>(static_cast<const u8*>(def) + 8)->c_str();
        if (user && slug && wallet::ManagesSpendable(slug) &&
            !game::Fn<bool (*)(void*)>(off::FGUserEntity_IsConnectedToHydra)(user))
            return wallet::Get(slug);
        return Orig(inventory, def);
    }
};

HOOK_DEFINE_TRAMPOLINE(KryptServerResponse) {
    static bool Callback(game::HydraHandle* handle) {
        auto ssc = handle ? handle->value : nullptr;
        if (ssc) {
            s32 state;
            u32 code;
            hydra::Describe(ssc, &state, &code);
            Log("server reply check: %p state %d code %#x", ssc, state, code);
        } else
            Log("server reply check: no request");
        bool ok = Orig(handle);
        Log("server reply check: %s", ok ? "success" : "failed");
        return ok;
    }
};

// The Krypt instance is forgotten when the Krypt shuts down, so wallet refreshes outside the Krypt
// (e.g. end-of-match rewards in towers) never touch a freed MK11Krypt.
HOOK_DEFINE_TRAMPOLINE(KryptCleanup) {
    static void Callback(void* krypt) {
        if (krypt == s_Krypt) s_Krypt = nullptr;
        Orig(krypt);
    }
};

HOOK_DEFINE_TRAMPOLINE(KryptDestroy) {
    static void Callback(void* krypt) {
        if (krypt == s_Krypt) s_Krypt = nullptr;
        Orig(krypt);
    }
};

// Inventory batches outside the Krypt (end-of-match rewards "match_completed", user actions, generic
// sends) all go through FGOnlineInventoryBatch::Post, which fails offline, so their Koins and items
// were lost. Offline, the batch (serialised through its virtual ToOnlineValue, vtable slot 2) is
// applied locally first. The original Post then reaches OnlineHydraSSC::Post, which never calls its
// completion callback offline; the game only commits the balances it showed ("pending experience")
// from that callback's response.rewards.items. So that one request is answered here: a completed
// reply listing the new wallet totals, passed to the callback as the server would.
namespace {
    bool s_AnswerBatchPost = false;

    // The current value of an experience type (player XP, a character's XP...), or false when `slug` is
    // not one. Offline the game keeps these in the profile stats (ExperienceView::GetExperience).
    bool ExperienceValue(void* user, const char* slug, int* value) {
        void* manager = *reinterpret_cast<void**>(exl::util::modules::GetTargetStart() + off::Got_ExperienceManager);
        game::NRSString name(slug);
        s32 type = game::Fn<s32 (*)(void*, const game::NRSString&)>(off::MK11ExperienceManager_GetExperienceType)(manager, name);
        if (type < 0) return false;
        void* experienceUser = game::Fn<void* (*)(void*)>(off::FGUserEntity_GetExperienceUser)(user);
        void* view = experienceUser ? game::Fn<void* (*)(void*, s32)>(off::ExperienceUser_GetExperienceView)(experienceUser, type) : nullptr;
        if (!view) return false;
        *value = game::Fn<s32 (*)(const void*)>(off::ExperienceView_GetExperience)(view);
        return true;
    }

    // {"rewards":{"items":[{"id","item_slug","count"}...]}} with the new totals the server would send:
    // the wallet totals of the Koins / Souls / Hearts the batch changed (already applied), and for
    // experience types (player, character and gear XP) the current value plus what the batch awards. The
    // game commits these through ExperienceView::OnInventoryUpdated, which offline stores XP in the profile
    // stats. Time Krystals are never included.
    char* BatchReply(void* user, const char* body) {
        json::Value* root = json::Parse(body);
        json::Value* list = json::Find(root, "rewards.inc_consumable");
        if (!list) list = json::Find(root, "inc_consumable");
        size_t size = 64;
        for (size_t i = 0; i < json::Count(list); i++) size += 128;
        char* out = static_cast<char*>(game::Malloc(size));
        int n = snprintf(out, size, "{\"rewards\":{\"items\":[");
        bool first = true;
        for (size_t i = 0; i < json::Count(list); i++) {
            const char* slug = json::String(json::Find(json::At(list, i), "item_slug"));
            if (!slug || !strcmp(slug, "Exp_PremiumCurrency")) continue;
            char quoted[96];
            snprintf(quoted, sizeof(quoted), "\"%s\"", slug);
            if (strstr(out, quoted)) continue;  // already listed
            int count;
            if (wallet::Manages(slug)) {
                count = wallet::Get(slug);
            } else {
                int current;
                if (!ExperienceValue(user, slug, &current)) continue;
                long long total = 0, amount;
                for (size_t j = 0; j < json::Count(list); j++) {
                    json::Value* entry = json::At(list, j);
                    const char* other = json::String(json::Find(entry, "item_slug"));
                    if (other && !strcmp(other, slug) && json::Integer(json::Find(entry, "inc_amount"), &amount)) total += amount;
                }
                count = current + (int)total;
                Log("experience: %s %d + %lld = %d", slug, current, total, count);
            }
            n += snprintf(out + n, size - n, "%s{\"id\":\"offline-%s\",\"item_slug\":%s,\"count\":%d}",
                          first ? "" : ",", slug, quoted, count);
            first = false;
        }
        snprintf(out + n, size - n, "]}}");
        json::Destroy(root);
        return out;
    }
}

HOOK_DEFINE_TRAMPOLINE(InventoryPost) {
    static game::HydraHandle Callback(void* batch, void* user, const game::NRSString* endpoint, void* done) {
        store::Batch storeBatch;  // the batch and the answered reply's wallet updates: one write
        bool offline = user && !game::Fn<bool (*)(void*)>(off::FGUserEntity_IsConnectedToHydra)(user);
        if (offline) {
            auto vtable = *reinterpret_cast<void* const* const*>(batch);
            game::OnlineValueHandle value = reinterpret_cast<game::OnlineValueHandle (*)(void*)>(vtable[2])(batch);
            if (value.value) {
                game::NRSString json = game::ToJsonString(value.value);
                store::RecordBatch(json.c_str());
                Log("inventory post %s: %.300s", endpoint->c_str(), json.c_str());
                batch::Apply(json.c_str(), false);
                RefreshKryptWallet();
            }
        }
        s_AnswerBatchPost = offline;
        game::HydraHandle reply = Orig(batch, user, endpoint, done);
        s_AnswerBatchPost = false;
        return reply;
    }
};

HOOK_DEFINE_TRAMPOLINE(HydraPost) {
    static game::HydraHandle Callback(const void* userHandle, const game::NRSString* endpoint, const game::OnlineValueHandle* body,
                                      const void* done, bool flag) {
        if (!s_AnswerBatchPost) return Orig(userHandle, endpoint, body, done, flag);
        s_AnswerBatchPost = false;
        char* bodyText = nullptr;
        if (body && body->value) {
            game::NRSString text = game::ToJsonString(body->value);
            bodyText = game::StrDup(text.c_str());
        }
        void* user = game::Fn<void* (*)(int, unsigned)>(off::GetActiveModeUser)(0, 1);
        char* replyText = BatchReply(user, bodyText ? bodyText : "{}");
        game::Free(bodyText);
        game::HydraHandle reply;
        hydra::CompletedReply(&reply, replyText);
        Log("inventory post %s answered: %.300s", endpoint->c_str(), replyText);
        game::Free(replyText);
        hydra::InvokeCompletion(done, &reply);
        return reply;
    }
};

// Story Mode: rewards for a story fight are only prepared while connected, then a proc
// (StoryRewardSSCProcData, the batch at +0x18) asks Hydra to grant them with RewardStorySequence and,
// once that succeeds, marks the sequence rewarded. Offline the rewards are prepared too, and the request
// applies the proc's batch locally and returns a completed reply.
namespace {
    constexpr ptrdiff_t StoryProcBatchOffset = 0x18;
}

// Story chapters finished offline were never rewarded ("you will not receive your rewards until you
// reconnect"); a story fight only asks for its reward the first time. When the story menu refreshes its
// server data (StoryInfo::UpdateServerProgressionData) offline, every completed sequence of the loaded
// story part without its reward gets it now: the game's own reward table for the sequence
// (GetRewardsLootTableForSequence) is applied like a reward batch and the sequence is marked rewarded.
namespace {
    constexpr ptrdiff_t StoryInfoFlowOffset = 4;             // StoryFlowType (story part) loaded
    constexpr ptrdiff_t StoryInfoSequenceCountOffset = 1504;  // sequences in the loaded reward table
}

namespace {
    void CatchUpStoryRewards(void* story, void* user) {
        if (story && user && !game::Fn<bool (*)(void*)>(off::FGUserEntity_IsConnectedToHydra)(user)) {
            s32 flow = static_cast<u8*>(story)[StoryInfoFlowOffset];
            s32 count = *reinterpret_cast<s32*>(static_cast<u8*>(story) + StoryInfoSequenceCountOffset);
            int granted = 0, completedCount = 0;
            store::Batch storeBatch;
            for (s32 seq = 0; seq < count && seq < 512; seq++) {
                auto completed = game::Fn<bool (*)(void*, s32, s32, u32)>(off::SaveDataUtils_HasCompletedStorySequence);
                if (!completed(user, flow, seq, 0) && !completed(user, flow, seq, 1)) continue;
                completedCount++;
                if (game::Fn<bool (*)(void*, s32, s32)>(off::SaveDataUtils_HasStorySequenceRewardBeenGiven)(user, flow, seq)) continue;
                alignas(16) u8 batchObject[0x400] = {};
                game::Fn<void (*)(void*)>(off::FGOnlineInventoryBatch_Ctor)(batchObject);
                if (game::Fn<bool (*)(void*, s32, void*)>(off::StoryInfo_GetRewardsLootTableForSequence)(story, seq, batchObject)) {
                    game::OnlineValueHandle value = game::Fn<game::OnlineValueHandle (*)(void*)>(off::InventoryBatch_ToOnlineValue)(batchObject);
                    if (value.value) {
                        game::NRSString json = game::ToJsonString(value.value);
                        store::RecordBatch(json.c_str());
                        Log("story: missed reward for part %d sequence %d: %.200s", flow, seq, json.c_str());
                        batch::Apply(json.c_str());
                        granted++;
                    }
                }
                game::Fn<void (*)(void*)>(off::FGOnlineInventoryBatch_Dtor)(batchObject);
                auto setGiven = game::Fn<void (*)(void*, s32, s32, bool)>(off::SaveDataUtils_SetStorySequenceRewardGiven);
                setGiven(user, flow, seq, true);
                setGiven(user, flow, seq, false);
            }
            if (count) Log("story: part %d: %d of %d sequences completed, %d missed reward(s) granted", flow, completedCount, count, granted);
            if (granted) game::Fn<void (*)(void*)>(off::FGSaveDataManager_SaveProgressionData)(user);
        }
    }
}

HOOK_DEFINE_TRAMPOLINE(StoryCatchUp) {
    static void Callback(void* story, void* user) {
        CatchUpStoryRewards(story, user);
        Orig(story, user);
    }
};

// Story fights: the reward table is loaded while a story part plays; missed rewards are caught up as
// the game sets up a fight's rewards (the fight's own reward then follows as usual).
HOOK_DEFINE_TRAMPOLINE(StoryFightRewards) {
    static void Callback(void* story, s32 sequence, bool flag) {
        Log("story: rewards for sequence %d (flag %d)", sequence, flag);
        CatchUpStoryRewards(story, game::Fn<void* (*)(int, unsigned)>(off::GetActiveModeUser)(0, 1));
        Orig(story, sequence, flag);
    }
};

// Chapter select: the story part's reward table may be loaded here (UpdateServerProgressionData only runs
// while online).
HOOK_DEFINE_TRAMPOLINE(ChapterSelectEnter) {
    static void Callback(void* driver) {
        Orig(driver);
        CatchUpStoryRewards(reinterpret_cast<void*>(exl::util::modules::GetTargetStart() + off::GFGStoryInfoStruct),
                            game::Fn<void* (*)(int, unsigned)>(off::GetActiveModeUser)(0, 1));
    }
};

HOOK_DEFINE_TRAMPOLINE(StoryReward) {
    static game::HydraHandle Callback(const void* user, s32 flow, s32 sequence, bool flag, s32 retries) {
        if (game::Fn<bool (*)(const void*)>(off::FGUserEntity_IsConnectedToHydra)(user)) return Orig(user, flow, sequence, flag, retries);
        store::Batch storeBatch;
        auto pdata = static_cast<u8*>(game::Fn<void* (*)(u32)>(off::GetCurrentProcPdata)(1));
        uintptr_t vtable = exl::util::modules::GetTargetStart() + off::Vtable_StoryRewardSSCProcData + 0x10;
        if (pdata && *reinterpret_cast<uintptr_t*>(pdata) == vtable) {
            game::OnlineValueHandle value = game::Fn<game::OnlineValueHandle (*)(void*)>(off::InventoryBatch_ToOnlineValue)(pdata + StoryProcBatchOffset);
            if (value.value) {
                game::NRSString json = game::ToJsonString(value.value);
                store::RecordBatch(json.c_str());
                Log("story: sequence %d rewards %.300s", sequence, json.c_str());
                batch::Apply(json.c_str());
            }
        } else {
            Log("story: sequence %d reward request without its batch", sequence);
        }
        game::HydraHandle reply;
        hydra::CompletedReply(&reply, nullptr);
        return reply;
    }
};

// Towers: the studio credits play (unskippably) after every character ending. Their two Movie_Play calls
// in the ladder ending script report "not started" instead, which skips them (config skip_tower_credits).
HOOK_DEFINE_TRAMPOLINE(MoviePlay) {
    static bool Callback(const char* name, u32 skipButtons, u32 bits2, void (*done)(), s32 index, u32 a5, u32 a6, s32 pad, u32 a8) {
        uintptr_t from = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - exl::util::modules::GetTargetStart();
        if ((from == off::LadderEnding_CreditsMovie1Return || from == off::LadderEnding_CreditsMovie2Return) &&
            config::SkipTowerCredits()) {
            Log("tower ending: credits movie %s skipped (skip_tower_credits=1)", name ? name : "?");
            return false;
        }
        return Orig(name, skipButtons, bits2, done, index, a5, a6, pad, a8);
    }
};

// Logs where in-game OK message boxes come from (to find offline warnings).
HOOK_DEFINE_TRAMPOLINE(OkMessageBox) {
    static u64 Callback(void* pad, const void* text, u32 flags, s32 group) {
        uintptr_t lr = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        Log("message box from main+%#lx", lr - exl::util::modules::GetTargetStart());
        return Orig(pad, text, flags, group);
    }
};

// Logs which code raised a Krypt critical error ("error contacting the server" kicks you out).
HOOK_DEFINE_TRAMPOLINE(KryptCriticalError) {
    static void Callback(const void* message, bool fatal) {
        uintptr_t lr = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        Log("Krypt critical error raised from main+%#lx (fatal %d)", lr - exl::util::modules::GetTargetStart(), fatal);
        Orig(message, fatal);
    }
};

HOOK_DEFINE_TRAMPOLINE(KryptInitialize) {
    static void Callback(void* krypt) {
        Orig(krypt);
        s_Krypt = krypt;
        store::Batch storeBatch;
        items::DumpIfMissing();
        void* user = game::Fn<void* (*)(int, unsigned)>(off::GetActiveModeUser)(0, 1);
        gear::ApplyGiveList(user);
        items::DumpInventory(user);
        if (int topUp = config::KryptCurrencyTopUp(); topUp > 0) {
            Log("Krypt start: topping currencies up to %d (config.ini)", topUp);
            for (const char* slug : { "Exp_Koins", "Exp_SoulFragments", "Exp_BrutalityHearts" })
                if (wallet::Get(slug) < topUp) wallet::Set(slug, topUp);
            RefreshKryptWallet();
        }
    }
};

extern "C" void exl_main(void* x0, void* x1) {
    exl::hook::Initialize();
    crash::Install();

    // GoingToKrypt: IsRestrictedFromEnteringOnlineFeature (needs an online-capable account) -> not restricted.
    exl::patch::CodePatcher(off::GoingToKrypt_IsRestrictedCall).Write<u32>(0x52800000);        // mov w0, #0
    // GoingToKrypt: "signed in to a Nintendo Account" check -> true.
    exl::patch::CodePatcher(off::GoingToKrypt_IsUserSignedInCall).Write<u32>(0x52800020);      // mov w0, #1
    // GoingToKrypt: inventory sync result -> 2 (synced).
    exl::patch::CodePatcher(off::GoingToKrypt_EnsureInventorySyncedCall).Write<u32>(0x52800040); // mov w0, #2
    // GoingToKlassicPortals: the same inventory sync result -> 2 (synced).
    exl::patch::CodePatcher(off::GoingToKlassic_EnsureInventorySyncedCall).Write<u32>(0x52800040); // mov w0, #2

    // Offline inventory prompts before Kustomize and fights: answer Yes (= continue offline, as the
    // player always must) without showing the box. ShowYesNoMsgBox returns 1 for Yes.
    for (ptrdiff_t call : { off::KustomizeSyncPromptCall, off::KustomizeDlcSyncPromptCall, off::FightingSyncPromptCall })
        exl::patch::CodePatcher(call).Write<u32>(0x52800020);                                  // mov w0, #1
    // Boot: skip the system error dialog (2123-0011) for the failed online endpoint lookup.
    exl::patch::CodePatcher(off::EndpointsShowErrorCall).Write<u32>(0xd503201f);               // nop
    system_dialogs::Install();
    portals::Install();
    InventoryPost::InstallAtOffset(off::FGOnlineInventoryBatch_Post);
    HydraPost::InstallAtOffset(off::OnlineHydraSSC_Post);
    MoviePlay::InstallAtOffset(off::MoviePlayer_Movie_Play);
    StoryReward::InstallAtOffset(off::MK11HydraSSCInterface_RewardStorySequence);
    StoryCatchUp::InstallAtOffset(off::StoryInfo_UpdateServerProgressionData);
    ChapterSelectEnter::InstallAtOffset(off::UIChapterSelectHUBDriver_Enter);
    StoryFightRewards::InstallAtOffset(off::StoryInfo_SetupAndShowRewardsForFight);
    OkMessageBox::InstallAtOffset(off::ShowOKMsgBox);
    // Story fight rewards are prepared offline too (StoryReward grants them).
    exl::patch::CodePatcher(off::StoryRewards_IsConnectedCall).Write<u32>(0x52800020);         // mov w0, #1
    KryptCleanup::InstallAtOffset(off::MK11Krypt_CleanupKryptCommon);
    KryptDestroy::InstallAtOffset(off::MK11Krypt_Dtor);

    KryptFeedWait::InstallAtOffset(off::KryptFeed_WaitForFinishWithCancel);
    KryptSave::InstallAtOffset(off::KryptSaveRestore_Save);
    KryptRestore::InstallAtOffset(off::KryptSaveRestore_Restore);
    KryptInitialize::InstallAtOffset(off::MK11Krypt_InitializeKrypt);
    WalletGet::InstallAtOffset(off::ExperienceView_GetExperience);
    WalletSet::InstallAtOffset(off::ExperienceView_OnInventoryUpdated);
    KryptCriticalError::InstallAtOffset(off::MK11KryptCriticalError_Display);
    KryptServerResponse::InstallAtOffset(off::Krypt_WaitForServerResponse);
    SpendableBalance::InstallAtOffset(off::SpendableItemInventory_GetCurrentBalance);
}

extern "C" NORETURN void exl_exception_entry() {
    EXL_ABORT("Default exception handler called!");
}
