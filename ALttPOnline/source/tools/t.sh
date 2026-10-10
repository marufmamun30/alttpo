#!/bin/bash
# Headless test run:  tools/t.sh <name> <frames> <shotEvery> [VAR=value ...]
# Runs the freshly built source/alttpo.exe from its own folder (ref/testbin), so the installed game
# in game-win64 is not touched and may even be running. Seeds named in ALTTPO_GAME are fetched from
# ref/testseeds or game-win64/seeds. Results (log, screenshots, saves) go to ref/tests/<name>/.
# TEST_EXE=<path> runs another build instead (an older version, for example); it is copied to ref/testbin too.
A="$(cd "$(dirname "$0")/../../.." && pwd)"   # the folder that holds ALttPOnline and ref
G="$A/ref/testbin"
T="$A/ref/tests/$1"
name=$1; frames=$2; every=$3; shift 3
mkdir -p "$T/shots" "$G/seeds"
rm -f "$T/shots/"*.png
cp -u "$A/ALttPOnline/source/alttpo.exe" "$G/ALttP Online.exe" 2>/dev/null
for kv in "$@"; do export "$kv"; done
if [ -n "$ALTTPO_GAME" ] && [ "$ALTTPO_GAME" != original ] && [ ! -f "$G/seeds/$ALTTPO_GAME.sfc" ]; then
  cp "$A/ref/testseeds/DR_$ALTTPO_GAME.sfc" "$G/seeds/$ALTTPO_GAME.sfc" 2>/dev/null || cp "$A/ALttPOnline/game-win64/seeds/$ALTTPO_GAME.sfc" "$G/seeds/" 2>/dev/null
fi
exe="$G/ALttP Online.exe"
if [ -n "$TEST_EXE" ]; then exe="$G/$(basename "$TEST_EXE")"; cp -u "$TEST_EXE" "$exe"; fi
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ALTTPO_DATA="$T" ALTTPO_SHOT_DIR="$T/shots" ALTTPO_SHOT_EVERY=$every ALTTPO_EXIT_AFTER=$frames ALTTPO_NO_SAVE_CFG=1
"$exe" > "$T/stdout.txt" 2>&1
echo "[$name] exit $?"
