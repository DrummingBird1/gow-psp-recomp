/* Minimal HLE (PSP OS) layer for the prototype.
 * Implements just enough of the kernel to follow the boot path and logs every call.
 * Threads are run synchronously when started; this is a stand-in until a real
 * fiber-based scheduler exists (Phase 2). */
#include <stdio.h>
#include <string.h>

#include "rt_internal.h"

#define USER_END 0x09F00000u  /* user RAM ends at 0x0A000000; the top 1MB holds the boot stack */
#define MAX_UIDS 256

typedef struct { int kind; uint32_t addr, size, entry, stack; char name[32]; } Uid;

static RtHleMode g_mode;
static uint32_t g_heap_lo, g_heap_hi;
static uint32_t g_calls, g_max_calls;
static Uid g_uids[MAX_UIDS];
static int g_next_uid = 1;
static uint32_t g_current_thread;
static char g_unimplemented[64][48];
static int g_unimplemented_count;

enum { UID_BLOCK = 1, UID_THREAD, UID_SEMA, UID_EVENT, UID_CALLBACK, UID_FPL };

void rt_hle_init(RtHleMode mode, uint32_t heap_start, uint32_t max_calls) {
    g_mode = mode;
    g_heap_lo = (heap_start + 0xFFF) & ~0xFFFu;
    g_heap_hi = USER_END;
    g_max_calls = max_calls;
}

static uint32_t new_uid(int kind, const char *name) {
    if (g_next_uid >= MAX_UIDS) return 0x80020001u;
    Uid *u = &g_uids[g_next_uid];
    memset(u, 0, sizeof(*u));
    u->kind = kind;
    if (name) strncpy(u->name, name, sizeof(u->name) - 1);
    return (uint32_t)g_next_uid++;
}

static Uid *get_uid(uint32_t id, int kind) {
    return (id > 0 && id < (uint32_t)g_next_uid && g_uids[id].kind == kind) ? &g_uids[id] : NULL;
}

static uint32_t alloc(uint32_t size, int high, uint32_t align) {
    size = (size + 0xFF) & ~0xFFu;
    if (align < 0x100) align = 0x100;
    if (high) {
        uint32_t a = (g_heap_hi - size) & ~(align - 1);
        if (a < g_heap_lo) return 0;
        g_heap_hi = a;
        return a;
    }
    uint32_t a = (g_heap_lo + align - 1) & ~(align - 1);
    if (a + size > g_heap_hi) return 0;
    g_heap_lo = a + size;
    return a;
}

static const char *guest_str(uint32_t a) { return a ? (const char *)rt_ptr(a) : "(null)"; }

static void note_unimplemented(const char *name) {
    for (int i = 0; i < g_unimplemented_count; i++)
        if (!strcmp(g_unimplemented[i], name)) return;
    if (g_unimplemented_count < 64) strncpy(g_unimplemented[g_unimplemented_count++], name, 47);
}

static uint32_t start_thread(Uid *t, uint32_t argsize, uint32_t argp) {
    RtCtx tc;
    memset(&tc, 0, sizeof(tc));
    uint32_t sp = t->stack + t->size - 0x40;
    if (argsize && argp) {
        sp = (sp - argsize) & ~0xFu;
        memmove(rt_ptr(sp), rt_ptr(argp), argsize);
        tc.r[5] = sp;
    }
    tc.r[4] = argsize;
    tc.r[28] = RT_GP;
    tc.r[29] = sp - 0x10;
    tc.r[31] = 0;
    uint32_t prev = g_current_thread;
    g_current_thread = (uint32_t)(t - g_uids);
    printf("[hle] -> running thread '%s' entry %08X synchronously\n", t->name, t->entry);
    rt_call(&tc, t->entry);
    printf("[hle] <- thread '%s' returned v0=%08X\n", t->name, tc.r[2]);
    g_current_thread = prev;
    return 0;
}

void rt_hle(RtCtx *c, uint32_t nid, const char *lib, const char *name) {
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7], t0 = c->r[8];
    uint32_t v0 = 0;
    int known = 1;
    if (g_mode == RT_HLE_ABORT) rt_abort(RT_ABORT_HLE, nid, c->r[31], name);
    if (++g_calls > g_max_calls) rt_abort(RT_ABORT_BUDGET, nid, g_calls, "HLE call budget exhausted");

    if (!strncmp(name, "sceKernelSetCompiledSdkVersion", 30) || !strcmp(name, "sceKernelSetCompilerVersion")) {
        v0 = 0;
    } else if (!strcmp(name, "sceKernelAllocPartitionMemory")) {
        uint32_t addr = alloc(a3, a2 == 1, 0x100);
        if (!addr) v0 = 0x800200D9u; /* SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED */
        else { v0 = new_uid(UID_BLOCK, guest_str(a1)); g_uids[v0].addr = addr; g_uids[v0].size = a3; }
    } else if (!strcmp(name, "sceKernelGetBlockHeadAddr")) {
        Uid *u = get_uid(a0, UID_BLOCK); v0 = u ? u->addr : 0;
    } else if (!strcmp(name, "sceKernelFreePartitionMemory")) {
        v0 = 0;
    } else if (!strcmp(name, "sceKernelMaxFreeMemSize")) {
        v0 = g_heap_hi - g_heap_lo;
    } else if (!strcmp(name, "sceKernelCreateThread")) {
        uint32_t stack = alloc(a3 ? a3 : 0x4000, 1, 0x100);
        v0 = new_uid(UID_THREAD, guest_str(a0));
        g_uids[v0].entry = a1; g_uids[v0].stack = stack; g_uids[v0].size = a3;
    } else if (!strcmp(name, "sceKernelStartThread")) {
        Uid *t = get_uid(a0, UID_THREAD);
        v0 = t ? start_thread(t, a1, a2) : 0x80020198u;
    } else if (!strcmp(name, "sceKernelGetThreadId")) {
        v0 = g_current_thread;
    } else if (!strcmp(name, "sceKernelCreateSema")) {
        v0 = new_uid(UID_SEMA, guest_str(a0));
    } else if (!strcmp(name, "sceKernelCreateEventFlag")) {
        v0 = new_uid(UID_EVENT, guest_str(a0));
    } else if (!strcmp(name, "sceKernelCreateCallback")) {
        v0 = new_uid(UID_CALLBACK, guest_str(a0));
    } else if (!strcmp(name, "sceKernelCreateFpl")) {
        v0 = new_uid(UID_FPL, guest_str(a0));
    } else if (!strcmp(name, "sceKernelStdin") || !strcmp(name, "sceKernelStdout") || !strcmp(name, "sceKernelStderr")) {
        v0 = !strcmp(name, "sceKernelStdin") ? 0 : !strcmp(name, "sceKernelStdout") ? 1 : 2;
    } else if (!strcmp(name, "sceKernelPrintf")) {
        printf("[guest printf] %s\n", guest_str(a0));
    } else if (!strcmp(name, "sceKernelRegisterExitCallback") || !strcmp(name, "sceKernelDelayThread")
               || !strcmp(name, "sceKernelSignalSema") || !strcmp(name, "sceKernelWaitSema")
               || !strcmp(name, "sceKernelChangeThreadPriority") || !strcmp(name, "scePowerSetClockFrequency350")) {
        v0 = 0;
    } else if (!strcmp(name, "sceKernelAllocateFpl") || !strcmp(name, "sceKernelTryAllocateFpl")) {
        /* Prototype: every allocation is a fresh block; FPL block size is not tracked */
        uint32_t addr = alloc(0x1000, 0, 0x40);
        if (a1) rt_w32(a1, addr);
        v0 = addr ? 0 : 0x80020190u;
    } else if (!strcmp(name, "sceKernelGetSystemTimeWide")) {
        static uint64_t fake_us;
        fake_us += 1000;
        c->r[2] = (uint32_t)fake_us;
        c->r[3] = (uint32_t)(fake_us >> 32);
        printf("[hle %4u] %s::%s() -> %llu us\n", g_calls, lib, name, (unsigned long long)fake_us);
        return;
    } else if (!strcmp(name, "sceKernelGetSystemTimeLow")) {
        static uint32_t fake_low;
        v0 = fake_low += 1000;
    } else if (!strcmp(name, "sceUmdCheckMedium")) {
        v0 = 1; /* disc present */
    } else if (!strcmp(name, "sceUmdActivate") || !strcmp(name, "sceUmdWaitDriveStatWithTimer")
               || !strcmp(name, "sceKernelPowerLock") || !strcmp(name, "sceKernelPowerUnlock")
               || !strcmp(name, "scePowerRegisterCallback") || !strcmp(name, "scePowerUnregisterCallback")
               || !strcmp(name, "sceKernelDeleteCallback") || !strcmp(name, "sceKernelSleepThreadCB")) {
        v0 = 0;
    } else if (!strcmp(name, "sceIoOpen") || !strcmp(name, "sceIoOpenAsync") || !strcmp(name, "sceIoDopen")
               || !strcmp(name, "sceIoGetstat") || !strcmp(name, "sceIoChdir")) {
        /* No file system yet (Phase 2): log the path and report "not found" */
        printf("[hle %4u] %s::%s(\"%s\") -> not found (no file system in the prototype)\n", g_calls, lib, name, guest_str(a0));
        c->r[2] = 0x80010002u;
        c->r[3] = 0;
        return;
    } else if (!strcmp(name, "sceKernelExitThread") || !strcmp(name, "sceKernelExitDeleteThread")
               || !strcmp(name, "sceKernelExitGame")) {
        rt_abort(RT_ABORT_HLE, nid, c->r[31], name);
    } else {
        known = 0;
        note_unimplemented(name);
        v0 = 0;
    }
    printf("[hle %4u] %s::%s(%08X, %08X, %08X, %08X, %08X) -> %08X%s\n", g_calls, lib, name, a0, a1, a2, a3, t0, v0,
           known ? "" : "  [unimplemented]");
    c->r[2] = v0;
    c->r[3] = 0;
}

void rt_hle_report(void) {
    printf("[hle] %u calls; %d unimplemented functions called:", g_calls, g_unimplemented_count);
    for (int i = 0; i < g_unimplemented_count; i++) printf(" %s", g_unimplemented[i]);
    printf("\n");
}
