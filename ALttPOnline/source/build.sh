#!/bin/bash
# Build with MSYS2:  C:\msys64\usr\bin\bash.exe -l /full/path/to/build.sh [install]
# The result is alttpo.exe in this folder. "install" also copies it to ../game-win64. That is not
# done on every build: the installed game may be in use, and everybody in a room needs the same
# version, so a new one should only go there when it is finished.
export MSYSTEM=MINGW64
export PATH=/mingw64/bin:/usr/bin:$PATH
cd "$(dirname "$0")" || exit 1
install=0
if [ "$1" = install ]; then install=1; shift; fi
make -j8 CC=gcc OS=Windows_NT "$@" 2>&1 | grep -v "^make"
status=${PIPESTATUS[0]}
if [ "$status" = "0" ] && [ $install = 1 ]; then
  if cp alttpo.exe "../game-win64/ALttP Online.exe" 2>/dev/null; then echo "installed in ../game-win64"
  else echo "NOT INSTALLED: could not replace ../game-win64/ALttP Online.exe (is the game running?)"; status=1; fi
fi
exit $status
