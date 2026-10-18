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

#include "rt_audio.h"
#include "rt_debug.h"
#include "rt_frames.h"
#include "rt_ge.h"
#include "rt_gpu.h"
#include "rt_host.h"
#include "rt_kernel.h"
#include "rt_prof.h"
#define CASE_TIMEOUT_MS 3000
#define EXIT_CASE_HUNG 3

/* Watchdog: a test case that runs longer than CASE_TIMEOUT_MS ends the process with
 * EXIT_CASE_HUNG; results written so far stay valid and the driver resumes after it. */
static volatile uint32_t g_case_running, g_case_index, g_timeout_ms = CASE_TIMEOUT_MS;
static volatile uint64_t g_case_started;
static volatile int g_boot_mode, g_profile;

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
                if (g_profile) rt_prof_report(40);  /* where a run that never yields spends its time */
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
        rt_ctx_init(&c);
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

typedef struct {
    const char *image, *disc, *ms0;
    uint32_t max_hle, cpu_ms, wall_ms;
    FrameOptions frames;
    HostOptions host;
    int profile, cpu_set, wall_set;
    const char *wav;
    int mute;
} BootOptions;

static void boot_report(void) {
    printf("[boot] %llu calls through rt_call (indirect or cross-build)\n", (unsigned long long)rt_call_count);
    rt_hle_report();
    rt_io_report();
    rt_ge_report();
    rt_co_report();
    rt_audio_report();
    rt_sas_report();
    rt_atrac_report();
    rt_mpeg_report();
    rt_savedata_report();
    rt_debug_report();
}

#ifdef _WIN32
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;
/* A crash (also inside a graphics or sound driver) leaves a line in the log, which the launcher
 * writes to a file: otherwise the buffered output would be lost and a bug report would show nothing. */
static LONG WINAPI on_crash(EXCEPTION_POINTERS *e) {
    rt_host_record_close();
    char mod[MAX_PATH] = "?";
    HMODULE hm = NULL;
    void *addr = e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionAddress : NULL;
    if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)addr, &hm))
        GetModuleFileNameA(hm, mod, sizeof(mod));
    const char *base = strrchr(mod, '\\');
    printf("[crash] exception %08lX at %p (%s+%llX)\n", e && e->ExceptionRecord ? (unsigned long)e->ExceptionRecord->ExceptionCode : 0ul,
           addr, base ? base + 1 : mod, (unsigned long long)((char *)addr - (char *)hm));
    fflush(NULL);
    return g_prev_filter ? g_prev_filter(e) : EXCEPTION_CONTINUE_SEARCH;
}
#endif

/* Runs the game under the thread scheduler: module_start starts in a kernel-style
 * thread (as the PSP module manager does) and the scheduler runs until every thread
 * is blocked on something the runtime cannot provide yet, or a limit is reached. */
static int run_boot(const BootOptions *o) {
    if (!rt_load_image(o->image, NULL)) return 1;
    rt_hle_init(RT_HLE_BOOT, RT_IMAGE_BASE + RT_IMAGE_SIZE, o->max_hle);
    if (o->ms0) rt_io_set_ms0(o->ms0);
    if (o->disc && !rt_io_open_disc(o->disc)) return 1;
    printf("[boot] %s (%s): image %08X+%X, entry %08X, %zu functions in this build\n", RT_GAME_ID, RT_MODULE_NAME,
           RT_IMAGE_BASE, RT_IMAGE_SIZE, RT_ENTRY, rt_funcs_count);
    static const char path[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    uint32_t argp = k_alloc(0x100, 0, 0x100);
    memcpy(rt_ptr(argp), path, sizeof(path));
    KThread *t = k_thread_create("module_start", RT_ENTRY, 0x20, 0x40000, 0x80000000u);
    k_thread_start(t, sizeof(path), argp);
#ifdef _WIN32
    g_prev_filter = SetUnhandledExceptionFilter(on_crash);  /* before the window and GPU start: drivers crash there too */
#endif
    if (!rt_host_init(&o->host)) return 1;
    AudioOptions ao = { o->wav, o->host.window && !o->mute };
    if (!rt_audio_init(&ao)) return 1;
    /* An interactive window runs until it is closed, unless limits were given explicitly. */
    uint32_t cpu_ms = o->host.window && !o->cpu_set ? 0xFFFFFFFFu : o->cpu_ms;
    g_boot_mode = 1;
    g_timeout_ms = o->wall_ms;
    g_case_started = now_ms();
    g_case_running = !o->host.window || o->wall_set;
    start_watchdog();
    rt_frames_init(&o->frames);
    if (o->host.window || rt_host_wants_idle()) k_idle_hook = rt_host_idle;
    if (o->profile) { rt_prof_start(); g_profile = 1; }
    k_run_scheduler(cpu_ms);
    g_case_running = 0;
    rt_frames_close(k_now);
    rt_audio_shutdown(k_now);
    rt_gpu_report();  /* before the host shuts the device down */
    rt_host_shutdown();
    boot_report();
    if (o->profile) rt_prof_report(40);
    return 0;
}

int main(int argc, char **argv) {
    const char *image = NULL, *cases = NULL, *results = NULL, *renderer = "soft", *extract = NULL, *extract_to = NULL;
    int boot = 0;
    BootOptions o = { NULL, NULL, NULL, 0, 30000, 60000, { 0 }, { 0, 2, 0, 0, NULL, NULL, "." }, 0, 0, 0, NULL, 0 };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--image") && i + 1 < argc) image = argv[++i];
        else if (!strcmp(argv[i], "--difftest") && i + 2 < argc) { cases = argv[++i]; results = argv[++i]; }
        else if (!strcmp(argv[i], "--boot")) boot = 1;
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) o.disc = argv[++i];
        else if (!strcmp(argv[i], "--ms0") && i + 1 < argc) o.ms0 = argv[++i];
        else if (!strcmp(argv[i], "--max-hle") && i + 1 < argc) o.max_hle = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--cpu-ms") && i + 1 < argc) { o.cpu_ms = (uint32_t)strtoul(argv[++i], NULL, 0); o.cpu_set = 1; }
        else if (!strcmp(argv[i], "--wall-ms") && i + 1 < argc) { o.wall_ms = (uint32_t)strtoul(argv[++i], NULL, 0); o.wall_set = 1; }
        else if (!strcmp(argv[i], "--window")) o.host.window = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) o.host.scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--icon") && i + 1 < argc) o.host.icon = argv[++i];
        else if (!strcmp(argv[i], "--controls") && i + 1 < argc) o.host.controls = argv[++i];
        else if (!strcmp(argv[i], "--fullscreen")) o.host.fullscreen = 1;
        else if (!strcmp(argv[i], "--nearest")) o.host.nearest = 1;
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) o.host.input = argv[++i];
        else if (!strcmp(argv[i], "--record-input") && i + 1 < argc) o.host.record_input = argv[++i];
        else if (!strcmp(argv[i], "--save-slot") && i + 1 < argc) rt_savedata_slot = argv[++i];
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) o.host.shot_dir = argv[++i];
        else if (!strcmp(argv[i], "--renderer") && i + 1 < argc) renderer = argv[++i];
        else if (!strcmp(argv[i], "--gpu-driver") && i + 1 < argc) o.host.gpu_driver = argv[++i];
        else if (!strcmp(argv[i], "--gpu-compare-at") && i + 1 < argc) o.frames.gpu_compare_at = argv[++i];
        else if (!strcmp(argv[i], "--trace-hle")) rt_hle_verbose = 1;
        else if (!strcmp(argv[i], "--frames-dir") && i + 1 < argc) o.frames.png_dir = argv[++i];
        else if (!strcmp(argv[i], "--frames-every") && i + 1 < argc) o.frames.png_every = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--frames-at") && i + 1 < argc) o.frames.png_at = argv[++i];
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) o.frames.video = argv[++i];
        else if (!strcmp(argv[i], "--record-host") && i + 1 < argc) o.frames.video_host = argv[++i];
        else if (!strcmp(argv[i], "--ffmpeg") && i + 1 < argc) o.frames.ffmpeg = argv[++i];
        else if (!strcmp(argv[i], "--perf-csv") && i + 1 < argc) o.frames.perf_csv = argv[++i];
        else if (!strcmp(argv[i], "--profile")) o.profile = 1;
        else if (!strcmp(argv[i], "--max-frames") && i + 1 < argc) rt_max_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fast-forward") && i + 1 < argc) rt_fast_forward = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) o.wav = argv[++i];
        else if (!strcmp(argv[i], "--mute")) o.mute = 1;
        else if (!strcmp(argv[i], "--extract") && i + 2 < argc) { extract = argv[++i]; extract_to = argv[++i]; }
        else if (!strcmp(argv[i], "--vfpu-selftest")) { rt_mem_init(); return rt_vfpu_selftest(); }
        else if (!strcmp(argv[i], "--movie-test") && i + 2 < argc) {
            const char *pmf = argv[i + 1], *out = argv[i + 2];
            int ch = i + 3 < argc && argv[i + 3][0] != '-' ? atoi(argv[i + 3]) : 0;
            return rt_mpeg_test(pmf, out, ch);
        }
        else if (rt_debug_option(argc, argv, &i)) continue;
        else if (rt_gpu_option(argc, argv, &i)) continue;
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (extract) {
        rt_mem_init();
        return o.disc && rt_io_open_disc(o.disc) && rt_io_extract(extract, extract_to) ? 0 : 1;
    }
    if (!image || (!cases && !boot)) {
        fprintf(stderr, "usage: %s --image image.bin (--difftest cases.bin results.bin |\n"
                        "       --boot [--disc game.chd|.iso] [--ms0 dir] [--max-hle N] [--cpu-ms N] [--wall-ms N] [--trace-hle]\n"
                        "              [--frames-dir dir (--frames-every N | --frames-at N,N,...)] [--record out.mp4] [--record-host out.mp4] [--ffmpeg exe]\n"
                        "              [--perf-csv frames.csv] [--profile] [--max-frames N] [--fast-forward N]\n"
                        "              [--window [--scale N] [--fullscreen] [--nearest] [--shots dir]] [--input frame:buttons[:frames],...|file]\n"
                        "              [--record-input file] [--save-slot NAME] [--icon file.png] [--controls file]\n"
                        "              [--renderer soft|gpu|both] [--gpu-driver vulkan|direct3d12] [--gpu-compare-at N,N,...]\n"
                        "              [--res-scale 1-8] [--fx-scale 1-8] [--msaa 2|4|8] [--aniso 2-16] [--lod-bias F] [--fxaa] [--sharpen 0-1]\n"
                        "              [--hdr [auto|scrgb|hdr10]] [--paper-white NITS] [--peak-nits NITS] [--vsync] [--fps HZ]\n"
                        "              [--aspect W:H] [--texture-dump dir] [--texture-pack dir]\n"
                        "              [--wav out.wav] [--mute]\n"
                        "              [--warp LEVEL[:SPAWN][@FRAME]] [--god] [--level-log] [--load-level NAME@FRAME]\n"
                        "              [--dump-ram N,N,...|stop:prefix])\n"
                        "       %s --disc game.chd|.iso --extract disc0:/path out.file (a directory is listed)\n"
                        "       %s --movie-test movie.pmf out_prefix [audio channel]\n",
                argv[0], argv[0], argv[0]);
        return 2;
    }
    if (strcmp(renderer, "soft") && strcmp(renderer, "gpu") && strcmp(renderer, "both")) {
        fprintf(stderr, "--renderer must be soft, gpu or both\n");
        return 2;
    }
    rt_ge_set_renderer(strcmp(renderer, "gpu") != 0, strcmp(renderer, "soft") != 0);
    o.host.gpu = strcmp(renderer, "soft") != 0;
    o.frames.from_gpu = !strcmp(renderer, "gpu");
    rt_ge_skip_draw = rt_fast_forward > 1;
    rt_mem_init();
    o.image = image;
    return cases ? run_difftest(image, cases, results) : run_boot(&o);
}
