// The port's debug console: a line-based text protocol on 127.0.0.1.
//
// Enabled unless WETRIX_DEBUG_PORT=0; the port defaults to 7464. Drive it with
// tools/wdbg.py ("python tools/wdbg.py stats", or no argument for a prompt).
// "help" lists every command. The point of it is to inspect the game while it
// runs, or while it is frozen: pause/step at display-list granularity, grab
// screenshots from the f3d renderer, disassemble the last display list, bisect
// draws, dump TMEM / RDRAM / game threads / recompiled-call history, capture a
// frame for offline replay, and inject controller input.
//
// Commands that touch GL (screenshots, redraw, target grabs) are executed on the
// gfx thread; everything else runs on the server thread.
#pragma once

#include <cstdint>
#include <functional>

#include "ultramodern/ultra64.h"

namespace wetrix::debug {

void start(uint8_t* rdram);

// Gfx thread, at the top of every M_GFXTASK. Records the task and blocks here
// while the game is paused (servicing gfx commands and repainting meanwhile).
void on_task(const OSTask* task);
// Gfx thread, from update_screen: services queued gfx commands.
void poll_gfx();

// Repaints the f3d window without new game input (used while paused). Set by
// the f3d context; absent when f3d is not running.
void set_repaint(std::function<void()> fn);
void set_f3d_active(bool active);

bool paused();

// The F10 key (src/main.cpp): freeze the game and write everything worth looking
// at to dumps/<time>/ next to the exe -- RDRAM, the renderer's picture, the
// recompiled-call history, the threads, the display list when f3d is active. Press
// it again to resume. Runs on its own thread; the game stays frozen afterwards.
void toggle_freeze_dump();

// Controller input injected over the console, merged in src/main.cpp.
uint16_t injected_buttons(int pad);
bool injected_stick(int pad, float* x, float* y);

} // namespace wetrix::debug
