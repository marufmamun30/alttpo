#!/bin/bash
# Route finding helper: runs one player unthrottled with a movement script and prints where Link ends up.
# Usage: tools/nav.sh <frames> <inputs after the file is loaded> [sheet frames...]
# Uses seed "basic1" and the save in ref/tests/solo1 (see t2.sh).
R="/c/Users/Maruf/Documents/Claude Code/ALTTP-Online/ref/tests"
S="/c/Users/Maruf/Documents/Claude Code/ALTTP-Online/ALttPOnline/source/tools"
frames=$1; inputs=$2; shift 2
rm -rf "$R/nav"; mkdir -p "$R/nav/saves"; cp "$R/solo1/saves/basic1.srm" "$R/nav/saves/"
ALTTPO_FAST=1 bash "$S/t.sh" nav $frames 50 ALTTPO_AUTO=solo ALTTPO_GAME=basic1 \
  "ALTTPO_INPUTS=${START:-500:8,510:0,700:8,710:0,1100:8,1110:0},$inputs" "ALTTPO_PEEK=10,11,1b,8a,a0,20,21,22,23,f36d" > /dev/null
grep "peek" "$R/nav/alttpo.log" | awk -F'[][ =]+' '{printf "%s m%s.%s in%s ow%s uw%s y%s%s x%s%s hp%s\n", $2,$5,$7,$9,$11,$13,$17,$15,$21,$19,$23}' | tail -n ${TAIL:-12}
if [ $# -gt 0 ]; then
  cd "$R/nav/shots"
  files=""; for f in "$@"; do files="$files f$(printf %06d $f).png"; done
  python "$S/sheet.py" ../sheet.png 4 50 $files
fi
