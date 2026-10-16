/* Hardware renderer on SDL3 GPU (Vulkan / Direct3D 12). rt_gesoft.c decodes, transforms,
 * lights, clips and assembles the primitives; this module draws them on the GPU. */
#ifndef RT_GPU_H
#define RT_GPU_H
#include <stdint.h>

/* A vertex in PSP screen space of the render target: x, y in pixels, z 0..65535, w = clip w
 * (1 for screen-linear interpolation), s, t, q texture coordinates (normalized), colours. */
typedef struct {
    float x, y, z, w;
    float s, t, q;
    uint8_t c0[4], c1[4];
    float fog;
} GpuVertex;

/* fragment flags (runtime/shaders/ge.hlsl) */
enum { GPU_F_TEX = 1, GPU_F_RGBA = 2, GPU_F_DOUBLE = 4, GPU_F_PROJ = 8, GPU_F_CLEAR = 16, GPU_F_FOG = 32, GPU_F_ZRANGE = 64 };

typedef struct {
    uint32_t fb_addr, fb_stride, fb_fmt, z_addr, z_stride;
    int sx1, sy1, sx2, sy2;            /* scissor, inclusive */
    int zfunc, zwrite;                 /* PSP compare function (1 = always) */
    int blend, bsrc, bdst, beq;
    uint32_t fixa, fixb;
    uint32_t mask_rgb;                 /* PSP colour mask: set bits are kept */
    int write_alpha;
    int stest, sfunc, sref, smask;     /* stencil test (the PSP keeps the stencil in the alpha channel) */
    int sfail, zfail, zpass;           /* PSP stencil operations */
    uint32_t skeep;                    /* stencil bits kept (PSP alpha write mask) */
    int clear_stencil;                 /* clear mode writing the stencil: the value is the vertex alpha */
    uint32_t flags, texfunc, afunc, aref, amask;
    uint32_t env, fogcol;
    int minz, maxz;
    int textured, minf, magf, mipf, clampS, clampT;
    int lodmode, lodoff, levels;       /* PSP LOD mode, offset in 1/16 levels; mip levels uploaded */
    uint64_t tex_key;                  /* identifies the decoded texture (address, format, content, palette) */
    int tex_target;                    /* the texture is something the GPU drew (render to texture): */
    uint32_t tex_addr, tex_pitch;      /*   address and bytes per row */
    int tex_bpp, tex_fmt, tex_w, tex_h;
} GpuDraw;

/* window: the SDL_Window to present to, or NULL (headless). driver: "vulkan", "direct3d12" or NULL. */
int rt_gpu_init(void *window, const char *driver);
int rt_gpu_active(void);
void rt_gpu_shutdown(void);
/* texture cache: is the texture with this key already on the GPU? If not, upload it: rgba
 * holds the levels one after another, level i being max(1, w >> i) x max(1, h >> i). */
int rt_gpu_texture_resident(uint64_t key);
void rt_gpu_texture_upload(uint64_t key, int w, int h, int levels, const uint32_t *rgba);
/* Does the texture at addr (pitch bytes per row, bpp bytes per pixel) lie in what the GPU drew? */
int rt_gpu_texture_is_target(uint32_t addr, uint32_t pitch, int bpp);
/* records a draw of n vertices (a triangle list) */
void rt_gpu_draw(const GpuDraw *d, const GpuVertex *v, uint32_t n);
/* GE block transfer of w x h pixels of bpp bytes (pitches in bytes). Call _begin before the
 * copy in PSP memory and _end after it: a source the GPU drew is copied on the GPU when the
 * destination is a GPU target too, or else read back into memory (readback: 1 when only the
 * GPU draws); a destination inside a GPU target is then updated from memory. */
void rt_gpu_transfer_begin(uint32_t src, uint32_t spitch, int sx, int sy, uint32_t dst, uint32_t dpitch, int dx, int dy,
                           int w, int h, int bpp, int readback);
void rt_gpu_transfer_end(uint32_t dst, uint32_t dpitch, int dx, int dy, int w, int h, int bpp);
/* Something other than the GE (the CPU, a video decoder) wrote the w x h rectangle at (dx, dy)
 * of dst in PSP memory: the part of a GPU target there is updated from memory. */
void rt_gpu_memory_written(uint32_t dst, uint32_t dpitch, int dx, int dy, int w, int h, int bpp);
/* Writes what the GPU drew in [addr, addr + size) to PSP memory, before something reads it directly. */
void rt_gpu_sync_span(uint32_t addr, uint32_t size);
/* end of frame: submits the recorded work and shows the framebuffer at addr in the window */
void rt_gpu_present(uint32_t addr, uint32_t stride, uint32_t fmt);
/* reads the 480x272 display area of the framebuffer at addr as the GPU drew it, as RGBA */
int rt_gpu_read_target(uint32_t addr, uint32_t stride, uint32_t fmt, uint8_t *rgba);
void rt_gpu_report(void);
/* debugging: compares a texture the GPU takes from a target with PSP memory */
void rt_gpu_debug_compare_texture(uint32_t addr, uint32_t pitch, int bpp, int fmt, int w, int h);

#endif
