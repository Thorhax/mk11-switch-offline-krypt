# MK11 (Switch) offline Krypt

An exlaunch module (`subsdk9`) that lets Mortal Kombat 11 Ultimate 1.0.26 on Switch
(title 0100F2200C984000, build ID 4F7AB1A3EEFC7EB2) enter, play and save the Krypt with no
server. Krypt progress and the Koins/Souls/Hearts wallet are stored on the SD card in
`sdmc:/mk11offline/`. It can also add any gear or augment to your inventory through a give list
(`sdmc:/mk11offline/give.txt`), which also takes currency and item amounts (`Exp_Koins 100000`). Online modes are not touched and paid DLC is never unlocked. Time Krystals can be earned offline
(level-ups, Krypt chests) and spent in the offline store.

The status, design and open problems are in [NOTES.md](NOTES.md).

## Layout

- `mod/exl/source/program/`: the module source (hooks, Krypt store, batch apply, config, log)
- `mod/offsets.txt`: symbol names, turned into `mk11_offsets.hpp` by `tools/gen_offsets.py`
- `mod/build.sh`: compiler-only build (devkitA64 + elf2nso)
- `mod/releases/`: release builds (`main.npdm` is not committed, because it is derived from the game's npdm. `build.sh` regenerates it)
- `tools/`: NSO, disassembly and xref tools, and the test-rig helpers in `tools/rig/`

## Building

`mod/build.sh` needs devkitPro and the game's own `exefs/main.npdm`, dumped from your own copy
of the game. No game files are included in this repository.

`tools/rig/ftp.py` reads the console address and login from `tools/rig/rig.local.json`, which is not committed.

## License

`mod/exl` is based on exlaunch; see `mod/exl/LICENSE`.
