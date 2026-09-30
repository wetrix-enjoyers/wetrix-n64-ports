// OpenGL / OpenGL ES 3.0 backend for the f3d renderer.
//
// The shader generator is Fast3D's (gfx_opengl.cpp in the Perfect Dark fork,
// MIT -- see LICENSE-fast3d.txt), trimmed to what this renderer drives and made
// to emit GLSL ES 3.00 or GLSL 3.30 core from the same text. Nothing here uses
// anything beyond GLES 3.0: no depth clamp, no mirror-clamp, no geometry or
// compute shaders, so what runs on the desktop ES context is what the R36S's
// Mali-G31 will run.

#include "f3d_gl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>

#include <SDL.h>

#include "glad/glad.h"

#include "gfx_cc.h"
#include "f3d_gbi.h"

namespace f3d::gl {

struct Program {
    GLuint id;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    GLint attrib_locations[16];
    uint8_t attrib_sizes[16];
    uint8_t num_attribs;
    GLint frame_count_location;
    GLint highlight_location;
    std::string vs_source, fs_source;
};

namespace {

std::map<std::pair<uint64_t, uint32_t>, Program> programs;
Program* current = nullptr;
GLuint vbo = 0;
GLuint vao = 0;
bool gles = false;
bool depth_mask = true;
bool highlight = false;
uint32_t frame_count = 0;
char glsl_version[16] = "330 core";
char version_str[256] = "";

void append(std::string& s, const char* str) { s += str; }
void line(std::string& s, const char* str) { s += str; s += '\n'; }

#define RAND_NOISE "((random(vec3(floor(gl_FragCoord.xy), float(frame_count))) + 1.0) / 2.0)"

const char* shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha,
                               bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0: return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_1: return with_alpha ? "vec4(1.0, 1.0, 1.0, 1.0)" : "vec3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1: return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2: return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3: return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4: return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_INPUT_5: return with_alpha || !inputs_have_alpha ? "vInput5" : "vInput5.rgb";
            case SHADER_INPUT_6: return with_alpha || !inputs_have_alpha ? "vInput6" : "vInput6.rgb";
            case SHADER_INPUT_7: return with_alpha || !inputs_have_alpha ? "vInput7" : "vInput7.rgb";
            case SHADER_TEXEL0: return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a" : (with_alpha ? "vec4(texVal0.a)" : "vec3(texVal0.a)");
            case SHADER_TEXEL1A:
                return hint_single_element ? "texVal1.a" : (with_alpha ? "vec4(texVal1.a)" : "vec3(texVal1.a)");
            case SHADER_TEXEL1: return with_alpha ? "texVal1" : "texVal1.rgb";
            case SHADER_COMBINED: return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE: return with_alpha ? "vec4(" RAND_NOISE ")" : "vec3(" RAND_NOISE ")";
        }
    } else {
        switch (item) {
            case SHADER_0: return "0.0";
            case SHADER_1: return "1.0";
            case SHADER_INPUT_1: return "vInput1.a";
            case SHADER_INPUT_2: return "vInput2.a";
            case SHADER_INPUT_3: return "vInput3.a";
            case SHADER_INPUT_4: return "vInput4.a";
            case SHADER_INPUT_5: return "vInput5.a";
            case SHADER_INPUT_6: return "vInput6.a";
            case SHADER_INPUT_7: return "vInput7.a";
            case SHADER_TEXEL0: return "texVal0.a";
            case SHADER_TEXEL0A: return "texVal0.a";
            case SHADER_TEXEL1A: return "texVal1.a";
            case SHADER_TEXEL1: return "texVal1.a";
            case SHADER_COMBINED: return "texel.a";
            case SHADER_NOISE: return RAND_NOISE;
        }
    }
    return "";
}

void append_formula(std::string& s, uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix,
                    bool with_alpha, bool only_alpha, bool opt_alpha) {
    auto item = [&](int k, bool single) {
        return shader_item_to_str(c[only_alpha][k], with_alpha, only_alpha, opt_alpha, single);
    };
    if (do_single) {
        append(s, item(3, false));
    } else if (do_multiply) {
        append(s, item(0, false)); append(s, " * "); append(s, item(2, true));
    } else if (do_mix) {
        append(s, "mix("); append(s, item(1, false)); append(s, ", "); append(s, item(0, false));
        append(s, ", "); append(s, item(2, true)); append(s, ")");
    } else {
        append(s, "("); append(s, item(0, false)); append(s, " - "); append(s, item(1, false));
        append(s, ") * "); append(s, item(2, true)); append(s, " + "); append(s, item(3, false));
    }
}

GLuint compile(GLenum type, const std::string& src) {
    GLuint sh = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        fprintf(stderr, "[f3d] shader compile failed:\n%s\n--- source ---\n%s\n", log, src.c_str());
        fflush(stderr);
    }
    return sh;
}

Program* create_program(uint64_t shader_id0, uint32_t shader_id1) {
    CCFeatures cc = {};
    gfx_cc_get_features(shader_id0, shader_id1, &cc);

    std::string vs, fs;
    char buf[256];
    size_t num_floats = 4;
    const char* st[2] = { "S", "T" };

    snprintf(buf, sizeof(buf), "#version %s\n", glsl_version);
    vs += buf;
    line(vs, "precision highp float;");
    line(vs, "in vec4 aVtxPos;");
    for (int i = 0; i < 2; i++) {
        if (!cc.used_textures[i]) continue;
        snprintf(buf, sizeof(buf), "in vec2 aTexCoord%d;\nout vec2 vTexCoord%d;\n", i, i);
        vs += buf;
        num_floats += 2;
        for (int j = 0; j < 2; j++) {
            if (cc.clamp[i][j]) {
                snprintf(buf, sizeof(buf), "in float aTexClamp%s%d;\nout float vTexClamp%s%d;\n", st[j], i, st[j], i);
                vs += buf;
                num_floats += 1;
            }
        }
    }
    if (cc.opt_fog) {
        line(vs, "in vec4 aFog;");
        line(vs, "out vec4 vFog;");
        num_floats += 4;
    }
    for (int i = 0; i < cc.num_inputs; i++) {
        snprintf(buf, sizeof(buf), "in vec%d aInput%d;\nout vec%d vInput%d;\n", cc.opt_alpha ? 4 : 3, i + 1,
                 cc.opt_alpha ? 4 : 3, i + 1);
        vs += buf;
        num_floats += cc.opt_alpha ? 4 : 3;
    }
    line(vs, "void main() {");
    for (int i = 0; i < 2; i++) {
        if (!cc.used_textures[i]) continue;
        snprintf(buf, sizeof(buf), "    vTexCoord%d = aTexCoord%d;\n", i, i);
        vs += buf;
        for (int j = 0; j < 2; j++) {
            if (cc.clamp[i][j]) {
                snprintf(buf, sizeof(buf), "    vTexClamp%s%d = aTexClamp%s%d;\n", st[j], i, st[j], i);
                vs += buf;
            }
        }
    }
    if (cc.opt_fog) line(vs, "    vFog = aFog;");
    for (int i = 0; i < cc.num_inputs; i++) {
        snprintf(buf, sizeof(buf), "    vInput%d = aInput%d;\n", i + 1, i + 1);
        vs += buf;
    }
    line(vs, "    gl_Position = aVtxPos;");
    line(vs, "}");

    snprintf(buf, sizeof(buf), "#version %s\n", glsl_version);
    fs += buf;
    line(fs, "precision highp float;");
    line(fs, "#define WRAP(x, low, high) mod((x)-(low), (high)-(low)) + (low)");
    for (int i = 0; i < 2; i++) {
        if (!cc.used_textures[i]) continue;
        snprintf(buf, sizeof(buf), "in vec2 vTexCoord%d;\n", i);
        fs += buf;
        for (int j = 0; j < 2; j++) {
            if (cc.clamp[i][j]) {
                snprintf(buf, sizeof(buf), "in float vTexClamp%s%d;\n", st[j], i);
                fs += buf;
            }
        }
    }
    if (cc.opt_fog) line(fs, "in vec4 vFog;");
    for (int i = 0; i < cc.num_inputs; i++) {
        snprintf(buf, sizeof(buf), "in vec%d vInput%d;\n", cc.opt_alpha ? 4 : 3, i + 1);
        fs += buf;
    }
    if (cc.used_textures[0]) line(fs, "uniform sampler2D uTex0;");
    if (cc.used_textures[1]) line(fs, "uniform sampler2D uTex1;");
    line(fs, "uniform int frame_count;");
    line(fs, "uniform int highlight;");
    line(fs, "out vec4 outColor;");
    line(fs, "float random(in vec3 value) {");
    line(fs, "    float random = dot(sin(value), vec3(12.9898, 78.233, 37.719));");
    line(fs, "    return fract(sin(random) * 143758.5453);");
    line(fs, "}");
    line(fs, "void main() {");
    for (int i = 0; i < 2; i++) {
        if (!cc.used_textures[i]) continue;
        bool s = cc.clamp[i][0], t = cc.clamp[i][1];
        snprintf(buf, sizeof(buf), "    vec2 texSize%d = vec2(textureSize(uTex%d, 0));\n", i, i);
        fs += buf;
        // The RDP clamps perspective-divided s/t to its S10.5 range; near a
        // horizon (w -> 0) that pins the texel instead of wrapping forever.
        snprintf(buf, sizeof(buf), "    vec2 tc%d = clamp(vTexCoord%d, vec2(-1024.0) / texSize%d, vec2(1023.97) / texSize%d);\n", i, i, i, i);
        fs += buf;
        if (!s && !t) {
            snprintf(buf, sizeof(buf), "    vec2 uv%d = tc%d;\n", i, i);
        } else if (s && t) {
            snprintf(buf, sizeof(buf),
                     "    vec2 uv%d = clamp(tc%d, 0.5 / texSize%d, vec2(vTexClampS%d, vTexClampT%d));\n",
                     i, i, i, i, i);
        } else if (s) {
            snprintf(buf, sizeof(buf),
                     "    vec2 uv%d = vec2(clamp(tc%d.s, 0.5 / texSize%d.s, vTexClampS%d), tc%d.t);\n",
                     i, i, i, i, i);
        } else {
            snprintf(buf, sizeof(buf),
                     "    vec2 uv%d = vec2(tc%d.s, clamp(tc%d.t, 0.5 / texSize%d.t, vTexClampT%d));\n",
                     i, i, i, i, i);
        }
        fs += buf;
        snprintf(buf, sizeof(buf), "    vec4 texVal%d = texture(uTex%d, uv%d);\n", i, i, i);
        fs += buf;
    }
    line(fs, cc.opt_alpha ? "    vec4 texel;" : "    vec3 texel;");
    for (int c = 0; c < (cc.opt_2cyc ? 2 : 1); c++) {
        append(fs, "    texel = ");
        if (!cc.color_alpha_same[c] && cc.opt_alpha) {
            append(fs, "vec4(");
            append_formula(fs, cc.c[c], cc.do_single[c][0], cc.do_multiply[c][0], cc.do_mix[c][0], false, false, true);
            append(fs, ", ");
            append_formula(fs, cc.c[c], cc.do_single[c][1], cc.do_multiply[c][1], cc.do_mix[c][1], true, true, true);
            append(fs, ")");
        } else {
            append_formula(fs, cc.c[c], cc.do_single[c][0], cc.do_multiply[c][0], cc.do_mix[c][0], cc.opt_alpha,
                           false, cc.opt_alpha);
        }
        line(fs, ";");
        if (c == 0) line(fs, "    texel = WRAP(texel, -1.01, 1.01);");
    }
    line(fs, "    texel = WRAP(texel, -0.51, 1.51);");
    line(fs, "    texel = clamp(texel, 0.0, 1.0);");
    if (cc.opt_fog) {
        if (cc.opt_alpha) line(fs, "    texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        else line(fs, "    texel = mix(texel, vFog.rgb, vFog.a);");
    }
    if (cc.opt_texture_edge && cc.opt_alpha) {
        line(fs, "    if (texel.a > 0.19) texel.a = 1.0; else discard;");
    }
    if (cc.opt_alpha && cc.opt_noise) {
        line(fs, "    texel.a *= floor(clamp(random(vec3(floor(gl_FragCoord.xy), float(frame_count))) + texel.a, 0.0, 1.0));");
    }
    if (cc.opt_alpha) {
        if (cc.opt_alpha_threshold) line(fs, "    if (texel.a < 8.0 / 256.0) discard;");
        if (cc.opt_invisible) line(fs, "    texel.a = 0.0;");
        line(fs, "    outColor = texel;");
    } else {
        line(fs, "    outColor = vec4(texel, 1.0);");
    }
    line(fs, "    if (highlight != 0) outColor = vec4(1.0, 0.0, 1.0, 1.0);");
    line(fs, "}");

    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "[f3d] program link failed: %s\n", log);
    }
    glDetachShader(prog, v);
    glDetachShader(prog, f);
    glDeleteShader(v);
    glDeleteShader(f);

    Program* prg = &programs[std::make_pair(shader_id0, shader_id1)];
    size_t cnt = 0;
    prg->attrib_locations[cnt] = glGetAttribLocation(prog, "aVtxPos");
    prg->attrib_sizes[cnt++] = 4;
    for (int i = 0; i < 2; i++) {
        if (!cc.used_textures[i]) continue;
        char name[32];
        snprintf(name, sizeof(name), "aTexCoord%d", i);
        prg->attrib_locations[cnt] = glGetAttribLocation(prog, name);
        prg->attrib_sizes[cnt++] = 2;
        for (int j = 0; j < 2; j++) {
            if (cc.clamp[i][j]) {
                snprintf(name, sizeof(name), "aTexClamp%s%d", st[j], i);
                prg->attrib_locations[cnt] = glGetAttribLocation(prog, name);
                prg->attrib_sizes[cnt++] = 1;
            }
        }
    }
    if (cc.opt_fog) {
        prg->attrib_locations[cnt] = glGetAttribLocation(prog, "aFog");
        prg->attrib_sizes[cnt++] = 4;
    }
    for (int i = 0; i < cc.num_inputs; i++) {
        char name[16];
        snprintf(name, sizeof(name), "aInput%d", i + 1);
        prg->attrib_locations[cnt] = glGetAttribLocation(prog, name);
        prg->attrib_sizes[cnt++] = cc.opt_alpha ? 4 : 3;
    }
    prg->id = prog;
    prg->num_inputs = cc.num_inputs;
    prg->used_textures[0] = cc.used_textures[0];
    prg->used_textures[1] = cc.used_textures[1];
    prg->num_floats = num_floats;
    prg->num_attribs = cnt;
    prg->vs_source = vs;
    prg->fs_source = fs;

    glUseProgram(prog);
    if (cc.used_textures[0]) glUniform1i(glGetUniformLocation(prog, "uTex0"), 0);
    if (cc.used_textures[1]) glUniform1i(glGetUniformLocation(prog, "uTex1"), 1);
    prg->frame_count_location = glGetUniformLocation(prog, "frame_count");
    prg->highlight_location = glGetUniformLocation(prog, "highlight");
    if (current != nullptr) glUseProgram(current->id);
    return prg;
}

void* load_proc(const char* name) { return SDL_GL_GetProcAddress(name); }

GLenum cm_to_gl(uint32_t val) {
    if (val & gbi::G_TX_CLAMP) return GL_CLAMP_TO_EDGE;
    return (val & gbi::G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

} // namespace

bool init(bool use_gles) {
    gles = use_gles;
    int ok = gles ? gladLoadGLES2Loader(load_proc) : gladLoadGLLoader(load_proc);
    if (!ok) {
        fprintf(stderr, "[f3d] could not load GL entry points (%s)\n", gles ? "GLES" : "GL");
        return false;
    }
    snprintf(glsl_version, sizeof(glsl_version), "%s", gles ? "300 es" : "330 core");
    snprintf(version_str, sizeof(version_str), "%s | %s | %s", (const char*)glGetString(GL_VERSION),
             (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VENDOR));
    fprintf(stderr, "[f3d] GL: %s (GLSL %s)\n", version_str, glsl_version);

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    return true;
}

bool is_gles() { return gles; }
const char* version_string() { return version_str; }

Program* get_program(uint64_t id0, uint32_t id1) {
    auto it = programs.find(std::make_pair(id0, id1));
    if (it != programs.end()) return &it->second;
    return create_program(id0, id1);
}

void use_program(Program* prg) {
    if (current != nullptr) {
        for (int i = 0; i < current->num_attribs; i++) {
            if (current->attrib_locations[i] >= 0) glDisableVertexAttribArray(current->attrib_locations[i]);
        }
    }
    current = prg;
    glUseProgram(prg->id);
    size_t pos = 0;
    for (int i = 0; i < prg->num_attribs; i++) {
        if (prg->attrib_locations[i] >= 0) {
            glEnableVertexAttribArray(prg->attrib_locations[i]);
            glVertexAttribPointer(prg->attrib_locations[i], prg->attrib_sizes[i], GL_FLOAT, GL_FALSE,
                                  prg->num_floats * sizeof(float), (void*)(pos * sizeof(float)));
        }
        pos += prg->attrib_sizes[i];
    }
    if (prg->frame_count_location >= 0) glUniform1i(prg->frame_count_location, frame_count);
    if (prg->highlight_location >= 0) glUniform1i(prg->highlight_location, highlight ? 1 : 0);
}

int program_num_inputs(const Program* prg) { return prg->num_inputs; }
bool program_uses_texture(const Program* prg, int i) { return prg->used_textures[i]; }

void set_highlight(bool on) {
    highlight = on;
    if (current != nullptr && current->highlight_location >= 0) glUniform1i(current->highlight_location, on ? 1 : 0);
}

void clear_programs() {
    glUseProgram(0);
    current = nullptr;
    for (auto& p : programs) glDeleteProgram(p.second.id);
    programs.clear();
}

size_t program_count() { return programs.size(); }

std::string program_source(const Program* prg) {
    return "// ---- vertex ----\n" + prg->vs_source + "// ---- fragment ----\n" + prg->fs_source;
}

uint32_t new_texture() {
    GLuint t;
    glGenTextures(1, &t);
    return t;
}

void delete_texture(uint32_t tex) {
    GLuint t = tex;
    glDeleteTextures(1, &t);
}

void bind_texture(int unit, uint32_t tex) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

void upload_texture(const uint8_t* rgba32, uint32_t width, uint32_t height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32);
}

void set_sampler(int unit, bool linear, uint32_t cms, uint32_t cmt) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, cm_to_gl(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, cm_to_gl(cmt));
}

void set_depth(bool test, bool update, bool compare, bool decal) {
    if (!test) {
        glDisable(GL_DEPTH_TEST);
        return;
    }
    glEnable(GL_DEPTH_TEST);
    glDepthMask(update ? GL_TRUE : GL_FALSE);
    depth_mask = update;
    glDepthFunc(compare ? GL_LEQUAL : GL_ALWAYS);
    if (decal && compare) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-2, -2);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

void set_viewport(int x, int y, int w, int h) { glViewport(x, y, w, h); }

void set_scissor(int x, int y, int w, int h) {
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w < 0 ? 0 : w, h < 0 ? 0 : h);
}

void set_blend(int mode) {
    if (mode == 2) {
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDisable(GL_BLEND);
        return;
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (mode == 1) glEnable(GL_BLEND);
    else glDisable(GL_BLEND);
}

void draw_triangles(const float* buf, size_t buf_floats, size_t num_tris) {
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * buf_floats, buf, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 3 * num_tris);
}

void set_wireframe(bool on) {
    if (!gles && glad_glPolygonMode != nullptr) glPolygonMode(GL_FRONT_AND_BACK, on ? GL_LINE : GL_FILL);
}

uint32_t create_depth_buffer(int w, int h) {
    GLuint rb;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    return rb;
}

void delete_depth_buffer(uint32_t rb) {
    GLuint r = rb;
    glDeleteRenderbuffers(1, &r);
}

Fbo create_fbo(int w, int h, uint32_t depth_rb) {
    Fbo f;
    f.w = w;
    f.h = h;
    glGenTextures(1, &f.color);
    glBindTexture(GL_TEXTURE_2D, f.color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &f.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, f.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f.color, 0);
    if (depth_rb != 0) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
    }
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[f3d] FBO %dx%d incomplete: 0x%x\n", w, h, status);
    }
    return f;
}

void delete_fbo(Fbo& f) {
    glDeleteFramebuffers(1, &f.fbo);
    glDeleteTextures(1, &f.color);
    f = Fbo{};
}

void bind_fbo(const Fbo* f, int window_w, int window_h) {
    if (f == nullptr) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, window_w, window_h);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
        glViewport(0, 0, f->w, f->h);
    }
}

void clear(bool color, bool depth, float r, float g, float b, float a) {
    glDisable(GL_SCISSOR_TEST);
    GLbitfield mask = 0;
    if (color) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(r, g, b, a);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (depth) {
        glDepthMask(GL_TRUE);
        if (gles) glClearDepthf(1.0f);
        else glClearDepth(1.0);
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    glClear(mask);
    if (depth) glDepthMask(depth_mask ? GL_TRUE : GL_FALSE);
}

void blit(const Fbo& src, int dx, int dy, int dw, int dh, bool linear) {
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src.fbo);
    glBlitFramebuffer(0, 0, src.w, src.h, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT,
                      linear ? GL_LINEAR : GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

void read_pixels(const Fbo& src, std::vector<uint8_t>& out) {
    std::vector<uint8_t> tmp((size_t)src.w * src.h * 4);
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_FRAMEBUFFER, src.fbo);
    glReadPixels(0, 0, src.w, src.h, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
    glBindFramebuffer(GL_FRAMEBUFFER, prev);
    out.resize(tmp.size());
    const size_t row = (size_t)src.w * 4;
    for (int y = 0; y < src.h; y++) {
        memcpy(&out[(size_t)y * row], &tmp[(size_t)(src.h - 1 - y) * row], row);
    }
}

void read_pixel(const Fbo& src, int x, int y, uint8_t out[4]) {
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_FRAMEBUFFER, src.fbo);
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    glBindFramebuffer(GL_FRAMEBUFFER, prev);
}

void finish_frame() { frame_count++; }

} // namespace f3d::gl
