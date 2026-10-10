#!/bin/bash
# Version interop test: an older build hosts, the current build joins (and the other way round).
# Usage: tools/tver.sh "<path to the old ALttP Online.exe>" [frames]
# The old exe runs from its own folder but keeps its log, config and saves in ref/tests/ver_old.
A="$(cd "$(dirname "$0")/../../.." && pwd)"   # the folder that holds ALttPOnline and ref
S="$A/ALttPOnline/source/tools"
OLD="$1"; frames=${2:-1500}
for round in oldhost newhost; do
  CODE=V$(printf "%04d" $((RANDOM % 10000)))
  rm -rf "$A/ref/tests/ver_old" "$A/ref/tests/ver_new"; mkdir -p "$A/ref/tests/ver_old"
  if [ $round = oldhost ]; then oldauto=host; newauto="join:$CODE"; else oldauto="join:$CODE"; newauto=host; fi
  ( export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ALTTPO_DATA="$A/ref/tests/ver_old" ALTTPO_NO_SAVE_CFG=1 ALTTPO_EXIT_AFTER=$frames \
      ALTTPO_AUTO=$oldauto ALTTPO_GAME=original-english ALTTPO_ROOM=$CODE ALTTPO_NAME=Old
    if [ $round = newhost ]; then sleep 2; fi
    "$OLD" > /dev/null 2>&1 ) &
  ( if [ $round = oldhost ]; then sleep 2; fi
    bash "$S/t.sh" ver_new $frames 0 ALTTPO_AUTO=$newauto ALTTPO_GAME=original-english ALTTPO_ROOM=$CODE ALTTPO_NAME=New > /dev/null ) &
  wait
  echo "=== $round (room $CODE)"
  echo "--- old build:"; grep -v "peek\|rando:" "$A/ref/tests/ver_old/alttpo.log" | cut -c1-150
  echo "--- new build:"; grep -v "peek\|rando:" "$A/ref/tests/ver_new/alttpo.log" | cut -c1-150
done
