# Wetrix: R36S port

The PC port (`../pc`) built for the R36S (ArkOS, Linux aarch64, Mali-G31 GLES
3.2), packaged for PortMaster. This folder holds only the device layer: the
launcher, the Select+Start exit watcher, the build, packaging and install
scripts. The game and its renderers come from `../pc`, the recompilation and
Wetter.

## Layout

| Path | Contents |
|---|---|
| `launcher/` | `Wetrix.sh` (PortMaster launcher), `port.json`, `gameinfo.xml` |
| `watch/wetrix-watch.c` | Select+Start exit watcher |
| `build.sh` | runs `../aarch64/build.sh`, then builds the watcher in the same container |
| `package.py` | the PortMaster zip |
| `build/`, `dist/` | build outputs and the zip (not committed) |

## Requirements

- Docker with arm64 emulation (the builder image is built on first use, see
  `../aarch64/Dockerfile`).
- The recompilation and Wetter, found or cloned by `../tools/fetch_deps.py` (see
  `../pc/README.md`).

## Build and package

```
./build.sh                      # incremental; ./build.sh clean to wipe
python package.py               # -> dist/wetrix.zip
```

The game itself is the generic Linux aarch64 build from `../aarch64` (output
`../aarch64/build/wetrix.aarch64`); this folder only adds the watcher
(`build/wetrix-watch`) and everything else device-specific. If Docker Desktop
answers with exit 125 or an API 500, restart it.

`package.py` checks both binaries are AArch64 ELF, refuses ROM files, and writes:

```
Wetrix.sh                 launcher
wetrix/
  wetrix.aarch64
  wetrix-watch
  data/                   put the ROM here (or in wetrix/)
  LICENSE-fast3d.txt  LICENSE-wetter.txt  port.json  gameinfo.xml  build-info.txt
```

## Install

Unzip `dist/wetrix.zip` into the device's ports folder (`/roms/ports`, or
`/roms2/ports` on a two-card R36S) so that `Wetrix.sh` and `wetrix/` sit there, make
`Wetrix.sh`, `wetrix/wetrix.aarch64` and `wetrix/wetrix-watch` executable, and put
your ROM in `wetrix/` (or `wetrix/data/`). `wetrix/renderer.txt`, containing
`fast3d`, `soft` or `hard`, picks the renderer.

## On the device

The launcher resolves its folder relative to itself, sets the device
environment, runs the game and the watcher, and logs to `wetrix/log.txt`.
Select+Start quits (the watcher kills the game).

- **Renderer:** `wetrix/renderer.txt` holds `fast3d`, `soft` or `hard`; without
  it, fast3d. hard is the one to use: accurate, at the game's paced speed. f3d
  shows triangle glitches here, and soft runs slowly.
- **Environment the launcher sets:**
  - `SDL_VIDEO_DRIVER=kmsdrm`, and `SDL_GAMECONTROLLERCONFIG` from PortMaster's
    `control.txt`;
  - `WETRIX_FULLSCREEN=1`, `WETRIX_F3D_SCALE=2` and `WETRIX_HARD_SCALE=2` (the
    640x480 panel);
  - the debug console off (`WETRIX_DEBUG_PORT=0`) and RDRAM snapshots off;
  - `WETTER_RENDER_TIME=1` and `WETRIX_PACE_LOG=1`, so `log.txt` carries per-list
    render times and task/present timing.
