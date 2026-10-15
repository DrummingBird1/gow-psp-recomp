/* VFPU self-test (gow_rt --vfpu-selftest): runs real instruction words taken from the games
 * on hand-chosen inputs and checks hand-computed results. Inputs and outputs are addressed
 * by register name (S/C/R vectors, M/E matrices), an independent path from rt_vfpu.c's
 * operand decoding. Expected values follow PPSSPP's interpreter semantics. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rt_internal.h"

static int g_fail, g_pass;

/* physical index of element i (of vector j for matrices) of a named register */
static int phys(const char *name, int n, int j, int i) {
    int m = name[1] - '0', a = name[2] - '0', b = name[3] - '0';
    switch (name[0]) {
    case 'S': return m * 16 + a * 4 + b;
    case 'C': return m * 16 + a * 4 + ((b + i) & 3);
    case 'R': return m * 16 + ((a + i) & 3) * 4 + b;
    case 'M': return m * 16 + ((a + j) & 3) * 4 + ((b + i) & 3);
    case 'E': return m * 16 + ((a + i) & 3) * 4 + ((b + j) & 3);
    }
    (void)n;
    return 0;
}
static void setv(RtCtx *c, const char *name, int n, const float *v) { for (int i = 0; i < n; i++) c->v[phys(name, n, 0, i)].f = v[i]; }
static void setm(RtCtx *c, const char *name, int n, const float *v) {
    for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) c->v[phys(name, n, j, i)].f = v[j * 4 + i];
}
static void expectv(RtCtx *c, const char *test, const char *name, int n, const float *want) {
    for (int i = 0; i < n; i++) {
        float got = c->v[phys(name, n, 0, i)].f;
        if (!(fabsf(got - want[i]) <= 1e-6f * (1.0f + fabsf(want[i])))) {
            printf("  FAIL %-28s %s[%d] = %.9g, expected %.9g\n", test, name, i, got, want[i]);
            g_fail++;
            return;
        }
    }
    g_pass++;
}
static void expectbits(RtCtx *c, const char *test, const char *name, int n, const uint32_t *want) {
    for (int i = 0; i < n; i++) {
        uint32_t got = c->v[phys(name, n, 0, i)].i;
        if (got != want[i]) {
            printf("  FAIL %-28s %s[%d] = %08X, expected %08X\n", test, name, i, got, want[i]);
            g_fail++;
            return;
        }
    }
    g_pass++;
}
static void expectu(const char *test, uint32_t got, uint32_t want) {
    if (got != want) { printf("  FAIL %-28s got %08X, expected %08X\n", test, got, want); g_fail++; }
    else g_pass++;
}
#define X(word) rt_vfpu(&c, (word), 0)
#define V(...) ((const float[]){ __VA_ARGS__ })
#define U(...) ((const uint32_t[]){ __VA_ARGS__ })

int rt_vfpu_selftest(void) {
    RtCtx c;
    jmp_buf jb;
    rt_abort_jmp = &jb;
    if (setjmp(jb)) { printf("  FAIL trapped: %s at word %08X\n", rt_last_abort.what, rt_last_abort.extra); return 1; }
    const uint32_t mem = 0x09000000u;

    /* memory and GPR transfers */
    rt_ctx_init(&c); c.r[5] = mem;
    for (int i = 0; i < 4; i++) { float f = (float)(i + 1); memcpy(rt_ptr(mem + 4u * (uint32_t)i), &f, 4); }
    X(0xd8a00000); expectv(&c, "lv.q C000,0(a1)", "C000", 4, V(1, 2, 3, 4));
    c.r[4] = mem + 0x40; setv(&c, "C000", 4, V(5, 6, 7, 8)); X(0xf8800002);
    { float f[4]; memcpy(f, rt_ptr(mem + 0x40), 16); expectu("sv.q C000,0(a0),wb", f[0] == 5 && f[1] == 6 && f[2] == 7 && f[3] == 8, 1); }
    c.r[4] = mem; X(0xc8820000); expectv(&c, "lv.s S020,0(a0)", "S020", 1, V(1));
    setv(&c, "S020", 1, V(9)); c.r[4] = mem + 0x80; X(0xe8820000);
    { float f; memcpy(&f, rt_ptr(mem + 0x80), 4); expectu("sv.s S020,0(a0)", f == 9.0f, 1); }
    setv(&c, "S000", 1, V(2.5f)); X(0x48640000); expectu("mfv a0,S000", c.r[4], 0x40200000u);
    c.r[4] = 0x3F800000u; X(0x48e40000); expectv(&c, "mtv a0,S000", "S000", 1, V(1));
    c.r[8] = 0xFF15; X(0x48e80083); expectu("mtvc t0,CC (masked)", c.vctrl[3], 0x15);

    /* arithmetic */
    rt_ctx_init(&c);
    setv(&c, "C120", 4, V(1, 2, 3, 4)); setv(&c, "C220", 4, V(10, 20, 30, 40));
    X(0x600a8686); expectv(&c, "vadd.q C120,C120,C220", "C120", 4, V(11, 22, 33, 44));
    setv(&c, "S113", 1, V(1.5f)); setv(&c, "S112", 1, V(2.25f)); X(0x60456506); expectv(&c, "vadd.s S120,S113,S112", "S120", 1, V(3.75f));
    setv(&c, "C000", 3, V(1, 2, 3)); setv(&c, "C010", 3, V(4, 5, 6)); X(0x60018009); expectv(&c, "vadd.t C210,C000,C010", "C210", 3, V(5, 7, 9));
    setv(&c, "C020", 4, V(5, 5, 5, 5)); setv(&c, "C120", 4, V(1, 2, 3, 4)); X(0x6086828a); expectv(&c, "vsub.q C220,C020,C120", "C220", 4, V(4, 3, 2, 1));
    setv(&c, "C000", 3, V(2, -3, 4)); X(0x64008001); expectv(&c, "vmul.t C010,C000,C000", "C010", 3, V(4, 9, 16));
    setv(&c, "C110", 4, V(1, 2, 3, 4)); setv(&c, "C010", 4, V(5, 6, 7, 8)); X(0x64818580); expectv(&c, "vdot.q S000,C110,C010", "S000", 1, V(70));
    setv(&c, "R100", 3, V(1, 2, 3)); setv(&c, "C200", 3, V(4, 5, 6)); X(0x6488a40c); expectv(&c, "vdot.t S300,R100,C200", "S300", 1, V(32));
    setv(&c, "C010", 3, V(1, 2, 3)); setv(&c, "S003", 1, V(2)); X(0x65608103); expectv(&c, "vscl.t C030,C010,S003", "C030", 3, V(2, 4, 6));
    setv(&c, "C300", 3, V(1, 5, 3)); setv(&c, "C310", 3, V(2, 4, 3));
    X(0x6d8d8c14); expectv(&c, "vmax.t C500,C300,C310", "C500", 3, V(2, 5, 3));
    X(0x6d0d8c18); expectv(&c, "vmin.t C600,C300,C310", "C600", 3, V(1, 4, 3));
    setv(&c, "R101", 3, V(1, 2, 3)); setv(&c, "R102", 3, V(4, 5, 6)); X(0xf2a6a508); expectv(&c, "vcrsp.t C200,R101,R102", "C200", 3, V(-3, 6, -3));
    setv(&c, "S700", 1, V(2)); X(0xd0021c5e); expectv(&c, "vneg.s S722,S700", "S722", 1, V(-2));
    setv(&c, "C000", 3, V(-1, 2, -3)); X(0xd0018018); expectv(&c, "vabs.t C600,C000", "C600", 3, V(1, 2, 3));
    setv(&c, "S002", 1, V(0.25f)); X(0xd0444040); expectv(&c, "vocp.s S002,S002", "S002", 1, V(0.75f));
    setv(&c, "S000", 1, V(-3)); X(0xd04a0020); expectv(&c, "vsgn.s S001,S000 (neg)", "S001", 1, V(-1));
    setv(&c, "S000", 1, V(0)); X(0xd04a0020); expectv(&c, "vsgn.s S001,S000 (zero)", "S001", 1, V(0));

    /* functions (angles in quarter turns) */
    setv(&c, "S000", 1, V(1)); X(0xd0120000); expectv(&c, "vsin.s S000 (1 -> 1)", "S000", 1, V(1));
    setv(&c, "S000", 1, V(0.5f)); X(0xd0120000); expectv(&c, "vsin.s S000 (0.5)", "S000", 1, V(0.70710677f));
    setv(&c, "S000", 1, V(2)); X(0xd0130000); expectv(&c, "vcos.s S000 (2 -> -1)", "S000", 1, V(-1));
    setv(&c, "S000", 1, V(4)); X(0xd0110020); expectv(&c, "vrsq.s S001,S000", "S001", 1, V(0.5f));
    setv(&c, "S002", 1, V(9)); X(0xd0164060); expectv(&c, "vsqrt.s S003,S002", "S003", 1, V(3));
    setv(&c, "S300", 1, V(4)); X(0xd0100c2c); expectv(&c, "vrcp.s S301,S300", "S301", 1, V(0.25f));
    setv(&c, "S003", 1, V(1)); X(0xd0176000); expectv(&c, "vasin.s S000,S003", "S000", 1, V(1));

    /* constants and moves */
    X(0xdfe04000); expectv(&c, "vfim.s S003,0x4000", "S003", 1, V(2));
    X(0xdf833b33); expectv(&c, "vfim.s S030,0x3b33", "S030", 1, V(0.89990234f));
    X(0xd00380a3); expectv(&c, "vidt.q R003", "R003", 4, V(0, 0, 0, 1));
    X(0xd0068000); expectv(&c, "vzero.t C000", "C000", 3, V(0, 0, 0));
    X(0xd00780a7); expectv(&c, "vone.q R103", "R103", 4, V(1, 1, 1, 1));
    setv(&c, "S333", 1, V(7)); X(0xd0006f23); expectv(&c, "vmov.s S031,S333", "S031", 1, V(7));
    setv(&c, "R201", 4, V(1, 2, 3, 4)); X(0xd000a9a8); expectv(&c, "vmov.q R200,R201", "R200", 4, V(1, 2, 3, 4));

    /* conversions */
    setv(&c, "C400", 4, V(1.0f, -1.5f, 0.25f, 1000.0f)); X(0xd2379090);
    expectbits(&c, "vf2iz.q C400,C400,23", "C400", 4, U(0x00800000u, (uint32_t)-12582912, 0x00200000u, 0x7FFFFFFFu));
    { uint32_t in[4] = { 0x7F000000u, 0x80000000u, 0x00800000u, 0x40000000u }; for (int i = 0; i < 4; i++) c.v[phys("C400", 4, 0, i)].i = in[i]; }
    X(0xd03c90d0); expectbits(&c, "vi2uc.q S402,C400", "S402", 1, U(0x800100FEu));
    { uint32_t in[4] = { 0x7F000000u, 0x80000000u, 0x01000000u, 0xFF000000u }; for (int i = 0; i < 4; i++) c.v[phys("C420", 4, 0, i)].i = in[i]; }
    X(0xd03d92b0); expectbits(&c, "vi2c.q S401,C420", "S401", 1, U(0xFF01807Fu));
    c.v[phys("C410", 2, 0, 0)].i = 0x7FFF0000u; c.v[phys("C410", 2, 0, 1)].i = (uint32_t)-5;
    X(0xd03e1190); expectbits(&c, "vi2us.p S400,C410", "S400", 1, U(0x0000FFFEu));

    /* compare, condition codes, conditional move */
    setv(&c, "C300", 3, V(1, 5, 3)); setv(&c, "C310", 3, V(2, 4, 3));
    /* lanes 0,2 true -> bits 0,2; any -> bit 4; not all -> bit 5 clear; bit 3 (unaffected) stays 1 from reset */
    X(0x6c0d8c03); expectu("vcmp.t LE: CC", c.vctrl[3] & 0x3F, 0x1D);
    setv(&c, "C200", 3, V(9, 9, 9)); X(0xd2a08c08); expectv(&c, "vcmovt.t C200,C300,CC[0]", "C200", 3, V(1, 5, 3));

    /* matrices */
    float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float b[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    setm(&c, "M100", 4, id); setm(&c, "M000", 4, b); X(0xf000a488);
    for (int j = 0; j < 4; j++) { char n[5] = { 'C', '2', (char)('0' + j), '0', 0 }; expectv(&c, "vmmul.q I*B", n, 4, &b[j * 4]); }
    setm(&c, "M100", 4, b); setm(&c, "M000", 4, id); X(0xf000a488);
    for (int j = 0; j < 4; j++) { char n[5] = { 'C', '2', (char)('0' + j), '0', 0 }; expectv(&c, "vmmul.q B*I (transposed vs)", n, 4, &b[j * 4]); }
    float m4[16] = { 1, 2, 3, 4, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    setm(&c, "M000", 4, m4); setv(&c, "C100", 4, V(1, 1, 1, 1)); X(0xf184808c);
    expectv(&c, "vtfm4.q C300,M000,C100", "C300", 4, V(10, 1, 1, 1));
    X(0xf383809c); for (int j = 0; j < 4; j++) { char n[5] = { 'C', '7', (char)('0' + j), '0', 0 }; expectv(&c, "vmidt.q M700", n, 4, &id[j * 4]); }
    setm(&c, "M200", 4, b); X(0xf380889c);
    for (int j = 0; j < 4; j++) { char n[5] = { 'C', '7', (char)('0' + j), '0', 0 }; expectv(&c, "vmmov.q M700,M200", n, 4, &b[j * 4]); }

    /* prefixes */
    rt_ctx_init(&c);
    setv(&c, "S002", 1, V(-9)); X(0xdc000124); X(0xd0164060); expectv(&c, "vpfxs [|X|]; vsqrt.s", "S003", 1, V(3));
    expectu("prefix eaten after use", c.vctrl[0], 0xE4);
    setv(&c, "C300", 2, V(3, 4)); X(0xdc0100e4); X(0xd0000c8d); expectv(&c, "vpfxs [-X,Y]; vmov.p", "C310", 2, V(-3, 4));
    setv(&c, "C000", 3, V(1, 2, 3)); X(0xdd0000c9); X(0x64008002); expectv(&c, "vpfxt [Y,Z,X]; vmul.t", "C020", 3, V(2, 6, 3));
    setv(&c, "C220", 4, V(1, 2, 3, 4)); setv(&c, "S333", 1, V(10)); X(0xde000800); X(0x656f8a8a);
    expectv(&c, "vpfxd [,,,M]; vscl.q (mask)", "C220", 4, V(10, 20, 30, 4));
    setv(&c, "S001", 1, V(5)); c.vctrl[3] = 1; X(0xdc001025); X(0xd2a02020); expectv(&c, "vpfxs [1]; vcmovt.s", "S001", 1, V(1));

    rt_abort_jmp = NULL;
    printf("VFPU self-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
