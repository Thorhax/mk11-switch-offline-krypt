#!/bin/sh
# Builds mk11dump.nro with the devkitPro copy in shaolinmonks/work.
set -e
cd "$(dirname "$0")"
export DEVKITPRO=/root/tmp/claudecode/switch-claude/shaolinmonks/work/devkitpro
B=$DEVKITPRO/devkitA64/bin; T=$DEVKITPRO/tools/bin
mkdir -p build
$B/aarch64-none-elf-gcc -O2 -Wall -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
  -D__SWITCH__ -I$DEVKITPRO/libnx/include source/main.c \
  -specs=$DEVKITPRO/libnx/switch.specs -L$DEVKITPRO/libnx/lib -lnx -o build/mk11dump.elf
$T/nacptool --create "mk11dump" "Thorhax" "1.0" build/mk11dump.nacp
$T/elf2nro build/mk11dump.elf build/mk11dump.nro --nacp=build/mk11dump.nacp --icon=$DEVKITPRO/libnx/default_icon.jpg
ls -l build/mk11dump.nro
