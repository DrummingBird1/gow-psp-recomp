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
