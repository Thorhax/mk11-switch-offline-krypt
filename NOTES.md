# MK11 (Switch) offline Krypt

Goal: play the Krypt on the console with no server. MK11 Ultimate 1.0.26, title 0100F2200C984000,
main build ID 4F7AB1A3EEFC7EB2. Everything below is specific to that build.

## Status (2026-11-01, v0.8)

| Area | State |
|---|---|
| Entering the Krypt offline | Works |
| Krypt progress (chests opened, map, intro, unlocked areas) saved and restored | Works, survives game restarts |
| Koins / Souls / Hearts balances offline | Works (on-console wallet) |
| Player, character and gear XP | Works (on-console wallet, every `Exp_` value; tested: player level 1 -> 2) |
| Crafting materials and Konsumables from chests | Works in v0.3 (on-console wallet; shown under Inventory → Forge Items) |
| Reward pop-ups after chests and vases, no "error contacting server" kick | Works in v0.3 (user-tested): each save returns a completed server reply |
| Chest costs deducted | Works |
| Chest unlockables (concept art etc.) granted | Works (local profile unlock bits) |
| Gear rewards from chests | v0.3: chests award item sequences, now resolved to their first item (the first gear chest froze the game in v0.2/early v0.3); sequence grant tested via give.txt, awaiting a real gear chest |
| Forge | Works (user-tested): materials spent, result added |
| Giving any item (give.txt) | Works: Squared Up (`AUG_MIL_SquareWave`) added, survives a game restart. All 12 Mileena augments at Lvl. 4 owned and confirmed equippable in Kustomize |
| Time Krystals | On-console wallet (`wallet/Exp_PremiumCurrency`): earned from player level-ups (as in the normal game) and Krypt chests (`krypt_chest_krystals`, default 10 per chest), spent in the Store |
| Cyrax, Sektor, Kronika | Selectable in local versus (2026-11-01), invisible cells (no art in the game) |
| Store (ZR) | Works offline (2026-11-01): 10 earnable items, 10 Time Krystals each by default, `store.txt` to change |
| Boot error 2123-0011 | Removed: the ShowError call in OnlineSystemGetEndpoints::FiberExecute is skipped |
| "Unable to sync your online inventory" (Kustomize, fights) | Removed: the Yes/No boxes are answered Yes without being shown |
| Klassic Towers | Works offline (tested 2026-10-31, Novice x4): full mode, tower record on the SD card (`portal/KlassicTowers` in krypt.dat), completions and best score kept, end-of-fight and completion rewards granted |
| End-of-match rewards (all modes) | Works: FGOnlineInventoryBatch::Post applied locally and answered, so totals no longer snap back |
| Towers of Time | Works offline (tested 2026-10-31: Premier tower "Terror Squad", first tower): the menu opens, the current calendar towers are generated from the portal definition script, progress, best score, objective tick and rewards (including gear) are kept. Premier objective rewards (all 5 towers) and UpdateMultiplePortals (global requirements) are untested / only logged |
| Tower credits | Skipped after each character ending (`skip_tower_credits=1`, the default) |
| Tower HIGH SCORE | Shown offline from the stored best scores; RANK stays N/A (online leaderboard) |
| Online modes | Unchanged: the patches only affect the Krypt entry path and the offline prompts above |

## Install (already on the test console)

```
atmosphere/contents/0100F2200C984000/exefs/subsdk9    the module
atmosphere/contents/0100F2200C984000/exefs/main.npdm  game npdm + svcs 0x40/0x43/0x74/0x75/0x77/0x78 + full fs access
mk11offline/config.ini                                optional settings
```

Release copies: `mod/releases/v0.6-xp/` (current), `mod/releases/v0.5-towers-of-time/`, `mod/releases/v0.4-towers/`, `mod/releases/v0.3-materials/`, `mod/releases/v0.2-give-items/`, `mod/releases/v0.1-offline-krypt/`. To uninstall, delete the `exefs` folder.

## Files the module writes on the SD card (`sdmc:/mk11offline/`)

- `krypt.dat`: the Krypt progress, tower records (`portal/<name>`) and the wallet, one `key<TAB>json` line
  each. Readable and editable. It is written once per reward batch / save (store::Batch), not per value.
  `krypt.bak` is the previous version. The wallet lines are `wallet/Exp_Koins`, `wallet/Exp_SoulFragments`,
  `wallet/Exp_BrutalityHearts`, and one `wallet/<item>` line per crafting material or Konsumable
  (e.g. `wallet/CopperPlating`).
- `inventory_batches.jsonl`, `portal_requests.jsonl`: every reward batch / tower request, one per line;
  rotated to `.old` above 1 MB.
- `log.txt`: the module's log for the current run.
- `replay_batch.json` (if you create it): applied once at the next Krypt save, then deleted.
- `items.txt`: every item definition the game has loaded (31,106): GUID, internal name, character,
  slot, type, augment flag, disabled flag, display name. Written on Krypt start when missing.
- `inventory.txt`: your current inventory, one line per item (Hydra ID, GUID, name, character,
  display name). Rewritten on every Krypt start.
- `give.txt` (if you create it): one internal name or GUID per line, `#` comments. Each item is
  added at the next Krypt start, then the file is deleted. Results are in `log.txt`.

`config.ini`:

```
krypt_currency_topup=0   # >0: top Koins/Souls/Hearts up to this amount on each Krypt start
apply_gear_rewards=1     # grant gear/augments from Krypt chests locally
offline_store=1          # Store (ZR) with the offline catalogue
krypt_chest_krystals=10  # Time Krystals per Krypt chest opened (0 = off)
```

## How it works

Hooks are in `mod/exl/source/program/`. The offsets are generated from symbol names (`mod/offsets.txt` →
`tools/gen_offsets.py`), because `main` exports about 383k dynamic symbols with full C++ names.

1. **Entry.** `MKS_MainMenu::GoingToKrypt` is a compiled menu script. Three call sites inside it are patched:
   - `IsRestrictedFromEnteringOnlineFeature` → 0
   - `MKOnline_IsUserSignedIn(user, true)` → 1
   - `EnsureInventoryIsDoneSyncing` → 2 ("synced")

   `MK11KryptProgressionFeedObj::WaitForFinishWithCancel` is replaced and marks the feed downloaded.
2. **Save.** `MK11KryptSaveRestore::Save` is replaced. Each `FKryptSaveDataBase` writes itself into an
   `OnlineValueMap` (vtable +0x10), which the game's `OnlineValue::ToJsonString` turns into JSON, and the
   JSON goes into `krypt.dat`. The completion callback is then called with `true` (vtable slot 6).
   The function returns a `HydraHandle` through x8.
3. **Load.** `MK11KryptSaveRestore::RestoreKryptData` is replaced. It parses the stored JSON with
   `OnlineValueUtils::ParseFromJson` and hands it to the object's loader (vtable +0x18).
4. **Wallet.** MK11 keeps no offline balance for Koins, Souls or Hearts, because their `ExperienceInfo`
   offline stat id is unused. So while there is no Hydra connection,
   `ExperienceView::GetExperience` / `OnInventoryUpdated` read and write `krypt.dat` instead.
5. **Server reply.** The Krypt keeps the save's `HydraHandle<OnlineHydraSSC>` in `GChestSSCHandle`.
   `MK11Krypt::ProcessPendingServerRewardRequest` waits for it, builds the reward pop-up from
   `response["rewards"]["unlockTracker"]`, and raises `ErrorContactingServer` (kicking you out of the Krypt)
   when the request did not succeed. `Save` therefore builds an `OnlineHydraSSC` itself
   (`IsolatedLongTermAllocator::Malloc(0x108)` + constructor + one reference, because
   `OnlineHydra::HydraObjectInit` returns null while offline, and it is never queued with OnlineHydra).
   It sets state +64 = 3 (complete) and error code +0x50 = 0, and stores
   `{"rewards": {"items": [], "user_data": <batch user_data>}}` as its response (+0xe8):
   `UnlockTracker::AddUnlockMessageFromOnlinePayload` reads the pop-up items from
   `user_data.data.unlockTracker`. The log records each reply check and the caller of any Krypt critical error.
6. **Rewards.** The `FGOnlineInventoryBatch` passed to `Save` is serialised with `ToOnlineValue` and
   applied by `batch_apply.cpp`:
   - `inc_consumable` → wallet. Crafting materials and Konsumables (`SpendableItemInventory`) are
     only counted in the Hydra inventory (`OnlineInventory::GetConsumable`), so offline
     `SpendableItemInventory::GetCurrentBalance` is hooked to read the wallet for every slug
     without the `Exp_` prefix. v0.2 and earlier dropped these rewards.
   - `game_unlockables` → `MK11Unlockables::Unlock(GMK11Unlockables, user, bit, page)`
   - `add_game_item` → gear (gated)

## Story Mode (main.cpp)
- Fight rewards: `StoryRewards_IsConnectedCall` lets `SetupAndShowRewardsForFight` prepare them offline and
  `MK11HydraSSCInterface::RewardStorySequence` applies the proc's batch locally. Chapters finished offline
  before this get their missing rewards when the chapter select opens (`CatchUpStoryRewards`).
- The "not connected… you will not receive your rewards until you reconnect" box is skipped in both places
  that show it (`MKS_MainMenu::StorySubmenu`, `StartStoryHUBDriver` for Aftermath).

## Towers (portals.cpp)

- Tower content ships with the game (`PortalDefinitions.mko`). Hydra stores one record per tower set,
  `{id, unique_key, expire_time, data: PortalSSCData}`, generated by the client
  (`MK11PortalObj::GenerateServerData`). Offline the records are kept as `portal/<name>` in the store and
  handed to the game with `UpdatePortalGOSObject`; Klassic Towers is eligible for the full experience when
  its record exists.
- `MK11PortalSSC::UpdatePortal` / `UpdatePortalEvent` carry update commands (`set`, `inc`, `max` with
  absolute dot paths), applied to the stored record (json.cpp). Changed records are handed to the game when
  the tower menu opens next, not inside the request.
- A completed tower sends its rewards as an inventory batch in `extraData.unlocks`; it is granted like a
  Krypt batch.
- Return types differ: `UpdatePortal` and `UpdateMultiplePortals` return a HydraHandle through x8, but
  `UpdatePortalEvent` returns the request's OnlineHydra object id in x0 (the caller calls
  `OnlineHydra::GetObjectById`). Getting this wrong corrupted memory and crashed after the credits.
- All requests are logged to `portal_requests.jsonl`.
- High scores: the tower screen reads the player's persistent event objects (`P<portal>_<stage>_<event>`,
  data `{score, completionRank}`), which Hydra sends as `persistentData` with a record. Best scores are kept
  in the record as `offline.persistent` and handed over with it (one `UpdatePortalGOSObject` call each).
- Towers of Time: calendar towers come from `GetCalendarPortalInfos(lastPortalRequestTime)`; offline that
  date is set to now and missing records are generated with `TimedPortalObj`, whose online
  `CreateEventsAndStages` is routed to the base one (`<name>_PortalInit` in PortalDefinitions.mko) while
  generating. The record's `endTime` is set from the tower's definition; records of ended towers are
  removed when Towers of Time opens. The game clock (`time()`) reads 1970
  offline, so `PlatformGetNetworkUTC` falls back to the console user clock.
- Credits: the two credits movies in `BasicLadder_PlayLadderEnding` are skipped by making `Movie_Play` report
  failure for those call sites.
- exlaunch's trampoline pool holds 200 bytes per hook; `JitSize` was raised to 0x4000. Module code
  allocates from the game heap (`game::Malloc` etc.), not newlib's malloc.

### Summoned towers (Tower Keys)
- Summonable Towers → A → Select Tower Key: the confirm is **held** A (a charged button), not a tap.
- `MK11PortalSSC::SummonKeyPortal` (Hydra `portal_key_activate`: activatePortalName, cost, deactivatePortalId)
  is answered offline (`SummonKey` hook): the key's record is generated like a Towers of Time one
  (`TimedPortalObj`, events from `<name>_PortalInit`), stored as `portal/<name>` with `data.type` `"key"`
  and `unique_key` = the tower name (records are looked up by `data.name`), the key is taken from the wallet,
  any earlier summoned record is removed.
- `MK11PortalInfo::CreateKeyPortal` only uses a record whose `data.type` is `"key"` and that is available
  (`now < data.endTime`). Record JSON cannot carry dates (`UpdatePortalGOSObject` reads them as
  `OnlineValueDate`, which JSON never produces), so the end is stored as `offlineEndMs` and written into the
  loaded record (`ApplyStoredEndTime`; PortalGOSData +0x70). Summoned towers last 24 hours.
- Key slugs: `Portal*Key` spendables (flag 0x10). "Lucky Key" = `PortalPremiumKey` → Test Your Luck (rewards
  are Konsumables / character items, no Time Krystals).

## Giving items (gear.cpp)

`gear::Add` mirrors `MK11Unlockables::Unlock(FGUserEntity*, FName, ...)`:
- `FindItemDefinition`
- `GearItem(handle, character level)`
- `InventoryManager::AddItem`

`AddItem` ignores its `BILootDropSourceData` argument. It calls `InventoryData::InternalAddItem` →
`RequestOnlineInventoryAddItem`, which applies the item locally (`OnUpdateItem`) and also queues an
`add_game_item` for Hydra. The game saves the item to its profile save on its own.

Krypt chests award *item sequences* (`FItemSequenceDefinition`: a gear piece with its upgrade states),
not item definitions. `FindItemDefinition` returns any `FItemObject` with the GUID, so `gear::Add` tells
them apart by vtable and grants a sequence's first state (+40 state array, +48 count), as the unlock
pop-up does. Passing a sequence to `GearItem`/`AddItem` hung the game.

The v0.1 crash was in the cleanup: it called `MKItem::~MKItem` on the `GearItem`. The game tears
`GearItem` down field by field (`DestroyGearItem`).

Item notes:
- `items.txt` slot `Socket_Gold` = character augments. Each has four definitions, one per level
  (`..`, `..2`, `..3`, `..4` = Lvl. 1–4).
- In Kustomize → Manage Augments, the generic ones are under Dragon Blood.
- Kustomize shows "Unable to sync your online inventory": answer Yes. Granted augments (Mileena's
  Lvl. 4 set, 2026-10-31) can then be found and equipped; the user tested this on the console.

## Store (ZR) (shop.cpp, `offline_store=1`)
- ZR opens `KombatFeedModalDriver` (Premium Shop + eShop pages). Offline the menu script's
  Nintendo-Account/reconnect check is skipped (`StoreOpen_ReconnectCheck`, applied at runtime only with
  `offline_store=1`), and the page's restriction and inventory-sync checks pass.
- Catalogue: `MK11PremiumShop::Initialize` with `{mTopSection: 9 slots, mBottomSection: 1 slot}`; each slot
  `{mAvailabilities: [{mReward: loot table, mStartTime, mEndTime}]}` (several = quantity choice). Loot table
  `{dropCount: 1, items: [{LootItemType: "SpendableItemLootItem", dropWeight: 1, id, amount,
  premiumCurrencyPrice}]}`: `IsDataValid` wants exactly 9 + 1 slots and a non-zero premiumCurrencyPrice;
  without dropWeight nothing drops. Currency loot items have no shop price. `store.txt` replaces it
  (`<slug> <amount>:<price> ...`, 10 lines).
- Price = Time Krystals (`premiumCurrencyPrice`), charged by the game's own `PurchaseProduct` batch
  `{Exp_PremiumCurrency -price, item +amount}`, applied to the offline wallet. Tested: 10 Krystals →
  PortalKoins1Key.
- **The crash (solved 2026-11-01):** `LootTable::ApplyRulesToAllItems` only clears items' enabled flags
  (`IRandomLootDrop`+12 bit 0). Other screens run inventory rules over the offer tables, leaving their only
  item disabled; then `GetLootItemDescriptionLines` is empty and `RefreshPremiumShopPage_GridItem` →
  `GetLootItemLine` copies line 0 from null (Data Abort at `TUNIString` copy), and a purchase rolls nothing.
  `ShopLootRules` re-enables offer items after every rule pass; `ShopDescription` fills an empty description
  from the items as a safety net. Found with an Atmosphère crash report once SaltyNX and the MK11 cheat file
  were disabled (see Tooling).

## Character select: Cyrax, Sektor, Kronika (roster.cpp, 2026-11-01)
- **Works (local versus, tested):** the cursor reaches them, the name line shows them, they have one "Default"
  variation, and full matches run with them as P1 or CPU (Cyrax vs Sub-Zero, Kronika vs Sektor; Cyrax wins,
  victory screen, rewards). Without character data they earn no character XP. Klassic Towers' grid reaches them
  too (a tower run with them, and its ending, is untested).
- **Not shown:** the select screen's Flash movie has no cell at the free grid positions (GetCellElement returns
  null) and the game has no art for them (no `cell_<Name>` in ui_ps_CharacterImages, no `<Name>_P1` in
  ui_ps_Versus_Jpegs), so their cells look empty and the large art keeps the previous fighter.
  Where: Cyrax top-left (up from Joker), Sektor top-right (up from Spawn), Kronika bottom-right (right of Rain).
- How: PlayerSelect.mko is compiled to native code (`MKS_PlayerSelect::*`). Its roster `charTable` (37 rows of
  0x30: row, col, `CHAR_<Name>`, portrait, key, ...) is reached through `MKS::VM::GetStackAddr(0x2810000000001288)`;
  that call returns a 40-row copy (extra rows copy Scorpion's tail fields), and the 23 row-count constants
  (36/37) in MKS_PlayerSelect become 39/40 once the copy is in use (each checked before patching).
  `UIPlayerSelectModuleScreen` gets a copy of the script's `UIGridSelectionNavTable` (42 entries
  `{cell, left, right, up, down}`) with the free cells linked in. `FGGameInfo::GetIsCharacterLocked` says
  unlocked for them (they have no character content entry) unless a user is connected to Hydra.
- Missing UI images used to close the game (`UIImageElement::SetTextureDecodeAsync` decompresses a null
  texture); they are now skipped and logged ("no image ...").
- Keys: `CYR`, `SEK`, `KRO`. Fighting them via a swapped selection showed the selection has to change at
  `CharacterDefinition::SetCharacter` time (swapping at `PLAYER_INFO` left Sub-Zero's intro assets requested).
- `NRS::FailAssert` is hooked to log assert messages (e.g. "Unable to load file X, failed to find import: Y").

## Tooling

- **Rig:** `tools/rig/sb.py` drives the console through sys-botbase (buttons, screenshots).
  `ftp.py` talks to ftpsrv. `boot_to_menu.sh` starts the game from HOME. Console details:
  `../syscon-guide.txt`.
- **Build:** `mod/build.sh` runs the compiler, linker and elf2nso only. It never runs exlaunch's own
  scripts, regenerates the offsets and builds `main.npdm`.
- **Analysis:**
  - `tools/nso.py` decompresses an NSO and dumps its symbols.
  - `tools/dis.py` disassembles with symbol names.
  - `tools/xrefs.py` lists BL/B callers.
  - `tools/relocs.py` finds where pointers are stored.
  - `tools/strrefs.py` lists the string literals a function uses.
- **Dumps:** `exefs/` came from DBI ("Installed games" → Update → browse → copy). `romfs` was only
  partly copied. sys-botbase can't peek the game's code pages.
- **Crashes:** a debugger attached to the game takes the exception, so neither Atmosphère's crash report
  nor `crash.cpp`'s handler sees it. SaltyNX attaches at launch even for titles in its `exceptions.txt`
  (only its plugins are skipped), and dmnt attaches when a cheat file for the build exists. With SaltyNX's
  `boot2.flag` removed and `cheats/4F7AB1A3EEFC7EB2.txt` moved away (backups in `data/sd_backup_2026-11-01/`)
  plus a full reboot, both work: `crash_reports/*_0100f2200c984000.log` names the functions, and `log.txt`
  gets `=== CRASH ===` with a backtrace.
- **Boot:** 2123-0011 came from `nn::err::ShowError` in `OnlineSystemGetEndpoints::FiberExecute` (main+0x1937890) after the
  endpoint lookup fails; the module skips that call. `system_dialogs.cpp` logs every SDK error dialog with its caller.
