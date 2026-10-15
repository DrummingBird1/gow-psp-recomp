/* Minimal fiber (cooperative context) abstraction used by the PSP thread scheduler. */
#pragma once
#include <stddef.h>

typedef struct RtFiber RtFiber;
typedef void (*RtFiberProc)(void *arg);

/* Turn the calling OS thread into the scheduler fiber. Call once. */
RtFiber *rt_fiber_init_main(void);
/* Create a fiber with its own C stack; it starts running `proc(arg)` on first switch.
 * `proc` must never return: it has to switch away for good instead. */
RtFiber *rt_fiber_create(size_t stack_size, RtFiberProc proc, void *arg);
void rt_fiber_switch(RtFiber *from, RtFiber *to);
/* Free a fiber that is not currently running. */
void rt_fiber_delete(RtFiber *f);
