// f3d core: the F3DEX.NoN display-list interpreter.
//
// Lineage: Fast3D's gfx_pc.cpp (Emill / MaikelChan, MIT; via the Perfect Dark
// port's fork, see LICENSE-fast3d.txt). The vertex pipeline, colour-combiner
// keying and triangle packing follow it closely. What is different, and why:
//
//  * Memory. Fast3D dereferences host pointers. Here every address is an N64
//    physical address into librecomp's RDRAM, which stores each 32-bit word in
//    host order (byte A lives at rdram[A ^ 3], halfword A at rdram[A ^ 2]). All
//    reads go through rd8/rd16/rd32 below.
//  * Textures. Fast3D remembers "the texture is at this pointer" per TMEM slot
//    and decodes straight from RAM. That breaks for anything that relies on how
//    the RDP actually lays texels out (odd-line word swaps, TLUTs sharing TMEM,
//    partial loads). This core emulates TMEM: LOADBLOCK / LOADTILE / LOADTLUT
//    write the 4KB the RDP would hold, and textures are decoded from TMEM through
//    the tile descriptor, then cached by a hash of the bytes they came from.
//  * Framebuffers. Every colour image address gets its own FBO; the VI origin
//    picks which one is shown. Frames the RDP never drew (CPU-drawn) fall back
//    to decoding the framebuffer out of RDRAM.
//  * The GBI is F3DEX (GBI1), not PD's Rare variant.

#include "f3d.h"
#include "f3d_gbi.h"
#include "f3d_gl.h"
#include "gfx_cc.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include "stb_image_write.h"   // implementation lives in src/debug_server.cpp

using namespace f3d::gbi;

namespace f3d {

namespace {

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
uint8_t* rdram = nullptr;
uint32_t rdram_mask = 0x7FFFFF;

inline uint32_t rd32(uint32_t pa) { return *reinterpret_cast<const uint32_t*>(rdram + ((pa & rdram_mask) & ~3u)); }
inline uint16_t rd16(uint32_t pa) { return *reinterpret_cast<const uint16_t*>(rdram + (((pa & rdram_mask) ^ 2) & ~1u)); }
inline uint8_t rd8(uint32_t pa) { return rdram[(pa & rdram_mask) ^ 3]; }
inline void wr16(uint32_t pa, uint16_t v) { *reinterpret_cast<uint16_t*>(rdram + (((pa & rdram_mask) ^ 2) & ~1u)) = v; }

#define SCALE_5_8(v) (((v) * 0xFF) / 0x1F)
#define SCALE_4_8(v) ((v) * 0x11)
#define SCALE_3_8(v) (((v) * 0x24) + ((v) >> 1))

inline uint32_t bits(uint32_t w, int pos, int width) { return (w >> pos) & ((1u << width) - 1); }

Options opts;
bool use_gles = false;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct RGBA {
    uint8_t r, g, b, a;
};

constexpr int MAX_VERTICES = 64;   // F3DEX.NoN holds 32; slack for rects

struct LoadedVertex {
    float x, y, z, w;
    float u, v;
    RGBA color;
    uint8_t fog;
    uint8_t clip_rej;
};

struct Light {
    uint8_t col[3];
    uint8_t colc[3];
    int8_t dir[3];
};

struct RSP {
    float modelview[18][4][4];
    int modelview_size;
    float MP[4][4];
    float P[4][4];
    Light lights[8];
    float light_coeffs[8][3];
    Light lookat[2];
    float lookat_coeffs[2][3];
    bool lookat_enabled;
    int num_lights;   // directional + ambient
    bool lights_changed;
    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;
    uint16_t tex_scale_s, tex_scale_t;
    bool tex_on;
    LoadedVertex verts[MAX_VERTICES + 4];
    uint32_t segments[16];
    uint32_t rdphalf1;
    struct {
        int16_t vscale[4], vtrans[4];
    } vp;
} rsp;

struct Tile {
    uint8_t fmt, siz;
    uint16_t line;   // in 64-bit words
    uint16_t tmem;   // in 64-bit words
    uint8_t palette;
    uint8_t cmt, maskt, shiftt, cms, masks, shifts;
    uint16_t uls, ult, lrs, lrt;   // 10.2
};

struct Viewport {
    float x, y, w, h;   // native pixels, y down
};

struct RDP {
    uint8_t tmem[4096];
    Tile tiles[8];
    struct {
        uint32_t addr;
        uint8_t fmt, siz;
        uint32_t width;
    } timg;
    uint32_t cimg_addr, cimg_width;
    uint8_t cimg_fmt, cimg_siz;
    uint32_t zimg_addr;
    uint32_t other_mode_l, other_mode_h;
    uint64_t combine_mode;
    RGBA env_color, prim_color, fog_color, blend_color;
    uint32_t fill_color;
    uint8_t prim_lod_frac, prim_min_lod;
    uint16_t prim_depth;
    uint8_t first_tile;
    Viewport viewport;
    struct {
        float ulx, uly, lrx, lry;
    } scissor;
    bool state_dirty;
} rdp;

// ---------------------------------------------------------------------------
// Render targets
// ---------------------------------------------------------------------------
struct Target {
    uint32_t addr;
    uint32_t width, height;
    // Widescreen: the target is ext_width wide, the game's width centred in it
    // with pad on either side (native pixels).
    uint32_t ext_width;
    float pad;
    uint8_t siz;
    gl::Fbo fbo;
    uint32_t depth_rb;
    uint32_t last_frame;
};

std::map<uint32_t, Target> targets_by_addr;
std::map<std::pair<int, int>, uint32_t> depth_by_size;
Target* cur_target = nullptr;
bool depth_clear_pending = false;
uint32_t presented_addr = 0;
gl::Fbo cpu_fb;   // CPU-framebuffer fallback image
bool presented_cpu = false;
int window_w = 0, window_h = 0;

// ---------------------------------------------------------------------------
// Rendering state (what GL currently has)
// ---------------------------------------------------------------------------
struct ColorCombiner {
    uint64_t shader_id0;
    uint32_t shader_id1;
    bool used_textures[2];
    gl::Program* prg[16];
    uint8_t shader_input_mapping[2][7];
};

std::map<ColorCombinerKey, ColorCombiner> combiner_pool;

struct TexEntry {
    uint32_t gl;
    uint32_t w, h;
    uint32_t last_used;
    bool linear;
    uint32_t cms, cmt;
};
std::unordered_map<uint64_t, TexEntry> tex_cache;
std::vector<uint8_t> tex_buf;

struct {
    gl::Program* program = nullptr;
    int depth_key = -1;
    int blend_mode = -1;
    int vp[4] = { -1, -1, -1, -1 };
    int sc[4] = { -1, -1, -1, -1 };
    uint32_t tex[2] = { 0, 0 };
    bool highlight = false;
} gls;

constexpr size_t MAX_BUFFERED = 256;
float vbo_buf[MAX_BUFFERED * 3 * 40];
size_t vbo_len = 0;
size_t vbo_tris = 0;

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------
Stats stats;
Stats last;
std::vector<CmdRecord> cmd_log;
std::vector<CmdRecord> last_cmd_log;
FILE* trace_file = nullptr;
int32_t draw_limit = -1;
int32_t highlight_index = -1;
int32_t probe_index = -1;
std::string probe_dir;
bool probe_done = false;
int32_t cur_draw_index = -1;
uint32_t frame_counter = 0;
uint32_t warned_ops[256];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
uint32_t seg_addr(uint32_t a) {
    return (rsp.segments[(a >> 24) & 0xF] + (a & 0x00FFFFFF)) & 0x03FFFFFF;
}

void flush() {
    if (vbo_tris == 0) return;
    gl::draw_triangles(vbo_buf, vbo_len, vbo_tris);
    stats.gl_draws++;
    vbo_len = 0;
    vbo_tris = 0;
}

int target_scale() { return std::max(1, opts.scale); }

// How much wider than 4:3 the picture is (1 at 4:3).
float extension() { return std::max(1.0f, opts.aspect / (4.0f / 3.0f)); }

// Spans nearly the colour image's width: games keep their scissor (and at times
// their viewport) inside the overscan margins, so a tenth either side counts.
bool spans_width(float x0, float x1, float width) {
    return x0 <= width * 0.1f && x1 >= width * 0.9f;
}

// A viewport as wide as the colour image is the 3D view widescreen widens.
bool viewport_is_full(const Viewport& vp) {
    return extension() > 1.0f && spans_width(vp.x, vp.x + vp.w, (float)rdp.cimg_width);
}

// Native framebuffer height guess for a colour image. The RDP never states it;
// the scissor the game sets for the image is the best witness, then 4:3.
uint32_t guess_height(uint32_t width) {
    uint32_t h = (uint32_t)std::lround(rdp.scissor.lry);
    if (h == 0 || h > 1024 || rdp.scissor.lrx > width + 1) h = width * 3 / 4;
    return std::max<uint32_t>(h, 1);
}

void import_rdram_into(Target& t);

Target* get_target() {
    if (rdp.cimg_addr == rdp.zimg_addr && rdp.zimg_addr != 0) return nullptr;
    auto it = targets_by_addr.find(rdp.cimg_addr);
    if (it != targets_by_addr.end() && it->second.width == rdp.cimg_width) {
        return &it->second;
    }
    if (it != targets_by_addr.end()) {
        gl::delete_fbo(it->second.fbo);
        targets_by_addr.erase(it);
    }
    Target t{};
    t.addr = rdp.cimg_addr;
    t.width = rdp.cimg_width;
    t.height = guess_height(rdp.cimg_width);
    t.siz = rdp.cimg_siz;
    t.ext_width = std::max<uint32_t>(t.width, (uint32_t)std::lround(t.width * extension()));
    t.pad = (float)(t.ext_width - t.width) / 2.0f;
    const int s = target_scale();
    auto key = std::make_pair((int)t.ext_width * s, (int)t.height * s);
    auto d = depth_by_size.find(key);
    if (d == depth_by_size.end()) {
        d = depth_by_size.emplace(key, gl::create_depth_buffer(key.first, key.second)).first;
    }
    t.depth_rb = d->second;
    t.fbo = gl::create_fbo(key.first, key.second, t.depth_rb);
    fprintf(stderr, "[f3d] new target 0x%06X %ux%u siz %u (fbo %dx%d)\n", t.addr, t.width, t.height, t.siz,
            key.first, key.second);
    auto& ref = targets_by_addr.emplace(t.addr, t).first->second;
    gl::bind_fbo(&ref.fbo);
    gl::clear(true, true, 0, 0, 0, 1);
    import_rdram_into(ref);
    return &ref;
}

void bind_current_target() {
    Target* t = get_target();
    if (t != cur_target) {
        flush();
        cur_target = t;
        stats.target_switches++;
        if (t != nullptr) gl::bind_fbo(&t->fbo);
        gls.vp[0] = gls.sc[0] = -1;   // force re-set
        gls.vp[2] = gls.sc[2] = -1;
    }
    if (t != nullptr) {
        t->last_frame = frame_counter;
        if (depth_clear_pending) {
            flush();
            gl::clear(false, true);
            gls.sc[2] = -1;
            depth_clear_pending = false;
        }
    }
}

void apply_viewport(const Viewport& vp) {
    if (cur_target == nullptr) return;
    const int s = target_scale();
    // Game x maps to x + pad in the (possibly wider) target. A full-width 3D
    // viewport widens by the extension about its centre; its vertices carry
    // x / extension (see sp_vertex), so nothing inside the old view moves.
    float vx = vp.x + cur_target->pad, vw = vp.w;
    if (viewport_is_full(vp)) {
        const float centre = vx + vw / 2.0f;
        vw *= extension();
        vx = centre - vw / 2.0f;
    }
    int x = (int)std::floor(vx * s);
    int w = (int)std::ceil((vx + vw) * s) - x;
    int y = (int)std::floor(((float)cur_target->height - (vp.y + vp.h)) * s);
    int h = (int)std::ceil(((float)cur_target->height - vp.y) * s) - y;
    if (x != gls.vp[0] || y != gls.vp[1] || w != gls.vp[2] || h != gls.vp[3]) {
        flush();
        gl::set_viewport(x, y, w, h);
        gls.vp[0] = x; gls.vp[1] = y; gls.vp[2] = w; gls.vp[3] = h;
    }
    float ulx = std::max(0.0f, rdp.scissor.ulx), uly = std::max(0.0f, rdp.scissor.uly);
    float lrx = std::min((float)cur_target->width, rdp.scissor.lrx);
    const float lry = std::min((float)cur_target->height, rdp.scissor.lry);
    if (cur_target->pad > 0.0f && spans_width(ulx, lrx, (float)cur_target->width)) {
        // A full-width scissor spans the whole target.
        ulx = 0.0f;
        lrx = (float)cur_target->ext_width;
    } else {
        ulx += cur_target->pad;
        lrx += cur_target->pad;
    }
    int sx = (int)std::floor(ulx * s);
    int sw = (int)std::ceil(lrx * s) - sx;
    int sy = (int)std::floor(((float)cur_target->height - lry) * s);
    int sh = (int)std::ceil(((float)cur_target->height - uly) * s) - sy;
    if (sx != gls.sc[0] || sy != gls.sc[1] || sw != gls.sc[2] || sh != gls.sc[3]) {
        flush();
        gl::set_scissor(sx, sy, sw, sh);
        gls.sc[0] = sx; gls.sc[1] = sy; gls.sc[2] = sw; gls.sc[3] = sh;
    }
}

// ---------------------------------------------------------------------------
// TMEM loads
// ---------------------------------------------------------------------------
inline uint32_t bytes_for_texels(uint32_t texels, uint8_t siz) { return (texels << siz) >> 1; }

void load_block(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    Tile& t = rdp.tiles[tile];
    const uint8_t siz = rdp.timg.siz;
    const uint32_t texels = lrs - uls + 1;
    const uint32_t bytes = std::max<uint32_t>(bytes_for_texels(texels, siz), 1);
    const uint32_t qwords = (bytes + 7) / 8;
    uint32_t src = rdp.timg.addr + bytes_for_texels(ult * rdp.timg.width + uls, siz);
    const uint32_t dst = t.tmem * 8u;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < qwords && i < 512; i++) {
        const bool odd = (acc >> 11) & 1;
        acc += dxt;
        uint8_t q[8];
        for (int b = 0; b < 8; b++) q[b] = rd8(src + i * 8 + b);
        if (siz == G_IM_SIZ_32b) {
            // Two texels RGBA RGBA: RG halves to the low bank, BA to the high bank.
            uint32_t bank = (dst + i * 4) ^ (odd ? 4 : 0);
            rdp.tmem[(bank + 0) & 0x7FF] = q[0];
            rdp.tmem[(bank + 1) & 0x7FF] = q[1];
            rdp.tmem[(bank + 2) & 0x7FF] = q[4];
            rdp.tmem[(bank + 3) & 0x7FF] = q[5];
            rdp.tmem[((bank + 0) & 0x7FF) | 0x800] = q[2];
            rdp.tmem[((bank + 1) & 0x7FF) | 0x800] = q[3];
            rdp.tmem[((bank + 2) & 0x7FF) | 0x800] = q[6];
            rdp.tmem[((bank + 3) & 0x7FF) | 0x800] = q[7];
        } else {
            const uint32_t base = dst + i * 8;
            for (int b = 0; b < 8; b++) rdp.tmem[(base + (b ^ (odd ? 4 : 0))) & 0xFFF] = q[b];
        }
    }
    (void)t;
}

void load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    Tile& t = rdp.tiles[tile];
    const uint8_t siz = rdp.timg.siz;
    const uint32_t x0 = uls >> 2, y0 = ult >> 2, x1 = lrs >> 2, y1 = lrt >> 2;
    const uint32_t line = t.line * 8u;
    const uint32_t dst = t.tmem * 8u;
    for (uint32_t y = 0; y + y0 <= y1 && y < 1024; y++) {
        const uint32_t row_src = rdp.timg.addr + bytes_for_texels((y0 + y) * rdp.timg.width + x0, siz);
        const uint32_t xor_ = (y & 1) ? 4 : 0;
        if (siz == G_IM_SIZ_32b) {
            for (uint32_t x = 0; x + x0 <= x1; x++) {
                uint32_t bank = (dst + y * line + x * 2) ^ xor_;
                rdp.tmem[bank & 0x7FF] = rd8(row_src + x * 4 + 0);
                rdp.tmem[(bank + 1) & 0x7FF] = rd8(row_src + x * 4 + 1);
                rdp.tmem[(bank & 0x7FF) | 0x800] = rd8(row_src + x * 4 + 2);
                rdp.tmem[((bank + 1) & 0x7FF) | 0x800] = rd8(row_src + x * 4 + 3);
            }
        } else {
            const uint32_t row_bytes = bytes_for_texels(x1 - x0 + 1, siz);
            const uint32_t row_bytes_q = (row_bytes + 7) & ~7u;
            for (uint32_t b = 0; b < row_bytes_q; b++) {
                rdp.tmem[((dst + y * line + b) ^ xor_) & 0xFFF] = rd8(row_src + b);
            }
        }
    }
    t.uls = uls; t.ult = ult; t.lrs = lrs; t.lrt = lrt;
}

void load_tlut(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    Tile& t = rdp.tiles[tile];
    const uint32_t count = (lrs >> 2) - (uls >> 2) + 1;
    const uint32_t src = rdp.timg.addr + ((ult >> 2) * rdp.timg.width + (uls >> 2)) * 2;
    const uint32_t dst = t.tmem * 8u;
    for (uint32_t i = 0; i < count && i < 256; i++) {
        const uint8_t hi = rd8(src + i * 2), lo = rd8(src + i * 2 + 1);
        for (int k = 0; k < 4; k++) {
            rdp.tmem[(dst + i * 8 + k * 2) & 0xFFF] = hi;
            rdp.tmem[(dst + i * 8 + k * 2 + 1) & 0xFFF] = lo;
        }
    }
    t.uls = uls; t.ult = ult; t.lrs = lrs; t.lrt = lrt;
}

// ---------------------------------------------------------------------------
// Texture decode from TMEM
// ---------------------------------------------------------------------------
inline uint32_t tile_width(const Tile& t) { return ((t.lrs - t.uls) >> 2) + 1; }
inline uint32_t tile_height(const Tile& t) { return ((t.lrt - t.ult) >> 2) + 1; }

void decode_dims(const Tile& t, uint32_t& w, uint32_t& h) {
    uint32_t tw = tile_width(t) & 0x3FF, th = tile_height(t) & 0x3FF;
    if (tw == 0) tw = 1;
    if (th == 0) th = 1;
    w = t.masks ? (1u << std::min<uint32_t>(t.masks, 10)) : tw;
    h = t.maskt ? (1u << std::min<uint32_t>(t.maskt, 10)) : th;
}

inline void palette_rgba(uint16_t e, bool ia, uint8_t* out) {
    if (ia) {
        out[0] = out[1] = out[2] = e >> 8;
        out[3] = e & 0xFF;
    } else {
        out[0] = SCALE_5_8(e >> 11);
        out[1] = SCALE_5_8((e >> 6) & 0x1F);
        out[2] = SCALE_5_8((e >> 1) & 0x1F);
        out[3] = (e & 1) ? 255 : 0;
    }
}

inline uint16_t tlut_entry(uint32_t idx) {
    const uint32_t a = 0x800 + (idx & 0xFF) * 8;
    return (uint16_t)((rdp.tmem[a] << 8) | rdp.tmem[a + 1]);
}

void decode_tile(const Tile& t, uint32_t w, uint32_t h, uint8_t* out) {
    const uint32_t base = t.tmem * 8u;
    const uint32_t line = t.line * 8u;
    const uint32_t tlut = rdp.other_mode_h & (3u << G_MDSFT_TEXTLUT);
    const bool use_tlut = tlut != G_TT_NONE && t.siz <= G_IM_SIZ_8b;
    const bool tlut_ia = tlut == G_TT_IA16;
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t row = base + y * line;
        const uint32_t xr = (y & 1) ? 4 : 0;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* o = out + (y * w + x) * 4;
            switch (t.siz) {
                case G_IM_SIZ_4b: {
                    const uint8_t byte = rdp.tmem[((row + (x >> 1)) ^ xr) & 0xFFF];
                    const uint8_t n = (x & 1) ? (byte & 0xF) : (byte >> 4);
                    if (use_tlut) {
                        palette_rgba(tlut_entry((t.palette << 4) | n), tlut_ia, o);
                    } else if (t.fmt == G_IM_FMT_IA) {
                        o[0] = o[1] = o[2] = SCALE_3_8(n >> 1);
                        o[3] = (n & 1) ? 255 : 0;
                    } else {
                        o[0] = o[1] = o[2] = o[3] = SCALE_4_8(n);
                    }
                    break;
                }
                case G_IM_SIZ_8b: {
                    const uint8_t byte = rdp.tmem[((row + x) ^ xr) & 0xFFF];
                    if (use_tlut) {
                        palette_rgba(tlut_entry(byte), tlut_ia, o);
                    } else if (t.fmt == G_IM_FMT_IA) {
                        o[0] = o[1] = o[2] = SCALE_4_8(byte >> 4);
                        o[3] = SCALE_4_8(byte & 0xF);
                    } else {
                        o[0] = o[1] = o[2] = o[3] = byte;
                    }
                    break;
                }
                case G_IM_SIZ_16b: {
                    const uint32_t a = (row + x * 2) ^ xr;
                    const uint16_t c = (uint16_t)((rdp.tmem[a & 0xFFF] << 8) | rdp.tmem[(a + 1) & 0xFFF]);
                    if (t.fmt == G_IM_FMT_IA) {
                        o[0] = o[1] = o[2] = c >> 8;
                        o[3] = c & 0xFF;
                    } else {
                        palette_rgba(c, false, o);
                    }
                    break;
                }
                case G_IM_SIZ_32b: {
                    const uint32_t a = ((row + x * 2) ^ xr) & 0x7FF;
                    o[0] = rdp.tmem[a];
                    o[1] = rdp.tmem[(a + 1) & 0x7FF];
                    o[2] = rdp.tmem[a | 0x800];
                    o[3] = rdp.tmem[((a + 1) & 0x7FF) | 0x800];
                    break;
                }
            }
        }
    }
}

uint64_t texture_key(const Tile& t, uint32_t w, uint32_t h) {
    const uint32_t tlut = rdp.other_mode_h & (3u << G_MDSFT_TEXTLUT);
    struct {
        uint8_t fmt, siz, pal, tlut;
        uint32_t w, h, line;
        uint64_t data, palette;
    } k{};
    k.fmt = t.fmt; k.siz = t.siz; k.pal = t.palette; k.tlut = (uint8_t)(tlut >> G_MDSFT_TEXTLUT);
    k.w = w; k.h = h; k.line = t.line;
    // Hash the TMEM span the decode reads, with wrap.
    const uint32_t base = t.tmem * 8u;
    uint32_t span = std::min<uint32_t>(h * std::max<uint32_t>(t.line, 1) * 8u, 4096);
    if (t.siz == G_IM_SIZ_32b) {
        uint8_t tmp[4096];
        span = std::min<uint32_t>(span, 2048);
        for (uint32_t i = 0; i < span; i++) {
            tmp[i] = rdp.tmem[(base + i) & 0x7FF];
            tmp[i + span] = rdp.tmem[((base + i) & 0x7FF) | 0x800];
        }
        k.data = XXH3_64bits(tmp, span * 2);
    } else if (base + span <= 4096) {
        k.data = XXH3_64bits(rdp.tmem + base, span);
    } else {
        uint8_t tmp[4096];
        for (uint32_t i = 0; i < span; i++) tmp[i] = rdp.tmem[(base + i) & 0xFFF];
        k.data = XXH3_64bits(tmp, span);
    }
    if (tlut != G_TT_NONE && t.siz <= G_IM_SIZ_8b) {
        if (t.siz == G_IM_SIZ_4b) k.palette = XXH3_64bits(rdp.tmem + 0x800 + t.palette * 128, 128);
        else k.palette = XXH3_64bits(rdp.tmem + 0x800, 2048);
    }
    return XXH3_64bits(&k, sizeof(k));
}

TexEntry* import_texture(int unit, const Tile& t) {
    uint32_t w, h;
    decode_dims(t, w, h);
    const uint64_t key = texture_key(t, w, h);
    auto it = tex_cache.find(key);
    if (it != tex_cache.end()) {
        stats.tex_cache_hits++;
        it->second.last_used = frame_counter;
        if (gls.tex[unit] != it->second.gl) {
            flush();
            gl::bind_texture(unit, it->second.gl);
            gls.tex[unit] = it->second.gl;
        }
        return &it->second;
    }
    stats.tex_decodes++;
    tex_buf.resize((size_t)w * h * 4);
    decode_tile(t, w, h, tex_buf.data());
    flush();
    TexEntry e{};
    e.gl = gl::new_texture();
    e.w = w;
    e.h = h;
    e.last_used = frame_counter;
    e.cms = e.cmt = 0xFF;
    gl::bind_texture(unit, e.gl);
    gl::upload_texture(tex_buf.data(), w, h);
    gls.tex[unit] = e.gl;
    return &tex_cache.emplace(key, e).first->second;
}

void evict_textures() {
    if (tex_cache.size() < 1024) return;
    for (auto it = tex_cache.begin(); it != tex_cache.end();) {
        if (frame_counter - it->second.last_used > 120) {
            if (gls.tex[0] == it->second.gl) gls.tex[0] = 0;
            if (gls.tex[1] == it->second.gl) gls.tex[1] = 0;
            gl::delete_texture(it->second.gl);
            it = tex_cache.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// Colour combiner (Fast3D's gfx_generate_cc, unchanged in substance)
// ---------------------------------------------------------------------------
void generate_cc(ColorCombiner* comb, const ColorCombinerKey& key) {
    bool is_2cyc = (key.options & (uint64_t)SHADER_OPT_2CYC) != 0;
    uint8_t c[2][2][4] = {};
    uint64_t shader_id0 = 0;
    uint32_t shader_id1 = (uint32_t)key.options;
    uint8_t shader_input_mapping[2][7] = {};
    bool used_textures[2] = { false, false };
    for (int i = 0; i < 2 && (i == 0 || is_2cyc); i++) {
        uint32_t rgb_a = (key.combine_mode >> (i * 28)) & 0xf;
        uint32_t rgb_b = (key.combine_mode >> (i * 28 + 4)) & 0xf;
        uint32_t rgb_c = (key.combine_mode >> (i * 28 + 8)) & 0x1f;
        uint32_t rgb_d = (key.combine_mode >> (i * 28 + 13)) & 7;
        uint32_t alpha_a = (key.combine_mode >> (i * 28 + 16)) & 7;
        uint32_t alpha_b = (key.combine_mode >> (i * 28 + 16 + 3)) & 7;
        uint32_t alpha_c = (key.combine_mode >> (i * 28 + 16 + 6)) & 7;
        uint32_t alpha_d = (key.combine_mode >> (i * 28 + 16 + 9)) & 7;
        if (rgb_a >= 8) rgb_a = G_CCMUX_0;
        if (rgb_b >= 8) rgb_b = G_CCMUX_0;
        if (rgb_c >= 16) rgb_c = G_CCMUX_0;
        if (rgb_d == 7) rgb_d = G_CCMUX_0;
        if (rgb_a == rgb_b || rgb_c == G_CCMUX_0) {
            rgb_a = G_CCMUX_0;
            rgb_b = G_CCMUX_0;
            rgb_c = G_CCMUX_0;
        }
        if (alpha_a == alpha_b || alpha_c == G_ACMUX_0) {
            alpha_a = G_ACMUX_0;
            alpha_b = G_ACMUX_0;
            alpha_c = G_ACMUX_0;
        }
        if (i == 1) {
            if (rgb_a != G_CCMUX_COMBINED && rgb_b != G_CCMUX_COMBINED && rgb_c != G_CCMUX_COMBINED &&
                rgb_d != G_CCMUX_COMBINED) {
                c[0][0][0] = c[0][0][1] = c[0][0][2] = c[0][0][3] = G_CCMUX_0;
            }
            if (rgb_c != G_CCMUX_COMBINED_ALPHA && alpha_a != G_ACMUX_COMBINED && alpha_b != G_ACMUX_COMBINED &&
                alpha_d != G_ACMUX_COMBINED) {
                c[0][1][0] = c[0][1][1] = c[0][1][2] = c[0][1][3] = G_ACMUX_0;
            }
        }
        c[i][0][0] = rgb_a;
        c[i][0][1] = rgb_b;
        c[i][0][2] = rgb_c;
        c[i][0][3] = rgb_d;
        c[i][1][0] = alpha_a;
        c[i][1][1] = alpha_b;
        c[i][1][2] = alpha_c;
        c[i][1][3] = alpha_d;
    }
    if (!is_2cyc) {
        for (int i = 0; i < 2; i++) {
            for (int k = 0; k < 4; k++) c[1][i][k] = i == 0 ? G_CCMUX_0 : G_ACMUX_0;
        }
    }
    {
        uint8_t input_number[32] = {};
        int next_input_number = SHADER_INPUT_1;
        for (int i = 0; i < 2 && (i == 0 || is_2cyc); i++) {
            for (int j = 0; j < 4; j++) {
                uint32_t val = 0;
                switch (c[i][0][j]) {
                    case G_CCMUX_0: val = SHADER_0; break;
                    case G_CCMUX_1:
                        // 6 is "1" for A and D, CENTER/SCALE for B and C (K4/K5 unsupported).
                        val = (j == 0 || j == 3) ? SHADER_1 : SHADER_0;
                        break;
                    case G_CCMUX_TEXEL0: val = SHADER_TEXEL0; used_textures[0] = true; break;
                    case G_CCMUX_TEXEL1: val = SHADER_TEXEL1; used_textures[1] = true; break;
                    case G_CCMUX_TEXEL0_ALPHA: val = SHADER_TEXEL0A; used_textures[0] = true; break;
                    case G_CCMUX_TEXEL1_ALPHA: val = SHADER_TEXEL1A; used_textures[1] = true; break;
                    case G_CCMUX_NOISE:
                        // 7: NOISE for A, K4 for B, COMBINED_ALPHA for C, 0 for D.
                        if (j == 0) { val = SHADER_NOISE; break; }
                        if (j == 2) {
                            if (input_number[G_CCMUX_COMBINED_ALPHA + 16] == 0) {
                                shader_input_mapping[0][next_input_number - 1] = G_CCMUX_COMBINED_ALPHA;
                                input_number[G_CCMUX_COMBINED_ALPHA + 16] = next_input_number++;
                            }
                            val = input_number[G_CCMUX_COMBINED_ALPHA + 16];
                            break;
                        }
                        val = SHADER_0;
                        break;
                    case G_CCMUX_PRIMITIVE:
                    case G_CCMUX_PRIMITIVE_ALPHA:
                    case G_CCMUX_PRIM_LOD_FRAC:
                    case G_CCMUX_SHADE:
                    case G_CCMUX_SHADE_ALPHA:
                    case G_CCMUX_ENVIRONMENT:
                    case G_CCMUX_ENV_ALPHA:
                    case G_CCMUX_LOD_FRACTION:
                        if (input_number[c[i][0][j]] == 0) {
                            shader_input_mapping[0][next_input_number - 1] = c[i][0][j];
                            input_number[c[i][0][j]] = next_input_number++;
                        }
                        val = input_number[c[i][0][j]];
                        break;
                    case G_CCMUX_COMBINED: val = SHADER_COMBINED; break;
                    default: val = SHADER_0; break;
                }
                shader_id0 |= (uint64_t)val << (i * 32 + j * 4);
            }
        }
    }
    {
        uint8_t input_number[16] = {};
        int next_input_number = SHADER_INPUT_1;
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 4; j++) {
                uint32_t val = 0;
                switch (c[i][1][j]) {
                    case G_ACMUX_0: val = SHADER_0; break;
                    case G_ACMUX_TEXEL0: val = SHADER_TEXEL0; used_textures[0] = true; break;
                    case G_ACMUX_TEXEL1: val = SHADER_TEXEL1; used_textures[1] = true; break;
                    case G_ACMUX_LOD_FRACTION:
                        if (j != 2) {
                            val = SHADER_COMBINED;
                            break;
                        }
                        c[i][1][j] = G_CCMUX_LOD_FRACTION;
                        [[fallthrough]];
                    case G_ACMUX_1:
                        if (j != 2) {
                            val = SHADER_1;
                            break;
                        }
                        [[fallthrough]];
                    case G_ACMUX_PRIMITIVE:
                    case G_ACMUX_SHADE:
                    case G_ACMUX_ENVIRONMENT:
                        if (input_number[c[i][1][j]] == 0) {
                            shader_input_mapping[1][next_input_number - 1] = c[i][1][j];
                            input_number[c[i][1][j]] = next_input_number++;
                        }
                        val = input_number[c[i][1][j]];
                        break;
                }
                shader_id0 |= (uint64_t)val << (i * 32 + 16 + j * 4);
            }
        }
    }
    comb->shader_id0 = shader_id0;
    comb->shader_id1 = shader_id1;
    comb->used_textures[0] = used_textures[0];
    comb->used_textures[1] = used_textures[1];
    memset(comb->prg, 0, sizeof(comb->prg));
    memcpy(comb->shader_input_mapping, shader_input_mapping, sizeof(shader_input_mapping));
}

ColorCombiner* lookup_combiner(const ColorCombinerKey& key) {
    auto it = combiner_pool.find(key);
    if (it != combiner_pool.end()) return &it->second;
    it = combiner_pool.emplace(key, ColorCombiner{}).first;
    generate_cc(&it->second, key);
    return &it->second;
}

// ---------------------------------------------------------------------------
// Matrices, lights, vertices
// ---------------------------------------------------------------------------
void matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(res, tmp, sizeof(tmp));
}

void read_matrix(uint32_t addr, float m[4][4]) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            int32_t int_part = (int32_t)rd32(addr + (i * 2 + j / 2) * 4);
            uint32_t frac_part = rd32(addr + 32 + (i * 2 + j / 2) * 4);
            m[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
            m[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
        }
    }
}

void recompute_mp() { matrix_mul(rsp.MP, rsp.modelview[rsp.modelview_size - 1], rsp.P); }

void sp_matrix(uint8_t params, uint32_t addr) {
    float m[4][4];
    read_matrix(addr, m);
    if (params & G_MTX_PROJECTION) {
        if (params & G_MTX_LOAD) memcpy(rsp.P, m, sizeof(m));
        else matrix_mul(rsp.P, m, rsp.P);
    } else {
        if ((params & G_MTX_PUSH) && rsp.modelview_size < 18) {
            ++rsp.modelview_size;
            memcpy(rsp.modelview[rsp.modelview_size - 1], rsp.modelview[rsp.modelview_size - 2], sizeof(m));
        }
        if (params & G_MTX_LOAD) memcpy(rsp.modelview[rsp.modelview_size - 1], m, sizeof(m));
        else matrix_mul(rsp.modelview[rsp.modelview_size - 1], m, rsp.modelview[rsp.modelview_size - 1]);
        rsp.lights_changed = true;
    }
    recompute_mp();
}

void sp_pop_matrix() {
    if (rsp.modelview_size > 1) {
        --rsp.modelview_size;
        rsp.lights_changed = true;
        recompute_mp();
    }
}

void normalize(float v[3]) {
    float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (s > 0) { v[0] /= s; v[1] /= s; v[2] /= s; }
}

void light_dir_to_model(const int8_t dir[3], float out[3]) {
    const float d[3] = { dir[0] / 127.f, dir[1] / 127.f, dir[2] / 127.f };
    const float (*m)[4] = rsp.modelview[rsp.modelview_size - 1];
    out[0] = d[0] * m[0][0] + d[1] * m[0][1] + d[2] * m[0][2];
    out[1] = d[0] * m[1][0] + d[1] * m[1][1] + d[2] * m[1][2];
    out[2] = d[0] * m[2][0] + d[1] * m[2][1] + d[2] * m[2][2];
    normalize(out);
}

void read_light(uint32_t addr, Light& l) {
    for (int i = 0; i < 3; i++) {
        l.col[i] = rd8(addr + i);
        l.colc[i] = rd8(addr + 4 + i);
        l.dir[i] = (int8_t)rd8(addr + 8 + i);
    }
}

void sp_vertex(uint32_t n, uint32_t dest, uint32_t addr) {
    for (uint32_t i = 0; i < n; i++, dest++) {
        if (dest >= MAX_VERTICES) break;
        const uint32_t a = addr + i * 16;
        const uint32_t w0 = rd32(a), w1 = rd32(a + 4), w2 = rd32(a + 8), w3 = rd32(a + 12);
        const float vx = (int16_t)(w0 >> 16), vy = (int16_t)w0, vz = (int16_t)(w1 >> 16);
        const int16_t vs = (int16_t)(w2 >> 16), vt = (int16_t)w2;
        const uint8_t c0 = w3 >> 24, c1 = w3 >> 16, c2 = w3 >> 8, c3 = w3;
        LoadedVertex* d = &rsp.verts[dest];
        const float (*M)[4] = rsp.MP;
        float x = vx * M[0][0] + vy * M[1][0] + vz * M[2][0] + M[3][0];
        float y = vx * M[0][1] + vy * M[1][1] + vz * M[2][1] + M[3][1];
        float z = vx * M[0][2] + vy * M[1][2] + vz * M[2][2] + M[3][2];
        float w = vx * M[0][3] + vy * M[1][3] + vz * M[2][3] + M[3][3];
        int32_t U = (int32_t)((vs * (int32_t)rsp.tex_scale_s) >> 16);
        int32_t V = (int32_t)((vt * (int32_t)rsp.tex_scale_t) >> 16);

        if (rsp.geometry_mode & G_LIGHTING) {
            if (rsp.lights_changed) {
                for (int l = 0; l < rsp.num_lights - 1; l++) light_dir_to_model(rsp.lights[l].dir, rsp.light_coeffs[l]);
                light_dir_to_model(rsp.lookat[0].dir, rsp.lookat_coeffs[0]);
                light_dir_to_model(rsp.lookat[1].dir, rsp.lookat_coeffs[1]);
                rsp.lights_changed = false;
            }
            const int8_t nx = (int8_t)c0, ny = (int8_t)c1, nz = (int8_t)c2;
            const Light& amb = rsp.lights[std::max(rsp.num_lights - 1, 0)];
            float r = amb.col[0], g = amb.col[1], b = amb.col[2];
            for (int l = 0; l < rsp.num_lights - 1; l++) {
                float intensity = (nx * rsp.light_coeffs[l][0] + ny * rsp.light_coeffs[l][1] +
                                   nz * rsp.light_coeffs[l][2]) / 127.0f;
                if (intensity > 0.0f) {
                    r += intensity * rsp.lights[l].col[0];
                    g += intensity * rsp.lights[l].col[1];
                    b += intensity * rsp.lights[l].col[2];
                }
            }
            d->color.r = r > 255 ? 255 : (uint8_t)r;
            d->color.g = g > 255 ? 255 : (uint8_t)g;
            d->color.b = b > 255 ? 255 : (uint8_t)b;
            if (rsp.geometry_mode & G_TEXTURE_GEN) {
                float dotx = (nx * rsp.lookat_coeffs[0][0] + ny * rsp.lookat_coeffs[0][1] + nz * rsp.lookat_coeffs[0][2]) / 127.0f;
                float doty = (nx * rsp.lookat_coeffs[1][0] + ny * rsp.lookat_coeffs[1][1] + nz * rsp.lookat_coeffs[1][2]) / 127.0f;
                dotx = std::clamp(dotx, -1.0f, 1.0f);
                doty = std::clamp(doty, -1.0f, 1.0f);
                if (rsp.geometry_mode & G_TEXTURE_GEN_LINEAR) {
                    dotx = acosf(-dotx) / 4.0f;
                    doty = acosf(-doty) / 4.0f;
                } else {
                    dotx = (dotx + 1.0f) / 4.0f;
                    doty = (doty + 1.0f) / 4.0f;
                }
                U = (int32_t)(dotx * rsp.tex_scale_s);
                V = (int32_t)(doty * rsp.tex_scale_t);
            }
        } else {
            d->color.r = c0;
            d->color.g = c1;
            d->color.b = c2;
        }
        d->color.a = c3;
        d->u = (float)U;
        d->v = (float)V;

        if (viewport_is_full(rdp.viewport)) x /= extension();

        d->clip_rej = 0;
        if (x < -w) d->clip_rej |= 1;
        if (x > w) d->clip_rej |= 2;
        if (y < -w) d->clip_rej |= 4;
        if (y > w) d->clip_rej |= 8;
        if (z > w) d->clip_rej |= 32;
        d->x = x; d->y = y; d->z = z; d->w = w;

        if (rsp.geometry_mode & G_FOG) {
            float ww = fabsf(w) < 0.001f ? 0.001f : w;
            float winv = 1.0f / ww;
            if (winv < 0.0f) winv = std::numeric_limits<int16_t>::max();
            float fog_z = z * winv * rsp.fog_mul + rsp.fog_offset;
            d->fog = (uint8_t)std::clamp(fog_z, 0.f, 255.f);
        } else {
            d->fog = rdp.fog_color.a;
        }
        stats.vertices++;
    }
}

// ---------------------------------------------------------------------------
// Triangles
// ---------------------------------------------------------------------------
bool draw_allowed() {
    return draw_limit < 0 || cur_draw_index < draw_limit;
}

void begin_draw_event() {
    stats.draw_events++;
    cur_draw_index = (int32_t)stats.draw_events - 1;
    const bool hl = highlight_index >= 0 && cur_draw_index == highlight_index;
    if (hl != gls.highlight) {
        flush();
        gl::set_highlight(hl);
        gls.highlight = hl;
    }
}

void write_probe(const ColorCombiner* comb, const ColorCombinerKey& key, gl::Program* prg, uint32_t tm,
                 LoadedVertex* const* v, bool is_rect) {
    const std::string path = probe_dir + "/state.txt";
    FILE* f = fopen(path.c_str(), "w");
    if (f == nullptr) {
        fprintf(stderr, "[f3d] probe: cannot write %s\n", path.c_str());
        return;
    }
    fprintf(f, "draw event %d (%s) in frame %u\n", cur_draw_index, is_rect ? "rect" : "triangle", frame_counter);
    fprintf(f, "cimg 0x%06X w %u siz %u  zimg 0x%06X\n", rdp.cimg_addr, rdp.cimg_width, rdp.cimg_siz, rdp.zimg_addr);
    fprintf(f, "othermode H %08X L %08X  geometry %08X\n", rdp.other_mode_h, rdp.other_mode_l, rsp.geometry_mode);
    fprintf(f, "combine %016llX  options %llX  shader id %016llX/%08X tm %u\n", (unsigned long long)key.combine_mode,
            (unsigned long long)key.options, (unsigned long long)comb->shader_id0, comb->shader_id1, tm);
    fprintf(f, "inputs:");
    for (int j = 0; j < 7; j++) fprintf(f, " [c%u a%u]", comb->shader_input_mapping[0][j], comb->shader_input_mapping[1][j]);
    fprintf(f, "\nprim %02X%02X%02X%02X env %02X%02X%02X%02X fog %02X%02X%02X%02X blend %02X%02X%02X%02X fill %08X\n",
            rdp.prim_color.r, rdp.prim_color.g, rdp.prim_color.b, rdp.prim_color.a, rdp.env_color.r, rdp.env_color.g,
            rdp.env_color.b, rdp.env_color.a, rdp.fog_color.r, rdp.fog_color.g, rdp.fog_color.b, rdp.fog_color.a,
            rdp.blend_color.r, rdp.blend_color.g, rdp.blend_color.b, rdp.blend_color.a, rdp.fill_color);
    fprintf(f, "viewport %.2f %.2f %.2f %.2f  scissor %.2f %.2f %.2f %.2f  first tile %u\n", rdp.viewport.x,
            rdp.viewport.y, rdp.viewport.w, rdp.viewport.h, rdp.scissor.ulx, rdp.scissor.uly, rdp.scissor.lrx,
            rdp.scissor.lry, rdp.first_tile);
    for (int i = 0; i < 3; i++) {
        fprintf(f, "v%d: pos %.3f %.3f %.3f %.3f  uv(10.5) %.2f %.2f  rgba %02X%02X%02X%02X fog %u clip %02X\n", i,
                v[i]->x, v[i]->y, v[i]->z, v[i]->w, v[i]->u, v[i]->v, v[i]->color.r, v[i]->color.g, v[i]->color.b,
                v[i]->color.a, v[i]->fog, v[i]->clip_rej);
    }
    for (int t = 0; t < 2; t++) {
        if (!comb->used_textures[t]) continue;
        const Tile& tile = rdp.tiles[(rdp.first_tile + t) & 7];
        uint32_t w, h;
        decode_dims(tile, w, h);
        fprintf(f, "texel%d: tile %u fmt %u siz %u line %u tmem 0x%03X pal %u cms %u masks %u shifts %u cmt %u maskt %u "
                   "shiftt %u size (%.2f,%.2f)-(%.2f,%.2f) -> decoded %ux%u key %016llX\n",
                t, (rdp.first_tile + t) & 7, tile.fmt, tile.siz, tile.line, tile.tmem, tile.palette, tile.cms, tile.masks,
                tile.shifts, tile.cmt, tile.maskt, tile.shiftt, tile.uls / 4.0, tile.ult / 4.0, tile.lrs / 4.0,
                tile.lrt / 4.0, w, h, (unsigned long long)texture_key(tile, w, h));
        std::vector<uint8_t> img((size_t)w * h * 4);
        decode_tile(tile, w, h, img.data());
        const std::string png = probe_dir + "/texel" + std::to_string(t) + ".png";
        stbi_write_png(png.c_str(), w, h, 4, img.data(), w * 4);
    }
    fprintf(f, "\n%s\n", gl::program_source(prg).c_str());
    fclose(f);
}

void emit_triangle(LoadedVertex* v1, LoadedVertex* v2, LoadedVertex* v3, bool is_rect) {
    if (cur_target == nullptr) return;
    if (!draw_allowed()) return;
    if (!is_rect && !opts.no_clip_reject && (v1->clip_rej & v2->clip_rej & v3->clip_rej)) {
        stats.triangles_culled++;
        return;
    }
    if (!is_rect && !opts.no_cull && (rsp.geometry_mode & G_CULL_BOTH) != 0) {
        float dx1 = v1->x / v1->w - v2->x / v2->w;
        float dy1 = v1->y / v1->w - v2->y / v2->w;
        float dx2 = v3->x / v3->w - v2->x / v2->w;
        float dy2 = v3->y / v3->w - v2->y / v2->w;
        float cross = dx1 * dy2 - dy1 * dx2;
        if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) cross = -cross;
        switch (rsp.geometry_mode & G_CULL_BOTH) {
            case G_CULL_FRONT: if (cross <= 0) { stats.triangles_culled++; return; } break;
            case G_CULL_BACK: if (cross >= 0) { stats.triangles_culled++; return; } break;
            case G_CULL_BOTH: stats.triangles_culled++; return;
        }
    }

    const uint32_t cycle = rdp.other_mode_h & (3u << G_MDSFT_CYCLETYPE);
    const bool zbuf = is_rect ? false : (rsp.geometry_mode & G_ZBUFFER) != 0;
    const bool depth_test = (zbuf || (rdp.other_mode_l & G_ZS_PRIM)) && (cycle == G_CYC_1CYCLE || cycle == G_CYC_2CYCLE);
    const bool depth_update = (rdp.other_mode_l & Z_UPD) != 0;
    const bool depth_compare = (rdp.other_mode_l & Z_CMP) != 0;
    const bool decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    const int depth_key = (depth_test ? 1 : 0) | (depth_update ? 2 : 0) | (depth_compare ? 4 : 0) | (decal ? 8 : 0);
    if (depth_key != gls.depth_key) {
        flush();
        gl::set_depth(depth_test, depth_update, depth_compare, decal);
        gls.depth_key = depth_key;
    }

    uint64_t cc_options = 0;
    bool use_alpha = (rdp.other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20) && (rdp.other_mode_l & (3 << 16)) == (G_BL_1MA << 16);
    const bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    const bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    const bool use_noise = (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_DITHER;
    const bool use_2cyc = cycle == G_CYC_2CYCLE;
    const bool alpha_threshold = (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_THRESHOLD;
    const bool invisible = (rdp.other_mode_l & (3 << 24)) == (G_BL_0 << 24) && (rdp.other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20);
    if (texture_edge) use_alpha = true;
    if (cycle == G_CYC_COPY) use_alpha = alpha_threshold;   // copy mode: no blender, only alpha compare
    if (cycle == G_CYC_FILL) use_alpha = false;
    if (use_alpha) cc_options |= SHADER_OPT_ALPHA;
    if (use_fog) cc_options |= SHADER_OPT_FOG;
    if (texture_edge) cc_options |= SHADER_OPT_TEXTURE_EDGE;
    if (use_noise) cc_options |= SHADER_OPT_NOISE;
    if (use_2cyc) cc_options |= SHADER_OPT_2CYC;
    if (alpha_threshold) cc_options |= SHADER_OPT_ALPHA_THRESHOLD;
    if (invisible) cc_options |= SHADER_OPT_INVISIBLE;

    ColorCombinerKey key;
    key.combine_mode = rdp.combine_mode;
    if (cycle == G_CYC_COPY) {
        // Copy mode bypasses the combiner: texel straight through.
        key.combine_mode = (G_CCMUX_0 & 0xf) | ((G_CCMUX_0 & 0xf) << 4) | ((G_CCMUX_0 & 0x1f) << 8) | ((G_CCMUX_TEXEL0 & 7) << 13) |
                           ((uint64_t)((G_ACMUX_0 & 7) | ((G_ACMUX_0 & 7) << 3) | ((G_ACMUX_0 & 7) << 6) | ((G_ACMUX_TEXEL0 & 7) << 9)) << 16);
        cc_options &= ~(uint64_t)(SHADER_OPT_2CYC | SHADER_OPT_FOG);
    } else if (cycle == G_CYC_FILL) {
        key.combine_mode = (G_CCMUX_0 & 0xf) | ((G_CCMUX_0 & 0xf) << 4) | ((G_CCMUX_0 & 0x1f) << 8) | ((G_CCMUX_SHADE & 7) << 13) |
                           ((uint64_t)((G_ACMUX_0 & 7) | ((G_ACMUX_0 & 7) << 3) | ((G_ACMUX_0 & 7) << 6) | ((G_ACMUX_SHADE & 7) << 9)) << 16);
        cc_options &= ~(uint64_t)(SHADER_OPT_2CYC | SHADER_OPT_FOG | SHADER_OPT_TEXTURE_EDGE);
    }
    key.options = cc_options;
    ColorCombiner* comb = lookup_combiner(key);

    uint32_t tm = 0;
    uint32_t tex_w[2] = { 1, 1 }, tex_h[2] = { 1, 1 }, tile_w[2] = { 1, 1 }, tile_h[2] = { 1, 1 };
    const bool linear = opts.bilinear && (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT && cycle != G_CYC_COPY;
    for (int i = 0; i < 2; i++) {
        if (!comb->used_textures[i]) continue;
        const Tile& t = rdp.tiles[(rdp.first_tile + i) & 7];
        TexEntry* e = import_texture(i, t);
        tex_w[i] = e->w;
        tex_h[i] = e->h;
        tile_w[i] = tile_width(t) & 0x3FF;
        tile_h[i] = tile_height(t) & 0x3FF;
        uint32_t cms = t.cms, cmt = t.cmt;
        if (t.masks == 0) cms |= G_TX_CLAMP;
        if (t.maskt == 0) cmt |= G_TX_CLAMP;
        if ((cms & G_TX_CLAMP) && ((cms & G_TX_MIRROR) || tex_w[i] != tile_w[i])) {
            tm |= 1 << (2 * i);
            cms &= ~G_TX_CLAMP;
        }
        if ((cmt & G_TX_CLAMP) && ((cmt & G_TX_MIRROR) || tex_h[i] != tile_h[i])) {
            tm |= 1 << (2 * i + 1);
            cmt &= ~G_TX_CLAMP;
        }
        if (e->linear != linear || e->cms != cms || e->cmt != cmt) {
            flush();
            gl::set_sampler(i, linear, cms, cmt);
            e->linear = linear;
            e->cms = cms;
            e->cmt = cmt;
        }
    }

    gl::Program* prg = comb->prg[tm];
    if (prg == nullptr) {
        comb->prg[tm] = prg = gl::get_program(comb->shader_id0, comb->shader_id1 | (tm * SHADER_OPT_TEXEL0_CLAMP_S));
    }
    if (prg != gls.program) {
        flush();
        gl::use_program(prg);
        gls.program = prg;
    }
    const int blend_mode = invisible ? 2 : (use_alpha ? 1 : 0);
    if (blend_mode != gls.blend_mode) {
        flush();
        gl::set_blend(blend_mode);
        gls.blend_mode = blend_mode;
    }

    const int num_inputs = gl::program_num_inputs(prg);
    LoadedVertex* v_arr[3] = { v1, v2, v3 };
    if (probe_index >= 0 && cur_draw_index == probe_index && !probe_done) {
        probe_done = true;
        write_probe(comb, key, prg, tm, v_arr, is_rect);
    }
    const bool flat = !is_rect && !(rsp.geometry_mode & G_SHADING_SMOOTH);
    // F3DEX.NoN = no near clipping (depth clamp off); GLES 3.0
    // has no depth clamp, so z is rewritten instead. z never moves a vertex on
    // screen (that is x/w, y/w), so:
    //  * all w > 0: clamp z/w into [-1, 1] per vertex (depth clamp);
    //  * any vertex behind the eye: z = 0 on all three, so z/w is 0 across the
    //    whole triangle and only the x/y planes and w = 0 can clip it. A per-
    //    vertex fix is not enough there: interpolated z escapes [-w, w] near the
    //    horizon and GL cuts a band off (Wetrix's gameplay sky).
    const bool behind_eye = v1->w <= 0.0f || v2->w <= 0.0f || v3->w <= 0.0f;
    for (int i = 0; i < 3; i++) {
        LoadedVertex* v = v_arr[i];
        vbo_buf[vbo_len++] = v->x;
        vbo_buf[vbo_len++] = v->y;
        float z = behind_eye ? 0.0f : std::clamp(v->z, -v->w, v->w);
        vbo_buf[vbo_len++] = z;
        vbo_buf[vbo_len++] = v->w;
        for (int t = 0; t < 2; t++) {
            if (!gl::program_uses_texture(prg, t)) continue;
            const Tile& tile = rdp.tiles[(rdp.first_tile + t) & 7];
            float u = v->u / 32.0f;
            float vv = v->v / 32.0f;
            if (tile.shifts != 0) {
                if (tile.shifts <= 10) u /= (float)(1 << tile.shifts);
                else u *= (float)(1 << (16 - tile.shifts));
            }
            if (tile.shiftt != 0) {
                if (tile.shiftt <= 10) vv /= (float)(1 << tile.shiftt);
                else vv *= (float)(1 << (16 - tile.shiftt));
            }
            u -= tile.uls / 4.0f;
            vv -= tile.ult / 4.0f;
            if (!is_rect) {
                if (!(rdp.other_mode_h & G_TP_PERSP)) {
                    u *= 0.5f;
                    vv *= 0.5f;
                }
                if (linear) {
                    u += 0.5f;
                    vv += 0.5f;
                }
            } else {
                // RDP texel i spans [i, i+1) and its bilinear weight is full at
                // s = i; a GL texel centre is at i + 0.5. Point sampling gets a
                // 1/64 nudge (below the 10.5 step) so exact integers do not round
                // down through float error.
                const float bias = linear ? 0.5f : (1.0f / 64.0f);
                u += bias;
                vv += bias;
            }
            vbo_buf[vbo_len++] = u / tex_w[t];
            vbo_buf[vbo_len++] = vv / tex_h[t];
            if (tm & (1 << (2 * t))) vbo_buf[vbo_len++] = (tile_w[t] - 0.5f) / tex_w[t];
            if (tm & (1 << (2 * t + 1))) vbo_buf[vbo_len++] = (tile_h[t] - 0.5f) / tex_h[t];
        }
        if (cc_options & SHADER_OPT_FOG) {
            vbo_buf[vbo_len++] = rdp.fog_color.r / 255.0f;
            vbo_buf[vbo_len++] = rdp.fog_color.g / 255.0f;
            vbo_buf[vbo_len++] = rdp.fog_color.b / 255.0f;
            vbo_buf[vbo_len++] = v->fog / 255.0f;
        }
        const RGBA& shade = flat ? v1->color : v->color;
        for (int j = 0; j < num_inputs; j++) {
            for (int k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
                RGBA tmp = { 0, 0, 0, 0 };
                const RGBA* color = &tmp;
                switch (comb->shader_input_mapping[k][j]) {
                    case G_CCMUX_PRIMITIVE: color = &rdp.prim_color; break;
                    case G_CCMUX_SHADE: color = &shade; break;
                    case G_CCMUX_ENVIRONMENT: color = &rdp.env_color; break;
                    case G_CCMUX_SHADE_ALPHA: tmp.r = tmp.g = tmp.b = shade.a; break;
                    case G_CCMUX_PRIMITIVE_ALPHA: tmp.r = tmp.g = tmp.b = rdp.prim_color.a; break;
                    case G_CCMUX_ENV_ALPHA: tmp.r = tmp.g = tmp.b = rdp.env_color.a; break;
                    case G_CCMUX_PRIM_LOD_FRAC: tmp.r = tmp.g = tmp.b = rdp.prim_lod_frac; break;
                    case G_CCMUX_LOD_FRACTION: tmp.r = tmp.g = tmp.b = tmp.a = 255; break;
                    case G_CCMUX_COMBINED_ALPHA: tmp.r = tmp.g = tmp.b = 255; break;
                    case G_ACMUX_PRIM_LOD_FRAC: tmp.a = rdp.prim_lod_frac; break;
                    default: break;
                }
                if (k == 0) {
                    vbo_buf[vbo_len++] = color->r / 255.0f;
                    vbo_buf[vbo_len++] = color->g / 255.0f;
                    vbo_buf[vbo_len++] = color->b / 255.0f;
                } else {
                    vbo_buf[vbo_len++] = color->a / 255.0f;
                }
            }
        }
    }
    stats.triangles++;
    if (++vbo_tris == MAX_BUFFERED) flush();
}

void sp_tri(uint32_t a, uint32_t b, uint32_t c) {
    if (a >= MAX_VERTICES || b >= MAX_VERTICES || c >= MAX_VERTICES) return;
    bind_current_target();
    apply_viewport(rdp.viewport);
    emit_triangle(&rsp.verts[a], &rsp.verts[b], &rsp.verts[c], false);
}

// ---------------------------------------------------------------------------
// Rectangles
// ---------------------------------------------------------------------------
void draw_rect(float ulx, float uly, float lrx, float lry, bool stretch_wide = false) {
    bind_current_target();
    if (cur_target == nullptr) return;
    const float W = (float)cur_target->width, H = (float)cur_target->height;
    // Widescreen: a solid fill across the image's width (a clear, a backdrop)
    // stretches over the whole target; everything else stays in the 4:3 area.
    if (stretch_wide && cur_target->pad > 0.0f && spans_width(ulx, lrx, W)) {
        ulx = -cur_target->pad;
        lrx = W + cur_target->pad;
    }
    const float x0 = ulx / W * 2.0f - 1.0f, x1 = lrx / W * 2.0f - 1.0f;
    const float y0 = 1.0f - uly / H * 2.0f, y1 = 1.0f - lry / H * 2.0f;
    float z = -1.0f;
    if (rdp.other_mode_l & G_ZS_PRIM) z = (rdp.prim_depth / 32767.0f) * 2.0f - 1.0f;
    LoadedVertex* ul = &rsp.verts[MAX_VERTICES + 0];
    LoadedVertex* ll = &rsp.verts[MAX_VERTICES + 1];
    LoadedVertex* lr = &rsp.verts[MAX_VERTICES + 2];
    LoadedVertex* ur = &rsp.verts[MAX_VERTICES + 3];
    ul->x = x0; ul->y = y0;
    ll->x = x0; ll->y = y1;
    lr->x = x1; lr->y = y1;
    ur->x = x1; ur->y = y0;
    for (LoadedVertex* v : { ul, ll, lr, ur }) {
        v->z = z;
        v->w = 1.0f;
        v->clip_rej = 0;
        v->fog = 0;
    }
    apply_viewport(Viewport{ 0, 0, W, H });
    emit_triangle(ul, ll, ur, true);
    emit_triangle(ll, lr, ur, true);
    stats.rects++;
}

void texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult,
                       int16_t dsdx, int16_t dtdy, bool flip) {
    const uint32_t cycle = rdp.other_mode_h & (3u << G_MDSFT_CYCLETYPE);
    if (cycle == G_CYC_COPY) {
        dsdx >>= 2;
        lrx += 4;
        lry += 4;
    }
    const float width = flip ? (lry - uly) : (lrx - ulx);    // 10.2
    const float height = flip ? (lrx - ulx) : (lry - uly);
    // uls/ult S10.5, dsdx/dtdy S5.10, width in 10.2 -> lrs in 10.5
    const float lrs = uls + dsdx * width / 128.0f;
    const float lrt = ult + dtdy * height / 128.0f;
    // The RDP evaluates s/t at each pixel's top-left corner; GL interpolates to
    // the pixel centre. Moving every vertex by -1/2 of the per-pixel step makes
    // the centre see the corner's value. Without it a negative step (Wetrix's
    // font is drawn with TEXRECTFLIP and dsdx = -1) samples one texel early and
    // the text lands a pixel off. Units: s/t 10.5, dsdx/dtdy 5.10 per pixel.
    const float s_ofs = -0.5f * dsdx / 32.0f;
    const float t_ofs = -0.5f * dtdy / 32.0f;
    LoadedVertex* ul = &rsp.verts[MAX_VERTICES + 0];
    LoadedVertex* ll = &rsp.verts[MAX_VERTICES + 1];
    LoadedVertex* lr = &rsp.verts[MAX_VERTICES + 2];
    LoadedVertex* ur = &rsp.verts[MAX_VERTICES + 3];
    ul->u = uls; ul->v = ult;
    lr->u = lrs; lr->v = lrt;
    if (!flip) {
        ll->u = uls; ll->v = lrt;
        ur->u = lrs; ur->v = ult;
    } else {
        ll->u = lrs; ll->v = ult;
        ur->u = uls; ur->v = lrt;
    }
    for (LoadedVertex* v : { ul, ll, lr, ur }) {
        v->u += s_ofs;
        v->v += t_ofs;
    }
    for (LoadedVertex* v : { ul, ll, lr, ur }) v->color = { 255, 255, 255, 255 };
    const uint8_t saved = rdp.first_tile;
    rdp.first_tile = tile;
    begin_draw_event();
    draw_rect(ulx / 4.0f, uly / 4.0f, lrx / 4.0f, lry / 4.0f);
    rdp.first_tile = saved;
}

void fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    const uint32_t cycle = rdp.other_mode_h & (3u << G_MDSFT_CYCLETYPE);
    stats.fills++;
    if (rdp.cimg_addr == rdp.zimg_addr && rdp.zimg_addr != 0) {
        // Z buffer clear.
        flush();
        depth_clear_pending = true;
        if (cur_target != nullptr) {
            gl::clear(false, true);
            gls.sc[2] = -1;
            depth_clear_pending = false;
        }
        return;
    }
    if (cycle == G_CYC_COPY || cycle == G_CYC_FILL) {
        lrx += 4;
        lry += 4;
    }
    RGBA c;
    if (cycle == G_CYC_FILL) {
        if (rdp.cimg_siz == G_IM_SIZ_32b) {
            c = { (uint8_t)(rdp.fill_color >> 24), (uint8_t)(rdp.fill_color >> 16), (uint8_t)(rdp.fill_color >> 8),
                  (uint8_t)rdp.fill_color };
        } else {
            const uint16_t c16 = (uint16_t)rdp.fill_color;
            c = { (uint8_t)SCALE_5_8(c16 >> 11), (uint8_t)SCALE_5_8((c16 >> 6) & 0x1F), (uint8_t)SCALE_5_8((c16 >> 1) & 0x1F),
                  (uint8_t)((c16 & 1) ? 255 : 0) };
        }
        c.a = 255;
    } else {
        c = rdp.prim_color;   // 1/2-cycle fill rects shade from the combiner inputs
    }
    for (int i = MAX_VERTICES; i < MAX_VERTICES + 4; i++) rsp.verts[i].color = c;
    // A fill-mode full-screen rect on a fresh target is a clear; keep it as a draw
    // so draw_limit / highlight still see it.
    begin_draw_event();
    draw_rect(ulx / 4.0f, uly / 4.0f, lrx / 4.0f, lry / 4.0f, true);
}

// ---------------------------------------------------------------------------
// RDRAM <-> target
// ---------------------------------------------------------------------------
void decode_rdram_fb(uint32_t addr, uint32_t w, uint32_t h, uint8_t siz, std::vector<uint8_t>& out) {
    out.resize((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* o = &out[((size_t)y * w + x) * 4];
            if (siz == G_IM_SIZ_32b) {
                const uint32_t p = rd32(addr + (y * w + x) * 4);
                o[0] = p >> 24; o[1] = p >> 16; o[2] = p >> 8; o[3] = 255;
            } else {
                const uint16_t p = rd16(addr + (y * w + x) * 2);
                o[0] = SCALE_5_8(p >> 11); o[1] = SCALE_5_8((p >> 6) & 0x1F); o[2] = SCALE_5_8((p >> 1) & 0x1F); o[3] = 255;
            }
        }
    }
}

// Seeds a brand-new target with whatever the CPU left in its RDRAM, so a frame
// that is partly CPU-drawn starts from the right pixels.
void import_rdram_into(Target& t) {
    std::vector<uint8_t> img;
    decode_rdram_fb(t.addr, t.width, t.height, t.siz, img);
    // Upload bottom-up, then scale into the FBO through a temporary.
    std::vector<uint8_t> flipped(img.size());
    const size_t row = (size_t)t.width * 4;
    for (uint32_t y = 0; y < t.height; y++) memcpy(&flipped[y * row], &img[(t.height - 1 - y) * row], row);
    gl::Fbo tmp = gl::create_fbo(t.width, t.height, 0);
    gl::bind_texture(0, tmp.color);
    gl::upload_texture(flipped.data(), t.width, t.height);
    gls.tex[0] = 0;
    gl::bind_fbo(&t.fbo);
    const int s = target_scale();
    gl::blit(tmp, (int)std::lround(t.pad * s), 0, (int)t.width * s, t.fbo.h, false);
    gl::delete_fbo(tmp);
}

// ---------------------------------------------------------------------------
// Display list walker
// ---------------------------------------------------------------------------
void sp_reset() {
    rsp.modelview_size = 1;
    memset(rsp.modelview[0], 0, sizeof(rsp.modelview[0]));
    for (int i = 0; i < 4; i++) rsp.modelview[0][i][i] = 1.0f;
    memcpy(rsp.P, rsp.modelview[0], sizeof(rsp.P));
    recompute_mp();
    rsp.num_lights = 2;
    rsp.lights_changed = true;
    rsp.geometry_mode = 0;
    memset(rsp.segments, 0, sizeof(rsp.segments));
    rsp.lookat[0] = { { 0, 0, 0 }, { 0, 0, 0 }, { 127, 0, 0 } };
    rsp.lookat[1] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 127, 0 } };
}

void run(uint32_t start) {
    uint32_t stack[32];
    int depth = 0;
    uint32_t pc = start;
    uint32_t guard = 0;
    for (;;) {
        if (++guard > 2000000) {
            fprintf(stderr, "[f3d] display list runaway at 0x%08X, stopping\n", pc);
            return;
        }
        const uint32_t w0 = rd32(pc), w1 = rd32(pc + 4);
        const uint8_t op = w0 >> 24;
        const uint32_t this_pc = pc;
        pc += 8;
        stats.commands++;
        const int32_t draws_before = (int32_t)stats.draw_events;
        if (trace_file != nullptr) {
            fprintf(trace_file, "%*s%06X: %08X %08X  %s\n", depth * 2, "", this_pc, w0, w1, disasm(w0, w1).c_str());
        }

        switch (op) {
            case G_SPNOOP:
            case G_NOOP:
            case G_RDPLOADSYNC:
            case G_RDPPIPESYNC:
            case G_RDPTILESYNC:
            case G_RDPFULLSYNC:
            case G_SETKEYGB:
            case G_SETKEYR:
            case G_SETCONVERT:
                break;
            case G_MTX:
                flush();
                sp_matrix(bits(w0, 16, 8), seg_addr(w1));
                break;
            case G_POPMTX:
                sp_pop_matrix();
                break;
            case G_MOVEMEM: {
                const uint8_t idx = bits(w0, 16, 8);
                const uint32_t a = seg_addr(w1);
                if (idx == G_MV_VIEWPORT) {
                    for (int i = 0; i < 4; i++) {
                        rsp.vp.vscale[i] = (int16_t)rd16(a + i * 2);
                        rsp.vp.vtrans[i] = (int16_t)rd16(a + 8 + i * 2);
                    }
                    const float sx = rsp.vp.vscale[0] / 4.0f, sy = rsp.vp.vscale[1] / 4.0f;
                    const float tx = rsp.vp.vtrans[0] / 4.0f, ty = rsp.vp.vtrans[1] / 4.0f;
                    rdp.viewport = { tx - fabsf(sx), ty - fabsf(sy), 2 * fabsf(sx), 2 * fabsf(sy) };
                } else if (idx == G_MV_LOOKATY || idx == G_MV_LOOKATX) {
                    read_light(a, rsp.lookat[idx == G_MV_LOOKATX ? 0 : 1]);
                    rsp.lights_changed = true;
                } else if (idx >= G_MV_L0 && idx <= G_MV_L7) {
                    read_light(a, rsp.lights[(idx - G_MV_L0) / 2]);
                    rsp.lights_changed = true;
                }
                break;
            }
            case G_MOVEWORD: {
                const uint8_t index = bits(w0, 0, 8);
                const uint16_t offset = bits(w0, 8, 16);
                switch (index) {
                    case G_MW_NUMLIGHT:
                        rsp.num_lights = std::clamp<int>((int)((w1 - 0x80000000u) / 32), 1, 8);
                        rsp.lights_changed = true;
                        break;
                    case G_MW_SEGMENT:
                        rsp.segments[(offset >> 2) & 0xF] = w1 & 0x00FFFFFF;
                        break;
                    case G_MW_FOG:
                        rsp.fog_mul = (int16_t)(w1 >> 16);
                        rsp.fog_offset = (int16_t)w1;
                        break;
                    case G_MW_LIGHTCOL: {
                        const int l = (offset / 0x20) & 7;
                        if ((offset & 7) == 0) {
                            rsp.lights[l].col[0] = w1 >> 24; rsp.lights[l].col[1] = w1 >> 16; rsp.lights[l].col[2] = w1 >> 8;
                        } else {
                            rsp.lights[l].colc[0] = w1 >> 24; rsp.lights[l].colc[1] = w1 >> 16; rsp.lights[l].colc[2] = w1 >> 8;
                        }
                        break;
                    }
                    case G_MW_PERSPNORM:
                    case G_MW_CLIP:
                    case G_MW_MATRIX:
                    case G_MW_POINTS:
                        break;
                }
                break;
            }
            case G_TEXTURE:
                rsp.tex_scale_s = bits(w1, 16, 16);
                rsp.tex_scale_t = bits(w1, 0, 16);
                rdp.first_tile = bits(w0, 8, 3);
                rsp.tex_on = bits(w0, 0, 8) != 0;
                break;
            case G_VTX: {
                const uint32_t n = bits(w0, 10, 6);
                const uint32_t v0 = bits(w0, 16, 8) / 2;
                sp_vertex(n, v0, seg_addr(w1));
                break;
            }
            case G_MODIFYVTX: {
                const uint32_t where = bits(w0, 16, 8);
                const uint32_t vi = bits(w0, 0, 16) / 2;
                if (vi < MAX_VERTICES) {
                    LoadedVertex& v = rsp.verts[vi];
                    if (where == G_MWO_POINT_ST) {
                        v.u = (int16_t)(w1 >> 16);
                        v.v = (int16_t)w1;
                    } else if (where == G_MWO_POINT_RGBA) {
                        v.color = { (uint8_t)(w1 >> 24), (uint8_t)(w1 >> 16), (uint8_t)(w1 >> 8), (uint8_t)w1 };
                    } else if (where == G_MWO_POINT_XYSCREEN) {
                        const float sx = (int16_t)(w1 >> 16) / 4.0f, sy = (int16_t)w1 / 4.0f;
                        const float vsx = rsp.vp.vscale[0] / 4.0f, vsy = rsp.vp.vscale[1] / 4.0f;
                        if (vsx != 0 && vsy != 0) {
                            v.x = (sx - rsp.vp.vtrans[0] / 4.0f) / vsx * v.w;
                            v.y = -(sy - rsp.vp.vtrans[1] / 4.0f) / vsy * v.w;
                        }
                    }
                }
                break;
            }
            case G_TRI1:
                begin_draw_event();
                sp_tri(bits(w1, 16, 8) / 2, bits(w1, 8, 8) / 2, bits(w1, 0, 8) / 2);
                break;
            case G_TRI2:
                begin_draw_event();
                sp_tri(bits(w0, 16, 8) / 2, bits(w0, 8, 8) / 2, bits(w0, 0, 8) / 2);
                sp_tri(bits(w1, 16, 8) / 2, bits(w1, 8, 8) / 2, bits(w1, 0, 8) / 2);
                break;
            case G_LINE3D: {
                // F3DEX: gSP1Quadrangle rides on the LINE3D opcode.
                const uint32_t a = bits(w1, 24, 8) / 2, b = bits(w1, 16, 8) / 2, c = bits(w1, 8, 8) / 2,
                               d = bits(w1, 0, 8) / 2;
                begin_draw_event();
                sp_tri(a, b, c);
                sp_tri(a, c, d);
                break;
            }
            case G_CULLDL: {
                const uint32_t vs = bits(w0, 0, 16) / 2, ve = bits(w1, 0, 16) / 2;
                uint8_t rej = 0xFF;
                for (uint32_t i = vs; i <= ve && i < MAX_VERTICES; i++) rej &= rsp.verts[i].clip_rej;
                if ((rej & 0xF) && !opts.no_culldl) {
                    if (depth == 0) return;
                    pc = stack[--depth];
                }
                break;
            }
            case G_BRANCH_Z: {
                bool take = opts.force_branch_z;
                if (!take) {
                    const uint32_t vi = bits(w0, 0, 12) / 2;
                    if (vi < MAX_VERTICES) {
                        const LoadedVertex& v = rsp.verts[vi];
                        const float zs = (v.w != 0 ? v.z / v.w : 0) * rsp.vp.vscale[2] + rsp.vp.vtrans[2];
                        take = (int32_t)(zs * 32.0f) <= (int32_t)w1;
                    }
                }
                if (take) pc = seg_addr(rsp.rdphalf1);
                break;
            }
            case G_DL:
                stats.dl_calls++;
                if (bits(w0, 16, 8) == 0) {
                    if (depth < 32) stack[depth++] = pc;
                }
                pc = seg_addr(w1);
                break;
            case G_ENDDL:
                if (depth == 0) {
                    if (trace_file) fprintf(trace_file, "-- end of task\n");
                    return;
                }
                pc = stack[--depth];
                break;
            case G_SETGEOMETRYMODE:
                rsp.geometry_mode |= w1;
                break;
            case G_CLEARGEOMETRYMODE:
                rsp.geometry_mode &= ~w1;
                break;
            case G_SETOTHERMODE_L:
            case G_SETOTHERMODE_H: {
                const uint32_t shift = bits(w0, 8, 8), len = bits(w0, 0, 8);
                const uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
                uint32_t& om = op == G_SETOTHERMODE_L ? rdp.other_mode_l : rdp.other_mode_h;
                flush();
                om = (om & ~mask) | (w1 & mask);
                break;
            }
            case G_RDPSETOTHERMODE:
                flush();
                rdp.other_mode_h = w0 & 0x00FFFFFF;
                rdp.other_mode_l = w1;
                break;
            case G_RDPHALF_1:
                rsp.rdphalf1 = w1;
                break;
            case G_RDPHALF_2:
                break;
            case G_LOAD_UCODE:
            case G_SPRITE2D_BASE:
                if (!warned_ops[op]++) fprintf(stderr, "[f3d] unhandled %s (0x%02X) %08X %08X\n", gbi::opcode_name(op), op, w0, w1);
                stats.unknown_commands++;
                break;

            // RDP
            case G_SETTIMG:
                rdp.timg.fmt = bits(w0, 21, 3);
                rdp.timg.siz = bits(w0, 19, 2);
                rdp.timg.width = bits(w0, 0, 12) + 1;
                rdp.timg.addr = seg_addr(w1);
                break;
            case G_SETZIMG:
                flush();
                rdp.zimg_addr = seg_addr(w1);
                break;
            case G_SETCIMG:
                flush();
                rdp.cimg_fmt = bits(w0, 21, 3);
                rdp.cimg_siz = bits(w0, 19, 2);
                rdp.cimg_width = bits(w0, 0, 12) + 1;
                rdp.cimg_addr = seg_addr(w1);
                break;
            case G_SETTILE: {
                flush();
                Tile& t = rdp.tiles[bits(w1, 24, 3)];
                t.fmt = bits(w0, 21, 3);
                t.siz = bits(w0, 19, 2);
                t.line = bits(w0, 9, 9);
                t.tmem = bits(w0, 0, 9);
                t.palette = bits(w1, 20, 4);
                t.cmt = bits(w1, 18, 2);
                t.maskt = bits(w1, 14, 4);
                t.shiftt = bits(w1, 10, 4);
                t.cms = bits(w1, 8, 2);
                t.masks = bits(w1, 4, 4);
                t.shifts = bits(w1, 0, 4);
                break;
            }
            case G_SETTILESIZE: {
                flush();
                Tile& t = rdp.tiles[bits(w1, 24, 3)];
                t.uls = bits(w0, 12, 12);
                t.ult = bits(w0, 0, 12);
                t.lrs = bits(w1, 12, 12);
                t.lrt = bits(w1, 0, 12);
                break;
            }
            case G_LOADBLOCK:
                flush();
                load_block(bits(w1, 24, 3), bits(w0, 12, 12), bits(w0, 0, 12), bits(w1, 12, 12), bits(w1, 0, 12));
                break;
            case G_LOADTILE:
                flush();
                load_tile(bits(w1, 24, 3), bits(w0, 12, 12), bits(w0, 0, 12), bits(w1, 12, 12), bits(w1, 0, 12));
                break;
            case G_LOADTLUT:
                flush();
                load_tlut(bits(w1, 24, 3), bits(w0, 12, 12), bits(w0, 0, 12), bits(w1, 12, 12), bits(w1, 0, 12));
                break;
            case G_SETENVCOLOR:
                rdp.env_color = { (uint8_t)(w1 >> 24), (uint8_t)(w1 >> 16), (uint8_t)(w1 >> 8), (uint8_t)w1 };
                break;
            case G_SETPRIMCOLOR:
                rdp.prim_color = { (uint8_t)(w1 >> 24), (uint8_t)(w1 >> 16), (uint8_t)(w1 >> 8), (uint8_t)w1 };
                rdp.prim_lod_frac = bits(w0, 0, 8);
                rdp.prim_min_lod = bits(w0, 8, 8);
                break;
            case G_SETFOGCOLOR:
                rdp.fog_color = { (uint8_t)(w1 >> 24), (uint8_t)(w1 >> 16), (uint8_t)(w1 >> 8), (uint8_t)w1 };
                break;
            case G_SETBLENDCOLOR:
                rdp.blend_color = { (uint8_t)(w1 >> 24), (uint8_t)(w1 >> 16), (uint8_t)(w1 >> 8), (uint8_t)w1 };
                break;
            case G_SETFILLCOLOR:
                rdp.fill_color = w1;
                break;
            case G_SETPRIMDEPTH:
                rdp.prim_depth = bits(w1, 16, 16);
                break;
            case G_SETCOMBINE: {
                auto cc = [](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
                    return (a & 0xf) | ((b & 0xf) << 4) | ((c & 0x1f) << 8) | ((d & 7) << 13);
                };
                auto ac = [](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
                    return (a & 7) | ((b & 7) << 3) | ((c & 7) << 6) | ((d & 7) << 9);
                };
                const uint32_t rgb = cc(bits(w0, 20, 4), bits(w1, 28, 4), bits(w0, 15, 5), bits(w1, 15, 3));
                const uint32_t alpha = ac(bits(w0, 12, 3), bits(w1, 12, 3), bits(w0, 9, 3), bits(w1, 9, 3));
                const uint32_t rgb2 = cc(bits(w0, 5, 4), bits(w1, 24, 4), bits(w0, 0, 5), bits(w1, 6, 3));
                const uint32_t alpha2 = ac(bits(w1, 21, 3), bits(w1, 3, 3), bits(w1, 18, 3), bits(w1, 0, 3));
                rdp.combine_mode = rgb | ((uint64_t)alpha << 16) | ((uint64_t)rgb2 << 28) | ((uint64_t)alpha2 << 44);
                break;
            }
            case G_SETSCISSOR:
                rdp.scissor.ulx = bits(w0, 12, 12) / 4.0f;
                rdp.scissor.uly = bits(w0, 0, 12) / 4.0f;
                rdp.scissor.lrx = bits(w1, 12, 12) / 4.0f;
                rdp.scissor.lry = bits(w1, 0, 12) / 4.0f;
                break;
            case G_FILLRECT:
                fill_rectangle(bits(w1, 12, 12), bits(w1, 0, 12), bits(w0, 12, 12), bits(w0, 0, 12));
                break;
            case G_TEXRECT:
            case G_TEXRECTFLIP: {
                const uint32_t w2 = rd32(pc + 4), w3 = rd32(pc + 12);
                if (trace_file != nullptr) {
                    fprintf(trace_file, "%*s        (+ %08X %08X)\n", depth * 2, "", w2, w3);
                }
                pc += 16;
                texture_rectangle(bits(w1, 12, 12), bits(w1, 0, 12), bits(w0, 12, 12), bits(w0, 0, 12),
                                  bits(w1, 24, 3), (int16_t)(w2 >> 16), (int16_t)w2, (int16_t)(w3 >> 16), (int16_t)w3,
                                  op == G_TEXRECTFLIP);
                break;
            }
            default:
                if (op >= 0xC8 && op <= 0xCF) {
                    if (!warned_ops[op]++) fprintf(stderr, "[f3d] raw RDP triangle 0x%02X in DL, skipped\n", op);
                } else if (!warned_ops[op]++) {
                    fprintf(stderr, "[f3d] unknown opcode 0x%02X at 0x%06X: %08X %08X\n", op, this_pc, w0, w1);
                }
                stats.unknown_commands++;
                break;
        }
        cmd_log.push_back({ this_pc, w0, w1, (uint16_t)depth,
                            (int32_t)stats.draw_events != draws_before ? (int32_t)stats.draw_events - 1 : -1 });
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool init(uint8_t* rdram_ptr, uint32_t rdram_size, const Options& o, bool gles) {
    rdram = rdram_ptr;
    rdram_mask = rdram_size - 1;
    opts = o;
    use_gles = gles;
    if (!gl::init(gles)) return false;
    memset(&rsp, 0, sizeof(rsp));
    memset(&rdp, 0, sizeof(rdp));
    sp_reset();
    rdp.scissor = { 0, 0, 320, 240 };
    rdp.viewport = { 0, 0, 320, 240 };
    return true;
}

void shutdown() {}

uint8_t* set_memory(uint8_t* m) {
    uint8_t* old = rdram;
    rdram = m;
    return old;
}

void run_dl(uint32_t dl_phys, uint32_t ucode_phys, uint32_t ucode_data_phys) {
    (void)ucode_phys;
    (void)ucode_data_phys;
    const auto t0 = std::chrono::steady_clock::now();
    stats = Stats{};
    stats.frame = ++frame_counter;
    cmd_log.clear();
    cur_draw_index = -1;
    sp_reset();
    cur_target = nullptr;
    gls = {};
    gls.highlight = false;
    gl::set_highlight(false);
    gl::set_blend(0);
    run(dl_phys & 0x03FFFFFF);
    flush();
    if (gls.highlight) gl::set_highlight(false);
    if (trace_file != nullptr) {
        fflush(trace_file);
        trace_file = nullptr;
    }
    evict_textures();
    stats.run_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    last = stats;
    last_cmd_log.swap(cmd_log);
}

void present(uint32_t vi_origin, uint32_t vi_width, uint32_t vi_status, uint32_t vi_x_scale, uint32_t vi_y_scale,
             int ww, int wh) {
    window_w = ww;
    window_h = wh;
    const uint32_t origin = vi_origin & 0x00FFFFFF;
    const Target* show = nullptr;
    for (auto& [addr, t] : targets_by_addr) {
        const uint32_t size = t.width * t.height * (t.siz == G_IM_SIZ_32b ? 4 : 2);
        if (origin >= addr && origin < addr + size) {
            show = &t;
            break;
        }
    }
    gl::bind_fbo(nullptr, ww, wh);
    gl::clear(true, false, 0, 0, 0, 1);
    const gl::Fbo* src = nullptr;
    presented_cpu = false;
    if (show != nullptr) {
        src = &show->fbo;
        presented_addr = show->addr;
    } else if ((vi_status & 3) >= 2 && origin != 0 && vi_width != 0) {
        // Nothing the RDP drew: show what the CPU left in RDRAM.
        const uint32_t w = vi_width;
        // The VI's scale registers say how many lines are scanned out; the
        // framebuffer itself carries no height, and 4:3 is what they give for Wetrix.
        (void)vi_x_scale;
        (void)vi_y_scale;
        const uint32_t h = std::clamp<uint32_t>(w * 3 / 4, 1, 576);
        std::vector<uint8_t> img;
        decode_rdram_fb(origin, w, h, (vi_status & 3) == 3 ? G_IM_SIZ_32b : G_IM_SIZ_16b, img);
        std::vector<uint8_t> flipped(img.size());
        const size_t row = (size_t)w * 4;
        for (uint32_t y = 0; y < h; y++) memcpy(&flipped[y * row], &img[(h - 1 - y) * row], row);
        if (cpu_fb.w != (int)w || cpu_fb.h != (int)h) {
            if (cpu_fb.fbo) gl::delete_fbo(cpu_fb);
            cpu_fb = gl::create_fbo(w, h, 0);
        }
        gl::bind_texture(0, cpu_fb.color);
        gl::upload_texture(flipped.data(), w, h);
        gls.tex[0] = 0;
        src = &cpu_fb;
        presented_addr = origin;
        presented_cpu = true;
        last.cpu_fallback_presents++;
    }
    if (src != nullptr) {
        // Letterbox at the picture's aspect: 4:3 times however much the target was widened.
        const float aspect = presented_cpu || show == nullptr
                                 ? 4.0f / 3.0f
                                 : 4.0f / 3.0f * (float)show->ext_width / (float)show->width;
        int dw = ww, dh = wh;
        if ((float)ww > (float)wh * aspect) dw = (int)std::lround(wh * aspect);
        else dh = (int)std::lround(ww / aspect);
        gl::bind_fbo(nullptr, ww, wh);
        gl::blit(*src, (ww - dw) / 2, (wh - dh) / 2, dw, dh, true);
    }
    gl::finish_frame();
}

bool grab_presented(std::vector<uint8_t>& rgba, int& w, int& h) {
    const gl::Fbo* src = nullptr;
    if (presented_cpu) src = &cpu_fb;
    else {
        auto it = targets_by_addr.find(presented_addr);
        if (it != targets_by_addr.end()) src = &it->second.fbo;
    }
    if (src == nullptr || src->fbo == 0) return false;
    gl::read_pixels(*src, rgba);
    w = src->w;
    h = src->h;
    return true;
}

bool grab_target(uint32_t addr, std::vector<uint8_t>& rgba, int& w, int& h) {
    auto it = targets_by_addr.find(addr & 0x00FFFFFF);
    if (it == targets_by_addr.end()) return false;
    gl::read_pixels(it->second.fbo, rgba);
    w = it->second.fbo.w;
    h = it->second.fbo.h;
    return true;
}

uint32_t presented_target() { return presented_cpu ? 0 : presented_addr; }
uint32_t last_cimg() { return rdp.cimg_addr; }

bool presented_pixel(int x, int y, uint8_t out[4]) {
    auto it = targets_by_addr.find(presented_addr);
    if (presented_cpu || it == targets_by_addr.end()) return false;
    const Target& t = it->second;
    const int s = target_scale();
    if (x < 0 || y < 0 || x >= (int)t.width || y >= (int)t.height) return false;
    gl::read_pixel(t.fbo, (int)std::lround((x + t.pad) * s) + s / 2, ((int)t.height - 1 - y) * s + s / 2, out);
    return true;
}

std::vector<TargetInfo> targets() {
    std::vector<TargetInfo> out;
    for (auto& [a, t] : targets_by_addr) out.push_back({ t.addr, t.width, t.height, t.siz, t.last_frame });
    return out;
}

void set_trace_file(FILE* f) { trace_file = f; }
void set_draw_limit(int32_t n) { draw_limit = n; }
void set_highlight(int32_t i) { highlight_index = i; }
void set_probe(int32_t i, const std::string& dir) {
    probe_index = i;
    probe_dir = dir;
    probe_done = false;
}
void set_wireframe(bool on) { gl::set_wireframe(on); }
const Stats& last_stats() { return last; }
const std::vector<CmdRecord>& last_commands() { return last_cmd_log; }
Options& options() { return opts; }

void dump_tmem(FILE* f) {
    for (int i = 0; i < 4096; i += 16) {
        fprintf(f, "%03X:", i);
        for (int j = 0; j < 16; j++) fprintf(f, " %02X", rdp.tmem[i + j]);
        fprintf(f, "\n");
    }
}

std::string describe_state() {
    char buf[4096];
    int n = snprintf(buf, sizeof(buf),
                     "frame %u  cmds %u  dls %u  verts %u  tris %u (culled %u)  rects %u  fills %u  draws %u  gl_draws %u\n"
                     "tex decodes %u  hits %u  cache %zu  programs %zu  targets %zu  switches %u  unknown %u  run %.2f ms\n"
                     "draw_limit %d  highlight %d  presented 0x%06X%s  gl %s\n",
                     last.frame, last.commands, last.dl_calls, last.vertices, last.triangles, last.triangles_culled,
                     last.rects, last.fills, last.draw_events, last.gl_draws, last.tex_decodes, last.tex_cache_hits,
                     tex_cache.size(), gl::program_count(), targets_by_addr.size(), last.target_switches,
                     last.unknown_commands, last.run_ms, draw_limit, highlight_index, presented_addr,
                     presented_cpu ? " (cpu fallback)" : "", gl::version_string());
    std::string s(buf, n);
    for (auto& [a, t] : targets_by_addr) {
        n = snprintf(buf, sizeof(buf), "  target 0x%06X %ux%u siz %u last frame %u\n", t.addr, t.width, t.height, t.siz,
                     t.last_frame);
        s.append(buf, n);
    }
    n = snprintf(buf, sizeof(buf), "  cimg 0x%06X w %u  zimg 0x%06X  othermode H %08X L %08X  combine %016llX\n",
                 rdp.cimg_addr, rdp.cimg_width, rdp.zimg_addr, rdp.other_mode_h, rdp.other_mode_l,
                 (unsigned long long)rdp.combine_mode);
    s.append(buf, n);
    for (int i = 0; i < 8; i++) {
        const Tile& t = rdp.tiles[i];
        n = snprintf(buf, sizeof(buf),
                     "  tile %d fmt %u siz %u line %u tmem 0x%03X pal %u cms %u masks %u shifts %u cmt %u maskt %u shiftt %u "
                     "size (%.2f,%.2f)-(%.2f,%.2f)\n",
                     i, t.fmt, t.siz, t.line, t.tmem, t.palette, t.cms, t.masks, t.shifts, t.cmt, t.maskt, t.shiftt,
                     t.uls / 4.0, t.ult / 4.0, t.lrs / 4.0, t.lrt / 4.0);
        s.append(buf, n);
    }
    return s;
}

} // namespace f3d
