// GBI constants for the microcode Wetrix actually runs: "RSP Gfx ucode
// F3DEX.NoN 1.21 Yoshitaka Yasumoto Nintendo." (string at the end of the ROM's
// gfx ucode data). F3DEX is a GBI1 microcode, so the opcode numbering is the
// original Fast3D one plus F3DEX's additions (TRI2, MODIFYVTX, BRANCH_Z,
// LOAD_UCODE), with vertex indices encoded x2 rather than x10.
//
// Only what the interpreter and the disassembler use is here; the values are
// the ones in libultra's gbi.h for F3DEX_GBI.
#pragma once

#include <cstdint>

namespace f3d::gbi {

// RSP (DMA) commands
constexpr uint8_t G_SPNOOP = 0x00;
constexpr uint8_t G_MTX = 0x01;
constexpr uint8_t G_MOVEMEM = 0x03;
constexpr uint8_t G_VTX = 0x04;
constexpr uint8_t G_DL = 0x06;
constexpr uint8_t G_SPRITE2D_BASE = 0x09;

// RSP immediate commands
constexpr uint8_t G_LOAD_UCODE = 0xAF;
constexpr uint8_t G_BRANCH_Z = 0xB0;
constexpr uint8_t G_TRI2 = 0xB1;
constexpr uint8_t G_MODIFYVTX = 0xB2;
constexpr uint8_t G_RDPHALF_2 = 0xB3;
constexpr uint8_t G_RDPHALF_1 = 0xB4;
constexpr uint8_t G_LINE3D = 0xB5;
constexpr uint8_t G_CLEARGEOMETRYMODE = 0xB6;
constexpr uint8_t G_SETGEOMETRYMODE = 0xB7;
constexpr uint8_t G_ENDDL = 0xB8;
constexpr uint8_t G_SETOTHERMODE_L = 0xB9;
constexpr uint8_t G_SETOTHERMODE_H = 0xBA;
constexpr uint8_t G_TEXTURE = 0xBB;
constexpr uint8_t G_MOVEWORD = 0xBC;
constexpr uint8_t G_POPMTX = 0xBD;
constexpr uint8_t G_CULLDL = 0xBE;
constexpr uint8_t G_TRI1 = 0xBF;

// RDP commands
constexpr uint8_t G_NOOP = 0xC0;
constexpr uint8_t G_TRI_FILL = 0xC8;  // 0xC8..0xCF: raw RDP triangles
constexpr uint8_t G_TEXRECT = 0xE4;
constexpr uint8_t G_TEXRECTFLIP = 0xE5;
constexpr uint8_t G_RDPLOADSYNC = 0xE6;
constexpr uint8_t G_RDPPIPESYNC = 0xE7;
constexpr uint8_t G_RDPTILESYNC = 0xE8;
constexpr uint8_t G_RDPFULLSYNC = 0xE9;
constexpr uint8_t G_SETKEYGB = 0xEA;
constexpr uint8_t G_SETKEYR = 0xEB;
constexpr uint8_t G_SETCONVERT = 0xEC;
constexpr uint8_t G_SETSCISSOR = 0xED;
constexpr uint8_t G_SETPRIMDEPTH = 0xEE;
constexpr uint8_t G_RDPSETOTHERMODE = 0xEF;
constexpr uint8_t G_LOADTLUT = 0xF0;
constexpr uint8_t G_SETTILESIZE = 0xF2;
constexpr uint8_t G_LOADBLOCK = 0xF3;
constexpr uint8_t G_LOADTILE = 0xF4;
constexpr uint8_t G_SETTILE = 0xF5;
constexpr uint8_t G_FILLRECT = 0xF6;
constexpr uint8_t G_SETFILLCOLOR = 0xF7;
constexpr uint8_t G_SETFOGCOLOR = 0xF8;
constexpr uint8_t G_SETBLENDCOLOR = 0xF9;
constexpr uint8_t G_SETPRIMCOLOR = 0xFA;
constexpr uint8_t G_SETENVCOLOR = 0xFB;
constexpr uint8_t G_SETCOMBINE = 0xFC;
constexpr uint8_t G_SETTIMG = 0xFD;
constexpr uint8_t G_SETZIMG = 0xFE;
constexpr uint8_t G_SETCIMG = 0xFF;

// G_MTX parameters (GBI1)
constexpr uint32_t G_MTX_PROJECTION = 0x01;
constexpr uint32_t G_MTX_LOAD = 0x02;
constexpr uint32_t G_MTX_PUSH = 0x04;

// G_MOVEMEM indices (GBI1)
constexpr uint8_t G_MV_VIEWPORT = 0x80;
constexpr uint8_t G_MV_LOOKATY = 0x82;
constexpr uint8_t G_MV_LOOKATX = 0x84;
constexpr uint8_t G_MV_L0 = 0x86;
constexpr uint8_t G_MV_L7 = 0x94;
constexpr uint8_t G_MV_TXTATT = 0x96;
constexpr uint8_t G_MV_MATRIX_1 = 0x9E;
constexpr uint8_t G_MV_MATRIX_4 = 0x98;

// G_MOVEWORD indices
constexpr uint8_t G_MW_MATRIX = 0x00;
constexpr uint8_t G_MW_NUMLIGHT = 0x02;
constexpr uint8_t G_MW_CLIP = 0x04;
constexpr uint8_t G_MW_SEGMENT = 0x06;
constexpr uint8_t G_MW_FOG = 0x08;
constexpr uint8_t G_MW_LIGHTCOL = 0x0A;
constexpr uint8_t G_MW_POINTS = 0x0C;
constexpr uint8_t G_MW_PERSPNORM = 0x0E;

// G_MODIFYVTX "where"
constexpr uint32_t G_MWO_POINT_RGBA = 0x10;
constexpr uint32_t G_MWO_POINT_ST = 0x14;
constexpr uint32_t G_MWO_POINT_XYSCREEN = 0x18;
constexpr uint32_t G_MWO_POINT_ZSCREEN = 0x1C;

// Geometry mode (GBI1)
constexpr uint32_t G_ZBUFFER = 0x00000001;
constexpr uint32_t G_SHADE = 0x00000004;
constexpr uint32_t G_SHADING_SMOOTH = 0x00000200;
constexpr uint32_t G_CULL_FRONT = 0x00001000;
constexpr uint32_t G_CULL_BACK = 0x00002000;
constexpr uint32_t G_CULL_BOTH = 0x00003000;
constexpr uint32_t G_FOG = 0x00010000;
constexpr uint32_t G_LIGHTING = 0x00020000;
constexpr uint32_t G_TEXTURE_GEN = 0x00040000;
constexpr uint32_t G_TEXTURE_GEN_LINEAR = 0x00080000;
constexpr uint32_t G_LOD = 0x00100000;
constexpr uint32_t G_CLIPPING = 0x00800000;

// Other mode H shifts
constexpr int G_MDSFT_ALPHADITHER = 4;
constexpr int G_MDSFT_RGBDITHER = 6;
constexpr int G_MDSFT_COMBKEY = 8;
constexpr int G_MDSFT_TEXTCONV = 9;
constexpr int G_MDSFT_TEXTFILT = 12;
constexpr int G_MDSFT_TEXTLUT = 14;
constexpr int G_MDSFT_TEXTLOD = 16;
constexpr int G_MDSFT_TEXTDETAIL = 17;
constexpr int G_MDSFT_TEXTPERSP = 19;
constexpr int G_MDSFT_CYCLETYPE = 20;
constexpr int G_MDSFT_PIPELINE = 23;

// Other mode L shifts
constexpr int G_MDSFT_ALPHACOMPARE = 0;
constexpr int G_MDSFT_ZSRCSEL = 2;
constexpr int G_MDSFT_RENDERMODE = 3;

constexpr uint32_t G_CYC_1CYCLE = 0u << G_MDSFT_CYCLETYPE;
constexpr uint32_t G_CYC_2CYCLE = 1u << G_MDSFT_CYCLETYPE;
constexpr uint32_t G_CYC_COPY = 2u << G_MDSFT_CYCLETYPE;
constexpr uint32_t G_CYC_FILL = 3u << G_MDSFT_CYCLETYPE;

constexpr uint32_t G_TP_PERSP = 1u << G_MDSFT_TEXTPERSP;
constexpr uint32_t G_TL_LOD = 1u << G_MDSFT_TEXTLOD;
constexpr uint32_t G_TD_DETAIL = 2u << G_MDSFT_TEXTDETAIL;
constexpr uint32_t G_TF_POINT = 0u << G_MDSFT_TEXTFILT;
constexpr uint32_t G_TF_AVERAGE = 3u << G_MDSFT_TEXTFILT;
constexpr uint32_t G_TF_BILERP = 2u << G_MDSFT_TEXTFILT;
constexpr uint32_t G_TT_NONE = 0u << G_MDSFT_TEXTLUT;
constexpr uint32_t G_TT_RGBA16 = 2u << G_MDSFT_TEXTLUT;
constexpr uint32_t G_TT_IA16 = 3u << G_MDSFT_TEXTLUT;

constexpr uint32_t G_AC_NONE = 0;
constexpr uint32_t G_AC_THRESHOLD = 1;
constexpr uint32_t G_AC_DITHER = 3;
constexpr uint32_t G_ZS_PRIM = 1u << G_MDSFT_ZSRCSEL;

// Render mode flag bits (low part of other mode L)
constexpr uint32_t AA_EN = 0x0008;
constexpr uint32_t Z_CMP = 0x0010;
constexpr uint32_t Z_UPD = 0x0020;
constexpr uint32_t IM_RD = 0x0040;
constexpr uint32_t CLR_ON_CVG = 0x0080;
constexpr uint32_t CVG_DST_WRAP = 0x0100;
constexpr uint32_t CVG_DST_FULL = 0x0200;
constexpr uint32_t CVG_DST_SAVE = 0x0300;
constexpr uint32_t ZMODE_OPA = 0x0000;
constexpr uint32_t ZMODE_INTER = 0x0400;
constexpr uint32_t ZMODE_XLU = 0x0800;
constexpr uint32_t ZMODE_DEC = 0x0C00;
constexpr uint32_t CVG_X_ALPHA = 0x1000;
constexpr uint32_t ALPHA_CVG_SEL = 0x2000;
constexpr uint32_t FORCE_BL = 0x4000;

// Blender mux values
constexpr uint32_t G_BL_CLR_IN = 0;
constexpr uint32_t G_BL_CLR_MEM = 1;
constexpr uint32_t G_BL_CLR_BL = 2;
constexpr uint32_t G_BL_CLR_FOG = 3;
constexpr uint32_t G_BL_A_IN = 0;
constexpr uint32_t G_BL_A_FOG = 1;
constexpr uint32_t G_BL_A_SHADE = 2;
constexpr uint32_t G_BL_1MA = 0;
constexpr uint32_t G_BL_A_MEM = 1;
constexpr uint32_t G_BL_1 = 2;
constexpr uint32_t G_BL_0 = 3;

// Image formats and sizes
constexpr uint8_t G_IM_FMT_RGBA = 0;
constexpr uint8_t G_IM_FMT_YUV = 1;
constexpr uint8_t G_IM_FMT_CI = 2;
constexpr uint8_t G_IM_FMT_IA = 3;
constexpr uint8_t G_IM_FMT_I = 4;
constexpr uint8_t G_IM_SIZ_4b = 0;
constexpr uint8_t G_IM_SIZ_8b = 1;
constexpr uint8_t G_IM_SIZ_16b = 2;
constexpr uint8_t G_IM_SIZ_32b = 3;

// Tile clamp/mirror bits
constexpr uint32_t G_TX_MIRROR = 0x1;
constexpr uint32_t G_TX_CLAMP = 0x2;

// Color combiner inputs
enum : uint32_t {
    G_CCMUX_COMBINED = 0,
    G_CCMUX_TEXEL0 = 1,
    G_CCMUX_TEXEL1 = 2,
    G_CCMUX_PRIMITIVE = 3,
    G_CCMUX_SHADE = 4,
    G_CCMUX_ENVIRONMENT = 5,
    G_CCMUX_CENTER = 6,
    G_CCMUX_SCALE = 6,
    G_CCMUX_COMBINED_ALPHA = 7,
    G_CCMUX_TEXEL0_ALPHA = 8,
    G_CCMUX_TEXEL1_ALPHA = 9,
    G_CCMUX_PRIMITIVE_ALPHA = 10,
    G_CCMUX_SHADE_ALPHA = 11,
    G_CCMUX_ENV_ALPHA = 12,
    G_CCMUX_LOD_FRACTION = 13,
    G_CCMUX_PRIM_LOD_FRAC = 14,
    G_CCMUX_NOISE = 7,
    G_CCMUX_K4 = 7,
    G_CCMUX_K5 = 15,
    G_CCMUX_1 = 6,
    G_CCMUX_0 = 31,
};

enum : uint32_t {
    G_ACMUX_COMBINED = 0,
    G_ACMUX_TEXEL0 = 1,
    G_ACMUX_TEXEL1 = 2,
    G_ACMUX_PRIMITIVE = 3,
    G_ACMUX_SHADE = 4,
    G_ACMUX_ENVIRONMENT = 5,
    G_ACMUX_LOD_FRACTION = 0,
    G_ACMUX_PRIM_LOD_FRAC = 6,
    G_ACMUX_1 = 6,
    G_ACMUX_0 = 7,
};

const char* opcode_name(uint8_t op);

} // namespace f3d::gbi
