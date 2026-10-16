/* Game coroutines on host fibers.
 *
 * Chains of Olympus runs its scripts in stack-copying coroutines. A resume function copies
 * the coroutine's saved stack into a shared stack region and calls a context switch that
 * saves the callee-saved registers in a 0x60-byte frame, swaps the stack pointer with a
 * global slot and "returns" to the return address saved in the other frame. The coroutine
 * yields through the same switch. Statically recompiled code cannot return into the middle
 * of another function, so the runtime provides the switch (recomp/runtime_funcs.json):
 *
 *  - memory is handled exactly as the game does it (same frame stores, same slot swap, the
 *    registers are loaded from the frame when a side continues), so the game's own stack
 *    copying and bookkeeping stay valid;
 *  - control moves between host fibers instead: one fiber per coroutine object (the game
 *    keeps the running object in a second global), the caller's fiber is the "main" side;
 *  - a coroutine seen for the first time (or whose frame no longer matches the one it was
 *    suspended with, e.g. a recycled object) starts in a new fiber at the return address
 *    stored in its frame. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_fiber.h"
#include "rt_kernel.h"

#define CO_FRAME 0x60u
#define CO_STACK (16u << 20)
#define MAX_CO 512

typedef struct {
    uint32_t obj;          /* the game's coroutine object */
    uint32_t frame, ra;    /* frame and return address it was suspended with */
    uint32_t start_frame;  /* frame to start from (new coroutines) */
    RtFiber *fiber, *main;
    KThread *thread;
    int live;
} Coro;

static Coro g_co[MAX_CO];
static Coro *g_running;
static uint64_t g_switches, g_created;

static void save_frame(RtCtx *c, uint32_t f) {
    rt_w32(f, c->r[4]);
    rt_w32(f + 4, c->r[31]);
    for (int i = 0; i < 8; i++) rt_w32(f + 8 + 4 * i, c->r[16 + i]);
    rt_w32(f + 0x28, c->r[30]);
    rt_w32(f + 0x2C, c->r[31]);
    for (int i = 0; i < 12; i++) rt_w32(f + 0x30 + 4 * i, c->fpr[20 + i].i);
}

/* Loads the registers the way the switch does when it continues on frame f. */
static void load_frame(RtCtx *c, uint32_t f) {
    c->r[4] = rt_r32(f);
    c->r[5] = rt_r32(f + 4);
    for (int i = 0; i < 8; i++) c->r[16 + i] = rt_r32(f + 8 + 4 * i);
    c->r[30] = rt_r32(f + 0x28);
    c->r[31] = rt_r32(f + 0x2C);
    for (int i = 0; i < 12; i++) c->fpr[20 + i].i = rt_r32(f + 0x30 + 4 * i);
    c->r[29] = f + CO_FRAME;
}

static void co_main(void *arg) {
    Coro *co = arg;
    RtCtx *c = &co->thread->ctx;
    load_frame(c, co->start_frame);
    rt_call(c, c->r[5]);
    /* The entry function returned: on the PSP it would continue at its return address. */
    for (int guard = 0; guard < 8; guard++) {
        uint32_t ra = c->r[31];
        rt_log("[coro] coroutine %08X returned to %08X\n", co->obj, ra);
        rt_call(c, ra);
    }
    rt_abort(RT_ABORT_TRAP, co->obj, c->r[31], "coroutine ran off its end");
}

static Coro *find(uint32_t obj) {
    for (int i = 0; i < MAX_CO; i++) if (g_co[i].live && g_co[i].obj == obj) return &g_co[i];
    return NULL;
}

static Coro *create(uint32_t obj, uint32_t start_frame) {
    for (int i = 0; i < MAX_CO; i++)
        if (!g_co[i].live) {
            Coro *co = &g_co[i];
            memset(co, 0, sizeof(*co));
            co->live = 1;
            co->obj = obj;
            co->start_frame = start_frame;
            co->thread = k_cur;
            co->fiber = rt_fiber_create(CO_STACK, co_main, co);
            g_created++;
            return co;
        }
    rt_abort(RT_ABORT_TRAP, obj, MAX_CO, "too many coroutines");
    return NULL;
}

/* f(c) for the game's context switch. swap_slot holds the other side's stack pointer,
 * cur_slot the coroutine object being run (0 outside a resume). */
void rt_co_switch(RtCtx *c, uint32_t swap_slot, uint32_t cur_slot) {
    uint32_t frame = c->r[29] - CO_FRAME;
    save_frame(c, frame);
    uint32_t target = rt_r32(swap_slot);
    rt_w32(swap_slot, frame);
    g_switches++;
    if (!g_running) {
        /* main side -> the coroutine object being resumed */
        uint32_t obj = rt_r32(cur_slot);
        Coro *co = find(obj);
        if (co && (co->frame != target || rt_r32(target + 4) != co->ra)) {
            /* the object was recycled or restarted: its old fiber is dead */
            rt_fiber_delete(co->fiber);
            co->live = 0;
            co = NULL;
        }
        if (!co) co = create(obj, target);
        if (co->thread != k_cur) rt_abort(RT_ABORT_TRAP, obj, 0, "coroutine resumed from another thread");
        co->main = k_cur->fiber;
        g_running = co;
        k_cur->fiber = co->fiber;
        rt_fiber_switch(co->main, co->fiber);
        /* the coroutine yielded back to us */
    } else {
        /* coroutine -> back to the main side */
        Coro *co = g_running;
        co->frame = frame;
        co->ra = c->r[31];
        g_running = NULL;
        k_cur->fiber = co->main;
        rt_fiber_switch(co->fiber, co->main);
        /* resumed: the main side's switch has made us g_running again */
    }
    load_frame(c, frame);  /* r5 = our return address: the C caller continues right there */
}

void rt_co_report(void) {
    int live = 0;
    for (int i = 0; i < MAX_CO; i++) live += g_co[i].live;
    if (g_switches) printf("[coro] %llu switches, %llu coroutines started, %d live\n",
                           (unsigned long long)g_switches, (unsigned long long)g_created, live);
}
