#include "lib.hpp"
#include "batch_apply.hpp"
#include "config.hpp"
#include "crash.hpp"
#include "game.hpp"
#include "gear.hpp"
#include "item_dump.hpp"
#include "krypt_store.hpp"
#include "log.hpp"
#include "mk11_offsets.hpp"
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
//  Wallet: MK11 keeps no offline balance for Koins, Souls and Hearts (their offline stat id is
//    unused), so while there is no Hydra connection their ExperienceView value lives in
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

    // OnlineHydraSSC (OnlineHydraBase, 0x108 bytes): +8 reference count, +64 request state (3 = complete), +0x48 OnlineError whose
    // +8 code is negative on failure, +0xe8 the response OnlineValueHandle.
    constexpr ptrdiff_t SscStateOffset = 64;
    constexpr ptrdiff_t SscErrorCodeOffset = 0x50;
    constexpr ptrdiff_t SscResponseOffset = 0xe8;
    constexpr s32 SscStateComplete = 3;
    constexpr int SscSize = 0x108;
    constexpr ptrdiff_t SscRefCountOffset = 8;

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
        void* user = game::Fn<void* (*)(int, unsigned)>(off::GetActiveModeUser)(0, 1);
        if (!user) { Log("Krypt save: no user for the server reply"); return; }
        const void* userHandle = game::Fn<const void* (*)(void*)>(off::FGUserEntity_GetUserHandle)(user);
        // Built like OnlineHydra::HydraObjectInit<OnlineHydraSSC>, which returns nothing while offline;
        // the request is not queued with OnlineHydra, so nothing ever sends it.
        auto ssc = static_cast<u8*>(game::Fn<void* (*)(int, int)>(off::IsolatedLongTermAllocator_Malloc)(SscSize, 8));
        if (!ssc) { Log("Krypt save: server reply allocation failed"); return; }
        game::Fn<void (*)(void*, const void*)>(off::OnlineHydraSSC_Ctor)(ssc, userHandle);
        __atomic_add_fetch(reinterpret_cast<s32*>(ssc + SscRefCountOffset), 1, __ATOMIC_SEQ_CST);
        out->value = ssc;

        const char* rewards = nullptr;
        size_t rewardsLen = 0;
        char* text = nullptr;
        if (batchJson && FindObject(batchJson, "\"user_data\":", &rewards, &rewardsLen)) {
            text = static_cast<char*>(malloc(rewardsLen + 48));
            if (text) snprintf(text, rewardsLen + 48, "{\"rewards\":{\"items\":[],\"user_data\":%.*s}}", (int)rewardsLen, rewards);
        }
        game::NRSString json(text ? text : "{}");
        free(text);
        game::OnlineValueHandle response = game::ParseFromJson(json);
        if (response.value) {
            auto slot = reinterpret_cast<game::OnlineValueHandle*>(ssc + SscResponseOffset);
            game::Fn<void (*)(game::OnlineValueHandle*)>(off::OnlineValueHandle_Dtor)(slot);
            slot->value = response.value;
            response.value = nullptr;
        }
        *reinterpret_cast<s32*>(ssc + SscErrorCodeOffset) = 0;
        *reinterpret_cast<s32*>(ssc + SscStateOffset) = SscStateComplete;
        Log("Krypt save: server reply %p (%s)", ssc, rewardsLen ? "with rewards" : "empty");
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
        auto ssc = static_cast<u8*>(handle ? handle->value : nullptr);
        if (ssc)
            Log("server reply check: %p state %d code %#x", ssc, *reinterpret_cast<s32*>(ssc + SscStateOffset),
                *reinterpret_cast<u32*>(ssc + SscErrorCodeOffset));
        else
            Log("server reply check: no request");
        bool ok = Orig(handle);
        Log("server reply check: %s", ok ? "success" : "failed");
        return ok;
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
