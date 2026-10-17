/* PSP GE display-list interpreter (control flow, state, matrices, CLUT) plus statistics.
 * Command semantics follow PPSSPP (GPU/GPUCommon.cpp, GPU/ge_constants.h, v1.20.4).
 * Drawing is done by rt_gesoft.c. */
#include <stdio.h>
#include <string.h>

#include "rt_ge_internal.h"
#include "rt_frames.h"

Ge ge;
GeStats ge_stats;
int rt_ge_skip_draw;
int ge_setup_dirty = 1;

/* Commands that never change what ge_draw_prim derives per draw: control flow, addresses, and
 * state the renderer reads per vertex or per use (bones, morph weights, texgen matrix,
 * transfers, CLUT loads). Any other command that writes a new value marks the setup dirty. */
static uint8_t g_setup_neutral[256];
static void init_setup_neutral(void) {
    static const uint8_t cmds[] = {
        GE_CMD_NOP, GE_CMD_VADDR, GE_CMD_IADDR, GE_CMD_PRIM, GE_CMD_BEZIER, GE_CMD_SPLINE, GE_CMD_BOUNDINGBOX,
        GE_CMD_JUMP, GE_CMD_BJUMP, GE_CMD_CALL, GE_CMD_RET, GE_CMD_END, GE_CMD_SIGNAL, GE_CMD_FINISH, GE_CMD_BASE,
        GE_CMD_OFFSETADDR, GE_CMD_ORIGIN, GE_CMD_BONEMATRIXNUMBER, GE_CMD_BONEMATRIXDATA, GE_CMD_TGENMATRIXNUMBER,
        GE_CMD_TGENMATRIXDATA, GE_CMD_WORLDMATRIXNUMBER, GE_CMD_VIEWMATRIXNUMBER, GE_CMD_PROJMATRIXNUMBER,
        GE_CMD_CLUTADDR, GE_CMD_CLUTADDRUPPER, GE_CMD_LOADCLUT, GE_CMD_TRANSFERSRC, GE_CMD_TRANSFERSRCW,
        GE_CMD_TRANSFERDST, GE_CMD_TRANSFERDSTW, GE_CMD_TRANSFERSTART, GE_CMD_TRANSFERSRCPOS, GE_CMD_TRANSFERDSTPOS,
        GE_CMD_TRANSFERSIZE, GE_CMD_TEXFLUSH, GE_CMD_TEXSYNC,
    };
    for (size_t i = 0; i < sizeof(cmds); i++) g_setup_neutral[cmds[i]] = 1;
    for (int i = 0; i < 8; i++) g_setup_neutral[GE_CMD_MORPHWEIGHT0 + i] = 1;
}

static float ge_float(uint32_t op) { uint32_t b = op << 8; float f; memcpy(&f, &b, 4); return f; }

uint32_t ge_rel(uint32_t data) {
    uint32_t ext = ((ge.cmd[GE_CMD_BASE] & 0x000F0000u) << 8) | data;
    return (ge.offset + ext) & 0x0FFFFFFFu;
}

static void count_u32(GeCount *t, int n, uint32_t key) {
    for (int i = 0; i < n; i++) {
        if (t[i].count && t[i].key == key) { t[i].count++; return; }
        if (!t[i].count) { t[i].key = key; t[i].count = 1; return; }
    }
}

static void note_prim(uint32_t op) {
    uint32_t vt = ge.cmd[GE_CMD_VERTEXTYPE] & 0xFFFFFF, prim = (op >> 16) & 7;
    ge_stats.prims++;
    ge_stats.prim_type[prim]++;
    ge_stats.vertices += op & 0xFFFF;
    count_u32(ge_stats.vtypes, GE_STAT_SLOTS, vt);
    if (vt & (1u << 23)) ge_stats.through++;
    if (ge.cmd[GE_CMD_CLEARMODE] & 1) ge_stats.clears++;
    if (ge.cmd[GE_CMD_LIGHTINGENABLE] & 1 && !(vt & (1u << 23))) ge_stats.lit++;
    if ((vt >> 9) & 3) ge_stats.skinned++;
    if (((vt >> 18) & 7) > 0) ge_stats.morphed++;
    if (ge.cmd[GE_CMD_TEXTUREMAPENABLE] & 1) {
        uint32_t tf = ge.cmd[GE_CMD_TEXFORMAT] & 0xF, sw = ge.cmd[GE_CMD_TEXMODE] & 1;
        count_u32(ge_stats.texfmts, GE_STAT_SLOTS, tf | (sw << 8) | ((ge.cmd[GE_CMD_CLUTFORMAT] & 3) << 12));
    }
    if (ge.cmd[GE_CMD_ALPHABLENDENABLE] & 1) count_u32(ge_stats.blends, GE_STAT_SLOTS, ge.cmd[GE_CMD_BLENDMODE] & 0xFFFFFF);
    count_u32(ge_stats.fbfmts, GE_STAT_SLOTS, (ge.cmd[GE_CMD_FRAMEBUFPIXFORMAT] & 3) | ((ge.cmd[GE_CMD_FRAMEBUFPTR] & 0x1FFFF0) << 4));
}

static void load_clut(uint32_t op) {
    uint32_t addr = ((ge.cmd[GE_CMD_CLUTADDRUPPER] & 0x0F0000) << 8) | (ge.cmd[GE_CMD_CLUTADDR] & 0xFFFFF0);
    uint32_t bytes = (op & 0x3F) * 32;
    if (bytes > sizeof(ge.clut)) bytes = sizeof(ge.clut);
    ge_sync_read(addr, bytes, FLUSH_CLUT);  /* the palette may be drawing still queued */
    memcpy(ge.clut, rt_ptr(addr), bytes);
    ge_clut_changed();
}

static struct { uint32_t pc, op, to; } g_hist[64];
static unsigned g_nhist;
static void hist(uint32_t pc, uint32_t op, uint32_t to) { unsigned i = g_nhist++ & 63; g_hist[i].pc = pc; g_hist[i].op = op; g_hist[i].to = to; }

static void run_list(uint32_t list, uint32_t stall) {
    uint32_t pc = list & 0x0FFFFFFFu, stack[32], stack_off[32], prev = 0;
    g_nhist = 0;
    int sp = 0;
    stall &= 0x0FFFFFFFu;
    ge.offset = 0;
    ge_stats.lists++;
    if (!g_setup_neutral[GE_CMD_NOP]) init_setup_neutral();
    ge_setup_dirty = 1;
    for (uint32_t steps = 0; steps < 4000000; steps++) {
        if (stall && pc == stall) { ge_stats.stalls++; return; }
        uint32_t op = rt_r32(pc), cmd = op >> 24, data = op & 0xFFFFFF;
        ge_stats.cmds[cmd]++;
        switch (cmd) {
        case GE_CMD_JUMP: hist(pc, op, ge_rel(data & 0xFFFFFC)); pc = ge_rel(data & 0xFFFFFC); prev = op; continue;
        case GE_CMD_BJUMP: if (ge.bbox_false) { pc = ge_rel(data & 0xFFFFFC); prev = op; continue; } break;
        case GE_CMD_CALL:
            if (sp < 32) { stack[sp] = pc + 4; stack_off[sp] = ge.offset; sp++; }
            hist(pc, op, ge_rel(data & 0xFFFFFC));
            pc = ge_rel(data & 0xFFFFFC);
            prev = op;
            continue;
        case GE_CMD_RET:
            if (sp > 0) { sp--; ge.offset = stack_off[sp]; hist(pc, op, stack[sp]); pc = stack[sp] & 0x0FFFFFFFu; prev = op; continue; }
            break;
        case GE_CMD_END:
            if ((prev >> 24) == GE_CMD_SIGNAL) ge_stats.signals++;
            return;  /* FINISH+END (or a bare END) completes the list */
        case GE_CMD_ORIGIN: ge.offset = pc; hist(pc, op, pc); break;
        case GE_CMD_OFFSETADDR: ge.offset = data << 8; hist(pc, op, ge.offset); break;
        case GE_CMD_VADDR: ge.vaddr = ge_rel(data); break;
        case GE_CMD_IADDR: ge.iaddr = ge_rel(data); break;
        case GE_CMD_PRIM: note_prim(op); ge_draw_prim(op); break;
        case GE_CMD_BEZIER: case GE_CMD_SPLINE: ge_stats.patches++; break;
        case GE_CMD_BOUNDINGBOX: ge.bbox_false = ge_bbox(op) == 0; break;
        case GE_CMD_LOADCLUT: load_clut(op); break;
        case GE_CMD_TRANSFERSTART: ge_stats.transfers++; ge_block_transfer(op); break;
        case GE_CMD_WORLDMATRIXNUMBER: ge.world_n = data & 0xF; break;
        case GE_CMD_WORLDMATRIXDATA: if (ge.world_n < 12) ge.world[ge.world_n++] = ge_float(op); break;
        case GE_CMD_VIEWMATRIXNUMBER: ge.view_n = data & 0xF; break;
        case GE_CMD_VIEWMATRIXDATA: if (ge.view_n < 12) ge.view[ge.view_n++] = ge_float(op); break;
        case GE_CMD_PROJMATRIXNUMBER: ge.proj_n = data & 0xF; break;
        case GE_CMD_PROJMATRIXDATA: if (ge.proj_n < 16) ge.proj[ge.proj_n++] = ge_float(op); break;
        case GE_CMD_TGENMATRIXNUMBER: ge.tgen_n = data & 0xF; break;
        case GE_CMD_TGENMATRIXDATA: if (ge.tgen_n < 12) ge.tgen[ge.tgen_n++] = ge_float(op); break;
        case GE_CMD_BONEMATRIXNUMBER: ge.bone_n = data & 0x7F; break;
        case GE_CMD_BONEMATRIXDATA: if (ge.bone_n < 96) ge.bone[ge.bone_n++] = ge_float(op); break;
        default:
            if (cmd >= GE_CMD_MORPHWEIGHT0 && cmd <= GE_CMD_MORPHWEIGHT0 + 7) ge.morph[cmd - GE_CMD_MORPHWEIGHT0] = ge_float(op);
            break;
        }
        if (!g_setup_neutral[cmd] && (op != ge.cmd[cmd] || cmd == GE_CMD_WORLDMATRIXDATA || cmd == GE_CMD_VIEWMATRIXDATA ||
                                      cmd == GE_CMD_PROJMATRIXDATA))
            ge_setup_dirty = 1;
        ge.cmd[cmd] = op;
        prev = op;
        pc += 4;
    }
    if (ge_stats.runaway++ == 0) {  /* describe the first runaway list once */
        printf("[ge] list %08X did not end; trace from start:\n", list);
        uint32_t p = list & 0x0FFFFFFFu;
        for (int i = 0; i < 24; i++, p += 4) printf("  %08X: %08X\n", p, rt_r32(p));
        printf("  ... ended scanning at pc %08X (op %08X); last control-flow events:\n", pc, rt_r32(pc));
        for (unsigned i = g_nhist > 20 ? g_nhist - 20 : 0; i < g_nhist; i++)
            printf("    at %08X op %08X -> %08X\n", g_hist[i & 63].pc, g_hist[i & 63].op, g_hist[i & 63].to);
    }
}

/* Lists run to completion when enqueued; queued drawing is finished before returning. */
void rt_ge_run(uint32_t list, uint32_t stall) {
    uint64_t t0 = rt_frames_host_us();
    run_list(list, stall);
    ge_flush(FLUSH_LIST_END);
    rt_perf_us[RT_PERF_GE] += rt_frames_host_us() - t0;
}

static const char *prim_names[8] = { "points", "lines", "line strip", "triangles", "tri strip", "tri fan", "sprites", "keep" };

static void print_table(const char *title, const GeCount *t, int n, const char *fmt) {
    printf("  %s:", title);
    for (int i = 0; i < n && t[i].count; i++) { printf(" "); printf(fmt, t[i].key); printf("x%u", t[i].count); }
    printf("\n");
}

void rt_ge_report(void) {
    printf("[ge] %u lists, %u prims (%llu vertices), %u through-mode, %u clears, %u lit, %u skinned, %u morphed, %u patches, %u transfers, %u stalls, %u runaway\n",
           ge_stats.lists, ge_stats.prims, (unsigned long long)ge_stats.vertices, ge_stats.through, ge_stats.clears, ge_stats.lit,
           ge_stats.skinned, ge_stats.morphed, ge_stats.patches, ge_stats.transfers, ge_stats.stalls, ge_stats.runaway);
    printf("  renderer: %llu vertices transformed, %llu draws reused the previous setup, %llu triangles queued, %llu pixels shaded, "
           "%llu written; %llu flushes of %llu queued primitives\n",
           (unsigned long long)ge_stats.verts_xformed, (unsigned long long)ge_stats.setups_reused, (unsigned long long)ge_stats.tris_raster,
           (unsigned long long)ge_stats.px_shaded, (unsigned long long)ge_stats.px_written,
           (unsigned long long)ge_stats.flushes, (unsigned long long)ge_stats.jobs);
    printf("  flushes by reason: list end %llu, texture %llu, target %llu, transfer %llu, clut %llu, trace %llu\n",
           (unsigned long long)ge_stats.flush_why[FLUSH_LIST_END], (unsigned long long)ge_stats.flush_why[FLUSH_TEXTURE],
           (unsigned long long)ge_stats.flush_why[FLUSH_TARGET],
           (unsigned long long)ge_stats.flush_why[FLUSH_TRANSFER], (unsigned long long)ge_stats.flush_why[FLUSH_CLUT],
           (unsigned long long)ge_stats.flush_why[FLUSH_TRACE]);
    ge_soft_report();
    printf("  primitives:");
    for (int i = 0; i < 8; i++) if (ge_stats.prim_type[i]) printf(" %s=%u", prim_names[i], ge_stats.prim_type[i]);
    printf("\n");
    print_table("vertex types", ge_stats.vtypes, GE_STAT_SLOTS, "%06X");
    print_table("texture fmt|swz<<8|clutfmt<<12", ge_stats.texfmts, GE_STAT_SLOTS, "%04X");
    print_table("blend modes", ge_stats.blends, GE_STAT_SLOTS, "%06X");
    print_table("framebuffer fmt|addr<<4", ge_stats.fbfmts, GE_STAT_SLOTS, "%07X");
    printf("  commands used:");
    for (int i = 0; i < 256; i++) if (ge_stats.cmds[i]) printf(" %02X", i);
    printf("\n");
}
