#!/bin/bash
# A player leaves a room without using the game's own Save and Quit and comes back: rupees, bombs and
# arrows (which are personal, so nobody else can give them back) have to be what they were.
# A hosts and stays. B joins and gets 300 rupees, 7 bombs and 15 arrows written into its game. Then B
# leaves, the program is started again with the same save folder and joins the same room.
# Usage: tools/tsave.sh [quit|menu|start] [VAR=value for all]
#   quit   B's program is closed (the default)        menu   B chooses LEAVE ROOM in the ESC menu first
#   start  B's program is closed while the game's item menu is open
# GAME=<seed> picks the game (default original-english; "original" is the Japanese ROM); a started file
# for it has to be in ref/testsaves/<seed>.srm.  OLD_B=<older "ALttP Online.exe"> runs B with that build.
S="$(cd "$(dirname "$0")" && pwd)"
A="$(cd "$S/../../.." && pwd)"
R="$A/ref/tests"
how=${1:-quit}; shift
GAME=${GAME:-original-english}
CODE=S$(printf "%04d" $((RANDOM % 10000)))
for p in a b; do rm -rf "$R/save_$p"; mkdir -p "$R/save_$p/saves"; cp "$A/ref/testsaves/$GAME.srm" "$R/save_$p/saves/"; done
mkdir -p "$A/ref/testbin"; cp -u "$A/ALttPOnline/source/alttpo.exe" "$A/ref/testbin/ALttP Online.exe"
PEEK="10,f360,f361,f343,f377"
show() { echo "$1" | awk -F'[][ =]+' '{printf "module %s, rupees %d, bombs %d, arrows %d", $5, strtonum("0x" $9 $7), strtonum("0x" $11), strtonum("0x" $13)}'; }
UI=""; if [ "$how" = menu ]; then UI="1800:menu,1810:down,1815:down,1820:down,1825:down,1830:ok"; fi
# Start picks the file; the A button answers a randomizer's "where do you want to start" with its first choice
BJ="1100:8,1110:0,1300:8,1310:0,1450:100,1460:0"
BI="$BJ"; if [ "$how" = start ]; then BI="$BI,1800:8,1810:0"; fi
bash "$S/t.sh" save_a 4900 0 ALTTPO_AUTO=host ALTTPO_GAME=$GAME ALTTPO_ROOM=$CODE ALTTPO_NAME=Alice ALTTPO_COLOR=2 \
  "ALTTPO_INPUTS=500:8,510:0,700:8,710:0,850:100,860:0" "$@" > /dev/null &
sleep 2
bash "$S/t.sh" save_b 1900 0 ALTTPO_AUTO=join:$CODE ALTTPO_NAME=Bob ALTTPO_COLOR=1 "ALTTPO_INPUTS=$BI" \
  "ALTTPO_POKE=1700:f360=2c,1700:f361=01,1700:f362=2c,1700:f363=01,1700:f343=07,1700:f377=0f" "ALTTPO_UI=$UI" "ALTTPO_PEEK=$PEEK" "TEST_EXE=$OLD_B" "$@" > /dev/null
grep peek "$R/save_b/alttpo.log" | tail -1 | awk -F'[][ =]+' '{print "B when it closes:    module " $5}'
before=$(grep peek "$R/save_b/alttpo.log" | awk -F'[][ ]+' '$2 <= 1790' | tail -1)
echo "B before leaving:    $(show "$before")"
grep "saved\|saving" "$R/save_b/alttpo.log" | cut -c1-100
bash "$S/t.sh" save_b 1900 0 ALTTPO_AUTO=join:$CODE ALTTPO_NAME=Bob ALTTPO_COLOR=1 "ALTTPO_INPUTS=$BJ" \
  "ALTTPO_PEEK=$PEEK" "TEST_EXE=$OLD_B" "$@" > /dev/null
after=$(grep peek "$R/save_b/alttpo.log" | tail -1)
echo "B after coming back: $(show "$after")"
wait
if [ "$(show "$before")" = "$(show "$after")" ] && [ "$(show "$after")" = "module 07, rupees 300, bombs 7, arrows 15" ]; then echo "PASS ($how, $GAME, room $CODE)"; else echo "FAIL ($how, $GAME, room $CODE)"; fi
