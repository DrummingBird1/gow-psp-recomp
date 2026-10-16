/* PSP thread scheduler and kernel objects (ThreadManForUser, SysMemUserForUser,
 * Kernel_Library, UtilsForUser).
 *
 * Every PSP thread runs on its own fiber. Scheduling follows the PSP model: the ready
 * thread with the numerically lowest priority runs; equal priorities are FIFO; a thread
 * runs until it blocks, exits or readies a higher-priority thread. Time is virtual
 * (microseconds) and advances to the next timed wake-up when every thread is blocked,
 * which keeps runs deterministic. Semantics follow PPSSPP (Core/HLE/sceKernel*.cpp). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rt_kernel.h"

#define MAX_OBJ 4096
#define FIBER_STACK (16u << 20)
#define MAX_WAITERS 64

/* --- objects ---------------------------------------------------------------- */
static struct { ObjKind kind; void *p; } g_obj[MAX_OBJ];

int k_obj_new(ObjKind kind, void *p) {
    for (int i = 1; i < MAX_OBJ; i++)
        if (g_obj[i].kind == OBJ_FREE) { g_obj[i].kind = kind; g_obj[i].p = p; return i; }
    return (int)SCE_ERR_NO_MEMORY;
}
void *k_obj_get(int uid, ObjKind kind) {
    return (uid > 0 && uid < MAX_OBJ && g_obj[uid].kind == kind) ? g_obj[uid].p : NULL;
}
void k_obj_free(int uid) {
    if (uid > 0 && uid < MAX_OBJ) { g_obj[uid].kind = OBJ_FREE; g_obj[uid].p = NULL; }
}

/* --- user memory partition (first fit, sorted blocks) ---------------------- */
typedef struct { uint32_t addr, size; } Blk;
static Blk g_blk[2048];
static int g_nblk;
static uint32_t g_mem_lo, g_mem_hi;

void k_mem_init(uint32_t lo, uint32_t hi) { g_mem_lo = (lo + 0xFF) & ~0xFFu; g_mem_hi = hi; g_nblk = 0; }

static int blk_insert(uint32_t addr, uint32_t size) {
    if (g_nblk >= (int)(sizeof(g_blk) / sizeof(g_blk[0]))) return 0;
    int i = g_nblk;
    while (i > 0 && g_blk[i - 1].addr > addr) { g_blk[i] = g_blk[i - 1]; i--; }
    g_blk[i].addr = addr; g_blk[i].size = size;
    g_nblk++;
    return 1;
}

uint32_t k_alloc(uint32_t size, int high, uint32_t align) {
    if (align < 0x100) align = 0x100;
    size = (size + 0xFF) & ~0xFFu;
    if (!size) size = 0x100;
    if (!high) {
        uint32_t cur = g_mem_lo;
        for (int i = 0; i <= g_nblk; i++) {
            uint32_t a = (cur + align - 1) & ~(align - 1);
            uint32_t gap_end = i < g_nblk ? g_blk[i].addr : g_mem_hi;
            if (a + size <= gap_end && a + size > a) return blk_insert(a, size) ? a : 0;
            if (i < g_nblk) cur = g_blk[i].addr + g_blk[i].size;
        }
    } else {
        uint32_t cur = g_mem_hi;
        for (int i = g_nblk - 1; i >= -1; i--) {
            uint32_t gap_start = i >= 0 ? g_blk[i].addr + g_blk[i].size : g_mem_lo;
            if (cur >= size) {
                uint32_t a = (cur - size) & ~(align - 1);
                if (a >= gap_start) return blk_insert(a, size) ? a : 0;
            }
            if (i >= 0) cur = g_blk[i].addr;
        }
    }
    return 0;
}

void k_free(uint32_t addr) {
    for (int i = 0; i < g_nblk; i++)
        if (g_blk[i].addr == addr) {
            memmove(&g_blk[i], &g_blk[i + 1], (size_t)(g_nblk - i - 1) * sizeof(Blk));
            g_nblk--;
            return;
        }
}

uint32_t k_max_free(void) {
    uint32_t best = 0, cur = g_mem_lo;
    for (int i = 0; i <= g_nblk; i++) {
        uint32_t end = i < g_nblk ? g_blk[i].addr : g_mem_hi;
        if (end > cur && end - cur > best) best = end - cur;
        if (i < g_nblk) cur = g_blk[i].addr + g_blk[i].size;
    }
    return best & ~0xFFu;
}

uint32_t k_total_free(void) {
    uint32_t used = 0;
    for (int i = 0; i < g_nblk; i++) used += g_blk[i].size;
    return g_mem_hi - g_mem_lo - used;
}

/* --- scheduler -------------------------------------------------------------- */
uint64_t k_now;
KThread *k_cur;
static RtFiber *g_sched;
static int64_t g_seq, g_front_seq;
static int g_dispatch_enabled = 1;
static int g_stopping;
static uint64_t g_switches;
static uint64_t g_next_wake;   /* no timed wait ends before this (a lower bound) */
static uint64_t g_eat_paced;   /* virtual time k_eat last kept the window in step with */

static KThread *thread_at(int i) { return g_obj[i].kind == OBJ_THREAD ? (KThread *)g_obj[i].p : NULL; }

static KThread *pick_ready(void) {
    KThread *best = NULL;
    for (int i = 1; i < MAX_OBJ; i++) {
        KThread *t = thread_at(i);
        if (!t || t->status != TS_READY) continue;
        if (!best || t->prio < best->prio || (t->prio == best->prio && t->ready_seq < best->ready_seq)) best = t;
    }
    return best;
}

static void to_scheduler(void) {
    KThread *t = k_cur;
    rt_fiber_switch(t->fiber, g_sched);
}

void k_wake(KThread *t, int32_t result) {
    if (t->timeout_ptr && t->wake_time != K_NO_TIMEOUT)
        rt_w32(t->timeout_ptr, t->wake_time > k_now ? (uint32_t)(t->wake_time - k_now) : 0);
    t->status = TS_READY;
    t->wait_type = WT_NONE;
    t->wait_result = result;
    t->wake_time = K_NO_TIMEOUT;
    t->timeout_ptr = 0;
    t->ready_seq = ++g_seq;
}

int32_t k_wait(int type, int id, uint64_t timeout_us) {
    KThread *t = k_cur;
    if (!t) rt_abort(RT_ABORT_HLE, (uint32_t)type, 0, "wait outside a thread");
    if (!g_dispatch_enabled) return (int32_t)SCE_ERR_CAN_NOT_WAIT;
    t->status = TS_WAIT;
    t->wait_type = type;
    t->wait_id = id;
    t->wait_start = k_now;
    t->wake_time = timeout_us == K_NO_TIMEOUT ? K_NO_TIMEOUT : k_now + timeout_us;
    if (t->wake_time < g_next_wake) g_next_wake = t->wake_time;
    t->wait_result = 0;
    to_scheduler();
    return t->wait_result;
}

void k_check_preempt(void) {
    if (!k_cur || !g_dispatch_enabled) return;
    KThread *best = pick_ready();
    if (best && best->prio < k_cur->prio) {
        k_cur->status = TS_READY;
        k_cur->ready_seq = --g_front_seq;  /* a preempted thread resumes first among its priority */
        to_scheduler();
    }
}

int k_waiters(int type, int id, int by_priority, KThread **out, int max) {
    int n = 0;
    for (int i = 1; i < MAX_OBJ && n < max; i++) {
        KThread *t = thread_at(i);
        if (t && t->status == TS_WAIT && t->wait_type == type && t->wait_id == id) out[n++] = t;
    }
    for (int i = 1; i < n; i++)  /* insertion sort: by priority or by wait start (FIFO) */
        for (int j = i; j > 0; j--) {
            KThread *a = out[j - 1], *b = out[j];
            int swap = by_priority ? (b->prio < a->prio || (b->prio == a->prio && b->wait_start < a->wait_start))
                                   : (b->wait_start < a->wait_start || (b->wait_start == a->wait_start && b->uid < a->uid));
            if (!swap) break;
            out[j - 1] = b; out[j] = a;
        }
    return n;
}

uint32_t k_timeout_arg(uint32_t ptr) { return ptr ? rt_r32(ptr) : 0; }
static uint64_t timeout_us(uint32_t ptr) { return ptr ? (uint64_t)rt_r32(ptr) : K_NO_TIMEOUT; }

static void wake_due(void) {
    uint64_t next = K_NO_TIMEOUT;
    for (int i = 1; i < MAX_OBJ; i++) {
        KThread *t = thread_at(i);
        if (!t || t->status != TS_WAIT || t->wake_time == K_NO_TIMEOUT) continue;
        if (t->wake_time > k_now) { if (t->wake_time < next) next = t->wake_time; continue; }
        int timed = t->wait_type == WT_DELAY || t->wait_type == WT_VBLANK || t->wait_type == WT_AUDIO || t->wait_type == WT_IO;
        k_wake(t, timed ? 0 : (int32_t)SCE_ERR_WAIT_TIMEOUT);
    }
    g_next_wake = next;
}

/* The running thread spends us of CPU time polling (PPSSPP's hleEatCycles + hleReSchedule):
 * virtual time moves on, threads whose wait has ended wake and a higher-priority one takes
 * over, and in the window the wall clock is kept in step, as when every thread waits. */
void k_eat(uint64_t us) {
    k_now += us;
    if (k_now >= g_next_wake) { wake_due(); k_check_preempt(); }
    if (k_idle_hook && k_now - g_eat_paced >= 1000) {
        g_eat_paced = k_now;
        if (k_idle_hook(k_now, k_now)) k_stop(RT_ABORT_HLE, 0, 0, "window closed");
    }
}

static void thread_release_waits_on(KThread *t, int32_t result) {
    KThread *w[MAX_WAITERS];
    int n = k_waiters(WT_THREADEND, t->uid, 0, w, MAX_WAITERS);
    for (int i = 0; i < n; i++) k_wake(w[i], result);
}

static void thread_free(KThread *t) {
    if (t->fiber && t != k_cur) { rt_fiber_delete(t->fiber); t->fiber = NULL; }
    if (t->stack_addr) k_free(t->stack_addr);
    k_obj_free(t->uid);
    free(t);
}

static void thread_exit(KThread *t, int32_t status, int del) {
    t->status = TS_DORMANT;
    t->exit_status = status;
    t->delete_on_exit = del;
    thread_release_waits_on(t, status);
    if (rt_hle_verbose || 1) rt_log("[thread] '%s' exited (status %08X)%s\n", t->name, (uint32_t)status, del ? ", deleted" : "");
    to_scheduler();  /* never resumed: a restart gets a new fiber */
    rt_abort(RT_ABORT_HLE, 0, 0, "dormant thread resumed");
}

static void thread_main(void *arg) {
    KThread *t = (KThread *)arg;
    rt_call(&t->ctx, t->entry);
    thread_exit(t, (int32_t)t->ctx.r[2], 0);
}

KThread *k_thread_create(const char *name, uint32_t entry, int prio, uint32_t stack_size, uint32_t attr) {
    KThread *t = (KThread *)calloc(1, sizeof(*t));
    if (name) strncpy(t->name, name, sizeof(t->name) - 1);
    t->entry = entry;
    t->prio = t->init_prio = prio;
    t->attr = attr;
    t->stack_size = (stack_size < 0x200 ? 0x200 : stack_size + 0xFF) & ~0xFFu;
    t->stack_addr = k_alloc(t->stack_size, 1, 0x100);
    if (!t->stack_addr) { free(t); return NULL; }
    if (!(attr & 0x00100000)) memset(rt_ptr(t->stack_addr), 0xFF, t->stack_size);  /* unless NO_FILLSTACK */
    t->gp = RT_GP;
    t->status = TS_DORMANT;
    t->wake_time = K_NO_TIMEOUT;
    t->uid = k_obj_new(OBJ_THREAD, t);
    rt_w32(t->stack_addr, (uint32_t)t->uid);
    return t;
}

void k_thread_start(KThread *t, uint32_t argsize, uint32_t argp) {
    rt_ctx_init(&t->ctx);
    uint32_t sp = t->stack_addr + t->stack_size - 0x100;
    if (argsize && argp) {
        sp = (sp - argsize) & ~0xFu;
        memmove(rt_ptr(sp), rt_ptr(argp), argsize);
        t->ctx.r[5] = sp;
    }
    t->ctx.r[4] = argsize;
    t->ctx.r[28] = t->gp;
    t->ctx.r[29] = sp - 0x40;
    if (t->fiber) rt_fiber_delete(t->fiber);
    t->fiber = rt_fiber_create(FIBER_STACK, thread_main, t);
    t->exit_status = 0;
    t->status = TS_READY;
    t->ready_seq = ++g_seq;
}

void k_stop(int kind, uint32_t addr, uint32_t extra, const char *what) {
    rt_last_abort.kind = kind;
    rt_last_abort.addr = addr;
    rt_last_abort.extra = extra;
    rt_last_abort.what = what;
    g_stopping = 1;
    if (k_cur) { to_scheduler(); for (;;) to_scheduler(); }
}

static void abort_hook(int kind, uint32_t addr, uint32_t extra, const char *what) { k_stop(kind, addr, extra, what); }

static void report_threads(void) {
    static const char *wt[] = { "-", "sleep", "delay", "sema", "evf", "thread-end", "fpl", "io", "vblank", "audio" };
    for (int i = 1; i < MAX_OBJ; i++) {
        KThread *t = thread_at(i);
        if (!t) continue;
        rt_log("  thread %3d %-24s prio %3d  %s%s%s\n", t->uid, t->name, t->prio,
               t->status == TS_READY ? "ready" : t->status == TS_WAIT ? "waiting on " : t->status == TS_DORMANT ? "dormant" : "running",
               t->status == TS_WAIT ? wt[t->wait_type] : "",
               t->status == TS_WAIT && t->wake_time != K_NO_TIMEOUT ? " (timed)" : "");
    }
}

int (*k_idle_hook)(uint64_t now_us, uint64_t next_us);

int k_run_scheduler(uint32_t cpu_ms_limit) {
    g_sched = rt_fiber_init_main();
    rt_abort_hook = abort_hook;
    clock_t start = clock();
    int result = 0;
    for (;;) {
        if (g_stopping) { result = rt_last_abort.kind; break; }
        KThread *t = pick_ready();
        if (!t) {
            uint64_t next = K_NO_TIMEOUT;
            for (int i = 1; i < MAX_OBJ; i++) {
                KThread *w = thread_at(i);
                if (w && w->status == TS_WAIT && w->wake_time < next) next = w->wake_time;
            }
            if (next == K_NO_TIMEOUT) {
                rt_last_abort.kind = RT_ABORT_DEADLOCK;
                rt_last_abort.what = "every thread is blocked with no timeout (waiting for an event the runtime does not produce yet)";
                result = RT_ABORT_DEADLOCK;
                break;
            }
            if (k_idle_hook && next > k_now && k_idle_hook(k_now, next)) {
                rt_last_abort.kind = RT_ABORT_HLE;
                rt_last_abort.what = "window closed";
                result = RT_ABORT_HLE;
                break;
            }
            if (next > k_now) k_now = next;
            wake_due();
            continue;
        }
        k_cur = t;
        t->status = TS_RUNNING;
        g_switches++;
        rt_fiber_switch(g_sched, t->fiber);
        if (t->status == TS_RUNNING) t->status = TS_READY;  /* only via k_stop */
        k_cur = NULL;
        for (int i = 1; i < MAX_OBJ; i++) {
            KThread *d = thread_at(i);
            if (d && d->status == TS_DORMANT && d->delete_on_exit) thread_free(d);
        }
        wake_due();
        if ((uint64_t)(clock() - start) * 1000 / CLOCKS_PER_SEC > cpu_ms_limit) {
            rt_last_abort.kind = RT_ABORT_BUDGET;
            rt_last_abort.what = "CPU time limit reached";
            result = RT_ABORT_BUDGET;
            break;
        }
    }
    rt_abort_hook = NULL;
    rt_log("[sched] stopped: %s (at %08X, extra %08X); virtual time %.3f s, %llu thread switches\n",
           rt_last_abort.what ? rt_last_abort.what : "?", rt_last_abort.addr, rt_last_abort.extra,
           (double)k_now / 1e6, (unsigned long long)g_switches);
    report_threads();
    return result;
}

/* --- ThreadManForUser: threads ---------------------------------------------- */
static KThread *thread_arg(uint32_t uid) { return uid == 0 ? k_cur : (KThread *)k_obj_get((int)uid, OBJ_THREAD); }

static void hle_CreateThread(RtCtx *c) {
    const char *name = A0 ? (const char *)rt_ptr(A0) : "";
    KThread *t = k_thread_create(name, A1, (int)A2, A3, T0);
    if (!t) RET(SCE_ERR_NO_MEMORY);
    rt_log("[thread] created '%s' entry %08X prio %d stack %X attr %08X -> uid %d\n", t->name, A1, (int)A2, A3, T0, t->uid);
    RET(t->uid);
}
static void hle_StartThread(RtCtx *c) {
    KThread *t = (KThread *)k_obj_get((int)A0, OBJ_THREAD);
    if (!t) RET(SCE_ERR_UNKNOWN_THID);
    if (t->status != TS_DORMANT) RET(SCE_ERR_NOT_DORMANT);
    k_thread_start(t, A1, A2);
    c->r[2] = 0;
    k_check_preempt();
    RET(0);
}
static void hle_ExitThread(RtCtx *c) { thread_exit(k_cur, (int32_t)A0, 0); }
static void hle_ExitDeleteThread(RtCtx *c) { thread_exit(k_cur, (int32_t)A0, 1); }
static void terminate(KThread *t) {
    t->status = TS_DORMANT;
    t->exit_status = (int32_t)0x800201ACu; /* SCE_KERNEL_ERROR_THREAD_TERMINATED */
    if (t->fiber) { rt_fiber_delete(t->fiber); t->fiber = NULL; }
    thread_release_waits_on(t, t->exit_status);
}
static void hle_TerminateThread(RtCtx *c) {
    KThread *t = thread_arg(A0);
    if (!t || t == k_cur) RET(SCE_ERR_UNKNOWN_THID);
    if (t->status != TS_DORMANT) terminate(t);
    RET(0);
}
static void hle_TerminateDeleteThread(RtCtx *c) {
    KThread *t = thread_arg(A0);
    if (!t || t == k_cur) RET(SCE_ERR_UNKNOWN_THID);
    if (t->status != TS_DORMANT) terminate(t);
    thread_free(t);
    RET(0);
}
static void hle_DeleteThread(RtCtx *c) {
    KThread *t = thread_arg(A0);
    if (!t || t == k_cur) RET(SCE_ERR_UNKNOWN_THID);
    if (t->status != TS_DORMANT) RET(SCE_ERR_NOT_DORMANT);
    thread_free(t);
    RET(0);
}
static void hle_WaitThreadEnd(RtCtx *c) {
    KThread *t = thread_arg(A0);
    if (!t) RET(SCE_ERR_UNKNOWN_THID);
    if (t->status == TS_DORMANT) RET(t->exit_status);
    k_cur->timeout_ptr = A1;
    RET(k_wait(WT_THREADEND, t->uid, timeout_us(A1)));
}
static void hle_GetThreadId(RtCtx *c) { RET(k_cur ? k_cur->uid : 0); }
static void hle_ReferThreadStatus(RtCtx *c) {
    KThread *t = thread_arg(A0);
    if (!t) RET(SCE_ERR_UNKNOWN_THID);
    if (!A1) RET(0x800200D3u);
    uint32_t size = rt_r32(A1);
    uint8_t info[0x68];
    memset(info, 0, sizeof(info));
    uint32_t v[] = { t->attr, (uint32_t)t->status, t->entry, t->stack_addr, t->stack_size, t->gp, (uint32_t)t->init_prio,
                     (uint32_t)t->prio, (uint32_t)t->wait_type, (uint32_t)t->wait_id, 0, (uint32_t)t->exit_status };
    memcpy(info, &size, 4);
    memcpy(info + 4, t->name, 32);
    memcpy(info + 36, v, sizeof(v));
    memcpy(rt_ptr(A1), info, size < sizeof(info) ? size : sizeof(info));
    RET(0);
}
static void hle_ChangeThreadPriority(RtCtx *c) {
    KThread *t = thread_arg(A0);
    if (!t) RET(SCE_ERR_UNKNOWN_THID);
    int prio = A1 ? (int)A1 : k_cur->prio;
    if (prio < 1 || prio > 127) RET(SCE_ERR_ILLEGAL_PRIORITY);
    t->prio = prio;
    if (t->status == TS_READY) t->ready_seq = ++g_seq;
    c->r[2] = 0;
    k_check_preempt();
    RET(0);
}
static void hle_DelayThread(RtCtx *c) {
    uint64_t us = A0 < 200 ? 210 : A0;
    k_wait(WT_DELAY, 0, us);
    RET(0);
}
static void hle_SleepThread(RtCtx *c) { RET(k_wait(WT_SLEEP, 0, K_NO_TIMEOUT)); }
static void hle_SuspendDispatchThread(RtCtx *c) { int old = g_dispatch_enabled; g_dispatch_enabled = 0; RET(old); }
static void hle_ResumeDispatchThread(RtCtx *c) {
    g_dispatch_enabled = A0 != 0;
    c->r[2] = 0;
    k_check_preempt();
    RET(0);
}
static void hle_CheckCallback(RtCtx *c) { RET(0); }
static void hle_CreateCallback(RtCtx *c) { RET(k_obj_new(OBJ_CALLBACK, NULL)); }
static void hle_DeleteCallback(RtCtx *c) { k_obj_free((int)A0); RET(0); }
static void hle_GetSystemTimeWide(RtCtx *c) { k_now++; RET64(k_now); }
static void hle_GetSystemTimeLow(RtCtx *c) { k_now++; RET((uint32_t)k_now); }

/* --- semaphores ------------------------------------------------------------- */
typedef struct { char name[32]; int32_t count, max; uint32_t attr; int uid; } Sema;

static void hle_CreateSema(RtCtx *c) {
    Sema *s = (Sema *)calloc(1, sizeof(*s));
    if (A0) strncpy(s->name, (const char *)rt_ptr(A0), sizeof(s->name) - 1);
    s->attr = A1; s->count = (int32_t)A2; s->max = (int32_t)A3;
    s->uid = k_obj_new(OBJ_SEMA, s);
    RET(s->uid);
}
static void sema_release(Sema *s) {
    KThread *w[MAX_WAITERS];
    int n = k_waiters(WT_SEMA, s->uid, (s->attr & 0x100) != 0, w, MAX_WAITERS);
    for (int i = 0; i < n; i++) {
        if (s->count < (int32_t)w[i]->wait_val) break;  /* waiters are served in order */
        s->count -= (int32_t)w[i]->wait_val;
        k_wake(w[i], 0);
    }
}
static void hle_DeleteSema(RtCtx *c) {
    Sema *s = (Sema *)k_obj_get((int)A0, OBJ_SEMA);
    if (!s) RET(SCE_ERR_UNKNOWN_SEMID);
    KThread *w[MAX_WAITERS];
    int n = k_waiters(WT_SEMA, s->uid, 0, w, MAX_WAITERS);
    for (int i = 0; i < n; i++) k_wake(w[i], (int32_t)SCE_ERR_WAIT_DELETE);
    k_obj_free(s->uid);
    free(s);
    c->r[2] = 0;
    k_check_preempt();
    RET(0);
}
static void hle_SignalSema(RtCtx *c) {
    Sema *s = (Sema *)k_obj_get((int)A0, OBJ_SEMA);
    if (!s) RET(SCE_ERR_UNKNOWN_SEMID);
    int32_t n = (int32_t)A1;
    if (s->count + n > s->max) RET(SCE_ERR_SEMA_OVF);
    s->count += n;
    sema_release(s);
    c->r[2] = 0;
    k_check_preempt();
    RET(0);
}
static void hle_WaitSema(RtCtx *c) {
    Sema *s = (Sema *)k_obj_get((int)A0, OBJ_SEMA);
    if (!s) RET(SCE_ERR_UNKNOWN_SEMID);
    int32_t n = (int32_t)A1;
    KThread *w[1];
    if (s->count >= n && k_waiters(WT_SEMA, s->uid, 0, w, 1) == 0) { s->count -= n; RET(0); }
    k_cur->wait_val = (uint32_t)n;
    k_cur->timeout_ptr = A2;
    RET(k_wait(WT_SEMA, s->uid, timeout_us(A2)));
}
static void hle_PollSema(RtCtx *c) {
    Sema *s = (Sema *)k_obj_get((int)A0, OBJ_SEMA);
    if (!s) RET(SCE_ERR_UNKNOWN_SEMID);
    if (s->count >= (int32_t)A1) { s->count -= (int32_t)A1; RET(0); }
    RET(SCE_ERR_SEMA_ZERO);
}
static void hle_ReferSemaStatus(RtCtx *c) {
    Sema *s = (Sema *)k_obj_get((int)A0, OBJ_SEMA);
    if (!s) RET(SCE_ERR_UNKNOWN_SEMID);
    KThread *w[MAX_WAITERS];
    uint32_t v[] = { s->attr, 0, (uint32_t)s->count, (uint32_t)s->max, (uint32_t)k_waiters(WT_SEMA, s->uid, 0, w, MAX_WAITERS) };
    memcpy(rt_ptr(A1 + 4), s->name, 32);
    memcpy(rt_ptr(A1 + 36), v, sizeof(v));
    RET(0);
}

/* --- event flags ------------------------------------------------------------ */
typedef struct { char name[32]; uint32_t bits, init, attr; int uid; } Evf;

static int evf_match(uint32_t bits, uint32_t pattern, uint32_t mode) {
    return (mode & 1) ? (bits & pattern) != 0 : (bits & pattern) == pattern;
}
static void evf_consume(Evf *e, uint32_t pattern, uint32_t mode, uint32_t out_ptr) {
    if (out_ptr) rt_w32(out_ptr, e->bits);
    if (mode & 0x10) e->bits = 0;
    else if (mode & 0x20) e->bits &= ~pattern;
}
static void hle_CreateEventFlag(RtCtx *c) {
    Evf *e = (Evf *)calloc(1, sizeof(*e));
    if (A0) strncpy(e->name, (const char *)rt_ptr(A0), sizeof(e->name) - 1);
    e->attr = A1; e->bits = e->init = A2;
    e->uid = k_obj_new(OBJ_EVF, e);
    RET(e->uid);
}
static void hle_DeleteEventFlag(RtCtx *c) {
    Evf *e = (Evf *)k_obj_get((int)A0, OBJ_EVF);
    if (!e) RET(SCE_ERR_UNKNOWN_EVFID);
    KThread *w[MAX_WAITERS];
    int n = k_waiters(WT_EVF, e->uid, 0, w, MAX_WAITERS);
    for (int i = 0; i < n; i++) k_wake(w[i], (int32_t)SCE_ERR_WAIT_DELETE);
    k_obj_free(e->uid);
    free(e);
    c->r[2] = 0;
    k_check_preempt();
    RET(0);
}
static void hle_SetEventFlag(RtCtx *c) {
    Evf *e = (Evf *)k_obj_get((int)A0, OBJ_EVF);
    if (!e) RET(SCE_ERR_UNKNOWN_EVFID);
    e->bits |= A1;
    KThread *w[MAX_WAITERS];
    int n = k_waiters(WT_EVF, e->uid, (e->attr & 0x100) != 0, w, MAX_WAITERS);
    for (int i = 0; i < n; i++)
        if (evf_match(e->bits, w[i]->wait_val, w[i]->wait_val2)) {
            evf_consume(e, w[i]->wait_val, w[i]->wait_val2, w[i]->wait_ptr);
            k_wake(w[i], 0);
        }
    c->r[2] = 0;
    k_check_preempt();
    RET(0);
}
static void hle_ClearEventFlag(RtCtx *c) {
    Evf *e = (Evf *)k_obj_get((int)A0, OBJ_EVF);
    if (!e) RET(SCE_ERR_UNKNOWN_EVFID);
    e->bits &= A1;
    RET(0);
}
static void hle_WaitEventFlag(RtCtx *c) {
    Evf *e = (Evf *)k_obj_get((int)A0, OBJ_EVF);
    if (!e) RET(SCE_ERR_UNKNOWN_EVFID);
    if (evf_match(e->bits, A1, A2)) { evf_consume(e, A1, A2, A3); RET(0); }
    KThread *w[1];
    if (!(e->attr & 0x200) && k_waiters(WT_EVF, e->uid, 0, w, 1) > 0) RET(SCE_ERR_EVF_MULTI);
    k_cur->wait_val = A1;
    k_cur->wait_val2 = A2;
    k_cur->wait_ptr = A3;
    k_cur->timeout_ptr = T0;
    int32_t r = k_wait(WT_EVF, e->uid, timeout_us(T0));
    if (r != 0 && A3) {
        Evf *still = (Evf *)k_obj_get((int)A0, OBJ_EVF);
        if (still) rt_w32(A3, still->bits);
    }
    RET(r);
}
static void hle_PollEventFlag(RtCtx *c) {
    Evf *e = (Evf *)k_obj_get((int)A0, OBJ_EVF);
    if (!e) RET(SCE_ERR_UNKNOWN_EVFID);
    if (evf_match(e->bits, A1, A2)) { evf_consume(e, A1, A2, A3); RET(0); }
    if (A3) rt_w32(A3, e->bits);
    RET(SCE_ERR_EVF_COND);
}
static void hle_ReferEventFlagStatus(RtCtx *c) {
    Evf *e = (Evf *)k_obj_get((int)A0, OBJ_EVF);
    if (!e) RET(SCE_ERR_UNKNOWN_EVFID);
    KThread *w[MAX_WAITERS];
    uint32_t v[] = { e->attr, e->init, e->bits, (uint32_t)k_waiters(WT_EVF, e->uid, 0, w, MAX_WAITERS) };
    memcpy(rt_ptr(A1 + 4), e->name, 32);
    memcpy(rt_ptr(A1 + 36), v, sizeof(v));
    RET(0);
}

/* --- fixed-size pools (FPL) ------------------------------------------------- */
typedef struct { char name[32]; uint32_t base, block, count, attr; uint8_t *used; int uid; } Fpl;

static void hle_CreateFpl(RtCtx *c) {
    Fpl *f = (Fpl *)calloc(1, sizeof(*f));
    if (A0) strncpy(f->name, (const char *)rt_ptr(A0), sizeof(f->name) - 1);
    f->attr = A2;
    f->block = (A3 + 3) & ~3u;
    f->count = T0;
    f->base = k_alloc(f->block * f->count, (f->attr & 0x4000) != 0, 0x100);
    if (!f->base || !f->count) { free(f); RET(SCE_ERR_NO_MEMORY); }
    f->used = (uint8_t *)calloc(f->count, 1);
    f->uid = k_obj_new(OBJ_FPL, f);
    rt_log("[mem] FPL '%s': %u x %X bytes at %08X\n", f->name, f->count, f->block, f->base);
    RET(f->uid);
}
static int fpl_take(Fpl *f, uint32_t out_ptr) {
    for (uint32_t i = 0; i < f->count; i++)
        if (!f->used[i]) { f->used[i] = 1; if (out_ptr) rt_w32(out_ptr, f->base + i * f->block); return 1; }
    return 0;
}
static void hle_AllocateFpl(RtCtx *c) {
    Fpl *f = (Fpl *)k_obj_get((int)A0, OBJ_FPL);
    if (!f) RET(SCE_ERR_UNKNOWN_FPLID);
    if (fpl_take(f, A1)) RET(0);
    k_cur->wait_ptr = A1;
    k_cur->timeout_ptr = A2;
    RET(k_wait(WT_FPL, f->uid, timeout_us(A2)));
}
static void hle_TryAllocateFpl(RtCtx *c) {
    Fpl *f = (Fpl *)k_obj_get((int)A0, OBJ_FPL);
    if (!f) RET(SCE_ERR_UNKNOWN_FPLID);
    RET(fpl_take(f, A1) ? 0 : SCE_ERR_NO_MEMORY);
}
static void hle_FreeFpl(RtCtx *c) {
    Fpl *f = (Fpl *)k_obj_get((int)A0, OBJ_FPL);
    if (!f) RET(SCE_ERR_UNKNOWN_FPLID);
    if (A1 < f->base || (A1 - f->base) % f->block || (A1 - f->base) / f->block >= f->count) RET(0x800200D3u);
    f->used[(A1 - f->base) / f->block] = 0;
    KThread *w[1];
    if (k_waiters(WT_FPL, f->uid, (f->attr & 0x100) != 0, w, 1) && fpl_take(f, w[0]->wait_ptr)) k_wake(w[0], 0);
    c->r[2] = 0;
    k_check_preempt();
    RET(0);
}
static void hle_DeleteFpl(RtCtx *c) {
    Fpl *f = (Fpl *)k_obj_get((int)A0, OBJ_FPL);
    if (!f) RET(SCE_ERR_UNKNOWN_FPLID);
    KThread *w[MAX_WAITERS];
    int n = k_waiters(WT_FPL, f->uid, 0, w, MAX_WAITERS);
    for (int i = 0; i < n; i++) k_wake(w[i], (int32_t)SCE_ERR_WAIT_DELETE);
    k_free(f->base);
    k_obj_free(f->uid);
    free(f->used);
    free(f);
    RET(0);
}

/* --- SysMemUserForUser ------------------------------------------------------ */
typedef struct { uint32_t addr, size; char name[32]; } MemBlockObj;

static void hle_AllocPartitionMemory(RtCtx *c) {
    uint32_t type = A2, size = A3, arg = T0, addr = 0;
    if (type == 2) {  /* at a given address: approximate with a low allocation */
        addr = k_alloc(size, 0, 0x100);
    } else {
        uint32_t align = (type == 3 || type == 4) && arg ? arg : 0x100;
        addr = k_alloc(size, type == 1 || type == 4, align);
    }
    if (!addr) RET(0x800200D9u);
    MemBlockObj *b = (MemBlockObj *)calloc(1, sizeof(*b));
    b->addr = addr; b->size = size;
    if (A1) strncpy(b->name, (const char *)rt_ptr(A1), sizeof(b->name) - 1);
    int uid = k_obj_new(OBJ_BLOCK, b);
    rt_log("[mem] partition block '%s' %X bytes at %08X (uid %d)\n", b->name, size, addr, uid);
    RET(uid);
}
static void hle_FreePartitionMemory(RtCtx *c) {
    MemBlockObj *b = (MemBlockObj *)k_obj_get((int)A0, OBJ_BLOCK);
    if (!b) RET(SCE_ERR_UNKNOWN_UID);
    k_free(b->addr);
    k_obj_free((int)A0);
    free(b);
    RET(0);
}
static void hle_GetBlockHeadAddr(RtCtx *c) {
    MemBlockObj *b = (MemBlockObj *)k_obj_get((int)A0, OBJ_BLOCK);
    RET(b ? b->addr : SCE_ERR_UNKNOWN_UID);
}
static void hle_MaxFreeMemSize(RtCtx *c) { RET(k_max_free()); }
static void hle_TotalFreeMemSize(RtCtx *c) { RET(k_total_free()); }
static void hle_Zero(RtCtx *c) { RET(0); }
static void hle_One(RtCtx *c) { RET(1); }
static void hle_Printf(RtCtx *c) { rt_log("[guest printf] %s\n", A0 ? (const char *)rt_ptr(A0) : ""); RET(0); }

/* --- Kernel_Library / UtilsForUser ------------------------------------------ */
static void hle_Memset(RtCtx *c) { memset(rt_ptr(A0), (int)(A1 & 0xFF), A2); RET(A0); }
static void hle_Memcpy(RtCtx *c) { memmove(rt_ptr(A0), rt_ptr(A1), A2); RET(A0); }
static void hle_LibcClock(RtCtx *c) { RET((uint32_t)k_now); }
static void hle_LibcTime(RtCtx *c) { uint32_t t = (uint32_t)time(NULL); if (A0) rt_w32(A0, t); RET(t); }
static void hle_LibcGettimeofday(RtCtx *c) {
    uint64_t us = (uint64_t)time(NULL) * 1000000u + k_now % 1000000u;
    if (A0) { rt_w32(A0, (uint32_t)(us / 1000000u)); rt_w32(A0 + 4, (uint32_t)(us % 1000000u)); }
    RET(0);
}

const HleEntry rt_hle_kernel[] = {
    {"sceKernelCreateThread", hle_CreateThread}, {"sceKernelStartThread", hle_StartThread},
    {"sceKernelExitThread", hle_ExitThread}, {"sceKernelExitDeleteThread", hle_ExitDeleteThread},
    {"sceKernelTerminateThread", hle_TerminateThread}, {"sceKernelTerminateDeleteThread", hle_TerminateDeleteThread},
    {"sceKernelDeleteThread", hle_DeleteThread}, {"sceKernelWaitThreadEnd", hle_WaitThreadEnd},
    {"sceKernelWaitThreadEndCB", hle_WaitThreadEnd},
    {"sceKernelGetThreadId", hle_GetThreadId}, {"sceKernelReferThreadStatus", hle_ReferThreadStatus},
    {"sceKernelChangeThreadPriority", hle_ChangeThreadPriority},
    {"sceKernelDelayThread", hle_DelayThread}, {"sceKernelDelayThreadCB", hle_DelayThread},
    {"sceKernelSleepThread", hle_SleepThread}, {"sceKernelSleepThreadCB", hle_SleepThread},
    {"sceKernelSuspendDispatchThread", hle_SuspendDispatchThread},
    {"sceKernelResumeDispatchThread", hle_ResumeDispatchThread},
    {"sceKernelCheckCallback", hle_CheckCallback}, {"sceKernelCreateCallback", hle_CreateCallback},
    {"sceKernelDeleteCallback", hle_DeleteCallback},
    {"sceKernelGetSystemTimeWide", hle_GetSystemTimeWide}, {"sceKernelGetSystemTimeLow", hle_GetSystemTimeLow},
    {"sceKernelCreateSema", hle_CreateSema}, {"sceKernelDeleteSema", hle_DeleteSema},
    {"sceKernelSignalSema", hle_SignalSema}, {"sceKernelWaitSema", hle_WaitSema},
    {"sceKernelWaitSemaCB", hle_WaitSema}, {"sceKernelPollSema", hle_PollSema},
    {"sceKernelReferSemaStatus", hle_ReferSemaStatus},
    {"sceKernelCreateEventFlag", hle_CreateEventFlag}, {"sceKernelDeleteEventFlag", hle_DeleteEventFlag},
    {"sceKernelSetEventFlag", hle_SetEventFlag}, {"sceKernelClearEventFlag", hle_ClearEventFlag},
    {"sceKernelWaitEventFlag", hle_WaitEventFlag}, {"sceKernelWaitEventFlagCB", hle_WaitEventFlag},
    {"sceKernelPollEventFlag", hle_PollEventFlag}, {"sceKernelReferEventFlagStatus", hle_ReferEventFlagStatus},
    {"sceKernelCreateFpl", hle_CreateFpl}, {"sceKernelDeleteFpl", hle_DeleteFpl},
    {"sceKernelAllocateFpl", hle_AllocateFpl}, {"sceKernelAllocateFplCB", hle_AllocateFpl},
    {"sceKernelTryAllocateFpl", hle_TryAllocateFpl}, {"sceKernelFreeFpl", hle_FreeFpl},
    {"sceKernelAllocPartitionMemory", hle_AllocPartitionMemory}, {"sceKernelFreePartitionMemory", hle_FreePartitionMemory},
    {"sceKernelGetBlockHeadAddr", hle_GetBlockHeadAddr}, {"sceKernelMaxFreeMemSize", hle_MaxFreeMemSize},
    {"sceKernelTotalFreeMemSize", hle_TotalFreeMemSize},
    {"sceKernelSetCompiledSdkVersion370", hle_Zero}, {"sceKernelSetCompiledSdkVersion603_605", hle_Zero},
    {"sceKernelSetCompilerVersion", hle_Zero}, {"sceKernelPrintf", hle_Printf},
    {"sceKernelCpuSuspendIntr", hle_One}, {"sceKernelCpuResumeIntr", hle_Zero}, {"sceKernelIsCpuIntrEnable", hle_One},
    {"sceKernelMemset", hle_Memset}, {"sceKernelMemcpy", hle_Memcpy},
    {"sceKernelDcacheWritebackAll", hle_Zero}, {"sceKernelDcacheWritebackInvalidateAll", hle_Zero},
    {"sceKernelLibcClock", hle_LibcClock}, {"sceKernelLibcTime", hle_LibcTime},
    {"sceKernelLibcGettimeofday", hle_LibcGettimeofday},
    {NULL, NULL},
};
