/* HLE dispatcher plus the smaller PSP modules: display, GE, controller, power, RTC, impose,
 * message dialog, modules, NpDrm. Audio lives in rt_audio.c, rt_sas.c and rt_atrac.c, save
 * data in rt_savedata.c.
 * Unimplemented functions return 0 and are logged the first time they are called. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "rt_frames.h"
#include "rt_ge.h"
#include "rt_host.h"
#include "rt_kernel.h"

int rt_hle_verbose;
static RtHleMode g_mode;
static uint64_t g_calls;
static uint64_t g_max_calls;

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (k_cur) printf("[%9.3f %-12.12s] ", (double)k_now / 1e6, k_cur->name);
    else printf("[%9.3f %-12s] ", (double)k_now / 1e6, "-");
    vprintf(fmt, ap);
    va_end(ap);
}

/* --- dispatcher ------------------------------------------------------------- */
typedef struct { uint32_t nid; int resolved; HleFn fn; const char *lib, *name; uint64_t count; } Slot;
static Slot g_slots[2048];

static const HleEntry *const g_tables[] = { rt_hle_kernel, rt_hle_io, rt_hle_misc, rt_hle_mpeg, rt_hle_audio, rt_hle_sas, rt_hle_atrac,
                                             rt_hle_savedata };

static Slot *slot_for(uint32_t nid, const char *lib, const char *name) {
    uint32_t h = (nid * 2654435761u) & 2047;
    while (g_slots[h].resolved && g_slots[h].nid != nid) h = (h + 1) & 2047;
    Slot *s = &g_slots[h];
    if (!s->resolved) {
        s->nid = nid; s->lib = lib; s->name = name; s->resolved = 1;
        for (size_t t = 0; t < sizeof(g_tables) / sizeof(g_tables[0]) && !s->fn; t++)
            for (const HleEntry *e = g_tables[t]; e->name; e++)
                if (!strcmp(e->name, name)) { s->fn = e->fn; break; }
    }
    return s;
}

void rt_hle_init(RtHleMode mode, uint32_t heap_start, uint32_t max_calls) {
    g_mode = mode;
    g_max_calls = max_calls;
    if (heap_start) k_mem_init(heap_start, 0x0A000000u);
}

void rt_hle(RtCtx *c, uint32_t nid, const char *lib, const char *name) {
    if (g_mode == RT_HLE_ABORT) rt_abort(RT_ABORT_HLE, nid, c->r[31], name);
    if (++g_calls > g_max_calls && g_max_calls) rt_abort(RT_ABORT_BUDGET, nid, (uint32_t)g_calls, "HLE call budget exhausted");
    Slot *s = slot_for(nid, lib, name);
    s->count++;
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
    if (s->fn) {
        s->fn(c);
        if (rt_hle_verbose)
            rt_log("[hle] %s::%s(%08X, %08X, %08X, %08X) -> %08X\n", lib, name, a0, a1, a2, a3, c->r[2]);
    } else {
        c->r[2] = 0;
        c->r[3] = 0;
        if (s->count == 1 || rt_hle_verbose)
            rt_log("[hle] UNIMPLEMENTED %s::%s(%08X, %08X, %08X, %08X) -> 0%s\n", lib, name, a0, a1, a2, a3,
                   rt_hle_verbose ? "" : "  (logged once)");
    }
}

void rt_hle_report(void) {
    Slot *top[2048];
    int n = 0, unimpl = 0;
    for (int i = 0; i < 2048; i++) if (g_slots[i].resolved) top[n++] = &g_slots[i];
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && top[j]->count > top[j - 1]->count; j--) { Slot *t = top[j]; top[j] = top[j - 1]; top[j - 1] = t; }
    printf("[hle] %llu calls to %d distinct functions. Most called:\n", (unsigned long long)g_calls, n);
    for (int i = 0; i < n && i < 15; i++)
        printf("  %8llu  %s::%s%s\n", (unsigned long long)top[i]->count, top[i]->lib, top[i]->name, top[i]->fn ? "" : "  [unimplemented]");
    printf("[hle] unimplemented functions that were called:");
    for (int i = 0; i < n; i++) if (!top[i]->fn) { printf(" %s", top[i]->name); unimpl++; }
    printf("%s\n", unimpl ? "" : " none");
}

/* --- display ---------------------------------------------------------------- */
#define VBLANK_US 16683u  /* 59.94 Hz */
static uint64_t g_frames;
static uint32_t g_fb_addr, g_fb_width, g_fb_format;

static void hle_DisplaySetMode(RtCtx *c) { RET(0); }
/* God of War flips faster than 60 Hz and relies on the real GE's rendering time to pace
 * itself; like PPSSPP's ForceMax60FPS compat flag (set for these games), a flip to a new
 * framebuffer that comes early delays the calling thread until the next frame slot. */
static uint64_t g_next_flip;
static void hle_DisplaySetFrameBuf(RtCtx *c) {
    uint32_t prev = g_fb_addr;
    g_frames++;
    g_fb_addr = A0; g_fb_width = A1; g_fb_format = A2;
    if (g_frames == 1 || g_frames % 600 == 0)
        rt_log("[display] frame %llu: framebuffer %08X width %u format %u\n", (unsigned long long)g_frames, A0, A1, A2);
    rt_ge_skip_draw = g_frames + 1 < rt_fast_forward;  /* the next frame's drawing */
    if (rt_fast_forward && g_frames + 1 == rt_fast_forward) rt_log("[display] fast-forward over: drawing from frame %u\n", rt_fast_forward);
    rt_frames_present(g_frames, k_now, A0, A1, A2);
    int closed = rt_host_present(g_frames, A0, A1, A2);
    rt_frames_after_present(g_frames, A0, A1, A2);
    ge_memory_changed();  /* the CPU may change textures before the next frame */
    if (closed) k_stop(RT_ABORT_HLE, 0, (uint32_t)g_frames, "window closed");
    if (rt_max_frames && g_frames >= rt_max_frames) k_stop(RT_ABORT_BUDGET, 0, (uint32_t)g_frames, "frame limit reached");
    if (A0 && prev && A0 != prev) {
        uint64_t now = k_now, wait = g_next_flip > now ? g_next_flip - now : 0;
        g_next_flip = (g_next_flip > now ? g_next_flip : now) + VBLANK_US - 10;
        if (wait > 1000) k_wait(WT_VBLANK, 0, wait);
    }
    RET(0);
}
static void hle_DisplayWaitVblankStart(RtCtx *c) {
    uint64_t next = (k_now / VBLANK_US + 1) * VBLANK_US;
    k_wait(WT_VBLANK, 0, next - k_now);
    RET(0);
}
static void hle_DisplayGetVcount(RtCtx *c) { RET((uint32_t)(k_now / VBLANK_US)); }
static void hle_DisplayGetFramePerSec(RtCtx *c) { c->fpr[0].f = 59.9400599f; RET(0); }

/* --- GE (graphics engine): lists are accepted and counted, nothing is drawn yet -- */
static uint32_t g_ge_lists;
static void hle_GeEdramGetAddr(RtCtx *c) { RET(0x04000000u); }
static void hle_GeEdramGetSize(RtCtx *c) { RET(0x00200000u); }
static void hle_GeEdramSetAddrTranslation(RtCtx *c) { RET(0x400u); }
static void hle_GeListEnQueue(RtCtx *c) {
    g_ge_lists++;
    rt_ge_run(A0, A1);
    if (g_ge_lists <= 3 || (g_ge_lists & (g_ge_lists - 1)) == 0)
        rt_log("[ge] display list #%u enqueued: list %08X stall %08X cb %d\n", g_ge_lists, A0, A1, (int)A2);
    RET(g_ge_lists);
}
static void hle_GeListSync(RtCtx *c) { RET(0); }  /* PSP_GE_LIST_COMPLETED */

/* --- controller --------------------------------------------------------------- */
/* SceCtrlData: TimeStamp, Buttons, Lx, Ly, 6 reserved bytes. Every entry gets the current
 * state (keyboard/gamepad in the window, or the --input script by displayed frame). */
static void hle_CtrlPeekBufferPositive(RtCtx *c) {
    uint32_t n = A1 ? A1 : 1;
    uint8_t lx, ly;
    uint32_t buttons = rt_host_buttons(g_frames, k_now, &lx, &ly);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = A0 + i * 16;
        rt_w32(p, (uint32_t)k_now);
        rt_w32(p + 4, buttons);
        rt_w8(p + 8, lx); rt_w8(p + 9, ly);
        memset(rt_ptr(p + 10), 0, 6);
    }
    RET(n);
}

/* --- power / volatile memory ----------------------------------------------------- */
static void hle_VolatileMemLock(RtCtx *c) {
    if (A1) rt_w32(A1, 0x08400000u);
    if (A2) rt_w32(A2, 0x00400000u);
    RET(0);
}

/* --- RTC / impose ------------------------------------------------------------- */
/* The guest's wall clock moves with virtual time from a fixed start, so a replay of an
 * input recording sees the same dates as the session it was recorded in. */
static int64_t g_epoch = -1;
void rt_wall_set_epoch(int64_t seconds) { g_epoch = seconds; }
int64_t rt_wall_epoch(void) {
    if (g_epoch < 0) g_epoch = (int64_t)time(NULL);
    return g_epoch;
}
uint64_t rt_wall_time_us(void) { return (uint64_t)rt_wall_epoch() * 1000000u + k_now; }

static void hle_RtcGetCurrentClockLocalTime(RtCtx *c) {
    uint64_t us = rt_wall_time_us();
    time_t now = (time_t)(us / 1000000u);
    struct tm *tm = localtime(&now);
    uint16_t v[6] = { (uint16_t)(tm->tm_year + 1900), (uint16_t)(tm->tm_mon + 1), (uint16_t)tm->tm_mday,
                      (uint16_t)tm->tm_hour, (uint16_t)tm->tm_min, (uint16_t)tm->tm_sec };
    memcpy(rt_ptr(A0), v, sizeof(v));
    rt_w32(A0 + 12, (uint32_t)(us % 1000000u));
    RET(0);
}
static void hle_ImposeGetLanguageMode(RtCtx *c) {
    if (A0) rt_w32(A0, 1);  /* English */
    if (A1) rt_w32(A1, 1);  /* cross confirms */
    RET(0);
}

/* --- message dialog: completes at once with OK (save data: rt_savedata.c) ----------- */
static int g_msg_status;
static int dialog_status(int *st) {
    int s = *st;
    if (s == 1) *st = 2;
    else if (s == 4) *st = 0;
    return s;
}
static void hle_MsgDialogInitStart(RtCtx *c) { g_msg_status = 1; rt_log("[utility] message dialog\n"); RET(0); }
static void hle_MsgDialogGetStatus(RtCtx *c) { RET(dialog_status(&g_msg_status)); }
static void hle_MsgDialogUpdate(RtCtx *c) { if (g_msg_status == 2) g_msg_status = 3; RET(0); }
static void hle_MsgDialogShutdownStart(RtCtx *c) { g_msg_status = 4; RET(0); }
/* --- modules / process ------------------------------------------------------------ */
static void hle_LoadModule(RtCtx *c) {
    rt_log("[module] load \"%s\" (system module: emulated by HLE)\n", A0 ? (const char *)rt_ptr(A0) : "");
    RET(k_obj_new(OBJ_MODULE, NULL));
}
static void hle_GetModuleId(RtCtx *c) { RET(1); }
static void hle_ExitGame(RtCtx *c) { k_stop(RT_ABORT_HLE, 0, 0, "game called sceKernelExitGame"); }
static void hle_StopUnloadSelf(RtCtx *c) { k_stop(RT_ABORT_HLE, 0, 0, "module unloaded itself"); }
static void hle_Zero(RtCtx *c) { RET(0); }
static void hle_Stdin(RtCtx *c) { RET(3); }
static void hle_Stdout(RtCtx *c) { RET(1); }
static void hle_Stderr(RtCtx *c) { RET(2); }

const HleEntry rt_hle_misc[] = {
    {"sceDisplaySetMode", hle_DisplaySetMode}, {"sceDisplaySetFrameBuf", hle_DisplaySetFrameBuf},
    {"sceDisplayWaitVblankStart", hle_DisplayWaitVblankStart}, {"sceDisplayWaitVblankStartCB", hle_DisplayWaitVblankStart},
    {"sceDisplayGetVcount", hle_DisplayGetVcount}, {"sceDisplayGetFramePerSec", hle_DisplayGetFramePerSec},
    {"sceGeEdramGetAddr", hle_GeEdramGetAddr}, {"sceGeEdramGetSize", hle_GeEdramGetSize},
    {"sceGeEdramSetAddrTranslation", hle_GeEdramSetAddrTranslation}, {"sceGeListEnQueue", hle_GeListEnQueue},
    {"sceGeListSync", hle_GeListSync}, {"sceGeContinue", hle_Zero},
    {"sceCtrlSetSamplingCycle", hle_Zero}, {"sceCtrlSetSamplingMode", hle_Zero},
    {"sceCtrlSetIdleCancelThreshold", hle_Zero}, {"sceCtrlPeekBufferPositive", hle_CtrlPeekBufferPositive},
    {"scePowerSetClockFrequency350", hle_Zero}, {"scePower_469989ad", hle_Zero},
    {"scePowerRegisterCallback", hle_Zero}, {"scePowerUnregisterCallback", hle_Zero},
    {"sceKernelPowerLock", hle_Zero}, {"sceKernelPowerUnlock", hle_Zero}, {"sceKernelPowerTick", hle_Zero},
    {"sceKernelVolatileMemLock", hle_VolatileMemLock}, {"sceKernelVolatileMemUnlock", hle_Zero},
    {"sceRtcGetCurrentClockLocalTime", hle_RtcGetCurrentClockLocalTime},
    {"sceImposeGetLanguageMode", hle_ImposeGetLanguageMode}, {"sceImposeSetLanguageMode", hle_Zero},
    {"sceUtilityMsgDialogInitStart", hle_MsgDialogInitStart}, {"sceUtilityMsgDialogGetStatus", hle_MsgDialogGetStatus},
    {"sceUtilityMsgDialogUpdate", hle_MsgDialogUpdate}, {"sceUtilityMsgDialogShutdownStart", hle_MsgDialogShutdownStart},
    {"sceUtilityLoadModule", hle_Zero}, {"sceUtilityUnloadModule", hle_Zero},
    {"sceKernelLoadModule", hle_LoadModule}, {"sceKernelStartModule", hle_Zero}, {"sceKernelStopModule", hle_Zero},
    {"sceKernelUnloadModule", hle_Zero}, {"sceKernelGetModuleId", hle_GetModuleId},
    {"sceKernelGetModuleIdByAddress", hle_GetModuleId}, {"sceKernelStopUnloadSelfModuleWithStatus", hle_StopUnloadSelf},
    {"sceKernelRegisterExitCallback", hle_Zero}, {"sceKernelExitGame", hle_ExitGame}, {"sceKernelLoadExec", hle_ExitGame},
    {"sceKernelStdin", hle_Stdin}, {"sceKernelStdout", hle_Stdout}, {"sceKernelStderr", hle_Stderr},
    {"sceNpDrmSetLicenseeKey", hle_Zero}, {"sceNpDrmEdataSetupKey", hle_Zero},
    {NULL, NULL},
};
