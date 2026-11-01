#include "lib.hpp"
#include "game.hpp"
#include "batch_apply.hpp"
#include "log.hpp"
#include "shop.hpp"
#include "config.hpp"
#include "nn/fs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nn::fs {
    void SetEnabledAutoAbort(bool enabled);
}

// The Store (ZR) offline.
//
// The Kombat Feed's Premium Shop page shows MK11PremiumShop's catalogue, which Hydra normally sends:
// {mTopSection: [9 slots], mBottomSection: [1 slot]}, each slot {mAvailabilities: [{mReward: <loot
// table>, mStartTime, mEndTime}]} (several availabilities = a quantity choice). Offline the module
// hands MK11PremiumShop::Initialize its own catalogue of items the game also gives out for playing
// (Towers of Time keys, Konsumables, materials), priced in Time Krystals. MK11PremiumShop::PurchaseProduct
// builds an inventory batch {Exp_PremiumCurrency -price, rewards}, which is applied like every other
// offline batch (Time Krystals live in the offline wallet). Nothing paid is sold.

using namespace mk11;

namespace {
    struct DateTimeValue {
        alignas(8) u8 bytes[0x10];
        ~DateTimeValue() {}
    };

    constexpr int TopSlots = 9;     // MK11PremiumShop::IsDataValid wants exactly 9 + 1 slots
    constexpr int BottomSlots = 1;
    constexpr int MaxChoices = 3;

    struct Offer {
        const char* slug;           // spendable item (currency loot items have no shop price)
        int amount[MaxChoices];     // 0 ends the list
        int price[MaxChoices];      // Time Krystals
    };

    // Slots 0-8 are the top section, slot 9 the bottom one. sdmc:/mk11offline/store.txt replaces it.
    Offer s_Offers[TopSlots + BottomSlots] = {
        { "PortalKoins1Key",     { 1, 5, 10 },  { 10, 50, 100 } },
        { "PortalSouls1Key",     { 1, 5, 10 },  { 10, 50, 100 } },
        { "PortalHearts1Key",    { 1, 5, 10 },  { 10, 50, 100 } },
        { "PortalXP1Key",        { 1, 5, 10 },  { 10, 50, 100 } },
        { "PortalForgeMatsKey",  { 1, 5, 10 },  { 10, 50, 100 } },
        { "PortalKollectorKey",  { 1, 5, 10 },  { 10, 50, 100 } },
        { "PortalFireKey",       { 1, 5, 10 },  { 10, 50, 100 } },
        { "Armor",               { 1, 5, 10 },  { 10, 50, 100 } },
        { "SkipFight",           { 1, 5, 10 },  { 10, 50, 100 } },
        { "SkeletonKey",         { 1, 5, 10 },  { 10, 50, 100 } },
    };

    // store.txt: one line per slot (the first 9 are the top section, the 10th the bottom one):
    //   <item slug> <amount>:<price> [<amount>:<price> ...]
    // Used only when it has all 10 lines; slugs live in s_StoreText.
    char* s_StoreText = nullptr;

    void LoadStoreFile() {
        constexpr const char* Path = "mk11sd:/mk11offline/store.txt";
        static bool loaded = false;
        if (loaded || !MountSd()) return;
        loaded = true;
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
        }
        nn::fs::SetEnabledAutoAbort(true);
        if (!buf) return;

        Offer offers[TopSlots + BottomSlots] = {};
        int count = 0;
        for (char* line = buf; line && *line;) {
            char* next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (char* hash = strchr(line, '#')) *hash = 0;
            char* save = nullptr;
            char* slug = strtok_r(line, " \t\r", &save);
            if (slug && count < TopSlots + BottomSlots) {
                Offer& offer = offers[count];
                offer.slug = slug;
                int c = 0;
                for (char* tok; c < MaxChoices && (tok = strtok_r(nullptr, " \t\r", &save));) {
                    char* colon = strchr(tok, ':');
                    if (!colon) continue;
                    offer.amount[c] = atoi(tok);
                    offer.price[c] = atoi(colon + 1);
                    if (offer.amount[c] > 0 && offer.price[c] > 0) c++;
                    else offer.amount[c] = 0;
                }
                if (c) count++;
                else Log("shop: store.txt: no amount:price for %s", slug);
            }
            line = next;
        }
        if (count != TopSlots + BottomSlots) {
            Log("shop: store.txt has %d usable lines, needs %d; using the built-in catalogue", count, TopSlots + BottomSlots);
            game::Free(buf);
            return;
        }
        memcpy(s_Offers, offers, sizeof(offers));
        s_StoreText = buf;
        Log("shop: catalogue from store.txt");
    }

    // TArray<MK11PremiumShopProduct>; a product is a TArray<MK11PremiumShopAvailability> (24 bytes each).
    struct Product {
        u8* availabilities;
        s32 num;
        s32 max;
    };
    struct ProductArray {
        Product* data;
        s32 num;
        s32 max;
    };
    constexpr size_t AvailabilitySize = 0x18;

    template<typename T>
    T& Global(ptrdiff_t offset) { return *reinterpret_cast<T*>(exl::util::modules::GetTargetStart() + offset); }

    void* User() { return game::Fn<void* (*)(int, unsigned)>(off::GetActiveModeUser)(0, 1); }

    bool Offline() {
        void* user = User();
        return user && !game::Fn<bool (*)(void*)>(off::FGUserEntity_IsConnectedToHydra)(user);
    }

    void FormatDateTime(s64 ms, char* out, size_t size) {
        s64 seconds = ms / 1000;
        s64 days = seconds / 86400, rest = seconds % 86400;
        if (rest < 0) { rest += 86400; days--; }
        days += 719468;
        s64 era = (days >= 0 ? days : days - 146096) / 146097;
        s64 doe = days - era * 146097;
        s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        s64 mp = (5 * doy + 2) / 153;
        s64 day = doy - (153 * mp + 2) / 5 + 1;
        s64 month = mp < 10 ? mp + 3 : mp - 9;
        s64 year = yoe + era * 400 + (month <= 2);
        snprintf(out, size, "\"%04d-%02d-%02dT%02d:%02d:%02d+0000\"", (int)(year % 10000), (int)month, (int)day,
                 (int)(rest / 3600), (int)(rest / 60 % 60), (int)(rest % 60));
    }

    // The loot JSON for `amount` of an offer's item, or false when the item does not exist.
    // premiumCurrencyPrice is the Time Krystal price (MK11PremiumShop::IsDataValid wants it non-zero).
    bool RewardJson(const Offer& offer, int amount, int price, char* out, size_t size) {
        game::NRSString slug(offer.slug);
        auto def = game::Fn<const u8* (*)(const game::NRSString&)>(off::SpendableItemManager_GetItemDefinitionBySlug)(slug);
        if (!def) {
            Log("shop: no spendable item %s", offer.slug);
            return false;
        }
        snprintf(out, size, "{\"name\":\"OfflineStore\",\"dropCount\":1,\"items\":[{\"LootItemType\":\"SpendableItemLootItem\",\"dropWeight\":1,\"id\":%u,\"amount\":%d,\"premiumCurrencyPrice\":%d}]}",
                 *reinterpret_cast<const u32*>(def), amount, price);
        return true;
    }

    // Hands MK11PremiumShop the offline catalogue (valid for a day from now).
    void LoadCatalogue() {
        LoadStoreFile();
        DateTimeValue now = game::Fn<DateTimeValue (*)()>(off::DateTime_CurrentGameTime)();
        s64 nowMs = *reinterpret_cast<const s64*>(now.bytes);
        char start[48], end[48];
        FormatDateTime(nowMs - 86400000LL, start, sizeof(start));
        FormatDateTime(nowMs + 86400000LL, end, sizeof(end));

        constexpr size_t Size = 16384;
        char* json = static_cast<char*>(game::Malloc(Size));
        if (!json) return;
        size_t n = snprintf(json, Size, "{\"mTopSection\":[");
        for (int slot = 0; slot < TopSlots + BottomSlots; slot++) {
            if (slot == TopSlots) n += snprintf(json + n, Size - n, "],\"mBottomSection\":[");
            else if (slot) n += snprintf(json + n, Size - n, ",");
            n += snprintf(json + n, Size - n, "{\"mAvailabilities\":[");
            const Offer& offer = s_Offers[slot];
            for (int c = 0; c < MaxChoices && offer.amount[c]; c++) {
                char reward[384];
                if (!RewardJson(offer, offer.amount[c], offer.price[c], reward, sizeof(reward))) break;
                n += snprintf(json + n, Size - n, "%s{\"mReward\":%s,\"mStartTime\":%s,\"mEndTime\":%s}",
                              c ? "," : "", reward, start, end);
            }
            n += snprintf(json + n, Size - n, "]}");
        }
        snprintf(json + n, Size - n, "]}");

        {
            game::NRSString text(json);
            game::OnlineValueHandle catalogue = game::ParseFromJson(text);
            if (catalogue.value) {
                game::Fn<void (*)(const game::OnlineValueHandle&)>(off::MK11PremiumShop_Initialize)(catalogue);
            }
            bool valid = game::Fn<bool (*)()>(off::MK11PremiumShop_IsDataValid)();
            Log("shop: offline catalogue %s (%zu bytes)", catalogue.value && valid ? "loaded" : "NOT loaded", n);
            if (!valid) {
                LogVerbose("shop: %s", json);
                const ProductArray* sections[] = { &Global<ProductArray>(off::MK11PremiumShop_sFeaturedItems),
                                                   &Global<ProductArray>(off::MK11PremiumShop_sDailyItems) };
                for (const ProductArray* products : sections) {
                    LogVerbose("shop: section with %d products", products->num);
                    for (s32 p = 0; p < products->num; p++)
                        for (s32 a = 0; a < products->data[p].num; a++) {
                            void* table = *reinterpret_cast<void**>(products->data[p].availabilities + a * AvailabilitySize);
                            s32 price = table ? reinterpret_cast<s32 (*)(void*)>((*reinterpret_cast<void***>(table))[16])(table) : -1;
                            s32 items = table ? game::Fn<s32 (*)(void*)>(off::LootTable_GetNumberOfEnabledItems)(table) : -1;
                            LogVerbose("shop:   product %d choice %d: table %p price %d items %d", p, a, table, price, items);
                        }
                }
            }
        }
        game::Free(json);
    }

    // The offer (slot) and choice an availability belongs to, or false when it is not in the catalogue.
    bool FindOffer(const void* availability, int* slot, int* choice) {
        const ProductArray* sections[] = {
            &Global<ProductArray>(off::MK11PremiumShop_sFeaturedItems),  // mTopSection
            &Global<ProductArray>(off::MK11PremiumShop_sDailyItems),     // mBottomSection
        };
        for (int s = 0; s < 2; s++) {
            const ProductArray* products = sections[s];
            for (s32 p = 0; p < products->num; p++) {
                const Product& product = products->data[p];
                for (s32 a = 0; a < product.num; a++)
                    if (product.availabilities + a * AvailabilitySize == availability) {
                        *slot = (s == 0 ? 0 : TopSlots) + p;
                        *choice = a;
                        return *slot < TopSlots + BottomSlots && a < MaxChoices;
                    }
            }
        }
        return false;
    }
}

// The Premium Shop page (KombatFeedModalDriver) enters with CheckPremiumShopState, which shows the
// products at once when the catalogue is there and otherwise "retrieving" until the page loop's
// MK11PremiumShop::TickDownload has the Hydra reply. Online the first case only happens when the page
// is opened again; showing products while the page is still being built closes the game. Offline each
// visit goes the online way: the page starts "retrieving", and a few ticks later TickDownload loads the
// offline catalogue (kept until it expires) instead of asking Hydra.
namespace {
    constexpr s32 DownloadIdle = 0, DownloadPending = 1, DownloadDone = 2;
    constexpr int OfflineDownloadTicks = 10;
    int s_DownloadTicks = 0;
}

// The offline items are Konsumables and keys, whose icons live in the ui_g_ConsumableIcons(_Large) Flash
// libraries. The Kombat Feed only loads those packages; drawing its grid without the libraries loaded
// closes the game. They are loaded as the Krypt's own shop does (MK11Krypt::ShowStoreAndWait:
// LoadScreenAsync into slot 3) and the offline "download" waits for the slot.
namespace {
    constexpr s32 KombatFeedSlot = 3;
    constexpr int SlotWaitTicks = 600;
    int s_SlotWaitTicks = 0;

    bool SlotLoading() {
        void* slots = *reinterpret_cast<void**>(exl::util::modules::GetTargetStart() + off::GSlotFileManager);
        return slots && game::Fn<bool (*)(void*, s32)>(off::SlotFileManager_IsSlotLoading)(slots, KombatFeedSlot);
    }
}

HOOK_DEFINE_TRAMPOLINE(ShopEnter) {
    static void Callback(void* driver) {
        if (Offline()) {
            Global<s32>(off::MK11PremiumShop_sDownloadState) = DownloadIdle;
            for (const char* library : { "ui_g_ConsumableIcons", "ui_g_ConsumableIcons_Large" })
                game::Fn<void (*)(const char*, s32)>(off::LoadScreenAsync)(library, KombatFeedSlot);
            s_SlotWaitTicks = SlotWaitTicks;
        }
        Orig(driver);
    }
};

HOOK_DEFINE_TRAMPOLINE(ShopTickDownload) {
    static void Callback() {
        if (!Offline()) return Orig();
        s32& state = Global<s32>(off::MK11PremiumShop_sDownloadState);
        if (state == DownloadIdle) {
            state = DownloadPending;
            s_DownloadTicks = OfflineDownloadTicks;
        } else if (state == DownloadPending && s_SlotWaitTicks > 0 && SlotLoading()) {
            if (--s_SlotWaitTicks == 0) Log("shop: icon libraries still loading, showing the catalogue anyway");
        } else if (state == DownloadPending && --s_DownloadTicks <= 0) {
            if (!game::Fn<bool (*)()>(off::MK11PremiumShop_IsDataValid)() || game::Fn<bool (*)()>(off::MK11PremiumShop_IsDataExpired)())
                LoadCatalogue();
            state = DownloadDone;
        }
    }
};

HOOK_DEFINE_TRAMPOLINE(ShopPurchase) {
    static game::HydraHandle Callback(void* user, const void* availability, game::NRSString* source) {
        int slot, choice;
        if (Offline() && FindOffer(availability, &slot, &choice))
            Log("shop: buying %d %s for %d Time Krystals (have %d)", s_Offers[slot].amount[choice], s_Offers[slot].slug,
                s_Offers[slot].price[choice], wallet::Get("Exp_PremiumCurrency"));
        return Orig(user, availability, source);
    }
};

// The page asks every product, every frame, whether it was bought or is locked to a character, by
// applying inventory rules (UniqueToInventoryAndCharUnlocked, CharUnlocked) to its loot table. Offline
// catalogue items are repeatable spendables: neither applies, and the rules are not run on them.
HOOK_DEFINE_TRAMPOLINE(ShopHasPurchased) {
    static bool Callback(const void* availability) {
        int slot, choice;
        if (Offline() && FindOffer(availability, &slot, &choice)) return false;
        return Orig(availability);
    }
};

HOOK_DEFINE_TRAMPOLINE(ShopCharacterLocked) {
    static bool Callback(const void* availability) {
        int slot, choice;
        if (Offline() && FindOffer(availability, &slot, &choice)) return false;
        return Orig(availability);
    }
};

// LootTable::ApplyRulesToAllItems only ever clears items' enabled flags (IRandomLootDrop+12 bit 0), so
// once another screen has run inventory rules over an offline offer's loot table its only item stays
// disabled: the purchase then rolls nothing and the page has no description to show. Offline offers are
// repeatable spendables, so their items are enabled again after every rule pass.
namespace {
    bool IsOfferTable(const void* table) {
        const ProductArray* sections[] = { &Global<ProductArray>(off::MK11PremiumShop_sFeaturedItems),
                                           &Global<ProductArray>(off::MK11PremiumShop_sDailyItems) };
        for (const ProductArray* products : sections)
            for (s32 p = 0; p < products->num; p++)
                for (s32 a = 0; a < products->data[p].num; a++)
                    if (*reinterpret_cast<void* const*>(products->data[p].availabilities + a * AvailabilitySize) == table)
                        return true;
        return false;
    }
}

HOOK_DEFINE_TRAMPOLINE(ShopLootRules) {
    static void Callback(u8* table, void* user, void* rules) {
        Orig(table, user, rules);
        if (!IsOfferTable(table) || !Offline()) return;
        u8** items = *reinterpret_cast<u8***>(table + 0x30);
        s32 count = *reinterpret_cast<s32*>(table + 0x38);
        for (s32 i = 0; i < count; i++) items[i][12] |= 1;
    }
};

// The grid and the product panel show line 0 of a product's loot description without checking that
// there is one. LootTable::GetDescription lists only the items its rules leave enabled, and the enabled
// flags are whatever the last rule pass over that table left (other screens run inventory rules on it),
// so an offline offer's single item is sometimes missing and the page closes the game. Offline offers
// are always available, so an empty description is filled from the items themselves.
namespace {
    struct DescriptionLines {
        void* data;
        s32 num;
        s32 max;
        bool condensed;
    };
    static_assert(sizeof(DescriptionLines) == 0x18);
}

HOOK_DEFINE_TRAMPOLINE(ShopDescription) {
    static DescriptionLines Callback(const Product* product) {
        DescriptionLines lines = Orig(product);
        int slot, choice;
        if (lines.num > 0 || !Offline() || product->num < 1 || !FindOffer(product->availabilities, &slot, &choice))
            return lines;
        // MK11PremiumShopAvailability::mReward (the loot table): items at +0x30, count at +0x38;
        // IRandomLootDrop::GetDescription(FGUserEntity*, LootItemDescriptionLines*) is vtable slot 6.
        u8* table = *reinterpret_cast<u8**>(product->availabilities);
        void** items = *reinterpret_cast<void***>(table + 0x30);
        s32 count = *reinterpret_cast<s32*>(table + 0x38);
        for (s32 i = 0; i < count; i++) {
            auto describe = (*reinterpret_cast<void (***)(void*, void*, DescriptionLines*)>(items[i]))[6];
            describe(items[i], User(), &lines);
        }
        static bool s_Logged = false;
        if (!s_Logged) Log("shop: %s had no description, %d line(s) from its items", s_Offers[slot].slug, lines.num);
        s_Logged = true;
        return lines;
    }
};

HOOK_DEFINE_TRAMPOLINE(StoreMenu) {
    static void Callback(void* vm) {
        static bool s_Decided = false;
        if (!s_Decided) {
            s_Decided = true;
            bool enabled = config::OfflineStore();
            if (enabled) exl::patch::CodePatcher(off::StoreOpen_ReconnectCheck).Write<u32>(0x14000047);  // b +0x11c (open)
            Log("shop: offline store %s", enabled ? "enabled (offline_store=1)" : "disabled (offline_store=0)");
        }
        Orig(vm);
    }
};

namespace mk11::shop {
    void Install() {
        ShopEnter::InstallAtOffset(off::KombatFeedModalDriver_EnterPremiumShop);
        ShopTickDownload::InstallAtOffset(off::MK11PremiumShop_TickDownload);
        ShopHasPurchased::InstallAtOffset(off::MK11PremiumShopAvailability_HasPurchased);
        ShopCharacterLocked::InstallAtOffset(off::MK11PremiumShopAvailability_IsCharacterLocked);
        ShopLootRules::InstallAtOffset(off::LootTable_ApplyRulesToAllItems);
        ShopDescription::InstallAtOffset(off::MK11PremiumShopProduct_GetLootItemDescriptionLines);
        ShopPurchase::InstallAtOffset(off::MK11PremiumShop_PurchaseProduct);
        // Main menu ZR opens the Kombat Feed without the Nintendo Account / reconnect checks only with
        // offline_store=1, the default (StoreMenu, once the SD card settings can be read).
        StoreMenu::InstallAtOffset(off::MKS_MainMenu_KombatFeedModalLoop);
        // Premium Shop page: FGOnlineCheck::CheckRestrictions -> 7 (no restriction).
        exl::patch::CodePatcher(off::PremiumShop_CheckRestrictionsCall).Write<u32>(0x528000e0);  // mov w0, #7
        // ...and InventoryData::GetSyncState -> 2 (synced; offline the inventory is always the console's).
        exl::patch::CodePatcher(off::PremiumShop_SyncStateCall).Write<u32>(0x52800040);          // mov w0, #2
        // Purchase: skip the network link and Hydra connection checks.
        for (ptrdiff_t check : { off::PremiumShopPurchase_LinkCheck, off::PremiumShopPurchase_HydraCheck })
            exl::patch::CodePatcher(check).Write<u32>(0xd503201f);                               // nop
    }
}
