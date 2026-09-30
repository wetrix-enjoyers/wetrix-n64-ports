# Notices

This repository is licensed under the **GNU General Public License v3** (see
`LICENSE`). Copyright (c) 2026 Wetrix Enjoyers. Executables and packages built from it
(`wetrix.exe`, `wetrix.aarch64`, the R36S zip) are distributed under the same
licence and must come with the source: this repository, `wetrix-n64-recompilation`
and `wetter`.

## Why GPL v3

The ports link `wetrix_game` from `wetrix-n64-recompilation`, which links the
N64ModernRuntime (`ultramodern` and `librecomp`), licensed under the GPL v3. The
combined work is therefore GPL v3. The R36S package carries the GPL text and this
notice.

## Source

The complete source for the programs built from this repository:

- Ports (this repository): <https://github.com/wetrix-enjoyers/wetrix-n64-ports>
- Recompilation: <https://github.com/wetrix-enjoyers/wetrix-n64-recompilation>
- Wetter (the soft and hard renderers): <https://github.com/wetrix-enjoyers/wetter>
- Disassembly (the ELF the recompiler reads): <https://github.com/wetrix-enjoyers/wetrix-n64-disassembly>

## Parts under other licences (kept as they are)

- `pc/fast3d/` (f3d): Fast3D-derived, MIT, Copyright (c) 2020 Emill, MaikelChan.
  The licence is `pc/fast3d/LICENSE-fast3d.txt`. `pc/fast3d/glad/khrplatform.h`
  carries Khronos's licence in the file.
- `pc/thirdparty/stb/stb_image_write.h`: public domain / MIT; the terms are at the
  end of the file.
- **Wetter** (`../wetter`): MIT, its own repository and licence. It does not depend
  on the runtime.
- **SDL2**: zlib licence. Linked dynamically; Windows packages also carry
  `SDL2.dll`.
- Windows packages also carry MinGW's runtime DLLs (`libstdc++`, `libgcc`,
  `libwinpthread`), each under its own licence (the GCC ones under the GPL with the
  GCC Runtime Library Exception).

## The game

Nothing here contains game data, and no ROM is ever included. Wetrix is the work of
its authors and publishers, not of this project.

This is not legal advice.
