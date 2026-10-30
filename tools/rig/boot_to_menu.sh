#!/bin/sh
# From the HOME menu with MK11 as the first tile: start it as Thorhax and click through to the main menu.
cd "$(dirname "$0")"
S=/tmp/claude-0/-root-tmp-claudecode-switch-claude-mk11/289d10f5-214c-4b84-8a3a-056213994f8c/scratchpad
python3 -I sb.py "click A"; sleep 4      # start
python3 -I sb.py "click A"; sleep 45     # user picker -> 2123-0011 dialog
python3 -I sb.py "click A"; sleep 33     # dismiss -> title screen
python3 -I sb.py "click A"; sleep 30     # title (or attract demo)
python3 -I sb.py "click A"; sleep 20     # attract demo -> main menu (harmless on the menu: opens Konquer)
python3 -I sb.py shot $S/menu.jpg
