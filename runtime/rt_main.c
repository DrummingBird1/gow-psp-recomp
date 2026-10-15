/* Prototype driver.
 *   gow_rt --image image.bin --difftest cases.bin results.bin
 *       Runs each recorded test case (function + initial CPU/memory state) and writes
 *       the final state for comparison against a reference emulator.
 *   gow_rt --image image.bin --boot [--max-hle N]
 *       Runs module_start with the minimal HLE layer until it finishes or stops. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

#include "rt_internal.h"

#define BOOT_STACK_TOP 0x09FFF000u
#define CASE_TIMEOUT_MS 3000
#define EXIT_CASE_HUNG 3

/* Watchdog: a test case that runs longer than CASE_TIMEOUT_MS ends the process with
 * EXIT_CASE_HUNG; results written so far stay valid and the driver resumes after it. */
static volatile uint32_t g_case_running, g_case_index, g_timeout_ms = CASE_TIMEOUT_MS;
static volatile uint64_t g_case_started;
static volatile int g_boot_mode;

static uint64_t now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

#ifdef _WIN32
static DWORD WINAPI watchdog(LPVOID arg)
#else
static void *watchdog(void *arg)
#endif
{
    (void)arg;
    for (;;) {
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
        if (g_case_running && now_ms() - g_case_started > g_timeout_ms) {
            if (g_boot_mode) {
                printf("[boot] stopped: time limit of %u ms reached (%llu recompiled calls through rt_call)\n",
                       g_timeout_ms, (unsigned long long)rt_call_count);
                rt_hle_report();
            } else {
                fprintf(stderr, "rt: case %u exceeded %u ms\n", g_case_index, g_timeout_ms);
            }
            fflush(NULL);
            _exit(EXIT_CASE_HUNG);
        }
    }
}

static void start_watchdog(void) {
#ifdef _WIN32
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
#else
    pthread_t t;
    pthread_create(&t, NULL, watchdog, NULL);
#endif
}

typedef struct {
    uint32_t magic, count, scratch_addr, scratch_size, stack_addr, stack_size;
} CaseHeader;

typedef struct {
    uint32_t status, abort_addr;
    uint32_t r[32], hi, lo, fpr[32], fpcond, fcr31;
    uint32_t h_image, h_scratch, h_stack;
} CaseResult;

static int run_difftest(const char *image, const char *cases_path, const char *results_path) {
    uint8_t *pristine = NULL;
    if (!rt_load_image(image, &pristine)) return 1;
    size_t n = 0;
    uint8_t *buf = rt_read_file(cases_path, &n);
    if (!buf || n < sizeof(CaseHeader)) { fprintf(stderr, "rt: cannot read %s\n", cases_path); return 1; }
    CaseHeader h;
    memcpy(&h, buf, sizeof(h));
    if (h.magic != 0x31435444u) { fprintf(stderr, "rt: bad case file magic\n"); return 1; }
    size_t regs = 4 * (1 + 32 + 2 + 32 + 2);
    size_t per_case = regs + h.scratch_size + h.stack_size;
    if (n != sizeof(h) + per_case * h.count) { fprintf(stderr, "rt: case file size mismatch\n"); return 1; }
    FILE *out = fopen(results_path, "wb");
    if (!out) { fprintf(stderr, "rt: cannot write %s\n", results_path); return 1; }
    rt_hle_init(RT_HLE_ABORT, 0, 0);
    start_watchdog();
    const uint8_t *p = buf + sizeof(h);
    for (uint32_t k = 0; k < h.count; k++, p += per_case) {
        RtCtx c;
        uint32_t w[1 + 32 + 2 + 32 + 2];
        memcpy(w, p, regs);
        memset(&c, 0, sizeof(c));
        memcpy(c.r, &w[1], 32 * 4);
        c.hi = w[33]; c.lo = w[34];
        for (int i = 0; i < 32; i++) c.fpr[i].i = w[35 + i];
        c.fcr31 = w[67]; c.fpcond = w[68];
        memcpy(rt_ptr(RT_IMAGE_BASE), pristine, RT_IMAGE_SIZE);
        memcpy(rt_ptr(h.scratch_addr), p + regs, h.scratch_size);
        memcpy(rt_ptr(h.stack_addr), p + regs + h.scratch_size, h.stack_size);
        CaseResult res;
        memset(&res, 0, sizeof(res));
        jmp_buf jb;
        rt_abort_jmp = &jb;
        g_case_index = k;
        g_case_started = now_ms();
        g_case_running = 1;
        int st = setjmp(jb);
        if (st == 0) rt_call(&c, w[0]);
        g_case_running = 0;
        rt_abort_jmp = NULL;
        res.status = (uint32_t)st;
        res.abort_addr = st ? rt_last_abort.addr : 0;
        memcpy(res.r, c.r, sizeof(res.r));
        res.hi = c.hi; res.lo = c.lo;
        for (int i = 0; i < 32; i++) res.fpr[i] = c.fpr[i].i;
        res.fpcond = c.fpcond; res.fcr31 = c.fcr31;
        res.h_image = rt_crc32(rt_ptr(RT_IMAGE_BASE), RT_IMAGE_SIZE);
        res.h_scratch = rt_crc32(rt_ptr(h.scratch_addr), h.scratch_size);
        res.h_stack = rt_crc32(rt_ptr(h.stack_addr), h.stack_size);
        fwrite(&res, sizeof(res), 1, out);
        fflush(out);
    }
    fclose(out);
    printf("difftest: ran %u cases\n", h.count);
    return 0;
}

static int run_boot(const char *image, uint32_t max_hle) {
    if (!rt_load_image(image, NULL)) return 1;
    rt_hle_init(RT_HLE_BOOT, RT_IMAGE_BASE + RT_IMAGE_SIZE, max_hle);
    static const char path[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    uint32_t argp = BOOT_STACK_TOP - 0x100;
    memcpy(rt_ptr(argp), path, sizeof(path));
    RtCtx c;
    memset(&c, 0, sizeof(c));
    c.r[4] = sizeof(path);
    c.r[5] = argp;
    c.r[28] = RT_GP;
    c.r[29] = argp - 0x40;
    printf("[boot] %s (%s): image %08X+%X, entry %08X, %zu functions in this build\n", RT_GAME_ID, RT_MODULE_NAME,
           RT_IMAGE_BASE, RT_IMAGE_SIZE, RT_ENTRY, rt_funcs_count);
    jmp_buf jb;
    rt_abort_jmp = &jb;
    g_boot_mode = 1;
    g_timeout_ms = 20000;
    g_case_started = now_ms();
    g_case_running = 1;
    start_watchdog();
    int st = setjmp(jb);
    if (st == 0) {
        rt_call(&c, RT_ENTRY);
        printf("[boot] module_start returned v0=%08X\n", c.r[2]);
    } else {
        printf("[boot] stopped (%d): %s at %08X (extra %08X)\n", st, rt_last_abort.what, rt_last_abort.addr, rt_last_abort.extra);
    }
    rt_abort_jmp = NULL;
    printf("[boot] %llu recompiled calls made\n", (unsigned long long)rt_call_count);
    rt_hle_report();
    return 0;
}

int main(int argc, char **argv) {
    const char *image = NULL, *cases = NULL, *results = NULL;
    int boot = 0;
    uint32_t max_hle = 2000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--image") && i + 1 < argc) image = argv[++i];
        else if (!strcmp(argv[i], "--difftest") && i + 2 < argc) { cases = argv[++i]; results = argv[++i]; }
        else if (!strcmp(argv[i], "--boot")) boot = 1;
        else if (!strcmp(argv[i], "--max-hle") && i + 1 < argc) max_hle = (uint32_t)strtoul(argv[++i], NULL, 0);
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (!image || (!cases && !boot)) {
        fprintf(stderr, "usage: %s --image image.bin (--difftest cases.bin results.bin | --boot [--max-hle N])\n", argv[0]);
        return 2;
    }
    rt_mem_init();
    return cases ? run_difftest(image, cases, results) : run_boot(image, max_hle);
}
