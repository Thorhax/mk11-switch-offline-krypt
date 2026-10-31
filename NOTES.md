# MK11 (Switch) offline Krypt

Goal: play the Krypt on the console with no server. MK11 Ultimate 1.0.26, title 0100F2200C984000,
main build ID 4F7AB1A3EEFC7EB2. Everything below is specific to that build.

## Status (2026-10-31, v0.3)

| Area | State |
|---|---|
| Entering the Krypt offline | Works |
| Krypt progress (chests opened, map, intro, unlocked areas) saved and restored | Works, survives game restarts |
| Koins / Souls / Hearts balances offline | Works (on-console wallet) |
| Crafting materials and Konsumables from chests | Works in v0.3 (on-console wallet; shown under Inventory → Forge Items) |
| Reward pop-ups after chests and vases, no "error contacting server" kick | Works in v0.3 (user-tested): each save returns a completed server reply |
| Chest costs deducted | Works |
| Chest unlockables (concept art etc.) granted | Works (local profile unlock bits) |
| Gear rewards from chests | v0.3: chests award item sequences, now resolved to their first item (the first gear chest froze the game in v0.2/early v0.3); sequence grant tested via give.txt, awaiting a real gear chest |
| Forge | Works (user-tested): materials spent, result added |
| Giving any item (give.txt) | Works: Squared Up (`AUG_MIL_SquareWave`) added, survives a game restart. All 12 Mileena augments at Lvl. 4 owned and confirmed equippable in Kustomize |
| Time Krystals | Never touched |
| Boot error 2123-0011 | Removed: the ShowError call in OnlineSystemGetEndpoints::FiberExecute is skipped |
| "Unable to sync your online inventory" (Kustomize, fights) | Removed: the Yes/No boxes are answered Yes without being shown |
| Online modes | Unchanged: the patches only affect the Krypt entry path and the offline prompts above |

## Install (already on the test console)

```
atmosphere/contents/0100F2200C984000/exefs/subsdk9    the module
atmosphere/contents/0100F2200C984000/exefs/main.npdm  game npdm + svcs 0x40/0x43/0x74/0x75/0x77/0x78 + full fs access
mk11offline/config.ini                                optional settings
```

Release copies: `mod/releases/v0.3-materials/` (current), `mod/releases/v0.2-give-items/`, `mod/releases/v0.1-offline-krypt/`. To uninstall, delete the `exefs` folder.

## Files the module writes on the SD card (`sdmc:/mk11offline/`)

- `krypt.dat`: the Krypt progress plus the wallet, one `key<TAB>json` line each. Readable and editable.
  `krypt.bak` is the previous version. The wallet lines are `wallet/Exp_Koins`, `wallet/Exp_SoulFragments`,
  `wallet/Exp_BrutalityHearts`, and one `wallet/<item>` line per crafting material or Konsumable
  (e.g. `wallet/CopperPlating`).
- `inventory_batches.jsonl`: every reward batch the Krypt would have sent to the server, one per line.
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
- **Crashes:** Atmosphère writes no crash reports for MK11 on this console. Use the module log, and
  `crash.cpp`, which hasn't caught anything yet.
- **Boot:** 2123-0011 came from `nn::err::ShowError` in `OnlineSystemGetEndpoints::FiberExecute` (main+0x1937890) after the
  endpoint lookup fails; the module skips that call. `system_dialogs.cpp` logs every SDK error dialog with its caller.
