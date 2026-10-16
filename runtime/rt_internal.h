/* Runtime-internal declarations (not used by generated code). */
#pragma once
#include <setjmp.h>

#include "psp_rt.h"

enum {
    RT_OK = 0,
    RT_ABORT_MISSING_FUNCTION = 2,
    RT_ABORT_TRAP = 3,
    RT_ABORT_BAD_JUMP = 4,
    RT_ABORT_HLE = 5,
    RT_ABORT_BUDGET = 6,
    RT_ABORT_DEADLOCK = 7,
};

typedef struct { int kind; uint32_t addr, extra; const char *what; } RtAbort;

extern jmp_buf *rt_abort_jmp;  /* set by drivers; NULL = abort the process */
/* When set (the thread scheduler), aborts are routed here instead of longjmp/exit. */
extern void (*rt_abort_hook)(int kind, uint32_t addr, uint32_t extra, const char *what);
extern RtAbort rt_last_abort;
extern uint64_t rt_call_count;

void rt_mem_init(void);
uint8_t *rt_read_file(const char *path, size_t *size);
int rt_load_image(const char *path, uint8_t **pristine);
void rt_abort(int kind, uint32_t addr, uint32_t extra, const char *what);
RtFunc rt_lookup(uint32_t addr);
uint32_t rt_crc32(const uint8_t *p, size_t n);

/* HLE layer */
typedef enum { RT_HLE_ABORT, RT_HLE_BOOT } RtHleMode;
void rt_hle_init(RtHleMode mode, uint32_t heap_start, uint32_t max_calls);
void rt_hle_report(void);

int rt_vfpu_selftest(void);

/* File I/O layer */
int rt_io_open_disc(const char *path);
void rt_io_set_ms0(const char *dir);
int rt_io_extract(const char *disc_path, const char *out);
void rt_io_report(void);
