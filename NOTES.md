# MK11 (Switch) offline Krypt

Goal: play the Krypt on the console with no server. MK11 Ultimate 1.0.26, title 0100F2200C984000,
main build ID 4F7AB1A3EEFC7EB2. Everything below is specific to that build.

## Status (2026-10-31)

| Area | State |
|---|---|
| Entering the Krypt offline | Works |
| Krypt progress (chests opened, map, intro, unlocked areas) saved and restored | Works, survives game restarts |
| Koins / Souls / Hearts balances offline | Works (on-console wallet) |
| Chest costs deducted | Works |
| Chest unlockables (concept art etc.) granted | Works (local profile unlock bits) |
| Gear rewards from chests | **Not yet**: recorded only. Granting them crashes the game (see below) |
| Time Krystals | Never touched |
| Online modes | Unchanged: the patches only affect the Krypt entry path |

## Install (already on the test console)

```
atmosphere/contents/0100F2200C984000/exefs/subsdk9    the module
atmosphere/contents/0100F2200C984000/exefs/main.npdm  game npdm + svcs 0x40/0x43/0x74/0x75/0x77/0x78 + full fs access
mk11offline/config.ini                                optional settings
```

Release copy: `mod/releases/v0.1-offline-krypt/`. To uninstall, delete the `exefs` folder.

## Files the module writes on the SD card (`sdmc:/mk11offline/`)

- `krypt.dat`: the Krypt progress plus the wallet, one `key<TAB>json` line each. Readable and editable.
  `krypt.bak` is the previous version. The wallet lines are `wallet/Exp_Koins`, `wallet/Exp_SoulFragments`
  and `wallet/Exp_BrutalityHearts`.
- `inventory_batches.jsonl`: every reward batch the Krypt would have sent to the server, one per line.
- `log.txt`: the module's log for the current run.
- `replay_batch.json` (if you create it): applied once at the next Krypt save, then deleted.

`config.ini`:

```
krypt_currency_topup=0   # >0: top Koins/Souls/Hearts up to this amount on each Krypt start
apply_gear_rewards=0     # 1: grant gear from chests locally (currently crashes)
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
5. **Rewards.** The `FGOnlineInventoryBatch` passed to `Save` is serialised with `ToOnlineValue` and
   applied by `batch_apply.cpp`:
   - `inc_consumable` → wallet
   - `game_unlockables` → `MK11Unlockables::Unlock(GMK11Unlockables, user, bit, page)`
   - `add_game_item` → gear (gated)

## Gear rewards: open problem

`add_game_item` entries look like `{"item_slug":"<definition GUID as %08X x4>","user_data":...}`.
`batch_apply.cpp:AddGear` mirrors `MK11Unlockables::Unlock(FGUserEntity*, FName, ...)`:
`FindItemDefinition` → `GearItem(handle, 1)` → `InventoryManager::AddItem` with a `BILootDropSourceData`
and an `EquipmentRefType`. A replay with three GUIDs taken from the save closed the game before
`FindItemDefinition` returned (no log line after "gear ... ..."), and the module's user-exception
handler logged nothing, so it was probably an abort rather than a CPU fault.

Leads to try first:
- whether those 16 bytes in the save really are definition GUIDs;
- `ProcSafeStackObject` inside `BILootDropSourceData`, which may assert on the stack it lives on;
- stack size: `AddGear` puts about 1.1 KB of buffers on the Krypt proc's stack.

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
- **Boot:** every boot shows error 2123-0011, which is harmless (Nintendo servers are blocked). Press OK.
