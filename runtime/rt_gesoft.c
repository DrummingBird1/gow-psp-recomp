/* GE software renderer: vertex decoding, transform and lighting, near-plane clipping,
 * triangle/sprite/line/point rasterization, texturing and the per-pixel pipeline.
 * Behaviour follows PPSSPP's software renderer (GPU/Software v1.20.4: TransformUnit,
 * Lighting, Clipper, Rasterizer, Sampler, DrawPixel, FuncId; GPU/GPUState.h); the code
 * here is an independent C implementation of those semantics. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  /* worker threads; included before the R macro below */
#endif

#include "rt_ge_internal.h"
#include "rt_gpu.h"

#ifdef _MSC_VER
#define HOT_INLINE static __forceinline
#else
#define HOT_INLINE static inline __attribute__((always_inline))
#endif

#define D(c) (ge.cmd[c] & 0xFFFFFFu)
#define F(c) f24(ge.cmd[c])
#define VRAM 0x04000000u

enum { CMP_NEVER, CMP_ALWAYS, CMP_EQUAL, CMP_NOTEQUAL, CMP_LESS, CMP_LEQUAL, CMP_GREATER, CMP_GEQUAL };
enum { CULL_OFF, CULL_CW, CULL_CCW };

static float f24(uint32_t op) { uint32_t b = op << 8; float f; memcpy(&f, &b, 4); return f; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

HOT_INLINE int cmp(int func, int a, int b) {
    switch (func) {
    case CMP_NEVER: return 0;
    case CMP_EQUAL: return a == b;
    case CMP_NOTEQUAL: return a != b;
    case CMP_LESS: return a < b;
    case CMP_LEQUAL: return a <= b;
    case CMP_GREATER: return a > b;
    case CMP_GEQUAL: return a >= b;
    default: return 1;
    }
}

/* 16-bit colours to RGBA8888 (red in the low byte) */
static inline uint32_t c565(uint32_t v) {
    uint32_t r = v & 31, g = (v >> 5) & 63, b = (v >> 11) & 31;
    return ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
}
static inline uint32_t c5551(uint32_t v) {
    uint32_t r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
    return ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16) | ((v & 0x8000) ? 0xFF000000u : 0);
}
static inline uint32_t c4444(uint32_t v) {
    return ((v & 15) * 0x11u) | ((((v >> 4) & 15) * 0x11u) << 8) | ((((v >> 8) & 15) * 0x11u) << 16) | ((((v >> 12) & 15) * 0x11u) << 24);
}
static inline uint32_t to565(uint32_t c) { return ((c >> 3) & 0x1F) | (((c >> 10) & 0x3F) << 5) | (((c >> 19) & 0x1F) << 11); }
static inline uint32_t to5551(uint32_t c) { return ((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10) | ((c >> 31) << 15); }
static inline uint32_t to4444(uint32_t c) { return ((c >> 4) & 0xF) | (((c >> 12) & 0xF) << 4) | (((c >> 20) & 0xF) << 8) | (((c >> 28) & 0xF) << 12); }

/* --- matrices (row vectors, as GE matrices are uploaded) ----------------------- */
static void v3_m43(float o[3], const float in[3], const float *m) {
    float x = in[0], y = in[1], z = in[2];
    o[0] = x * m[0] + y * m[3] + z * m[6] + m[9];
    o[1] = x * m[1] + y * m[4] + z * m[7] + m[10];
    o[2] = x * m[2] + y * m[5] + z * m[8] + m[11];
}
static void n3_m43(float o[3], const float in[3], const float *m) {
    float x = in[0], y = in[1], z = in[2];
    o[0] = x * m[0] + y * m[3] + z * m[6];
    o[1] = x * m[1] + y * m[4] + z * m[7];
    o[2] = x * m[2] + y * m[5] + z * m[8];
}
static void v3_m44(float o[4], const float in[3], const float *m) {
    for (int j = 0; j < 4; j++) o[j] = in[0] * m[j] + in[1] * m[4 + j] + in[2] * m[8 + j] + m[12 + j];
}
static void m43_to_44(float o[16], const float *m) {
    for (int r = 0; r < 4; r++) {
        o[r * 4 + 0] = m[r * 3 + 0]; o[r * 4 + 1] = m[r * 3 + 1]; o[r * 4 + 2] = m[r * 3 + 2];
        o[r * 4 + 3] = r == 3 ? 1.0f : 0.0f;
    }
}
static void m44_mul(float o[16], const float *a, const float *b) {
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, t, sizeof(t));
}
static float normalize_or_001(float v[3]) {
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len == 0.0f) { v[0] = 0; v[1] = 0; v[2] = 1; return 0.0f; }
    v[0] /= len; v[1] /= len; v[2] /= len;
    return len;
}
static inline float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* --- vertex format --------------------------------------------------------------- */
typedef struct {
    int tc, col, nrm, pos, wt, idx, nw, morph, through;
    int tcoff, coloff, nrmoff, posoff, onesize, size;
} VFmt;

static int align_up(int v, int a) { return a > 1 ? (v + a - 1) & ~(a - 1) : v; }

static void vfmt_init(VFmt *f, uint32_t vt) {
    static const uint8_t tcsize[4] = { 0, 2, 4, 8 }, tcalign[4] = { 0, 1, 2, 4 };
    static const uint8_t colsize[8] = { 0, 0, 0, 0, 2, 2, 2, 4 };
    static const uint8_t nrmsize[4] = { 0, 3, 6, 12 }, nrmalign[4] = { 0, 1, 2, 4 };
    static const uint8_t possize[4] = { 3, 3, 6, 12 }, posalign[4] = { 1, 1, 2, 4 };
    static const uint8_t wtsize[4] = { 0, 1, 2, 4 };
    int size = 0, biggest = 1;
    f->tc = vt & 3; f->col = (vt >> 2) & 7; f->nrm = (vt >> 5) & 3; f->pos = (vt >> 7) & 3;
    f->wt = (vt >> 9) & 3; f->idx = (vt >> 11) & 3; f->nw = ((vt >> 14) & 7) + 1;
    f->morph = ((vt >> 18) & 7) + 1; f->through = (vt >> 23) & 1;
    if (f->wt) { size += wtsize[f->wt] * f->nw; biggest = imax(biggest, wtsize[f->wt]); }
    if (f->tc) { size = align_up(size, tcalign[f->tc]); f->tcoff = size; size += tcsize[f->tc]; biggest = imax(biggest, tcalign[f->tc]); }
    if (f->col) { size = align_up(size, colsize[f->col]); f->coloff = size; size += colsize[f->col]; biggest = imax(biggest, colsize[f->col]); }
    if (f->nrm) { size = align_up(size, nrmalign[f->nrm]); f->nrmoff = size; size += nrmsize[f->nrm]; biggest = imax(biggest, nrmalign[f->nrm]); }
    size = align_up(size, posalign[f->pos]); f->posoff = size; size += possize[f->pos]; biggest = imax(biggest, posalign[f->pos]);
    f->onesize = align_up(size, biggest);
    f->size = f->onesize * f->morph;
}

typedef struct { float pos[3], nrm[3], uv[2]; uint32_t color; } RawV;

static uint32_t read_color(int fmt, const uint8_t *p) {
    switch (fmt) {
    case 4: return c565(rd16(p));
    case 5: return c5551(rd16(p));
    case 6: return c4444(rd16(p));
    case 7: return rd32(p);
    default: return 0;
    }
}

/* Decodes one vertex (with morphing and skinning) into model-space floats. */
static void decode_vertex(const VFmt *f, const uint8_t *p, RawV *o) {
    static const float tcscale[4] = { 0, 1.0f / 128.0f, 1.0f / 32768.0f, 1.0f };
    static const float nscale[4] = { 0, 1.0f / 128.0f, 1.0f / 32768.0f, 1.0f };
    float col[4] = { 0, 0, 0, 0 };
    memset(o, 0, sizeof(*o));
    for (int m = 0; m < f->morph; m++) {
        const uint8_t *q = p + m * f->onesize;
        float k = f->morph > 1 ? ge.morph[m] : 1.0f;
        if (f->tc) {
            const uint8_t *t = q + f->tcoff;
            float u, v;
            if (f->tc == 1) { u = t[0]; v = t[1]; }
            else if (f->tc == 2) { u = rd16(t); v = rd16(t + 2); }
            else { u = rdf(t); v = rdf(t + 4); }
            float s = (f->through && f->tc != 1) ? 1.0f : tcscale[f->tc];
            o->uv[0] += k * u * s; o->uv[1] += k * v * s;
        }
        if (f->col >= 4) {
            uint32_t c = read_color(f->col, q + f->coloff);
            for (int i = 0; i < 4; i++) col[i] += k * ((c >> (8 * i)) & 0xFF);
        }
        if (f->nrm) {
            const uint8_t *n = q + f->nrmoff;
            for (int i = 0; i < 3; i++) {
                float v = f->nrm == 1 ? (float)(int8_t)n[i] : f->nrm == 2 ? (float)(int16_t)rd16(n + 2 * i) : rdf(n + 4 * i);
                o->nrm[i] += k * v * nscale[f->nrm];
            }
        }
        const uint8_t *ps = q + f->posoff;
        if (f->through) {
            if (f->pos == 2) { o->pos[0] += k * (int16_t)rd16(ps); o->pos[1] += k * (int16_t)rd16(ps + 2); o->pos[2] += k * rd16(ps + 4); }
            else if (f->pos == 3) {
                float z = rdf(ps + 8);
                o->pos[0] += k * rdf(ps); o->pos[1] += k * rdf(ps + 4); o->pos[2] += k * (z > 65535.0f ? 65535.0f : z < 0.0f ? 0.0f : z);
            }  /* 8-bit positions decode to zero in through mode */
        } else {
            for (int i = 0; i < 3; i++) {
                float v = f->pos == 3 ? rdf(ps + 4 * i) : f->pos == 2 ? (int16_t)rd16(ps + 2 * i) / 32768.0f : (int8_t)ps[i] / 128.0f;
                o->pos[i] += k * v;
            }
        }
    }
    if (f->col >= 4) {
        uint32_t c = 0;
        for (int i = 0; i < 4; i++) c |= (uint32_t)iclamp((int)(col[i] + 0.5f), 0, 255) << (8 * i);
        o->color = c;
    }
    if (f->wt && !f->through) {
        float skin[12] = { 0 };
        for (int j = 0; j < f->nw && j < 8; j++) {
            float w = f->wt == 1 ? p[j] / 128.0f : f->wt == 2 ? rd16(p + 2 * j) / 32768.0f : rdf(p + 4 * j);
            if (w != 0.0f)
                for (int i = 0; i < 12; i++) skin[i] += w * ge.bone[j * 12 + i];
        }
        float t[3];
        v3_m43(t, o->pos, skin); memcpy(o->pos, t, sizeof(t));
        n3_m43(t, o->nrm, skin); memcpy(o->nrm, t, sizeof(t));
    }
}

/* --- transformed vertex ---------------------------------------------------------- */
typedef struct {
    float cx, cy, cz, cw;  /* clip coordinates */
    int x, y, z;           /* screen position in 1/16 pixel, depth 0..65535 */
    float s, t, q;         /* texture coordinates (normalized; texels in through mode) */
    float fog;
    int c0[4], c1[3];      /* primary RGBA, secondary RGB */
    int outside;           /* screen position out of range: the primitive is dropped */
} Vtx;

/* --- lighting ------------------------------------------------------------------- */
typedef struct {
    int on, directional, spot, powered, specular;
    float pos[3], att[3], dir[3], cutoff, exp;
    int amb[4], dif[4], spe[4];
} Light;

typedef struct {
    int through, light, fog, readuv, uvgen, projmode, negnrm, world_to_clip, clamp, needs_normal;
    float m[16], posfog[4], scale[3], add[3], uscale, vscale, uoff, voff;
    int offx, offy;
    Light l[4];
    int from_amb, from_dif, from_spe, set_c1, add_c1, any_spec;
    int mat_amb[4], mat_dif[4], mat_spe[4], base_amb[4], emissive[4];
    float spec_exp;
} XState;

static XState X;

static void factor(int o[4], uint32_t c) { for (int i = 0; i < 4; i++) o[i] = (int)((c >> (8 * i)) & 0xFF) * 2 + 1; }
static float light_pow(float v, float e) { if (e <= 0.0f) return 1.0f; return v > 0.0f ? powf(v, e) : v; }
static int scale512(float f) { int v = (int)ceilf(512.0f * f + 1.0f); return v > 512 ? 512 : v; }

static void setup_lighting(int has_color) {
    for (int i = 0; i < 4; i++) {
        Light *l = &X.l[i];
        uint32_t lt = D(GE_CMD_LIGHTTYPE0 + i);
        l->on = D(GE_CMD_LIGHTENABLE0 + i) & 1;
        if (!l->on) continue;
        l->powered = (lt & 3) == 2;
        l->specular = (lt & 3) == 1;
        l->directional = ((lt >> 8) & 3) == 0;
        l->spot = ((lt >> 8) & 3) >= 2;
        factor(l->amb, D(GE_CMD_LAC0 + 3 * i));
        factor(l->dif, D(GE_CMD_LAC0 + 3 * i + 1));
        factor(l->spe, D(GE_CMD_LAC0 + 3 * i + 2));
        for (int k = 0; k < 3; k++) {
            l->pos[k] = F(GE_CMD_LX0 + 3 * i + k);
            l->att[k] = F(GE_CMD_LKA0 + 3 * i + k);
            l->dir[k] = F(GE_CMD_LDX0 + 3 * i + k);
        }
        if (l->directional) normalize_or_001(l->pos);
        if (l->spot) {
            float len = sqrtf(dot3(l->dir, l->dir));
            if (len > 0) for (int k = 0; k < 3; k++) l->dir[k] /= len;
            l->cutoff = F(GE_CMD_LKO0 + i);
            if (isnan(l->cutoff) && signbit(l->cutoff)) l->cutoff = 0.0f;
            l->exp = F(GE_CMD_LKS0 + i);
            if (!(l->exp > 0.0f)) l->exp = isnan(l->exp) && !signbit(l->exp) ? INFINITY : 0.0f;
        }
    }
    int mu = has_color ? (int)(D(GE_CMD_MATERIALUPDATE) & 7) : 0;
    X.from_amb = mu & 1; X.from_dif = (mu >> 1) & 1; X.from_spe = (mu >> 2) & 1;
    factor(X.mat_amb, (D(GE_CMD_MATERIALAMBIENT) & 0xFFFFFF) | (D(GE_CMD_MATERIALALPHA) & 0xFF) << 24);
    factor(X.mat_dif, D(GE_CMD_MATERIALDIFFUSE) & 0xFFFFFF);
    factor(X.mat_spe, D(GE_CMD_MATERIALSPECULAR) & 0xFFFFFF);
    factor(X.base_amb, (D(GE_CMD_AMBIENTCOLOR) & 0xFFFFFF) | (D(GE_CMD_AMBIENTALPHA) & 0xFF) << 24);
    uint32_t em = D(GE_CMD_MATERIALEMISSIVE);
    for (int i = 0; i < 3; i++) X.emissive[i] = (em >> (8 * i)) & 0xFF;
    X.emissive[3] = 0;
    X.spec_exp = F(GE_CMD_MATERIALSPECULARCOEF);
    if (!(X.spec_exp > 0.0f)) X.spec_exp = isnan(X.spec_exp) && !signbit(X.spec_exp) ? INFINITY : 0.0f;
    X.any_spec = 0;
    for (int i = 0; i < 4; i++) X.any_spec |= X.l[i].on && X.l[i].specular;
    X.set_c1 = (D(GE_CMD_LIGHTMODE) & 1) && X.any_spec;
    X.add_c1 = !(D(GE_CMD_LIGHTMODE) & 1) && X.any_spec;
}

static void do_lighting(Vtx *v, const float wp[3], const float wn[3]) {
    int cf[4], fin[4], spec[4] = { 0, 0, 0, 0 };
    uint32_t vc = (uint32_t)v->c0[0] | (uint32_t)v->c0[1] << 8 | (uint32_t)v->c0[2] << 16 | (uint32_t)v->c0[3] << 24;
    factor(cf, vc);
    const int *mac = X.from_amb ? cf : X.mat_amb;
    for (int i = 0; i < 4; i++) fin[i] = X.emissive[i] + ((mac[i] * X.base_amb[i]) >> 10);
    for (int li = 0; li < 4; li++) {
        const Light *l = &X.l[li];
        if (!l->on) continue;
        float L[3] = { l->pos[0], l->pos[1], l->pos[2] }, attspot = 1.0f;
        if (!l->directional) {
            L[0] -= wp[0]; L[1] -= wp[1]; L[2] -= wp[2];
            float d = normalize_or_001(L);
            float att = 1.0f / (l->att[0] + l->att[1] * d + l->att[2] * d * d);
            attspot = !(att > 0.0f) ? 0.0f : att > 1.0f ? 1.0f : att;
        }
        if (l->spot) {
            float raw = dot3(l->dir, L), sp;
            if (isnan(raw)) raw = signbit(raw) ? 0.0f : 1.0f;
            sp = raw >= l->cutoff ? light_pow(raw, l->exp) : 0.0f;
            if (isnan(sp)) sp = 0.0f;
            attspot *= sp;
        }
        int a512 = scale512(attspot);
        for (int i = 0; i < 4; i++) fin[i] += (l->amb[i] * mac[i] * a512) >> 19;
        float df = dot3(L, wn);
        if (l->powered) df = light_pow(df, X.spec_exp);
        if (df > 0.0f) {
            const int *mdc = X.from_dif ? cf : X.mat_dif;
            int d512 = scale512(attspot * df);
            for (int i = 0; i < 4; i++) fin[i] += (l->dif[i] * mdc[i] * d512) >> 19;
        }
        if (l->specular && df >= 0.0f) {
            float H[3] = { L[0], L[1], L[2] + 1.0f };
            normalize_or_001(H);
            float sf = light_pow(dot3(H, wn), X.spec_exp);
            if (sf > 0.0f) {
                const int *msc = X.from_spe ? cf : X.mat_spe;
                int s512 = scale512(attspot * sf);
                for (int i = 0; i < 4; i++) spec[i] += (l->spe[i] * msc[i] * s512) >> 19;
            }
        }
    }
    for (int i = 0; i < 4; i++) v->c0[i] = iclamp(fin[i] + (X.add_c1 && i < 3 ? spec[i] : 0), 0, 255);
    for (int i = 0; i < 3; i++) v->c1[i] = X.set_c1 ? iclamp(spec[i], 0, 255) : 0;
}

/* --- transform -------------------------------------------------------------------- */
static void setup_transform(const VFmt *f) {
    X.through = f->through;
    X.light = (D(GE_CMD_LIGHTINGENABLE) & 1) && !f->through;
    X.fog = (D(GE_CMD_FOGENABLE) & 1) && !f->through;
    X.readuv = !(D(GE_CMD_CLEARMODE) & 1) && (D(GE_CMD_TEXTUREMAPENABLE) & 1) && f->tc;
    X.negnrm = D(GE_CMD_REVERSENORMAL) & 1;
    X.uvgen = D(GE_CMD_TEXMAPMODE) & 3;
    if (X.uvgen == 3) X.uvgen = 0;
    X.projmode = (D(GE_CMD_TEXMAPMODE) >> 8) & 3;
    X.clamp = D(GE_CMD_DEPTHCLAMPENABLE) & 1;
    X.uscale = F(GE_CMD_TEXSCALEU); X.vscale = F(GE_CMD_TEXSCALEV);
    X.uoff = F(GE_CMD_TEXOFFSETU); X.voff = F(GE_CMD_TEXOFFSETV);
    X.offx = D(GE_CMD_OFFSETX) & 0xFFFF; X.offy = D(GE_CMD_OFFSETY) & 0xFFFF;
    if (f->through) return;
    int uses_worldpos = 0;
    if (X.light) {
        setup_lighting(f->col >= 4);
        for (int i = 0; i < 4; i++) uses_worldpos |= X.l[i].on && !X.l[i].directional;
    }
    X.needs_normal = X.light || X.uvgen == 2;
    float world[16], view[16], wv[16];
    m43_to_44(world, ge.world);
    m43_to_44(view, ge.view);
    m44_mul(wv, world, view);
    X.world_to_clip = uses_worldpos;
    if (uses_worldpos) m44_mul(X.m, view, ge.proj);
    else m44_mul(X.m, wv, ge.proj);
    if (X.fog) {
        float end = F(GE_CMD_FOG1), slope = F(GE_CMD_FOG2);
        X.posfog[0] = wv[2]; X.posfog[1] = wv[6]; X.posfog[2] = wv[10]; X.posfog[3] = wv[14] + end;
        if (isnan(end) || isinf(end)) {
            int sign = signbit(end) != 0;
            if (signbit(slope)) sign = !sign;
            if (slope == 0.0f) sign = 1;
            X.posfog[0] = X.posfog[1] = X.posfog[2] = 0.0f;
            X.posfog[3] = sign ? 0.0f : 1.0f;
        } else {
            if (isnan(slope) || isinf(slope)) slope = signbit(slope) ? -262144.0f : 262144.0f;
            for (int i = 0; i < 4; i++) X.posfog[i] *= slope;
        }
    }
    X.scale[0] = F(GE_CMD_VIEWPORTXSCALE); X.scale[1] = F(GE_CMD_VIEWPORTYSCALE); X.scale[2] = F(GE_CMD_VIEWPORTZSCALE);
    X.add[0] = F(GE_CMD_VIEWPORTXCENTER); X.add[1] = F(GE_CMD_VIEWPORTYCENTER); X.add[2] = F(GE_CMD_VIEWPORTZCENTER);
}

#define SCREEN_BOUND (4095.0f + 15.5f / 16.0f)

/* clip -> screen (ClipToScreen); returns 1 when the position is outside the drawable range */
static int project(Vtx *v, int always_check) {
    float sx = v->cx * X.scale[0] / v->cw + X.add[0];
    float sy = v->cy * X.scale[1] / v->cw + X.add[1];
    float sz = v->cz * X.scale[2] / v->cw + X.add[2];
    int out = 0;
    if (X.clamp) {
        if ((always_check || v->cz > -v->cw) && (sx >= SCREEN_BOUND || sy >= SCREEN_BOUND || sx < 0 || sy < 0)) out = 1;
        sz = sz < 0.0f ? 0.0f : sz > 65535.0f ? 65535.0f : sz;
    } else if (sx > SCREEN_BOUND || sy >= SCREEN_BOUND || sx < 0 || sy < 0 || sz < 0.0f || sz >= 65536.0f) {
        out = 1;
    }
    if (!(sx == sx) || !(sy == sy)) out = 1;  /* NaN */
    if (out) return 1;
    v->x = (int)(sx * 16.0f + 0.375f - X.offx);
    v->y = (int)(sy * 16.0f + 0.375f - X.offy);
    v->z = (int)sz;
    return 0;
}

static float g_last_uv[2], g_last_nrm[3];

static void transform_vertex(const VFmt *f, const RawV *r, Vtx *v) {
    float uv[2], nrm[3];
    if (X.readuv) {
        uv[0] = r->uv[0]; uv[1] = r->uv[1];
        if (!f->through && X.uvgen == 0) { uv[0] = uv[0] * X.uscale + X.uoff; uv[1] = uv[1] * X.vscale + X.voff; }
        g_last_uv[0] = uv[0]; g_last_uv[1] = uv[1];
    } else {
        uv[0] = g_last_uv[0]; uv[1] = g_last_uv[1];
    }
    v->s = uv[0]; v->t = uv[1]; v->q = 0.0f;
    if (f->nrm) memcpy(g_last_nrm, r->nrm, sizeof(g_last_nrm));
    memcpy(nrm, g_last_nrm, sizeof(nrm));
    if (X.negnrm) { nrm[0] = -nrm[0]; nrm[1] = -nrm[1]; nrm[2] = -nrm[2]; }
    uint32_t c = f->col >= 4 ? r->color : (D(GE_CMD_MATERIALAMBIENT) & 0xFFFFFF) | (D(GE_CMD_MATERIALALPHA) & 0xFF) << 24;
    for (int i = 0; i < 4; i++) v->c0[i] = (c >> (8 * i)) & 0xFF;
    v->c1[0] = v->c1[1] = v->c1[2] = 0;
    v->outside = 0;
    if (f->through) {
        v->x = (int)(r->pos[0] * 16.0f);
        v->y = (int)(r->pos[1] * 16.0f);
        v->z = (int)r->pos[2];
        v->cx = r->pos[0]; v->cy = r->pos[1]; v->cz = r->pos[2]; v->cw = 1.0f;
        v->fog = 1.0f;
        return;
    }
    float wp[3] = { 0, 0, 0 }, clip[4];
    if (X.world_to_clip) { v3_m43(wp, r->pos, ge.world); v3_m44(clip, wp, X.m); }
    else v3_m44(clip, r->pos, X.m);
    v->cx = clip[0]; v->cy = clip[1]; v->cz = clip[2]; v->cw = clip[3];
    if (project(v, 0)) { v->outside = 1; return; }
    v->fog = X.fog ? X.posfog[0] * r->pos[0] + X.posfog[1] * r->pos[1] + X.posfog[2] * r->pos[2] + X.posfog[3] : 1.0f;
    float wn[3] = { 0, 0, 1 };
    if (X.needs_normal) { n3_m43(wn, nrm, ge.world); normalize_or_001(wn); }
    if (X.uvgen == 1) {
        float src[3], stq[3];
        switch (X.projmode) {
        case 0: memcpy(src, r->pos, sizeof(src)); break;
        case 1: src[0] = uv[0]; src[1] = uv[1]; src[2] = 0.0f; break;
        case 2: { float len = sqrtf(dot3(nrm, nrm)); for (int i = 0; i < 3; i++) src[i] = len > 0 ? nrm[i] / len : nrm[i]; break; }
        default: memcpy(src, nrm, sizeof(src)); break;
        }
        v3_m43(stq, src, ge.tgen);
        v->s = stq[0]; v->t = stq[1]; v->q = stq[2];
    } else if (X.uvgen == 2) {
        uint32_t ls = D(GE_CMD_TEXSHADELS);
        float L[3];
        for (int k = 0; k < 3; k++) L[k] = F(GE_CMD_LX0 + 3 * (ls & 3) + k);
        normalize_or_001(L);
        v->s = (dot3(L, wn) + 1.0f) / 2.0f;
        for (int k = 0; k < 3; k++) L[k] = F(GE_CMD_LX0 + 3 * ((ls >> 8) & 3) + k);
        normalize_or_001(L);
        v->t = (dot3(L, wn) + 1.0f) / 2.0f;
    }
    if (X.light) do_lighting(v, wp, wn);
}

/* --- rasterizer state ------------------------------------------------------------ */
/* Primitives are queued with a snapshot of this state and drawn at the next flush, in
 * parallel: the rows are cut into bands, each thread takes the next free band and draws
 * every queued primitive that touches it, in order, so each pixel sees the same drawing
 * order as with one thread. */
typedef struct {
    uint8_t *fb, *zb;
    int fbstride, zbstride, fmt, bpp;
    int clear, clear_color, clear_stencil, clear_depth, through, gouraud;
    int sx1, sy1, sx2, sy2;
    int depth_range, minz, maxz;
    int zfunc, zwrite, afunc, aref, amask, ctest, cfunc;
    uint32_t cref, cmask;
    int stest, sfunc, sref, smask, sfail, zfail, zpass;
    int blend, bsrc, bdst, beq;
    uint32_t fixa, fixb;
    int fog;
    uint32_t fogcol, wmask;
    int logic, lop;
    /* texturing */
    int tex, tfmt, swz, clampS, clampT, minf, magf, mipf, maxlevel, func, rgba, dbl, lodmode, lodoff;
    int needs_lod, proj;
    float lodslope;
    uint32_t env;
    int clutfmt, clutshift, clutmask, clutoff, clutshared;
    uint32_t clut_idx;        /* entry in the CLUT pool */
    const uint8_t *clut;      /* set when the queue runs */
    int use_pal;              /* CLUT4/CLUT8: pal holds the colours with shift/mask/offset applied */
    uint32_t pal[256];
    const uint8_t *tptr[8];
    int tbufw[8], tw[8], th[8];
} Raster;

/* Raster state is reached through a pointer named rs in every function below. */
#define R (*rs)


/* Per-thread drawing context: the state of the primitive being drawn and the band of rows. */
typedef struct {
    const Raster *rs;
    int ylo, yhi;
    uint64_t shaded, written;
} Ctx;

#define BAND_ROWS 4

static const int tex_bits[16] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0 };

static void setup_raster(Raster *rs, int through) {
    memset(rs, 0, sizeof(*rs));
    R.through = through;
    R.clear = D(GE_CMD_CLEARMODE) & 1;
    R.clear_color = (D(GE_CMD_CLEARMODE) >> 8) & 1;
    R.clear_stencil = (D(GE_CMD_CLEARMODE) >> 9) & 1;
    R.clear_depth = (D(GE_CMD_CLEARMODE) >> 10) & 1;
    R.fmt = D(GE_CMD_FRAMEBUFPIXFORMAT) & 3;
    R.bpp = R.fmt == 3 ? 4 : 2;
    R.fb = rt_ptr(VRAM | (D(GE_CMD_FRAMEBUFPTR) & 0x1FFFF0));
    R.fbstride = D(GE_CMD_FRAMEBUFWIDTH) & 0x7FC;
    R.zb = rt_ptr(VRAM | (D(GE_CMD_ZBUFPTR) & 0x1FFFF0));
    R.zbstride = D(GE_CMD_ZBUFWIDTH) & 0x7FC;
    R.gouraud = !R.clear && (D(GE_CMD_SHADEMODE) & 1);
    R.sx1 = D(GE_CMD_SCISSOR1) & 0x3FF; R.sy1 = (D(GE_CMD_SCISSOR1) >> 10) & 0x3FF;
    R.sx2 = D(GE_CMD_SCISSOR2) & 0x3FF; R.sy2 = (D(GE_CMD_SCISSOR2) >> 10) & 0x3FF;
    R.depth_range = !through;
    R.minz = D(GE_CMD_MINZ) & 0xFFFF; R.maxz = D(GE_CMD_MAXZ) & 0xFFFF;
    int ztest = D(GE_CMD_ZTESTENABLE) & 1;
    uint32_t mask = (D(GE_CMD_MASKRGB) & 0xFFFFFF) | (D(GE_CMD_MASKALPHA) & 0xFF) << 24;
    R.stest = 0;
    if (R.clear) {
        R.zfunc = CMP_ALWAYS; R.zwrite = 0; R.afunc = CMP_ALWAYS; R.ctest = 0; R.blend = 0; R.fog = 0; R.logic = 0;
        if (!R.clear_stencil || R.fmt == 0) mask |= 0xFF000000u;
    } else {
        R.zfunc = ztest ? (int)(D(GE_CMD_ZTEST) & 7) : CMP_ALWAYS;
        R.zwrite = ztest && !(D(GE_CMD_ZWRITEDISABLE) & 1);
        R.afunc = (D(GE_CMD_ALPHATESTENABLE) & 1) ? (int)(D(GE_CMD_ALPHATEST) & 7) : CMP_ALWAYS;
        R.amask = (D(GE_CMD_ALPHATEST) >> 16) & 0xFF;
        R.aref = ((D(GE_CMD_ALPHATEST) >> 8) & 0xFF) & R.amask;
        R.ctest = (D(GE_CMD_COLORTESTENABLE) & 1) && (D(GE_CMD_COLORTEST) & 3) != CMP_ALWAYS;
        R.cfunc = D(GE_CMD_COLORTEST) & 3;
        R.cmask = D(GE_CMD_COLORTESTMASK) & 0xFFFFFF;
        R.cref = D(GE_CMD_COLORREF) & R.cmask;
        if ((D(GE_CMD_STENCILTESTENABLE) & 1) && R.fmt != 0) {
            uint32_t st = D(GE_CMD_STENCILTEST), so = D(GE_CMD_STENCILOP);
            R.stest = 1;
            R.sfunc = st & 7;
            R.smask = (st >> 16) & 0xFF;
            R.sref = (st >> 8) & 0xFF;
            R.sfail = (so & 7) <= 5 ? (int)(so & 7) : 0;
            R.zfail = ((so >> 8) & 7) <= 5 ? (int)((so >> 8) & 7) : 0;
            R.zpass = ((so >> 16) & 7) <= 5 ? (int)((so >> 16) & 7) : 0;
            if (!ztest) R.zfail = 0;
        }
        if (!R.stest) mask |= 0xFF000000u;
        uint32_t bm = D(GE_CMD_BLENDMODE);
        R.beq = (bm >> 8) & 7;
        R.blend = (D(GE_CMD_ALPHABLENDENABLE) & 1) && R.beq <= 5;
        R.bsrc = bm & 0xF; R.bdst = (bm >> 4) & 0xF;
        R.fixa = D(GE_CMD_BLENDFIXEDA); R.fixb = D(GE_CMD_BLENDFIXEDB);
        R.fog = (D(GE_CMD_FOGENABLE) & 1) && !through;
        R.fogcol = D(GE_CMD_FOGCOLOR);
        R.logic = (D(GE_CMD_LOGICOPENABLE) & 1) && (D(GE_CMD_LOGICOP) & 0xF) != 3;
        R.lop = D(GE_CMD_LOGICOP) & 0xF;
    }
    R.wmask = mask;

    R.tex = (D(GE_CMD_TEXTUREMAPENABLE) & 1) && !R.clear;
    if (!R.tex) return;
    uint32_t tm = D(GE_CMD_TEXMODE), tf = D(GE_CMD_TEXFILTER), tfn = D(GE_CMD_TEXFUNC), cf = D(GE_CMD_CLUTFORMAT);
    R.tfmt = D(GE_CMD_TEXFORMAT) & 0xF;
    R.swz = tm & 1;
    R.maxlevel = (tf & 4) ? (int)((tm >> 16) & 7) : 0;
    R.clutshared = R.tfmt != 4 || R.maxlevel == 0 || !(tm & 0x100);
    R.minf = tf & 1; R.magf = (tf >> 8) & 1; R.mipf = (tf >> 1) & 1;
    R.clampS = D(GE_CMD_TEXWRAP) & 1; R.clampT = (D(GE_CMD_TEXWRAP) >> 8) & 1;
    R.func = tfn & 7; R.rgba = (tfn >> 8) & 1; R.dbl = (tfn >> 16) & 1;
    R.env = D(GE_CMD_TEXENVCOLOR);
    R.clutfmt = cf & 3; R.clutshift = (cf >> 2) & 0x1F; R.clutmask = (cf >> 8) & 0xFF; R.clutoff = ((cf >> 16) & 0x1F) << 4;
    R.lodmode = D(GE_CMD_TEXLEVEL) & 3;
    R.lodoff = (int8_t)((D(GE_CMD_TEXLEVEL) >> 16) & 0xFF);
    R.lodslope = F(GE_CMD_TEXLODSLOPE);
    R.proj = X.uvgen == 1 && !through;
    for (int i = 0; i <= R.maxlevel; i++) {
        uint32_t addr = (D(GE_CMD_TEXADDR0 + i) & 0xFFFFF0) | ((D(GE_CMD_TEXBUFWIDTH0 + i) << 8) & 0x0F000000);
        static const uint32_t align_mask[16] = { 0x7F8, 0x7F8, 0x7F8, 0x7FC, 0x7E0, 0x7F0, 0x7F8, 0x7FC, 0x7FF, 0x7FF, 0x7FF };
        int bufw = (int)(D(GE_CMD_TEXBUFWIDTH0 + i) & align_mask[R.tfmt]);
        if (bufw == 0 && R.tfmt <= 10) bufw = 128 / tex_bits[R.tfmt];
        R.tptr[i] = rt_ptr(addr);
        R.tbufw[i] = bufw;
        R.tw[i] = 1 << (D(GE_CMD_TEXSIZE0 + i) & 0xF);
        R.th[i] = 1 << ((D(GE_CMD_TEXSIZE0 + i) >> 8) & 0xF);
    }
    /* derivatives are only needed when the level or the filter can change */
    R.needs_lod = R.maxlevel > 0 || R.minf != R.magf || R.lodmode == 1;
}

/* --- texture sampling ------------------------------------------------------------ */
HOT_INLINE uint32_t clut_lookup(const Raster *rs, uint32_t index, int level) {
    uint32_t i = ((index >> R.clutshift) & (uint32_t)R.clutmask) | (R.clutoff & (R.clutfmt == 3 ? 0xFF : 0x1FF));
    if (!R.clutshared) i += level * 16;
    if (R.clutfmt == 3) return rd32(R.clut + (i & 0xFF) * 4);
    uint32_t c = rd16(R.clut + (i & 0x1FF) * 2);
    return R.clutfmt == 0 ? c565(c) : R.clutfmt == 1 ? c5551(c) : c4444(c);
}

HOT_INLINE uint32_t texel_offset(const Raster *rs, int bits, int bufw, int u, int v) {
    if (!R.swz) return (uint32_t)v * (bufw * bits >> 3) + ((uint32_t)u * bits >> 3);
    uint32_t bx = (uint32_t)u * bits >> 3, pitch = (uint32_t)bufw * bits >> 3;
    return ((uint32_t)(v >> 3) * (pitch >> 4) + (bx >> 4)) * 128 + (v & 7) * 16 + (bx & 15);
}

static uint32_t dxt_texel(int fmt, const uint8_t *base, int bufw, int u, int v) {
    int bs = fmt == 8 ? 8 : 16;
    const uint8_t *b = base + ((v >> 2) * (bufw >> 2) + (u >> 2)) * bs;
    const uint8_t *cb = fmt == 8 ? b : b + 8;
    /* PSP DXT colour blocks hold the 32-bit index word first, then the two 565 colours */
    uint32_t lines = rd32(cb), c0 = rd16(cb + 4), c1 = rd16(cb + 6);
    uint32_t k0 = c565(c0), k1 = c565(c1), pal[4];
    pal[0] = k0; pal[1] = k1;
    if (c0 > c1 || fmt != 8) {
        uint32_t a = 0, bb = 0;
        for (int i = 0; i < 3; i++) {
            uint32_t x = (k0 >> (8 * i)) & 0xFF, y = (k1 >> (8 * i)) & 0xFF;
            a |= ((2 * x + y) / 3) << (8 * i);
            bb |= ((x + 2 * y) / 3) << (8 * i);
        }
        pal[2] = a | 0xFF000000u; pal[3] = bb | 0xFF000000u;
    } else {
        uint32_t a = 0;
        for (int i = 0; i < 3; i++) a |= ((((k0 >> (8 * i)) & 0xFF) + ((k1 >> (8 * i)) & 0xFF)) / 2) << (8 * i);
        pal[2] = a | 0xFF000000u; pal[3] = 0;
    }
    uint32_t px = pal[(lines >> (2 * ((v & 3) * 4 + (u & 3)))) & 3];
    if (fmt == 8) return px;
    if (fmt == 9) {
        uint32_t a4 = (rd16(b + 2 * (v & 3)) >> (4 * (u & 3))) & 0xF;
        return (px & 0xFFFFFF) | (a4 * 0x11u) << 24;
    }
    /* DXT5 alpha block: 48 bits of 3-bit indices, then the two alpha end points */
    uint64_t bits = (uint64_t)rd32(b) | ((uint64_t)rd16(b + 4) << 32);
    uint32_t a0 = b[6], a1 = b[7], ai = (uint32_t)(bits >> (3 * ((v & 3) * 4 + (u & 3)))) & 7, a;
    if (ai == 0) a = a0;
    else if (ai == 1) a = a1;
    else if (a0 > a1) a = ((8 - ai) * a0 + (ai - 1) * a1) / 7;
    else if (ai == 6) a = 0;
    else if (ai == 7) a = 255;
    else a = ((6 - ai) * a0 + (ai - 1) * a1) / 5;
    return (px & 0xFFFFFF) | a << 24;
}

HOT_INLINE uint32_t fetch(const Raster *rs, int level, int u, int v) {
    const uint8_t *base = R.tptr[level];
    int bufw = R.tbufw[level];
    switch (R.tfmt) {
    case 0: return c565(rd16(base + texel_offset(rs, 16, bufw, u, v)));
    case 1: return c5551(rd16(base + texel_offset(rs, 16, bufw, u, v)));
    case 2: return c4444(rd16(base + texel_offset(rs, 16, bufw, u, v)));
    case 3: return rd32(base + texel_offset(rs, 32, bufw, u, v));
    case 4: {
        uint8_t b = base[texel_offset(rs, 4, bufw, u, v)];
        return R.use_pal ? R.pal[(u & 1) ? b >> 4 : b & 0xF] : clut_lookup(rs, (u & 1) ? b >> 4 : b & 0xF, level);
    }
    case 5: { uint8_t b = base[texel_offset(rs, 8, bufw, u, v)]; return R.use_pal ? R.pal[b] : clut_lookup(rs, b, level); }
    case 6: return clut_lookup(rs, rd16(base + texel_offset(rs, 16, bufw, u, v)), level);
    case 7: return clut_lookup(rs, rd32(base + texel_offset(rs, 32, bufw, u, v)), level);
    case 8: case 9: case 10: return dxt_texel(R.tfmt, base, bufw, u, v);
    default: return 0;
    }
}

HOT_INLINE int wrap_clamp(int v, int size, int clamp) {
    if (clamp) { if (v >= size - 1) v = size - 1; if (v >= 511) v = 511; return v < 0 ? 0 : v; }
    return v & (size - 1) & 511;
}

HOT_INLINE void sample_level(const Raster *rs, int out[4], int level, float s, float t, int linear) {
    int w = R.tw[level], h = R.th[level];
    if (!linear) {
        int u = wrap_clamp((int)(s * w * 256.0f) >> 8, w, R.clampS), v = wrap_clamp((int)(t * h * 256.0f) >> 8, h, R.clampT);
        uint32_t c = fetch(rs, level, u, v);
        for (int i = 0; i < 4; i++) out[i] = (c >> (8 * i)) & 0xFF;
        return;
    }
    int bu = (int)(s * w * 256.0f) - 128, bv = (int)(t * h * 256.0f) - 128;
    int fu = (bu >> 4) & 15, fv = (bv >> 4) & 15;
    bu >>= 8; bv >>= 8;
    int u0 = wrap_clamp(bu, w, R.clampS), u1 = wrap_clamp(bu + 1, w, R.clampS);
    int v0 = wrap_clamp(bv, h, R.clampT), v1 = wrap_clamp(bv + 1, h, R.clampT);
    uint32_t tl, tr, bl, br;
    if (R.use_pal && R.swz) {
        /* swizzled palette texture (most of the game's pixels): the four addresses share their
         * row and column parts; 16-byte x 8-row blocks, block rows of pitch*8 bytes */
        const uint8_t *base = R.tptr[level];
        uint32_t pitch = (uint32_t)R.tbufw[level] * (R.tfmt == 4 ? 4u : 8u) / 8u;
        uint32_t r0 = (uint32_t)(v0 >> 3) * (pitch >> 4) * 128 + (uint32_t)(v0 & 7) * 16;
        uint32_t r1 = (uint32_t)(v1 >> 3) * (pitch >> 4) * 128 + (uint32_t)(v1 & 7) * 16;
        if (R.tfmt == 5) {
            uint32_t c0 = (uint32_t)(u0 >> 4) * 128 + (uint32_t)(u0 & 15), c1 = (uint32_t)(u1 >> 4) * 128 + (uint32_t)(u1 & 15);
            tl = R.pal[base[r0 + c0]]; tr = R.pal[base[r0 + c1]]; bl = R.pal[base[r1 + c0]]; br = R.pal[base[r1 + c1]];
        } else {
            uint32_t x0 = (uint32_t)u0 >> 1, x1 = (uint32_t)u1 >> 1;
            uint32_t c0 = (x0 >> 4) * 128 + (x0 & 15), c1 = (x1 >> 4) * 128 + (x1 & 15);
            int s0 = (u0 & 1) * 4, s1 = (u1 & 1) * 4;
            tl = R.pal[(base[r0 + c0] >> s0) & 15]; tr = R.pal[(base[r0 + c1] >> s1) & 15];
            bl = R.pal[(base[r1 + c0] >> s0) & 15]; br = R.pal[(base[r1 + c1] >> s1) & 15];
        }
    } else {
        tl = fetch(rs, level, u0, v0); tr = fetch(rs, level, u1, v0); bl = fetch(rs, level, u0, v1); br = fetch(rs, level, u1, v1);
    }
    for (int i = 0; i < 4; i++) {
        int sh = 8 * i;
        int top = (int)((tl >> sh) & 0xFF) * (16 - fu) + (int)((tr >> sh) & 0xFF) * fu;
        int bot = (int)((bl >> sh) & 0xFF) * (16 - fu) + (int)((br >> sh) & 0xFF) * fu;
        out[i] = (top * (16 - fv) + bot * fv) >> 8;
    }
}

static int tex_log2(float delta) {
    uint32_t b;
    memcpy(&b, &delta, 4);
    return (int)((b >> 19) & 0x0FFF) - 127 * 16;
}

/* Samples the texture and applies the texture function to colour c (RGBA). ds/dt are the
 * texture-coordinate steps to the next pixel in x and y (for the level of detail). */
static void texture(const Raster *rs, int c[4], float s, float t, float ds, float dt, float w) {
    int level = 0, frac = 0, linear = R.magf;
    if (R.needs_lod) {
        int detail;
        if (R.lodmode == 0) {
            float a = fabsf(ds * R.tw[0]), b = fabsf(dt * R.th[0]);
            detail = tex_log2(a > b ? a : b);
        } else if (R.lodmode == 1) {
            detail = tex_log2(2.0f * w * R.lodslope);
        } else {
            detail = 0;
        }
        detail += R.lodoff;
        if (detail > 0 && R.maxlevel > 0) {
            int level8 = imin(detail, R.maxlevel * 16);
            if (!R.mipf) level8 += 8;
            level = level8 >> 4;
            frac = R.mipf ? level8 & 15 : 0;
            if (level > R.maxlevel) level = R.maxlevel;
        }
        linear = detail > 0 ? R.minf : R.magf;
    }
    int tc[4];
    sample_level(rs, tc, level, s, t, linear);
    if (frac && level < R.maxlevel) {
        int t2[4];
        sample_level(rs, t2, level + 1, s, t, linear);
        for (int i = 0; i < 4; i++) tc[i] = (t2[i] * frac + tc[i] * (16 - frac)) >> 4;
    }
    int r[3], a;
    switch (R.func) {
    case 0:  /* modulate */
        for (int i = 0; i < 3; i++) r[i] = (c[i] + 1) * tc[i] * (R.dbl ? 2 : 1) >> 8;
        a = R.rgba ? (c[3] + 1) * tc[3] >> 8 : c[3];
        break;
    case 1:  /* decal */
        if (R.rgba) for (int i = 0; i < 3; i++) r[i] = ((c[i] + 1) * (255 - tc[3]) + (tc[i] + 1) * tc[3]) >> (R.dbl ? 7 : 8);
        else for (int i = 0; i < 3; i++) r[i] = tc[i] * (R.dbl ? 2 : 1);
        a = c[3];
        break;
    case 2:  /* blend with the environment colour */
        for (int i = 0; i < 3; i++) r[i] = ((255 - tc[i]) * c[i] + tc[i] * (int)((R.env >> (8 * i)) & 0xFF) + 255) >> (R.dbl ? 7 : 8);
        a = R.rgba ? (c[3] + 1) * tc[3] >> 8 : c[3];
        break;
    case 3:  /* replace */
        for (int i = 0; i < 3; i++) r[i] = tc[i] * (R.dbl ? 2 : 1);
        a = R.rgba ? tc[3] : c[3];
        break;
    default: /* add */
        for (int i = 0; i < 3; i++) r[i] = (c[i] + tc[i]) * (R.dbl ? 2 : 1);
        a = R.rgba ? (c[3] + 1) * tc[3] >> 8 : c[3];
        break;
    }
    c[0] = r[0]; c[1] = r[1]; c[2] = r[2]; c[3] = a;
}

/* --- per-pixel pipeline ----------------------------------------------------------- */
static inline uint32_t read_fb(const Raster *rs, const uint8_t *p, uint32_t *raw) {
    switch (R.fmt) {
    case 0: *raw = rd16(p); return c565(*raw) & 0x00FFFFFF;
    case 1: *raw = rd16(p); return c5551(*raw);
    case 2: *raw = rd16(p); return c4444(*raw);
    default: *raw = rd32(p); return *raw;
    }
}

static inline int stencil_of(const Raster *rs, uint32_t c) { return R.fmt == 0 ? 0 : (int)(c >> 24); }

static int stencil_op(const Raster *rs, int op, int old) {
    switch (op) {
    case 1: return 0;
    case 2: return R.sref;
    case 3: return ~old & 0xFF;
    case 4:  /* increment */
        if (R.fmt == 3) return old != 0xFF ? old + 1 : old;
        if (R.fmt == 1) return 0xFF;
        return old < 0xF0 ? old + 0x10 : old;
    case 5:  /* decrement */
        if (R.fmt == 2) return old >= 0x10 ? old - 0x10 : old;
        if (R.fmt == 1) return 0;
        return old ? old - 1 : 0;
    default: return old;
    }
}

static void write_fb(const Raster *rs, uint8_t *p, uint32_t c, uint32_t raw_old, uint32_t mask) {
    switch (R.fmt) {
    case 0: { uint32_t m = to565(mask), v = (to565(c) & ~m) | (raw_old & m); p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); break; }
    case 1: { uint32_t m = to5551(mask), v = (to5551(c) & ~m) | (raw_old & m); p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); break; }
    case 2: { uint32_t m = to4444(mask), v = (to4444(c) & ~m) | (raw_old & m); p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); break; }
    default: { uint32_t v = (c & ~mask) | (raw_old & mask); memcpy(p, &v, 4); break; }
    }
}

static void write_stencil(const Raster *rs, uint8_t *p, uint32_t raw_old, int st) {
    if (R.fmt == 3) {
        uint32_t m = R.wmask | 0x00FFFFFF, v = (raw_old & m) | (((uint32_t)st << 24) & ~m);
        memcpy(p, &v, 4);
    } else if (R.fmt == 1) {
        if (to5551(R.wmask) & 0x8000) return;
        uint32_t v = (raw_old & 0x7FFF) | ((st & 0x80) << 8);
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    } else if (R.fmt == 2) {
        uint32_t m = to4444(R.wmask) | 0x0FFF, v = (raw_old & m) | (((uint32_t)st << 8) & ~m);
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    }
}

static inline int blend_factor(int f, const int *src, const int *dst, uint32_t fix, int ch, int for_dst) {
    const int *other = for_dst ? src : dst;
    switch (f) {
    case 0: return other[ch];
    case 1: return 255 - other[ch];
    case 2: return src[3];
    case 3: return 255 - src[3];
    case 4: return dst[3];
    case 5: return 255 - dst[3];
    case 6: return 2 * src[3];
    case 7: return 255 - imin(2 * src[3], 255);
    case 8: return 2 * dst[3];
    case 9: return 255 - imin(2 * dst[3], 255);
    default: return (fix >> (8 * ch)) & 0xFF;
    }
}

static uint32_t logic_op(const Raster *rs, uint32_t old, uint32_t c) {
    uint32_t a = c & 0xFF000000u, n = c & 0xFFFFFF, o = old & 0xFFFFFF;
    switch (R.lop) {
    case 0: n = 0; break;
    case 1: n &= o; break;
    case 2: n &= ~o; break;
    case 4: n = ~n & o; break;
    case 5: n = o; break;
    case 6: n ^= o; break;
    case 7: n |= o; break;
    case 8: n = ~(n | o); break;
    case 9: n = ~(n ^ o); break;
    case 10: n = ~o; break;
    case 11: n |= ~o; break;
    case 12: n = ~n; break;
    case 13: n = ~n | o; break;
    case 14: n = ~(n & o); break;
    case 15: n = 0xFFFFFF; break;
    default: break;
    }
    return a | (n & 0xFFFFFF);
}

static void put_pixel(Ctx *cx, int x, int y, int z, int fog, const int cin[4]) {
    const Raster *rs = cx->rs;
    int c[4];
    cx->shaded++;
    for (int i = 0; i < 4; i++) c[i] = iclamp(cin[i], 0, 255);
    if (R.depth_range && (z < R.minz || z > R.maxz)) return;
    if (!R.clear) {
        if (R.afunc != CMP_ALWAYS && !cmp(R.afunc, c[3] & R.amask, R.aref)) return;
        if (R.fog)
            for (int i = 0; i < 3; i++) c[i] = (c[i] * fog + (int)((R.fogcol >> (8 * i)) & 0xFF) * (255 - fog) + 255) >> 8;
        if (R.ctest) {
            uint32_t rgb = ((uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16) & R.cmask;
            if (R.cfunc == CMP_NEVER || (R.cfunc == CMP_EQUAL && rgb != R.cref) || (R.cfunc == CMP_NOTEQUAL && rgb == R.cref)) return;
        }
    }
    uint8_t *fp = R.fb + ((size_t)y * R.fbstride + x) * R.bpp;
    uint16_t *zp = (uint16_t *)(R.zb + ((size_t)y * R.zbstride + x) * 2);
    uint32_t raw_old, old = read_fb(rs, fp, &raw_old);
    int st = R.clear ? c[3] : stencil_of(rs, old);
    if (R.clear) {
        if (R.clear_depth) *zp = (uint16_t)z;
    } else if (R.stest) {
        int sv = R.smask != 0xFF ? st & R.smask : st, ref = R.sref & R.smask;
        if (!cmp(R.sfunc, ref, sv)) {
            write_stencil(rs, fp, raw_old, stencil_op(rs, R.sfail, st));
            return;
        }
        if (R.zfunc != CMP_ALWAYS && !cmp(R.zfunc, z, *zp)) {
            write_stencil(rs, fp, raw_old, stencil_op(rs, R.zfail, st));
            return;
        }
        st = stencil_op(rs, R.zpass, st);
    } else if (R.zfunc != CMP_ALWAYS && !cmp(R.zfunc, z, *zp)) {
        return;
    }
    if (R.zwrite) *zp = (uint16_t)z;
    uint32_t nc;
    if (R.blend) {
        int d[4] = { (int)(old & 0xFF), (int)((old >> 8) & 0xFF), (int)((old >> 16) & 0xFF), (int)(old >> 24) }, o[3];
        int sf0 = R.bsrc >= 2 && R.bsrc <= 9 ? blend_factor(R.bsrc, c, d, 0, 0, 0) : -1;
        int df0 = R.bdst >= 2 && R.bdst <= 9 ? blend_factor(R.bdst, c, d, 0, 0, 1) : -1;
        for (int i = 0; i < 3; i++) {
            int sf = sf0 >= 0 ? sf0 : blend_factor(R.bsrc, c, d, R.fixa, i, 0), df = df0 >= 0 ? df0 : blend_factor(R.bdst, c, d, R.fixb, i, 1);
            int l = ((c[i] * 2 + 1) * (sf * 2 + 1)) >> 10, r = ((d[i] * 2 + 1) * (df * 2 + 1)) >> 10;
            switch (R.beq) {
            case 0: o[i] = l + r; break;
            case 1: o[i] = l - r; break;
            case 2: o[i] = r - l; break;
            case 3: o[i] = imin(c[i], d[i]); break;
            case 4: o[i] = imax(c[i], d[i]); break;
            default: o[i] = abs(c[i] - d[i]); break;
            }
            o[i] = iclamp(o[i], 0, 255);
        }
        nc = (uint32_t)o[0] | (uint32_t)o[1] << 8 | (uint32_t)o[2] << 16 | (uint32_t)st << 24;
    } else {
        nc = (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 | (uint32_t)st << 24;
    }
    if (R.logic) nc = logic_op(rs, old, nc);
    if (R.clear) {
        if (!R.clear_color) nc = (nc & 0xFF000000u) | (old & 0x00FFFFFF);
        if (!R.clear_stencil) nc = (nc & 0x00FFFFFF) | (old & 0xFF000000u);
    }
    write_fb(rs, fp, nc, raw_old, R.wmask);
    cx->written++;
}

static inline int fog_value(float f) {
    if (!(f > 0.0f)) return 0;
    if (f >= 1.0f) return 255;
    return (int)(f * 256.0f);
}

/* --- triangles ---------------------------------------------------------------------- */
static int right_or_flat_bottom(const Vtx *v, const Vtx *l1, const Vtx *l2) {
    if (l1->y == l2->y) return v->y < l1->y;
    return v->x < l1->x + (int)((int64_t)(l2->x - l1->x) * (v->y - l1->y) / (l2->y - l1->y));
}

static int64_t floor_div(int64_t a, int64_t b) { int64_t q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
static int64_t ceil_div(int64_t a, int64_t b) { return -floor_div(-a, b); }

/* Draws (this thread's rows of) a triangle whose vertices are ordered so that the edge
 * functions are positive inside. */
static void raster_tri(Ctx *cx, const Vtx *a, const Vtx *b, const Vtx *c) {
    const Raster *rs = cx->rs;
    const Vtx *v[3] = { a, b, c };
    int64_t A[3], B[3], C[3];
    for (int i = 0; i < 3; i++) {
        const Vtx *p = v[(i + 1) % 3], *q = v[(i + 2) % 3];
        A[i] = (int64_t)p->y - q->y;
        B[i] = (int64_t)q->x - p->x;
        C[i] = (int64_t)q->y * p->x - (int64_t)q->x * p->y;
    }
    int64_t sum = A[0] * a->x + B[0] * a->y + C[0];
    if (sum <= 0) return;
    int minx = imin(a->x, imin(b->x, c->x)), maxx = imax(a->x, imax(b->x, c->x));
    int miny = imin(a->y, imin(b->y, c->y)), maxy = imax(a->y, imax(b->y, c->y));
    int px0 = imax((int)ceil_div(minx - 7, 16), R.sx1), px1 = imin((int)floor_div(maxx - 7, 16), R.sx2);
    int py0 = imax((int)ceil_div(miny - 7, 16), R.sy1), py1 = imin((int)floor_div(maxy - 7, 16), R.sy2);
    px0 = imax(px0, 0); py0 = imax(py0, 0); px1 = imin(px1, 1023); py1 = imin(py1, 1023);
    py0 = imax(py0, cx->ylo); py1 = imin(py1, cx->yhi);
    if (px0 > px1 || py0 > py1) return;
    int64_t bias[3] = { right_or_flat_bottom(a, b, c) ? -1 : 0, right_or_flat_bottom(b, c, a) ? -1 : 0, right_or_flat_bottom(c, a, b) ? -1 : 0 };

    const float rs_ = 1.0f / (float)sum;
    const int flatz = a->z == b->z && a->z == c->z;
    if (R.depth_range && flatz && (c->z < R.minz || c->z > R.maxz)) return;
    const int flat0 = !R.gouraud || (!memcmp(a->c0, b->c0, sizeof(a->c0)) && !memcmp(a->c0, c->c0, sizeof(a->c0)));
    const int flat1 = !R.gouraud || (!memcmp(a->c1, b->c1, sizeof(a->c1)) && !memcmp(a->c1, c->c1, sizeof(a->c1)));
    const int has_c1 = (a->c1[0] | a->c1[1] | a->c1[2] | b->c1[0] | b->c1[1] | b->c1[2] | c->c1[0] | c->c1[1] | c->c1[2]) != 0;
    const int nofog = R.clear || !R.fog || (a->fog >= 1.0f && b->fog >= 1.0f && c->fog >= 1.0f);
    const int tex = R.tex;
    float iw[3] = { 1.0f, 1.0f, 1.0f };
    if (tex && !R.through) for (int i = 0; i < 3; i++) iw[i] = 1.0f / v[i]->cw;
    float inv_tw = tex ? 1.0f / R.tw[0] : 0.0f, inv_th = tex ? 1.0f / R.th[0] : 0.0f;

    for (int y = py0; y <= py1; y++) {
        int64_t sy = (int64_t)y * 16 + 7, sx0 = (int64_t)px0 * 16 + 7;
        int64_t w[3];
        int64_t kmin = 0, kmax = px1 - px0;
        for (int i = 0; i < 3; i++) {
            w[i] = A[i] * sx0 + B[i] * sy + C[i];
            int64_t base = w[i] + bias[i], step = A[i] * 16;
            if (step > 0) { int64_t k = ceil_div(-base, step); if (k > kmin) kmin = k; }
            else if (step < 0) { int64_t k = floor_div(base, -step); if (k < kmax) kmax = k; }
            else if (base < 0) kmax = -1;
        }
        for (int64_t k = kmin; k <= kmax; k++) {
            float f0 = (float)(w[0] + A[0] * 16 * k), f1 = (float)(w[1] + A[1] * 16 * k), f2 = (float)(w[2] + A[2] * 16 * k);
            int x = px0 + (int)k;
            int z = flatz ? c->z : (int)((f0 * a->z + f1 * b->z + f2 * c->z) * rs_);
            if (R.depth_range && !flatz && (z < R.minz || z > R.maxz)) continue;
            int col[4];
            if (flat0) memcpy(col, c->c0, sizeof(col));
            else for (int i = 0; i < 4; i++) col[i] = (int)((f0 * a->c0[i] + f1 * b->c0[i] + f2 * c->c0[i]) * rs_);
            if (tex) {
                float s, t, ds = 0, dt = 0, cw = 0;
                if (R.through) {
                    s = (f0 * a->s + f1 * b->s + f2 * c->s) * rs_ * inv_tw;
                    t = (f0 * a->t + f1 * b->t + f2 * c->t) * rs_ * inv_th;
                    if (R.needs_lod) {
                        ds = (float)(A[0] * 16) * (a->s - c->s) * rs_ * inv_tw + (float)(A[1] * 16) * (b->s - c->s) * rs_ * inv_tw;
                        dt = (float)(B[0] * 16) * (a->t - c->t) * rs_ * inv_th + (float)(B[1] * 16) * (b->t - c->t) * rs_ * inv_th;
                    }
                } else {
                    float q0 = f0 * iw[0], q1 = f1 * iw[1], q2 = f2 * iw[2];
                    float den = R.proj ? q0 * a->q + q1 * b->q + q2 * c->q : q0 + q1 + q2;
                    float rq = 1.0f / den;
                    s = (q0 * a->s + q1 * b->s + q2 * c->s) * rq;
                    t = (q0 * a->t + q1 * b->t + q2 * c->t) * rq;
                    if (R.needs_lod) {
                        float g0 = f0 + A[0] * 16, g1 = f1 + A[1] * 16, g2 = f2 + A[2] * 16;
                        float r0 = g0 * iw[0], r1 = g1 * iw[1], r2 = g2 * iw[2];
                        float dx = R.proj ? r0 * a->q + r1 * b->q + r2 * c->q : r0 + r1 + r2;
                        ds = (r0 * a->s + r1 * b->s + r2 * c->s) / dx - s;
                        g0 = f0 + B[0] * 16; g1 = f1 + B[1] * 16; g2 = f2 + B[2] * 16;
                        r0 = g0 * iw[0]; r1 = g1 * iw[1]; r2 = g2 * iw[2];
                        float dy = R.proj ? r0 * a->q + r1 * b->q + r2 * c->q : r0 + r1 + r2;
                        dt = (r0 * a->t + r1 * b->t + r2 * c->t) / dy - t;
                        cw = (f0 * a->cw + f1 * b->cw + f2 * c->cw) * rs_;
                    }
                }
                texture(rs, col, s, t, ds, dt, cw);
            }
            if (has_c1 && !R.clear) {
                if (flat1) for (int i = 0; i < 3; i++) col[i] += c->c1[i];
                else for (int i = 0; i < 3; i++) col[i] += (int)((f0 * a->c1[i] + f1 * b->c1[i] + f2 * c->c1[i]) * rs_);
            }
            int fog = nofog ? 255 : fog_value((f0 * a->fog + f1 * b->fog + f2 * c->fog) * rs_);
            put_pixel(cx, x, y, z, fog, col);
        }
    }
}

/* --- sprites and points -------------------------------------------------------------- */
static void raster_rect(Ctx *cx, const Vtx *v0, const Vtx *v1) {
    const Raster *rs = cx->rs;
    int x1 = imin(v0->x, v1->x), x2 = imax(v0->x, v1->x), y1 = imin(v0->y, v1->y), y2 = imax(v0->y, v1->y);
    int px0 = imax((int)ceil_div(x1 - 8, 16), imax(R.sx1, 0)), px1 = imin((int)ceil_div(x2 - 8, 16) - 1, imin(R.sx2, 1023));
    int py0 = imax((int)ceil_div(y1 - 8, 16), imax(R.sy1, 0)), py1 = imin((int)ceil_div(y2 - 8, 16) - 1, imin(R.sy2, 1023));
    py0 = imax(py0, cx->ylo); py1 = imin(py1, cx->yhi);
    if (px0 > px1 || py0 > py1) return;
    if (R.depth_range && (v1->z < R.minz || v1->z > R.maxz)) return;
    float s0 = v0->s, t0 = v0->t, s1 = v1->s, t1 = v1->t;
    if (R.tex && R.through) { s0 /= R.tw[0]; s1 /= R.tw[0]; t0 /= R.th[0]; t1 /= R.th[0]; }
    float dx = (float)(x2 - x1), dy = (float)(y2 - y1);
    int fog = R.fog ? fog_value(v1->fog) : 255;
    for (int y = py0; y <= py1; y++) {
        float fy = ((y * 16 + 8) - y1) / dy;
        for (int x = px0; x <= px1; x++) {
            int col[4];
            memcpy(col, v1->c0, sizeof(col));
            if (R.tex) {
                float fx = ((x * 16 + 8) - x1) / dx, s, t;
                if (v0->x < v1->x) {
                    if (v0->y < v1->y) { s = s0 + fx * (s1 - s0); t = t0 + fy * (t1 - t0); }
                    else { s = s1 - fy * (s1 - s0); t = t0 + fx * (t1 - t0); }
                } else {
                    if (v0->y < v1->y) { s = s0 + fy * (s1 - s0); t = t1 - fx * (t1 - t0); }
                    else { s = s1 - fx * (s1 - s0); t = t1 - fy * (t1 - t0); }
                }
                texture(rs, col, s, t, (s1 - s0) * 16.0f / dx, (t1 - t0) * 16.0f / dy, v1->cw);
            }
            if (!R.clear) for (int i = 0; i < 3; i++) col[i] += v1->c1[i];
            put_pixel(cx, x, y, v1->z, fog, col);
        }
    }
}

static void raster_point(Ctx *cx, const Vtx *v) {
    const Raster *rs = cx->rs;
    int x = v->x >> 4, y = v->y >> 4;
    if (x < R.sx1 || x > R.sx2 || y < R.sy1 || y > R.sy2 || x < 0 || y < 0 || y < cx->ylo || y > cx->yhi) return;
    int col[4];
    memcpy(col, v->c0, sizeof(col));
    if (R.tex) texture(rs, col, R.through ? v->s / R.tw[0] : v->s, R.through ? v->t / R.th[0] : v->t, 0, 0, v->cw);
    put_pixel(cx, x, y, v->z, R.fog ? fog_value(v->fog) : 255, col);
}

/* --- job queue and worker threads ----------------------------------------------------- */
enum { JOB_TRI, JOB_RECT, JOB_POINT };
typedef struct { uint32_t kind, state; int y0, y1; Vtx v[3]; } Job;  /* y0..y1: rows it may touch */

static Raster g_cur;                 /* state of the draw being queued */
static uint32_t g_mem_gen = 1;       /* bumped whenever PSP memory may have changed (GPU texture cache) */
static Raster *g_states;             /* snapshots referenced by queued jobs */
static Job *g_jobs;
static uint8_t (*g_cluts)[1024];     /* CLUT contents referenced by the snapshots */
static size_t g_nstates, g_scap, g_njobs, g_jcap, g_nclut, g_ccap;
static uint32_t g_clut_version = 1, g_clut_queued;  /* CLUT version in g_cluts[g_nclut - 1] */

#define MAX_THREADS 32
static int g_nthreads;
static Ctx g_ctx[MAX_THREADS];
static int g_ymin = 1 << 30, g_ymax = -1;  /* rows touched by the queue */
static uint64_t g_work;                     /* estimated pixels (bounding boxes) in the queue */
#define SMALL_QUEUE_PIXELS 16384            /* below this the calling thread draws alone */
static volatile long g_next_band;

static void grow(void **p, size_t *cap, size_t need, size_t elem) {
    if (need <= *cap) return;
    size_t n = *cap ? *cap : 1024;
    while (n < need) n *= 2;
    *p = realloc(*p, n * elem);
    *cap = n;
}

void ge_clut_changed(void) { g_clut_version++; }

/* Snapshot of g_cur for the jobs that follow (shared with the previous one when equal). */
static uint32_t g_state_ix;
static void push_state(void) {
    if (g_cur.tex && (g_cur.tfmt == 4 || g_cur.tfmt == 5) && g_cur.clutshared) {
        const Raster *rs = &g_cur;
        g_cur.use_pal = 1;
        g_cur.clut = ge.clut;
        for (int i = 0; i < (g_cur.tfmt == 4 ? 16 : 256); i++) g_cur.pal[i] = clut_lookup(rs, (uint32_t)i, 0);
        g_cur.clut = NULL;
    } else if (g_cur.tex && g_cur.tfmt >= 4 && g_cur.tfmt <= 7) {
        if (g_clut_queued != g_clut_version || !g_nclut) {
            grow((void **)&g_cluts, &g_ccap, g_nclut + 1, sizeof(*g_cluts));
            memcpy(g_cluts[g_nclut++], ge.clut, sizeof(ge.clut));
            g_clut_queued = g_clut_version;
        }
        g_cur.clut_idx = (uint32_t)(g_nclut - 1);
    }
    if (g_nstates && !memcmp(&g_states[g_nstates - 1], &g_cur, sizeof(g_cur))) { g_state_ix = (uint32_t)(g_nstates - 1); return; }
    grow((void **)&g_states, &g_scap, g_nstates + 1, sizeof(Raster));
    g_states[g_nstates] = g_cur;
    g_state_ix = (uint32_t)g_nstates++;
}

/* ymin/ymax: screen rows in 1/16 pixel (clipped to the scissor here); w16: width in 1/16 pixel */
/* RT_GE_STATE_STATS=1: estimated pixels per raster-state combination (which fast paths matter) */
typedef struct { uint64_t key, area; } StateStat;
static StateStat g_sstat[512];
static int g_sstat_on = -1;

static uint64_t state_key(const Raster *rs) {
    uint64_t k = 0;
    k |= (uint64_t)R.fmt; k |= (uint64_t)R.clear << 2; k |= (uint64_t)R.through << 3; k |= (uint64_t)R.gouraud << 4;
    k |= (uint64_t)R.tex << 5;
    if (R.tex) {
        k |= (uint64_t)R.tfmt << 6; k |= (uint64_t)R.swz << 10; k |= (uint64_t)R.magf << 11; k |= (uint64_t)R.minf << 12;
        k |= (uint64_t)R.func << 13; k |= (uint64_t)R.rgba << 16; k |= (uint64_t)R.dbl << 17; k |= (uint64_t)(R.maxlevel > 0) << 18;
        k |= (uint64_t)R.clampS << 19; k |= (uint64_t)R.clampT << 20;
    }
    k |= (uint64_t)R.blend << 21;
    if (R.blend) { k |= (uint64_t)R.bsrc << 22; k |= (uint64_t)R.bdst << 26; k |= (uint64_t)R.beq << 30; }
    k |= (uint64_t)R.zfunc << 33; k |= (uint64_t)R.zwrite << 36; k |= (uint64_t)(R.afunc != CMP_ALWAYS) << 37;
    k |= (uint64_t)R.fog << 38; k |= (uint64_t)R.stest << 39; k |= (uint64_t)R.ctest << 40; k |= (uint64_t)R.logic << 41;
    k |= (uint64_t)(R.wmask != 0xFF000000u) << 42;
    return k;
}

static void state_stat(uint64_t area) {
    if (g_sstat_on < 0) g_sstat_on = getenv("RT_GE_STATE_STATS") != NULL;
    if (!g_sstat_on) return;
    uint64_t k = state_key(&g_cur);
    for (int i = 0; i < 512; i++) {
        if (g_sstat[i].area && g_sstat[i].key != k) continue;
        g_sstat[i].key = k;
        g_sstat[i].area += area;
        return;
    }
}

void ge_soft_report(void) {
    if (g_sstat_on <= 0) return;
    uint64_t total = 0;
    for (int i = 0; i < 512; i++) total += g_sstat[i].area;
    printf("  raster states by estimated pixels (fmt clear through gouraud | tex fmt swz mag min func rgba dbl mip clampS clampT | blend src dst eq | zfunc zwrite atest fog stest ctest logic mask):\n");
    for (int n = 0; n < 20; n++) {
        int best = -1;
        for (int i = 0; i < 512; i++) if (g_sstat[i].area && (best < 0 || g_sstat[i].area > g_sstat[best].area)) best = i;
        if (best < 0) break;
        uint64_t k = g_sstat[best].key;
        printf("   %5.1f%%  fb%llu c%llu t%llu g%llu | tex%llu f%llu s%llu mag%llu min%llu fn%llu a%llu d%llu mip%llu cs%llu ct%llu | bl%llu %llu %llu %llu | z%llu w%llu at%llu fog%llu st%llu ct%llu lo%llu m%llu\n",
               100.0 * g_sstat[best].area / (total ? total : 1),
               k & 3, (k >> 2) & 1, (k >> 3) & 1, (k >> 4) & 1, (k >> 5) & 1, (k >> 6) & 15, (k >> 10) & 1, (k >> 11) & 1, (k >> 12) & 1,
               (k >> 13) & 7, (k >> 16) & 1, (k >> 17) & 1, (k >> 18) & 1, (k >> 19) & 1, (k >> 20) & 1,
               (k >> 21) & 1, (k >> 22) & 15, (k >> 26) & 15, (k >> 30) & 7,
               (k >> 33) & 7, (k >> 36) & 1, (k >> 37) & 1, (k >> 38) & 1, (k >> 39) & 1, (k >> 40) & 1, (k >> 41) & 1, (k >> 42) & 1);
        g_sstat[best].area = 0;
    }
}

static Job *new_job(uint32_t kind, int ymin, int ymax, int w16) {
    grow((void **)&g_jobs, &g_jcap, g_njobs + 1, sizeof(Job));
    Job *j = &g_jobs[g_njobs++];
    j->kind = kind;
    j->state = g_state_ix;
    j->y0 = imax((ymin >> 4) - 1, imax(g_cur.sy1, 0));
    j->y1 = imin((ymax >> 4) + 1, imin(g_cur.sy2, 1023));
    if (j->y0 < g_ymin) g_ymin = j->y0;
    if (j->y1 > g_ymax) g_ymax = j->y1;
    if (j->y1 >= j->y0) {
        uint64_t area = (uint64_t)(j->y1 - j->y0 + 1) * (uint64_t)((w16 >> 4) + 1);
        g_work += area;
        state_stat(kind == JOB_TRI ? area / 2 : area);
    }
    return j;
}

#ifdef _WIN32
static long next_band(void) { return InterlockedIncrement(&g_next_band) - 1; }
#else
static long next_band(void) { return g_next_band++; }
#endif

/* Draws bands until none are left. */
static void run_jobs(int id) {
    Ctx *cx = &g_ctx[id];
    long nbands = (g_ymax - g_ymin) / BAND_ROWS + 1;
    for (long b; (b = next_band()) < nbands;) {
        cx->ylo = g_ymin + (int)b * BAND_ROWS;
        cx->yhi = cx->ylo + BAND_ROWS - 1;
        for (size_t i = 0; i < g_njobs; i++) {
            const Job *j = &g_jobs[i];
            if (j->y1 < cx->ylo || j->y0 > cx->yhi) continue;
            cx->rs = &g_states[j->state];
            switch (j->kind) {
            case JOB_TRI: raster_tri(cx, &j->v[0], &j->v[1], &j->v[2]); break;
            case JOB_RECT: raster_rect(cx, &j->v[0], &j->v[1]); break;
            default: raster_point(cx, &j->v[0]); break;
            }
        }
    }
}

#ifdef _WIN32
static SRWLOCK g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv_start = CONDITION_VARIABLE_INIT, g_cv_done = CONDITION_VARIABLE_INIT;
static uint32_t g_gen, g_remaining;

static DWORD WINAPI worker(LPVOID arg) {
    int id = (int)(intptr_t)arg;
    uint32_t seen = 0;
    for (;;) {
        AcquireSRWLockExclusive(&g_lock);
        while (g_gen == seen) SleepConditionVariableSRW(&g_cv_start, &g_lock, INFINITE, 0);
        seen = g_gen;
        ReleaseSRWLockExclusive(&g_lock);
        run_jobs(id);
        AcquireSRWLockExclusive(&g_lock);
        if (--g_remaining == 0) WakeConditionVariable(&g_cv_done);
        ReleaseSRWLockExclusive(&g_lock);
    }
}

static void start_threads(void) {
    const char *e = getenv("RT_GE_THREADS");
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_nthreads = e ? atoi(e) : (int)si.dwNumberOfProcessors;
    if (g_nthreads < 1) g_nthreads = 1;
    if (g_nthreads > MAX_THREADS) g_nthreads = MAX_THREADS;
    for (int i = 1; i < g_nthreads; i++) CloseHandle(CreateThread(NULL, 0, worker, (LPVOID)(intptr_t)i, 0, NULL));
}

static void run_parallel(void) {
    if (g_nthreads == 1 || g_work < SMALL_QUEUE_PIXELS) { run_jobs(0); return; }
    AcquireSRWLockExclusive(&g_lock);
    g_remaining = (uint32_t)(g_nthreads - 1);
    g_gen++;
    WakeAllConditionVariable(&g_cv_start);
    ReleaseSRWLockExclusive(&g_lock);
    run_jobs(0);  /* the calling thread draws its share too */
    AcquireSRWLockExclusive(&g_lock);
    while (g_remaining) SleepConditionVariableSRW(&g_cv_done, &g_lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&g_lock);
}
#else
static void start_threads(void) { g_nthreads = 1; }
static void run_parallel(void) { run_jobs(0); }
#endif

/* --- memory hazards between the queue and new work ------------------------------------
 * The queue is drawn later and in parallel bands, so new work must wait for a flush when it
 * reads memory the queue writes (a texture that is a pending render target), or writes memory
 * the queue reads or writes (a new render target over pending textures, a block transfer).
 * Accesses are kept as regions: a byte span plus, for 2D buffers, the rectangle inside a
 * buffer of a given stride, so that e.g. drawing into the right half of a buffer while
 * sampling its left half, or sampling only the top rows of a 512x512 screen texture, does not
 * count as overlapping. Depth tests and blending read the target pixel by pixel in the same
 * bands as the writes, so they are not hazards for the queue itself. */
typedef struct {
    uint32_t lo, hi;        /* [lo, hi) bytes (guest addresses masked to 28 bits) */
    uint32_t base, stride;  /* rectangle: rows y0..y1 of `stride` bytes from base, bytes x0..x1-1 */
    int32_t x0, x1, y0, y1;
    int rect;
    int local;              /* read pixel by pixel in band order (depth test): no hazard for draws to the same buffer */
} Region;

typedef struct { Region *r; size_t n, cap; int32_t hash[256]; } RegionList;  /* hash: index+1 by base */
/* pending writes; pending reads in VRAM (render-to-texture sources, depth) and elsewhere (other
 * textures: only block transfers can hit those, render targets are always in VRAM) */
static RegionList g_pw, g_pr, g_pr_ram;

static Region region_span(uint32_t lo, uint32_t hi) {
    Region g;
    memset(&g, 0, sizeof(g));
    g.lo = lo & 0x0FFFFFFF;
    g.hi = g.lo + (hi - lo);
    return g;
}

static Region region_rect(uint32_t base, uint32_t stride, int x0, int x1, int y0, int y1) {
    Region g;
    if (stride == 0 || x1 <= x0 || y1 < y0) return region_span(base, base);
    g.base = base & 0x0FFFFFFF;
    g.stride = stride;
    g.x0 = x0; g.x1 = x1; g.y0 = y0; g.y1 = y1;
    g.lo = g.base + (uint32_t)y0 * stride + (uint32_t)x0;
    g.hi = g.base + (uint32_t)y1 * stride + (uint32_t)x1;
    g.rect = 1;
    return g;
}

static int region_overlap(const Region *a, const Region *b) {
    if (a->hi <= a->lo || b->hi <= b->lo || a->hi <= b->lo || b->hi <= a->lo) return 0;
    if (!a->rect || !b->rect || a->stride != b->stride) return 1;
    int64_t s = a->stride, d = (int64_t)b->base - (int64_t)a->base;
    int64_t dy = floor_div(d, s), dx = d - dy * s;  /* b's origin in a's rows and bytes */
    int64_t bx0 = b->x0 + dx, bx1 = b->x1 + dx, by0 = b->y0 + dy, by1 = b->y1 + dy;
    if (bx1 > s) return 1;  /* b's rows wrap around a's: stay conservative */
    return !(bx1 <= a->x0 || a->x1 <= bx0 || by1 < a->y0 || a->y1 < by0);
}

static int list_overlaps(const RegionList *l, const Region *g) {
    for (size_t i = 0; i < l->n; i++) if (region_overlap(&l->r[i], g)) return 1;
    return 0;
}

/* A draw's write against pending reads: pixel-local reads of the same buffer are in band order. */
static int list_overlaps_draw(const RegionList *l, const Region *g) {
    for (size_t i = 0; i < l->n; i++) {
        const Region *e = &l->r[i];
        if (e->local && e->rect && g->rect && e->base == g->base && e->stride == g->stride) continue;
        if (region_overlap(e, g)) return 1;
    }
    return 0;
}

static void list_reset(RegionList *l) {
    if (!l->n) return;
    l->n = 0;
    memset(l->hash, 0, sizeof(l->hash));
}

/* Adds a region; one with the same base and stride as the entry last added for that base is
 * merged into it (the bounding rectangle), an identical span is not added twice. */
static void list_add(RegionList *l, const Region *g) {
    if (g->hi <= g->lo) return;
    uint32_t key = g->rect ? g->base : g->lo, h = ((key >> 4) * 2654435761u) >> 24;
    int32_t ix = l->hash[h] - 1;
    if (ix >= 0 && (size_t)ix < l->n) {
        Region *e = &l->r[ix];
        if (e->rect && g->rect && e->base == g->base && e->stride == g->stride && e->local == g->local) {
            e->x0 = imin(e->x0, g->x0); e->x1 = imax(e->x1, g->x1);
            e->y0 = imin(e->y0, g->y0); e->y1 = imax(e->y1, g->y1);
            e->lo = e->base + (uint32_t)e->y0 * e->stride + (uint32_t)e->x0;
            e->hi = e->base + (uint32_t)e->y1 * e->stride + (uint32_t)e->x1;
            return;
        }
        if (!e->rect && !g->rect && e->lo == g->lo && e->hi == g->hi) return;
    }
    grow((void **)&l->r, &l->cap, l->n + 1, sizeof(Region));
    l->r[l->n++] = *g;
    l->hash[h] = (int32_t)l->n;
}

static int in_vram(const Region *g) { return (g->lo & 0x0F000000) == VRAM; }

/* Texels a draw may sample from texture level `level`: the vertices' coordinate range (plus a
 * texel for filtering), or the whole texture when coordinates wrap or are generated. */
static Region texture_region(const Raster *rs, int level, const float *srange, int known) {
    uint32_t addr = (D(GE_CMD_TEXADDR0 + level) & 0xFFFFF0) | ((D(GE_CMD_TEXBUFWIDTH0 + level) << 8) & 0x0F000000);
    int bits = tex_bits[R.tfmt], w = R.tw[level], h = R.th[level];
    uint32_t pitch = (uint32_t)R.tbufw[level] * (uint32_t)bits / 8;
    int u0 = 0, u1 = w - 1, v0 = 0, v1 = h - 1;
    if (known) {
        if (R.clampS || (srange[0] >= 0.0f && srange[1] <= 1.0f)) {
            u0 = imax((int)floorf(srange[0] * w) - 1, 0); u1 = imin((int)ceilf(srange[1] * w) + 1, w - 1);
        }
        if (R.clampT || (srange[2] >= 0.0f && srange[3] <= 1.0f)) {
            v0 = imax((int)floorf(srange[2] * h) - 1, 0); v1 = imin((int)ceilf(srange[3] * h) + 1, h - 1);
        }
    }
    if (R.swz || R.tfmt >= 8) {  /* blocks: whole block rows */
        int rows = R.tfmt >= 8 ? 4 : 8;
        uint32_t row_bytes = R.tfmt >= 8 ? pitch * 4 : pitch * 8;
        return region_span(addr + (uint32_t)(v0 / rows) * row_bytes, addr + (uint32_t)(v1 / rows + 1) * row_bytes);
    }
    int x0 = u0 * bits / 8, x1 = ((u1 + 1) * bits + 7) / 8;
    if ((uint32_t)x1 > pitch) return region_span(addr, addr + pitch * (uint32_t)h);
    return region_rect(addr, pitch, x0, x1, v0, v1);
}

/* Checks the draw being queued against the queue (flushing on a hazard), then records what it
 * will read and write. bbox: screen rectangle of its vertices in pixels, or NULL (scissor);
 * srange: normalized s/t range of its vertices, or NULL (unknown). */
static void track_draw(const int *bbox, const float *srange) {
    const Raster *rs = &g_cur;
    int x0 = imax(R.sx1, 0), x1 = imin(R.sx2, 1023), y0 = imax(R.sy1, 0), y1 = imin(R.sy2, 1023);
    if (bbox) { x0 = imax(x0, bbox[0]); x1 = imin(x1, bbox[1]); y0 = imax(y0, bbox[2]); y1 = imin(y1, bbox[3]); }
    if (x1 < x0 || y1 < y0) return;  /* nothing can be drawn */
    Region w[2], zr, r[8];
    int nw = 0, nr = 0, zread = 0;
    w[nw++] = region_rect(VRAM | (D(GE_CMD_FRAMEBUFPTR) & 0x1FFFF0), (uint32_t)R.fbstride * R.bpp, x0 * R.bpp, (x1 + 1) * R.bpp, y0, y1);
    zr = region_rect(VRAM | (D(GE_CMD_ZBUFPTR) & 0x1FFFF0), (uint32_t)R.zbstride * 2, x0 * 2, (x1 + 1) * 2, y0, y1);
    if (R.zwrite || (R.clear && R.clear_depth)) w[nw++] = zr;
    else if (R.zfunc != CMP_ALWAYS) { zread = 1; zr.local = 1; }
    if (R.tex)
        for (int i = 0; i <= R.maxlevel && nr < 8; i++) r[nr++] = texture_region(rs, i, srange, srange != NULL);
    int why = -1;
    for (int i = 0; i < nr && why < 0; i++) if (list_overlaps(&g_pw, &r[i])) why = FLUSH_TEXTURE;  /* read after write */
    for (int i = 0; i < nw && why < 0; i++) if (list_overlaps_draw(&g_pr, &w[i])) why = FLUSH_TARGET;  /* write after read */
    if (why >= 0) {
        static int shown = -1;
        if (shown < 0) { const char *e = getenv("RT_GE_FLUSH_TRACE"); shown = e ? atoi(e) : 0; }
        if (shown > 0) {
            shown--;
            printf("[flush] list %u %s: target %08X stride %u x %d..%d y %d..%d", ge_stats.lists, why == FLUSH_TEXTURE ? "texture" : "target",
                   w[0].base, w[0].stride, w[0].x0, w[0].x1, w[0].y0, w[0].y1);
            for (int i = 0; i < nr; i++)
                printf("  tex %08X..%08X%s rect %d stride %u x %d..%d y %d..%d", r[i].lo, r[i].hi, list_overlaps(&g_pw, &r[i]) ? "*" : "",
                       r[i].rect, r[i].stride, r[i].x0, r[i].x1, r[i].y0, r[i].y1);
            if (why == FLUSH_TARGET)
                for (size_t k = 0; k < g_pr.n; k++)
                    for (int i = 0; i < nw; i++)
                        if (region_overlap(&g_pr.r[k], &w[i]) && !g_pr.r[k].local)
                            printf("  [w%d hits pending read %08X..%08X rect %d stride %u x %d..%d y %d..%d]", i, g_pr.r[k].lo, g_pr.r[k].hi,
                                   g_pr.r[k].rect, g_pr.r[k].stride, g_pr.r[k].x0, g_pr.r[k].x1, g_pr.r[k].y0, g_pr.r[k].y1);
            printf("\n");
        }
        ge_flush(why);
    }
    for (int i = 0; i < nw; i++) list_add(&g_pw, &w[i]);
    for (int i = 0; i < nr; i++) list_add(in_vram(&r[i]) ? &g_pr : &g_pr_ram, &r[i]);
    if (zread) list_add(&g_pr, &zr);
}

/* Called before something reads [addr, addr+size) directly (e.g. LOADCLUT). */
static int g_soft = 1, g_gpu;           /* which renderers draw */

void ge_sync_read(uint32_t addr, uint32_t size, int why) {
    Region g = region_span(addr, addr + size);
    if (list_overlaps(&g_pw, &g)) ge_flush(why);
    if (g_gpu && !g_soft) rt_gpu_sync_span(addr, size);  /* only the GPU has what was drawn there */
}

/* Draws everything queued. Called at the end of each display list and before anything
 * that reads or changes memory the queued drawing may touch. */
void ge_flush(int why) {
    list_reset(&g_pw);
    list_reset(&g_pr);
    list_reset(&g_pr_ram);
    if (!g_njobs) return;
    ge_stats.flush_why[why]++;
    if (!g_nthreads) start_threads();
    for (size_t i = 0; i < g_nstates; i++) {
        Raster *st = &g_states[i];
        st->clut = st->tex && st->tfmt >= 4 && st->tfmt <= 7 && !st->use_pal ? g_cluts[st->clut_idx] : NULL;
    }
    g_next_band = 0;
    ge_stats.flushes++;
    g_mem_gen++;  /* software drawing changes memory textures may come from */
    ge_stats.jobs += g_njobs;
    if (g_ymin <= g_ymax) run_parallel();
    for (int i = 0; i < g_nthreads; i++) {
        ge_stats.px_shaded += g_ctx[i].shaded; ge_stats.px_written += g_ctx[i].written;
        g_ctx[i].shaded = g_ctx[i].written = 0;
    }
    g_njobs = 0;
    g_work = 0;
    g_ymin = 1 << 30; g_ymax = -1;
    g_nstates = 0;
    g_nclut = 0;
    g_clut_queued = 0;
}

#undef R
#define R g_cur

/* --- primitive assembly (main thread) ------------------------------------------------- */
/* --- GPU path (rt_gpu.c) -------------------------------------------------------------
 * In "gpu" and "both" modes the same assembled primitives also become GPU vertices; textures
 * are decoded to RGBA once per content (keyed by address, format, palette and a content hash
 * computed at most once per memory generation). */
static GpuVertex *g_gv;
static size_t g_ngv, g_gvcap;

void rt_ge_set_renderer(int soft, int gpu) { g_soft = soft; g_gpu = gpu; }

/* RT_GPU_TRACE_RTT=1: prints each new combination of render target and texture in VRAM, and
 * each new block transfer touching VRAM, with the frame it first appears in. */
static int g_rtt_trace = -1;
static uint64_t g_flip;
static uint64_t g_tk[4096];
static int trace_new(uint64_t key) {
    key |= 1;
    size_t i = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 52) & 4095;
    for (int k = 0; k < 4096; k++, i = (i + 1) & 4095) {
        if (g_tk[i] == key) return 0;
        if (!g_tk[i]) { g_tk[i] = key; return 1; }
    }
    return 0;
}
static int rtt_tracing(void) {
    if (g_rtt_trace < 0) { const char *e = getenv("RT_GPU_TRACE_RTT"); g_rtt_trace = e ? atoi(e) : 0; }
    return g_rtt_trace;
}

void ge_memory_changed(void) { g_mem_gen++; g_flip++; }

static uint64_t hash_bytes(const uint8_t *p, size_t n, uint64_t h) {
    h ^= (uint64_t)n * 0x9E3779B97F4A7C15ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 32);
}

/* The same kind of hash over four independent lanes (no serial dependency): texture contents. */
static uint64_t hash_wide(const uint8_t *p, size_t n, uint64_t seed) {
    uint64_t a = seed ^ 0x9E3779B97F4A7C15ull, b = seed + 0xC2B2AE3D27D4EB4Full, c = ~seed, d = seed * 0x165667B19E3779F9ull;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        a = (a ^ v[0]) * 0x100000001B3ull; a ^= a >> 29;
        b = (b ^ v[1]) * 0x100000001B3ull; b ^= b >> 29;
        c = (c ^ v[2]) * 0x100000001B3ull; c ^= c >> 29;
        d = (d ^ v[3]) * 0x100000001B3ull; d ^= d >> 29;
    }
    uint64_t h = hash_bytes(p + i, n - i, a ^ (b * 31) ^ (c * 1009) ^ (d * 65537));
    return h;
}

typedef struct { uint64_t id, content; uint32_t gen, stamp; } TexMemo;
static TexMemo g_tmemo[4096];

/* Write stamps of 4 KB memory pages, for block transfers: a transfer (the games make over a
 * hundred per frame, uploading textures into VRAM) only invalidates the textures on its pages. */
#define PAGE_SHIFT 12
#define NPAGES (0x0A000000u >> PAGE_SHIFT)
static uint32_t g_page_stamp[NPAGES], g_write_stamp = 1;

static void stamp_pages(uint32_t lo, uint32_t hi) {
    lo &= 0x0FFFFFFF;
    hi = lo + (hi - lo);
    uint32_t s = ++g_write_stamp;
    for (uint32_t p = lo >> PAGE_SHIFT; p <= (hi - 1) >> PAGE_SHIFT && p < NPAGES; p++) g_page_stamp[p] = s;
}

static int pages_written_since(uint32_t lo, uint32_t hi, uint32_t stamp) {
    lo &= 0x0FFFFFFF;
    hi = lo + (hi - lo);
    for (uint32_t p = lo >> PAGE_SHIFT; p <= (hi - 1) >> PAGE_SHIFT && p < NPAGES; p++)
        if (g_page_stamp[p] > stamp) return 1;
    return 0;
}
static uint32_t *g_decode;
static size_t g_decode_cap;

/* Key of the current texture; decodes and uploads it when the GPU does not have it. The mip
 * levels go along when they form a chain of halving sizes (*levels says how many). */
static uint64_t gpu_texture(int *levels) {
    Raster *rs = &g_cur;
    int w = R.tw[0], h = R.th[0], bits = tex_bits[R.tfmt], nl = 1;
    while (nl <= R.maxlevel && R.tptr[nl] && R.tw[nl] == imax(1, w >> nl) && R.th[nl] == imax(1, h >> nl)) nl++;
    *levels = nl;
    uint64_t id = (uint64_t)R.tfmt | (uint64_t)nl << 4 | (uint64_t)w << 8 | (uint64_t)h << 20 | (uint64_t)R.swz << 48;
    for (int l = 0; l < nl; l++) {
        uint32_t addr = (D(GE_CMD_TEXADDR0 + l) & 0xFFFFF0) | ((D(GE_CMD_TEXBUFWIDTH0 + l) << 8) & 0x0F000000);
        id = hash_bytes((const uint8_t *)&addr, 4, id ^ (uint64_t)R.tbufw[l] << 32);
    }
    if (R.tfmt >= 4 && R.tfmt <= 7) {
        /* the palette is the loaded CLUT read through the CLUT parameters; hashed once per load */
        static uint32_t clut_hashed = 0xFFFFFFFFu;
        static uint64_t clut_hash;
        if (clut_hashed != g_clut_version) { clut_hash = hash_bytes(ge.clut, sizeof(ge.clut), 0x51D); clut_hashed = g_clut_version; }
        id = hash_bytes((const uint8_t *)&clut_hash, 8, id ^ ((uint64_t)R.clutfmt | (uint64_t)R.clutshift << 2 |
                                                               (uint64_t)R.clutmask << 8 | (uint64_t)R.clutoff << 16 |
                                                               (uint64_t)R.clutshared << 24));
    }
    TexMemo *m = &g_tmemo[(id >> 7) & 4095];
    int stale = m->id != id || m->gen != g_mem_gen;
    for (int l = 0; l < nl && !stale; l++) {
        uint32_t addr = (D(GE_CMD_TEXADDR0 + l) & 0xFFFFF0) | ((D(GE_CMD_TEXBUFWIDTH0 + l) << 8) & 0x0F000000);
        uint32_t pitch = (uint32_t)R.tbufw[l] * (uint32_t)bits / 8, rows = (uint32_t)((R.th[l] + 7) & ~7);
        stale = pages_written_since(addr, addr + pitch * rows, m->stamp);
    }
    if (stale) {
        m->id = id;
        m->gen = g_mem_gen;
        m->stamp = g_write_stamp;
        m->content = 0x12345;
        for (int l = 0; l < nl; l++) {
            uint32_t pitch = (uint32_t)R.tbufw[l] * (uint32_t)bits / 8, rows = (uint32_t)((R.th[l] + 7) & ~7);
            m->content = hash_wide(R.tptr[l], (size_t)pitch * rows, m->content);
        }
    }
    uint64_t key = id ^ (m->content * 0xC2B2AE3D27D4EB4Full);
    if (!rt_gpu_texture_resident(key)) {
        size_t total = 0;
        for (int l = 0; l < nl; l++) total += (size_t)R.tw[l] * R.th[l];
        grow((void **)&g_decode, &g_decode_cap, total, sizeof(uint32_t));
        const uint8_t *saved = R.clut;
        if (!R.use_pal) R.clut = ge.clut;
        uint32_t *o = g_decode;
        for (int l = 0; l < nl; l++)
            for (int y = 0; y < R.th[l]; y++)
                for (int x = 0; x < R.tw[l]; x++) *o++ = fetch(rs, l, x, y);
        R.clut = saved;
        rt_gpu_texture_upload(key, w, h, nl, g_decode);
    }
    return key;
}

static void gpu_setup(GpuDraw *d) {
    const Raster *rs = &g_cur;
    memset(d, 0, sizeof(*d));
    d->fb_addr = VRAM | (D(GE_CMD_FRAMEBUFPTR) & 0x1FFFF0);
    d->fb_stride = (uint32_t)R.fbstride;
    d->fb_fmt = (uint32_t)R.fmt;
    d->z_addr = VRAM | (D(GE_CMD_ZBUFPTR) & 0x1FFFF0);
    d->z_stride = (uint32_t)R.zbstride;
    d->sx1 = R.sx1; d->sy1 = R.sy1; d->sx2 = R.sx2; d->sy2 = R.sy2;
    d->zfunc = R.clear ? CMP_ALWAYS : R.zfunc;
    d->zwrite = R.clear ? R.clear_depth : R.zwrite;
    d->blend = R.blend; d->bsrc = R.bsrc; d->bdst = R.bdst; d->beq = R.beq;
    d->fixa = R.fixa; d->fixb = R.fixb;
    d->mask_rgb = R.clear && !R.clear_color ? 0xFFFFFFu : (R.wmask & 0xFFFFFFu);
    d->write_alpha = R.clear && R.clear_stencil && R.fmt != 0;
    d->clear_stencil = d->write_alpha;
    d->stest = R.stest;
    d->sfunc = R.sfunc; d->sref = R.sref; d->smask = R.smask;
    d->sfail = R.sfail; d->zfail = R.zfail; d->zpass = R.zpass;
    d->skeep = R.wmask >> 24;
    d->flags = (R.clear ? GPU_F_CLEAR : 0) | (R.fog ? GPU_F_FOG : 0) |
               (R.depth_range && (R.minz > 0 || R.maxz < 65535) ? GPU_F_ZRANGE : 0);
    d->texfunc = (uint32_t)R.func;
    d->afunc = (uint32_t)R.afunc;
    d->aref = (uint32_t)R.aref;
    d->amask = (uint32_t)R.amask;
    d->env = R.env;
    d->fogcol = R.fogcol;
    d->minz = R.minz;
    d->maxz = R.maxz;
    if (R.tex) {
        d->flags |= GPU_F_TEX | (R.rgba ? GPU_F_RGBA : 0) | (R.dbl ? GPU_F_DOUBLE : 0) | (R.proj ? GPU_F_PROJ : 0);
        d->textured = 1;
        d->minf = R.minf;
        d->magf = R.magf;
        d->mipf = R.mipf;
        d->clampS = R.clampS;
        d->clampT = R.clampT;
        d->lodmode = R.lodmode;
        d->lodoff = R.lodoff;
        /* render to texture: a plain texture in VRAM where the GPU drew comes from the GPU target */
        uint32_t addr = (D(GE_CMD_TEXADDR0) & 0xFFFFF0) | ((D(GE_CMD_TEXBUFWIDTH0) << 8) & 0x0F000000);
        int bpp = tex_bits[R.tfmt] / 8;
        uint32_t pitch = (uint32_t)R.tbufw[0] * (uint32_t)bpp;
        if (R.tfmt <= 3 && !R.swz && (addr & 0x0F000000) == VRAM && rt_gpu_texture_is_target(addr, pitch, bpp)) {
            d->tex_target = 1;
            d->tex_addr = addr;
            d->tex_pitch = pitch;
            d->tex_bpp = bpp;
            d->tex_fmt = R.tfmt;
            d->tex_w = R.tw[0];
            d->tex_h = R.th[0];
            d->levels = 1;
        } else {
            d->tex_key = gpu_texture(&d->levels);
        }
    }
}

/* RT_GPU_CHECK_FLIP=N ("both" mode): in frame N, every texture the GPU takes from a render
 * target is compared with what the software renderer drew there (debugging). */
static void gpu_check_rtt(const float *sr) {
    static long check = -2;
    if (check == -2) { const char *e = getenv("RT_GPU_CHECK_FLIP"); check = e ? atol(e) : -1; }
    if ((long)g_flip != check || !R.tex || R.tfmt > 3 || R.swz) return;
    uint32_t addr = (D(GE_CMD_TEXADDR0) & 0xFFFFF0) | ((D(GE_CMD_TEXBUFWIDTH0) << 8) & 0x0F000000);
    int bpp = tex_bits[R.tfmt] / 8;
    uint32_t pitch = (uint32_t)R.tbufw[0] * (uint32_t)bpp;
    if ((addr & 0x0F000000) != VRAM || !rt_gpu_texture_is_target(addr, pitch, bpp)) return;
    ge_flush(FLUSH_TRACE);  /* memory gets the software renderer's result */
    printf("[gpu-check] list %u:\n", ge_stats.lists);
    int x0 = 0, y0 = 0, x1 = R.tw[0], y1 = R.th[0];  /* the texels this draw samples */
    if (sr) {
        x0 = iclamp((int)floorf(sr[0] * R.tw[0]), 0, R.tw[0] - 1); x1 = iclamp((int)ceilf(sr[1] * R.tw[0]), x0 + 1, R.tw[0]);
        y0 = iclamp((int)floorf(sr[2] * R.th[0]), 0, R.th[0] - 1); y1 = iclamp((int)ceilf(sr[3] * R.th[0]), y0 + 1, R.th[0]);
    }
    rt_gpu_debug_compare_texture(addr + (uint32_t)y0 * pitch + (uint32_t)x0 * (uint32_t)bpp, pitch, bpp, R.tfmt, x1 - x0, y1 - y0);
}

/* off: triangles sample pixel x at x + 7/16 (raster_tri), the GPU at x + 8/16, so their
 * vertices move by 1/16; sprites already sample at the centre (raster_rect). */
static void gpu_vertex(GpuVertex *o, const Vtx *v, float w, int off) {
    o->x = (v->x + off) / 16.0f;
    o->y = (v->y + off) / 16.0f;
    o->z = (float)v->z;
    o->w = w;
    if (R.tex && R.through) { o->s = v->s / R.tw[0]; o->t = v->t / R.th[0]; }
    else { o->s = v->s; o->t = v->t; }
    o->q = R.proj ? v->q : 1.0f;
    for (int i = 0; i < 4; i++) o->c0[i] = (uint8_t)iclamp(v->c0[i], 0, 255);
    for (int i = 0; i < 3; i++) o->c1[i] = (uint8_t)iclamp(v->c1[i], 0, 255);
    o->c1[3] = 0;
    o->fog = v->fog;
}

static void gpu_tri(const Vtx *a, const Vtx *b, const Vtx *c) {
    grow((void **)&g_gv, &g_gvcap, g_ngv + 3, sizeof(GpuVertex));
    const Vtx *v[3] = { a, b, c };
    for (int i = 0; i < 3; i++) gpu_vertex(&g_gv[g_ngv++], v[i], R.through ? 1.0f : v[i]->cw, 1);
}

/* sprite: two triangles; texture coordinates follow the corner cases of raster_rect */
static void gpu_rect(const Vtx *v0, const Vtx *v1) {
    float s0 = v0->s, t0 = v0->t, s1 = v1->s, t1 = v1->t, cs[4], ct[4];
    if (v0->x < v1->x) {
        if (v0->y < v1->y) { cs[0] = s0; ct[0] = t0; cs[1] = s1; ct[1] = t0; cs[2] = s0; ct[2] = t1; cs[3] = s1; ct[3] = t1; }
        else { cs[0] = s1; ct[0] = t0; cs[1] = s1; ct[1] = t1; cs[2] = s0; ct[2] = t0; cs[3] = s0; ct[3] = t1; }
    } else {
        if (v0->y < v1->y) { cs[0] = s0; ct[0] = t1; cs[1] = s0; ct[1] = t0; cs[2] = s1; ct[2] = t1; cs[3] = s1; ct[3] = t0; }
        else { cs[0] = s1; ct[0] = t1; cs[1] = s0; ct[1] = t1; cs[2] = s1; ct[2] = t0; cs[3] = s0; ct[3] = t0; }
    }
    int x1 = imin(v0->x, v1->x), x2 = imax(v0->x, v1->x), y1 = imin(v0->y, v1->y), y2 = imax(v0->y, v1->y);
    Vtx c[4];
    for (int i = 0; i < 4; i++) {
        c[i] = *v1;
        c[i].x = (i & 1) ? x2 : x1;
        c[i].y = (i & 2) ? y2 : y1;
        c[i].s = cs[i];
        c[i].t = ct[i];
    }
    grow((void **)&g_gv, &g_gvcap, g_ngv + 6, sizeof(GpuVertex));
    static const int order[6] = { 0, 1, 2, 2, 1, 3 };
    for (int i = 0; i < 6; i++) gpu_vertex(&g_gv[g_ngv++], &c[order[i]], 1.0f, 0);
}

static int64_t area2(const Vtx *a, const Vtx *b, const Vtx *c) {
    return (int64_t)(b->x - a->x) * (c->y - a->y) - (int64_t)(c->x - a->x) * (b->y - a->y);
}

static void draw_oriented(const Vtx *a, const Vtx *b, const Vtx *c, int cull) {
    if (a->outside || b->outside || c->outside) return;
    int64_t ar = area2(a, b, c);
    Job *j;
    int ymin = imin(a->y, imin(b->y, c->y)), ymax = imax(a->y, imax(b->y, c->y));
    int w16 = imax(a->x, imax(b->x, c->x)) - imin(a->x, imin(b->x, c->x));
    if (ar > 0 || (ar < 0 && cull == CULL_OFF)) ge_stats.tris_raster++;
    if (g_gpu && (ar > 0 || (ar < 0 && cull == CULL_OFF))) gpu_tri(a, b, c);
    if (!g_soft) return;
    if (ar > 0) { j = new_job(JOB_TRI, ymin, ymax, w16); j->v[0] = *a; j->v[1] = *b; j->v[2] = *c; }
    else if (ar < 0 && cull == CULL_OFF) { j = new_job(JOB_TRI, ymin, ymax, w16); j->v[0] = *c; j->v[1] = *b; j->v[2] = *a; }
}

static void lerp_vtx(Vtx *o, float t, const Vtx *a, const Vtx *b) {
    o->cx = a->cx + (b->cx - a->cx) * t; o->cy = a->cy + (b->cy - a->cy) * t;
    o->cz = a->cz + (b->cz - a->cz) * t; o->cw = a->cw + (b->cw - a->cw) * t;
    o->s = a->s + (b->s - a->s) * t; o->t = a->t + (b->t - a->t) * t; o->q = a->q + (b->q - a->q) * t;
    o->fog = a->fog + (b->fog - a->fog) * t;
    for (int i = 0; i < 4; i++) o->c0[i] = (int)(a->c0[i] + (b->c0[i] - a->c0[i]) * t + 0.5f);
    for (int i = 0; i < 3; i++) o->c1[i] = (int)(a->c1[i] + (b->c1[i] - a->c1[i]) * t + 0.5f);
    o->outside = project(o, 1);
}

static int outside_z(const Vtx *v, int *pos, int *neg) {
    const float lim = 1.000030517578125f;
    float z = v->cz / v->cw;
    if (z >= lim) { (*pos)++; return 1; }
    if (-z >= lim) { (*neg)++; return 1; }
    return 0;
}

static void submit_tri(const Vtx *v0, const Vtx *v1, const Vtx *v2, const Vtx *prov, int cull) {
    if (!R.through) {
        if (v0->outside || v1->outside || v2->outside) return;
        if (v0->cw < 0 && v1->cw < 0 && v2->cw < 0) return;
        int pos = 0, neg = 0;
        outside_z(v0, &pos, &neg); outside_z(v1, &pos, &neg); outside_z(v2, &pos, &neg);
        if (pos + neg > 0 && !X.clamp) return;
        if (pos >= 3 || neg >= 3) return;
    }
    int clips = !R.through && (v0->cz < -v0->cw || v1->cz < -v1->cw || v2->cz < -v2->cw);
    if (R.gouraud && !clips) {
        if (cull == CULL_CW) draw_oriented(v2, v1, v0, cull);
        else draw_oriented(v0, v1, v2, cull);
        return;
    }
    Vtx t[3] = { *v0, *v1, *v2 };
    if (!R.gouraud)
        for (int i = 0; i < 3; i++) { memcpy(t[i].c0, prov->c0, sizeof(t[i].c0)); memcpy(t[i].c1, prov->c1, sizeof(t[i].c1)); }
    if (cull == CULL_CW) { Vtx s = t[0]; t[0] = t[2]; t[2] = s; }
    int needs_clip = !R.through && (t[0].cz < -t[0].cw || t[1].cz < -t[1].cw || t[2].cz < -t[2].cw);
    if (!needs_clip) { draw_oriented(&t[0], &t[1], &t[2], cull); return; }
    /* Clip against the near plane only (z >= -w), as the PSP does. */
    Vtx out[8];
    int m = 0;
    for (int i = 0; i < 3; i++) {
        const Vtx *p = &t[i], *q = &t[(i + 1) % 3];
        float dp = p->cz + p->cw, dq = q->cz + q->cw;
        if (dp >= 0) out[m++] = *p;
        if ((dp <= 0 && dq > 0) || (dp > 0 && dq <= 0)) {
            if (dq < 0) lerp_vtx(&out[m++], dp / (dp - dq), p, q);
            else lerp_vtx(&out[m++], dq / (dq - dp), q, p);
        }
    }
    for (int i = 2; i < m; i++) draw_oriented(&out[0], &out[i - 1], &out[i], cull);
}

static void submit_rect(const Vtx *a, const Vtx *b) {
    if (!R.through) {
        if (a->outside || b->outside) return;
        int pos = 0, neg = 0;
        outside_z(a, &pos, &neg); outside_z(b, &pos, &neg);
        if (pos + neg > 0 && !X.clamp) return;
        if (pos >= 2 || neg >= 2) return;
    }
    if (g_gpu) gpu_rect(a, b);
    if (!g_soft) return;
    Job *j = new_job(JOB_RECT, imin(a->y, b->y), imax(a->y, b->y), abs(a->x - b->x));
    j->v[0] = *a; j->v[1] = *b;
}

/* GPU: a point covers the pixel it falls in (as raster_point), drawn as a one-pixel sprite */
static void gpu_point(const Vtx *v) {
    Vtx a = *v, b = *v;
    a.x = (v->x >> 4) << 4;
    a.y = (v->y >> 4) << 4;
    b.x = a.x + 16;
    b.y = a.y + 16;
    gpu_rect(&a, &b);
}

static void submit_point(const Vtx *v) {
    if (!R.through && v->outside) return;
    if (g_gpu) gpu_point(v);
    if (!g_soft) return;
    new_job(JOB_POINT, v->y, v->y, 0)->v[0] = *v;
}

/* lines are drawn pixel by pixel, the same pixels in both renderers */
static void submit_line(const Vtx *a, const Vtx *b) {
    if (!R.through && (a->outside || b->outside)) return;
    int x0 = a->x >> 4, y0 = a->y >> 4, x1 = b->x >> 4, y1 = b->y >> 4;
    int n = imax(abs(x1 - x0), abs(y1 - y0));
    for (int i = 0; i < n; i++) {  /* the last pixel is not drawn */
        float f = (float)i / n;
        Vtx p = *a;
        p.x = (int)((x0 + (x1 - x0) * f) * 16); p.y = (int)((y0 + (y1 - y0) * f) * 16);
        p.z = (int)(a->z + (b->z - a->z) * f);
        p.s = a->s + (b->s - a->s) * f; p.t = a->t + (b->t - a->t) * f; p.fog = a->fog + (b->fog - a->fog) * f;
        for (int k = 0; k < 4; k++) p.c0[k] = R.gouraud ? (int)(a->c0[k] + (b->c0[k] - a->c0[k]) * f) : b->c0[k];
        p.outside = 0;
        if (g_gpu) gpu_point(&p);
        if (g_soft) new_job(JOB_POINT, p.y, p.y, 0)->v[0] = p;
    }
}

/* --- PRIM --------------------------------------------------------------------------- */
static Vtx *g_vtx;
static int *g_map;
static size_t g_vcap, g_mapcap;

void ge_draw_prim(uint32_t op) {
    uint32_t count = op & 0xFFFF, prim = (op >> 16) & 7;
    if (count == 0) return;
    static VFmt f;
    static uint32_t f_vt = 0xFFFFFFFFu;
    if (D(GE_CMD_VERTEXTYPE) != f_vt) { f_vt = D(GE_CMD_VERTEXTYPE); vfmt_init(&f, f_vt); }
    uint32_t isize = f.idx ? 1u << (f.idx - 1) : 0;
    const uint8_t *vbase = rt_ptr(ge.vaddr), *ibase = f.idx ? rt_ptr(ge.iaddr) : NULL;
    if (f.idx) ge.iaddr += count * isize;
    else ge.vaddr += count * f.size;
    if (rt_ge_skip_draw) return;
    if (f.pos == 0) return;  /* vertices without a position are culled */
    if (prim == 7) prim = 3;

    /* kept from the previous draw when no command since changed their inputs (ge_setup_dirty,
     * rt_ge.c). They go together: setup_raster reads the transform's uvgen. push_state only
     * rewrites fields of g_cur that it recomputes for every draw anyway. */
    if (ge_setup_dirty) {
        setup_transform(&f);
        setup_raster(&g_cur, f.through);
        ge_setup_dirty = 0;
    } else {
        ge_stats.setups_reused++;
    }
    static int trace_list = -1;
    if (trace_list < 0) { const char *e = getenv("RT_GE_TRACE_LIST"); trace_list = e ? atoi(e) : 0; }
    int tracing = trace_list && (int)ge_stats.lists == trace_list;
    if (tracing) ge_flush(FLUSH_TRACE);

    /* transform each referenced vertex once */
    grow((void **)&g_map, &g_mapcap, count, sizeof(int));
    uint32_t lo = 0, hi = count - 1;
    if (f.idx) {
        lo = 0xFFFFFFFFu; hi = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t ix = f.idx == 1 ? ibase[i] : f.idx == 2 ? rd16(ibase + 2 * i) : rd32(ibase + 4 * i);
            g_map[i] = (int)ix;
            if (ix < lo) lo = ix;
            if (ix > hi) hi = ix;
        }
        if (hi - lo > 65535) return;
    } else {
        for (uint32_t i = 0; i < count; i++) g_map[i] = (int)i;
    }
    grow((void **)&g_vtx, &g_vcap, hi - lo + 1, sizeof(Vtx));
    ge_stats.verts_xformed += hi - lo + 1;
    for (uint32_t i = lo; i <= hi; i++) {
        RawV r;
        decode_vertex(&f, vbase + (size_t)i * f.size, &r);
        transform_vertex(&f, &r, &g_vtx[i - lo]);
    }
#define V(i) (&g_vtx[g_map[i] - (int)lo])

    /* screen box and texture-coordinate range of the draw, for the hazard check */
    int bbox[4] = { 1 << 30, -(1 << 30), 1 << 30, -(1 << 30) }, bb_ok = 1, sr_ok = g_cur.tex && !(X.uvgen == 1 && !f.through);
    float sr[4] = { 1e30f, -1e30f, 1e30f, -1e30f };
    int ranges = g_soft || rtt_tracing();  /* only the software queue and the RTT trace use them */
    for (uint32_t i = lo; ranges && i <= hi; i++) {
        const Vtx *v = &g_vtx[i - lo];
        if (v->outside || (!f.through && v->cz < -v->cw)) bb_ok = 0;
        else {
            bbox[0] = imin(bbox[0], v->x >> 4); bbox[1] = imax(bbox[1], (v->x >> 4) + 1);
            bbox[2] = imin(bbox[2], v->y >> 4); bbox[3] = imax(bbox[3], (v->y >> 4) + 1);
        }
        if (sr_ok) {
            float s = f.through ? v->s / g_cur.tw[0] : v->s, t = f.through ? v->t / g_cur.th[0] : v->t;
            if (!(s == s) || !(t == t)) sr_ok = 0;
            if (s < sr[0]) sr[0] = s;
            if (s > sr[1]) sr[1] = s;
            if (t < sr[2]) sr[2] = t;
            if (t > sr[3]) sr[3] = t;
        }
    }
    if ((g_cur.stest || (g_cur.clear && g_cur.clear_stencil)) && rtt_tracing()) {
        uint32_t fa = D(GE_CMD_FRAMEBUFPTR) & 0x1FFFF0;
        if (trace_new((uint64_t)fa ^ (uint64_t)g_cur.sfunc << 22 ^ (uint64_t)g_cur.sref << 25 ^ (uint64_t)g_cur.smask << 33 ^
                      (uint64_t)(g_cur.sfail | g_cur.zfail << 3 | g_cur.zpass << 6) << 41 ^ (uint64_t)(g_cur.wmask >> 24) << 50 ^
                      (uint64_t)g_cur.clear << 58 ^ (uint64_t)g_cur.zfunc << 59 ^ 0x77))
            printf("[stencil] flip %llu: fb %06X fmt %d %s func %d ref %02X mask %02X ops %d/%d/%d z %d/%d amask %02X tex %d blend %d %d,%d\n",
                   (unsigned long long)g_flip, fa, g_cur.fmt, g_cur.clear ? "clear" : "draw", g_cur.sfunc, g_cur.sref, g_cur.smask,
                   g_cur.sfail, g_cur.zfail, g_cur.zpass, g_cur.zfunc, g_cur.zwrite, g_cur.wmask >> 24, g_cur.tex, g_cur.blend, g_cur.bsrc, g_cur.bdst);
    }
    if (g_cur.tex && rtt_tracing()) {
        uint32_t ta = (D(GE_CMD_TEXADDR0) & 0xFFFFF0) | ((D(GE_CMD_TEXBUFWIDTH0) << 8) & 0x0F000000);
        uint32_t fa = D(GE_CMD_FRAMEBUFPTR) & 0x1FFFF0, za = D(GE_CMD_ZBUFPTR) & 0x1FFFF0;
        if ((ta & 0x0F000000) == VRAM &&
            trace_new((uint64_t)fa ^ (uint64_t)(ta & 0x1FFFFF) << 21 ^ (uint64_t)g_cur.tfmt << 42 ^ (uint64_t)g_cur.fmt << 46 ^
                      (uint64_t)g_cur.tw[0] << 48 ^ (uint64_t)g_cur.th[0] << 54 ^ (uint64_t)g_cur.tbufw[0] << 30 ^ (uint64_t)prim << 60))
            printf("[rtt] flip %llu: fb %06X/%d fmt %d z %06X <- tex %06X fmt %d %dx%d bufw %d swz %d clut %d/%d/%02X/%d through %d prim %u "
                   "blend %d %d,%d func %d rgba %d atest %d/%02X uv %.3f..%.3f,%.3f..%.3f bbox %d..%d,%d..%d\n",
                   (unsigned long long)g_flip, fa, g_cur.fbstride, g_cur.fmt, za, ta & 0x1FFFFF, g_cur.tfmt, g_cur.tw[0], g_cur.th[0],
                   g_cur.tbufw[0], g_cur.swz, g_cur.clutfmt, g_cur.clutshift, g_cur.clutmask, g_cur.clutoff, f.through, prim, g_cur.blend,
                   g_cur.bsrc, g_cur.bdst, g_cur.func, g_cur.rgba, g_cur.afunc, g_cur.aref, sr[0], sr[1], sr[2], sr[3], bbox[0], bbox[1], bbox[2], bbox[3]);
    }
    if (g_soft && g_gpu) gpu_check_rtt(sr_ok ? sr : NULL);
    if (g_soft) {
        track_draw(bb_ok ? bbox : NULL, sr_ok ? sr : NULL);  /* flushes first if this draw depends on (or changes) memory the queue uses */
        push_state();
    }
    GpuDraw gd;
    if (g_gpu) { gpu_setup(&gd); g_ngv = 0; }

    int cull = CULL_OFF;
    if ((D(GE_CMD_CULLFACEENABLE) & 1) && !R.clear) cull = (D(GE_CMD_CULL) & 1) ? CULL_CCW : CULL_CW;
    uint64_t px_before = ge_stats.px_shaded;
    switch (prim) {
    case 0: for (uint32_t i = 0; i < count; i++) submit_point(V(i)); break;
    case 1: for (uint32_t i = 0; i + 1 < count; i += 2) submit_line(V(i), V(i + 1)); break;
    case 2: for (uint32_t i = 0; i + 1 < count; i++) submit_line(V(i), V(i + 1)); break;
    case 3: for (uint32_t i = 0; i + 2 < count; i += 3) submit_tri(V(i), V(i + 1), V(i + 2), V(i + 2), cull); break;
    case 4:
        for (uint32_t i = 0; i + 2 < count; i++) {
            if (i & 1) submit_tri(V(i + 1), V(i), V(i + 2), V(i + 2), cull);
            else submit_tri(V(i), V(i + 1), V(i + 2), V(i + 2), cull);
        }
        break;
    case 5: for (uint32_t i = 2; i < count; i++) submit_tri(V(0), V(i - 1), V(i), V(i), cull); break;
    case 6: for (uint32_t i = 0; i + 1 < count; i += 2) submit_rect(V(i), V(i + 1)); break;
    default: break;
    }
#undef V
    if (g_gpu && g_ngv) rt_gpu_draw(&gd, g_gv, (uint32_t)g_ngv);
    if (tracing) {
        ge_flush(FLUSH_TRACE);
        printf("[ge-trace] prim %u x%-5u vtype %06X fb %06X/%u fmt %u scissor %u,%u-%u,%u %s%s%s%s tex %u %ux%u @%06X -> %llu px\n",
               prim, count, D(GE_CMD_VERTEXTYPE), D(GE_CMD_FRAMEBUFPTR) & 0x1FFFF0, R.fbstride, R.fmt, R.sx1, R.sy1, R.sx2, R.sy2,
               R.clear ? "clear " : "", R.blend ? "blend " : "", R.zfunc != CMP_ALWAYS ? "ztest " : "", R.stest ? "stencil " : "",
               R.tex ? R.tfmt : 99, R.tex ? R.tw[0] : 0, R.tex ? R.th[0] : 0, D(GE_CMD_TEXADDR0) & 0xFFFFF0,
               (unsigned long long)(ge_stats.px_shaded - px_before));
        if (getenv("RT_GE_TRACE_DETAIL"))
            printf("    func %d rgba %d dbl %d c0 %d,%d,%d,%d c1 %d,%d,%d | blend %d eq %d src %d dst %d fix %06X/%06X | atest %d/%02X mask %08X "
                   "stencil %d func %d ref %02X ops %d/%d/%d | ctest %d logic %d fog %d mag %d min %d clamp %d%d\n",
                   R.func, R.rgba, R.dbl, g_vtx[g_map[0] - (int)lo].c0[0], g_vtx[g_map[0] - (int)lo].c0[1], g_vtx[g_map[0] - (int)lo].c0[2], g_vtx[g_map[0] - (int)lo].c0[3], g_vtx[g_map[0] - (int)lo].c1[0], g_vtx[g_map[0] - (int)lo].c1[1], g_vtx[g_map[0] - (int)lo].c1[2],
                   R.blend, R.beq, R.bsrc, R.bdst, R.fixa, R.fixb, R.afunc, R.aref, R.wmask, R.stest, R.sfunc, R.sref, R.sfail, R.zfail,
                   R.zpass, R.ctest, R.logic, R.fog, R.magf, R.minf, R.clampS, R.clampT);
    }
}

int ge_bbox(uint32_t op) { (void)op; return 1; }

/* TRANSFERSTART: rectangle copy between memory areas (masks as PPSSPP GPUState.h) */
void ge_block_transfer(uint32_t op) {
    uint32_t src = (ge.cmd[GE_CMD_TRANSFERSRC] & 0xFFFFF0) | ((ge.cmd[GE_CMD_TRANSFERSRCW] & 0xFF0000) << 8);
    uint32_t dst = (ge.cmd[GE_CMD_TRANSFERDST] & 0xFFFFF0) | ((ge.cmd[GE_CMD_TRANSFERDSTW] & 0xFF0000) << 8);
    uint32_t sstride = ge.cmd[GE_CMD_TRANSFERSRCW] & 0x7F8, dstride = ge.cmd[GE_CMD_TRANSFERDSTW] & 0x7F8;
    if (sstride > 0x400) sstride = 0;
    if (dstride > 0x400) dstride = 0;
    uint32_t sx = ge.cmd[GE_CMD_TRANSFERSRCPOS] & 0x3FF, sy = (ge.cmd[GE_CMD_TRANSFERSRCPOS] >> 10) & 0x3FF;
    uint32_t dx = ge.cmd[GE_CMD_TRANSFERDSTPOS] & 0x3FF, dy = (ge.cmd[GE_CMD_TRANSFERDSTPOS] >> 10) & 0x3FF;
    uint32_t w = (ge.cmd[GE_CMD_TRANSFERSIZE] & 0x3FF) + 1, h = ((ge.cmd[GE_CMD_TRANSFERSIZE] >> 10) & 0x3FF) + 1;
    uint32_t bpp = (op & 1) ? 4 : 2;
    Region rsrc = sstride ? region_rect(src, sstride * bpp, (int)(sx * bpp), (int)((sx + w) * bpp), (int)sy, (int)(sy + h - 1))
                          : region_span(src + sx * bpp, src + (sx + w) * bpp);
    Region rdst = dstride ? region_rect(dst, dstride * bpp, (int)(dx * bpp), (int)((dx + w) * bpp), (int)dy, (int)(dy + h - 1))
                          : region_span(dst + dx * bpp, dst + (dx + w) * bpp);
    if (g_soft && (list_overlaps(&g_pw, &rsrc) || list_overlaps(&g_pw, &rdst) || list_overlaps(&g_pr, &rdst) || list_overlaps(&g_pr_ram, &rdst)))
        ge_flush(FLUSH_TRANSFER);
    if (g_gpu)
        rt_gpu_transfer_begin(src, sstride * bpp, (int)sx, (int)sy, dst, dstride * bpp, (int)dx, (int)dy, (int)w, (int)h, (int)bpp, !g_soft);
    if (rtt_tracing() && ((src & 0x0F000000) == VRAM || (dst & 0x0F000000) == VRAM) &&
        trace_new((uint64_t)src ^ (uint64_t)dst << 28 ^ (uint64_t)(w | h << 10) << 40 ^ (uint64_t)bpp << 62 ^ 0x5A5A))
        printf("[rtt] flip %llu: transfer %08X/%u (%u,%u) -> %08X/%u (%u,%u) %ux%u bpp %u\n", (unsigned long long)g_flip,
               src, sstride, sx, sy, dst, dstride, dx, dy, w, h, bpp);
    for (uint32_t y = 0; y < h; y++)
        memmove(rt_ptr(dst + ((dy + y) * dstride + dx) * bpp), rt_ptr(src + ((sy + y) * sstride + sx) * bpp), w * bpp);
    stamp_pages(dst + (dy * dstride + dx) * bpp, dst + ((dy + h - 1) * dstride + dx + w) * bpp);  /* textures there changed */
    if (g_gpu) rt_gpu_transfer_end(dst, dstride * bpp, (int)dx, (int)dy, (int)w, (int)h, (int)bpp);
}

/* An HLE function (the video decoder) wrote w x h pixels at addr. Lists are drawn when they
 * end, so no queued drawing is pending here: textures on those pages and a GPU target there
 * take the new memory. */
void rt_ge_cpu_wrote(uint32_t addr, uint32_t pitch, int w, int h, int bpp) {
    if (w <= 0 || h <= 0) return;
    stamp_pages(addr, addr + (uint32_t)(h - 1) * pitch + (uint32_t)(w * bpp));
    if (g_gpu) rt_gpu_memory_written(addr, pitch, 0, 0, w, h, bpp);
}
