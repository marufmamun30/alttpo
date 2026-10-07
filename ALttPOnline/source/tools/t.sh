#!/bin/bash
# Headless test run:  tools/t.sh <name> <frames> <shotEvery> [VAR=value ...]
# Results (log, screenshots, saves) go to ../../ref/tests/<name>/
G="/c/Users/Maruf/Documents/Claude Code/ALTTP-Online/ALttPOnline/game-win64"
T="/c/Users/Maruf/Documents/Claude Code/ALTTP-Online/ref/tests/$1"
name=$1; frames=$2; every=$3; shift 3
mkdir -p "$T/shots"
rm -f "$T/shots/"*.png
for kv in "$@"; do export "$kv"; done
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ALTTPO_DATA="$T" ALTTPO_SHOT_DIR="$T/shots" ALTTPO_SHOT_EVERY=$every ALTTPO_EXIT_AFTER=$frames ALTTPO_NO_SAVE_CFG=1
"$G/ALttP Online.exe" > "$T/stdout.txt" 2>&1
echo "[$name] exit $?"
