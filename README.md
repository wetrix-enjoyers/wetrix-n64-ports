# Wetrix ports

One folder per target:

| Port | What it builds | From |
|---|---|---|
| `pc/` | `wetrix.exe` (Windows) and a native Linux build: the app with f3d and Wetter's soft and hard | `../wetrix-n64-recompilation` and `../wetter` |
| `aarch64/` | `wetrix.aarch64`: `pc` built for Linux on 64-bit ARM, device-agnostic | `pc/` |
| `r36s/` | the PortMaster package for the R36S: launcher, watcher, packaging and install | `aarch64/` |

The game's code lives outside this folder: `wetrix-n64-disassembly` (the ROM split
into assembly, and the ELF the recompiler reads) and `wetrix-n64-recompilation` (the game recompiled from the ROM, as a
library). The renderers soft and hard live in `wetter`.

## Building

Each port's `build.sh` finds the repositories it builds against (the recompilation
and Wetter) as sibling folders, or in `deps/`, or clones them from
`https://github.com/wetrix-enjoyers/` into `deps/` (`tools/fetch_deps.py`);
`WETRIX_GIT_BASE` points the clones somewhere else. The recompilation does the same
for the disassembly and fetches the N64ModernRuntime. You supply your own ROM:
`WETRIX_ROM=<path>`, or a ROM found near the recompilation (`README.md` there).

## Licence

GPL v3 (`LICENSE`, Copyright (c) 2026 Wetrix Enjoyers), because the ports link the GPL v3
N64ModernRuntime through the recompilation; parts keep their own licences, see
`NOTICE.md`.
