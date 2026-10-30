#!/bin/sh
# From the HOME menu (MK11 second tile), start MK11 as Thorhax, then wait and screenshot.
cd "$(dirname "$0")"
S=/tmp/claude-0/-root-tmp-claudecode-switch-claude-mk11/289d10f5-214c-4b84-8a3a-056213994f8c/scratchpad
python3 -I sb.py "click HOME"; sleep 2
python3 -I sb.py "click DLEFT" "click DLEFT" "click DLEFT" "click A"; sleep 4
python3 -I sb.py "click A"; sleep ${1:-30}
python3 -I sb.py shot $S/launch.jpg
