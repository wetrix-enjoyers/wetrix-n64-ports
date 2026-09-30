# Wetrix: PC port

The app around the recompiled game: window, input, audio, runtime callbacks,
renderer selection, debug console and diagnostics. It builds `wetrix.exe` on
Windows and a native Linux build; `../aarch64` builds it for ARM Linux, and
`../r36s` packages that for the R36S.

It builds against two other repositories, found by `../tools/fetch_deps.py`:

- `wetrix-n64-recompilation`, for the `wetrix_game` library (`WETRIX_RECOMP_DIR`);
- `wetter`, the soft and hard renderers (`WETTER_DIR`).

Each is used from the folder its environment variable names, or from a sibling
folder next to this repository, or else it is cloned from GitHub into `../deps/`.

## Layout

| Path | Contents |
|---|---|
| `src/` | entry point, window, input, audio, renderer selection and adapters, debug console, sampler, diagnostics |
| `include/` | `render_select.h`, `debug_server.h` |
| `fast3d/` | **f3d**, the OpenGL ES 3.0 renderer (Fast3D-derived, MIT, see `LICENSE-fast3d.txt`) |
| `thirdparty/stb/` | `stb_image_write.h` (screenshots, probe dumps) |
| `tools/` | debug client, navigation script, backtrace resolver |
| `build/` | builds (not committed) |

## Renderers

The runtime ships no renderer; the port brings three, chosen at startup.

| Renderer | What |
|---|---|
| **f3d** | Fast3D-derived, OpenGL ES 3.0 (or desktop GL 3.3 core) |
| **soft** | Wetter's software RDP, on several threads |
| **hard** | Wetter's GPU RDP, OpenGL ES 3.0 / GL 3.3 core |

`src/render_select.cpp` owns the choice. It wraps the live backends in one
`ultramodern::renderer::RendererContext`, so every display list reaches every
active renderer before the runtime signals DP completion. It also paces the game
(below). Modes:

- `fast3d` (default), `soft`, `hard`: one renderer in the main window.
- `softab`: soft in the main window, f3d in a second.
- `hardab`: soft in the main window, hard in a second.

Windows are fitted to the desktop's usable area (DPI aware): one 4:3 window, or
two side by side.

### Pacing

Wetrix steps its simulation once per frame, so its speed is its frame rate; the
console's RDP held it to about 3 VIs per frame. Each graphics task starts on the
runtime's VI grid, `WETRIX_FRAME_VIS` ticks after the last (a late one starts at
once, with no catch-up). A frame is presented only when the VI origin moves or a
task has drawn, not again on the VIs in between.

### f3d

An F3DEX.NoN 1.21 interpreter (the ROM's graphics microcode):

- reads display lists, vertices, matrices, lights and textures straight from the
  runtime's RDRAM (word-swapped layout, N64 physical addresses, RSP segments);
- emulates the RDP's 4KB TMEM (LOADBLOCK / LOADTILE / LOADTLUT) and decodes
  textures from it, cached by content hash;
- keeps one render target per colour-image address; the VI origin picks what is
  shown; frames the RDP never drew are decoded from RDRAM;
- handles NoN (no near clipping) by rewriting clip-space z, because GLES 3.0 has
  no depth clamp;
- generates colour-combiner shaders the Fast3D way;
- widescreen with `WETRIX_F3D_ASPECT`.

Files: `f3d_core.cpp` (interpreter), `f3d_gl.cpp` (GL backend + shader
generator), `f3d_disasm.cpp` (GBI disassembler), `f3d_gbi.h` (F3DEX constants),
`gfx_cc.*` and `glad/` (from Fast3D, unchanged).

## Build

### Dependencies

Windows: `build.sh` installs what it needs into MSYS2 (`gcc`, `cmake`, `ninja`,
`pkgconf`, `SDL2`, all `mingw-w64-ucrt-x86_64-*`) with `pacman --needed`; nothing
to do by hand. Linux: `libsdl2-dev`, `pkg-config`, cmake and a C++20 compiler
(`../aarch64/Dockerfile` is the ARM version of that list).

### Windows

MSYS2 UCRT64 from Git Bash:

```
./build.sh
```

It fetches the dependencies, then runs the recompilation's `tools/regen.py` (the
whole chain from the disassembly down, each step only if its inputs changed;
`WETRIX_NO_REGEN=1` skips it), then configures `build/win` (Ninja, Release,
`WETTER_DEBUGINFO=ON`) and builds it. By hand, the same without the
chain:

```
cmake -S . -B build/win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/win --parallel
```

If a generated source is stale, the configure stops and says to run `regen.py`.

Output: `build/win/wetrix.exe` plus SDL2, the MinGW runtime and `wetrix.z64`. The
folder is self-contained. `-DWETTER_DEBUGINFO=ON` gives Wetter line tables for
`bench-soft ... prof`.

Close a running `wetrix.exe` before rebuilding; the linker can't replace it.

## Run

```
wetrix.exe [--renderer fast3d|soft|softab|hard|hardab] [--frame-vis <n>]
```

Double-clicking works: the working directory and librecomp's config path are
pinned to the exe's folder. When the process owns its console, diagnostics go to
`wetrix.log` and the console is detached.

### Controls

| N64 | Keyboard | Gamepad |
|---|---|---|
| Stick | arrows / WASD | left stick |
| A / B | X / C | A / B |
| Z | Z / Left Shift | Back |
| Start | Enter | Start |
| L / R | Q / E | LB / RB |
| C-buttons | I J K L | right stick, X = C-left, Y = C-up |
| D-pad | | d-pad |

### Environment

| Variable | Effect | Default |
|---|---|---|
| `WETRIX_RENDERER` | `fast3d`, `soft`, `softab`, `hard`, `hardab` (`--renderer` overrides) | `fast3d` |
| `WETRIX_FRAME_VIS` | VI ticks per game frame (see Pacing). `--frame-vis <n>` overrides; `0` = unpaced | 3 |
| `WETRIX_DETERMINISTIC` | if set (to anything), the R4300 COUNT register `osGetCount` reads is frozen at 0, so the game's time-based random seed repeats every run; for A/B captures that must match. Unset, it is a real 46.875 MHz counter | unset |
| `WETRIX_PRESENT_EVERY_VI` | `1` presents on every VI, not only when the picture changed | off |
| `WETRIX_PACE_LOG` | `1` logs every task (arrive, start, end) and present (time, cost, origin) | off |
| `WETRIX_GL` | `core` = desktop GL 3.3 core instead of trying GLES 3.0 first | GLES first |
| `WETRIX_F3D_SCALE` | f3d internal resolution multiplier | 3 |
| `WETRIX_F3D_FILTER` | `point` disables bilinear | bilinear |
| `WETRIX_F3D_ASPECT` | display aspect, `16:9` or a number; wider than 4:3 widens the 3D view, 2D stays centred | `4:3` |
| `WETRIX_F3D_BRANCH_Z` | `real` evaluates G_BRANCH_Z; otherwise it always branches | always |
| `WETRIX_HARD_SCALE` | hard: internal resolution multiplier (integer) | 3 |
| `WETRIX_HARD_ASPECT` | hard: display aspect, as `WETRIX_F3D_ASPECT` | `4:3` |
| `WETRIX_HARD_COPYBACK` | hard: `task` writes every drawn image back to RDRAM after each task (texture reads of drawn images are always synced) | off |
| `WETRIX_BRANCH_Z` | soft and hard: `real` runs G_BRANCH_Z's depth test instead of always branching | always |
| `WETRIX_WINDOW` | `WxH` forces the window size | fitted to desktop |
| `WETRIX_FULLSCREEN` | `1` = fullscreen desktop | off |
| `WETRIX_DEBUG_PORT` | debug console TCP port, `0` = off | 7464 |
| `WETRIX_SNAPSHOT` | `0` disables per-task RDRAM snapshots used by console replays | on |
| `WETRIX_STALL_PROBE` | `1` arms the hang detector (thread states, host IPs, call rings after 10s without a display list) | off |
| `WETRIX_AUTOPRESS` | `1` presses Start at 2s and A every 10s | off |
| `WETRIX_AUDIO_LOG` | `1` logs what the audio device is fed | off |

Wetter's own variables (`WETTER_RENDER_TIME`, `WETTER_SOFT_THREADS`, …) are in its
README.

## Debug console

A line protocol on `127.0.0.1:<WETRIX_DEBUG_PORT>`. Client:

```
python tools/wdbg.py <command> [<command> ...]     # or no arguments for a prompt

**F10** in the game window freezes the game and writes `dumps/<time>/` beside the exe:
RDRAM (`rdram.bin`), the renderer's picture, `calls.txt`, `threads.txt`, `status.txt`,
and the display list when f3d is active. F10 again resumes.
```

File arguments are paths as the game process sees them (on Windows use `C:/...`,
not `/c/...`). `help` lists everything.

| Command | Effect |
|---|---|
| `status` | renderer, pause state, task count |
| `pause`, `resume`, `step [n]` | freeze / continue at display-list granularity; windows keep repainting while paused |
| `stats` | f3d state of the last display list: counts, targets, tiles, other modes |
| `shot <png>`, `shot-soft <png>`, `shot-hard <png>` | a renderer's presented image |
| `ab <prefix>` | `softab` or `hardab`: the two images, a diff and error metrics. Pause first so both show the same frame. |
| `bench-soft [n] [file] [prof\|profw]` | time soft on a saved frame; `prof` samples the gfx thread, `profw` a worker (see Wetter's `tools/soft_prof.py`) |
| `targets`, `target <addr> <png>` | f3d render targets |
| `dl <file>` | last display list: address, words, nesting, draw number, disassembly |
| `trace <file>` | full disassembly of the next (or, paused, the shown) display list |
| `redraw` | re-run the shown frame from its RDRAM snapshot |
| `limit <n>`, `highlight <n>` | draw only the first n draw events / paint draw n magenta (`-1` off) |
| `probe <n> <dir>` | draw event n: combiner, other modes, colours, viewport, vertices, tiles, shader source, decoded textures |
| `pixhist <x> <y>` | every draw event that changed a pixel of the shown frame (paused) |
| `set <switch> 0\|1` | `nocull`, `noclip`, `noculldl`, `branchz`, `bilinear` |
| `wire on\|off` | wireframe (desktop GL only) |
| `tmem <file>`, `rdram <file>`, `read <addr> [len]` | memory dumps (`read` prints N64 byte order) |
| `capture <file>` | last task + VI registers + RDRAM |
| `threads`, `calls [n]` | game threads; recent recompiled calls per thread (trace rings) |
| `press <btns> [ms]`, `hold <btns>`, `release`, `stick <x> <y> [ms]` | input injection; buttons `a b z start up down left right l r cu cd cl cr`, joined with `+` |
| `quit` | exit |

Redraw-based commands (`redraw`, `trace`, `probe`, `pixhist`) replay against a
snapshot of RDRAM taken when the shown frame's task started, because the game
keeps writing memory while paused.

## Diagnostics

- **Crash handler** (Windows): prints a dbghelp backtrace. Resolve it with
  `python tools/resolve_backtrace.py <exe> < log` (needs MSYS2's `nm` on PATH).
- **Stall probe** (`WETRIX_STALL_PROBE=1`, Windows): after 10s without a display
  list, dumps game threads and queues, host thread IPs, and the call rings.
- **Trace rings**: with the recompilation's `trace_mode = true`, each thread
  records recompiled calls (repeats collapsed). Read them with the stall probe or
  `calls`.

## Known gaps

- f3d never writes rendered frames back to RDRAM, so CPU reads of the framebuffer
  would see stale data (hard can: `WETRIX_HARD_COPYBACK`).
- The splash and menu show one bubble where the original shows several. The game
  draws every bubble at the same position, so it isn't a renderer issue.
