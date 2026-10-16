/* Core runtime: guest memory, function lookup, aborts, image loading. */
#include "rt_internal.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

uint8_t *rt_mem;
jmp_buf *rt_abort_jmp;
void (*rt_abort_hook)(int kind, uint32_t addr, uint32_t extra, const char *what);
RtAbort rt_last_abort;
uint64_t rt_call_count;

void rt_mem_init(void) {
#ifdef _WIN32
    rt_mem = (uint8_t *)VirtualAlloc(NULL, RT_ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    rt_mem = (uint8_t *)mmap(NULL, RT_ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (rt_mem == MAP_FAILED) rt_mem = NULL;
#endif
    if (!rt_mem) { fprintf(stderr, "rt: cannot reserve %u bytes of guest memory\n", RT_ARENA_SIZE); exit(1); }
}

uint8_t *rt_read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (size) *size = (size_t)n;
    return buf;
}

int rt_load_image(const char *path, uint8_t **pristine) {
    size_t n = 0;
    uint8_t *img = rt_read_file(path, &n);
    if (!img) { fprintf(stderr, "rt: cannot read image %s\n", path); return 0; }
    if (n != RT_IMAGE_SIZE) {
        fprintf(stderr, "rt: image %s is %zu bytes, build expects %u (regenerate with recomp.py)\n", path, n, RT_IMAGE_SIZE);
        free(img);
        return 0;
    }
    memcpy(rt_ptr(RT_IMAGE_BASE), img, n);
    if (pristine) *pristine = img; else free(img);
    return 1;
}

void rt_abort(int kind, uint32_t addr, uint32_t extra, const char *what) {
    rt_last_abort.kind = kind;
    rt_last_abort.addr = addr;
    rt_last_abort.extra = extra;
    rt_last_abort.what = what;
    if (rt_abort_hook) rt_abort_hook(kind, addr, extra, what);
    if (rt_abort_jmp) longjmp(*rt_abort_jmp, kind);
    fprintf(stderr, "rt: fatal %s at %08X (%08X)\n", what ? what : "abort", addr, extra);
    exit(2);
}

RtFunc rt_lookup(uint32_t addr) {
    size_t lo = 0, hi = rt_funcs_count;
    addr &= RT_ADDR_MASK;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        uint32_t a = rt_funcs[mid].addr & RT_ADDR_MASK;
        if (a == addr) return rt_funcs[mid].fn;
        if (a < addr) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

void rt_call(RtCtx *c, uint32_t addr) {
    RtFunc f = rt_lookup(addr);
    rt_call_count++;
    if (!f) rt_abort(RT_ABORT_MISSING_FUNCTION, addr, c->r[31], "function not in this build");
    f(c);
}

/* Calls guest code from HLE (e.g. a game callback) with up to 8 arguments and returns its v0.
 * The caller's whole register state is restored afterwards. */
uint32_t rt_call_guest(RtCtx *c, uint32_t addr, int nargs, const uint32_t *args) {
    RtCtx saved = *c;
    for (int i = 0; i < nargs && i < 8; i++) c->r[4 + i] = args[i];
    rt_call(c, addr);
    uint32_t v0 = c->r[2];
    *c = saved;
    return v0;
}

void rt_bad_jump(RtCtx *c, uint32_t pc, uint32_t target) {
    (void)c;
    rt_abort(RT_ABORT_BAD_JUMP, pc, target, "jump-table target not in table");
}

void rt_trap(RtCtx *c, uint32_t pc, uint32_t word, const char *what) {
    /* the return address points into the caller (a game assert calls a function holding the break) */
    printf("[trap] %s at %08X (code %08X), ra %08X, a0..a3 %08X %08X %08X %08X\n", what, pc, word,
           c->r[31], c->r[4], c->r[5], c->r[6], c->r[7]);
    printf("[trap] stack at %08X:", c->r[29]);
    for (int i = 0; i < 24; i++) printf("%s%08X", i % 8 ? " " : "\n  ", rt_r32(c->r[29] + 4u * i));
    printf("\n");
    rt_abort(RT_ABORT_TRAP, pc, word, what);
}

/* zlib-compatible CRC-32, so test tooling can compare with zlib.crc32 */
uint32_t rt_crc32(const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
