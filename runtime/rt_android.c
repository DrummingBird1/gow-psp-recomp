#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
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
    install_handlers();
    atexit(on_exit_log);
    char *args[] = { "gow", "--image", image, "--boot", "--disc", disc,
                     "--ms0", ms0, "--window", "--fullscreen", "--renderer", "gpu", "--gpu-driver", "vulkan" };
    int r = gow_main((int)(sizeof args / sizeof *args), args);
    fprintf(stderr, "rt_android: gow_main retornou %d\n", r);
    return r;
}
