#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <pthread.h>
#include <unistd.h>

extern int gow_main(int argc, char **argv);
#include <unwind.h>
static _Unwind_Reason_Code bt_cb(struct _Unwind_Context *c, void *arg) {
    int *n = (int *)arg;
    uintptr_t pc = _Unwind_GetIP(c);
    if (!pc) return _URC_END_OF_STACK;
    Dl_info d;
    char b[400];
    int k;
    if (dladdr((void *)pc, &d) && d.dli_fname)
        k = snprintf(b, sizeof b, "  #%d %s+0x%lx %s\n", *n, d.dli_fname,
                     (unsigned long)(pc - (uintptr_t)d.dli_fbase), d.dli_sname ? d.dli_sname : "");
    else
        k = snprintf(b, sizeof b, "  #%d pc=%p\n", *n, (void *)pc);
    write(2, b, (size_t)k);
    return ++*n >= 30 ? _URC_END_OF_STACK : _URC_NO_REASON;
}

static void on_exit_log(void) { fprintf(stderr, "rt_android: exit() chamado\n"); }

static void on_signal(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
    uintptr_t lr = (uintptr_t)uc->uc_mcontext.regs[30];
    Dl_info d;
    uintptr_t base = 0;
    if (dladdr((void *)pc, &d)) base = (uintptr_t)d.dli_fbase;
    char buf[400];
    int n = snprintf(buf, sizeof buf,
        "CRASH sinal=%d addr=%p pc=%p off_pc=0x%lx off_lr=0x%lx lib=%s\n",
        sig, si->si_addr, (void *)pc, (unsigned long)(pc - base),
        (unsigned long)(lr - base), d.dli_fname ? d.dli_fname : "?");
    write(2, buf, (size_t)n);
    { int cnt = 0; _Unwind_Backtrace(bt_cb, &cnt); }
    _exit(128 + sig);
}

static void install_handlers(void) {
    static stack_t ss;
    ss.ss_size = 65536;
    ss.ss_sp = malloc(ss.ss_size);
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
}

/* SDL's Android event path can call into Java. The PSP scheduler runs on a custom
 * fiber stack, so route SDL_PollEvent/SDL_WaitEventTimeout to the parked scheduler
 * stack on the same SDL pthread. */
extern void *rt_fiber_main_parked_sp(void);
static uintptr_t g_lo, g_hi;
static unsigned g_host_event_calls;

__asm__(
    ".text\n"
    ".align 4\n"
    ".global gow_call_on_stack\n"
    ".type gow_call_on_stack, %function\n"
    "gow_call_on_stack:\n"
    "    stp x29, x30, [sp, #-16]!\n"
    "    mov x29, sp\n"
    "    mov sp, x0\n"
    "    mov x0, x2\n"
    "    blr x1\n"
    "    mov sp, x29\n"
    "    ldp x29, x30, [sp], #16\n"
    "    ret\n"
    ".size gow_call_on_stack, .-gow_call_on_stack\n"
);
void gow_call_on_stack(void *newsp, void (*fn)(void *), void *arg);

static void on_host(void (*fn)(void *), void *arg) {
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    uintptr_t parked = (uintptr_t)rt_fiber_main_parked_sp();
    unsigned call = ++g_host_event_calls;

    if (g_lo && g_hi > g_lo && sp >= g_lo && sp < g_hi) {
        if (call <= 8)
            fprintf(stderr, "rt_android: SDL event call %u already on pthread stack\n", call);
        fn(arg);
        return;
    }

    /* Keep a safety margin above the pthread stack guard and retain 16-byte ABI alignment. */
    if (!g_lo || g_hi <= g_lo || !parked ||
        parked < g_lo + 65536u || parked >= g_hi) {
        if (call <= 8)
            fprintf(stderr,
                    "rt_android: SDL event call %u using current stack (bounds=%p..%p parked=%p)\n",
                    call, (void *)g_lo, (void *)g_hi, (void *)parked);
        fn(arg);
        return;
    }

    uintptr_t target = parked & ~(uintptr_t)15;
    if (target < g_lo + 32768u || target >= g_hi) {
        if (call <= 8)
            fprintf(stderr, "rt_android: SDL event call %u rejected stack target=%p\n",
                    call, (void *)target);
        fn(arg);
        return;
    }
    if (call <= 8)
        fprintf(stderr, "rt_android: SDL event call %u switching to pthread stack at %p\n",
                call, (void *)target);
    gow_call_on_stack((void *)target, fn, arg);
}

typedef struct { SDL_Event *e; Sint32 t; bool r; } hostev_t;
static void poll_fn(void *a) { hostev_t *h = (hostev_t *)a; h->r = SDL_PollEvent(h->e); }
static void wait_fn(void *a) { hostev_t *h = (hostev_t *)a; h->r = SDL_WaitEventTimeout(h->e, h->t); }
bool gow_host_poll(SDL_Event *e) { hostev_t h = { e, 0, false }; on_host(poll_fn, &h); return h.r; }
bool gow_host_wait(SDL_Event *e, Sint32 t) { hostev_t h = { e, t, false }; on_host(wait_fn, &h); return h.r; }

static void init_stack_bounds(void) {
    pthread_attr_t at;
    void *addr = NULL;
    size_t sz = 0;
    if (pthread_getattr_np(pthread_self(), &at) == 0) {
        if (pthread_attr_getstack(&at, &addr, &sz) != 0) { addr = NULL; sz = 0; }
        pthread_attr_destroy(&at);
    }
    g_lo = (uintptr_t)addr;
    g_hi = g_lo + sz;
    fprintf(stderr, "rt_android: pthread stack %p..%p (%zu bytes)\n", addr, (void *)g_hi, sz);
}

static void *mem_thread(void *u) {
    (void)u;
    for (;;) {
        long rss = 0, swap = 0;
        char l[128];
        FILE *f = fopen("/proc/self/status", "r");
        if (f) {
            while (fgets(l, sizeof l, f)) { sscanf(l, "VmRSS: %ld", &rss); sscanf(l, "VmSwap: %ld", &swap); }
            fclose(f);
        }
        fprintf(stderr, "[mem] rss=%ld MB swap=%ld MB\n", rss / 1024, swap / 1024);
        sleep(3);
    }
    return NULL;
}

static SDL_LogOutputFunction g_def_log;
static void SDLCALL sdl_log_to_file(void *u, int cat, SDL_LogPriority pri, const char *msg) {
    fprintf(stderr, "[sdl cat=%d pri=%d] %s\n", cat, (int)pri, msg);
    if (g_def_log) g_def_log(u, cat, pri, msg);
}

static char g_args_buf[2048];
static char *g_argv[96];
static int g_argc;
static void add_arg(char *s) { if (g_argc < 95) g_argv[g_argc++] = s; }
static void load_extra_args(const char *base) {
    char p[600];
    snprintf(p, sizeof p, "%s/args.txt", base);
    FILE *f = fopen(p, "r");
    if (!f) return;
    size_t n = fread(g_args_buf, 1, sizeof g_args_buf - 1, f);
    fclose(f);
    g_args_buf[n] = 0;
    for (char *t = strtok(g_args_buf, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) add_arg(t);
}

__attribute__((visibility("default")))
int SDL_main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *base = SDL_GetAndroidExternalStoragePath();
    if (!base) base = "/sdcard";
    static char image[600], disc[600], ms0[600], logp[600];
    snprintf(image, sizeof image, "%s/image.bin", base);
    snprintf(disc, sizeof disc, "%s/game.iso", base);
    snprintf(ms0, sizeof ms0, "%s/ms0", base);
    snprintf(logp, sizeof logp, "%s/log.txt", base);
    freopen(logp, "w", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(fileno(stderr), fileno(stdout));
    setvbuf(stdout, NULL, _IONBF, 0);
    { pthread_t mt; pthread_create(&mt, NULL, mem_thread, NULL); pthread_detach(mt); }
    { char gp[600]; snprintf(gp, sizeof gp, "%s/gpu_sync", base); if (access(gp, F_OK) == 0) { setenv("RT_GPU_SYNC", "1", 1); fprintf(stderr, "rt_android: RT_GPU_SYNC ligado (arquivo gpu_sync)\n"); } }
    g_def_log = SDL_GetDefaultLogOutputFunction();
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);
    SDL_SetLogOutputFunction(sdl_log_to_file, NULL);
    init_stack_bounds();
    install_handlers();
    atexit(on_exit_log);
    add_arg("gow"); add_arg("--image"); add_arg(image); add_arg("--boot");
    add_arg("--disc"); add_arg(disc); add_arg("--ms0"); add_arg(ms0);
    add_arg("--window"); add_arg("--fullscreen"); add_arg("--renderer"); add_arg("gpu");
    load_extra_args(base);
    fprintf(stderr, "rt_android: %d argumentos:", g_argc);
    for (int i = 1; i < g_argc; i++) fprintf(stderr, " %s", g_argv[i]);
    fprintf(stderr, "\n");
    g_argv[g_argc] = NULL;
    int r = gow_main(g_argc, g_argv);
    fprintf(stderr, "rt_android: gow_main retornou %d\n", r);
    return r;
}
