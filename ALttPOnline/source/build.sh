#!/bin/bash
# Build with MSYS2:  C:\msys64\usr\bin\bash.exe -l build.sh
export MSYSTEM=MINGW64
export PATH=/mingw64/bin:/usr/bin:$PATH
cd "$(dirname "$0")" || exit 1
make -j8 CC=gcc OS=Windows_NT "$@" 2>&1 | grep -v "^make"
status=${PIPESTATUS[0]}
if [ "$status" = "0" ] && [ -f alttpo.exe ] && [ -d ../game-win64 ]; then
  cp alttpo.exe "../game-win64/ALttP Online.exe" 2>/dev/null || echo "note: could not copy the exe (is the game running?)"
fi
exit $status
