#!/bin/bash
# Two player online test on one PC. Usage: tools/t2.sh <frames> [extra VAR=value for both]
# Needs a seed called "basic1" in game-win64/seeds and a save with a started file in ref/tests/solo1/saves/basic1.srm.
# A_INPUTS / B_INPUTS, A_POKE / B_POKE and PEEK replace the default script; SHOTS sets the screenshot interval.
S="$(cd "$(dirname "$0")" && pwd)"
R="$(cd "$S/../../.." && pwd)/ref/tests"
frames=${1:-3600}; shift
CODE=T$(printf "%04d" $((RANDOM % 10000)))
for p in a b; do rm -rf "$R/net_$p"; mkdir -p "$R/net_$p/saves"; cp "$R/solo1/saves/basic1.srm" "$R/net_$p/saves/"; done
START_A="500:8,510:0,700:8,710:0,1100:8,1110:0"
START_B="1100:8,1110:0,1300:8,1310:0,1700:8,1710:0"
bash "$S/t.sh" net_a $frames ${SHOTS:-300} ALTTPO_AUTO=host ALTTPO_GAME=basic1 ALTTPO_ROOM=$CODE ALTTPO_NAME=Alice ALTTPO_COLOR=2 \
  "ALTTPO_INPUTS=$START_A,${A_INPUTS:-2000:40,2040:0}" "ALTTPO_POKE=${A_POKE:-2400:f34a=01,2400:f359=01,2460:f375=05}" \
  "ALTTPO_PEEK=${PEEK:-10,f34a,f359,f343,f36f,f37c,f36c,f36d}" "$@" &
sleep 2
bash "$S/t.sh" net_b $frames ${SHOTS:-300} ALTTPO_AUTO=join:$CODE ALTTPO_NAME=Bob ALTTPO_COLOR=1 \
  "ALTTPO_INPUTS=$START_B,${B_INPUTS:-2300:80,2340:0}" "ALTTPO_POKE=${B_POKE:-}" \
  "ALTTPO_PEEK=${PEEK:-10,f34a,f359,f343,f36f,f37c,f36c,f36d}" "$@" &
wait
echo "room $CODE"
