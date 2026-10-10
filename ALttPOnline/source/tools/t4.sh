#!/bin/bash
# Four players on one PC, everyone on a brand new file. Usage: tools/t4.sh <frames> <seed>
S="$(cd "$(dirname "$0")" && pwd)"
R="$(cd "$S/../../.." && pwd)/ref/tests"
frames=${1:-3000}; game=${2:-original-english}
CODE=F$(printf "%04d" $((RANDOM % 10000)))
NEWFILE="500:8,510:0,700:8,710:0,800:100,810:0,830:100,840:0,860:100,870:0,900:8,910:0,1100:8,1110:0,1300:8,1310:0,1500:100,1510:0,1600:100,1610:0,1700:100,1710:0,1800:100,1810:0,1900:100,1910:0,2000:100,2010:0"
names=(Alice Bob Carol Dave)
moves=("2300:40,2330:0" "2300:80,2330:0" "2300:20,2330:0" "2300:20,2320:40,2350:0")
for i in 0 1 2 3; do
  d="net4_$i"; rm -rf "$R/$d"
  if [ $i = 0 ]; then auto="host"; else auto="join:$CODE"; fi
  bash "$S/t.sh" $d $frames 500 ALTTPO_AUTO=$auto ALTTPO_GAME=$game ALTTPO_ROOM=$CODE ALTTPO_NAME=${names[$i]} ALTTPO_COLOR=$i \
    "ALTTPO_INPUTS=$NEWFILE,${moves[$i]}" "ALTTPO_PEEK=10,11,f359,f36d,f3c5" &
  sleep 1.5
done
wait
echo "room $CODE"
