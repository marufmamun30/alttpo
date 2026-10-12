#!/bin/bash
# Warp to Player: B opens the ESC menu and warps to A. Afterwards B has to stand where A stands, with
# the game running normally (B then walks on, through a door or to the next area).
# Usage: tools/twarp.sh [mode] [VAR=value for both]
#   out     A stands outside Link's house, B is in the house   (indoors to the overworld)
#   west    A is one area further west, B outside the house    (overworld to overworld)
#   castle  A is two rooms into Hyrule Castle, B in the house  (into a dungeon room, then back through its door)
#   far     like castle, but B is outside the house
#   house   A is in Link's house, B outside                    (into a house, then out of its door)
#   hut     A went into a hut from outside Link's house, B is in the house; then both walk out of the hut
#           and have to come out at the same spot (the game remembers where a hut was entered)
#   dark    A is in the Dark World, B in the house in the Light World
#   bunny   like dark, but nobody has the moon pearl: A is a bunny, and B has to arrive as one
#   light   B is in the Dark World as a bunny and warps to A in Link's house: B has to be Link again
#   escort  B has somebody following and must not be able to warp
# GAME=<seed> picks the game (default original-english); a started file for it has to be in
# ref/testsaves/<seed>.srm.   OLD_A=<older "ALttP Online.exe"> runs A with that build (B then gets no
# answer and stays where it is).
# A gets into buildings with what the game does when Link walks into a door, and into the Dark World
# with what it does when he steps on a warp tile (see the README); sprites can not hurt the players.
S="$(cd "$(dirname "$0")" && pwd)"
A="$(cd "$S/../../.." && pwd)"
R="$A/ref/tests"
mode=${1:-out}; shift
GAME=${GAME:-original-english}
OFF=${OFF:-119}        # B is started 2 seconds after A: its frame counter is this far behind
G=1100                 # A's frame from which A acts (its file is loaded)
H=$((1800 - OFF))      # B's frame from which B acts
T=2300                 # A's frame at which B warps
if [ "$mode" = light ]; then T=3000; fi
W=$((T + 420 - OFF))   # B's frame at which B walks on after the warp
END=$((T + 900))
safe() { local s=""; for f in $(seq $2 10 ${3:-$END}); do s="$s,$((f - $1)):37b=01"; done; echo "${s#,}"; } # <offset> <from> [to]: sprites do not hurt
door() { echo "$1:10e=$2,$1:10c=06,$1:11=00,$1:b0=00,$1:10=0f"; }                                          # <frame> <entrance>: through that door
todark() { echo "$1:f357=0$2,$1:7b=00,$1:2db=01,$1:b0=00,$1:5d=14,$1:11=23"; }                              # <frame> <moon pearl>: into the other world
NORAIN="950:f3c5=02"                                 # the story is past the rainy night (the team shares that)
outside="$H:20,$((H + 260)):0"                       # B walks out of the house
AI=""; AP="$(safe 0 $G)"; BI=""; BP="$(safe $OFF $G)"
case $mode in
  out)    AI="$G:20,$((G + 300)):0";                                  BI="$W:40,$((W + 280)):0" ;;
  west)   AI="$G:20,$((G + 360)):0,$((G + 370)):40,$((G + 750)):0";   BI="$outside,$W:40,$((W + 280)):0" ;;
  castle) AP="$(door $G 05),$AP"; AI="$((G + 300)):10,$((G + 800)):0"; BI="$W:20,$((W + 340)):0" ;;
  far)    AP="$(door $G 05),$AP"; AI="$((G + 300)):10,$((G + 800)):0"; BI="$outside,$W:20,$((W + 340)):0" ;;
  house)  AI="$G:10,$((G + 30)):0";                                   BI="$outside,$W:20,$((W + 340)):0" ;;
  hut)    AI="$G:20,$((G + 300)):0,$((W + OFF)):20,$((W + OFF + 340)):0"; AP="$(safe 0 $G $((G + 390))),$(door $((G + 400)) 4b),$(safe 0 $((G + 400)))"
          BI="$W:20,$((W + 340)):0" ;;
  dark)   AI="$G:20,$((G + 300)):0"; AP="$NORAIN,$(safe 0 $G $((G + 390))),$(todark $((G + 400)) 1),$(safe 0 $((G + 400)))"; BI="$W:40,$((W + 80)):0" ;;
  bunny)  AI="$G:20,$((G + 300)):0"; AP="$NORAIN,$(safe 0 $G $((G + 390))),$(todark $((G + 400)) 0),$(safe 0 $((G + 400)))"; BI="$W:40,$((W + 80)):0" ;;
  light)  AP="$NORAIN,$AP"; BI="$outside,$W:20,$((W + 340)):0"; BP="$(safe $OFF $G $((H + OFF + 390))),$(todark $((H + 400)) 0),$(safe $OFF $((H + OFF + 400)))" ;;
  escort) BP="$H:f3cc=01,$(safe $OFF $((H + OFF)))" ;;
esac
UI="$((T - OFF)):menu,$((T + 10 - OFF)):down,$((T + 20 - OFF)):ok,$((T + 30 - OFF)):ok"
CODE=W$(printf "%04d" $((RANDOM % 10000)))
for p in a b; do rm -rf "$R/warp_$p"; mkdir -p "$R/warp_$p/saves"; cp "$A/ref/testsaves/$GAME.srm" "$R/warp_$p/saves/"; done
mkdir -p "$A/ref/testbin"; cp -u "$A/ALttPOnline/source/alttpo.exe" "$A/ref/testbin/ALttP Online.exe"
PEEK="10,11,1b,a0,a1,8a,20,21,22,23,5d,f36d,f3ca"
# Start picks the file; A answers a randomizer's "where do you want to start" with its first choice
bash "$S/t.sh" warp_a $END 60 ALTTPO_AUTO=host ALTTPO_GAME=$GAME ALTTPO_ROOM=$CODE ALTTPO_NAME=Alice ALTTPO_COLOR=2 \
  "ALTTPO_INPUTS=500:8,510:0,700:8,710:0,850:100,860:0${AI:+,$AI}" "ALTTPO_POKE=$AP" "ALTTPO_PEEK=$PEEK" "TEST_EXE=$OLD_A" "$@" > /dev/null &
sleep 2
bash "$S/t.sh" warp_b $((END - OFF)) 30 ALTTPO_AUTO=join:$CODE ALTTPO_NAME=Bob ALTTPO_COLOR=1 \
  "ALTTPO_INPUTS=1100:8,1110:0,1300:8,1310:0,1450:100,1460:0${BI:+,$BI}" "ALTTPO_POKE=$BP" "ALTTPO_UI=$UI" "ALTTPO_PEEK=$PEEK" "$@" > /dev/null &
wait
at() { grep peek "$R/warp_$1/alttpo.log" | awk -F'[][ ]+' -v t=$2 '$2 <= t' | tail -1; }
place() { echo "$1" | awk -F'[][ =]+' '{printf "frame %s module %s.%s %s x %s%s y %s%s %s%s", $2, $5, $7, ($9 == "01" ? "room " $13 $11 : "area " $15), $23, $21, $19, $17, ($29 == "40" ? "dark world" : "light world"), ($25 == "17" ? ", bunny" : "")}'; }
where() { echo "$1" | awk -F'[][ =]+' '{printf "%s %s%s %s%s %s %s", ($9 == "01" ? "room " $13 $11 : "area " $15), $23, $21, $19, $17, $29, $25}'; }
a=$(at a $((T + 60))); before=$(at b $((T - OFF - 10))); arrived=$(at b $((T + 400 - OFF)))
aend=$(grep peek "$R/warp_a/alttpo.log" | tail -1); bend=$(grep peek "$R/warp_b/alttpo.log" | tail -1)
echo "A when B warps:  $(place "$a")"
echo "B before:        $(place "$before")"
echo "B after warping: $(place "$arrived")"
echo "B at the end:    $(place "$bend")"
if [ "$mode" = hut ]; then echo "A at the end:    $(place "$aend")"; fi
grep "note:\|warp:" "$R/warp_b/alttpo.log" | grep -v "joined\|entered\|left the room\|found" | cut -c1-110
grep "note:.*warping" "$R/warp_a/alttpo.log" | cut -c1-110
ok=0
if [ "$mode" = escort ] || [ -n "$OLD_A" ]; then
  [ "$(where "$before")" = "$(where "$arrived")" ] && ok=1; what="B stays where it is"
else
  [ "$(where "$a")" = "$(where "$arrived")" ] && echo "$arrived" | grep -q "00010=0[79] 00011=00" && ok=1; what="B stands where A stands"
  if [ "$mode" = hut ]; then
    # (they stop walking a frame apart, so "the same spot" is within a few pixels)
    what="$what and both leave the hut to the same spot"
    xy() { echo "$1" | awk -F'[][ =]+' '{print ($9 == "00" ? strtonum("0x" $15) : -1), strtonum("0x" $23 $21), strtonum("0x" $19 $17)}'; }
    read aa ax ay <<< "$(xy "$aend")"; read ba bx by <<< "$(xy "$bend")"
    [ "$aa" != -1 ] && [ "$aa" = "$ba" ] && [ $(( (ax - bx) * (ax - bx) + (ay - by) * (ay - by) )) -le 32 ] || ok=0
  fi
fi
if [ $ok = 1 ]; then echo "PASS ($mode, $GAME: $what, room $CODE)"; else echo "FAIL ($mode, $GAME: not true that $what, room $CODE)"; fi
