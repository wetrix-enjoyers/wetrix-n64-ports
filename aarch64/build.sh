#!/bin/bash
# build.sh - the PC port (../pc) built for Linux aarch64: f3d, soft and hard,
# inside an arm64 container (qemu-emulated on an x86 PC). Generic
# aarch64 Linux; nothing device-specific (see ../r36s for the handheld).
#
#   ./build.sh          incremental build
#   ./build.sh clean    remove build/
#   BUILDER=<image> ./build.sh   use another arm64 builder image
#
# The builder image (Dockerfile here) is built on first use.
#
# Output: build/wetrix.aarch64
#
# This repository, the recompilation and Wetter (found, or cloned into ../deps, by
# ../tools/fetch_deps.py) are mounted into the container as /src/ports, /src/recomp
# and /src/wetter.
# regen.py runs first, so a change anywhere from the disassembly down is built.
set -e
cd "$(dirname "$0")"

PORTS_WIN="$(cd .. && (pwd -W 2>/dev/null || pwd))"
eval "$(python ../tools/fetch_deps.py --shell)"   # WETRIX_RECOMP_DIR, WETTER_DIR
BUILDER="${BUILDER:-wetrix-arm64:noble}"
HERE=/src/ports/aarch64

if [ "$1" = "clean" ]; then
  rm -rf build
  echo "cleaned build/"
  exit 0
fi
mkdir -p build

docker image inspect "$BUILDER" >/dev/null 2>&1 || {
  [ "$BUILDER" = "wetrix-arm64:noble" ] || { echo "no image $BUILDER" >&2; exit 1; }
  docker build --platform linux/arm64 -t "$BUILDER" -f Dockerfile .
}

# The generated sources are made on the host (the recompiler tools are host
# binaries); this brings the whole chain up to date first. WETRIX_NO_REGEN=1 skips it.
[ -n "$WETRIX_NO_REGEN" ] || python "$WETRIX_RECOMP_DIR/tools/regen.py"

MSYS_NO_PATHCONV=1 docker run --rm --platform linux/arm64 \
  -v "$PORTS_WIN:/src/ports" -v "$WETRIX_RECOMP_DIR:/src/recomp" -v "$WETTER_DIR:/src/wetter" -w "$HERE" \
  "$BUILDER" \
  bash -c "cmake -S /src/ports/pc -B $HERE/build -DCMAKE_BUILD_TYPE=Release -DWETRIX_RECOMP_DIR=/src/recomp -DWETTER_DIR=/src/wetter \
    && cmake --build $HERE/build -j\$(nproc) \
    && readelf -h $HERE/build/wetrix.aarch64 | grep -E 'Class|Machine' \
    && sha256sum $HERE/build/wetrix.aarch64"
