# Wetrix: Linux aarch64 port

The PC port (`../pc`) built for Linux on 64-bit ARM, with f3d, Wetter's soft
and hard. Generic: nothing here knows a particular device; device
packages (like `../r36s`) take the binary from here.

## Requirements

- Docker with arm64 emulation. The builder image (Ubuntu 24.04, gcc 13, SDL2) is
  built on first use from `Dockerfile`.
- The recompilation and Wetter, found or cloned by `../tools/fetch_deps.py` (see
  `../pc/README.md`).

## Build

```
./build.sh          # incremental; ./build.sh clean to wipe
```

`build.sh` first finds (or clones) the dependencies, then runs the recompilation's
`tools/regen.py` on the host
(the recompiler tools are host binaries; `WETRIX_NO_REGEN=1` skips it), so a
change anywhere from the disassembly down reaches the binary. Then it mounts
the folder holding `wetrix-n64-ports/` into `wetrix-arm64:noble` under `docker
--platform linux/arm64` and builds `../pc` into `build/`. Output: `build/wetrix.aarch64`. The
recompiled RSP vector code maps to NEON through the runtime's `sse2neon.h`; SDL2
comes from pkg-config.
