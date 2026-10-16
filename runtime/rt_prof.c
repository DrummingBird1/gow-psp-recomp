/* Sampling profiler (--profile): a thread samples the main thread's instruction pointer
 * about once per millisecond; at exit the samples are attributed to functions through the
 * executable's PDB (DbgHelp) and the most frequent ones are printed. Windows only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_prof.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#define SLOTS (1 << 16)
static struct { uint64_t pc; uint32_t n; } g_tab[SLOTS];
static volatile LONG g_stop;
static HANDLE g_target, g_sampler;
static uint64_t g_total, g_lost;

static void add(uint64_t pc) {
    uint32_t h = (uint32_t)((pc * 0x9E3779B97F4A7C15ull) >> 48) & (SLOTS - 1);
    for (int i = 0; i < 64; i++, h = (h + 1) & (SLOTS - 1)) {
        if (g_tab[h].pc == pc) { g_tab[h].n++; return; }
        if (!g_tab[h].pc) { g_tab[h].pc = pc; g_tab[h].n = 1; return; }
    }
    g_lost++;
}

static DWORD WINAPI sampler(LPVOID arg) {
    (void)arg;
    timeBeginPeriod(1);
    while (!g_stop) {
        Sleep(1);
        if (SuspendThread(g_target) == (DWORD)-1) continue;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_target, &ctx)) { add(ctx.Rip); g_total++; }
        ResumeThread(g_target);
    }
    timeEndPeriod(1);
    return 0;
}

void rt_prof_start(void) {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    g_sampler = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    SetThreadPriority(g_sampler, THREAD_PRIORITY_TIME_CRITICAL);
}

typedef struct { char name[96]; uint64_t n; } Fn;
static int by_count(const void *a, const void *b) {
    uint64_t x = ((const Fn *)a)->n, y = ((const Fn *)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}

void rt_prof_report(int top) {
    if (!g_sampler) return;
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(g_sampler, INFINITE);
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);
    static Fn fns[8192];
    int nf = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (!g_tab[i].pc) continue;
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        memset(buf, 0, sizeof(buf));
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        char name[96];
        DWORD64 disp = 0;
        if (SymFromAddr(proc, g_tab[i].pc, &disp, si)) snprintf(name, sizeof(name), "%s", si->Name);
        else snprintf(name, sizeof(name), "?%llx", (unsigned long long)g_tab[i].pc);
        int j;
        for (j = 0; j < nf && strcmp(fns[j].name, name); j++) {}
        if (j == nf) {
            if (nf == (int)(sizeof(fns) / sizeof(fns[0]))) continue;
            memcpy(fns[nf].name, name, sizeof(name));
            fns[nf++].n = 0;
        }
        fns[j].n += g_tab[i].n;
    }
    qsort(fns, nf, sizeof(Fn), by_count);
    printf("[prof] %llu samples (%llu not recorded); top functions:\n", (unsigned long long)g_total, (unsigned long long)g_lost);
    for (int i = 0; i < nf && i < top; i++)
        printf("  %5.1f%%  %s\n", g_total ? 100.0 * fns[i].n / g_total : 0.0, fns[i].name);
    SymCleanup(proc);
}
#else
void rt_prof_start(void) {}
void rt_prof_report(int top) { (void)top; }
#endif
