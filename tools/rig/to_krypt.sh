#!/bin/sh
# From the 2123-0011 boot dialog: click through to the main menu, then Konquer > Krypt. Screenshot at the end.
cd "$(dirname "$0")"
S=/tmp/claude-0/-root-tmp-claudecode-switch-claude-mk11/289d10f5-214c-4b84-8a3a-056213994f8c/scratchpad
python3 -I sb.py "click A"; sleep 25          # error dialog -> warning/title
python3 -I sb.py "click A"; sleep 8           # title "press A"
python3 -I sb.py "click A"; sleep 20          # into main menu
python3 -I sb.py shot $S/menu.jpg
python3 -I sb.py "click A"; sleep 6           # Konquer
python3 -I sb.py "click DRIGHT" "click DRIGHT" "click DRIGHT"; sleep 1
python3 -I sb.py "click A"; sleep ${1:-10}    # Krypt
python3 -I sb.py shot $S/krypt.jpg
