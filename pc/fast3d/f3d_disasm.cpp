// F3DEX (GBI1) command disassembler: turns a w0/w1 pair into the gbi.h macro
// it came from, with decoded arguments. Used by the trace and the debug
// server's "dl" dump.

#include "f3d.h"
#include "f3d_gbi.h"

#include <cstdio>

namespace f3d {

namespace gbi {

const char* opcode_name(uint8_t op) {
    switch (op) {
        case G_SPNOOP: return "G_SPNOOP";
        case G_MTX: return "G_MTX";
        case G_MOVEMEM: return "G_MOVEMEM";
        case G_VTX: return "G_VTX";
        case G_DL: return "G_DL";
        case G_SPRITE2D_BASE: return "G_SPRITE2D_BASE";
        case G_LOAD_UCODE: return "G_LOAD_UCODE";
        case G_BRANCH_Z: return "G_BRANCH_Z";
        case G_TRI2: return "G_TRI2";
        case G_MODIFYVTX: return "G_MODIFYVTX";
        case G_RDPHALF_2: return "G_RDPHALF_2";
        case G_RDPHALF_1: return "G_RDPHALF_1";
        case G_LINE3D: return "G_QUAD";
        case G_CLEARGEOMETRYMODE: return "G_CLEARGEOMETRYMODE";
        case G_SETGEOMETRYMODE: return "G_SETGEOMETRYMODE";
        case G_ENDDL: return "G_ENDDL";
        case G_SETOTHERMODE_L: return "G_SETOTHERMODE_L";
        case G_SETOTHERMODE_H: return "G_SETOTHERMODE_H";
        case G_TEXTURE: return "G_TEXTURE";
        case G_MOVEWORD: return "G_MOVEWORD";
        case G_POPMTX: return "G_POPMTX";
        case G_CULLDL: return "G_CULLDL";
        case G_TRI1: return "G_TRI1";
        case G_NOOP: return "G_NOOP";
        case G_TEXRECT: return "G_TEXRECT";
        case G_TEXRECTFLIP: return "G_TEXRECTFLIP";
        case G_RDPLOADSYNC: return "G_RDPLOADSYNC";
        case G_RDPPIPESYNC: return "G_RDPPIPESYNC";
        case G_RDPTILESYNC: return "G_RDPTILESYNC";
        case G_RDPFULLSYNC: return "G_RDPFULLSYNC";
        case G_SETKEYGB: return "G_SETKEYGB";
        case G_SETKEYR: return "G_SETKEYR";
        case G_SETCONVERT: return "G_SETCONVERT";
        case G_SETSCISSOR: return "G_SETSCISSOR";
        case G_SETPRIMDEPTH: return "G_SETPRIMDEPTH";
        case G_RDPSETOTHERMODE: return "G_RDPSETOTHERMODE";
        case G_LOADTLUT: return "G_LOADTLUT";
        case G_SETTILESIZE: return "G_SETTILESIZE";
        case G_LOADBLOCK: return "G_LOADBLOCK";
        case G_LOADTILE: return "G_LOADTILE";
        case G_SETTILE: return "G_SETTILE";
        case G_FILLRECT: return "G_FILLRECT";
        case G_SETFILLCOLOR: return "G_SETFILLCOLOR";
        case G_SETFOGCOLOR: return "G_SETFOGCOLOR";
        case G_SETBLENDCOLOR: return "G_SETBLENDCOLOR";
        case G_SETPRIMCOLOR: return "G_SETPRIMCOLOR";
        case G_SETENVCOLOR: return "G_SETENVCOLOR";
        case G_SETCOMBINE: return "G_SETCOMBINE";
        case G_SETTIMG: return "G_SETTIMG";
        case G_SETZIMG: return "G_SETZIMG";
        case G_SETCIMG: return "G_SETCIMG";
    }
    if (op >= 0xC8 && op <= 0xCF) return "G_RDP_TRIANGLE";
    return "?";
}

} // namespace gbi

using namespace gbi;

namespace {

inline uint32_t b(uint32_t w, int pos, int width) { return (w >> pos) & ((1u << width) - 1); }

const char* fmt_name(uint32_t f) {
    static const char* n[] = { "RGBA", "YUV", "CI", "IA", "I", "?5", "?6", "?7" };
    return n[f & 7];
}

const char* siz_name(uint32_t s) {
    static const char* n[] = { "4b", "8b", "16b", "32b" };
    return n[s & 3];
}

std::string geom_flags(uint32_t m) {
    std::string s;
    auto add = [&](uint32_t bit, const char* name) {
        if (m & bit) {
            if (!s.empty()) s += '|';
            s += name;
        }
    };
    add(G_ZBUFFER, "ZBUFFER");
    add(G_SHADE, "SHADE");
    add(G_SHADING_SMOOTH, "SMOOTH");
    add(G_CULL_FRONT, "CULL_FRONT");
    add(G_CULL_BACK, "CULL_BACK");
    add(G_FOG, "FOG");
    add(G_LIGHTING, "LIGHTING");
    add(G_TEXTURE_GEN, "TEXGEN");
    add(G_TEXTURE_GEN_LINEAR, "TEXGEN_LINEAR");
    add(G_LOD, "LOD");
    add(G_CLIPPING, "CLIPPING");
    if (s.empty()) s = "0";
    return s;
}

} // namespace

std::string disasm(uint32_t w0, uint32_t w1) {
    char buf[256];
    const uint8_t op = w0 >> 24;
    const char* name = opcode_name(op);
    switch (op) {
        case G_MTX: {
            const uint32_t p = b(w0, 16, 8);
            snprintf(buf, sizeof(buf), "%s 0x%08X %s %s %s", name, w1, (p & G_MTX_PROJECTION) ? "PROJ" : "MODELVIEW",
                     (p & G_MTX_LOAD) ? "LOAD" : "MUL", (p & G_MTX_PUSH) ? "PUSH" : "NOPUSH");
            break;
        }
        case G_VTX:
            snprintf(buf, sizeof(buf), "%s 0x%08X n=%u v0=%u", name, w1, b(w0, 10, 6), b(w0, 16, 8) / 2);
            break;
        case G_DL:
            snprintf(buf, sizeof(buf), "%s 0x%08X %s", name, w1, b(w0, 16, 8) ? "BRANCH" : "CALL");
            break;
        case G_MOVEMEM: {
            const uint32_t idx = b(w0, 16, 8);
            const char* what = idx == G_MV_VIEWPORT ? "VIEWPORT"
                               : idx == G_MV_LOOKATX ? "LOOKATX"
                               : idx == G_MV_LOOKATY ? "LOOKATY"
                               : (idx >= G_MV_L0 && idx <= G_MV_L7) ? "LIGHT" : "?";
            if (idx >= G_MV_L0 && idx <= G_MV_L7) {
                snprintf(buf, sizeof(buf), "%s %s%u 0x%08X", name, what, (idx - G_MV_L0) / 2, w1);
            } else {
                snprintf(buf, sizeof(buf), "%s %s (0x%02X) 0x%08X size %u", name, what, idx, w1, b(w0, 0, 16));
            }
            break;
        }
        case G_MOVEWORD: {
            const uint32_t idx = b(w0, 0, 8), off = b(w0, 8, 16);
            if (idx == G_MW_SEGMENT) snprintf(buf, sizeof(buf), "%s SEGMENT %u = 0x%08X", name, off / 4, w1);
            else if (idx == G_MW_NUMLIGHT) snprintf(buf, sizeof(buf), "%s NUMLIGHT %d", name, (int)((w1 - 0x80000000u) / 32) - 1);
            else if (idx == G_MW_FOG) snprintf(buf, sizeof(buf), "%s FOG mul %d ofs %d", name, (int16_t)(w1 >> 16), (int16_t)w1);
            else snprintf(buf, sizeof(buf), "%s idx 0x%02X off 0x%04X = 0x%08X", name, idx, off, w1);
            break;
        }
        case G_TRI1:
            snprintf(buf, sizeof(buf), "%s %u %u %u", name, b(w1, 16, 8) / 2, b(w1, 8, 8) / 2, b(w1, 0, 8) / 2);
            break;
        case G_TRI2:
            snprintf(buf, sizeof(buf), "%s %u %u %u / %u %u %u", name, b(w0, 16, 8) / 2, b(w0, 8, 8) / 2, b(w0, 0, 8) / 2,
                     b(w1, 16, 8) / 2, b(w1, 8, 8) / 2, b(w1, 0, 8) / 2);
            break;
        case G_LINE3D:
            snprintf(buf, sizeof(buf), "%s %u %u %u %u", name, b(w1, 24, 8) / 2, b(w1, 16, 8) / 2, b(w1, 8, 8) / 2,
                     b(w1, 0, 8) / 2);
            break;
        case G_CULLDL:
            snprintf(buf, sizeof(buf), "%s %u..%u", name, b(w0, 0, 16) / 2, b(w1, 0, 16) / 2);
            break;
        case G_BRANCH_Z:
            snprintf(buf, sizeof(buf), "%s vtx %u z<=0x%08X", name, b(w0, 0, 12) / 2, w1);
            break;
        case G_SETGEOMETRYMODE:
        case G_CLEARGEOMETRYMODE:
            snprintf(buf, sizeof(buf), "%s %s", name, geom_flags(w1).c_str());
            break;
        case G_SETOTHERMODE_H:
        case G_SETOTHERMODE_L:
            snprintf(buf, sizeof(buf), "%s shift %u len %u = 0x%08X", name, b(w0, 8, 8), b(w0, 0, 8), w1);
            break;
        case G_TEXTURE:
            snprintf(buf, sizeof(buf), "%s s 0x%04X t 0x%04X level %u tile %u %s", name, b(w1, 16, 16), b(w1, 0, 16),
                     b(w0, 11, 3), b(w0, 8, 3), b(w0, 0, 8) ? "ON" : "OFF");
            break;
        case G_SETTIMG:
        case G_SETCIMG:
            snprintf(buf, sizeof(buf), "%s %s %s w %u 0x%08X", name, fmt_name(b(w0, 21, 3)), siz_name(b(w0, 19, 2)),
                     b(w0, 0, 12) + 1, w1);
            break;
        case G_SETZIMG:
            snprintf(buf, sizeof(buf), "%s 0x%08X", name, w1);
            break;
        case G_SETTILE:
            snprintf(buf, sizeof(buf), "%s tile %u %s %s line %u tmem 0x%03X pal %u cmt %u mt %u st %u cms %u ms %u ss %u",
                     name, b(w1, 24, 3), fmt_name(b(w0, 21, 3)), siz_name(b(w0, 19, 2)), b(w0, 9, 9), b(w0, 0, 9),
                     b(w1, 20, 4), b(w1, 18, 2), b(w1, 14, 4), b(w1, 10, 4), b(w1, 8, 2), b(w1, 4, 4), b(w1, 0, 4));
            break;
        case G_SETTILESIZE:
        case G_LOADTILE:
        case G_LOADTLUT:
            snprintf(buf, sizeof(buf), "%s tile %u (%.2f,%.2f)-(%.2f,%.2f)", name, b(w1, 24, 3), b(w0, 12, 12) / 4.0,
                     b(w0, 0, 12) / 4.0, b(w1, 12, 12) / 4.0, b(w1, 0, 12) / 4.0);
            break;
        case G_LOADBLOCK:
            snprintf(buf, sizeof(buf), "%s tile %u uls %u ult %u texels %u dxt %u", name, b(w1, 24, 3), b(w0, 12, 12),
                     b(w0, 0, 12), b(w1, 12, 12) + 1, b(w1, 0, 12));
            break;
        case G_FILLRECT:
            snprintf(buf, sizeof(buf), "%s (%.2f,%.2f)-(%.2f,%.2f)", name, b(w1, 12, 12) / 4.0, b(w1, 0, 12) / 4.0,
                     b(w0, 12, 12) / 4.0, b(w0, 0, 12) / 4.0);
            break;
        case G_TEXRECT:
        case G_TEXRECTFLIP:
            snprintf(buf, sizeof(buf), "%s tile %u (%.2f,%.2f)-(%.2f,%.2f)", name, b(w1, 24, 3), b(w1, 12, 12) / 4.0,
                     b(w1, 0, 12) / 4.0, b(w0, 12, 12) / 4.0, b(w0, 0, 12) / 4.0);
            break;
        case G_SETSCISSOR:
            snprintf(buf, sizeof(buf), "%s (%.2f,%.2f)-(%.2f,%.2f)", name, b(w0, 12, 12) / 4.0, b(w0, 0, 12) / 4.0,
                     b(w1, 12, 12) / 4.0, b(w1, 0, 12) / 4.0);
            break;
        case G_SETPRIMCOLOR:
            snprintf(buf, sizeof(buf), "%s min %u frac %u rgba %02X %02X %02X %02X", name, b(w0, 8, 8), b(w0, 0, 8),
                     w1 >> 24, (w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF);
            break;
        case G_SETENVCOLOR:
        case G_SETFOGCOLOR:
        case G_SETBLENDCOLOR:
            snprintf(buf, sizeof(buf), "%s rgba %02X %02X %02X %02X", name, w1 >> 24, (w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF,
                     w1 & 0xFF);
            break;
        case G_SETFILLCOLOR:
            snprintf(buf, sizeof(buf), "%s 0x%08X", name, w1);
            break;
        case G_SETCOMBINE:
            snprintf(buf, sizeof(buf),
                     "%s c1 (%u-%u)*%u+%u a1 (%u-%u)*%u+%u | c2 (%u-%u)*%u+%u a2 (%u-%u)*%u+%u", name,
                     b(w0, 20, 4), b(w1, 28, 4), b(w0, 15, 5), b(w1, 15, 3), b(w0, 12, 3), b(w1, 12, 3), b(w0, 9, 3),
                     b(w1, 9, 3), b(w0, 5, 4), b(w1, 24, 4), b(w0, 0, 5), b(w1, 6, 3), b(w1, 21, 3), b(w1, 3, 3),
                     b(w1, 18, 3), b(w1, 0, 3));
            break;
        case G_RDPSETOTHERMODE:
            snprintf(buf, sizeof(buf), "%s H 0x%06X L 0x%08X", name, w0 & 0xFFFFFF, w1);
            break;
        case G_RDPHALF_1:
        case G_RDPHALF_2:
            snprintf(buf, sizeof(buf), "%s 0x%08X", name, w1);
            break;
        default:
            snprintf(buf, sizeof(buf), "%s", name);
            break;
    }
    return buf;
}

} // namespace f3d
