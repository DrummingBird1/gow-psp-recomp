/* Hardware renderer on SDL3 GPU (Vulkan / Direct3D 12).
 *
 * rt_gesoft.c decodes, transforms, lights, clips and assembles the primitives exactly as for
 * the software renderer and hands finished triangles (PSP screen space) to rt_gpu_draw. Work
 * is recorded during the frame and submitted at the flip: one copy pass (vertices, new
 * textures), then the recorded commands in order (render passes, and copy passes for
 * render-to-texture snapshots and memory uploads), then the displayed framebuffer is drawn
 * into the window (runtime/shaders/post.hlsl).
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
 * Graphics options (docs/phase13-graphics.md): a target's textures can be a multiple of its
 * size in PSP pixels (--res-scale). Everything recorded stays in PSP pixels (vertices, scissors,
 * rectangles) and is scaled when it runs: the viewport, scissors, snapshots and copies; memory
 * uploaded into a scaled target goes through a 1x staging texture that is scaled into it, and
 * the CPU reads a target back at 1x (point-sampled data, or box-filtered pictures). With
 * multisampling (--msaa) a target draws into a multisampled colour texture that is resolved into
 * its colour texture only when something reads that before the next pass (planned per frame,
 * plan_resolves); copies and uploads into the colour texture are copied back into the
 * multisampled one (unresolve).
 *
 * Not yet: CLUT textures indexing a target, logic ops, double blend factors. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psp_rt.h"
#include "rt_frames.h"
#include "rt_gpu.h"

GpuOptions rt_gpu_opt;

/* --res-scale N and the other graphics options (options parsing for rt_main.c) */
int rt_gpu_option(int argc, char **argv, int *i) {
    const char *a = argv[*i], *v = *i + 1 < argc ? argv[*i + 1] : NULL;
    GpuOptions *o = &rt_gpu_opt;
    if (!strcmp(a, "--fxaa")) { o->fxaa = 1; return 1; }
    if (!strcmp(a, "--vsync")) { o->vsync = 1; return 1; }
    if (!strcmp(a, "--hdr")) {
        o->hdr = 1;
        if (v && (!strcmp(v, "scrgb") || !strcmp(v, "hdr10") || !strcmp(v, "auto"))) {
            o->hdr = !strcmp(v, "scrgb") ? 2 : !strcmp(v, "hdr10") ? 3 : 1;
            (*i)++;
        }
        return 1;
    }
    if (!v) return 0;
    if (!strcmp(a, "--res-scale")) o->res_scale = atoi(v);
    else if (!strcmp(a, "--fx-scale")) o->fx_scale = atoi(v);
    else if (!strcmp(a, "--msaa")) o->msaa = atoi(v);
    else if (!strcmp(a, "--aniso")) o->aniso = atoi(v);
    else if (!strcmp(a, "--lod-bias")) o->lod_bias = (float)atof(v);
    else if (!strcmp(a, "--paper-white")) o->paper_white = (float)atof(v);
    else if (!strcmp(a, "--peak-nits")) o->peak_nits = (float)atof(v);
    else if (!strcmp(a, "--sharpen")) o->sharpen = (float)atof(v);
    else if (!strcmp(a, "--fps")) o->fps = (float)atof(v);
    else if (!strcmp(a, "--texture-dump")) o->texture_dump = v;
    else if (!strcmp(a, "--texture-pack")) o->texture_pack = v;
    else if (!strcmp(a, "--aspect")) {  /* W:H or a number */
        const char *colon = strchr(v, ':');
        double w = atof(v), h = colon ? atof(colon + 1) : 1.0;
        o->aspect = h > 0 && w / h >= 1.0 && w / h <= 4.0 ? (float)(w / h) : 0.0f;
    }
    else return 0;
    (*i)++;
    return 1;
}

#ifdef GOW_HAVE_SDL
#include <SDL3/SDL.h>

#include "gpu_shaders.h"

#define DISP_W 480
#define DISP_H 272
#define MAX_TARGETS 64
#define MAX_PIPELINES 512
#define MAX_SAMPLERS 1024
#define MAX_SNAPS 512
#define TEX_SLOTS 16384  /* power of two */
#define F_SRCFIX 128u
#define F_TEXOPAQUE 256u
#define VRAM_LO 0x04000000u
#define VRAM_HI 0x04200000u

typedef struct {
    uint32_t addr, pitch;      /* base address, bytes per row */
    int bpp, fmt, w, h;        /* bytes per pixel, PSP pixel format, size in pixels */
    int s;                     /* scale: the textures are w*s x h*s */
    int ux0, uy0, ux1, uy1;    /* area drawn (or uploaded by transfers): [ux0, ux1) x [uy0, uy1) */
    SDL_GPUTexture *tex, *ds;  /* colour (resolved, when multisampled), depth-stencil */
    SDL_GPUTexture *ms;        /* multisampled colour, or NULL */
    int ds_init;
    uint64_t seq;              /* order of the latest write (the most recent target wins) */
    uint32_t version;          /* changes with every write (snapshot validity) */
    uint64_t drawn_frame;      /* frame of the latest GPU draw */
    uint64_t mem_hash;         /* display area of PSP memory when last uploaded */
    int stencil_dirty;         /* stencil written since its values were last copied into the alpha channel */
    int want_s;                /* scale to change to after the frame (0: none) */
} Target;
typedef struct { uint64_t key; SDL_GPUTexture *tex; int w, h; uint64_t used; int levels, replaced, lod_shift; } TexEntry;
typedef struct { uint64_t key; SDL_GPUGraphicsPipeline *p; } Pipe;
typedef struct { uint32_t key; SDL_GPUSampler *s; } Samp;
/* pooled textures: render-to-texture snapshots, staging for scaled uploads */
typedef struct { SDL_GPUTexture *tex; int w, h, busy, target, x, y; uint32_t version; uint64_t used; } Snap;

/* layout of FragmentUniforms in ge.hlsl */
typedef struct { uint32_t mode[4]; float env[4], fogcol[4], fixa[4], zrange[4]; } FragU;
/* layout of PostUniforms in post.hlsl */
typedef struct { float src[4], size[4]; uint32_t mode[4]; float p0[4], p1[4]; } PostU;
enum { P_COPY, P_BOX, P_FXAA, P_CAS, P_PRESENT };

enum { CMD_DRAW, CMD_COPY, CMD_UPLOAD, CMD_BLIT, CMD_UNRESOLVE };
typedef struct {
    int kind, target, pipe;
    int src_target;                /* copy, blit: the target whose colour texture is read, or -1 */
    int resolve;                   /* first draw of a pass (multisampling): resolve when the pass ends */
    SDL_GPUSampler *sampler;
    SDL_Rect scissor;              /* draw, unresolve: in PSP pixels of the target */
    SDL_FColor blend_const;
    SDL_GPUTexture *tex;           /* draw: texture; copy, blit: source */
    SDL_GPUTexture *dst;           /* copy, upload, blit: destination */
    int sx, sy, sw, sh, dx, dy, w, h;  /* copy, upload, blit: rectangles in texels */
    SDL_GPUFilter filter;          /* blit */
    uint32_t first, count, offset; /* draw: vertices; upload: staged bytes */
    int stencil, stencil_ref;
    FragU fu;
} Cmd;

typedef struct { SDL_GPUTexture *tex; uint32_t offset; int w, h, level; } Upload;

static SDL_GPUDevice *g_dev;
static SDL_Window *g_win;
static SDL_GPUShader *g_vs, *g_fs, *g_post_vs, *g_post_fs;
static SDL_GPUTextureFormat g_dsfmt, g_swfmt;
static SDL_GPUSampleCount g_sc = SDL_GPU_SAMPLECOUNT_1;
static int g_scale = 1, g_fx_scale = 1, g_samples = 1;
static int g_hdr_out;  /* 0 SDR, 1 scRGB, 2 HDR10 */
static float g_paper = 200.0f, g_peak = 1000.0f;
static Samp g_samps[MAX_SAMPLERS];
static int g_nsamps;
static int g_samp_overflow, g_samp_failed, g_aniso_failed;  /* sampler problems, reported once and at exit */
static SDL_GPUSampler *g_point, *g_linear;  /* full-screen passes */
static SDL_GPUTexture *g_white;  /* bound for untextured draws */
static Target g_targets[MAX_TARGETS];
static int g_ntargets;
static uint64_t g_seq;
static Snap g_snaps[MAX_SNAPS];
static int g_nsnaps;
static Pipe g_pipes[MAX_PIPELINES];
static int g_npipes;
static struct { SDL_GPUTextureFormat fmt; int samples; SDL_GPUGraphicsPipeline *p; } g_ppipes[8];
static int g_nppipes;
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
static SDL_GPUTexture *g_rb, *g_post[2];  /* 1x readback staging; post-processing at the internal resolution */
static int g_rb_w, g_rb_h, g_post_w, g_post_h;
static SDL_GPUTexture *g_release[64];     /* released once the frame that still uses them is submitted */
static int g_nrelease;
static uint32_t g_disp[16];               /* framebuffer addresses shown so far */
static int g_ndisp;
static uint64_t g_frame;
static int g_xfer_gpu;  /* the block transfer in progress was made on the GPU */
static struct {
    uint64_t frames, draws, merged, verts, passes, uploads, upload_bytes, pipelines, targets, snapshots, target_uploads, gpu_copies,
        readbacks, submits, resolves, fills, gpu_wait_ns, gpu_wait_max, acquire_ns, acquire_max, acquire_slow, msaa_resolves,
        unresolves, blits, snap_bytes, dumped, replaced, pack_loads;
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
int rt_gpu_scale(void) { return g_dev ? g_scale : 1; }
int rt_gpu_capture_full(void) { return g_dev && (g_scale > 1 || rt_gpu_opt.fxaa || rt_gpu_opt.sharpen > 0); }

/* --aspect: the window shows the 480x272 picture stretched to W:H, so the 3D view is drawn that
 * much narrower (a wider field of view: rt_gesoft.c scales clip x), and the HUD is drawn narrower
 * around the centre to keep its shape (pillarboxed, rt_gpu_draw) */
float rt_gpu_aspect_squeeze(void) {
    float a = rt_gpu_opt.aspect;
    return g_dev && a > 0 && (a < 1.76f || a > 1.77f) ? ((float)DISP_W / DISP_H) / a : 1.0f;
}

int rt_gpu_shown_fb(uint32_t addr) {
    for (int i = 0; i < g_ndisp; i++) if (g_disp[i] == addr) return 1;
    return 0;
}

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
static SDL_GPUShader *make_shader(SDL_GPUShaderFormat fmt, SDL_GPUShaderStage stage, const unsigned char *spv, size_t nspv,
                                  const unsigned char *dxil, size_t ndxil, int samplers, int uniforms) {
    SDL_GPUShaderCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.code = fmt == SDL_GPU_SHADERFORMAT_SPIRV ? spv : dxil;
    ci.code_size = fmt == SDL_GPU_SHADERFORMAT_SPIRV ? nspv : ndxil;
    ci.entrypoint = stage == SDL_GPU_SHADERSTAGE_VERTEX ? "vs_main" : "fs_main";
    ci.format = fmt;
    ci.stage = stage;
    ci.num_samplers = (Uint32)samplers;
    ci.num_uniform_buffers = (Uint32)uniforms;
    SDL_GPUShader *s = SDL_CreateGPUShader(g_dev, &ci);
    if (!s) fprintf(stderr, "[gpu] shader: %s\n", SDL_GetError());
    return s;
}

static SDL_GPUTexture *make_texture(SDL_GPUTextureFormat fmt, SDL_GPUTextureUsageFlags usage, int w, int h, int levels,
                                    SDL_GPUSampleCount sc) {
    SDL_GPUTextureCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = fmt;
    ci.usage = usage;
    ci.width = (Uint32)w;
    ci.height = (Uint32)h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = (Uint32)levels;
    ci.sample_count = sc;
    return SDL_CreateGPUTexture(g_dev, &ci);
}

static SDL_GPUSampler *make_sampler(SDL_GPUFilter f) {
    SDL_GPUSamplerCreateInfo si;
    memset(&si, 0, sizeof(si));
    si.min_filter = si.mag_filter = f;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    return SDL_CreateGPUSampler(g_dev, &si);
}

/* swapchain: present mode, and HDR when asked for and the display shows it */
static void setup_swapchain(void) {
    SDL_GPUPresentMode pm = rt_gpu_opt.vsync ? SDL_GPU_PRESENTMODE_VSYNC
                          : SDL_WindowSupportsGPUPresentMode(g_dev, g_win, SDL_GPU_PRESENTMODE_MAILBOX) ? SDL_GPU_PRESENTMODE_MAILBOX
                          : SDL_WindowSupportsGPUPresentMode(g_dev, g_win, SDL_GPU_PRESENTMODE_IMMEDIATE) ? SDL_GPU_PRESENTMODE_IMMEDIATE
                          : SDL_GPU_PRESENTMODE_VSYNC;
    SDL_PropertiesID props = SDL_GetWindowProperties(g_win);
    int hdr_on = SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
    float white = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
    float headroom = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
    int can10 = SDL_WindowSupportsGPUSwapchainComposition(g_dev, g_win, SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084);
    int canx = SDL_WindowSupportsGPUSwapchainComposition(g_dev, g_win, SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR);
    printf("[gpu] display: HDR %s (SDR white %.0f nits, headroom %.2fx); swapchains: HDR10 %s, scRGB %s\n", hdr_on ? "on" : "off",
           white * 80.0f, headroom, can10 ? "yes" : "no", canx ? "yes" : "no");
    SDL_GPUSwapchainComposition comp = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    g_hdr_out = 0;
    if (rt_gpu_opt.hdr) {
        int want10 = rt_gpu_opt.hdr == 3 || rt_gpu_opt.hdr == 1, wantx = rt_gpu_opt.hdr == 2 || rt_gpu_opt.hdr == 1;
        if (want10 && can10) { comp = SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084; g_hdr_out = 2; }
        else if (wantx && canx) { comp = SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR; g_hdr_out = 1; }
        else printf("[gpu] HDR output is not available on this display (is HDR on in the system's display settings?): SDR\n");
    }
    if (!SDL_SetGPUSwapchainParameters(g_dev, g_win, comp, pm)) {
        fprintf(stderr, "[gpu] swapchain: %s; SDR\n", SDL_GetError());
        g_hdr_out = 0;
        SDL_SetGPUSwapchainParameters(g_dev, g_win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, pm);
    }
    g_swfmt = SDL_GetGPUSwapchainTextureFormat(g_dev, g_win);
    g_paper = rt_gpu_opt.paper_white > 0 ? rt_gpu_opt.paper_white : hdr_on && white > 1.0f ? white * 80.0f : 200.0f;
    g_peak = rt_gpu_opt.peak_nits > 0 ? rt_gpu_opt.peak_nits : hdr_on && headroom > 1.0f ? white * 80.0f * headroom : 1000.0f;
    if (g_peak < g_paper) g_peak = g_paper;
    printf("[gpu] present mode: %s, output %s", pm == SDL_GPU_PRESENTMODE_MAILBOX ? "mailbox" : pm == SDL_GPU_PRESENTMODE_IMMEDIATE ? "immediate" : "vsync",
           g_hdr_out == 2 ? "HDR10 (ST 2084)" : g_hdr_out == 1 ? "scRGB" : "SDR");
    if (g_hdr_out) printf(" (paper white %.0f nits, peak %.0f nits)", g_paper, g_peak);
    printf("\n");
}

static void pack_index(void);

int rt_gpu_init(void *window, const char *driver) {
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "[gpu] SDL_Init: %s\n", SDL_GetError()); return 0; }
    g_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL, getenv("RT_GPU_DEBUG") != NULL, driver);
    if (!g_dev) { fprintf(stderr, "[gpu] SDL_CreateGPUDevice: %s\n", SDL_GetError()); return 0; }
    SDL_GPUShaderFormat fmts = SDL_GetGPUShaderFormats(g_dev);
    SDL_GPUShaderFormat use = (fmts & SDL_GPU_SHADERFORMAT_SPIRV) ? SDL_GPU_SHADERFORMAT_SPIRV : SDL_GPU_SHADERFORMAT_DXIL;
    g_vs = make_shader(use, SDL_GPU_SHADERSTAGE_VERTEX, ge_vs_spirv, sizeof(ge_vs_spirv), ge_vs_dxil, sizeof(ge_vs_dxil), 0, 1);
    g_fs = make_shader(use, SDL_GPU_SHADERSTAGE_FRAGMENT, ge_fs_spirv, sizeof(ge_fs_spirv), ge_fs_dxil, sizeof(ge_fs_dxil), 1, 1);
    g_post_vs = make_shader(use, SDL_GPU_SHADERSTAGE_VERTEX, post_vs_spirv, sizeof(post_vs_spirv), post_vs_dxil, sizeof(post_vs_dxil), 0, 0);
    g_post_fs = make_shader(use, SDL_GPU_SHADERSTAGE_FRAGMENT, post_fs_spirv, sizeof(post_fs_spirv), post_fs_dxil, sizeof(post_fs_dxil), 1, 1);
    if (!g_vs || !g_fs || !g_post_vs || !g_post_fs) return 0;
    g_dsfmt = SDL_GPUTextureSupportsFormat(g_dev, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
                                           SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                  ? SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT
                  : SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    /* graphics options */
    GpuOptions *o = &rt_gpu_opt;
    g_scale = o->res_scale < 1 ? 1 : o->res_scale > 8 ? 8 : o->res_scale;
    g_fx_scale = o->fx_scale < 1 ? g_scale : o->fx_scale > 8 ? 8 : o->fx_scale;
    g_samples = 1;
    for (int n = 8; n > 1; n /= 2) {
        SDL_GPUSampleCount sc = n == 8 ? SDL_GPU_SAMPLECOUNT_8 : n == 4 ? SDL_GPU_SAMPLECOUNT_4 : SDL_GPU_SAMPLECOUNT_2;
        if (o->msaa >= n && SDL_GPUTextureSupportsSampleCount(g_dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, sc) &&
            SDL_GPUTextureSupportsSampleCount(g_dev, g_dsfmt, sc)) {
            g_samples = n;
            g_sc = sc;
            break;
        }
    }
    if (o->msaa > 1 && g_samples != o->msaa) printf("[gpu] %dx MSAA is not supported here: %dx\n", o->msaa, g_samples);
    g_point = make_sampler(SDL_GPU_FILTER_NEAREST);
    g_linear = make_sampler(SDL_GPU_FILTER_LINEAR);
    g_tex = calloc(TEX_SLOTS, sizeof(TexEntry));
    {
        g_white = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, 1, 1, 1, SDL_GPU_SAMPLECOUNT_1);
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
        setup_swapchain();
    }
    pack_index();
    printf("[gpu] SDL GPU device: %s, %s shaders%s\n", SDL_GetGPUDeviceDriver(g_dev),
           use == SDL_GPU_SHADERFORMAT_SPIRV ? "SPIR-V" : "DXIL", window ? "" : " (headless)");
    {
        SDL_PropertiesID p = SDL_GetGPUDeviceProperties(g_dev);  /* for bug reports: which GPU and driver */
        printf("[gpu] adapter: %s; driver: %s %s\n", SDL_GetStringProperty(p, SDL_PROP_GPU_DEVICE_NAME_STRING, "?"),
               SDL_GetStringProperty(p, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, ""),
               SDL_GetStringProperty(p, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, ""));
        fflush(stdout);
    }
    if (g_scale > 1 || g_samples > 1 || o->aniso > 1 || o->lod_bias != 0 || o->fxaa || o->sharpen > 0)
        printf("[gpu] internal resolution %dx (%dx%d), effects %dx, MSAA %dx, anisotropic filtering %dx, LOD bias %+.2f, FXAA %s, "
               "sharpening %.2f\n",
               g_scale, DISP_W * g_scale, DISP_H * g_scale, g_fx_scale, g_samples, o->aniso > 1 ? o->aniso : 1, o->lod_bias,
               o->fxaa ? "on" : "off", o->sharpen);
    return 1;
}

static Cmd *new_cmd(int kind) {
    grow((void **)&g_cmds, &g_ccap, g_ncmds + 1, sizeof(Cmd));
    Cmd *c = &g_cmds[g_ncmds++];
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->src_target = -1;
    return c;
}

/* A pooled texture, free again after the frame: exactly w x h, or (exact = 0) at least that big. */
static Snap *pool_get(int w, int h, int exact) {
    Snap *best = NULL;
    for (int i = 0; i < g_nsnaps; i++) {
        Snap *s = &g_snaps[i];
        if (s->busy || s->w < w || s->h < h || (exact && (s->w != w || s->h != h))) continue;
        if (!best || (int64_t)s->w * s->h < (int64_t)best->w * best->h) best = s;
    }
    if (!best) {
        if (g_nsnaps == MAX_SNAPS) return NULL;
        SDL_GPUTexture *tex = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                           SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, w, h, 1, SDL_GPU_SAMPLECOUNT_1);
        if (!tex) return NULL;
        best = &g_snaps[g_nsnaps++];
        memset(best, 0, sizeof(*best));
        best->tex = tex;
        best->w = w;
        best->h = h;
        g_st.snap_bytes += (uint64_t)w * h * 4;
    }
    best->busy = 1;
    best->target = -1;
    best->used = g_frame;
    return best;
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

static void record_blit(SDL_GPUTexture *src, int src_target, int sx, int sy, int sw, int sh, SDL_GPUTexture *dst, int dx, int dy, int dw,
                        int dh, SDL_GPUFilter filter) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    Cmd *c = new_cmd(CMD_BLIT);
    c->tex = src;
    c->src_target = src_target;
    c->sx = sx; c->sy = sy; c->sw = sw; c->sh = sh;
    c->dst = dst;
    c->dx = dx; c->dy = dy; c->w = dw; c->h = dh;
    c->filter = filter;
    g_st.blits++;
}

/* multisampling: the rectangle (PSP pixels) of target ti's colour texture was written by a copy or
 * an upload; its multisampled colour takes it from there */
static void unresolve(int ti, int x, int y, int w, int h) {
    if (!g_targets[ti].ms || w <= 0 || h <= 0) return;
    Cmd *c = new_cmd(CMD_UNRESOLVE);
    c->target = ti;
    c->scissor.x = x; c->scissor.y = y; c->scissor.w = w; c->scissor.h = h;
    g_st.unresolves++;
}

/* PSP memory into rectangle (x, y, w, h) of target ti (PSP pixels): at scale 1 uploaded as is,
 * otherwise uploaded to a staging texture that is scaled into the target (point sampling keeps
 * data exact; linear filtering suits pictures such as video frames). */
static void upload_to_target(int ti, int x, int y, int w, int h, uint32_t addr, uint32_t pitch, int fmt, SDL_GPUFilter filter) {
    Target *t = &g_targets[ti];
    if (w <= 0 || h <= 0) return;
    if (t->s == 1) {
        record_upload(t->tex, x, y, w, h, addr, pitch, fmt);
    } else {
        Snap *st = pool_get(w, h, 0);
        if (!st) return;
        record_upload(st->tex, 0, 0, w, h, addr, pitch, fmt);
        record_blit(st->tex, -1, 0, 0, w, h, t->tex, x * t->s, y * t->s, w * t->s, h * t->s, filter);
    }
    unresolve(ti, x, y, w, h);
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

static int target_textures(Target *t) {
    int pw = t->w * t->s, ph = t->h * t->s;
    t->tex = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, pw, ph, 1,
                          SDL_GPU_SAMPLECOUNT_1);
    t->ds = make_texture(g_dsfmt, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, pw, ph, 1, g_sc);
    t->ms = g_samples > 1 ? make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, pw, ph, 1, g_sc) : NULL;
    t->ds_init = 0;
    if (!t->tex || !t->ds || (g_samples > 1 && !t->ms)) {
        fprintf(stderr, "[gpu] render target %08X (%dx%d): %s\n", t->addr, pw, ph, SDL_GetError());
        return 0;
    }
    return 1;
}

/* The scale of a new target: the effects scale (--fx-scale) unless it is a framebuffer the game shows */
static int target_scale(uint32_t addr) {
    if (g_fx_scale == g_scale) return g_scale;
    for (int i = 0; i < g_ndisp; i++) if (g_disp[i] == addr) return g_scale;
    return g_fx_scale;
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
    t->s = target_scale(addr);
    t->ux0 = t->uy0 = 1 << 30;
    if (!target_textures(t)) return 0;
    /* start from what is in memory: an image the CPU or a transfer put there before any draw */
    upload_to_target(g_ntargets, 0, 0, t->w, t->h, addr, pitch, fmt, SDL_GPU_FILTER_NEAREST);
    t->mem_hash = display_hash(t);
    g_st.targets++;
    return g_ntargets++;
}

/* Changes target ti to scale s between frames (a framebuffer made at the effects scale turned out
 * to be shown): new textures, the old picture scaled into them, a new depth-stencil buffer. */
static void rescale_target(int ti) {
    Target *t = &g_targets[ti];
    int s = t->want_s, os = t->s;
    t->want_s = 0;
    if (!s || s == os || g_nrelease + 3 > (int)(sizeof(g_release) / sizeof(g_release[0]))) return;
    SDL_GPUTexture *old[3] = { t->tex, t->ds, t->ms };
    t->s = s;
    if (!target_textures(t)) {
        t->s = os;
        t->tex = old[0]; t->ds = old[1]; t->ms = old[2];
        return;
    }
    for (int i = 0; i < 3; i++) if (old[i]) g_release[g_nrelease++] = old[i];
    record_blit(old[0], -1, 0, 0, t->w * os, t->h * os, t->tex, 0, 0, t->w * s, t->h * s, SDL_GPU_FILTER_LINEAR);
    unresolve(ti, 0, 0, t->w, t->h);
    t->version++;
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

enum { DL_POINT, DL_BOX };
static int download(int ti, int x, int y, int w, int h, uint32_t *out, int mode);
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
    if (!g || !download(ti, x, y, w, h, g, DL_POINT)) { free(g); return; }
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

/* A copy of the w x h region at (x, y) of target ti (PSP pixels) as it is at this point of the
 * frame, at the target's scale. */
static SDL_GPUTexture *snapshot(int ti, int x, int y, int w, int h) {
    Target *t = &g_targets[ti];
    int s = t->s;
    record_resolve(ti);
    for (int i = 0; i < g_nsnaps; i++) {
        const Snap *p = &g_snaps[i];
        if (p->busy && p->target == ti && p->version == t->version && p->x == x && p->y == y && p->w == w * s && p->h == h * s) return p->tex;
    }
    Snap *p = pool_get(w * s, h * s, 1);
    if (!p) return NULL;
    p->target = ti;
    p->version = t->version;
    p->x = x;
    p->y = y;
    Cmd *c = new_cmd(CMD_COPY);
    c->tex = t->tex;
    c->src_target = ti;
    c->sx = x * s; c->sy = y * s;
    c->dst = p->tex;
    c->w = imin(w, t->w - x) * s;
    c->h = imin(h, t->h - y) * s;
    g_st.snapshots++;
    return p->tex;
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
 *      stencil test 1 | func 3 | fail 3 | zfail 3 | zpass 3 | compare mask 8 | write mask 8 | alpha add 1
 * (every pipeline has the targets' sample count) */
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
    ci.multisample_state.sample_count = g_sc;
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

/* the full-screen passes of post.hlsl, for a colour format and sample count */
static SDL_GPUGraphicsPipeline *post_pipeline(SDL_GPUTextureFormat fmt, SDL_GPUSampleCount sc) {
    for (int i = 0; i < g_nppipes; i++) if (g_ppipes[i].fmt == fmt && g_ppipes[i].samples == (int)sc) return g_ppipes[i].p;
    if (g_nppipes == (int)(sizeof(g_ppipes) / sizeof(g_ppipes[0]))) return NULL;
    SDL_GPUColorTargetDescription ct;
    memset(&ct, 0, sizeof(ct));
    ct.format = fmt;
    SDL_GPUGraphicsPipelineCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.vertex_shader = g_post_vs;
    ci.fragment_shader = g_post_fs;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    ci.multisample_state.sample_count = sc;
    ci.target_info.color_target_descriptions = &ct;
    ci.target_info.num_color_targets = 1;
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(g_dev, &ci);
    if (!p) fprintf(stderr, "[gpu] post pipeline: %s\n", SDL_GetError());
    g_ppipes[g_nppipes].fmt = fmt;
    g_ppipes[g_nppipes].samples = (int)sc;
    g_ppipes[g_nppipes++].p = p;
    return p;
}

/* One full-screen pass of post.hlsl into viewport vp of dst: u->src is the source rectangle in
 * texels of src (tw x th). */
static void post_pass(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *dst, SDL_GPUTextureFormat fmt, SDL_GPUSampleCount sc, SDL_GPULoadOp load,
                      const SDL_GPUViewport *vp, const SDL_Rect *scissor, SDL_GPUTexture *src, int tw, int th, PostU *u, int linear) {
    SDL_GPUGraphicsPipeline *p = post_pipeline(fmt, sc);
    if (!p) return;
    SDL_GPUColorTargetInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.texture = dst;
    ci.load_op = load;
    ci.store_op = SDL_GPU_STOREOP_STORE;
    ci.clear_color.a = 1.0f;
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ci, 1, NULL);
    SDL_BindGPUGraphicsPipeline(rp, p);
    SDL_SetGPUViewport(rp, vp);
    if (scissor) SDL_SetGPUScissor(rp, scissor);
    u->size[0] = (float)tw; u->size[1] = (float)th; u->size[2] = 1.0f / tw; u->size[3] = 1.0f / th;
    SDL_GPUTextureSamplerBinding tb = { src, linear ? g_linear : g_point };
    SDL_BindGPUFragmentSamplers(rp, 0, &tb, 1);
    SDL_PushGPUFragmentUniformData(cmd, 0, u, sizeof(*u));
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
}

/* --- samplers -------------------------------------------------------------------- */
/* PSP texture filtering: the level of detail is log2(texels per pixel) plus an offset in
 * 1/16 levels (LOD mode 0); minf above level 0, magf at or below it; mipf blends levels.
 * Mode 2 uses the offset as a constant level. Mode 1 (slope from w) is approximated by mode 0.
 * Options: --lod-bias adds to the mip level; --aniso filters 3D textures anisotropically (linear
 * filtering only); at a higher internal resolution, sprites drawing texels 1:1 keep point
 * sampling, so that they look as at 1x and do not take in the texels next to them. */
static SDL_GPUSampler *sampler_for(const GpuDraw *d) {
    int levels = d->textured && d->levels > 1 ? d->levels : 1;
    int nearest = g_scale > 1 && d->pixel_mapped && !d->tex_target;
    /* anisotropy only with linear filtering (some drivers fail on anisotropic point sampling), and
     * not after a driver refused an anisotropic sampler once (g_aniso_failed) */
    int aniso = rt_gpu_opt.aniso > 1 && !g_aniso_failed && d->textured && !d->through && !d->tex_target && d->minf && d->magf && !nearest;
    uint32_t key = (uint32_t)(d->minf & 1) | (uint32_t)(d->magf & 1) << 1 | (uint32_t)(d->mipf & 1) << 2 |
                   (uint32_t)(d->clampS & 1) << 3 | (uint32_t)(d->clampT & 1) << 4 | (uint32_t)(d->lodmode & 3) << 5 |
                   (uint32_t)(d->lodoff & 0x3FF) << 7 | (uint32_t)(levels - 1) << 17 | (uint32_t)aniso << 22 | (uint32_t)nearest << 23;
    for (int i = 0; i < g_nsamps; i++)
        if (g_samps[i].key == key) return g_samps[i].s;
    SDL_GPUSamplerCreateInfo si;
    memset(&si, 0, sizeof(si));
    si.min_filter = d->minf && !nearest ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    si.mag_filter = d->magf && !nearest ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode = d->mipf ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = d->clampS ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    si.address_mode_v = d->clampT ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    float off = (float)d->lodoff / 16.0f, top = (float)(levels - 1);
    if (d->lodmode == 2) {
        float l = off < 0 ? 0 : off > top ? top : off;
        si.min_lod = si.max_lod = l;
    } else {
        float bias = off + (levels > 1 ? rt_gpu_opt.lod_bias : 0.0f);
        si.mip_lod_bias = bias < -15.99f ? -15.99f : bias > 15.99f ? 15.99f : bias;  /* the D3D12 / Vulkan limits */
        si.min_lod = 0;
        si.max_lod = top;
    }
    if (aniso) {
        si.enable_anisotropy = true;
        si.max_anisotropy = (float)(rt_gpu_opt.aniso > 16 ? 16 : rt_gpu_opt.aniso);
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    }
    if (g_nsamps >= MAX_SAMPLERS) {
        /* never create samplers without bound (drivers have a limited sampler heap): use a shared one */
        if (!g_samp_overflow++) fprintf(stderr, "[gpu] more than %d sampler kinds: using a plain linear sampler for the rest\n", MAX_SAMPLERS);
        return g_linear;
    }
    SDL_GPUSampler *s = SDL_CreateGPUSampler(g_dev, &si);
    if (!s && aniso) {
        /* the driver refused anisotropic filtering: carry on without it rather than crash */
        fprintf(stderr, "[gpu] the driver refused an anisotropic sampler (%dx): %s; anisotropic filtering is off from here\n",
                rt_gpu_opt.aniso, SDL_GetError());
        g_aniso_failed = 1;
        si.enable_anisotropy = false;
        si.max_anisotropy = 0;
        s = SDL_CreateGPUSampler(g_dev, &si);
    }
    if (!s) {
        if (!g_samp_failed++) fprintf(stderr, "[gpu] SDL_CreateGPUSampler failed: %s; using a plain linear sampler\n", SDL_GetError());
        return g_linear;
    }
    g_samps[g_nsamps].key = key;
    g_samps[g_nsamps++].s = s;
    return s;
}

/* --- texture packs ------------------------------------------------------------------
 * A texture's pack key is a hash of its decoded picture (level 0, RGBA): the same picture has the
 * same key whatever its address, format, palette or swizzling. --texture-dump writes each new
 * picture once as DIR/<key>.png (16 hex digits); --texture-pack loads DIR/<key>.png, of any size,
 * in its place (with a full mip chain made on the GPU). Texture coordinates are normalized, so a
 * bigger picture needs nothing else. */
#define PACK_SLOTS 65536  /* power of two */
typedef struct { uint64_t key; SDL_GPUTexture *tex; int w, levels, failed; } PackEntry;
static PackEntry *g_pack;     /* the pack's keys; the texture once loaded */
static uint64_t *g_dumped;    /* keys already dumped */
static SDL_GPUTexture **g_mipgen;
static size_t g_nmipgen, g_mipcap;

static uint64_t image_key(const uint32_t *px, int w, int h) {
    uint64_t a = 0x9E3779B97F4A7C15ull ^ ((uint64_t)w << 32 | (uint32_t)h), b = 0xC2B2AE3D27D4EB4Full;
    size_t n = (size_t)w * h, i = 0;
    for (; i + 2 <= n; i += 2) {
        a = (a ^ px[i]) * 0x100000001B3ull; a ^= a >> 29;
        b = (b ^ px[i + 1]) * 0x100000001B3ull; b ^= b >> 31;
    }
    if (i < n) a = (a ^ px[i]) * 0x100000001B3ull;
    a ^= b * 0x165667B19E3779F9ull;
    return (a ^ (a >> 33)) | 1;
}

static uint64_t *key_slot(uint64_t *set, uint64_t key) {
    size_t i = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 48) & (PACK_SLOTS - 1);
    for (int n = 0; n < PACK_SLOTS && set[i] && set[i] != key; n++) i = (i + 1) & (PACK_SLOTS - 1);
    return &set[i];
}

static PackEntry *pack_find(uint64_t key) {
    size_t i = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 48) & (PACK_SLOTS - 1);
    for (int n = 0; n < PACK_SLOTS && g_pack[i].key; n++, i = (i + 1) & (PACK_SLOTS - 1))
        if (g_pack[i].key == key) return &g_pack[i];
    return NULL;
}

static void pack_index(void) {
    const char *dir = rt_gpu_opt.texture_pack;
    if (rt_gpu_opt.texture_dump) {
        SDL_CreateDirectory(rt_gpu_opt.texture_dump);
        g_dumped = calloc(PACK_SLOTS, sizeof(uint64_t));
    }
    if (!dir) return;
    g_pack = calloc(PACK_SLOTS, sizeof(PackEntry));
    int n = 0, found = 0;
    char **files = SDL_GlobDirectory(dir, "*.png", SDL_GLOB_CASEINSENSITIVE, &n);
    for (int i = 0; files && i < n && found < PACK_SLOTS / 2; i++) {
        const char *name = strrchr(files[i], '/') ? strrchr(files[i], '/') + 1 : files[i];
        char *end;
        uint64_t key = strtoull(name, &end, 16);
        if (end - name != 16 || SDL_strcasecmp(end, ".png")) continue;
        size_t s = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 48) & (PACK_SLOTS - 1);
        while (g_pack[s].key && g_pack[s].key != key) s = (s + 1) & (PACK_SLOTS - 1);
        if (!g_pack[s].key) found++;
        g_pack[s].key = key;
    }
    SDL_free(files);
    printf("[gpu] texture pack %s: %d replacement textures (of %d .png files)\n", dir, found, n);
    /* PPSSPP packs (textures.ini, its own hashes) don't match: say so instead of silently doing nothing */
    char ini[1024];
    snprintf(ini, sizeof(ini), "%s/textures.ini", dir);
    if (!found && SDL_GetPathInfo(ini, NULL))
        fprintf(stderr, "[gpu] texture pack %s has a textures.ini: it is a PPSSPP pack, which this port can't load (its file names "
                        "are PPSSPP's texture hashes). This port loads <16 hex digits>.png files named as --texture-dump writes them.\n", dir);
    else if (n && !found)
        fprintf(stderr, "[gpu] texture pack %s: none of its .png files is named <16 hex digits>.png as --texture-dump writes them\n", dir);
}

static void dump_texture(uint64_t key, int w, int h, const uint32_t *rgba) {
    if (!g_dumped || w < 4 || h < 4) return;
    uint64_t *slot = key_slot(g_dumped, key);
    if (*slot) return;  /* dumped already, or the set is full */
    *slot = key;
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, (void *)rgba, w * 4);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%016llx.png", rt_gpu_opt.texture_dump, (unsigned long long)key);
    if (s && SDL_SavePNG(s, path)) g_st.dumped++;
    SDL_DestroySurface(s);
}

/* the pack's picture for this key, loaded (and its mip chain queued) the first time */
static PackEntry *pack_texture(uint64_t key) {
    PackEntry *p = g_pack ? pack_find(key) : NULL;
    if (!p || p->tex || p->failed) return p && p->tex ? p : NULL;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%016llx.png", rt_gpu_opt.texture_pack, (unsigned long long)key);
    SDL_Surface *s = SDL_LoadPNG(path), *c = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32) : NULL;
    SDL_DestroySurface(s);
    if (!c) { fprintf(stderr, "[gpu] texture pack: %s: %s\n", path, SDL_GetError()); p->failed = 1; return NULL; }
    int w = c->w, h = c->h, levels = 1;
    while ((w >> levels) || (h >> levels)) levels++;
    p->tex = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, w, h, levels,
                          SDL_GPU_SAMPLECOUNT_1);
    if (!p->tex) { SDL_DestroySurface(c); p->failed = 1; return NULL; }
    p->levels = levels;
    p->w = w;
    size_t bytes = (size_t)w * h * 4, off = (g_upsize + 511) & ~(size_t)511;
    grow((void **)&g_upbytes, &g_upbcap, off + bytes, 1);
    for (int y = 0; y < h; y++) memcpy(g_upbytes + off + (size_t)y * w * 4, (const uint8_t *)c->pixels + (size_t)y * c->pitch, (size_t)w * 4);
    g_upsize = off + bytes;
    SDL_DestroySurface(c);
    grow((void **)&g_ups, &g_upcap, g_nups + 1, sizeof(Upload));
    memset(&g_ups[g_nups], 0, sizeof(Upload));
    g_ups[g_nups].tex = p->tex; g_ups[g_nups].offset = (uint32_t)off; g_ups[g_nups].w = w; g_ups[g_nups].h = h;
    g_nups++;
    if (levels > 1) {
        grow((void **)&g_mipgen, &g_mipcap, g_nmipgen + 1, sizeof(SDL_GPUTexture *));
        g_mipgen[g_nmipgen++] = p->tex;
    }
    g_st.pack_loads++;
    g_st.upload_bytes += bytes;
    return p;
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
    int nl = levels < 1 ? 1 : levels;
    if (g_pack || g_dumped) {
        uint64_t ik = image_key(rgba, w, h);
        dump_texture(ik, w, h, rgba);
        PackEntry *p = pack_texture(ik);
        if (p) {  /* the pack's picture: shared by every texture with this picture, kept for the run */
            e->tex = p->tex;
            e->key = key;
            e->w = w;
            e->h = h;
            e->levels = p->levels;
            e->lod_shift = 0;  /* mip levels the pack's picture has above the PSP's level 0 */
            while ((w << (e->lod_shift + 1)) <= p->w) e->lod_shift++;
            e->replaced = 1;
            e->used = g_frame;
            g_tex_count++;
            g_st.replaced++;
            return;
        }
    }
    e->tex = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, w, h, nl, SDL_GPU_SAMPLECOUNT_1);
    if (!e->tex) return;
    e->key = key;
    e->w = w;
    e->h = h;
    e->levels = nl;
    e->replaced = 0;
    e->used = g_frame;
    g_tex_count++;
    for (int l = 0; l < nl; l++) {
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
        if (g_frame - old[i].used > 300) {
            if (!old[i].replaced) SDL_ReleaseGPUTexture(g_dev, old[i].tex);  /* a pack's picture stays */
            continue;
        }
        *tex_slot(old[i].key) = old[i];
        g_tex_count++;
    }
    free(old);
}

/* releases pooled textures not used for a while (at a high internal resolution they are big) */
static void evict_pool(void) {
    if (g_frame % 120) return;
    int n = 0;
    for (int i = 0; i < g_nsnaps; i++) {
        if (g_frame - g_snaps[i].used > 240) {
            g_st.snap_bytes -= (uint64_t)g_snaps[i].w * g_snaps[i].h * 4;
            SDL_ReleaseGPUTexture(g_dev, g_snaps[i].tex);
            continue;
        }
        g_snaps[n++] = g_snaps[i];
    }
    g_nsnaps = n;
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
            if (e->key) {
                tex = e->tex;
                e->used = g_frame;
                if (e->replaced) {  /* a pack's picture: its own mip chain, filtered as it is */
                    static GpuDraw dr;
                    dr = *d;
                    dr.levels = e->levels;
                    if (dr.lodmode == 2) dr.lodoff += 16 * e->lod_shift;  /* a constant level counts from the PSP's level 0 */
                    dr.pixel_mapped = 0;
                    d = &dr;
                }
            }
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
    SDL_Rect sc = c->scissor;
    /* the PSP draws in many short strips: a draw with the same state as the command just
     * before it (same target, pipeline, texture, sampler and uniforms) extends that command */
    Cmd *p = g_ncmds >= 2 ? c - 1 : NULL;
    if (p && p->kind == CMD_DRAW && p->first + p->count == c->first && p->target == c->target && p->pipe == c->pipe &&
        p->tex == c->tex && p->sampler == c->sampler && p->stencil == c->stencil && p->stencil_ref == c->stencil_ref &&
        !memcmp(&p->scissor, &c->scissor, sizeof(c->scissor)) && !memcmp(&p->blend_const, &c->blend_const, sizeof(c->blend_const)) &&
        !memcmp(&p->fu, &c->fu, sizeof(c->fu))) {
        p->count += n;
        g_ncmds--;
        g_st.merged++;
    }
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
    /* --aspect: 2D on a shown framebuffer (HUD, text, markers placed over the 3D view) is drawn
     * narrower around the centre, as the 3D view is, so that the wider window gives it back its
     * shape. Not full-width passes (screen effects, fades), clears, or copies of render targets. */
    float sq = rt_gpu_aspect_squeeze();
    if (sq != 1.0f && d->through && !d->tex_target && !(d->flags & GPU_F_CLEAR) && bx1 - bx0 < DISP_W - 8 && rt_gpu_shown_fb(t->addr)) {
        float cx = (float)ox + DISP_W / 2.0f;
        for (uint32_t i = 0; i < n; i++) o[i].x = cx + (o[i].x - cx) * sq;
    }
    g_nverts += n;
    int wx0 = imax(x1, (int)bx0), wy0 = imax(y1, (int)by0), wx1 = imin(x2 + 1, (int)bx1 + 1), wy1 = imin(y2 + 1, (int)by1 + 1);
    if (sc.w && sc.h) mark_written(t, wx0, wy0, wx1, wy1);
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
    for (size_t i = 0; i < g_nmipgen; i++) SDL_GenerateMipmapsForGPUTexture(cmd, g_mipgen[i]);  /* texture pack pictures */
    g_nmipgen = 0;
}

/* commands other than draws run between render passes, in order: copies (snapshots, transfers),
 * uploads into targets, blits (scaled copies) and unresolves */
static void run_other(SDL_GPUCommandBuffer *cmd, const Cmd *c) {
    if (c->kind == CMD_UNRESOLVE) {
        const Target *t = &g_targets[c->target];
        int s = t->s;
        SDL_GPUViewport vp = { 0, 0, (float)(t->w * s), (float)(t->h * s), 0.0f, 1.0f };
        SDL_Rect sc = { c->scissor.x * s, c->scissor.y * s, c->scissor.w * s, c->scissor.h * s };
        PostU u;
        memset(&u, 0, sizeof(u));
        u.src[2] = (float)(t->w * s);
        u.src[3] = (float)(t->h * s);
        u.mode[0] = P_COPY;
        post_pass(cmd, t->ms, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, g_sc, SDL_GPU_LOADOP_LOAD, &vp, &sc, t->tex, t->w * s, t->h * s, &u, 0);
        return;
    }
    if (c->w <= 0 || c->h <= 0) return;
    if (c->kind == CMD_BLIT) {
        SDL_GPUBlitInfo bi;
        memset(&bi, 0, sizeof(bi));
        bi.source.texture = c->tex;
        bi.source.x = (Uint32)c->sx; bi.source.y = (Uint32)c->sy; bi.source.w = (Uint32)c->sw; bi.source.h = (Uint32)c->sh;
        bi.destination.texture = c->dst;
        bi.destination.x = (Uint32)c->dx; bi.destination.y = (Uint32)c->dy; bi.destination.w = (Uint32)c->w; bi.destination.h = (Uint32)c->h;
        bi.load_op = SDL_GPU_LOADOP_LOAD;
        bi.filter = c->filter;
        SDL_BlitGPUTexture(cmd, &bi);
        return;
    }
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

static int drawable(const Cmd *c) { return c->count && c->scissor.w && c->scissor.h && g_pipes[c->pipe].p; }

/* Multisampling: a pass resolves its multisampled colour into the target's colour texture only
 * when a command reads that texture before the next pass on the target, or when it is the
 * target's last pass in this submission. The flag goes on the pass's first draw. */
static void plan_resolves(void) {
    if (g_samples == 1) return;
    int open[MAX_TARGETS], cur = -1;
    for (int i = 0; i < g_ntargets; i++) open[i] = -1;
    for (size_t i = 0; i < g_ncmds; i++) {
        Cmd *c = &g_cmds[i];
        if (c->kind == CMD_DRAW) {
            if (!drawable(c)) continue;
            if (c->target != cur) {
                open[c->target] = (int)i;  /* the pass before on this target, if undecided, needs no resolve */
                c->resolve = 0;
                cur = c->target;
            }
            continue;
        }
        cur = -1;
        if (c->src_target >= 0 && open[c->src_target] >= 0) {
            g_cmds[open[c->src_target]].resolve = 1;
            open[c->src_target] = -1;
        }
    }
    for (int i = 0; i < g_ntargets; i++) if (open[i] >= 0) g_cmds[open[i]].resolve = 1;
}

static void render_frame(SDL_GPUCommandBuffer *cmd) {
    plan_resolves();
    SDL_GPURenderPass *pass = NULL;
    int cur_t = -1, cur_pipe = -1, s = 1;
    for (size_t i = 0; i < g_ncmds; i++) {
        Cmd *c = &g_cmds[i];
        if (c->kind != CMD_DRAW) {
            if (pass) { SDL_EndGPURenderPass(pass); pass = NULL; cur_t = -1; }
            run_other(cmd, c);
            continue;
        }
        if (!drawable(c)) continue;
        if (c->target != cur_t) {
            if (pass) SDL_EndGPURenderPass(pass);
            Target *t = &g_targets[c->target];
            s = t->s;
            SDL_GPUColorTargetInfo ci;
            memset(&ci, 0, sizeof(ci));
            ci.texture = t->ms ? t->ms : t->tex;
            ci.load_op = SDL_GPU_LOADOP_LOAD;  /* a new target was uploaded from memory first */
            ci.store_op = SDL_GPU_STOREOP_STORE;
            if (t->ms) {
                ci.resolve_texture = t->tex;
                if (c->resolve) { ci.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE; g_st.msaa_resolves++; }
            }
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
            SDL_GPUViewport vp = { 0, 0, (float)(t->w * s), (float)(t->h * s), 0.0f, 1.0f };
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
        SDL_Rect sc = { c->scissor.x * s, c->scissor.y * s, c->scissor.w * s, c->scissor.h * s };
        SDL_SetGPUScissor(pass, &sc);
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

/* staging for reading a scaled target back at 1x */
static int readback_texture(int w, int h) {
    if (g_rb && g_rb_w >= w && g_rb_h >= h) return 1;
    if (g_rb) SDL_ReleaseGPUTexture(g_dev, g_rb);
    g_rb_w = imax(w, g_rb_w);
    g_rb_h = imax(h, g_rb_h);
    g_rb = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, g_rb_w,
                        g_rb_h, 1, SDL_GPU_SAMPLECOUNT_1);
    return g_rb != NULL;
}

/* Records the copy of rectangle (x, y, w, h) of tex into a transfer buffer at the end of cmd,
 * submits cmd and waits for it, and reads the pixels (RGBA8) into out. */
static int download_tex(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, int x, int y, int w, int h, uint32_t *out) {
    uint32_t bytes = (uint32_t)w * (uint32_t)h * 4;
    SDL_GPUTransferBufferCreateInfo ti = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes, 0 };
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(g_dev, &ti);
    if (!tb) { SDL_CancelGPUCommandBuffer(cmd); return 0; }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion r;
    memset(&r, 0, sizeof(r));
    r.texture = src;
    r.x = (Uint32)x; r.y = (Uint32)y; r.w = (Uint32)w; r.h = (Uint32)h; r.d = 1;
    SDL_GPUTextureTransferInfo dst = { tb, 0, (Uint32)w, (Uint32)h };
    SDL_DownloadFromGPUTexture(cp, &r, &dst);
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

/* Reads rectangle (x, y, w, h) of target ti (PSP pixels) as RGBA8 into out, after all work
 * recorded before. A scaled target is brought to 1x on the GPU first: point-sampled (DL_POINT:
 * data the CPU reads back keeps exact values) or box-filtered (DL_BOX: pictures). */
static int download(int ti, int x, int y, int w, int h, uint32_t *out, int mode) {
    if (w <= 0 || h <= 0) return 0;
    submit_pending();
    const Target *t = &g_targets[ti];
    int s = t->s, rx = x * s, ry = y * s;
    SDL_GPUTexture *src = t->tex;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (!cmd) return 0;
    if (s > 1) {
        if (!readback_texture(w, h)) { SDL_CancelGPUCommandBuffer(cmd); return 0; }
        if (mode == DL_BOX) {
            PostU u;
            memset(&u, 0, sizeof(u));
            u.src[0] = (float)rx; u.src[1] = (float)ry; u.src[2] = (float)(rx + w * s); u.src[3] = (float)(ry + h * s);
            u.mode[0] = P_BOX;
            u.mode[1] = (uint32_t)s;
            SDL_GPUViewport vp = { 0, 0, (float)w, (float)h, 0.0f, 1.0f };
            post_pass(cmd, g_rb, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_SAMPLECOUNT_1, SDL_GPU_LOADOP_DONT_CARE, &vp, NULL, t->tex,
                      t->w * s, t->h * s, &u, 0);
        } else {
            SDL_GPUBlitInfo bi;
            memset(&bi, 0, sizeof(bi));
            bi.source.texture = t->tex;
            bi.source.x = (Uint32)rx; bi.source.y = (Uint32)ry; bi.source.w = (Uint32)(w * s); bi.source.h = (Uint32)(h * s);
            bi.destination.texture = g_rb;
            bi.destination.w = (Uint32)w; bi.destination.h = (Uint32)h;
            bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
            bi.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(cmd, &bi);
        }
        src = g_rb;
        rx = ry = 0;
    }
    return download_tex(cmd, src, rx, ry, w, h, out);
}

/* Writes the w x h region at (x, y) of target ti into PSP memory, in the target's format. */
static void write_back(int ti, int x, int y, int w, int h) {
    Target *t = &g_targets[ti];
    w = imin(w, t->w - x);
    h = imin(h, t->h - y);
    if (w <= 0 || h <= 0) return;
    grow((void **)&g_dl, &g_dlcap, (size_t)w * h, sizeof(uint32_t));
    record_resolve(ti);  /* memory gets the stencil in the alpha channel */
    if (!download(ti, x, y, w, h, g_dl, DL_POINT)) return;
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
        Target *dt = &g_targets[d];
        int ss = g_targets[s].s, ds = dt->s, cw = imin(w, dt->w - dtx), ch = imin(h, dt->h - dty);
        if (ss == ds) {
            Cmd *c = new_cmd(CMD_COPY);
            c->tex = snap;
            c->dst = dt->tex;
            c->dx = dtx * ds; c->dy = dty * ds;
            c->w = cw * ds;
            c->h = ch * ds;
        } else {
            record_blit(snap, -1, 0, 0, cw * ss, ch * ss, dt->tex, dtx * ds, dty * ds, cw * ds, ch * ds, SDL_GPU_FILTER_NEAREST);
        }
        unresolve(d, dtx, dty, cw, ch);
        mark_written(dt, dtx, dty, dtx + w, dty + h);
        g_xfer_gpu = 1;
        g_st.gpu_copies++;
        return;
    }
    if (readback) write_back(s, stx, sty, w, h);  /* the copy in memory then moves what the GPU drew */
}

/* PSP memory changed under a rectangle of a target: the target takes it */
static void memory_written(uint32_t dst, uint32_t dpitch, int dx, int dy, int w, int h, int bpp, SDL_GPUFilter filter) {
    if (!g_dev) return;
    int tx = 0, ty = 0, d = target_for_rect(dst, dpitch, bpp, dx, dy, w, h, 0, &tx, &ty);
    if (d < 0) return;
    Target *t = &g_targets[d];
    upload_to_target(d, tx, ty, w, h, t->addr + (uint32_t)ty * t->pitch + (uint32_t)tx * (uint32_t)t->bpp, t->pitch, t->fmt, filter);
    mark_written(t, tx, ty, tx + w, ty + h);
    t->mem_hash = display_hash(t);
}

void rt_gpu_transfer_end(uint32_t dst, uint32_t dpitch, int dx, int dy, int w, int h, int bpp) {
    if (!g_xfer_gpu) memory_written(dst, dpitch, dx, dy, w, h, bpp, SDL_GPU_FILTER_NEAREST);
}

/* the CPU or a video decoder: pictures, scaled smoothly into a scaled target */
void rt_gpu_memory_written(uint32_t dst, uint32_t dpitch, int dx, int dy, int w, int h, int bpp) {
    memory_written(dst, dpitch, dx, dy, w, h, bpp, SDL_GPU_FILTER_LINEAR);
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

/* post-processing at the internal resolution: ping-pong textures of the display area's size */
static int post_textures(int w, int h) {
    if (g_post[0] && g_post_w == w && g_post_h == h) return 1;
    for (int i = 0; i < 2; i++) {
        if (g_post[i]) SDL_ReleaseGPUTexture(g_dev, g_post[i]);
        g_post[i] = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, w, h, 1,
                                 SDL_GPU_SAMPLECOUNT_1);
    }
    g_post_w = w;
    g_post_h = h;
    return g_post[0] && g_post[1];
}

/* FXAA and sharpening (when on) of the display area at (x, y) of target t, at the internal
 * resolution: returns the texture holding the picture, its size (*tw x *th) and the area in it (r). */
static SDL_GPUTexture *post_chain(SDL_GPUCommandBuffer *cmd, int t, int x, int y, int *tw, int *th, float r[4]) {
    const Target *tg = &g_targets[t];
    int s = tg->s, pw = imin(DISP_W, tg->w - x) * s, ph = imin(DISP_H, tg->h - y) * s;
    SDL_GPUTexture *src = tg->tex;
    *tw = tg->w * s;
    *th = tg->h * s;
    r[0] = (float)(x * s); r[1] = (float)(y * s); r[2] = (float)(x * s + pw); r[3] = (float)(y * s + ph);
    int passes[2] = { rt_gpu_opt.fxaa ? P_FXAA : -1, rt_gpu_opt.sharpen > 0 ? P_CAS : -1 }, k = 0;
    for (int i = 0; i < 2; i++) {
        if (passes[i] < 0 || !post_textures(pw, ph)) continue;
        PostU u;
        memset(&u, 0, sizeof(u));
        memcpy(u.src, r, sizeof(u.src));
        u.mode[0] = (uint32_t)passes[i];
        u.p0[0] = rt_gpu_opt.sharpen;
        SDL_GPUViewport vp = { 0, 0, (float)pw, (float)ph, 0.0f, 1.0f };
        post_pass(cmd, g_post[k], SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_SAMPLECOUNT_1, SDL_GPU_LOADOP_DONT_CARE, &vp, NULL, src, *tw, *th,
                  &u, 1);
        src = g_post[k];
        *tw = pw; *th = ph;
        r[0] = r[1] = 0; r[2] = (float)pw; r[3] = (float)ph;
        k ^= 1;
    }
    return src;
}

/* The displayed framebuffer into the window: FXAA and sharpening at the internal resolution
 * (when on), then scaled into the window, letterboxed, as SDR or HDR. */
static void draw_to_window(SDL_GPUCommandBuffer *cmd, int t, int x, int y) {
    SDL_GPUTexture *sc = NULL;
    Uint32 sw = 0, sh = 0;
    uint64_t t0 = SDL_GetTicksNS();
    int ok = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, g_win, &sc, &sw, &sh);
    uint64_t dt = SDL_GetTicksNS() - t0;
    g_st.acquire_ns += dt;
    if (dt > g_st.acquire_max) g_st.acquire_max = dt;
    if (dt > 4000000) g_st.acquire_slow++;
    if (!ok || !sc) return;
    if (t < 0) {
        SDL_GPUColorTargetInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.texture = sc;
        ci.load_op = SDL_GPU_LOADOP_CLEAR;
        ci.store_op = SDL_GPU_STOREOP_STORE;
        ci.clear_color.a = 1.0f;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &ci, 1, NULL));
        return;
    }
    int tw, th;
    float r[4];
    SDL_GPUTexture *src = post_chain(cmd, t, x, y, &tw, &th, r);
    float pw = r[2] - r[0], ph = r[3] - r[1];
    PostU u;
    /* letterbox the 480x272 picture into the window (stretched to --aspect) */
    float lw = DISP_W / rt_gpu_aspect_squeeze();
    float f = (float)sw / lw < (float)sh / DISP_H ? (float)sw / lw : (float)sh / DISP_H;
    float dw = floorf(lw * f), dh = floorf(DISP_H * f);
    SDL_GPUViewport vp = { floorf((sw - dw) / 2), floorf((sh - dh) / 2), dw, dh, 0.0f, 1.0f };
    memset(&u, 0, sizeof(u));
    memcpy(u.src, r, sizeof(r));
    u.mode[0] = P_PRESENT;
    float ratio = pw / dw > ph / dh ? pw / dw : ph / dh;  /* source texels per window pixel */
    u.mode[1] = ratio > 1.01f && !rt_gpu_opt.nearest ? (uint32_t)ceilf(ratio) : 1u;
    u.mode[2] = (uint32_t)g_hdr_out;
    u.p0[0] = g_paper; u.p0[1] = g_peak; u.p0[2] = 0.55f;
    u.p1[0] = pw / dw; u.p1[1] = ph / dh;
    post_pass(cmd, sc, g_swfmt, SDL_GPU_SAMPLECOUNT_1, SDL_GPU_LOADOP_CLEAR, &vp, NULL, src, tw, th, &u, !rt_gpu_opt.nearest);
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
        if (download(i, t->ux0, t->uy0, w, h, g, DL_POINT)) {
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
        int known = 0;
        for (int i = 0; i < g_ndisp && !known; i++) known = g_disp[i] == addr;
        if (!known && g_ndisp < (int)(sizeof(g_disp) / sizeof(g_disp[0]))) g_disp[g_ndisp++] = addr;
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
                upload_to_target(t, 0, 0, imin(DISP_W, tg->w), imin(DISP_H, tg->h), tg->addr, tg->pitch, tg->fmt, SDL_GPU_FILTER_LINEAR);
                tg->version++;
            }
        }
        if (t >= 0 && g_targets[t].s != g_scale) g_targets[t].want_s = g_scale;  /* shown: the full scale, from the next frame */
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (!cmd) return;
    upload_frame(cmd);
    render_frame(cmd);
    if (g_win) draw_to_window(cmd, t, x, y);
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
        rt_perf_us[RT_PERF_GPU] += dt / 1000;  /* --perf-csv gpu_ms (in the next frame's row) */
        if (dt > g_st.gpu_wait_max) g_st.gpu_wait_max = dt;
    } else {
        SDL_SubmitGPUCommandBuffer(cmd);
    }
    reset_recorded();
    for (int i = 0; i < g_nrelease; i++) SDL_ReleaseGPUTexture(g_dev, g_release[i]);  /* submitted: SDL keeps them until done */
    g_nrelease = 0;
    debug_dump();
    for (int i = 0; i < g_nsnaps; i++) g_snaps[i].busy = 0;
    g_frame++;
    g_st.frames++;
    for (int i = 0; i < g_ntargets; i++) if (g_targets[i].want_s) rescale_target(i);
    evict_textures();
    evict_pool();
}

int rt_gpu_read_target(uint32_t addr, uint32_t stride, uint32_t fmt, uint8_t *rgba) {
    if (!g_dev) return 0;
    int x = 0, y = 0, t = display_target(addr, stride, fmt, &x, &y);
    if (t < 0) return 0;
    const Target *tg = &g_targets[t];
    int w = imin(DISP_W, tg->w - x), h = imin(DISP_H, tg->h - y);
    grow((void **)&g_dl, &g_dlcap, (size_t)DISP_W * DISP_H, sizeof(uint32_t));
    if (!download(t, x, y, w, h, g_dl, DL_BOX)) return 0;
    memset(rgba, 0, (size_t)DISP_W * DISP_H * 4);
    for (int j = 0; j < h; j++) memcpy(rgba + (size_t)j * DISP_W * 4, g_dl + (size_t)j * w, (size_t)w * 4);
    return 1;
}

uint8_t *rt_gpu_read_target_full(uint32_t addr, uint32_t stride, uint32_t fmt, int *w, int *h) {
    if (!g_dev) return NULL;
    int x = 0, y = 0, t = display_target(addr, stride, fmt, &x, &y);
    if (t < 0) return NULL;
    const Target *tg = &g_targets[t];
    int lw = imin(DISP_W, tg->w - x), lh = imin(DISP_H, tg->h - y);
    uint8_t *out = malloc((size_t)lw * lh * tg->s * tg->s * 4);
    if (!out) return NULL;
    /* as the window shows it: with FXAA and sharpening, when they are on */
    submit_pending();
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    int tw, th;
    float r[4];
    SDL_GPUTexture *src = cmd ? post_chain(cmd, t, x, y, &tw, &th, r) : NULL;
    if (!src || !download_tex(cmd, src, (int)r[0], (int)r[1], lw * tg->s, lh * tg->s, (uint32_t *)out)) { free(out); return NULL; }
    *w = lw * tg->s;
    *h = lh * tg->s;
    return out;
}

void rt_gpu_report(void) {
    if (!g_dev) return;
    printf("[gpu] %llu frames: %llu draws (%llu merged into the one before), %llu vertices, %llu render passes, %llu texture uploads (%.1f MB), "
           "%llu pipelines\n",
           (unsigned long long)g_st.frames, (unsigned long long)g_st.draws, (unsigned long long)g_st.merged, (unsigned long long)g_st.verts,
           (unsigned long long)g_st.passes, (unsigned long long)g_st.uploads, g_st.upload_bytes / 1048576.0,
           (unsigned long long)g_st.pipelines);
    printf("[gpu] %llu render targets, %llu render-to-texture snapshots, %llu uploads into targets, %llu GPU copies, "
           "%llu readbacks, %llu mid-frame submits, %llu stencil-to-alpha copies, %llu fills across formats\n",
           (unsigned long long)g_st.targets, (unsigned long long)g_st.snapshots, (unsigned long long)g_st.target_uploads,
           (unsigned long long)g_st.gpu_copies, (unsigned long long)g_st.readbacks, (unsigned long long)g_st.submits,
           (unsigned long long)g_st.resolves, (unsigned long long)g_st.fills);
    if (g_scale > 1 || g_samples > 1) {
        uint64_t tb = 0;
        for (int i = 0; i < g_ntargets; i++) {
            const Target *t = &g_targets[i];
            int per_px = 4 + 4 * g_samples + (g_samples > 1 ? 4 * g_samples : 0);  /* colour, depth-stencil, multisampled colour */
            tb += (uint64_t)t->w * t->s * t->h * t->s * per_px;
        }
        printf("[gpu] %dx: %llu scaled copies (blits), %llu MSAA resolves, %llu unresolves; render targets %.0f MB, pooled textures %.0f MB\n",
               g_scale, (unsigned long long)g_st.blits, (unsigned long long)g_st.msaa_resolves, (unsigned long long)g_st.unresolves,
               tb / 1048576.0, g_st.snap_bytes / 1048576.0);
    }
    if (g_dumped || g_pack)
        printf("[gpu] texture pack: %llu pictures dumped, %llu replacements loaded, %llu textures replaced\n", (unsigned long long)g_st.dumped,
               (unsigned long long)g_st.pack_loads, (unsigned long long)g_st.replaced);
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
            printf("[gpu]   target %08X pitch %u fmt %d %dx%d (x%d) drawn x %d..%d y %d..%d\n", t->addr, t->pitch, t->fmt, t->w, t->h, t->s,
                   t->ux0, t->ux1, t->uy0, t->uy1);
        }
}

void rt_gpu_shutdown(void) {
    if (!g_dev) return;
    SDL_WaitForGPUIdle(g_dev);
    for (size_t i = 0; i < TEX_SLOTS; i++) if (g_tex[i].key && !g_tex[i].replaced) SDL_ReleaseGPUTexture(g_dev, g_tex[i].tex);
    for (size_t i = 0; g_pack && i < PACK_SLOTS; i++) if (g_pack[i].tex) SDL_ReleaseGPUTexture(g_dev, g_pack[i].tex);
    free(g_pack);
    free(g_dumped);
    g_pack = NULL;
    g_dumped = NULL;
    for (int i = 0; i < g_ntargets; i++) {
        SDL_ReleaseGPUTexture(g_dev, g_targets[i].tex);
        SDL_ReleaseGPUTexture(g_dev, g_targets[i].ds);
        if (g_targets[i].ms) SDL_ReleaseGPUTexture(g_dev, g_targets[i].ms);
    }
    for (int i = 0; i < g_nsnaps; i++) SDL_ReleaseGPUTexture(g_dev, g_snaps[i].tex);
    for (int i = 0; i < g_nrelease; i++) SDL_ReleaseGPUTexture(g_dev, g_release[i]);
    for (int i = 0; i < g_npipes; i++) if (g_pipes[i].p) SDL_ReleaseGPUGraphicsPipeline(g_dev, g_pipes[i].p);
    for (int i = 0; i < g_nppipes; i++) if (g_ppipes[i].p) SDL_ReleaseGPUGraphicsPipeline(g_dev, g_ppipes[i].p);
    printf("[gpu] %d samplers%s%s\n", g_nsamps, g_samp_overflow ? " (cache full: some draws used a plain sampler)" : "",
           g_aniso_failed ? "; anisotropic filtering was refused by the driver and turned off" : "");
    for (int i = 0; i < g_nsamps; i++) SDL_ReleaseGPUSampler(g_dev, g_samps[i].s);
    g_nsamps = g_nsnaps = g_ntargets = g_nrelease = g_npipes = g_nppipes = 0;
    if (g_rb) SDL_ReleaseGPUTexture(g_dev, g_rb);
    for (int i = 0; i < 2; i++) if (g_post[i]) SDL_ReleaseGPUTexture(g_dev, g_post[i]);
    SDL_ReleaseGPUSampler(g_dev, g_point);
    SDL_ReleaseGPUSampler(g_dev, g_linear);
    SDL_ReleaseGPUTexture(g_dev, g_white);
    if (g_vbuf) SDL_ReleaseGPUBuffer(g_dev, g_vbuf);
    if (g_tbuf) SDL_ReleaseGPUTransferBuffer(g_dev, g_tbuf);
    SDL_ReleaseGPUShader(g_dev, g_vs);
    SDL_ReleaseGPUShader(g_dev, g_fs);
    SDL_ReleaseGPUShader(g_dev, g_post_vs);
    SDL_ReleaseGPUShader(g_dev, g_post_fs);
    if (g_win) SDL_ReleaseWindowFromGPUDevice(g_dev, g_win);
    SDL_DestroyGPUDevice(g_dev);
    g_dev = NULL;
}

#else  /* no SDL: no hardware renderer */
int rt_gpu_init(void *window, const char *driver) { (void)window; (void)driver; fprintf(stderr, "[gpu] built without SDL3\n"); return 0; }
int rt_gpu_active(void) { return 0; }
int rt_gpu_scale(void) { return 1; }
int rt_gpu_capture_full(void) { return 0; }
float rt_gpu_aspect_squeeze(void) { return 1.0f; }
int rt_gpu_shown_fb(uint32_t addr) { (void)addr; return 0; }
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
void rt_gpu_memory_written(uint32_t dst, uint32_t dpitch, int dx, int dy, int w, int h, int bpp) {
    (void)dst; (void)dpitch; (void)dx; (void)dy; (void)w; (void)h; (void)bpp;
}
void rt_gpu_sync_span(uint32_t addr, uint32_t size) { (void)addr; (void)size; }
void rt_gpu_present(uint32_t addr, uint32_t stride, uint32_t fmt) { (void)addr; (void)stride; (void)fmt; }
int rt_gpu_read_target(uint32_t addr, uint32_t stride, uint32_t fmt, uint8_t *rgba) { (void)addr; (void)stride; (void)fmt; (void)rgba; return 0; }
uint8_t *rt_gpu_read_target_full(uint32_t addr, uint32_t stride, uint32_t fmt, int *w, int *h) {
    (void)addr; (void)stride; (void)fmt; (void)w; (void)h;
    return NULL;
}
void rt_gpu_report(void) {}
void rt_gpu_debug_compare_texture(uint32_t addr, uint32_t pitch, int bpp, int fmt, int w, int h) {
    (void)addr; (void)pitch; (void)bpp; (void)fmt; (void)w; (void)h;
}
#endif
