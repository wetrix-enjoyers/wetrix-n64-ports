// OpenGL / OpenGL ES 3.0 backend for the f3d renderer. Internal to fast3d/.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace f3d::gl {

struct Program;

bool init(bool gles);
bool is_gles();
const char* version_string();

// Colour-combiner programs, keyed exactly as Fast3D keys them.
Program* get_program(uint64_t shader_id0, uint32_t shader_id1);
void use_program(Program* prg);
int program_num_inputs(const Program* prg);
bool program_uses_texture(const Program* prg, int i);
void set_highlight(bool on);   // uniform that forces magenta output
void clear_programs();
size_t program_count();
std::string program_source(const Program* prg);

uint32_t new_texture();
void delete_texture(uint32_t tex);
void bind_texture(int unit, uint32_t tex);
void upload_texture(const uint8_t* rgba32, uint32_t width, uint32_t height);
void set_sampler(int unit, bool linear, uint32_t cms, uint32_t cmt);

void set_depth(bool test, bool update, bool compare, bool decal);
void set_viewport(int x, int y, int w, int h);
void set_scissor(int x, int y, int w, int h);
// 0 = opaque, 1 = src-alpha blend, 2 = invisible (colour writes off)
void set_blend(int mode);
void draw_triangles(const float* buf, size_t buf_floats, size_t num_tris);
void set_wireframe(bool on);

// Render targets.
uint32_t create_depth_buffer(int w, int h);
void delete_depth_buffer(uint32_t rb);
struct Fbo {
    uint32_t fbo = 0, color = 0;
    int w = 0, h = 0;
};
Fbo create_fbo(int w, int h, uint32_t depth_rb);
void delete_fbo(Fbo& f);
void bind_fbo(const Fbo* f, int window_w = 0, int window_h = 0);  // nullptr = default framebuffer
void clear(bool color, bool depth, float r = 0, float g = 0, float b = 0, float a = 1);
void blit(const Fbo& src, int dx, int dy, int dw, int dh, bool linear);
void read_pixels(const Fbo& src, std::vector<uint8_t>& rgba_top_down);
void read_pixel(const Fbo& src, int x, int y_bottom_up, uint8_t out[4]);
void finish_frame();

} // namespace f3d::gl
