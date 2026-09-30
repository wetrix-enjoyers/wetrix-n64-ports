#!/bin/bash
# build.sh - everything the R36S package needs: the aarch64 game
# (../aarch64/build.sh) plus the Select+Start watcher, built in the same
# arm64 container.
#
#   ./build.sh          incremental build
#   ./build.sh clean    remove build/ here and in ../aarch64
#
# Outputs: ../aarch64/build/wetrix.aarch64 and build/wetrix-watch.
set -e
cd "$(dirname "$0")"

PORTS_WIN="$(cd .. && (pwd -W 2>/dev/null || pwd))"
BUILDER="${BUILDER:-wetrix-arm64:noble}"
HERE=/src/ports/r36s

if [ "$1" = "clean" ]; then
  rm -rf build
  bash ../aarch64/build.sh clean
  exit 0
fi
bash ../aarch64/build.sh
mkdir -p build

MSYS_NO_PATHCONV=1 docker run --rm --platform linux/arm64 \
  -v "$PORTS_WIN:/src/ports" -w "$HERE" \
  "$BUILDER" \
  bash -c "gcc -O2 -Wall $HERE/watch/wetrix-watch.c \$(pkg-config --cflags --libs sdl2) -o $HERE/build/wetrix-watch \
    && sha256sum $HERE/build/wetrix-watch"
