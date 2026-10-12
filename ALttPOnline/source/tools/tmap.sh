#!/bin/bash
# Players on the world map: B opens the game's map and has to see A on it. This one is checked by eye:
# the screenshots are in ref/tests/map_b/shots, and the script prints which of them show the map.
# Usage: tools/tmap.sh [out|in|flute] [VAR=value for both]
#   out     A walks out of Link's house and one area west; B walks out, opens the map (X), zooms once
#   in      A stays in the house: the map shows A at the door of the house
#   flute   like out, but B gets the map the flute's bird brings up
#   three   like in, with a third player who stays in the house too: two names at the same door
# OLD_A=<older "ALttP Online.exe"> runs A with that build (it is then only shown while it is outdoors).
# SHOTS=<n> takes B's screenshots every n frames (the game's own marker for Link blinks).
# Needs seed "original-english" and a started file for it in ref/testsaves/original-english.srm.
S="$(cd "$(dirname "$0")" && pwd)"
A="$(cd "$S/../../.." && pwd)"
R="$A/ref/tests"
mode=${1:-out}; shift
OFF=${OFF:-119}        # B is started 2 seconds after A: its frame counter is this far behind
M=$((2100 - OFF))      # B's frame at which the map comes up
safe() { local s=""; for f in $(seq $2 10 ${3:-2800}); do s="$s,$((f - $1)):37b=01"; done; echo "${s#,}"; } # <offset> <from> [to]: sprites do not hurt
AI="1000:20,1360:0,1370:40,1750:0"; if [ "$mode" = in ] || [ "$mode" = three ]; then AI=""; fi
BI="$((1700 - OFF)):20,$((1960 - OFF)):0"; BP="$(safe $OFF 1000)"
if [ "$mode" = flute ]; then
  # what the game does when the bird has picked Link up
  BP="$(safe $OFF 1000 $((M + OFF - 10))),$M:10c=09,$M:11=0a,$M:200=00,$M:10=0e,$(safe $OFF $((M + OFF)))"
else
  BI="$BI,$M:200,$((M + 10)):0,$((M + 300)):200,$((M + 310)):0"   # X for the map (zoomed in on Link), X again for the whole world
fi
CODE=M$(printf "%04d" $((RANDOM % 10000)))
for p in a b c; do rm -rf "$R/map_$p"; mkdir -p "$R/map_$p/saves"; cp "$A/ref/testsaves/original-english.srm" "$R/map_$p/saves/"; done
mkdir -p "$A/ref/testbin"; cp -u "$A/ALttPOnline/source/alttpo.exe" "$A/ref/testbin/ALttP Online.exe"
PEEK="10,11,200,636,8a,20,21,22,23"
bash "$S/t.sh" map_a 2800 0 ALTTPO_AUTO=host ALTTPO_GAME=original-english ALTTPO_ROOM=$CODE ALTTPO_NAME=Alice ALTTPO_COLOR=2 \
  "ALTTPO_INPUTS=500:8,510:0,700:8,710:0${AI:+,$AI}" "ALTTPO_POKE=$(safe 0 1000)" "ALTTPO_PEEK=$PEEK" "TEST_EXE=$OLD_A" "$@" > /dev/null &
sleep 2
bash "$S/t.sh" map_b $((2800 - OFF)) ${SHOTS:-30} ALTTPO_AUTO=join:$CODE ALTTPO_NAME=Bob ALTTPO_COLOR=1 \
  "ALTTPO_INPUTS=1100:8,1110:0,1300:8,1310:0,$BI" "ALTTPO_POKE=$BP" "ALTTPO_PEEK=$PEEK" "$@" > /dev/null &
if [ "$mode" = three ]; then
  sleep 1.5
  bash "$S/t.sh" map_c 2500 0 ALTTPO_AUTO=join:$CODE ALTTPO_NAME=Carol ALTTPO_COLOR=5 "ALTTPO_INPUTS=1100:8,1110:0,1300:8,1310:0" "$@" > /dev/null &
fi
wait
echo "A: $(grep peek "$R/map_a/alttpo.log" | tail -1 | awk -F'[][ =]+' '{printf "module %s.%s area %s x %s%s y %s%s", $5, $7, $13, $21, $19, $17, $15}')"
grep peek "$R/map_b/alttpo.log" | awk -F'[][ =]+' '$5 == "0e" && ($7 == "07" || $7 == "0a") && $9 == "05" {printf "B shows the map at frame %s (%s)\n", $2, ($11 == "00" ? "whole world" : "zoomed in")}'
echo "room $CODE"
