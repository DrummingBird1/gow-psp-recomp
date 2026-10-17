/* Internal GE state shared by the list interpreter (rt_ge.c) and the renderer (rt_gesoft.c). */
#pragma once
#include <stdint.h>

#include "psp_rt.h"
#include "rt_ge.h"

enum {
    GE_CMD_NOP = 0x00, GE_CMD_VADDR = 0x01, GE_CMD_IADDR = 0x02, GE_CMD_PRIM = 0x04, GE_CMD_BEZIER = 0x05,
    GE_CMD_SPLINE = 0x06, GE_CMD_BOUNDINGBOX = 0x07, GE_CMD_JUMP = 0x08, GE_CMD_BJUMP = 0x09, GE_CMD_CALL = 0x0A,
    GE_CMD_RET = 0x0B, GE_CMD_END = 0x0C, GE_CMD_SIGNAL = 0x0E, GE_CMD_FINISH = 0x0F, GE_CMD_BASE = 0x10,
    GE_CMD_VERTEXTYPE = 0x12, GE_CMD_OFFSETADDR = 0x13, GE_CMD_ORIGIN = 0x14, GE_CMD_REGION1 = 0x15, GE_CMD_REGION2 = 0x16,
    GE_CMD_LIGHTINGENABLE = 0x17, GE_CMD_LIGHTENABLE0 = 0x18, GE_CMD_DEPTHCLAMPENABLE = 0x1C, GE_CMD_CULLFACEENABLE = 0x1D,
    GE_CMD_TEXTUREMAPENABLE = 0x1E, GE_CMD_FOGENABLE = 0x1F, GE_CMD_DITHERENABLE = 0x20, GE_CMD_ALPHABLENDENABLE = 0x21,
    GE_CMD_ALPHATESTENABLE = 0x22, GE_CMD_ZTESTENABLE = 0x23, GE_CMD_STENCILTESTENABLE = 0x24, GE_CMD_ANTIALIASENABLE = 0x25,
    GE_CMD_PATCHCULLENABLE = 0x26, GE_CMD_COLORTESTENABLE = 0x27, GE_CMD_LOGICOPENABLE = 0x28,
    GE_CMD_BONEMATRIXNUMBER = 0x2A, GE_CMD_BONEMATRIXDATA = 0x2B, GE_CMD_MORPHWEIGHT0 = 0x2C,
    GE_CMD_PATCHDIVISION = 0x36, GE_CMD_PATCHPRIMITIVE = 0x37, GE_CMD_PATCHFACING = 0x38,
    GE_CMD_WORLDMATRIXNUMBER = 0x3A, GE_CMD_WORLDMATRIXDATA = 0x3B, GE_CMD_VIEWMATRIXNUMBER = 0x3C,
    GE_CMD_VIEWMATRIXDATA = 0x3D, GE_CMD_PROJMATRIXNUMBER = 0x3E, GE_CMD_PROJMATRIXDATA = 0x3F,
    GE_CMD_TGENMATRIXNUMBER = 0x40, GE_CMD_TGENMATRIXDATA = 0x41,
    GE_CMD_VIEWPORTXSCALE = 0x42, GE_CMD_VIEWPORTYSCALE = 0x43, GE_CMD_VIEWPORTZSCALE = 0x44,
    GE_CMD_VIEWPORTXCENTER = 0x45, GE_CMD_VIEWPORTYCENTER = 0x46, GE_CMD_VIEWPORTZCENTER = 0x47,
    GE_CMD_TEXSCALEU = 0x48, GE_CMD_TEXSCALEV = 0x49, GE_CMD_TEXOFFSETU = 0x4A, GE_CMD_TEXOFFSETV = 0x4B,
    GE_CMD_OFFSETX = 0x4C, GE_CMD_OFFSETY = 0x4D, GE_CMD_SHADEMODE = 0x50, GE_CMD_REVERSENORMAL = 0x51,
    GE_CMD_MATERIALUPDATE = 0x53, GE_CMD_MATERIALEMISSIVE = 0x54, GE_CMD_MATERIALAMBIENT = 0x55,
    GE_CMD_MATERIALDIFFUSE = 0x56, GE_CMD_MATERIALSPECULAR = 0x57, GE_CMD_MATERIALALPHA = 0x58,
    GE_CMD_MATERIALSPECULARCOEF = 0x5B, GE_CMD_AMBIENTCOLOR = 0x5C, GE_CMD_AMBIENTALPHA = 0x5D, GE_CMD_LIGHTMODE = 0x5E,
    GE_CMD_LIGHTTYPE0 = 0x5F, GE_CMD_LX0 = 0x63, GE_CMD_LDX0 = 0x6F, GE_CMD_LKA0 = 0x7B, GE_CMD_LKS0 = 0x87, GE_CMD_LKO0 = 0x8B,
    GE_CMD_LAC0 = 0x8F, GE_CMD_CULL = 0x9B, GE_CMD_FRAMEBUFPTR = 0x9C, GE_CMD_FRAMEBUFWIDTH = 0x9D,
    GE_CMD_ZBUFPTR = 0x9E, GE_CMD_ZBUFWIDTH = 0x9F, GE_CMD_TEXADDR0 = 0xA0, GE_CMD_TEXBUFWIDTH0 = 0xA8,
    GE_CMD_CLUTADDR = 0xB0, GE_CMD_CLUTADDRUPPER = 0xB1, GE_CMD_TRANSFERSRC = 0xB2, GE_CMD_TRANSFERSRCW = 0xB3,
    GE_CMD_TRANSFERDST = 0xB4, GE_CMD_TRANSFERDSTW = 0xB5, GE_CMD_TEXSIZE0 = 0xB8, GE_CMD_TEXMAPMODE = 0xC0,
    GE_CMD_TEXSHADELS = 0xC1, GE_CMD_TEXMODE = 0xC2, GE_CMD_TEXFORMAT = 0xC3, GE_CMD_LOADCLUT = 0xC4,
    GE_CMD_CLUTFORMAT = 0xC5, GE_CMD_TEXFILTER = 0xC6, GE_CMD_TEXWRAP = 0xC7, GE_CMD_TEXLEVEL = 0xC8,
    GE_CMD_TEXFUNC = 0xC9, GE_CMD_TEXENVCOLOR = 0xCA, GE_CMD_TEXFLUSH = 0xCB, GE_CMD_TEXSYNC = 0xCC,
    GE_CMD_FOG1 = 0xCD, GE_CMD_FOG2 = 0xCE, GE_CMD_FOGCOLOR = 0xCF, GE_CMD_TEXLODSLOPE = 0xD0,
    GE_CMD_FRAMEBUFPIXFORMAT = 0xD2, GE_CMD_CLEARMODE = 0xD3, GE_CMD_SCISSOR1 = 0xD4, GE_CMD_SCISSOR2 = 0xD5,
    GE_CMD_MINZ = 0xD6, GE_CMD_MAXZ = 0xD7, GE_CMD_COLORTEST = 0xD8, GE_CMD_COLORREF = 0xD9, GE_CMD_COLORTESTMASK = 0xDA,
    GE_CMD_ALPHATEST = 0xDB, GE_CMD_STENCILTEST = 0xDC, GE_CMD_STENCILOP = 0xDD, GE_CMD_ZTEST = 0xDE,
    GE_CMD_BLENDMODE = 0xDF, GE_CMD_BLENDFIXEDA = 0xE0, GE_CMD_BLENDFIXEDB = 0xE1, GE_CMD_LOGICOP = 0xE6,
    GE_CMD_ZWRITEDISABLE = 0xE7, GE_CMD_MASKRGB = 0xE8, GE_CMD_MASKALPHA = 0xE9, GE_CMD_TRANSFERSTART = 0xEA,
    GE_CMD_TRANSFERSRCPOS = 0xEB, GE_CMD_TRANSFERDSTPOS = 0xEC, GE_CMD_TRANSFERSIZE = 0xEE,
};

typedef struct {
    uint32_t cmd[256];   /* last word written for every command (like PPSSPP's gstate.cmdmem) */
    float world[12], view[12], proj[16], tgen[12], bone[96];
    uint32_t world_n, view_n, proj_n, tgen_n, bone_n;
    float morph[8];
    uint32_t offset, vaddr, iaddr;
    int bbox_false;
    uint8_t clut[1024];
} Ge;

/* why the renderer's queue was drawn (ge_flush) */
enum { FLUSH_LIST_END, FLUSH_TEXTURE, FLUSH_TARGET, FLUSH_TRANSFER, FLUSH_CLUT, FLUSH_TRACE, FLUSH_REASONS };
#define GE_STAT_SLOTS 48
typedef struct { uint32_t key, count; } GeCount;
typedef struct {
    uint32_t lists, prims, through, clears, lit, skinned, morphed, patches, transfers, stalls, runaway, signals;
    uint64_t vertices, verts_xformed, tris_raster, px_shaded, px_written, flushes, jobs, setups_reused;
    uint64_t flush_why[FLUSH_REASONS];
    uint32_t prim_type[8];
    uint32_t cmds[256];
    GeCount vtypes[GE_STAT_SLOTS], texfmts[GE_STAT_SLOTS], blends[GE_STAT_SLOTS], fbfmts[GE_STAT_SLOTS];
} GeStats;

extern Ge ge;
extern GeStats ge_stats;
/* set when a command since the last PRIM may have changed state that ge_draw_prim derives
 * once per draw (transform, lighting and raster setup); otherwise that setup is reused */
extern int ge_setup_dirty;

uint32_t ge_rel(uint32_t data);

/* rt_gesoft.c */
void ge_draw_prim(uint32_t op);
int ge_bbox(uint32_t op);
void ge_block_transfer(uint32_t op);
void ge_flush(int why);       /* draws everything queued */
void ge_sync_read(uint32_t addr, uint32_t size, int why);  /* flushes if the queue writes that memory */
void ge_soft_report(void);
void ge_clut_changed(void);   /* LOADCLUT replaced ge.clut */
