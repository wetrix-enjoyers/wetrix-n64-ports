// Which renderer draws the game, and the windows they draw into.
//
//   WETRIX_RENDERER=hard     Wetter's GPU renderer (OpenGL ES 3.0 / GL 3.3); the default
//   WETRIX_RENDERER=fast3d   the f3d renderer only (OpenGL ES 3.0 / GL 3.3)
//   WETRIX_RENDERER=soft     Wetter's software renderer (CPU rasterizer,
//                            presented with SDL_Renderer)
//   WETRIX_RENDERER=softab   soft in the main window + f3d in a second (A/B)
//   WETRIX_RENDERER=hardab   soft in the main window + hard in a second (A/B)
//
// `--renderer <name>` on the command line does the same and wins over the
// environment. WETRIX_GL=core skips the GLES attempt and asks for desktop GL
// 3.3 core directly; the default is GLES 3.0 first, because that is what the
// R36S has.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "ultramodern/renderer_context.hpp"

struct SDL_Window;

namespace wetrix::render {

enum class Mode { Fast3D, Soft, SoftAB, Hard, HardAB };

void parse_args(int argc, char** argv);
Mode mode();
const char* mode_name(Mode m);

// True when the main window must be created as an OpenGL window.
bool main_window_is_gl();
// "Wetrix - <renderer>" for the main window.
const char* main_window_title();
// Window rectangle that fits the desktop's usable area (after DPI scaling):
// one 4:3 window centred, or in the A/B modes two side by side. `slot` 0 = main,
// 1 = the f3d window. WETRIX_WINDOW=WxH overrides the size.
void window_rect(int slot, int& x, int& y, int& w, int& h);
// Creates the second window of the A/B modes (main thread).
SDL_Window* create_f3d_window();
void set_main_window(SDL_Window* w);

// The factory handed to ultramodern (runs on the gfx thread).
std::unique_ptr<ultramodern::renderer::RendererContext>
create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

// hard's shown picture, RGBA8, at its render resolution. The gfx thread only.
bool hard_capture(std::vector<uint8_t>& rgba, int& w, int& h);

} // namespace wetrix::render
