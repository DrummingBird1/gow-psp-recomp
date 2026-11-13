/* Game functions the runtime provides because the translation alone misbehaves on a PC
 * (wired in by recomp/runtime_funcs.json, like rt_coro.c).
 *
 * Ghost of Sparta: animation controllers at a high frame rate (issues #51, #56, #62, #65, #66)
 *
 * The game advances its animation and event timelines in whole milliseconds. FUN_00261854
 * (0x08A65854, "advance controller CTL by DT ms") scales the frame time by the controller's
 * speed (float at CTL + 0xC) and rounds it to the nearest millisecond:
 *
 *     step = (int)(dt * speed + 0.5)        (dt: 16 or 17 at 60 FPS)
 *
 * and hands the step to the timeline (CTL + 0, its vtable's +0x10 entry; FUN_00262484 adds it
 * to the position at timeline + 0x40). A controller slower than 0.5 ms per frame never moves:
 * at 60 game frames per second that is any speed below 0.03. The PSP runs heavy scenes well
 * below 60 FPS, where the same speed still moves. "The Vortex" (vtx_030_boat) waits on
 * such a timeline: its controller runs at speed 0.026, which is 0.42-0.44 ms per frame at
 * 60 FPS (0 after rounding) and 0.86 ms at 30 FPS (1). In the port it never advances, so the
 * boat never reaches the vortex and the enemy waves go on forever. PPSSPP has the same bug
 * (hrydgard/ppsspp#8299, "gow_vortex_hack" in its ReplaceTables.cpp).
 *
 * The fix: when the rounded step is 0 although the scaled time is above 0, the fraction is
 * carried over for that controller until it makes a whole millisecond, so such a controller
 * runs at its own speed (0.026 ms per ms) at any frame rate. Every other step, including
 * every step of a controller faster than 0.5 ms per frame, is exactly the game's. */
#include <math.h>
#include <string.h>

#include "rt_kernel.h"

#define GOS_CURRENT_CTL 0x08C14EE0u  /* the controller being advanced (saved and restored) */
#define GOS_ADVANCE_RA  0x08A65950u  /* the return address of the timeline call in FUN_00261854 */

/* carried fractions of a millisecond, by controller address */
#define SLOW_SLOTS 1024
static struct { uint32_t ctl; float carry; } g_slow[SLOW_SLOTS];
static uint64_t g_slow_steps;

static float *slow_carry(uint32_t ctl) {
    uint32_t h = (ctl >> 2) * 2654435761u >> 22;  /* 10 bits */
    for (int i = 0; i < 8; i++) {
        uint32_t k = (h + i) & (SLOW_SLOTS - 1);
        if (g_slow[k].ctl == ctl) return &g_slow[k].carry;
        if (!g_slow[k].ctl) { g_slow[k].ctl = ctl; g_slow[k].carry = 0.0f; return &g_slow[k].carry; }
    }
    g_slow[h].ctl = ctl;  /* full: a freed controller's leftover fraction is dropped */
    g_slow[h].carry = 0.0f;
    return &g_slow[h].carry;
}

/* the game's rounding: to the nearest integer, halves away from zero (cvt via trunc.w.s) */
static int32_t game_round(float f) {
    const float two31 = 2147483648.0f;
    if (f <= 0.0f) {
        float g = -f + 0.5f;
        return g < two31 ? -(int32_t)rt_trunc_w_s(g) : -(int32_t)(rt_trunc_w_s(g - two31) + 0x80000000u);
    }
    float g = f + 0.5f;
    return g < two31 ? (int32_t)rt_trunc_w_s(g) : (int32_t)(rt_trunc_w_s(g - two31) + 0x80000000u);
}

static float rd_f32(uint32_t a) { uint32_t v = rt_r32(a); float f; memcpy(&f, &v, 4); return f; }

/* FUN_00261854(ctl, dt): the game's code, in its own stack frame and with its own registers, except for
 * the carried fraction above. */
void rt_gos_advance_controller(RtCtx *c) {
    uint32_t ctl = c->r[4], dt = c->r[5], timeline = rt_r32(ctl), sp = c->r[29] - 0x20u;
    rt_w32(sp + 0xCu, c->r[16]);
    rt_w32(sp + 0x10u, c->r[17]);
    rt_w32(sp + 0x14u, c->r[18]);
    rt_w32(sp + 0x18u, c->r[31]);
    c->r[6] = timeline;
    c->r[4] = dt;
    if (!timeline) return;

    uint32_t outer = rt_r32(GOS_CURRENT_CTL);
    rt_w32(GOS_CURRENT_CTL, ctl);
    float scaled = (float)(int32_t)dt * rd_f32(ctl + 0xCu);
    int32_t step = game_round(scaled);
    if (step == 0 && scaled > 0.0f) {
        float *carry = slow_carry(ctl);
        *carry += scaled;
        step = game_round(*carry);
        *carry -= (float)step;
        if (step && !g_slow_steps++)
            rt_log("[gamefix] a controller slower than 0.5 ms per frame (%08X, %.4f ms per frame) moves by its carried fraction\n",
                   ctl, scaled);
    }
    rt_w32(sp + 0x4u, dt);
    rt_w32(sp + 0x8u, (uint32_t)step);

    uint32_t entry = rt_r32(timeline + 0xCu) + 0x10u;  /* vtable + 0x10: this adjustment, function */
    uint32_t adj = (uint32_t)(int32_t)(int16_t)rt_r16(entry), fn = rt_r32(entry + 4u);
    c->r[29] = sp;
    c->r[16] = ctl;
    c->r[17] = outer;
    c->r[18] = 0x08C10000u;
    c->r[4] = timeline + adj;
    c->r[5] = sp + 0x4u;
    c->r[6] = fn;
    c->r[7] = adj;
    c->r[31] = GOS_ADVANCE_RA;
    rt_call(c, fn);
    if (c->r[2]) rt_w32(ctl + 0x10u, rt_r32(ctl + 0x10u) | 1u);
    rt_w32(GOS_CURRENT_CTL, outer);
    c->r[16] = rt_r32(sp + 0xCu);
    c->r[17] = rt_r32(sp + 0x10u);
    c->r[18] = rt_r32(sp + 0x14u);
    c->r[31] = rt_r32(sp + 0x18u);
    c->r[29] = sp + 0x20u;
}
