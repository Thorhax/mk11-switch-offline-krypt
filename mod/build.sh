#!/bin/sh
# Builds the MK11 offline module (exl/ sources) into deploy/subsdk9 without running any
# third-party scripts: compiler, linker and elf2nso only. main.npdm is produced by
# tools/npdm_addsvc.py from the game's own npdm.
set -e
cd "$(dirname "$0")"
DKP=/root/tmp/claudecode/switch-claude/shaolinmonks/work/devkitpro
BIN=$DKP/devkitA64/bin/aarch64-none-elf
S=exl/source
O=exl/build; mkdir -p $O deploy
python3 -I ../tools/gen_offsets.py
INC="-I$S -I$S/lib -I$S/nn -I$S/program -I$S/rtld -I$DKP/libnx/include"
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC -fvisibility=hidden"
DEF="-D__SWITCH__ -D__RTLD_6XX__ -DEXL_LOAD_KIND=Module -DEXL_LOAD_KIND_ENUM=2 -DEXL_PROGRAM_ID=0x0100F2200C984000"
CF="-g -Wall -Werror -O3 -ffunction-sections -fdata-sections -Wno-format-zero-length $ARCH $DEF $INC"
CXF="$CF -fno-rtti -fno-exceptions -fno-asynchronous-unwind-tables -fno-unwind-tables -std=gnu++23"
OBJS=""
for f in $(find $S -name '*.cpp' -o -name '*.c' -o -name '*.s'); do
  o=$O/$(echo "$f" | tr / _).o; OBJS="$OBJS $o"
  if [ ! -e "$o" ] || [ "$f" -nt "$o" ] || [ -n "$(find $S -name '*.hpp' -newer "$o" | head -1)" ]; then
    case "$f" in
      *.cpp) $BIN-g++ $CXF -c "$f" -o "$o" ;;
      *.c)   $BIN-gcc $CF -c "$f" -o "$o" ;;
      *.s)   $BIN-gcc -x assembler-with-cpp -g $ARCH $DEF $INC -c "$f" -o "$o" ;;
    esac
  fi
done
cd $O
$BIN-g++ -specs=../misc/specs/module.specs -g $ARCH -nostartfiles -Wl,-Map,mk11offline.map $(for o in $OBJS; do basename $o; done) -o mk11offline.elf
cd ../..
$DKP/tools/bin/elf2nso $O/mk11offline.elf deploy/subsdk9
python3 -I ../tools/npdm_addsvc.py ../exefs/main.npdm deploy/main.npdm 0x40 0x43 0x74 0x75 0x77 0x78
ls -l deploy
