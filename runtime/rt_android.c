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


extern void *rt_fiber_main_parked_sp(void);
static uintptr_t g_lo, g_hi;

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
    if (!parked || parked < g_lo || parked >= g_hi || (sp >= g_lo && sp < g_hi)) {
        fn(arg);
        return;
    }
    gow_call_on_stack((void *)((parked - 1024) & ~(uintptr_t)15), fn, arg);
}

typedef struct { SDL_Event *e; Sint32 t; bool r; } hostev_t;
static void poll_fn(void *a) { hostev_t *h = (hostev_t *)a; h->r = SDL_PollEvent(h->e); }
static void wait_fn(void *a) { hostev_t *h = (hostev_t *)a; h->r = SDL_WaitEventTimeout(h->e, h->t); }
bool gow_host_poll(SDL_Event *e) { hostev_t h = { e, 0, false }; on_host(poll_fn, &h); return h.r; }
bool gow_host_wait(SDL_Event *e, Sint32 t) { hostev_t h = { e, t, false }; on_host(wait_fn, &h); return h.r; }

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
    {
        static char lcp[600];
        snprintf(lcp, sizeof lcp, "%s/logcat_prev.txt", base);
        FILE *o = fopen(lcp, "w");
        FILE *pp = popen("logcat -d -t 800 '*:E' 2>&1", "r");
        if (o && pp) { char b[512]; while (fgets(b, sizeof b, pp)) fputs(b, o); }
        if (pp) pclose(pp);
        if (o) fclose(o);
    }
    freopen(logp, "w", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);
    {
        pthread_attr_t at;
        void *addr = NULL;
        size_t sz = 0;
        if (pthread_getattr_np(pthread_self(), &at) == 0) {
            pthread_attr_getstack(&at, &addr, &sz);
            pthread_attr_destroy(&at);
        }
        g_lo = (uintptr_t)addr;
        g_hi = g_lo + sz;
        fprintf(stderr, "rt_android: pilha real %p..%p\n", addr, (void *)g_hi);
    }
    install_handlers();
    atexit(on_exit_log);
    char *args[] = { "gow", "--image", image, "--boot", "--disc", disc,
                     "--ms0", ms0, "--window", "--fullscreen", "--renderer", "soft" };
    int r = gow_main((int)(sizeof args / sizeof *args), args);
    fprintf(stderr, "rt_android: gow_main retornou %d\n", r);
    return r;
}
