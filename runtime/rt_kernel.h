/* PSP kernel emulation (HLE): objects, threads/scheduler, memory, HLE registration. */
#pragma once
#include <stdint.h>

#include "rt_fiber.h"
#include "rt_internal.h"

/* --- HLE function tables ------------------------------------------------- */
typedef void (*HleFn)(RtCtx *c);
typedef struct { const char *name; HleFn fn; } HleEntry;

extern const HleEntry rt_hle_kernel[];   /* rt_thread.c: ThreadMan, SysMem, Kernel_Library, Utils */
extern const HleEntry rt_hle_io[];       /* rt_io.c: IoFileMgr, Umd */
extern const HleEntry rt_hle_misc[];     /* rt_hle.c: display, ge, ctrl, audio, utility, power, ... */

#define A0 (c->r[4])
#define A1 (c->r[5])
#define A2 (c->r[6])
#define A3 (c->r[7])
#define T0 (c->r[8])
#define T1 (c->r[9])
#define T2 (c->r[10])
#define T3 (c->r[11])
#define RET(v) do { c->r[2] = (uint32_t)(v); return; } while (0)
#define RET64(v) do { uint64_t r64_ = (uint64_t)(v); c->r[2] = (uint32_t)r64_; c->r[3] = (uint32_t)(r64_ >> 32); return; } while (0)

extern int rt_hle_verbose;  /* 0: important events only, 1: every call */
void rt_log(const char *fmt, ...);

/* --- Error codes (PSP kernel) --------------------------------------------- */
#define SCE_ERR_NO_MEMORY        0x80020190u
#define SCE_ERR_ILLEGAL_PRIORITY 0x80020193u
#define SCE_ERR_UNKNOWN_THID     0x80020198u
#define SCE_ERR_UNKNOWN_SEMID    0x80020199u
#define SCE_ERR_UNKNOWN_EVFID    0x8002019Au
#define SCE_ERR_UNKNOWN_FPLID    0x8002019Du
#define SCE_ERR_UNKNOWN_UID      0x800200CBu
#define SCE_ERR_DORMANT          0x800201A2u
#define SCE_ERR_NOT_DORMANT      0x800201A4u
#define SCE_ERR_CAN_NOT_WAIT     0x800201A7u
#define SCE_ERR_WAIT_TIMEOUT     0x800201A8u
#define SCE_ERR_SEMA_ZERO        0x800201ADu
#define SCE_ERR_SEMA_OVF         0x800201AEu
#define SCE_ERR_EVF_COND         0x800201AFu
#define SCE_ERR_EVF_MULTI        0x800201B0u
#define SCE_ERR_WAIT_DELETE      0x800201B5u
#define SCE_ERR_FILE_NOT_FOUND   0x80010002u
#define SCE_ERR_BADF             0x80020323u
#define SCE_ERR_NOASYNC          0x8002032Au

/* --- Kernel objects ------------------------------------------------------- */
typedef enum { OBJ_FREE, OBJ_THREAD, OBJ_SEMA, OBJ_EVF, OBJ_FPL, OBJ_CALLBACK, OBJ_BLOCK, OBJ_MODULE } ObjKind;

int k_obj_new(ObjKind kind, void *p);
void *k_obj_get(int uid, ObjKind kind);
void k_obj_free(int uid);

/* --- Threads -------------------------------------------------------------- */
enum { TS_RUNNING = 1, TS_READY = 2, TS_WAIT = 4, TS_DORMANT = 16 };
enum { WT_NONE, WT_SLEEP, WT_DELAY, WT_SEMA, WT_EVF, WT_THREADEND, WT_FPL, WT_IO, WT_VBLANK, WT_AUDIO };

#define K_NO_TIMEOUT UINT64_MAX

typedef struct KThread {
    int uid;
    char name[32];
    uint32_t entry, attr, stack_addr, stack_size, gp;
    int init_prio, prio;
    int status;
    int wait_type, wait_id;
    uint32_t wait_val, wait_val2, wait_ptr, timeout_ptr;
    uint64_t wake_time, wait_start;
    int32_t wait_result;
    int32_t exit_status;
    int64_t ready_seq;
    int delete_on_exit;
    RtCtx ctx;
    RtFiber *fiber;
} KThread;

extern uint64_t k_now;            /* virtual time, microseconds */
extern KThread *k_cur;            /* running PSP thread (NULL while scheduling) */

KThread *k_thread_create(const char *name, uint32_t entry, int prio, uint32_t stack_size, uint32_t attr);
void k_thread_start(KThread *t, uint32_t argsize, uint32_t argp);
/* Block the current thread; returns the wake result (0, a timeout or a deletion error). */
int32_t k_wait(int type, int id, uint64_t timeout_us);
void k_wake(KThread *t, int32_t result);
/* Give up the CPU if a ready thread now has higher priority than the current one. */
void k_check_preempt(void);
/* Iterate over threads waiting on (type, id), in wake order (priority attr or FIFO). */
int k_waiters(int type, int id, int by_priority, KThread **out, int max);
void k_stop(int kind, uint32_t addr, uint32_t extra, const char *what);
int k_run_scheduler(uint32_t timeout_ms_wall);
uint32_t k_timeout_arg(uint32_t ptr);  /* reads a guest timeout pointer (0 = infinite) */

/* --- Memory (user partition) --------------------------------------------- */
void k_mem_init(uint32_t lo, uint32_t hi);
uint32_t k_alloc(uint32_t size, int high, uint32_t align);
void k_free(uint32_t addr);
uint32_t k_max_free(void);
uint32_t k_total_free(void);
