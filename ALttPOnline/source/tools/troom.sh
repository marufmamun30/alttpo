#!/bin/bash
# Two players walk from one castle room into the next, B a few frames behind A, after the guards of the
# first room were killed. Sprite numbers start at 0 in every room, so this checks that the list of dead
# enemies of the room that was left is not applied to the room that is entered: the guards of the second
# room have to be there at the end, in both games.
# Usage: tools/troom.sh [door|stairs|late] [gap in frames] [VAR=value for both]
#   door    east lobby (room 62) -> east hall (52) through a doorway. The game is told that shutter doors
#           were opened in the lobby, so it pauses after arriving the way it does for a door that shuts
#           behind Link. Guards 0, 1 and 2 on both sides.
#   stairs  lobby (61) -> throne room (51) up the stairs. Guards 1 and 2 on both sides.
#   late    the opposite check, same way as "door": A also kills the guards of the hall, B comes in 300
#           frames later and has to find them dead (the list is still passed on where it belongs).
# OLD_A / OLD_B: an older "ALttP Online.exe" to run as player A / B (mixed versions).
# Needs seed "original-english" and a started file for it in ref/testsaves/original-english.srm.
# The players are put into the castle with ALTTPO_POKE (the game's own "walk through door number N")
# and sprites cannot hurt them, so the walk is the same every time.
S="$(cd "$(dirname "$0")" && pwd)"
A="$(cd "$S/../../.." && pwd)"
R="$A/ref/tests"
mode=${1:-door}; gap=$2; shift; shift
ent=05; room=52; want="0 1 2"; expect=09; wrong="is missing"
if [ "$mode" = stairs ]; then ent=04; room=51; want="1 2"; fi
if [ "$mode" = late ]; then gap=${gap:-300}; expect=00; wrong="is still there"; fi
gap=${gap:-3}
OFF=${OFF:-119}   # B is started 2 seconds after A: its frame counter is this far behind
T0=1700           # A's frame at which both walk into the castle; both are in their file by then
END=$((T0 + 1000 + (gap > 0 ? gap : 0)))
script() { # <how far this player's frame counter is behind A's> <extra frames before walking>
  local sh=$1 e=$((T0 - $1)) k=$((T0 + 250 - $1)) w=$((T0 + 300 - $1 + $2))
  POKES="$e:10e=$ent,$e:10c=06,$e:11=00,$e:b0=00,$e:10=0f,$k:dd0=00,$k:dd1=00,$k:dd2=00"
  if [ "$mode" != stairs ]; then POKES="$POKES,$((w + 370)):468=00"; fi
  if [ "$mode" = late ] && [ $sh = 0 ]; then POKES="$POKES,$((w + 500)):dd0=00,$((w + 500)):dd1=00,$((w + 500)):dd2=00"; fi
  for f in $(seq $((T0 + 150)) 10 $END); do POKES="$POKES,$((f - sh)):37b=01"; done
  INPUTS="$w:10,$((w + 600)):0"
}
CODE=D$(printf "%04d" $((RANDOM % 10000)))
for p in a b; do rm -rf "$R/room_$p"; mkdir -p "$R/room_$p/saves"; cp "$A/ref/testsaves/original-english.srm" "$R/room_$p/saves/"; done
mkdir -p "$A/ref/testbin"; cp -u "$A/ALttPOnline/source/alttpo.exe" "$A/ref/testbin/ALttP Online.exe"
PEEK="10,11,a0,dd0,dd1,dd2,bc0,bc1,bc2"
script 0 0
bash "$S/t.sh" room_a $END 100 ALTTPO_AUTO=host ALTTPO_GAME=original-english ALTTPO_ROOM=$CODE ALTTPO_NAME=Alice ALTTPO_COLOR=2 \
  "ALTTPO_INPUTS=500:8,510:0,700:8,710:0,$INPUTS" "ALTTPO_POKE=$POKES" "ALTTPO_PEEK=$PEEK" ALTTPO_ENEMY_LOG=30 "TEST_EXE=$OLD_A" "$@" > /dev/null &
sleep 2
script $OFF $gap
bash "$S/t.sh" room_b $END 100 ALTTPO_AUTO=join:$CODE ALTTPO_NAME=Bob ALTTPO_COLOR=1 \
  "ALTTPO_INPUTS=1100:8,1110:0,1300:8,1310:0,$INPUTS" "ALTTPO_POKE=$POKES" "ALTTPO_PEEK=$PEEK" ALTTPO_ENEMY_LOG=30 "TEST_EXE=$OLD_B" "$@" > /dev/null &
wait
ok=1
for p in a b; do
  log="$R/room_$p/alttpo.log"
  t=$(grep "place 100$room running" "$log" | head -1 | sed 's/.*t=\([0-9]*\).*/\1/')
  echo "--- $p ($(head -1 "$log" | cut -c12-)), in room $room at ${t:-?} ms"
  eval "t_$p=\${t:-0}"
  grep "enemy:" "$log" | grep -v " t=" | cut -c1-110
  last=$(grep peek "$log" | tail -1)
  echo "$last" | awk -F'[][ =]+' '{printf "    end: module %s.%s room %s | sprite state %s %s %s | number %s %s %s\n", $5,$7,$9,$11,$13,$15,$17,$19,$21}'
  [ "$(echo "$last" | awk -F'[][ =]+' '{print $9}')" = "$room" ] || { echo "    did not get to room $room"; ok=0; }
  for k in $want; do
    [ "$(echo "$last" | awk -F'[][ =]+' -v c=$((11 + 2 * k)) '{print $c}')" = "$expect" ] || { echo "    guard $k $wrong"; ok=0; }
  done
done
if [ $t_a != 0 ] && [ $t_b != 0 ]; then when="B arrived $(( (10#$t_b - 10#$t_a) * 6 / 100 )) frames after A"; else when="arrival times unknown (older build)"; fi
if [ $ok = 1 ]; then echo "PASS ($mode, $when, room $CODE)"; else echo "FAIL ($mode, $when, room $CODE)"; fi
