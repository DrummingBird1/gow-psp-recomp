#include "rt_fiber.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct RtFiber { void *handle; RtFiberProc proc; void *arg; int is_main; };

static void WINAPI fiber_entry(void *p) {
    RtFiber *f = (RtFiber *)p;
    f->proc(f->arg);
    fprintf(stderr, "rt: fiber procedure returned\n");
    abort();
}

RtFiber *rt_fiber_init_main(void) {
    RtFiber *f = (RtFiber *)calloc(1, sizeof(*f));
    f->handle = ConvertThreadToFiber(NULL);
    f->is_main = 1;
    if (!f->handle) { fprintf(stderr, "rt: ConvertThreadToFiber failed\n"); exit(1); }
    return f;
}

RtFiber *rt_fiber_create(size_t stack_size, RtFiberProc proc, void *arg) {
    RtFiber *f = (RtFiber *)calloc(1, sizeof(*f));
    f->proc = proc;
    f->arg = arg;
    /* Commit little, reserve a lot: recompiled code nests C calls as deeply as the game nests calls */
    f->handle = CreateFiberEx(64 * 1024, stack_size, FIBER_FLAG_FLOAT_SWITCH, fiber_entry, f);
    if (!f->handle) { fprintf(stderr, "rt: CreateFiberEx failed\n"); exit(1); }
    return f;
}

void rt_fiber_switch(RtFiber *from, RtFiber *to) {
    (void)from;
    SwitchToFiber(to->handle);
}

void rt_fiber_delete(RtFiber *f) {
    if (!f) return;
    if (!f->is_main) DeleteFiber(f->handle);
    free(f);
}

#elif defined(__aarch64__) && defined(__linux__)
/* Android / Linux on ARM64: bionic has no makecontext/swapcontext, so the switch is written
 * by hand. It saves what the AAPCS64 calling convention makes the callee keep (x19-x30, sp,
 * d8-d15) on the old stack, stores sp in `from`, loads sp of `to` and returns into it. */
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct RtFiber { void *sp; void *stack; size_t size; RtFiberProc proc; void *arg; };

void rt_fiber_swap_a64(void **from_sp, void *to_sp);
void rt_fiber_tramp_a64(void);

__asm__(
    ".text\n"
    ".align 4\n"
    ".global rt_fiber_swap_a64\n"
    ".type rt_fiber_swap_a64, %function\n"
    "rt_fiber_swap_a64:\n"
    "    sub sp, sp, #176\n"
    "    stp x19, x20, [sp, #0]\n"
    "    stp x21, x22, [sp, #16]\n"
    "    stp x23, x24, [sp, #32]\n"
    "    stp x25, x26, [sp, #48]\n"
    "    stp x27, x28, [sp, #64]\n"
    "    stp x29, x30, [sp, #80]\n"
    "    stp d8,  d9,  [sp, #96]\n"
    "    stp d10, d11, [sp, #112]\n"
    "    stp d12, d13, [sp, #128]\n"
    "    stp d14, d15, [sp, #144]\n"
    "    mov x2, sp\n"
    "    str x2, [x0]\n"
    "    mov sp, x1\n"
    "    ldp x19, x20, [sp, #0]\n"
    "    ldp x21, x22, [sp, #16]\n"
    "    ldp x23, x24, [sp, #32]\n"
    "    ldp x25, x26, [sp, #48]\n"
    "    ldp x27, x28, [sp, #64]\n"
    "    ldp x29, x30, [sp, #80]\n"
    "    ldp d8,  d9,  [sp, #96]\n"
    "    ldp d10, d11, [sp, #112]\n"
    "    ldp d12, d13, [sp, #128]\n"
    "    ldp d14, d15, [sp, #144]\n"
    "    add sp, sp, #176\n"
    "    ret\n"
    ".size rt_fiber_swap_a64, .-rt_fiber_swap_a64\n"
    ".align 4\n"
    ".global rt_fiber_tramp_a64\n"
    ".type rt_fiber_tramp_a64, %function\n"
    "rt_fiber_tramp_a64:\n"
    "    mov x0, x19\n"
    "    blr x20\n"
    "    brk #0\n"
    ".size rt_fiber_tramp_a64, .-rt_fiber_tramp_a64\n"
);

static void fiber_entry(RtFiber *f) {
    f->proc(f->arg);
    fprintf(stderr, "rt: fiber procedure returned\n");
    abort();
}

/* The scheduler fiber is parked on the SDL thread's original pthread stack while a
 * PSP thread runs on its own fiber stack. Android SDL event polling must use that
 * original stack because SDL may cross into Java through JNI. */
static RtFiber *g_main;
void *rt_fiber_main_parked_sp(void) { return g_main ? g_main->sp : NULL; }

RtFiber *rt_fiber_init_main(void) {
    RtFiber *f = (RtFiber *)calloc(1, sizeof(RtFiber));
    if (!g_main) g_main = f;
    return f;
}

RtFiber *rt_fiber_create(size_t stack_size, RtFiberProc proc, void *arg) {
    RtFiber *f = (RtFiber *)calloc(1, sizeof(*f));
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    f->proc = proc;
    f->arg = arg;
    f->size = (stack_size + (size_t)page - 1) / (size_t)page * (size_t)page + (size_t)page;
    f->stack = mmap(NULL, f->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (f->stack == MAP_FAILED) { fprintf(stderr, "rt: fiber stack mmap failed\n"); exit(1); }
    mprotect(f->stack, (size_t)page, PROT_NONE);
    uintptr_t top = ((uintptr_t)f->stack + f->size) & ~(uintptr_t)15;
    uint64_t *frame = (uint64_t *)(top - 176);
    memset(frame, 0, 176);
    frame[0] = (uint64_t)(uintptr_t)f;
    frame[1] = (uint64_t)(uintptr_t)fiber_entry;
    frame[11] = (uint64_t)(uintptr_t)rt_fiber_tramp_a64;
    f->sp = frame;
    return f;
}

void rt_fiber_switch(RtFiber *from, RtFiber *to) { rt_fiber_swap_a64(&from->sp, to->sp); }

void rt_fiber_delete(RtFiber *f) {
    if (!f) return;
    if (f->stack) munmap(f->stack, f->size);
    free(f);
}

#else
#include <stdint.h>
#include <ucontext.h>

struct RtFiber { ucontext_t ctx; void *stack; RtFiberProc proc; void *arg; };

static void fiber_entry(unsigned hi, unsigned lo) {
    RtFiber *f = (RtFiber *)(((uintptr_t)hi << 32) | (uintptr_t)lo);
    f->proc(f->arg);
    fprintf(stderr, "rt: fiber procedure returned\n");
    abort();
}

RtFiber *rt_fiber_init_main(void) {
    return (RtFiber *)calloc(1, sizeof(RtFiber));
}

RtFiber *rt_fiber_create(size_t stack_size, RtFiberProc proc, void *arg) {
    RtFiber *f = (RtFiber *)calloc(1, sizeof(*f));
    f->proc = proc;
    f->arg = arg;
    f->stack = malloc(stack_size);
    getcontext(&f->ctx);
    f->ctx.uc_stack.ss_sp = f->stack;
    f->ctx.uc_stack.ss_size = stack_size;
    uintptr_t p = (uintptr_t)f;
    makecontext(&f->ctx, (void (*)(void))fiber_entry, 2, (unsigned)(p >> 32), (unsigned)p);
    return f;
}

void rt_fiber_switch(RtFiber *from, RtFiber *to) { swapcontext(&from->ctx, &to->ctx); }

void rt_fiber_delete(RtFiber *f) {
    if (!f) return;
    free(f->stack);
    free(f);
}
#endif
