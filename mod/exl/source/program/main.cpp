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
//    krypt_store. Time Krystals (Exp_PremiumCurrency) are never touched.
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

        if (!game::Fn<bool (*)(void*)>(off::InventoryBatch_IsEmpty)(batch)) {
            game::OnlineValueHandle batchValue = game::Fn<game::OnlineValueHandle (*)(void*)>(off::InventoryBatch_ToOnlineValue)(batch);
            if (batchValue.value) {
                game::NRSString json = game::ToJsonString(batchValue.value);
                store::RecordBatch(json.c_str());
                Log("Krypt save: inventory batch %.300s", json.c_str());
                batch::Apply(json.c_str());
                RefreshKryptWallet();
            }
        }

        game::InvokeSaveCallback(done, true);
        return {};
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
}

extern "C" NORETURN void exl_exception_entry() {
    EXL_ABORT("Default exception handler called!");
}
