/* Minimal runtime for statically recompiled PSP code.
 * Instruction semantics mirror PPSSPP's interpreter (Core/MIPS/MIPSInt.cpp, v1.20.4). */
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef union { float f; uint32_t i; } RtFpr;

typedef struct RtCtx {
    uint32_t r[32];
    uint32_t hi, lo;
    RtFpr fpr[32];
    uint32_t fcr31, fpcond;
    uint32_t ic;
    /* VFPU: 128 registers, physical index = matrix*16 + column*4 + row (PPSSPP layout),
     * and control registers (S/T/D prefix, CC, INF4, RSV, REV, RCX0-7). */
    RtFpr v[128];
    uint32_t vctrl[16];
} RtCtx;

/* Resets a CPU context, including the VFPU prefix/control defaults. */
void rt_ctx_init(RtCtx *c);
/* Executes one VFPU instruction (rt_vfpu.c). Branches (bvf/bvt) are emitted as C. */
void rt_vfpu(RtCtx *c, uint32_t op, uint32_t pc);
#define RT_VFPU_CC(c, i) (((c)->vctrl[3] >> (i)) & 1)

typedef void (*RtFunc)(RtCtx *c);
typedef struct { uint32_t addr; RtFunc fn; } RtFuncEntry;

/* Generated per game */
extern const RtFuncEntry rt_funcs[];
extern const size_t rt_funcs_count;
extern const char *const RT_GAME_ID;
extern const char *const RT_MODULE_NAME;
extern const uint32_t RT_IMAGE_BASE, RT_IMAGE_SIZE, RT_ENTRY, RT_GP;

/* Guest memory: a flat arena indexed by the PSP physical address (mirrors masked off). */
extern uint8_t *rt_mem;
#define RT_ADDR_MASK 0x1FFFFFFFu
#define RT_ARENA_SIZE 0x20000000u

static inline uint8_t *rt_ptr(uint32_t a) { return rt_mem + (a & RT_ADDR_MASK); }
static inline uint32_t rt_r32(uint32_t a) { uint32_t v; memcpy(&v, rt_ptr(a), 4); return v; }
static inline uint32_t rt_r16(uint32_t a) { uint16_t v; memcpy(&v, rt_ptr(a), 2); return v; }
static inline uint32_t rt_r8(uint32_t a) { return *rt_ptr(a); }
static inline void rt_w32(uint32_t a, uint32_t v) { memcpy(rt_ptr(a), &v, 4); }
static inline void rt_w16(uint32_t a, uint16_t v) { memcpy(rt_ptr(a), &v, 2); }
static inline void rt_w8(uint32_t a, uint8_t v) { *rt_ptr(a) = v; }

static inline uint32_t rt_lwl(uint32_t a, uint32_t rt) {
    uint32_t shift = (a & 3) * 8, mem = rt_r32(a & ~3u);
    return (rt & (0x00ffffffu >> shift)) | (mem << (24 - shift));
}
static inline uint32_t rt_lwr(uint32_t a, uint32_t rt) {
    uint32_t shift = (a & 3) * 8, mem = rt_r32(a & ~3u);
    return (rt & (0xffffff00u << (24 - shift))) | (mem >> shift);
}
static inline void rt_swl(uint32_t a, uint32_t rt) {
    uint32_t shift = (a & 3) * 8, mem = rt_r32(a & ~3u);
    rt_w32(a & ~3u, (rt >> (24 - shift)) | (mem & (0xffffff00u << shift)));
}
static inline void rt_swr(uint32_t a, uint32_t rt) {
    uint32_t shift = (a & 3) * 8, mem = rt_r32(a & ~3u);
    rt_w32(a & ~3u, (rt << shift) | (mem & (0x00ffffffu >> (24 - shift))));
}

static inline uint32_t rt_rotr(uint32_t v, uint32_t s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
static inline uint32_t rt_clz32(uint32_t v) { uint32_t n = 0; if (!v) return 32; while (!(v & 0x80000000u)) { v <<= 1; n++; } return n; }
static inline uint32_t rt_bswap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
static inline uint32_t rt_bitrev(uint32_t v) { uint32_t t = 0; for (int i = 0; i < 32; i++) if (v & (1u << i)) t |= 0x80000000u >> i; return t; }

static inline void rt_set_hilo(RtCtx *c, uint64_t v) { c->lo = (uint32_t)v; c->hi = (uint32_t)(v >> 32); }
static inline uint64_t rt_hilo(RtCtx *c) { return ((uint64_t)c->hi << 32) | c->lo; }
static inline void rt_mult(RtCtx *c, uint32_t a, uint32_t b) { rt_set_hilo(c, (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b)); }
static inline void rt_multu(RtCtx *c, uint32_t a, uint32_t b) { rt_set_hilo(c, (uint64_t)a * b); }
static inline void rt_madd(RtCtx *c, uint32_t a, uint32_t b) { rt_set_hilo(c, (uint64_t)((int64_t)rt_hilo(c) + (int64_t)(int32_t)a * (int64_t)(int32_t)b)); }
static inline void rt_maddu(RtCtx *c, uint32_t a, uint32_t b) { rt_set_hilo(c, rt_hilo(c) + (uint64_t)a * b); }
static inline void rt_msub(RtCtx *c, uint32_t a, uint32_t b) { rt_set_hilo(c, (uint64_t)((int64_t)rt_hilo(c) - (int64_t)(int32_t)a * (int64_t)(int32_t)b)); }
static inline void rt_msubu(RtCtx *c, uint32_t a, uint32_t b) { rt_set_hilo(c, rt_hilo(c) - (uint64_t)a * b); }
static inline void rt_div(RtCtx *c, uint32_t ua, uint32_t ub) {
    int32_t a = (int32_t)ua, b = (int32_t)ub;
    if (a == INT32_MIN && b == -1) { c->lo = 0x80000000u; c->hi = 0xFFFFFFFFu; }
    else if (b != 0) { c->lo = (uint32_t)(a / b); c->hi = (uint32_t)(a % b); }
    else { c->lo = a < 0 ? 1u : 0xFFFFFFFFu; c->hi = ua; }
}
static inline void rt_divu(RtCtx *c, uint32_t a, uint32_t b) {
    if (b != 0) { c->lo = a / b; c->hi = a % b; }
    else { c->lo = a <= 0xFFFFu ? 0xFFFFu : 0xFFFFFFFFu; c->hi = a; }
}

/* Float -> int conversions. NaN/Inf follow PPSSPP; finite overflow saturates (no C UB). */
static inline uint32_t rt_f2i_sat(double v) {
    if (v >= 2147483648.0) return 0x7FFFFFFFu;
    if (v < -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}
static inline int rt_nan_or_inf(float f) { return isnan(f) || isinf(f); }
static inline uint32_t rt_special_w(float f) { return (isinf(f) && f < 0.0f) ? 0x80000000u : 0x7FFFFFFFu; }
static inline uint32_t rt_round_w_s(float f) { return rt_nan_or_inf(f) ? rt_special_w(f) : rt_f2i_sat(floorf(f + 0.5f)); }
static inline uint32_t rt_trunc_w_s(float f) { return rt_nan_or_inf(f) ? rt_special_w(f) : rt_f2i_sat(f >= 0.0f ? floorf(f) : ceilf(f)); }
static inline uint32_t rt_ceil_w_s(float f) { return rt_nan_or_inf(f) ? rt_special_w(f) : rt_f2i_sat(ceilf(f)); }
static inline uint32_t rt_floor_w_s(float f) { return rt_nan_or_inf(f) ? rt_special_w(f) : rt_f2i_sat(floorf(f)); }
static inline uint32_t rt_cvt_w_s(RtCtx *c, float f) {
    if (rt_nan_or_inf(f)) return rt_special_w(f);
    switch (c->fcr31 & 3) {
    case 0: return rt_f2i_sat(rintf(f));
    case 1: return rt_f2i_sat((double)f);
    case 2: return rt_f2i_sat(ceilf(f));
    default: return rt_f2i_sat(floorf(f));
    }
}
static inline uint32_t rt_cfc1(RtCtx *c, int fs) {
    if (fs == 31) { c->fcr31 = (c->fcr31 & ~(1u << 23)) | ((c->fpcond & 1) << 23); return c->fcr31; }
    return fs == 0 ? 0x00003351u : 0;  /* FCR0 value used by PPSSPP */
}
static inline void rt_ctc1(RtCtx *c, int fs, uint32_t v) {
    if (fs == 31) { c->fcr31 = v & 0x0181FFFFu; c->fpcond = (v >> 23) & 1; }
}

/* Implemented in rt_core.c / rt_hle.c */
void rt_call(RtCtx *c, uint32_t addr);
void rt_bad_jump(RtCtx *c, uint32_t pc, uint32_t target);
void rt_trap(RtCtx *c, uint32_t pc, uint32_t word, const char *what);
void rt_hle(RtCtx *c, uint32_t nid, const char *lib, const char *name);
