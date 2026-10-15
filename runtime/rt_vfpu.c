/* VFPU interpreter used by recompiled code: every VFPU instruction in the generated C is a
 * call to rt_vfpu(c, word, pc); VFPU branches (bvf/bvt) are emitted as C branches.
 *
 * Semantics follow PPSSPP's interpreter (Core/MIPS/MIPSIntVFPU.cpp, v1.20.4): register
 * layout, S/T source prefixes (swizzle, abs, constants, negate), D prefix (saturation,
 * write mask), prefix "rewrites" of the special instructions, and summation order.
 * Only the instructions used by the two games are implemented; anything else traps.
 * vsin/vcos/vasin use libm (exact at quarter turns) instead of PSP's bit-exact tables. */
#include <math.h>
#include <string.h>

#include "rt_internal.h"

enum { C_S = 0, C_T = 1, C_D = 2, C_CC = 3, C_INF4 = 4, C_REV = 7 };
enum { K_NONE = -1, K_ZERO, K_ONE };  /* VFPUConst values used here */

void rt_ctx_init(RtCtx *c) {
    static const uint32_t ctrl[16] = { 0xE4, 0xE4, 0, 0x3F, 0, 0, 0, 0x7772CEAB,
                                       0x3F800001, 0x3F800002, 0x3F800004, 0x3F800008,
                                       0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000 };
    memset(c, 0, sizeof(*c));
    memcpy(c->vctrl, ctrl, sizeof(ctrl));
}

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static int nan_or_inf(float f) { return isnan(f) || isinf(f); }

#define VD(op) ((int)((op) & 0x7F))
#define VS(op) ((int)(((op) >> 8) & 0x7F))
#define VT(op) ((int)(((op) >> 16) & 0x7F))
static int vlen(uint32_t op) { return (int)(((op >> 7) & 1) | ((op >> 14) & 2)) + 1; }

/* Physical index of element i of an n-element vector register (PPSSPP ReadVector). */
static int vidx(int reg, int n, int i) {
    int mtx = (reg >> 2) & 7, col = reg & 3;
    if (n == 1) return mtx * 16 + col * 4 + ((reg >> 5) & 3);
    int row = n == 3 ? (reg >> 6) & 1 : (reg >> 5) & 2;
    if ((reg >> 5) & 1) return mtx * 16 + col + ((row + i) & 3) * 4;
    return mtx * 16 + col * 4 + ((row + i) & 3);
}
/* Physical index of element i of vector j of a side x side matrix register (PPSSPP ReadMatrix). */
static int midx(int reg, int side, int j, int i) {
    int mtx = (reg >> 2) & 7, col = reg & 3, tr = (reg >> 5) & 1, row;
    if (side == 1) { tr = 0; row = (reg >> 5) & 3; }
    else row = side == 3 ? (reg >> 6) & 1 : (reg >> 5) & 2;
    if (tr) return mtx * 16 + ((row + i) & 3) * 4 + ((col + j) & 3);
    return mtx * 16 + ((col + j) & 3) * 4 + ((row + i) & 3);
}

static void vread(RtCtx *c, uint32_t *d, int n, int reg) { for (int i = 0; i < n; i++) d[i] = c->v[vidx(reg, n, i)].i; }
static void vwrite(RtCtx *c, const uint32_t *d, int n, int reg) {
    uint32_t mask = (c->vctrl[C_D] >> 8) & 0xF;
    for (int i = 0; i < n; i++) if (!((mask >> i) & 1)) c->v[vidx(reg, n, i)].i = d[i];
}
static void mread(RtCtx *c, uint32_t *d, int side, int reg) {
    for (int j = 0; j < side; j++) for (int i = 0; i < side; i++) d[j * 4 + i] = c->v[midx(reg, side, j, i)].i;
}
static void mwrite(RtCtx *c, const uint32_t *d, int side, int reg) {
    for (int j = 0; j < side; j++) for (int i = 0; i < side; i++) c->v[midx(reg, side, j, i)].i = d[j * 4 + i];
}

/* --- prefixes ------------------------------------------------------------------- */
static void apply_st(uint32_t *r, uint32_t data, int n, uint32_t invalid) {
    static const float k[8] = { 0.f, 1.f, 2.f, 0.5f, 3.f, 1.f / 3.f, 0.25f, 1.f / 6.f };
    if (data == 0xE4) return;
    uint32_t orig[4] = { invalid, invalid, invalid, invalid };
    for (int i = 0; i < n; i++) orig[i] = r[i];
    for (int i = 0; i < n; i++) {
        int reg = (data >> (i * 2)) & 3, ab = (data >> (8 + i)) & 1, neg = (data >> (16 + i)) & 1, cst = (data >> (12 + i)) & 1;
        if (!cst) { r[i] = orig[reg]; if (ab) r[i] &= 0x7FFFFFFFu; }
        else r[i] = f2u(k[reg + (ab << 2)]);
        if (neg) r[i] ^= 0x80000000u;
    }
}
static uint32_t rewrite(RtCtx *c, int ctrl, uint32_t remove, uint32_t add) { return (c->vctrl[ctrl] & ~remove) | add; }
static uint32_t make_consts(int x, int y, int z, int w) {
    int k[4] = { x, y, z, w };
    uint32_t r = 0;
    for (int i = 0; i < 4; i++)
        if (k[i] != K_NONE) r |= ((uint32_t)(k[i] & 3) << (i * 2)) | ((uint32_t)((k[i] >> 2) & 1) << (8 + i)) | (1u << (12 + i));
    return r;
}
static float clampf(float v, float lo, float hi) { return v >= hi ? hi : (v <= lo ? lo : v); }
static void apply_d(RtCtx *c, uint32_t *v, int n, int only_mask) {
    uint32_t data = c->vctrl[C_D];
    if (!data || only_mask) return;
    for (int i = 0; i < n; i++) {
        int sat = (data >> (i * 2)) & 3;
        if (sat == 1) v[i] = f2u(clampf(u2f(v[i]), 0.0f, 1.0f));
        else if (sat == 3) v[i] = f2u(clampf(u2f(v[i]), -1.0f, 1.0f));
    }
}
/* Some instructions apply D (mask and saturation) only to their last element. */
static void d_last_only(RtCtx *c, int last) {
    uint32_t d = c->vctrl[C_D];
    c->vctrl[C_D] = ((d & (1u << 8)) << last) | ((d & 3u) << (last * 2));
}
static void retain_invalid(RtCtx *c, uint32_t *d, int n) {
    uint32_t s = c->vctrl[C_S], t = c->vctrl[C_T];
    for (int i = 0; i < n; i++) {
        int sws = (s >> (2 * i)) & 3, swt = (t >> (2 * i)) & 3, cs = (s >> (12 + i)) & 1, ct = (t >> (12 + i)) & 1;
        if ((sws >= n && !cs) || (swt >= n && !ct)) d[i] = 0;
    }
}
static void eat(RtCtx *c) { c->vctrl[C_S] = 0xE4; c->vctrl[C_T] = 0xE4; c->vctrl[C_D] = 0; }

/* --- transcendentals (angles in quarter turns, as on the VFPU) ------------------------- */
static float vsin(float x) {
    if (nan_or_inf(x)) return u2f((f2u(x) & 0x80000000u) ^ 0x7F800001u);
    double r = fmod((double)x, 4.0);
    if (r < 0) r += 4.0;
    if (r == 0.0) return x == 0.0f ? x : 0.0f;
    if (r == 1.0) return 1.0f;
    if (r == 2.0) return 0.0f;
    if (r == 3.0) return -1.0f;
    return (float)sin(r * 1.5707963267948966);
}
static float vcos(float x) {
    if (nan_or_inf(x)) return u2f((f2u(x) & 0x80000000u) ^ 0x7F800001u);
    double r = fmod(fabs((double)x), 4.0);
    if (r == 0.0) return 1.0f;
    if (r == 1.0 || r == 3.0) return 0.0f;
    if (r == 2.0) return -1.0f;
    return (float)cos(r * 1.5707963267948966);
}
static float vasin(float x) { return (float)(asin((double)x) * 0.6366197723675814); }

/* --- instructions ------------------------------------------------------------------ */
static void op_do3(RtCtx *c, uint32_t op, int type) {  /* 0 vadd, 1 vsub, 7 vdiv, 8 vmul */
    int n = vlen(op);
    uint32_t s[4], t[4], d[4];
    vread(c, s, n, VS(op));
    vread(c, t, n, VT(op));
    if (type != 7) { apply_st(s, c->vctrl[C_S], n, 0); apply_st(t, c->vctrl[C_T], n, 0); }
    else { apply_st(&s[n - 1], c->vctrl[C_S], 1, f2u(-INFINITY)); apply_st(&t[n - 1], c->vctrl[C_T], 1, f2u(-INFINITY)); }
    for (int i = 0; i < n; i++) {
        float a = u2f(s[i]), b = u2f(t[i]);
        d[i] = f2u(type == 0 ? a + b : type == 1 ? a - b : type == 7 ? a / b : a * b);
    }
    if (type == 7) d_last_only(c, n - 1);
    else retain_invalid(c, d, n);
    apply_d(c, d, n, 0);
    vwrite(c, d, n, VD(op));
    eat(c);
}

static void op_vdot(RtCtx *c, uint32_t op) {
    int n = vlen(op);
    uint32_t s[4] = { 0 }, t[4] = { 0 };
    vread(c, s, n, VS(op));
    apply_st(s, c->vctrl[C_S], 4, 0);
    vread(c, t, n, VT(op));
    apply_st(t, c->vctrl[C_T], 4, 0);
    float d = 0.0f;
    for (int i = 0; i < 4; i++) d += u2f(s[i]) * u2f(t[i]);
    uint32_t r = f2u(d);
    apply_d(c, &r, 1, 0);
    vwrite(c, &r, 1, VD(op));
    eat(c);
}

static void op_vscl(RtCtx *c, uint32_t op) {
    int n = vlen(op), tl = (VT(op) >> 5) & 3;
    uint32_t s[4], t[4] = { 0 }, d[4];
    vread(c, s, n, VS(op));
    apply_st(s, c->vctrl[C_S], n, 0);
    t[tl] = c->v[vidx(VT(op), 1, 0)].i;
    apply_st(t, rewrite(c, C_T, 0xFF, (uint32_t)tl * 0x55u), 4, 0);
    for (int i = 0; i < n; i++) d[i] = f2u(u2f(s[i]) * u2f(t[i]));
    apply_d(c, d, n, 0);
    vwrite(c, d, n, VD(op));
    eat(c);
}

static void op_vminmax(RtCtx *c, uint32_t op, int is_min) {
    int n = vlen(op);
    uint32_t s[4], t[4], d[4];
    vread(c, s, n, VS(op));
    apply_st(s, c->vctrl[C_S], n, 0);
    vread(c, t, n, VT(op));
    apply_st(t, c->vctrl[C_T], n, 0);
    for (int i = 0; i < n; i++) {
        float fs = u2f(s[i]), ft = u2f(t[i]);
        int32_t is = (int32_t)s[i], it = (int32_t)t[i];
        if (nan_or_inf(fs) || nan_or_inf(ft)) {
            int both_neg = is < 0 && it < 0;
            int take_max = is_min ? both_neg : !both_neg;
            d[i] = (uint32_t)(take_max ? (it > is ? it : is) : (it < is ? it : is));
        } else if (is_min) {
            d[i] = f2u(fs < ft ? fs : ft);  /* std::min(t, s) */
        } else {
            d[i] = f2u(ft < fs ? fs : ft);  /* std::max(t, s) */
        }
    }
    retain_invalid(c, d, n);
    apply_d(c, d, n, 0);
    vwrite(c, d, n, VD(op));
    eat(c);
}

static void op_vcmp(RtCtx *c, uint32_t op) {
    int n = vlen(op), cond = (int)(op & 0xF);
    uint32_t s[4], t[4];
    vread(c, s, n, VS(op));
    apply_st(s, c->vctrl[C_S], n, 0);
    vread(c, t, n, VT(op));
    apply_st(t, c->vctrl[C_T], n, 0);
    int cc = 0, or_v = 0, and_v = 1, affected = 0x30;
    for (int i = 0; i < n; i++) {
        float a = u2f(s[i]), b = u2f(t[i]);
        int r = 0;
        switch (cond) {
        case 0: r = 0; break;               case 1: r = a == b; break;
        case 2: r = a < b; break;           case 3: r = a <= b; break;
        case 4: r = 1; break;               case 5: r = a != b; break;
        case 6: r = a >= b; break;          case 7: r = a > b; break;
        case 8: r = a == 0.0f; break;       case 9: r = isnan(a) != 0; break;
        case 10: r = isinf(a) != 0; break;  case 11: r = nan_or_inf(a); break;
        case 12: r = a != 0.0f; break;      case 13: r = !isnan(a); break;
        case 14: r = !isinf(a); break;      default: r = !nan_or_inf(a); break;
        }
        cc |= r << i;
        or_v |= r;
        and_v &= r;
        affected |= 1 << i;
    }
    c->vctrl[C_CC] = (c->vctrl[C_CC] & ~(uint32_t)affected) | ((uint32_t)(cc | (or_v << 4) | (and_v << 5)) & (uint32_t)affected);
    eat(c);
}

static void op_vcmov(RtCtx *c, uint32_t op, uint32_t pc) {
    int n = vlen(op), tf = (op >> 19) & 1, imm3 = (op >> 16) & 7;
    uint32_t s[4], d[4];
    vread(c, s, n, VS(op));
    apply_st(s, c->vctrl[C_S], n, 0);
    vread(c, d, n, VD(op));
    apply_st(d, c->vctrl[C_T], n, 0);
    uint32_t cc = c->vctrl[C_CC];
    if (imm3 < 6) {
        if ((int)((cc >> imm3) & 1) == !tf) for (int i = 0; i < n; i++) d[i] = s[i];
    } else if (imm3 == 6) {
        for (int i = 0; i < n; i++) if ((int)((cc >> i) & 1) == !tf) d[i] = s[i];
    } else {
        rt_trap(c, pc, op, "vcmov with invalid condition");
    }
    apply_d(c, d, n, 0);
    vwrite(c, d, n, VD(op));
    eat(c);
}

static void op_vv2op(RtCtx *c, uint32_t op) {
    int n = vlen(op), t = (op >> 16) & 0x1F;
    uint32_t s[4], d[4];
    vread(c, s, n, VS(op));
    switch (t) {
    case 1: apply_st(s, rewrite(c, C_S, 0, 0xF00), n, 0); break;      /* vabs: forced abs */
    case 2: apply_st(s, rewrite(c, C_S, 0, 0xF0000), n, 0); break;    /* vneg: forced negate */
    case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23:
        apply_st(&s[n - 1], c->vctrl[C_S], 1, f2u(INFINITY)); break;
    case 24: case 26:
        apply_st(&s[n - 1], rewrite(c, C_S, 0x10000, 0), 1, f2u(-INFINITY)); break;
    case 28:
        apply_st(&s[n - 1], rewrite(c, C_S, 0x10000, 0), 1, f2u(INFINITY)); break;
    default: apply_st(s, c->vctrl[C_S], n, 0); break;
    }
    for (int i = 0; i < n; i++) {
        float x = u2f(s[i]), r = 0.0f;
        switch (t) {
        case 0: case 1: case 2: d[i] = s[i]; continue;
        case 4: r = x <= 0 ? 0.0f : (x > 1.0f ? 1.0f : x); if (isnan(x)) { d[i] = s[i]; continue; } break;
        case 5: r = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x); if (isnan(x)) { d[i] = s[i]; continue; } break;
        case 16: r = 1.0f / x; break;
        case 17: r = 1.0f / sqrtf(x); break;
        case 18: r = vsin(x); break;
        case 19: r = vcos(x); break;
        case 20: r = exp2f(x); break;
        case 21: r = log2f(x); break;
        case 22: r = fabsf(sqrtf(x)); break;
        case 23: r = vasin(x); break;
        case 24: r = -(1.0f / x); break;
        case 26: r = -vsin(x); break;
        case 28: r = exp2f(-x); break;
        }
        d[i] = f2u(r);
    }
    if (t == 5) apply_d(c, d, n, 1);
    else {
        if (t >= 16) d_last_only(c, n - 1);
        apply_d(c, d, n, 0);
    }
    vwrite(c, d, n, VD(op));
    eat(c);
}

static void op_vidt(RtCtx *c, uint32_t op) {
    int n = vlen(op), vd = VD(op), offmask = (n == 4 || n == 3) ? 3 : 1, off = vd & offmask;
    uint32_t f[4] = { 0 };
    uint32_t add = make_consts(off == (0 & offmask) ? K_ONE : K_ZERO, off == (1 & offmask) ? K_ONE : K_ZERO,
                               off == (2 & offmask) ? K_ONE : K_ZERO, off == (3 & offmask) ? K_ONE : K_ZERO);
    apply_st(f, rewrite(c, C_S, 0xFF, add), n, 0);
    apply_d(c, f, n, 0);
    vwrite(c, f, n, vd);
    eat(c);
}

static void op_vvecinit(RtCtx *c, uint32_t op, int one) {
    int n = vlen(op), k = one ? K_ONE : K_ZERO;
    uint32_t d[4] = { 0 };
    apply_st(d, rewrite(c, C_S, 0xFF, make_consts(k, k, k, k)), n, 0);
    apply_d(c, d, n, 0);
    vwrite(c, d, n, VD(op));
    eat(c);
}

static void op_vmatinit(RtCtx *c, uint32_t op, uint32_t pc) {
    int side = vlen(op), kind = (op >> 16) & 0xF;
    if (c->vctrl[C_S] & 0xF0F00) rt_trap(c, pc, op, "matrix init with an S prefix (not supported)");
    uint32_t m[16];
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            m[j * 4 + i] = kind == 3 ? (i == j ? f2u(1.0f) : 0) : kind == 7 ? f2u(1.0f) : 0;
    mwrite(c, m, side, VD(op));
    eat(c);
}

static void op_vmmov(RtCtx *c, uint32_t op) {
    int side = vlen(op), off = side - 1;
    uint32_t s[16] = { 0 };
    mread(c, s, side, VS(op));
    apply_st(&s[off * 4], c->vctrl[C_S], 4, 0);
    apply_d(c, &s[off * 4], 4, 0);
    mwrite(c, s, side, VD(op));
    eat(c);
}

static void op_vmmul(RtCtx *c, uint32_t op) {
    int n = vlen(op);
    uint32_t s[16] = { 0 }, t[16] = { 0 }, d[16] = { 0 };
    mread(c, s, n, VS(op));
    mread(c, t, n, VT(op));
    for (int a = 0; a < n; a++)
        for (int b = 0; b < n; b++) {
            float sum = 0.0f;
            int last = a == n - 1 && b == n - 1;
            if (last) { apply_st(&s[b * 4], c->vctrl[C_S], 4, 0); apply_st(&t[a * 4], c->vctrl[C_T], 4, 0); }
            for (int k = 0; k < (last ? 4 : n); k++) sum += u2f(s[b * 4 + k]) * u2f(t[a * 4 + k]);
            d[a * 4 + b] = f2u(sum);
        }
    d_last_only(c, n - 1);
    apply_d(c, &d[4 * (n - 1)], 4, 0);
    mwrite(c, d, n, VD(op));
    eat(c);
}

static void op_vtfm(RtCtx *c, uint32_t op) {
    int ins = (op >> 23) & 3, n = vlen(op), tn = n < ins + 1 ? n : ins + 1;
    uint32_t s[16] = { 0 }, t[4] = { 0 }, d[4] = { 0 };
    mread(c, s, ins + 1, VS(op));
    vread(c, t, ins + 1, VT(op));
    for (int i = 0; i < ins; i++) {
        float acc = u2f(s[i * 4]) * u2f(t[0]);
        for (int k = 1; k < tn; k++) acc += u2f(s[i * 4 + k]) * u2f(t[k]);
        if (ins >= n) acc += u2f(s[i * 4 + ins]);
        d[i] = f2u(acc);
    }
    apply_st(&s[ins * 4], c->vctrl[C_S], 4, 0);
    int k4[4] = { K_NONE, n < 2 ? K_ZERO : K_NONE, n < 3 ? K_ZERO : K_NONE, n < 4 ? K_ZERO : K_NONE };
    if (ins >= n) k4[ins] = K_ONE;
    uint32_t remove = (uint32_t)((n < 2 ? 3 : 0) << 2 | (n < 3 ? 3 : 0) << 4 | (n < 4 ? 3 : 0) << 6);
    apply_st(t, rewrite(c, C_T, remove, make_consts(k4[0], k4[1], k4[2], k4[3])), 4, 0);
    float acc = u2f(s[ins * 4]) * u2f(t[0]);
    for (int k = 1; k < 4; k++) acc += u2f(s[ins * 4 + k]) * u2f(t[k]);
    d[ins] = f2u(acc);
    d_last_only(c, ins);
    apply_d(c, d, ins + 1, 0);
    vwrite(c, d, ins + 1, VD(op));
    eat(c);
}

static void op_vcrsp(RtCtx *c, uint32_t op, uint32_t pc) {
    int n = vlen(op);
    uint32_t s[4] = { 0 }, t[4] = { 0 }, d[4] = { 0 };
    vread(c, s, n, VS(op));
    vread(c, t, n, VT(op));
    float fs[4], ft[4];
    for (int i = 0; i < 4; i++) { fs[i] = u2f(s[i]); ft[i] = u2f(t[i]); }
    uint32_t remove = 0xFFu | 0xF0000u;
    if (n == 3) {  /* vcrsp.t */
        d[0] = f2u(fs[1] * ft[2] - fs[2] * ft[1]);
        d[1] = f2u(fs[2] * ft[0] - fs[0] * ft[2]);
        apply_st(t, rewrite(c, C_T, remove, (1u | 0u << 2 | 3u << 4 | 2u << 6) | 0x20000u), 4, 0);
        apply_st(s, c->vctrl[C_S], 4, 0);
        d[2] = f2u(u2f(s[0]) * u2f(t[0]) + u2f(s[1]) * u2f(t[1]) + u2f(s[2]) * u2f(t[2]) + u2f(s[3]) * u2f(t[3]));
    } else if (n == 4) {  /* vqmul.q */
        d[0] = f2u(fs[0] * ft[3] + fs[1] * ft[2] - fs[2] * ft[1] + fs[3] * ft[0]);
        d[1] = f2u(-fs[0] * ft[2] + fs[1] * ft[3] + fs[2] * ft[0] + fs[3] * ft[1]);
        d[2] = f2u(fs[0] * ft[1] - fs[1] * ft[0] + fs[2] * ft[3] + fs[3] * ft[2]);
        apply_st(t, rewrite(c, C_T, remove, (0u | 1u << 2 | 2u << 4 | 3u << 6) | 0x70000u), 4, 0);
        apply_st(s, c->vctrl[C_S], 4, 0);
        d[3] = f2u(u2f(s[0]) * u2f(t[0]) + u2f(s[1]) * u2f(t[1]) + u2f(s[2]) * u2f(t[2]) + u2f(s[3]) * u2f(t[3]));
    } else {
        rt_trap(c, pc, op, "vcrsp/vqmul with pair/single size (not supported)");
    }
    d_last_only(c, n - 1);
    apply_d(c, d, n, 0);
    vwrite(c, d, n, VD(op));
    eat(c);
}

static void op_vocp(RtCtx *c, uint32_t op) {
    int n = vlen(op);
    uint32_t s[4], t[4] = { 0 }, d[4];
    vread(c, s, n, VS(op));
    apply_st(s, rewrite(c, C_S, 0, 0xF0000u), n, 0);
    apply_st(t, rewrite(c, C_T, 0xFF, make_consts(K_ONE, K_ONE, K_ONE, K_ONE)), n, 0);
    for (int i = 0; i < n; i++) {
        float x = u2f(s[i]);
        d[i] = isnan(x) ? f2u(fabsf(x)) : f2u(u2f(t[i]) + x);
    }
    retain_invalid(c, d, n);
    apply_d(c, d, n, 0);
    vwrite(c, d, n, VD(op));
    eat(c);
}

static void op_vsgn(RtCtx *c, uint32_t op) {
    int n = vlen(op);
    uint32_t s[4] = { 0 }, t[4] = { 0 }, d[4];
    vread(c, s, n, VS(op));
    apply_st(t, rewrite(c, C_T, 0xFF, make_consts(K_ZERO, K_ZERO, K_ZERO, K_ZERO)), n, 0);
    for (int i = n; i < 4; i++) s[i] = t[i];
    apply_st(s, c->vctrl[C_S], 4, 0);
    for (int i = 0; i < n; i++) {
        uint32_t v = f2u(u2f(s[i]) - u2f(t[i]));
        d[i] = (v == 0 || v == 0x80000000u) ? 0 : (v >> 31) == 0 ? f2u(1.0f) : f2u(-1.0f);
    }
    apply_d(c, d, n, 0);
    vwrite(c, d, n, VD(op));
    eat(c);
}

static void op_vf2i(RtCtx *c, uint32_t op) {
    int n = vlen(op), imm = (op >> 16) & 0x1F, mode = (op >> 21) & 0x1F;
    float mult = (float)(1u << imm);
    uint32_t s[4], d[4];
    vread(c, s, n, VS(op));
    apply_st(s, c->vctrl[C_S], n, 0);
    for (int i = 0; i < n; i++) {
        float x = u2f(s[i]);
        if (isnan(x)) { d[i] = 0x7FFFFFFFu; continue; }
        double sv = (double)(x * mult);
        if (sv > 2147483647.0) d[i] = 0x7FFFFFFFu;
        else if (sv <= -2147483648.0) d[i] = 0x80000000u;
        else {
            switch (mode) {
            case 16: d[i] = (uint32_t)(int32_t)rint(sv); break;
            case 17: d[i] = (uint32_t)(int32_t)(x >= 0 ? floor(sv) : ceil(sv)); break;
            case 18: d[i] = (uint32_t)(int32_t)ceil(sv); break;
            default: d[i] = (uint32_t)(int32_t)floor(sv); break;
            }
        }
    }
    apply_d(c, d, n, 1);
    vwrite(c, d, n, VD(op));
    eat(c);
}

static void op_vi2f(RtCtx *c, uint32_t op) {
    int n = vlen(op), imm = (op >> 16) & 0x1F;
    float mult = 1.0f / (float)(1u << imm);
    uint32_t s[4], d[4];
    vread(c, s, n, VS(op));
    apply_st(s, c->vctrl[C_S], n, 0);
    for (int i = 0; i < n; i++) d[i] = f2u((float)(int32_t)s[i] * mult);
    apply_d(c, d, n, 0);
    vwrite(c, d, n, VD(op));
    eat(c);
}

static void op_vi2x(RtCtx *c, uint32_t op) {
    int n = vlen(op), kind = (op >> 16) & 3, oz = 1;
    uint32_t s[4] = { 0 }, d[2] = { 0 };
    vread(c, s, n, VS(op));
    apply_st(s, c->vctrl[C_S], 4, 0);
    if (kind == 0) {        /* vi2uc */
        for (int i = 0; i < 4; i++) { int32_t v = (int32_t)s[i]; if (v < 0) v = 0; v >>= 23; d[0] |= ((uint32_t)v & 0xFF) << (i * 8); }
    } else if (kind == 1) { /* vi2c */
        for (int i = 0; i < 4; i++) d[0] |= (s[i] >> 24) << (i * 8);
    } else {                /* vi2us / vi2s */
        int elems = (n + 1) / 2;
        for (int i = 0; i < elems; i++) {
            if (kind == 2) {
                int32_t lo = (int32_t)s[i * 2], hi = (int32_t)s[i * 2 + 1];
                if (lo < 0) lo = 0;
                if (hi < 0) hi = 0;
                d[i] = ((uint32_t)lo >> 15) | (((uint32_t)hi >> 15) << 16);
            } else {
                d[i] = (s[i * 2] >> 16) | ((s[i * 2 + 1] >> 16) << 16);
            }
        }
        oz = n >= 3 ? 2 : 1;
    }
    apply_d(c, d, oz, 0);
    vwrite(c, d, oz, VD(op));
    eat(c);
}

static float half_to_float(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, frac = h & 0x3FF;
    if (exp == 0x1F) return u2f((sign << 31) | (255u << 23) | frac);
    if (exp == 0 && frac == 0) return sign ? -0.0f : 0.0f;
    int e = (int)exp;
    if (e == 0) { do { frac <<= 1; e--; } while (!(frac & 0x400)); frac &= 0x3FF; }
    return u2f((sign << 31) | ((uint32_t)(e + 112) << 23) | (frac << 13));
}

static void op_viim(RtCtx *c, uint32_t op, int type) {
    uint32_t f = type == 6 ? f2u((float)(int16_t)(op & 0xFFFF)) : f2u(half_to_float((uint16_t)(op & 0xFFFF)));
    apply_d(c, &f, 1, 0);
    vwrite(c, &f, 1, VT(op));
    eat(c);
}

static void op_mxv(RtCtx *c, uint32_t op, uint32_t pc) {
    int imm = op & 0xFF, rt = (op >> 16) & 0x1F, to_vfpu = ((op >> 21) & 0x1F) == 7;
    if (!to_vfpu) {
        if (rt == 0) return;
        if (imm < 128) c->r[rt] = c->v[vidx(imm, 1, 0)].i;
        else if (imm < 144) c->r[rt] = c->vctrl[imm - 128];
        else rt_trap(c, pc, op, "mfv: invalid register");
        return;
    }
    if (imm < 128) { c->v[vidx(imm, 1, 0)].i = c->r[rt]; return; }
    static const uint32_t mask[16] = { 0xFFFFF, 0xFFFFF, 0xFFF, 0x3F, 0xFFFFFFFFu, 0, 0, 0,
                                       0x3FFFFFFF, 0x3FFFFFFF, 0x3FFFFFFF, 0x3FFFFFFF, 0x3FFFFFFF, 0x3FFFFFFF, 0x3FFFFFFF, 0x3FFFFFFF };
    if (imm >= 144) rt_trap(c, pc, op, "mtv: invalid register");
    else if (mask[imm - 128]) c->vctrl[imm - 128] = c->r[rt] & mask[imm - 128];
}

static uint32_t vaddr(RtCtx *c, uint32_t op) { return c->r[(op >> 21) & 0x1F] + (uint32_t)(int32_t)(int16_t)(op & 0xFFFC); }

void rt_vfpu(RtCtx *c, uint32_t op, uint32_t pc) {
    switch (op >> 26) {
    case 18: {  /* COP2: mfv/mfvc, mtv/mtvc (bvf/bvt are emitted as C branches) */
        int rs = (op >> 21) & 0x1F;
        if (rs == 3 || rs == 7) op_mxv(c, op, pc);
        else rt_trap(c, pc, op, "unsupported COP2 instruction");
        return;
    }
    case 24: {
        int s = (op >> 23) & 7;
        if (s == 0 || s == 1 || s == 7) op_do3(c, op, s);
        else rt_trap(c, pc, op, "unsupported VFPU0 instruction");
        return;
    }
    case 25: {
        int s = (op >> 23) & 7;
        if (s == 0) op_do3(c, op, 8);
        else if (s == 1) op_vdot(c, op);
        else if (s == 2) op_vscl(c, op);
        else rt_trap(c, pc, op, "unsupported VFPU1 instruction");
        return;
    }
    case 27: {
        int s = (op >> 23) & 7;
        if (s == 0) op_vcmp(c, op);
        else if (s == 2 || s == 3) op_vminmax(c, op, s == 2);
        else rt_trap(c, pc, op, "unsupported VFPU3 instruction");
        return;
    }
    case 50: {  /* lv.s */
        int vt = ((op >> 16) & 0x1F) | ((op & 3) << 5);
        c->v[vidx(vt, 1, 0)].i = rt_r32(vaddr(c, op));
        return;
    }
    case 58: {  /* sv.s */
        int vt = ((op >> 16) & 0x1F) | ((op & 3) << 5);
        rt_w32(vaddr(c, op), c->v[vidx(vt, 1, 0)].i);
        return;
    }
    case 54: {  /* lv.q */
        int vt = ((op >> 16) & 0x1F) | ((op & 1) << 5);
        uint32_t a = vaddr(c, op), d[4];
        for (int i = 0; i < 4; i++) d[i] = rt_r32(a + 4u * (uint32_t)i);
        vwrite(c, d, 4, vt);
        return;
    }
    case 62: {  /* sv.q */
        int vt = ((op >> 16) & 0x1F) | ((op & 1) << 5);
        uint32_t a = vaddr(c, op), d[4];
        vread(c, d, 4, vt);
        for (int i = 0; i < 4; i++) rt_w32(a + 4u * (uint32_t)i, d[i]);
        return;
    }
    case 52: {
        int sub = (op >> 21) & 0x1F, t = (op >> 16) & 0x1F;
        if (sub == 0) {
            if (t == 3) op_vidt(c, op);
            else if (t == 6 || t == 7) op_vvecinit(c, op, t == 7);
            else if (t <= 5 || (t >= 16 && t <= 24) || t == 26 || t == 28) op_vv2op(c, op);
            else rt_trap(c, pc, op, "unsupported VFPU4 instruction");
        } else if (sub == 1 && t >= 28) {
            op_vi2x(c, op);
        } else if (sub == 2 && t == 4) {
            op_vocp(c, op);
        } else if (sub == 2 && t == 10) {
            op_vsgn(c, op);
        } else if (sub >= 16 && sub <= 19) {
            op_vf2i(c, op);
        } else if (sub == 20) {
            op_vi2f(c, op);
        } else if (sub == 21) {
            op_vcmov(c, op, pc);
        } else {
            rt_trap(c, pc, op, "unsupported VFPU4-group instruction");
        }
        return;
    }
    case 55: {
        int s = (op >> 23) & 7;
        if (s <= 5) {  /* vpfxs / vpfxt / vpfxd */
            int which = (op >> 24) & 3;
            uint32_t data = op & 0x000FFFFF;
            if (which == C_D) data &= 0xFFF;
            c->vctrl[which] = data;
        } else {
            op_viim(c, op, s);
        }
        return;
    }
    case 60: {
        int s = (op >> 21) & 0x1F;
        if (s <= 3) op_vmmul(c, op);
        else if (s <= 15) op_vtfm(c, op);
        else if (s >= 20 && s <= 23) op_vcrsp(c, op, pc);
        else if (s == 28) {
            int k = (op >> 16) & 0xF;
            if (k == 0) op_vmmov(c, op);
            else if (k == 3 || k == 6 || k == 7) op_vmatinit(c, op, pc);
            else rt_trap(c, pc, op, "unsupported VFPU matrix instruction");
        } else rt_trap(c, pc, op, "unsupported VFPU6 instruction");
        return;
    }
    case 63:  /* vflush / vnop / vsync: only 0xFFFFxxxx keeps the prefixes */
        if ((op & 0xFFFF0000u) != 0xFFFF0000u) eat(c);
        return;
    default:
        rt_trap(c, pc, op, "unsupported VFPU instruction");
    }
}
