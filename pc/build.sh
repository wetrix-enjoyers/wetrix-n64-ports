#!/bin/bash
# build.sh - the Windows build, in build/win (MSYS2 UCRT64, Ninja, Release).
# Runs the recompilation's tools/regen.py first, so a change anywhere from the
# disassembly down is built; WETRIX_NO_REGEN=1 skips that. The recompilation and
# Wetter are found (or cloned into ../deps) by ../tools/fetch_deps.py. It also installs
# the MSYS2 packages it needs if they are missing (WETRIX_NO_DEPS=1 skips that).
#
#   ./build.sh          incremental build
set -e
cd "$(dirname "$0")"

eval "$(python ../tools/fetch_deps.py --shell)"   # WETRIX_RECOMP_DIR, WETTER_DIR
[ -n "$WETRIX_NO_REGEN" ] || python "$WETRIX_RECOMP_DIR/tools/regen.py"

HERE="$(pwd -W 2>/dev/null || pwd)"
MSYSTEM=UCRT64 /c/msys64/usr/bin/bash -lc "
  set -e
  [ -n \"\$WETRIX_NO_DEPS\" ] || pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-SDL2
  cd \"\$(cygpath -u '$HERE')\"
  cmake -S . -B build/win -G Ninja -DCMAKE_BUILD_TYPE=Release -DWETTER_DEBUGINFO=ON -DWETRIX_RECOMP_DIR='$WETRIX_RECOMP_DIR' -DWETTER_DIR='$WETTER_DIR'
  cmake --build build/win --parallel"
