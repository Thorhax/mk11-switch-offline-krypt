# MK11 (Switch) offline Krypt

Goal: play the Krypt on the console with no server. MK11 Ultimate 1.0.26, title 0100F2200C984000,
main build ID 4F7AB1A3EEFC7EB2. Everything below is specific to that build.

## Status (2026-11-02, v0.12)

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
| Gear rewards from chests | Chests award item sequences, resolved to their first item (the first gear chest froze the game in v0.2/early v0.3); sequence grant tested via give.txt. Reward pop-up enabled (v0.9); a pop-up from a real gear chest is untested |
| Forge | Works (user-tested): materials spent, result added |
| Giving any item (give.txt) | Works: Squared Up (`AUG_MIL_SquareWave`) added, survives a game restart. All 12 Mileena augments at Lvl. 4 owned and confirmed equippable in Kustomize. `<slug> <amount>` lines add currencies and stackable items (v0.10, tested: Souls +1000) |
| Time Krystals | On-console wallet (`wallet/Exp_PremiumCurrency`): earned from player level-ups (as in the normal game) and Krypt chests (`krypt_chest_krystals`, default 10 per chest), spent in the Store |
| Cyrax, Sektor, Kronika | Selectable in local versus (2026-11-01). Their select screen cells show borrowed art, or the user's art from `sdmc:/mk11offline/art/` (cell and versus art, v0.11). Full Klassic Towers with all three completed; their own tower endings play from `romfs/Movies/` when present (v0.11) and are listed in Kollection > Endings; finishers disabled in their fights. Move lists in the pause menu and Quick Reference, cursor reaches every row (v0.10, tested with Cyrax) |
| Kombat Kard (ZL) | Opens offline (2026-11-01): wallet, Kareer Stats, notifications. Daily Challenges: three a day, progress kept, Koins paid at the goal (tested: throws, uppercuts) |
| Krypt Shrine and Kollector | Work offline (user-tested 2026-11-01/02): Kollector trades spend relics; Shrine Koin offerings give Konsumables (up to 74,000) and gear or skins (75,000 and up), granted and kept after equip and save. Gear and skin pop-ups shown for chests, Kollector and Shrine (the `AddUnlockMessageFromOnlinePayload` call sites in ProcessPendingServerRewardRequest, KryptExecuteStoreTrade and OnReward pass true). Known: Koin offerings also get the chest bonus Time Krystals |
| Free spending (`free_currency`, `free_items`) | Optional, off by default (v0.9): `wallet::Set` ignores decreases of the four currencies / of non-`Exp_` spendables. Tested in the Store; untested for chests, the Forge and Kollector trades |
| AI fighter (`p1_cpu`) | Optional, off by default: player 1 uses the game's user-owned AI path; the end-of-match menu answers the controller (v0.10, tested in Fight > Local and in Klassic Towers) |
| Log (`verbose_log`) | Routine lines off by default (v0.10) |
| Store (ZR) | Works offline (2026-11-01): 10 earnable items, 10 Time Krystals each by default, `store.txt` to change. One crash on a second Time Krystal purchase (2026-11-01, MK11 cheat file active) was not reproduced in 17 tries |
| Boot error 2123-0011 | Removed: the ShowError call in OnlineSystemGetEndpoints::FiberExecute is skipped |
| "Unable to sync your online inventory" (Kustomize, fights) | Removed: the Yes/No boxes are answered Yes without being shown |
| Klassic Towers | Works offline (tested 2026-10-31, Novice x4): full mode, tower record on the SD card (`portal/KlassicTowers` in krypt.dat), completions and best score kept, end-of-fight and completion rewards granted |
| End-of-match rewards (all modes) | Works: FGOnlineInventoryBatch::Post applied locally and answered, so totals no longer snap back |
| Towers of Time | Works offline (tested 2026-10-31: Premier tower "Terror Squad", first tower): the menu opens, the current calendar towers are generated from the portal definition script, progress, best score, objective tick and rewards (including gear) are kept. Premier objective ticks, tower unlocks and the final objective reward work (Chittering Treasures, all 5 towers, 2026-11-01) |
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
  `krypt.bak` is the previous version. `backups/` holds krypt.dat as it was at each game start
  (`krypt-<date>-<time>.dat`, newest 10) and at the first start of each day (`daily-<date>.dat`, newest 14);
  dates are the console clock's (this console's reads 2003). To restore one, copy it over krypt.dat with the game closed. The wallet lines are `wallet/Exp_Koins`, `wallet/Exp_SoulFragments`,
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
p1_cpu=0                 # 1: player 1 is CPU-controlled in every fight (CPU vs CPU, testing)
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
- Premier objectives: completing a tower updates its record (challengeData.currentValue), but the game went back
  to the tower screen without reloading records, so ticks and the towers they unlock only showed after
  re-entering Towers of Time. Changed records are injected at `UITowersMainScreen::StartScreen` now.
  `portal_update_multiple` (global requirements, {<tower>: [commands]}) is applied to the stored records.
- Premier objective reward: finishing the last tower sends `UpdatePortal` "portal_quest_completed" whose extra
  data carries `unlocks` (Koins + `add_game_item` item sequences). The hook now grants them (`GrantUnlocks`) and
  commits them with the record. Before this fix the reward was dropped; the missed Chittering Treasures reward
  (62,500 Koins + D'Vorah True Bug / Kollector Mercenary skins) was replayed via `replay_batch.json` and
  verified in game 2026-11-01.
- Gear reward pop-ups in the Krypt: `UnlockTracker::AddUnlockMessageFromOnlinePayload(payload, group, bool)`
  shows an `Equipment` unlockTracker entry from the matching server-made instance in `rewards.items`; only when
  the bool is set does it fall back to building the gear from the entry's GUID (Story fights pass true). Our reply's
  `items` is empty, and `KryptExecuteStoreTrade` (Kollector), `ProcessPendingServerRewardRequest` (chests,
  vases) and `MK11Krypt::OnReward` (Shrine) pass false, so gear was granted without a pop-up. All three call
  sites now pass true (2026-11-01; the Shrine's 2026-11-02, user-tested with 48,000-100,000 offerings).
- Free spending (config.ini, off by default): `free_currency=1` / `free_items=1` make `wallet::Set` ignore
  decreases of Koins, Souls, Hearts and Time Krystals / of spendable items (materials, relics, keys,
  Konsumables). Every offline spend goes through the wallet, so this covers the Krypt, Shrine, Kollector,
  Forge, store and key summons. Verified with a store purchase (Time Krystals stayed at 400), 2026-11-01.
- Logging: routine lines (wallet changes, store flushes, Krypt saves and server replies, daily progress, portal
  commands, shop catalogue dumps) use `LogVerbose`, written only with `verbose_log=1`. Errors, grants and one-off
  events always go to log.txt.
- give.txt also takes `<slug> <amount>` lines: Exp_Koins, Exp_SoulFragments, Exp_BrutalityHearts,
  Exp_PremiumCurrency or any spendable slug (checked with `GetItemDefinitionBySlug`); the amount is added to the
  wallet. Verified 2026-11-01 (Souls +1000 on the Krypt HUD).
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
- **Select screen cells (selectart.cpp, 2026-11-02, tested):** the movie has all 42 cells
  (`_root.playerSelectPos.playerSelectGrid.cells.Cell_<row * 6 + col>`); the five free ones are hidden
  (`visible = false`). The script sets each cell's portrait with `UIImageElement::setTexture` on
  `Cell_<n>.Cell.cellImage` ("#MainlineManaged.UI.ui_ps_CharacterImages.cell_<Name>.tga", then a `LoadAsset("img://<path>")`
  invoke). The hook swaps the texture for borrowed art and sets `visible = true` on the cell (cellImage's grandparent)
  and scale/y on cellImage through Scaleform `Value::ObjectInterface::GetMember/SetMember`, directly, only when the
  element is not queued (+32 clear), as the game does itself in that call. Art: Cyrax `Consumable_CyraxAssist` (net),
  Sektor `Consumable_SektorAssist` (missiles), Kronika `ui_g_KosmeticIcons.Announcer.AnnouncerVoice_Kronika` (portrait,
  512x512). The two assist icons are 64x64 (scale 1.6), cellImage starts at about 0.78 (128 x 256 portraits).
  The borrowed textures belong to other UI and are gone after a fight, so the first time each is found
  (`StaticFindObject(UTexture2D::PrivateStaticClass, null, full path)`) it is rooted (`UObject::AddToRoot`); until
  then the cell stays hidden. Tested: Fight > Local (both cursors), back out and in, Fighter Select after a match,
  Klassic Towers select. The large art beside the grid still keeps the previous fighter (no `<Name>_P1` versus art).
- **Art from the SD card (2026-11-02, tested):** every Flash image request goes through
  `FGFxMovie::FGFxImageLoader::LoadProtocolImage(info, "img://<element path>")`; the hook answers it for the extra
  fighters' elements with a Scaleform `RawImage` (`RawImage::Create(format 1 = R8G8B8A8, ...)`, ImageData at +0x28,
  plane 0 at [+0x38]: w, h, pitch +8, data +24) filled from `sdmc:/mk11offline/art/<Name>_cell.art` / `_large.art`
  (tools/make_art.py: "MK11ART2", width, height, left padding, RGBA). A 128x256 cell image fills the frame like the
  game's portraits. Versus art: roster.cpp no longer just skips the missing `<Name>_P1/_P2.jpg`: a stand-in
  (Scorpion's) decodes so the element still asks for its image, and the request gets the file's image. The
  1024-wide image is placed from its left edge: player 1 at x 0 (0.665 px per texel, about 650 px visible beside
  the grid), player 2 at about x 430 with only x 840 on visible, where the game's own art has the face; player 2's
  image is the picture mirrored and moved right by 480 black columns (face at about x 1090, reaching past the
  screen edge). All three fighters have art from the user (artwork/, not committed; converted copies in
  data/art/). Scaling Kronika's 512x512 announcer icon on the versus image element (a 2560 x 2160 box) put it
  off screen; that fallback was dropped.
- Dead ends while getting there: a second trampoline on `UIImageElement::SetTextureDecodeAsync` (already hooked in
  roster.cpp) looped forever; creating a `UIElement` for `Cell_<n>` from inside an invoke crashed in
  `AS3::MovieRoot::GetVariable`; reading Flash values from a `gotoAndPlay` ("GainFocus") hook crashed.
  Texture lookups by full path with roster.cpp's image class fail (that class is the JPEG versus art's).
- How: PlayerSelect.mko is compiled to native code (`MKS_PlayerSelect::*`). Its roster `charTable` (37 rows of
  0x30: row, col, `CHAR_<Name>`, portrait, key, ...) is reached through `MKS::VM::GetStackAddr(0x2810000000001288)`;
  that call returns a 40-row copy (extra rows copy Scorpion's tail fields), and the 23 row-count constants
  (36/37) in MKS_PlayerSelect become 39/40 once the copy is in use (each checked before patching).
  `UIPlayerSelectModuleScreen` gets a copy of the script's `UIGridSelectionNavTable` (42 entries
  `{cell, left, right, up, down}`) with the free cells linked in. `FGGameInfo::GetIsCharacterLocked` says
  unlocked for them (they have no character content entry) unless a user is connected to Hydra.
- Missing UI images used to close the game (`UIImageElement::SetTextureDecodeAsync` decompresses a null
  texture); they are now skipped and logged ("no image ...").
- Finishers: a Brutality on Cyrax closed the game (MKObj::AssignAndInitRagdoll in Sub-Zero's brutality: no victim
  data for him). `IsBrutalityAllowed`, `IsFatalityAvailable` and `IsBackgroundFatalityAvailable` return false
  while one of the three is fighting; Fatal Blows work. Klassic Tower with Cyrax (AI, p1_cpu): four fights and
  the Kronika boss fight run (2026-11-01).
- Keys: `CYR`, `SEK`, `KRO`. Fighting them via a swapped selection showed the selection has to change at
  `CharacterDefinition::SetCharacter` time (swapping at `PLAYER_INFO` left Sub-Zero's intro assets requested).
- `NRS::FailAssert` is hooked to log assert messages (e.g. "Unable to load file X, failed to find import: Y").

## Player 1 as CPU (autoplay.cpp, `p1_cpu=1`)
- Fight scripts call `FGAIGameInterface::CheckForDrones` at round start. A player whose `CharacterDefinition` is
  marked user-owned AI (+81 bit 2, `SetIsUserOwnedAI`; the game's own AI fighter modes) becomes a drone through
  `TurnPlayerIntoUserOwnedAI`, which keeps the physical pad linked from the virtual one (+16); the game hands
  it back after the match. `p1_cpu=1` marks player 1 that way. Tested in local versus (Klassic Towers was
  tested with the earlier plain-CPU version only).
- The first version set `PLAYER_INFO` +56 bit 0 (plain CPU) instead: `TurnPlayerIntoDrone` then forgot the
  physical pad, and the end of match menu ignored the controller even after rebinding it. Fixed 2026-11-02:
  two Scorpion vs Sub-Zero matches (Retry between them), the end menu answered, Quit returned to the main menu.
  The old hand-back at `UIFightHUD::BuildOnEndOfMatchFade` stays as a fallback and clears the flag.

## Move lists (movelist.cpp)
- The pause menu move list of Cyrax, Sektor and Kronika is a "PLACEHOLDER" line. The move list script fills a page
  with `UIMoveListScreen::SetTotalNumMoves`, `SetLocalizedHeader(0, key)`, then `SetLocalizedMove(row, nameKey,
  input)` (input: a 4-byte wide string; names and headers are localisation keys, unknown ones read "BUG_ME").
  For them each page gets the user's list (Switch layout): names written into the row's label after the game
  builds it (label element paths: move name at screen +728, header at +760), the script's other headers and
  blank lines for those rows dropped, rows after the list blanked. Tested: Cyrax, Sektor.
- Cursor: the script's pages are a script global (`GetStackAddr(0x2810000000000090)`), 4 x 824 bytes: up to 100
  line object pointers, line count at +808; a line's type at +12, types 1/2/4 take the cursor
  (`MoveList_CanSelectMove`). `MoveList_FillMoveCells` is hooked: rows 1..N of each page point at the page's
  placeholder move line and the count becomes 1 + N, so the cursor reaches (and wraps over) every row. The move
  and frame data fields (`SetLocalizedFrameData`) read "-" for them. Tested with Cyrax 2026-11-02.
- The pause menu's Quick Reference box is the quick move list (`SetLocalizedQuickMove(row, nameKey, input, player,
  …)`, rows at screen +0x200 / +0x250 per player, name in "FocusText0.Text" under the path at +808): it shows
  the fighter's special moves (fighter found from the player's character definition). Tested: Cyrax.

## Tower endings for the extra fighters (endings.cpp, 2026-11-02)
- A tower's character ending is a movie: `MKS_Ladder::BasicLadder_PlayLadderEnding` plays `Ladder_Ending` (the
  game's Kronika defeat), then `PLAYER_INFO::GetLadderEnding` = character key + `_Ending` (Cyrax: `CYR_Ending`
  already), then the two credits movies (skipped). Movies are `../MOVIES/<name>.mp4` (`mk_get_movie_filepath`),
  in the romfs as `rom:/Movies/` (`rom:/NX64TOC.txt` lists `..\Movies\SCO_Ending.mp4` etc.).
- Format of the game's endings (SCO_Ending.mp4): H.264 High L3.1 1280x720 29.97 fps (2 B-frames, 1 ref), six AAC
  LC stereo 48 kHz tracks (eng fra deu ita por spa) before the video track. tools/make_ending.py writes that.
- New movies go in with LayeredFS (`atmosphere/contents/0100F2200C984000/romfs/Movies/CYR_Ending.mp4`), but the
  engine only opens files in its table of contents: without an entry the player waits on a black screen
  forever. `FTableOfContents::AddEntry` / `AddToTOC` after boot puts the file in the table (`ExistsInTOC` 1) and the
  movie still does not start, so `FTableOfContents::ParseFromBuffer` is hooked instead and the main TOC text gets
  `<size> 21 ..\Movies\<KEY>_Ending.mp4 55` for each ending found (chunk and flags as the game's endings).
- Tested 2026-11-02: Klassic Tower Novice with Cyrax, Sektor and Kronika (AI fighter, Very Easy): Ladder_Ending,
  then the user's CYR_Ending / SEK_Ending / KRO_Ending (Kronika's is a preliminary cut), credits skipped, results.
- Replacing a video: convert it again with make_ending.py, overwrite the mp4 on the SD card and restart the game
  (the TOC entry, with the file's size, is written at boot).
- Kollection > Endings: the list is category 4 of the Kollection table (`UMK11KollectionItemInfo`, 0x80-byte
  `FKollectionItemInfo`: +0x00 unlock name, +0x18 / +0x58 images, +0x38 key, +0x48 package FName, +0x68 movie,
  +0x78 category, +0x7c child/credits bits), registered by `MK11KollectionInfo::KollectionInfoInit` each time the
  Kollection opens. After it, each extra fighter whose movie exists gets a copy of a neighbouring entry (empty
  unlock name = never locked, its own key and movie), registered with `RegisterItem` and moved to its place in
  name order; `KollectionItem::GetLocalizedName` returns its name (32-bit wide text). Tested 2026-11-02: all three
  listed (after Cetrion, Kotal Kahn, Scorpion) and play from the list. The preview images are the neighbour's.
- Subtitles: the game draws prerendered subtitles by movie name (`mk_execute_prerendered_subtitles`); there are
  none for CYR_Ending, so the video carries its own.

## Kombat Kard and Daily Challenges (daily.cpp)
- ZL opens `PlayerProfileModalDriver::RunInputLoop`, which loads the Nintendo network service account data
  (`OnlineNX64UserData::LoadNSAData`, fails offline: "You must be connected...") and then waits on "Connecting to
  server..." until `OnlineHydra::IsConnected`. Both are passed offline (main+0x20cb494 tbz -> nop; main+0x20cb620
  `mov w22, w0` -> `mov w22, #1`). Kareer Stats show the local history.
- Daily Challenges (`daily_challenges=1`, the default): Hydra's `daily_challenge_get` entries are
  `{id, data: {type, key, current, goal, data: {goal, currencyType (1 Koins, 4 Souls, 5 Hearts), currencyAmount,
  stars, category | awardOnWinOnly}}}`; types DailyOnlineFightingStatChallengeInfo / ProfileStat / PlayAsChar, key a
  stat name (745 names: data/stat_names.txt). The module picks three a day (console clock) from a pool of fighting
  stats with challenge text (TOTAL_SPECIALS_USED, KRUSHING_BLOWS_PERFORMED, TOTAL_SUPERMOVES, TOTAL_THROWS,
  TOTAL_UPPERCUTS, TOTAL_BRUTALITIES, TOTAL_FATALITIES; "BUG_ME" for TOTAL_WINS, TOTAL_FLAWLESS_ROUNDS,
  TOTAL_COMBOS, TOTAL_JUMP_ATTACKS, TOTAL_PROJECTILES_FIRED) and creates them on the main menu as the reply handler
  does (`DailyChallengeGOSData` + `CreateChallengesForUser`).
- Progress: after a match with a human player, `UpdateChallenges` -> `DailyChallengeObject::DailyChallengeUpdate`
  -> `ChallengeInfoUpdate` reads the match's fight stats and only queues an increment
  (`OnlineDailyChallengeInfo::QueueIncrementCommand`, info +56/+64) for the next upload, which never happens
  offline. The DailyChallengeUpdate hook applies the queue to the info (current +32, goal +36), stores it in
  krypt.dat (`daily/<id>`) and pays the Koins once at the goal (`daily/<id>/done`). Tested 2026-11-01: throws and
  uppercuts completed and paid. AI-only matches (p1_cpu) do not count (no human player).
- A completed challenge used to stay listed until the next day; claiming it on the Kombat Kard asked Hydra
  (`daily_challenge_reward`) and closed the game. The list is now rebuilt without it on the next main menu loop,
  and a reward post for a paid challenge pays nothing.
- Note: PLAYER_DAMAGE_TAKEN is a fraction of the health bar (float < 1 per match), not a usable test stat.

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
