/* Hardware renderer on SDL3 GPU (Vulkan / Direct3D 12).
 *
 * rt_gesoft.c decodes, transforms, lights, clips and assembles the primitives exactly as for
 * the software renderer and hands finished triangles (PSP screen space) to rt_gpu_draw. Work
 * is recorded during the frame and submitted at the flip: one copy pass (vertices, new
 * textures), then the recorded commands in order (render passes, and copy passes for
 * render-to-texture snapshots and memory uploads), then a blit of the displayed framebuffer
 * to the window.
 *
 * Framebuffers: a GPU render target (RGBA8) stands for a VRAM area of a given pitch and pixel
 * size. A draw to an address inside an existing target (the games draw into parts of a scratch
 * buffer by moving the framebuffer pointer) renders into that target at the matching offset.
 * A texture whose address, pitch and pixel size fall in what the GPU drew comes from the
 * target: the region is copied into a snapshot texture at that point of the command stream,
 * which is what the PSP reads from VRAM at that moment (also when a draw samples its own
 * target). Each target has its own depth-stencil buffer: the PSP keeps the stencil in the
 * alpha channel of the colour buffer, so it belongs with the colour target. A new target starts
 * from what is in memory; block transfers between targets are GPU copies, transfers into a
 * target are uploaded, and a target read directly (transfer source, CLUT) is read back into
 * PSP memory.
 *
 * Not yet: the stencil is not mirrored into the colour alpha (nothing seen so far reads it as
 * colour), CLUT textures indexing a target, logic ops, double blend factors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psp_rt.h"
#include "rt_gpu.h"

#ifdef GOW_HAVE_SDL
#include <SDL3/SDL.h>

#include "gpu_shaders.h"

#define DISP_W 480
#define DISP_H 272
#define MAX_TARGETS 64
#define MAX_PIPELINES 512
#define MAX_SAMPLERS 256
#define MAX_SNAPS 512
#define TEX_SLOTS 16384  /* power of two */
#define F_SRCFIX 128u
#define F_TEXOPAQUE 256u
#define VRAM_LO 0x04000000u
#define VRAM_HI 0x04200000u

typedef struct {
    uint32_t addr, pitch;      /* base address, bytes per row */
    int bpp, fmt, w, h;        /* bytes per pixel, PSP pixel format, size in pixels */
    int ux0, uy0, ux1, uy1;    /* area drawn (or uploaded by transfers): [ux0, ux1) x [uy0, uy1) */
    SDL_GPUTexture *tex, *ds;  /* colour, depth-stencil */
    int ds_init;
    uint64_t seq;              /* order of the latest write (the most recent target wins) */
    uint32_t version;          /* changes with every write (snapshot validity) */
    uint64_t drawn_frame;      /* frame of the latest GPU draw */
    uint64_t mem_hash;         /* display area of PSP memory when last uploaded */
    int stencil_dirty;         /* stencil written since its values were last copied into the alpha channel */
} Target;
typedef struct { uint64_t key; SDL_GPUTexture *tex; int w, h; uint64_t used; } TexEntry;
typedef struct { uint64_t key; SDL_GPUGraphicsPipeline *p; } Pipe;
typedef struct { uint32_t key; SDL_GPUSampler *s; } Samp;
typedef struct { SDL_GPUTexture *tex; int w, h, busy, target, x, y; uint32_t version; } Snap;

/* layout of FragmentUniforms in ge.hlsl */
typedef struct { uint32_t mode[4]; float env[4], fogcol[4], fixa[4], zrange[4]; } FragU;

enum { CMD_DRAW, CMD_COPY, CMD_UPLOAD };
typedef struct {
    int kind, target, pipe;
    SDL_GPUSampler *sampler;
    SDL_Rect scissor;
    SDL_FColor blend_const;
    SDL_GPUTexture *tex;           /* draw: texture; copy: source */
    SDL_GPUTexture *dst;           /* copy, upload: destination */
    int sx, sy, dx, dy, w, h;      /* copy, upload: rectangle */
    uint32_t first, count, offset; /* draw: vertices; upload: staged bytes */
    int stencil, stencil_ref;
    FragU fu;
} Cmd;

typedef struct { SDL_GPUTexture *tex; uint32_t offset; int w, h, level; } Upload;

static SDL_GPUDevice *g_dev;
static SDL_Window *g_win;
static SDL_GPUShader *g_vs, *g_fs;
static SDL_GPUTextureFormat g_dsfmt;
static Samp g_samps[MAX_SAMPLERS];
static int g_nsamps;
static SDL_GPUTexture *g_white;  /* bound for untextured draws */
static Target g_targets[MAX_TARGETS];
static int g_ntargets;
static uint64_t g_seq;
static Snap g_snaps[MAX_SNAPS];
static int g_nsnaps;
static Pipe g_pipes[MAX_PIPELINES];
static int g_npipes;
static TexEntry *g_tex;
static size_t g_tex_count;
static GpuVertex *g_verts;
static size_t g_nverts, g_vcap;
static Cmd *g_cmds;
static size_t g_ncmds, g_ccap;
static Upload *g_ups;
static size_t g_nups, g_upcap;
static uint8_t *g_upbytes;
static size_t g_upsize, g_upbcap;
static uint32_t *g_dl;
static size_t g_dlcap;
static SDL_GPUBuffer *g_vbuf;
static uint32_t g_vbuf_size;
static SDL_GPUTransferBuffer *g_tbuf;
static uint32_t g_tbuf_size, g_voff;
static uint64_t g_frame;
static int g_xfer_gpu;  /* the block transfer in progress was made on the GPU */
static struct {
    uint64_t frames, draws, verts, passes, uploads, upload_bytes, pipelines, targets, snapshots, target_uploads, gpu_copies,
        readbacks, submits, resolves, fills, gpu_wait_ns, gpu_wait_max, acquire_ns, acquire_max, acquire_slow;
} g_st;

static void grow(void **p, size_t *cap, size_t need, size_t elem) {
    if (need <= *cap) return;
    size_t n = *cap ? *cap : 1024;
    while (n < need) n *= 2;
    *p = realloc(*p, n * elem);
    *cap = n;
}

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

int rt_gpu_active(void) { return g_dev != NULL; }

/* --- PSP pixels (as the software renderer packs and expands them) ------------------------- */
static uint32_t pixel_in(const uint8_t *p, int fmt) {
    uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8, r, g, b, a;
    switch (fmt) {
    case 0:
        r = v & 31; g = (v >> 5) & 63; b = (v >> 11) & 31;
        return ((r << 3) | (r >> 2)) | ((g << 2) | (g >> 4)) << 8 | ((b << 3) | (b >> 2)) << 16 | 0xFF000000u;
    case 1:
        r = v & 31; g = (v >> 5) & 31; b = (v >> 10) & 31; a = v >> 15;
        return ((r << 3) | (r >> 2)) | ((g << 3) | (g >> 2)) << 8 | ((b << 3) | (b >> 2)) << 16 | (a ? 0xFF000000u : 0);
    case 2:
        r = v & 15; g = (v >> 4) & 15; b = (v >> 8) & 15; a = v >> 12;
        return r * 17 | (g * 17) << 8 | (b * 17) << 16 | (a * 17) << 24;
    default: { uint32_t c; memcpy(&c, p, 4); return c; }
    }
}

static void pixel_out(uint8_t *p, uint32_t c, int fmt) {
    uint32_t v;
    switch (fmt) {
    case 0: v = ((c >> 3) & 0x1F) | (((c >> 10) & 0x3F) << 5) | (((c >> 19) & 0x1F) << 11); break;
    case 1: v = ((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10) | ((c >> 31) << 15); break;
    case 2: v = ((c >> 4) & 0xF) | (((c >> 12) & 0xF) << 4) | (((c >> 20) & 0xF) << 8) | (((c >> 28) & 0xF) << 12); break;
    default: memcpy(p, &c, 4); return;
    }
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* --- device ------------------------------------------------------------------------- */
static SDL_GPUShader *make_shader(SDL_GPUShaderFormat fmt, SDL_GPUShaderStage stage) {
    SDL_GPUShaderCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    int vs = stage == SDL_GPU_SHADERSTAGE_VERTEX;
    if (fmt == SDL_GPU_SHADERFORMAT_SPIRV) {
        ci.code = vs ? ge_vs_spirv : ge_fs_spirv;
        ci.code_size = vs ? sizeof(ge_vs_spirv) : sizeof(ge_fs_spirv);
    } else {
        ci.code = vs ? ge_vs_dxil : ge_fs_dxil;
        ci.code_size = vs ? sizeof(ge_vs_dxil) : sizeof(ge_fs_dxil);
    }
    ci.entrypoint = vs ? "vs_main" : "fs_main";
    ci.format = fmt;
    ci.stage = stage;
    ci.num_samplers = vs ? 0 : 1;
    ci.num_uniform_buffers = 1;
    SDL_GPUShader *s = SDL_CreateGPUShader(g_dev, &ci);
    if (!s) fprintf(stderr, "[gpu] shader: %s\n", SDL_GetError());
    return s;
}

int rt_gpu_init(void *window, const char *driver) {
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "[gpu] SDL_Init: %s\n", SDL_GetError()); return 0; }
    g_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL, getenv("RT_GPU_DEBUG") != NULL, driver);
    if (!g_dev) { fprintf(stderr, "[gpu] SDL_CreateGPUDevice: %s\n", SDL_GetError()); return 0; }
    SDL_GPUShaderFormat fmts = SDL_GetGPUShaderFormats(g_dev);
    SDL_GPUShaderFormat use = (fmts & SDL_GPU_SHADERFORMAT_SPIRV) ? SDL_GPU_SHADERFORMAT_SPIRV : SDL_GPU_SHADERFORMAT_DXIL;
    g_vs = make_shader(use, SDL_GPU_SHADERSTAGE_VERTEX);
    g_fs = make_shader(use, SDL_GPU_SHADERSTAGE_FRAGMENT);
    if (!g_vs || !g_fs) return 0;
    g_dsfmt = SDL_GPUTextureSupportsFormat(g_dev, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
                                           SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                  ? SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT
                  : SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    g_tex = calloc(TEX_SLOTS, sizeof(TexEntry));
    {
        SDL_GPUTextureCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ci.width = ci.height = 1;
        ci.layer_count_or_depth = ci.num_levels = 1;
        g_white = SDL_CreateGPUTexture(g_dev, &ci);
        static const uint32_t white = 0xFFFFFFFFu;
        size_t off = (g_upsize + 511) & ~(size_t)511;
        grow((void **)&g_upbytes, &g_upbcap, off + 4, 1);
        memcpy(g_upbytes + off, &white, 4);
        g_upsize = off + 4;
        grow((void **)&g_ups, &g_upcap, g_nups + 1, sizeof(Upload));
        memset(&g_ups[g_nups], 0, sizeof(Upload));
        g_ups[g_nups].tex = g_white; g_ups[g_nups].offset = (uint32_t)off; g_ups[g_nups].w = g_ups[g_nups].h = 1;
        g_nups++;
    }
    if (window) {
        g_win = window;
        if (!SDL_ClaimWindowForGPUDevice(g_dev, g_win)) { fprintf(stderr, "[gpu] claim window: %s\n", SDL_GetError()); return 0; }
        SDL_GPUPresentMode pm = SDL_WindowSupportsGPUPresentMode(g_dev, g_win, SDL_GPU_PRESENTMODE_MAILBOX) ? SDL_GPU_PRESENTMODE_MAILBOX
                              : SDL_WindowSupportsGPUPresentMode(g_dev, g_win, SDL_GPU_PRESENTMODE_IMMEDIATE) ? SDL_GPU_PRESENTMODE_IMMEDIATE
                              : SDL_GPU_PRESENTMODE_VSYNC;
        SDL_SetGPUSwapchainParameters(g_dev, g_win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, pm);
        printf("[gpu] present mode: %s\n", pm == SDL_GPU_PRESENTMODE_MAILBOX ? "mailbox" : pm == SDL_GPU_PRESENTMODE_IMMEDIATE ? "immediate" : "vsync");
    }
    printf("[gpu] SDL GPU device: %s, %s shaders%s\n", SDL_GetGPUDeviceDriver(g_dev),
           use == SDL_GPU_SHADERFORMAT_SPIRV ? "SPIR-V" : "DXIL", window ? "" : " (headless)");
    return 1;
}

static Cmd *new_cmd(int kind) {
    grow((void **)&g_cmds, &g_ccap, g_ncmds + 1, sizeof(Cmd));
    Cmd *c = &g_cmds[g_ncmds++];
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    return c;
}

/* Stages w x h pixels of PSP memory (rows of `pitch` bytes from addr, pixel format fmt) as
 * RGBA8 and records their upload to (x, y) of dst, in command order. */
static void record_upload(SDL_GPUTexture *dst, int x, int y, int w, int h, uint32_t addr, uint32_t pitch, int fmt) {
    if (w <= 0 || h <= 0) return;
    int bpp = fmt == 3 ? 4 : 2;
    size_t bytes = (size_t)w * h * 4, off = (g_upsize + 511) & ~(size_t)511;
    grow((void **)&g_upbytes, &g_upbcap, off + bytes, 1);
    uint32_t *o = (uint32_t *)(g_upbytes + off);
    for (int j = 0; j < h; j++) {
        const uint8_t *row = rt_ptr(addr + (uint32_t)j * pitch);
        for (int i = 0; i < w; i++) o[(size_t)j * w + i] = pixel_in(row + (size_t)i * bpp, fmt);
    }
    g_upsize = off + bytes;
    Cmd *c = new_cmd(CMD_UPLOAD);
    c->dst = dst;
    c->dx = x; c->dy = y; c->w = w; c->h = h;
    c->offset = (uint32_t)off;
    g_st.target_uploads++;
    g_st.upload_bytes += bytes;
}

/* --- render targets ----------------------------------------------------------------- */
/* hash of the display area of a target's memory: shows whether the CPU changed it */
static uint64_t display_hash(const Target *t) {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    int rows = imin(DISP_H, t->h), n = imin(DISP_W, t->w) * t->bpp;
    for (int y = 0; y < rows; y++) {
        const uint8_t *p = rt_ptr(t->addr + (uint32_t)y * t->pitch);
        int i = 0;
        for (; i + 8 <= n; i += 8) {
            uint64_t v;
            memcpy(&v, p + i, 8);
            h = (h ^ v) * 0x100000001B3ull;
            h ^= h >> 29;
        }
        for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    }
    return h;
}

static int new_target(uint32_t addr, uint32_t pitch, int bpp, int fmt) {
    if (g_ntargets == MAX_TARGETS) return 0;
    Target *t = &g_targets[g_ntargets];
    memset(t, 0, sizeof(*t));
    t->addr = addr;
    t->pitch = pitch;
    t->bpp = bpp;
    t->fmt = fmt;
    t->w = imax(1, imin(1024, (int)(pitch / (uint32_t)bpp)));
    uint32_t rows = addr >= VRAM_LO && addr < VRAM_HI ? (VRAM_HI - addr) / pitch : 512;
    t->h = rows > 512 ? 512 : rows < 1 ? 1 : (int)rows;
    t->ux0 = t->uy0 = 1 << 30;
    SDL_GPUTextureCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = (Uint32)t->w;
    ci.height = (Uint32)t->h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    t->tex = SDL_CreateGPUTexture(g_dev, &ci);
    ci.format = g_dsfmt;
    ci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    t->ds = SDL_CreateGPUTexture(g_dev, &ci);
    if (!t->tex || !t->ds) {
        fprintf(stderr, "[gpu] render target %08X: %s\n", addr, SDL_GetError());
        return 0;
    }
    /* start from what is in memory: an image the CPU or a transfer put there before any draw */
    record_upload(t->tex, 0, 0, t->w, t->h, addr, pitch, fmt);
    t->mem_hash = display_hash(t);
    g_st.targets++;
    return g_ntargets++;
}

/* Position of addr in target t when it uses this pitch and pixel size. */
static int locate(const Target *t, uint32_t addr, uint32_t pitch, int bpp, int *x, int *y) {
    if (t->pitch != pitch || t->bpp != bpp || addr < t->addr) return 0;
    uint32_t off = addr - t->addr;
    if ((off % pitch) % (uint32_t)bpp) return 0;
    *y = (int)(off / pitch);
    *x = (int)((off % pitch) / (uint32_t)bpp);
    return *y < t->h && *x < t->w;
}

/* The target a draw to addr goes to, and the draw's offset in it: the target starting there,
 * or one whose drawn rows contain addr, or a new one. */
static int target_for_draw(uint32_t addr, uint32_t pitch, int bpp, int fmt, int *ox, int *oy) {
    int best = -1, bx = 0, by = 0;
    for (int i = 0; i < g_ntargets; i++) {
        int x, y;
        if (!locate(&g_targets[i], addr, pitch, bpp, &x, &y)) continue;
        if (x == 0 && y == 0) { best = i; bx = by = 0; break; }
        if (y < g_targets[i].uy1 && (best < 0 || g_targets[i].seq > g_targets[best].seq)) { best = i; bx = x; by = y; }
    }
    if (best < 0) { best = new_target(addr, pitch, bpp, fmt); bx = by = 0; }
    *ox = bx;
    *oy = by;
    return best;
}

/* The most recently written target that drew at addr (a texture's first texel), or -1. */
static int target_for_texture(uint32_t addr, uint32_t pitch, int bpp, int *ox, int *oy) {
    int best = -1;
    for (int i = 0; i < g_ntargets; i++) {
        const Target *t = &g_targets[i];
        int x, y;
        if (!locate(t, addr, pitch, bpp, &x, &y) || x < t->ux0 || x >= t->ux1 || y < t->uy0 || y >= t->uy1) continue;
        if (best < 0 || t->seq > g_targets[best].seq) { best = i; *ox = x; *oy = y; }
    }
    return best;
}

/* The target holding rectangle (x, y, w, h) of a buffer at base: any that contains it, or (drawn)
 * only one where it overlaps what was drawn. */
static int target_for_rect(uint32_t base, uint32_t pitch, int bpp, int x, int y, int w, int h, int drawn, int *tx, int *ty) {
    if (!pitch) return -1;
    uint32_t addr = base + (uint32_t)y * pitch + (uint32_t)x * (uint32_t)bpp;
    int best = -1;
    for (int i = 0; i < g_ntargets; i++) {
        const Target *t = &g_targets[i];
        int px, py;
        if (!locate(t, addr, pitch, bpp, &px, &py) || px + w > t->w || py + h > t->h) continue;
        if (drawn && !(px < t->ux1 && px + w > t->ux0 && py < t->uy1 && py + h > t->uy0)) continue;
        if (best < 0 || t->seq > g_targets[best].seq) { best = i; *tx = px; *ty = py; }
    }
    return best;
}

static void mark_written(Target *t, int x0, int y0, int x1, int y1) {
    x0 = imax(x0, 0); y0 = imax(y0, 0); x1 = imin(x1, t->w); y1 = imin(y1, t->h);
    if (x1 <= x0 || y1 <= y0) return;
    t->ux0 = imin(t->ux0, x0); t->uy0 = imin(t->uy0, y0);
    t->ux1 = imax(t->ux1, x1); t->uy1 = imax(t->uy1, y1);
    t->seq = ++g_seq;
    t->version++;
}

int rt_gpu_texture_is_target(uint32_t addr, uint32_t pitch, int bpp) {
    int x, y;
    return g_dev && target_for_texture(addr, pitch, bpp, &x, &y) >= 0;
}

static int download(SDL_GPUTexture *tex, int x, int y, int w, int h, uint32_t *out);
static void record_resolve(int ti);

/* Debugging ("both" mode, software drawing flushed): compares a render-to-texture source as the
 * GPU has it with PSP memory, as the software renderer drew it. */
void rt_gpu_debug_compare_texture(uint32_t addr, uint32_t pitch, int bpp, int fmt, int w, int h) {
    int x = 0, y = 0, ti = target_for_texture(addr, pitch, bpp, &x, &y);
    if (!g_dev || ti < 0) return;
    const Target *t = &g_targets[ti];
    w = imin(w, t->ux1 - x);  /* only what was drawn */
    h = imin(h, t->uy1 - y);
    if (w <= 0 || h <= 0) return;
    record_resolve(ti);
    uint32_t *g = malloc((size_t)w * h * 4);
    if (!g || !download(t->tex, x, y, w, h, g)) { free(g); return; }
    double sum = 0;
    int worst = 0, wx = 0, wy = 0, nch = fmt == 3 ? 4 : 3;  /* 8888: the alpha (stencil) too */
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint32_t m = pixel_in(rt_ptr(addr + (uint32_t)j * pitch + (uint32_t)i * (uint32_t)bpp), fmt), c = g[(size_t)j * w + i];
            for (int ch = 0; ch < nch; ch++) {
                int d = abs((int)((m >> (8 * ch)) & 0xFF) - (int)((c >> (8 * ch)) & 0xFF));
                sum += d;
                if (d > worst) { worst = d; wx = i; wy = j; }
            }
        }
    uint32_t mw = pixel_in(rt_ptr(addr + (uint32_t)wy * pitch + (uint32_t)wx * (uint32_t)bpp), fmt), gw = g[(size_t)wy * w + wx];
    printf("[gpu-check] texture %08X pitch %u fmt %d %dx%d (target %08X +%d,%d): mean |diff| %.2f, max %d at %d,%d mem %08X gpu %08X\n",
           addr, pitch, fmt, w, h, t->addr, x, y, sum / ((double)w * h * nch), worst, wx, wy, mw, gw);
    free(g);
}

static int pipeline(uint64_t key);
static SDL_GPUSampler *sampler_for(const GpuDraw *d);

/* Records a quad over [x0, x1) x [y0, y1) of target ti in one colour (shader clear mode: the
 * vertex colour as is), with the pipeline of `key`. */
static void record_quad(int ti, int x0, int y0, int x1, int y1, uint32_t rgba, uint64_t key, int stencil, int sref) {
    static GpuDraw plain;
    Cmd *c = new_cmd(CMD_DRAW);
    c->target = ti;
    c->pipe = pipeline(key);
    c->stencil = stencil;
    c->stencil_ref = sref;
    c->scissor.x = x0; c->scissor.y = y0; c->scissor.w = x1 - x0; c->scissor.h = y1 - y0;
    c->sampler = sampler_for(&plain);
    c->fu.mode[0] = GPU_F_CLEAR;
    c->blend_const.a = 1;
    c->first = (uint32_t)g_nverts;
    c->count = 6;
    grow((void **)&g_verts, &g_vcap, g_nverts + 6, sizeof(GpuVertex));
    static const int cx[6] = { 0, 1, 0, 0, 1, 1 }, cy[6] = { 0, 0, 1, 1, 0, 1 };
    for (int i = 0; i < 6; i++) {
        GpuVertex *o = &g_verts[g_nverts++];
        memset(o, 0, sizeof(*o));
        o->x = (float)(cx[i] ? x1 : x0);
        o->y = (float)(cy[i] ? y1 : y0);
        o->w = o->q = 1.0f;
        memcpy(o->c0, &rgba, 4);
    }
}

/* The PSP keeps the stencil in the alpha channel of the colour buffer, the GPU in the
 * depth-stencil buffer. Before a target is read (as a texture, or back into memory) its alpha
 * gets the stencil values: it is zeroed, then each stencil bit adds its value where the stencil
 * test finds the bit set. */
static void record_resolve(int ti) {
    Target *t = &g_targets[ti];
    if (!t->stencil_dirty) return;
    t->stencil_dirty = 0;
    if (t->ux1 <= t->ux0 || t->uy1 <= t->uy0) return;
    uint64_t base = 1ull << 16 | 8ull << 12;  /* depth test always, alpha written only */
    record_quad(ti, t->ux0, t->uy0, t->ux1, t->uy1, 0, base, 0, 0);
    for (int b = 0; b < 8; b++) {
        uint64_t skey = 1 | 3u << 1 | (uint64_t)(1u << b) << 13;  /* stencil & bit != 0, keep, no writes */
        record_quad(ti, t->ux0, t->uy0, t->ux1, t->uy1, (uint32_t)(1u << b) << 24, base | 1ull << 49 | skey << 20, 1, 0);
    }
    t->version++;
    g_st.resolves++;
}

/* A copy of the w x h region at (x, y) of target ti as it is at this point of the frame. */
static SDL_GPUTexture *snapshot(int ti, int x, int y, int w, int h) {
    Target *t = &g_targets[ti];
    record_resolve(ti);
    for (int i = 0; i < g_nsnaps; i++) {
        const Snap *s = &g_snaps[i];
        if (s->busy && s->target == ti && s->version == t->version && s->x == x && s->y == y && s->w == w && s->h == h) return s->tex;
    }
    Snap *s = NULL;
    for (int i = 0; i < g_nsnaps && !s; i++) if (!g_snaps[i].busy && g_snaps[i].w == w && g_snaps[i].h == h) s = &g_snaps[i];
    if (!s) {
        if (g_nsnaps == MAX_SNAPS) return NULL;
        SDL_GPUTextureCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ci.width = (Uint32)w;
        ci.height = (Uint32)h;
        ci.layer_count_or_depth = 1;
        ci.num_levels = 1;
        SDL_GPUTexture *tex = SDL_CreateGPUTexture(g_dev, &ci);
        if (!tex) return NULL;
        s = &g_snaps[g_nsnaps++];
        s->tex = tex;
        s->w = w;
        s->h = h;
    }
    s->busy = 1;
    s->target = ti;
    s->version = t->version;
    s->x = x;
    s->y = y;
    Cmd *c = new_cmd(CMD_COPY);
    c->tex = t->tex;
    c->sx = x; c->sy = y;
    c->dst = s->tex;
    c->w = imin(w, t->w - x);
    c->h = imin(h, t->h - y);
    g_st.snapshots++;
    return s->tex;
}

/* --- pipelines ----------------------------------------------------------------- */
static SDL_GPUBlendFactor map_factor(int f, int is_dst, uint32_t fix, int *uses_const) {
    switch (f) {
    case 0: return is_dst ? SDL_GPU_BLENDFACTOR_SRC_COLOR : SDL_GPU_BLENDFACTOR_DST_COLOR;
    case 1: return is_dst ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR : SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case 2: case 6: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;            /* 6: 2 x src alpha (approximated) */
    case 3: case 7: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case 4: case 8: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case 5: case 9: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    default:
        fix &= 0xFFFFFF;
        if (fix == 0) return SDL_GPU_BLENDFACTOR_ZERO;
        if (fix == 0xFFFFFF) return SDL_GPU_BLENDFACTOR_ONE;
        *uses_const = 1;
        return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    }
}

static const SDL_GPUCompareOp k_cmp[8] = {
    SDL_GPU_COMPAREOP_NEVER, SDL_GPU_COMPAREOP_ALWAYS, SDL_GPU_COMPAREOP_EQUAL, SDL_GPU_COMPAREOP_NOT_EQUAL,
    SDL_GPU_COMPAREOP_LESS, SDL_GPU_COMPAREOP_LESS_OR_EQUAL, SDL_GPU_COMPAREOP_GREATER, SDL_GPU_COMPAREOP_GREATER_OR_EQUAL,
};
static const SDL_GPUBlendOp k_op[6] = {
    SDL_GPU_BLENDOP_ADD, SDL_GPU_BLENDOP_SUBTRACT, SDL_GPU_BLENDOP_REVERSE_SUBTRACT,
    SDL_GPU_BLENDOP_MIN, SDL_GPU_BLENDOP_MAX, SDL_GPU_BLENDOP_ADD,  /* 5: absolute difference (approximated) */
};
/* PSP stencil operations: keep, zero, replace, invert, increment, decrement */
static const SDL_GPUStencilOp k_sop[8] = {
    SDL_GPU_STENCILOP_KEEP, SDL_GPU_STENCILOP_ZERO, SDL_GPU_STENCILOP_REPLACE, SDL_GPU_STENCILOP_INVERT,
    SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP, SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP, SDL_GPU_STENCILOP_KEEP, SDL_GPU_STENCILOP_KEEP,
};

/* key: blend 1 | src 4 | dst 4 | op 3 | write mask 4 | zfunc 3 | zwrite 1 |
 *      stencil test 1 | func 3 | fail 3 | zfail 3 | zpass 3 | compare mask 8 | write mask 8 | alpha add 1 */
static int pipeline(uint64_t key) {
    for (int i = 0; i < g_npipes; i++) if (g_pipes[i].key == key) return i;
    if (g_npipes == MAX_PIPELINES) return 0;
    SDL_GPUVertexBufferDescription vb = { 0, sizeof(GpuVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 };
    SDL_GPUVertexAttribute va[5] = {
        { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 },
        { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 16 },
        { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 28 },
        { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 32 },
        { 4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 36 },
    };
    SDL_GPUColorTargetDescription ct;
    memset(&ct, 0, sizeof(ct));
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ct.blend_state.enable_blend = key & 1;
    ct.blend_state.src_color_blendfactor = (SDL_GPUBlendFactor)((key >> 1) & 15);
    ct.blend_state.dst_color_blendfactor = (SDL_GPUBlendFactor)((key >> 5) & 15);
    ct.blend_state.color_blend_op = (SDL_GPUBlendOp)((key >> 9) & 7);
    ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    if ((key >> 49) & 1) {  /* stencil-to-alpha copy: alpha += source alpha */
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    }
    ct.blend_state.enable_color_write_mask = true;
    ct.blend_state.color_write_mask = (SDL_GPUColorComponentFlags)((key >> 12) & 15);
    SDL_GPUGraphicsPipelineCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.vertex_shader = g_vs;
    ci.fragment_shader = g_fs;
    ci.vertex_input_state.vertex_buffer_descriptions = &vb;
    ci.vertex_input_state.num_vertex_buffers = 1;
    ci.vertex_input_state.vertex_attributes = va;
    ci.vertex_input_state.num_vertex_attributes = 5;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  /* culled on the CPU like the PSP does */
    ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    ci.rasterizer_state.enable_depth_clip = false;          /* depth is already in range */
    ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    ci.depth_stencil_state.enable_depth_test = true;
    ci.depth_stencil_state.compare_op = k_cmp[(key >> 16) & 7];
    ci.depth_stencil_state.enable_depth_write = (key >> 19) & 1;
    if ((key >> 20) & 1) {
        SDL_GPUStencilOpState so;
        so.compare_op = k_cmp[(key >> 21) & 7];
        so.fail_op = k_sop[(key >> 24) & 7];
        so.depth_fail_op = k_sop[(key >> 27) & 7];
        so.pass_op = k_sop[(key >> 30) & 7];
        ci.depth_stencil_state.enable_stencil_test = true;
        ci.depth_stencil_state.front_stencil_state = so;
        ci.depth_stencil_state.back_stencil_state = so;
        ci.depth_stencil_state.compare_mask = (Uint8)(key >> 33);
        ci.depth_stencil_state.write_mask = (Uint8)(key >> 41);
    }
    ci.target_info.color_target_descriptions = &ct;
    ci.target_info.num_color_targets = 1;
    ci.target_info.depth_stencil_format = g_dsfmt;
    ci.target_info.has_depth_stencil_target = true;
    g_pipes[g_npipes].key = key;
    g_pipes[g_npipes].p = SDL_CreateGPUGraphicsPipeline(g_dev, &ci);
    if (!g_pipes[g_npipes].p) fprintf(stderr, "[gpu] pipeline %llX: %s\n", (unsigned long long)key, SDL_GetError());
    g_st.pipelines++;
    return g_npipes++;
}

/* --- samplers -------------------------------------------------------------------- */
/* PSP texture filtering: the level of detail is log2(texels per pixel) plus an offset in
 * 1/16 levels (LOD mode 0); minf above level 0, magf at or below it; mipf blends levels.
 * Mode 2 uses the offset as a constant level. Mode 1 (slope from w) is approximated by mode 0. */
static SDL_GPUSampler *sampler_for(const GpuDraw *d) {
    int levels = d->textured && d->levels > 1 ? d->levels : 1;
    uint32_t key = (uint32_t)(d->minf & 1) | (uint32_t)(d->magf & 1) << 1 | (uint32_t)(d->mipf & 1) << 2 |
                   (uint32_t)(d->clampS & 1) << 3 | (uint32_t)(d->clampT & 1) << 4 | (uint32_t)(d->lodmode & 3) << 5 |
                   (uint32_t)(d->lodoff & 0xFF) << 7 | (uint32_t)(levels - 1) << 15;
    for (int i = 0; i < g_nsamps; i++)
        if (g_samps[i].key == key) return g_samps[i].s;
    SDL_GPUSamplerCreateInfo si;
    memset(&si, 0, sizeof(si));
    si.min_filter = d->minf ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    si.mag_filter = d->magf ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode = d->mipf ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = d->clampS ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    si.address_mode_v = d->clampT ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    float off = (float)d->lodoff / 16.0f, top = (float)(levels - 1);
    if (d->lodmode == 2) {
        float l = off < 0 ? 0 : off > top ? top : off;
        si.min_lod = si.max_lod = l;
    } else {
        si.mip_lod_bias = off;
        si.min_lod = 0;
        si.max_lod = top;
    }
    SDL_GPUSampler *s = SDL_CreateGPUSampler(g_dev, &si);
    if (g_nsamps < MAX_SAMPLERS) {
        g_samps[g_nsamps].key = key;
        g_samps[g_nsamps++].s = s;
    }
    return s;
}

/* --- textures -------------------------------------------------------------------- */
static TexEntry *tex_slot(uint64_t key) {
    size_t i = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 50) & (TEX_SLOTS - 1);
    while (g_tex[i].key && g_tex[i].key != key) i = (i + 1) & (TEX_SLOTS - 1);
    return &g_tex[i];
}

int rt_gpu_texture_resident(uint64_t key) {
    TexEntry *e = tex_slot(key | 1);
    if (!e->key) return 0;
    e->used = g_frame;
    return 1;
}

void rt_gpu_texture_upload(uint64_t key, int w, int h, int levels, const uint32_t *rgba) {
    key |= 1;
    TexEntry *e = tex_slot(key);
    if (e->key) return;
    if (g_tex_count > TEX_SLOTS * 3 / 4) return;  /* full until the next eviction */
    SDL_GPUTextureCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = (Uint32)w;
    ci.height = (Uint32)h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = (Uint32)(levels < 1 ? 1 : levels);
    e->tex = SDL_CreateGPUTexture(g_dev, &ci);
    if (!e->tex) return;
    e->key = key;
    e->w = w;
    e->h = h;
    e->used = g_frame;
    g_tex_count++;
    for (int l = 0; l < (int)ci.num_levels; l++) {
        int lw = w >> l > 0 ? w >> l : 1, lh = h >> l > 0 ? h >> l : 1;
        size_t bytes = (size_t)lw * lh * 4, off = (g_upsize + 511) & ~(size_t)511;
        grow((void **)&g_upbytes, &g_upbcap, off + bytes, 1);
        memcpy(g_upbytes + off, rgba, bytes);
        rgba += (size_t)lw * lh;
        g_upsize = off + bytes;
        grow((void **)&g_ups, &g_upcap, g_nups + 1, sizeof(Upload));
        g_ups[g_nups].tex = e->tex;
        g_ups[g_nups].offset = (uint32_t)off;
        g_ups[g_nups].w = lw;
        g_ups[g_nups].h = lh;
        g_ups[g_nups].level = l;
        g_nups++;
        g_st.upload_bytes += bytes;
    }
    g_st.uploads++;
}

/* releases textures not used for a while (after the frame was submitted) */
static void evict_textures(void) {
    if (g_tex_count < TEX_SLOTS / 2 && g_frame % 600) return;
    TexEntry *old = g_tex;
    g_tex = calloc(TEX_SLOTS, sizeof(TexEntry));
    g_tex_count = 0;
    for (size_t i = 0; i < TEX_SLOTS; i++) {
        if (!old[i].key) continue;
        if (g_frame - old[i].used > 300) { SDL_ReleaseGPUTexture(g_dev, old[i].tex); continue; }
        *tex_slot(old[i].key) = old[i];
        g_tex_count++;
    }
    free(old);
}

/* --- recording ------------------------------------------------------------------- */
static void color_f(uint32_t c, float out[4]) {
    for (int i = 0; i < 3; i++) out[i] = ((c >> (8 * i)) & 0xFF) / 255.0f;
    out[3] = 1.0f;
}

void rt_gpu_draw(const GpuDraw *d, const GpuVertex *v, uint32_t n) {
    if (!g_dev || !n) return;
    int bpp = d->fb_fmt == 3 ? 4 : 2, ox, oy;
    uint32_t pitch = (d->fb_stride ? d->fb_stride : 512) * (uint32_t)bpp;
    int ti = target_for_draw(d->fb_addr, pitch, bpp, (int)d->fb_fmt, &ox, &oy);
    Target *t = &g_targets[ti];
    /* texture: decoded from memory, or a snapshot of what the GPU drew there */
    SDL_GPUTexture *tex = NULL;
    uint32_t extra = 0;
    if (d->textured) {
        if (d->tex_target) {
            int tx = 0, ty = 0, s = target_for_texture(d->tex_addr, d->tex_pitch, d->tex_bpp, &tx, &ty);
            if (s >= 0) tex = snapshot(s, tx, ty, d->tex_w, d->tex_h);
            if (d->tex_fmt == 0) extra |= F_TEXOPAQUE;  /* 565 has no alpha */
            static long trace_frame = -2;
            if (trace_frame == -2) { const char *e = getenv("RT_GPU_TRACE_FRAME"); trace_frame = e ? atol(e) : -1; }
            if ((long)g_frame == trace_frame)
                printf("[gpu-rtt] draw to %08X (target %08X +%d,%d) samples %08X pitch %u fmt %d %dx%d from target %08X +%d,%d\n",
                       d->fb_addr, t->addr, ox, oy, d->tex_addr, d->tex_pitch, d->tex_fmt, d->tex_w, d->tex_h,
                       s >= 0 ? g_targets[s].addr : 0, tx, ty);
        } else {
            TexEntry *e = tex_slot(d->tex_key | 1);
            if (e->key) { tex = e->tex; e->used = g_frame; }
        }
    }
    /* blending */
    int uses_a = 0, uses_b = 0, srcfix = 0;
    SDL_GPUBlendFactor sf = SDL_GPU_BLENDFACTOR_ONE, df = SDL_GPU_BLENDFACTOR_ZERO;
    SDL_FColor bc = { 0, 0, 0, 1 };
    if (d->blend) {
        sf = map_factor(d->bsrc, 0, d->fixa, &uses_a);
        df = map_factor(d->bdst, 1, d->fixb, &uses_b);
        if (uses_a && uses_b) { srcfix = 1; sf = SDL_GPU_BLENDFACTOR_ONE; uses_a = 0; }
        float c[4];
        color_f(uses_a ? d->fixa : d->fixb, c);
        bc.r = c[0]; bc.g = c[1]; bc.b = c[2];
    }
    uint32_t wmask = ((d->mask_rgb & 0xFF) != 0xFF ? 1u : 0) | ((d->mask_rgb & 0xFF00) != 0xFF00 ? 2u : 0) |
                     ((d->mask_rgb & 0xFF0000) != 0xFF0000 ? 4u : 0) | (d->write_alpha ? 8u : 0);
    /* stencil: tested and updated in the depth-stencil buffer as the PSP does in the alpha channel */
    uint64_t skey = 0;
    int sref = 0;
    if (d->clear_stencil) {
        skey = 1 | 1u << 1 | 2u << 4 | 2u << 7 | 2u << 10 | 0xFFull << 13 | (uint64_t)(~d->skeep & 0xFF) << 21;
        sref = v[0].c0[3];
    } else if (d->stest) {
        skey = 1 | (uint64_t)(d->sfunc & 7) << 1 | (uint64_t)(d->sfail & 7) << 4 | (uint64_t)(d->zfail & 7) << 7 |
               (uint64_t)(d->zpass & 7) << 10 | (uint64_t)(d->smask & 0xFF) << 13 | (uint64_t)(~d->skeep & 0xFF) << 21;
        sref = d->sref & 0xFF;
    }
    uint64_t key = (d->blend ? 1u : 0) | ((uint64_t)sf << 1) | ((uint64_t)df << 5) | ((uint64_t)k_op[d->beq % 6] << 9) |
                   ((uint64_t)wmask << 12) | ((uint64_t)(d->zfunc & 7) << 16) | ((uint64_t)(d->zwrite ? 1 : 0) << 19) | skey << 20;
    Cmd *c = new_cmd(CMD_DRAW);
    c->target = ti;
    c->pipe = pipeline(key);
    c->blend_const = bc;
    c->stencil = skey != 0;
    c->stencil_ref = sref;
    int x1 = imax(d->sx1 + ox, 0), y1 = imax(d->sy1 + oy, 0);
    int x2 = imin(d->sx2 + ox, t->w - 1), y2 = imin(d->sy2 + oy, t->h - 1);
    c->scissor.x = x1; c->scissor.y = y1;
    c->scissor.w = x2 >= x1 ? x2 - x1 + 1 : 0; c->scissor.h = y2 >= y1 ? y2 - y1 + 1 : 0;
    c->tex = tex;
    c->sampler = sampler_for(d);
    c->fu.mode[0] = d->flags | (srcfix ? F_SRCFIX : 0) | extra;
    if (d->textured && !tex) c->fu.mode[0] &= ~(uint32_t)GPU_F_TEX;  /* texture not available: untextured */
    c->fu.mode[1] = d->texfunc;
    c->fu.mode[2] = d->afunc;
    c->fu.mode[3] = (d->aref & 0xFF) | ((d->amask & 0xFF) << 8);
    color_f(d->env, c->fu.env);
    color_f(d->fogcol, c->fu.fogcol);
    color_f(d->fixa, c->fu.fixa);
    c->fu.zrange[0] = d->minz / 65535.0f;
    c->fu.zrange[1] = d->maxz / 65535.0f;
    c->first = (uint32_t)g_nverts;
    c->count = n;
    grow((void **)&g_verts, &g_vcap, g_nverts + n, sizeof(GpuVertex));
    GpuVertex *o = g_verts + g_nverts;
    memcpy(o, v, n * sizeof(GpuVertex));
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (uint32_t i = 0; i < n; i++) {
        o[i].x += (float)ox;
        o[i].y += (float)oy;
        if (o[i].x < bx0) bx0 = o[i].x;
        if (o[i].x > bx1) bx1 = o[i].x;
        if (o[i].y < by0) by0 = o[i].y;
        if (o[i].y > by1) by1 = o[i].y;
    }
    g_nverts += n;
    int wx0 = imax(x1, (int)bx0), wy0 = imax(y1, (int)by0), wx1 = imin(x2 + 1, (int)bx1 + 1), wy1 = imin(y2 + 1, (int)by1 + 1);
    if (c->scissor.w && c->scissor.h) mark_written(t, wx0, wy0, wx1, wy1);
    if (skey && (d->clear_stencil || ((d->sfail || d->zfail || d->zpass) && (~d->skeep & 0xFF)))) t->stencil_dirty = 1;
    t->drawn_frame = g_frame;
    t->fmt = (int)d->fb_fmt;
    g_st.draws++;
    g_st.verts += n;
    /* a 32-bit fill over memory a 16-bit target uses (games clear 16-bit buffers with 32-bit
     * fills): the same fill there, when the 32-bit value is two equal 16-bit pixels */
    if (bpp == 4 && !d->textured && !(d->flags & GPU_F_FOG) && !d->mask_rgb && !d->skeep && wx1 > wx0 && wy1 > wy0) {
        int same = 1, alpha = -1;
        for (uint32_t i = 1; i < n && same; i++) same = !memcmp(v[i].c0, v[0].c0, 4) && !memcmp(v[i].c1, v[0].c1, 3);
        same = same && !v[0].c1[0] && !v[0].c1[1] && !v[0].c1[2];
        if (d->blend && !(d->bsrc == 2 && d->bdst == 3 && d->beq == 0 && v[0].c0[3] == 255)) same = 0;
        if (d->flags & GPU_F_CLEAR) alpha = d->clear_stencil ? v[0].c0[3] : -1;
        else if (d->stest && d->sfunc == 1 && d->zfunc == 1) alpha = d->zpass == 1 ? 0 : d->zpass == 2 ? (d->sref & 0xFF) : -1;
        uint32_t v32 = (uint32_t)v[0].c0[0] | (uint32_t)v[0].c0[1] << 8 | (uint32_t)v[0].c0[2] << 16 | (uint32_t)alpha << 24;
        if (same && alpha >= 0 && (v32 & 0xFFFF) == (v32 >> 16)) {
            uint8_t px[2] = { (uint8_t)v32, (uint8_t)(v32 >> 8) };
            for (int i = 0; i < g_ntargets; i++) {
                Target *u = &g_targets[i];
                if (i == ti || u->pitch != t->pitch || u->bpp != 2) continue;
                int64_t off = (int64_t)t->addr - (int64_t)u->addr, dy = off >= 0 ? off / u->pitch : -((-off + u->pitch - 1) / u->pitch);
                int64_t dxb = off - dy * (int64_t)u->pitch;  /* bytes */
                if (dxb & 1 || dxb + (int64_t)wx1 * 4 > (int64_t)u->pitch) continue;
                int ux0 = imax(0, (int)(dxb / 2) + 2 * wx0), ux1 = imin(u->w, (int)(dxb / 2) + 2 * wx1);
                int uy0 = imax(0, (int)dy + wy0), uy1 = imin(u->h, (int)dy + wy1);
                if (ux1 <= ux0 || uy1 <= uy0 || ux1 <= u->ux0 || ux0 >= u->ux1 || uy1 <= u->uy0 || uy0 >= u->uy1) continue;
                record_quad(i, ux0, uy0, ux1, uy1, pixel_in(px, u->fmt), 1ull << 16 | 7ull << 12, 0, 0);
                mark_written(u, ux0, uy0, ux1, uy1);
                g_st.fills++;
            }
        }
    }
}

/* --- submission -------------------------------------------------------------------- */
static void upload_frame(SDL_GPUCommandBuffer *cmd) {
    uint32_t vbytes = (uint32_t)(g_nverts * sizeof(GpuVertex));
    g_voff = (uint32_t)((vbytes + 511) & ~511u);
    uint32_t total = g_voff + (uint32_t)g_upsize;
    if (!total) return;
    if (total > g_tbuf_size) {
        if (g_tbuf) SDL_ReleaseGPUTransferBuffer(g_dev, g_tbuf);
        g_tbuf_size = total + total / 2;
        SDL_GPUTransferBufferCreateInfo ti = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, g_tbuf_size, 0 };
        g_tbuf = SDL_CreateGPUTransferBuffer(g_dev, &ti);
    }
    if (vbytes > g_vbuf_size) {
        if (g_vbuf) SDL_ReleaseGPUBuffer(g_dev, g_vbuf);
        g_vbuf_size = vbytes + vbytes / 2;
        SDL_GPUBufferCreateInfo bi = { SDL_GPU_BUFFERUSAGE_VERTEX, g_vbuf_size, 0 };
        g_vbuf = SDL_CreateGPUBuffer(g_dev, &bi);
    }
    uint8_t *p = SDL_MapGPUTransferBuffer(g_dev, g_tbuf, true);
    if (vbytes) memcpy(p, g_verts, vbytes);
    if (g_upsize) memcpy(p + g_voff, g_upbytes, g_upsize);
    SDL_UnmapGPUTransferBuffer(g_dev, g_tbuf);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    if (vbytes) {
        SDL_GPUTransferBufferLocation src = { g_tbuf, 0 };
        SDL_GPUBufferRegion dst = { g_vbuf, 0, vbytes };
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    }
    for (size_t i = 0; i < g_nups; i++) {
        SDL_GPUTextureTransferInfo src = { g_tbuf, g_voff + g_ups[i].offset, (Uint32)g_ups[i].w, (Uint32)g_ups[i].h };
        SDL_GPUTextureRegion dst;
        memset(&dst, 0, sizeof(dst));
        dst.texture = g_ups[i].tex;
        dst.mip_level = (Uint32)g_ups[i].level;
        dst.w = (Uint32)g_ups[i].w;
        dst.h = (Uint32)g_ups[i].h;
        dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(cp);
}

/* copy commands (snapshots, uploads into targets) run between render passes, in order */
static void run_copy(SDL_GPUCommandBuffer *cmd, const Cmd *c) {
    if (c->w <= 0 || c->h <= 0) return;
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    if (c->kind == CMD_COPY) {
        SDL_GPUTextureLocation s, d;
        memset(&s, 0, sizeof(s));
        memset(&d, 0, sizeof(d));
        s.texture = c->tex; s.x = (Uint32)c->sx; s.y = (Uint32)c->sy;
        d.texture = c->dst; d.x = (Uint32)c->dx; d.y = (Uint32)c->dy;
        SDL_CopyGPUTextureToTexture(cp, &s, &d, (Uint32)c->w, (Uint32)c->h, 1, false);
    } else {
        SDL_GPUTextureTransferInfo src = { g_tbuf, g_voff + c->offset, (Uint32)c->w, (Uint32)c->h };
        SDL_GPUTextureRegion r;
        memset(&r, 0, sizeof(r));
        r.texture = c->dst; r.x = (Uint32)c->dx; r.y = (Uint32)c->dy; r.w = (Uint32)c->w; r.h = (Uint32)c->h; r.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &r, false);
    }
    SDL_EndGPUCopyPass(cp);
}

static void render_frame(SDL_GPUCommandBuffer *cmd) {
    SDL_GPURenderPass *pass = NULL;
    int cur_t = -1, cur_pipe = -1;
    for (size_t i = 0; i < g_ncmds; i++) {
        Cmd *c = &g_cmds[i];
        if (c->kind != CMD_DRAW) {
            if (pass) { SDL_EndGPURenderPass(pass); pass = NULL; cur_t = -1; }
            run_copy(cmd, c);
            continue;
        }
        if (!c->count || !c->scissor.w || !c->scissor.h || !g_pipes[c->pipe].p) continue;
        if (c->target != cur_t) {
            if (pass) SDL_EndGPURenderPass(pass);
            Target *t = &g_targets[c->target];
            SDL_GPUColorTargetInfo ci;
            memset(&ci, 0, sizeof(ci));
            ci.texture = t->tex;
            ci.load_op = SDL_GPU_LOADOP_LOAD;  /* a new target was uploaded from memory first */
            ci.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPUDepthStencilTargetInfo di;
            memset(&di, 0, sizeof(di));
            di.texture = t->ds;
            di.load_op = t->ds_init ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
            di.store_op = SDL_GPU_STOREOP_STORE;
            di.stencil_load_op = t->ds_init ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
            di.stencil_store_op = SDL_GPU_STOREOP_STORE;
            di.clear_depth = 0.0f;
            di.clear_stencil = 0;
            t->ds_init = 1;
            pass = SDL_BeginGPURenderPass(cmd, &ci, 1, &di);
            SDL_GPUViewport vp = { 0, 0, (float)t->w, (float)t->h, 0.0f, 1.0f };
            SDL_SetGPUViewport(pass, &vp);
            SDL_GPUBufferBinding vbind = { g_vbuf, 0 };
            SDL_BindGPUVertexBuffers(pass, 0, &vbind, 1);
            float screen[4] = { 2.0f / t->w, 2.0f / t->h, 0, 0 };
            SDL_PushGPUVertexUniformData(cmd, 0, screen, sizeof(screen));
            cur_t = c->target;
            cur_pipe = -1;
            g_st.passes++;
        }
        if (c->pipe != cur_pipe) { SDL_BindGPUGraphicsPipeline(pass, g_pipes[c->pipe].p); cur_pipe = c->pipe; }
        SDL_SetGPUScissor(pass, &c->scissor);
        SDL_SetGPUBlendConstants(pass, c->blend_const);
        if (c->stencil) SDL_SetGPUStencilReference(pass, (Uint8)c->stencil_ref);
        SDL_GPUTextureSamplerBinding tb = { c->tex ? c->tex : g_white, c->sampler };
        SDL_BindGPUFragmentSamplers(pass, 0, &tb, 1);
        SDL_PushGPUFragmentUniformData(cmd, 0, &c->fu, sizeof(c->fu));
        SDL_DrawGPUPrimitives(pass, c->count, 1, c->first, 0);
    }
    if (pass) SDL_EndGPURenderPass(pass);
}

static void reset_recorded(void) {
    g_nverts = 0;
    g_ncmds = 0;
    g_nups = 0;
    g_upsize = 0;
}

/* Submits the work recorded so far (before a readback in the middle of a frame). */
static void submit_pending(void) {
    if (!g_ncmds && !g_nups) return;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (!cmd) return;
    upload_frame(cmd);
    render_frame(cmd);
    SDL_SubmitGPUCommandBuffer(cmd);
    reset_recorded();
    g_st.submits++;
}

/* Reads w x h pixels at (x, y) of tex as RGBA8, after all work recorded before. */
static int download(SDL_GPUTexture *tex, int x, int y, int w, int h, uint32_t *out) {
    if (w <= 0 || h <= 0) return 0;
    submit_pending();
    uint32_t bytes = (uint32_t)w * (uint32_t)h * 4;
    SDL_GPUTransferBufferCreateInfo ti = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes, 0 };
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(g_dev, &ti);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (!tb || !cmd) return 0;
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src;
    memset(&src, 0, sizeof(src));
    src.texture = tex;
    src.x = (Uint32)x; src.y = (Uint32)y; src.w = (Uint32)w; src.h = (Uint32)h; src.d = 1;
    SDL_GPUTextureTransferInfo dst = { tb, 0, (Uint32)w, (Uint32)h };
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_dev, true, &f, 1);
    SDL_ReleaseGPUFence(g_dev, f);
    const uint8_t *p = SDL_MapGPUTransferBuffer(g_dev, tb, false);
    memcpy(out, p, bytes);
    SDL_UnmapGPUTransferBuffer(g_dev, tb);
    SDL_ReleaseGPUTransferBuffer(g_dev, tb);
    g_st.readbacks++;
    return 1;
}

/* Writes the w x h region at (x, y) of target ti into PSP memory, in the target's format. */
static void write_back(int ti, int x, int y, int w, int h) {
    Target *t = &g_targets[ti];
    w = imin(w, t->w - x);
    h = imin(h, t->h - y);
    if (w <= 0 || h <= 0) return;
    grow((void **)&g_dl, &g_dlcap, (size_t)w * h, sizeof(uint32_t));
    record_resolve(ti);  /* memory gets the stencil in the alpha channel */
    if (!download(t->tex, x, y, w, h, g_dl)) return;
    for (int j = 0; j < h; j++) {
        uint8_t *row = rt_ptr(t->addr + (uint32_t)(y + j) * t->pitch) + (size_t)x * t->bpp;
        for (int i = 0; i < w; i++) pixel_out(row + (size_t)i * t->bpp, g_dl[(size_t)j * w + i], t->fmt);
    }
}

void rt_gpu_transfer_begin(uint32_t src, uint32_t spitch, int sx, int sy, uint32_t dst, uint32_t dpitch, int dx, int dy,
                           int w, int h, int bpp, int readback) {
    g_xfer_gpu = 0;
    if (!g_dev) return;
    int stx = 0, sty = 0, dtx = 0, dty = 0;
    int s = target_for_rect(src, spitch, bpp, sx, sy, w, h, 1, &stx, &sty);
    if (s < 0) return;
    int d = target_for_rect(dst, dpitch, bpp, dx, dy, w, h, 0, &dtx, &dty);
    if (d >= 0) {
        /* through a snapshot: source and destination may overlap */
        SDL_GPUTexture *snap = snapshot(s, stx, sty, w, h);
        if (!snap) return;
        Cmd *c = new_cmd(CMD_COPY);
        c->tex = snap;
        c->dst = g_targets[d].tex;
        c->dx = dtx; c->dy = dty;
        c->w = imin(w, g_targets[d].w - dtx);
        c->h = imin(h, g_targets[d].h - dty);
        mark_written(&g_targets[d], dtx, dty, dtx + w, dty + h);
        g_xfer_gpu = 1;
        g_st.gpu_copies++;
        return;
    }
    if (readback) write_back(s, stx, sty, w, h);  /* the copy in memory then moves what the GPU drew */
}

void rt_gpu_transfer_end(uint32_t dst, uint32_t dpitch, int dx, int dy, int w, int h, int bpp) {
    if (!g_dev || g_xfer_gpu) return;
    int tx = 0, ty = 0, d = target_for_rect(dst, dpitch, bpp, dx, dy, w, h, 0, &tx, &ty);
    if (d < 0) return;
    Target *t = &g_targets[d];
    record_upload(t->tex, tx, ty, w, h, t->addr + (uint32_t)ty * t->pitch + (uint32_t)tx * (uint32_t)t->bpp, t->pitch, t->fmt);
    mark_written(t, tx, ty, tx + w, ty + h);
    t->mem_hash = display_hash(t);
}

void rt_gpu_sync_span(uint32_t addr, uint32_t size) {
    if (!g_dev || !size) return;
    int order[MAX_TARGETS], n = 0;
    for (int i = 0; i < g_ntargets; i++) {
        const Target *t = &g_targets[i];
        if (t->ux1 <= t->ux0 || t->uy1 <= t->uy0) continue;
        uint32_t lo = t->addr + (uint32_t)t->uy0 * t->pitch, hi = t->addr + (uint32_t)t->uy1 * t->pitch;
        if (addr + size <= lo || addr >= hi) continue;
        int k = n++;
        while (k > 0 && g_targets[order[k - 1]].seq > t->seq) { order[k] = order[k - 1]; k--; }
        order[k] = i;  /* oldest first: the most recent write ends up in memory */
    }
    for (int k = 0; k < n; k++) {
        const Target *t = &g_targets[order[k]];
        int y0 = t->uy0, y1 = t->uy1;
        if (addr > t->addr) y0 = imax(y0, (int)((addr - t->addr) / t->pitch));
        y1 = imin(y1, (int)((addr + size - t->addr + t->pitch - 1) / t->pitch));
        write_back(order[k], t->ux0, y0, t->ux1 - t->ux0, y1 - y0);
    }
}

/* the target showing the framebuffer at addr, and where in it */
static int display_target(uint32_t addr, uint32_t stride, uint32_t fmt, int *x, int *y) {
    int bpp = fmt == 3 ? 4 : 2, best = -1;
    uint32_t pitch = stride * (uint32_t)bpp;
    for (int i = 0; i < g_ntargets; i++) {
        int px, py;
        if (!locate(&g_targets[i], addr, pitch, bpp, &px, &py)) continue;
        if (px == 0 && py == 0) { *x = *y = 0; return i; }
        if (best < 0 || g_targets[i].seq > g_targets[best].seq) { best = i; *x = px; *y = py; }
    }
    return best;
}

static void blit_to_window(SDL_GPUCommandBuffer *cmd, int t, int x, int y) {
    SDL_GPUTexture *sc = NULL;
    Uint32 sw = 0, sh = 0;
    uint64_t t0 = SDL_GetTicksNS();
    int ok = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, g_win, &sc, &sw, &sh);
    uint64_t dt = SDL_GetTicksNS() - t0;
    g_st.acquire_ns += dt;
    if (dt > g_st.acquire_max) g_st.acquire_max = dt;
    if (dt > 4000000) g_st.acquire_slow++;
    if (!ok || !sc) return;
    SDL_GPUColorTargetInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.texture = sc;
    ci.load_op = SDL_GPU_LOADOP_CLEAR;
    ci.store_op = SDL_GPU_STOREOP_STORE;
    ci.clear_color.a = 1.0f;
    SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &ci, 1, NULL));
    if (t < 0) return;
    /* letterbox 480x272 into the window */
    float s = (float)sw / DISP_W < (float)sh / DISP_H ? (float)sw / DISP_W : (float)sh / DISP_H;
    Uint32 dw = (Uint32)(DISP_W * s), dh = (Uint32)(DISP_H * s);
    SDL_GPUBlitInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.source.texture = g_targets[t].tex;
    bi.source.x = (Uint32)x;
    bi.source.y = (Uint32)y;
    bi.source.w = (Uint32)imin(DISP_W, g_targets[t].w - x);
    bi.source.h = (Uint32)imin(DISP_H, g_targets[t].h - y);
    bi.destination.texture = sc;
    bi.destination.x = (sw - dw) / 2;
    bi.destination.y = (sh - dh) / 2;
    bi.destination.w = dw;
    bi.destination.h = dh;
    bi.load_op = SDL_GPU_LOADOP_LOAD;
    bi.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cmd, &bi);
}

/* RT_GPU_DUMP=N: after frame N, writes every target's drawn area (gpu) and the same area of PSP
 * memory (mem, filled by the software renderer in "both" mode) as PPM files, for debugging. */
static void write_ppm(const char *path, const uint32_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint8_t c[3] = { (uint8_t)px[i], (uint8_t)(px[i] >> 8), (uint8_t)(px[i] >> 16) }; fwrite(c, 1, 3, f); }
    fclose(f);
}

static void debug_dump(void) {
    static long when = -2;
    if (when == -2) { const char *e = getenv("RT_GPU_DUMP"); when = e ? atol(e) : -1; }
    if (when < 0 || (long)g_frame != when) return;
    for (int i = 0; i < g_ntargets; i++) {
        const Target *t = &g_targets[i];
        int w = t->ux1 - t->ux0, h = t->uy1 - t->uy0;
        if (w <= 0 || h <= 0) continue;
        uint32_t *g = malloc((size_t)w * h * 4), *m = malloc((size_t)w * h * 4);
        if (download(t->tex, t->ux0, t->uy0, w, h, g)) {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    m[(size_t)y * w + x] = pixel_in(rt_ptr(t->addr + (uint32_t)(t->uy0 + y) * t->pitch + (uint32_t)(t->ux0 + x) * t->bpp), t->fmt);
            char path[256];
            snprintf(path, sizeof(path), "gpudump_%08X_%d_gpu.ppm", t->addr, t->fmt);
            write_ppm(path, g, w, h);
            snprintf(path, sizeof(path), "gpudump_%08X_%d_mem.ppm", t->addr, t->fmt);
            write_ppm(path, m, w, h);
            printf("[gpu-dump] frame %llu target %08X fmt %d area %d,%d %dx%d\n", (unsigned long long)g_frame, t->addr, t->fmt, t->ux0, t->uy0, w, h);
        }
        free(g);
        free(m);
    }
}

void rt_gpu_present(uint32_t addr, uint32_t stride, uint32_t fmt) {
    if (!g_dev) return;
    int t = -1, x = 0, y = 0;
    if (addr && stride) {
        t = display_target(addr, stride, fmt, &x, &y);
        if (t < 0) {
            /* never drawn on the GPU: shows what the CPU put in memory */
            int bpp = fmt == 3 ? 4 : 2;
            t = new_target(addr, stride * (uint32_t)bpp, bpp, (int)fmt);
        } else if (g_targets[t].drawn_frame != g_frame) {
            /* nothing drawn on the GPU this frame: if the CPU changed the memory (e.g. a video), show that */
            Target *tg = &g_targets[t];
            uint64_t h = display_hash(tg);
            if (h != tg->mem_hash) {
                tg->mem_hash = h;
                record_upload(tg->tex, 0, 0, imin(DISP_W, tg->w), imin(DISP_H, tg->h), tg->addr, tg->pitch, tg->fmt);
                tg->version++;
            }
        }
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (!cmd) return;
    upload_frame(cmd);
    render_frame(cmd);
    if (g_win) blit_to_window(cmd, t, x, y);
    static int sync = -1;
    if (sync < 0) sync = getenv("RT_GPU_SYNC") != NULL;
    if (sync) {
        /* RT_GPU_SYNC: wait for each frame, measuring how long the GPU takes after submission */
        uint64_t t0 = SDL_GetTicksNS();
        SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        SDL_WaitForGPUFences(g_dev, true, &f, 1);
        SDL_ReleaseGPUFence(g_dev, f);
        uint64_t dt = SDL_GetTicksNS() - t0;
        g_st.gpu_wait_ns += dt;
        if (dt > g_st.gpu_wait_max) g_st.gpu_wait_max = dt;
    } else {
        SDL_SubmitGPUCommandBuffer(cmd);
    }
    reset_recorded();
    debug_dump();
    for (int i = 0; i < g_nsnaps; i++) g_snaps[i].busy = 0;
    g_frame++;
    g_st.frames++;
    evict_textures();
}

int rt_gpu_read_target(uint32_t addr, uint32_t stride, uint32_t fmt, uint8_t *rgba) {
    if (!g_dev) return 0;
    int x = 0, y = 0, t = display_target(addr, stride, fmt, &x, &y);
    if (t < 0) return 0;
    const Target *tg = &g_targets[t];
    int w = imin(DISP_W, tg->w - x), h = imin(DISP_H, tg->h - y);
    grow((void **)&g_dl, &g_dlcap, (size_t)DISP_W * DISP_H, sizeof(uint32_t));
    if (!download(tg->tex, x, y, w, h, g_dl)) return 0;
    memset(rgba, 0, (size_t)DISP_W * DISP_H * 4);
    for (int j = 0; j < h; j++) memcpy(rgba + (size_t)j * DISP_W * 4, g_dl + (size_t)j * w, (size_t)w * 4);
    return 1;
}

void rt_gpu_report(void) {
    if (!g_dev) return;
    printf("[gpu] %llu frames: %llu draws, %llu vertices, %llu render passes, %llu texture uploads (%.1f MB), %llu pipelines\n",
           (unsigned long long)g_st.frames, (unsigned long long)g_st.draws, (unsigned long long)g_st.verts,
           (unsigned long long)g_st.passes, (unsigned long long)g_st.uploads, g_st.upload_bytes / 1048576.0,
           (unsigned long long)g_st.pipelines);
    printf("[gpu] %llu render targets, %llu render-to-texture snapshots, %llu uploads into targets, %llu GPU copies, "
           "%llu readbacks, %llu mid-frame submits, %llu stencil-to-alpha copies, %llu fills across formats\n",
           (unsigned long long)g_st.targets, (unsigned long long)g_st.snapshots, (unsigned long long)g_st.target_uploads,
           (unsigned long long)g_st.gpu_copies, (unsigned long long)g_st.readbacks, (unsigned long long)g_st.submits,
           (unsigned long long)g_st.resolves, (unsigned long long)g_st.fills);
    if (g_st.gpu_wait_ns)
        printf("[gpu] waiting for the GPU after each frame: %.2f ms on average, %.2f ms at most\n",
               g_st.gpu_wait_ns / 1e6 / (double)(g_st.frames ? g_st.frames : 1), g_st.gpu_wait_max / 1e6);
    if (g_win)
        printf("[gpu] swapchain acquire: %.2f ms on average, %.2f ms at most, %llu frames over 4 ms\n",
               g_st.acquire_ns / 1e6 / (double)(g_st.frames ? g_st.frames : 1), g_st.acquire_max / 1e6,
               (unsigned long long)g_st.acquire_slow);
    if (getenv("RT_GPU_TRACE_RTT"))
        for (int i = 0; i < g_ntargets; i++) {
            const Target *t = &g_targets[i];
            printf("[gpu]   target %08X pitch %u fmt %d %dx%d drawn x %d..%d y %d..%d\n", t->addr, t->pitch, t->fmt, t->w, t->h,
                   t->ux0, t->ux1, t->uy0, t->uy1);
        }
}

void rt_gpu_shutdown(void) {
    if (!g_dev) return;
    SDL_WaitForGPUIdle(g_dev);
    for (size_t i = 0; i < TEX_SLOTS; i++) if (g_tex[i].key) SDL_ReleaseGPUTexture(g_dev, g_tex[i].tex);
    for (int i = 0; i < g_ntargets; i++) {
        SDL_ReleaseGPUTexture(g_dev, g_targets[i].tex);
        SDL_ReleaseGPUTexture(g_dev, g_targets[i].ds);
    }
    for (int i = 0; i < g_nsnaps; i++) SDL_ReleaseGPUTexture(g_dev, g_snaps[i].tex);
    for (int i = 0; i < g_npipes; i++) if (g_pipes[i].p) SDL_ReleaseGPUGraphicsPipeline(g_dev, g_pipes[i].p);
    for (int i = 0; i < g_nsamps; i++) SDL_ReleaseGPUSampler(g_dev, g_samps[i].s);
    g_nsamps = g_nsnaps = g_ntargets = 0;
    SDL_ReleaseGPUTexture(g_dev, g_white);
    if (g_vbuf) SDL_ReleaseGPUBuffer(g_dev, g_vbuf);
    if (g_tbuf) SDL_ReleaseGPUTransferBuffer(g_dev, g_tbuf);
    SDL_ReleaseGPUShader(g_dev, g_vs);
    SDL_ReleaseGPUShader(g_dev, g_fs);
    if (g_win) SDL_ReleaseWindowFromGPUDevice(g_dev, g_win);
    SDL_DestroyGPUDevice(g_dev);
    g_dev = NULL;
}

#else  /* no SDL: no hardware renderer */
int rt_gpu_init(void *window, const char *driver) { (void)window; (void)driver; fprintf(stderr, "[gpu] built without SDL3\n"); return 0; }
int rt_gpu_active(void) { return 0; }
void rt_gpu_shutdown(void) {}
int rt_gpu_texture_resident(uint64_t key) { (void)key; return 1; }
void rt_gpu_texture_upload(uint64_t key, int w, int h, int levels, const uint32_t *rgba) { (void)key; (void)w; (void)h; (void)levels; (void)rgba; }
int rt_gpu_texture_is_target(uint32_t addr, uint32_t pitch, int bpp) { (void)addr; (void)pitch; (void)bpp; return 0; }
void rt_gpu_draw(const GpuDraw *d, const GpuVertex *v, uint32_t n) { (void)d; (void)v; (void)n; }
void rt_gpu_transfer_begin(uint32_t src, uint32_t spitch, int sx, int sy, uint32_t dst, uint32_t dpitch, int dx, int dy,
                           int w, int h, int bpp, int readback) {
    (void)src; (void)spitch; (void)sx; (void)sy; (void)dst; (void)dpitch; (void)dx; (void)dy; (void)w; (void)h; (void)bpp; (void)readback;
}
void rt_gpu_transfer_end(uint32_t dst, uint32_t dpitch, int dx, int dy, int w, int h, int bpp) {
    (void)dst; (void)dpitch; (void)dx; (void)dy; (void)w; (void)h; (void)bpp;
}
void rt_gpu_sync_span(uint32_t addr, uint32_t size) { (void)addr; (void)size; }
void rt_gpu_present(uint32_t addr, uint32_t stride, uint32_t fmt) { (void)addr; (void)stride; (void)fmt; }
int rt_gpu_read_target(uint32_t addr, uint32_t stride, uint32_t fmt, uint8_t *rgba) { (void)addr; (void)stride; (void)fmt; (void)rgba; return 0; }
void rt_gpu_report(void) {}
void rt_gpu_debug_compare_texture(uint32_t addr, uint32_t pitch, int bpp, int fmt, int w, int h) {
    (void)addr; (void)pitch; (void)bpp; (void)fmt; (void)w; (void)h;
}
#endif
