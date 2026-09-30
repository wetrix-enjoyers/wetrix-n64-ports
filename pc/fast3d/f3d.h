// The Fast3D-lineage renderer for the Wetrix recomp.
//
// It interprets F3DEX.NoN display lists straight out of the runtime's RDRAM and
// draws them with OpenGL (desktop 3.3 core or OpenGL ES 3.0 -- the R36S target).
// Structure and the colour-combiner shader generator come from Fast3D (MIT, see
// LICENSE-fast3d.txt, via the Perfect Dark port's fork); the display-list walker,
// memory access, TMEM emulation and framebuffer handling are written for this
// port, because the recomp keeps game memory in librecomp's word-swapped layout
// and addresses everything with N64 physical addresses rather than host pointers.
//
// Every entry point must be called on the thread that owns the current GL
// context (the runtime's gfx thread).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace f3d {

struct Options {
    // Internal resolution multiplier over the N64 framebuffer size.
    int scale = 3;
    // Wetrix's reference setup forces G_BRANCH_Z to always branch for Wetrix
    // (it picks LODs through it). Same default here so A/B compares like with like.
    bool force_branch_z = true;
    // Bilinear filtering where the RDP asks for it; off = always point sample.
    bool bilinear = true;
    // Display aspect ratio. Wider than 4:3 widens the render targets and the 3D
    // view around the centre; 2D stays in the centred 4:3 area.
    float aspect = 4.0f / 3.0f;
    // Debug switches (console "set").
    bool no_cull = false;        // ignore G_CULL_*
    bool no_clip_reject = false; // draw triangles that are wholly off-screen
    bool no_culldl = false;      // never take G_CULLDL
};

struct Stats {
    uint32_t frame = 0;          // display lists run since init
    uint32_t commands = 0;
    uint32_t dl_calls = 0;
    uint32_t vertices = 0;
    uint32_t triangles = 0;      // submitted
    uint32_t triangles_culled = 0;
    uint32_t rects = 0;
    uint32_t fills = 0;
    uint32_t draw_events = 0;    // tri batches + rects; the unit draw_limit counts
    uint32_t gl_draws = 0;
    uint32_t tex_decodes = 0;
    uint32_t tex_cache_hits = 0;
    uint32_t target_switches = 0;
    uint32_t unknown_commands = 0;
    uint32_t cpu_fallback_presents = 0;
    double run_ms = 0;
};

// One executed command, kept for the whole last display list so it can be
// dumped after the fact (debug server "dl" command).
struct CmdRecord {
    uint32_t addr;
    uint32_t w0, w1;
    uint16_t depth;
    int32_t draw_index;   // draw event this command produced, or -1
};

struct TargetInfo {
    uint32_t addr;
    uint32_t width, height;   // native
    uint8_t siz;
    uint32_t last_frame;
};

bool init(uint8_t* rdram, uint32_t rdram_size, const Options& options, bool gles);
void shutdown();

// Points the renderer at a different copy of RDRAM (debug replays of a
// snapshot); returns the previous pointer.
uint8_t* set_memory(uint8_t* rdram);

// Runs one M_GFXTASK display list (physical address).
void run_dl(uint32_t dl_phys, uint32_t ucode_phys, uint32_t ucode_data_phys);

// Draws the framebuffer the VI is scanning out into the default framebuffer.
void present(uint32_t vi_origin, uint32_t vi_width, uint32_t vi_status,
             uint32_t vi_x_scale, uint32_t vi_y_scale, int window_w, int window_h);

// Reads back the image that the last present() showed, at internal resolution,
// top row first, RGBA8.
bool grab_presented(std::vector<uint8_t>& rgba, int& w, int& h);

// Reads back any render target by its RDRAM address.
bool grab_target(uint32_t addr, std::vector<uint8_t>& rgba, int& w, int& h);
// One pixel (native coordinates, y down) of the target the VI last showed.
bool presented_pixel(int x, int y, uint8_t out[4]);
uint32_t presented_target();
// Colour image the last display list finished on.
uint32_t last_cimg();

std::vector<TargetInfo> targets();

// Debug controls.
void set_trace_file(FILE* f);              // disassemble every command of the next DL
void set_draw_limit(int32_t n);            // draw only the first n draw events (-1: all)
void set_highlight(int32_t draw_index);    // tint one draw event magenta (-1: none)
void set_wireframe(bool on);               // GL desktop only; ignored on ES
// Dump everything about one draw event of the next display list into `dir`:
// state.txt (combiner, other modes, tiles, vertices, shader source) and the
// decoded texture(s) as PNG. -1 disables.
void set_probe(int32_t draw_index, const std::string& dir);
const Stats& last_stats();
const std::vector<CmdRecord>& last_commands();
std::string disasm(uint32_t w0, uint32_t w1);
void dump_tmem(FILE* f);
std::string describe_state();

Options& options();

} // namespace f3d
