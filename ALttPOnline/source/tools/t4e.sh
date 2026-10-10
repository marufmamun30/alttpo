#!/bin/bash
# Four players fight the guards west of Link's house (shared enemies under load).
# Usage: tools/t4e.sh <frames> [extra VAR=value for all].  Same seed and save as t2.sh.
S="$(cd "$(dirname "$0")" && pwd)"
R="$(cd "$S/../../.." && pwd)/ref/tests"
frames=${1:-4600}; shift
CODE=E$(printf "%04d" $((RANDOM % 10000)))
names=(Alice Bob Carol Dave)
for i in 0 1 2 3; do
  d="net4_$i"; rm -rf "$R/$d"; mkdir -p "$R/$d/saves"; cp "$R/solo1/saves/basic1.srm" "$R/$d/saves/"
  if [ $i = 0 ]; then auto="host"; t0=1400; start="500:8,510:0,700:8,710:0,1100:8,1110:0"
  else auto="join:$CODE"; t0=2000; start="1100:8,1110:0,1300:8,1310:0,1700:8,1710:0"; fi
  # out of the house, down to the path, west into the next area, then swing the sword
  inputs="$start,$t0:20,$((t0+360)):0,$((t0+370)):40,$((t0+700+i*12)):0"
  for f in $(seq $((t0+760)) 22 $((frames-200))); do inputs="$inputs,$f:1,$((f+6)):0"; done
  bash "$S/t.sh" $d $frames 200 ALTTPO_AUTO=$auto ALTTPO_GAME=basic1 ALTTPO_ROOM=$CODE ALTTPO_NAME=${names[$i]} ALTTPO_COLOR=$i \
    "ALTTPO_INPUTS=$inputs" "ALTTPO_POKE=$((t0-100)):f36c=a0,$((t0-100)):f36d=a0,$((t0-100)):f359=02" "ALTTPO_PEEK=10,11,8a,f36d" ALTTPO_ENEMY_LOG=120 "$@" &
  sleep 1.5
done
wait
echo "room $CODE"
for i in 0 1 2 3; do echo "--- ${names[$i]}"; grep "enemy:" "$R/net4_$i/alttpo.log" | grep -v " t=" | cut -c1-110; done
