/* Debug features for automated testing (docs/phase11-level-tour.md). Chains of Olympus only.
 *
 *   --warp LEVEL[:SPAWN][@FRAME]
 *                         at the first controller read of displayed frame FRAME (default 1000,
 *                         the main menu after a boot with an empty ms0), starts a story game in
 *                         LEVEL (a file name in disc0:/PSP_GAME/USRDIR/data without ".bin").
 *                         It calls the game's own "start at level" function, the one the
 *                         Challenge of the Gods menu calls with "cog_01".."cog_05" (0x08976FE8):
 *                         new game state, load LEVEL, Kratos at the level's spawn point 0 (or
 *                         SPAWN: game + 0x120 picks one of the level's spawn points, +0x134/+0x148),
 *                         close the menu. The challenge flag it sets (game + 0x9C) is cleared
 *                         again, except for the challenge levels themselves (cog_*).
 *   --god                 keeps Kratos's health at its maximum (his health component, found
 *                         through the game object). Falls and instant kills still happen; they
 *                         are logged ("Kratos died").
 *   --level-log           logs the level manager's loaded levels (name and load state) and
 *                         the game's current level whenever they change.
 *   --load-level NAME@FRAME
 *                         at frame FRAME, asks the level manager to load NAME the way the game
 *                         streams a neighbouring level (LoadLevel(mgr, NAME, 1, 0), 0x088347B8),
 *                         e.g. after a --warp into the level next to it.
 *   --dump-ram N,..:PFX   writes user RAM (0x08800000, 24 MB) to PFX_<frame>.bin at frames N;
 *                         "stop" as a frame writes PFX_stop.bin when the run ends (a trap, too).
 *
 * The call runs on the reading thread (user_main) through rt_call_guest, which restores
 * every register afterwards, so the controller read itself is unaffected. Everything here
 * happens at fixed displayed frames, so runs stay deterministic. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_debug.h"
#include "rt_kernel.h"

#define COO_START_AT_LEVEL 0x08976FE8u  /* FUN_00172fe8(unused, const char *level) */
#define COO_LOAD_LEVEL     0x088347B8u  /* FUN_000307b8(mgr, const char *level, char, char) */
#define COO_GAME_PTR       0x08B3655Cu  /* the game object (FUN_0022bfb4) */
#define COO_LEVELMGR_PTR   0x08B3CB44u  /* the level manager (FUN_00282d1c) */
#define GAME_CHALLENGE     0x9Cu        /* 1 in Challenge of the Gods */
#define GAME_LEVEL_NAME    0xA0u        /* the level the player is in */
#define GAME_SPAWN_INDEX   0x120u       /* spawn point in that level (FUN_000c1008) */
#define MGR_LEVELS         0x24u        /* array of level pointers */
#define MGR_COUNT          0x38u
#define MGR_CURRENT        0x40u
#define LEVEL_NAME         0xB4u
#define LEVEL_STATE        0x12Cu       /* low byte: load state, 5 and up = loaded */
#define LEVEL_SPAWNS       0x148u       /* number of spawn points (0x1C bytes each at +0x134) */
#define GAME_HERO          0x84u        /* Kratos (FUN_000bdd18 spawns him) */
#define HERO_COMPONENTS    0x24u        /* component pointers (FUN_0003407c looks them up) */
#define HEALTH_VTABLE      0x08B3E248u  /* Kratos's health component: */
#define HEALTH_CUR         0x64u        /*   current health (float, 100 at the start) */
#define HEALTH_MAX         0x70u        /*   maximum */

static const char *g_warp;
static uint32_t g_warp_frame = 1000;
static int g_warp_spawn = -1;
static int g_warp_done, g_level_log, g_god;
static uint32_t g_god_refills, g_deaths;
static char g_last_levels[1024];
static const char *g_dump_frames, *g_dump_prefix;  /* --dump-ram N,N,...:prefix */
static uint64_t g_last_dump;
static char g_load_name[64];                         /* --load-level NAME@FRAME */
static uint32_t g_load_frame;
static int g_load_done;

static int is_coo(void) { return !strcmp(RT_GAME_ID, "UCUS98653"); }

int rt_debug_option(int argc, char **argv, int *i) {
    const char *a = argv[*i];
    if (!strcmp(a, "--warp") && *i + 1 < argc) {
        static char name[64];
        snprintf(name, sizeof(name), "%s", argv[++*i]);
        char *at = strchr(name, '@');
        if (at) { *at = 0; g_warp_frame = (uint32_t)strtoul(at + 1, NULL, 0); }
        char *colon = strchr(name, ':');
        if (colon) { *colon = 0; g_warp_spawn = atoi(colon + 1); }
        g_warp = name;
        g_level_log = 1;
        return 1;
    }
    if (!strcmp(a, "--level-log")) { g_level_log = 1; return 1; }
    if (!strcmp(a, "--god")) { g_god = 1; return 1; }
    if (!strcmp(a, "--load-level") && *i + 1 < argc) {
        snprintf(g_load_name, sizeof(g_load_name), "%s", argv[++*i]);
        char *at = strchr(g_load_name, '@');
        if (!at) { fprintf(stderr, "--load-level wants NAME@FRAME\n"); return 0; }
        *at = 0;
        g_load_frame = (uint32_t)strtoul(at + 1, NULL, 0);
        g_level_log = 1;
        return 1;
    }
    if (!strcmp(a, "--dump-ram") && *i + 1 < argc) {
        static char spec[512];
        snprintf(spec, sizeof(spec), "%s", argv[++*i]);
        char *colon = strrchr(spec, ':');
        if (!colon || colon == spec + 1) { fprintf(stderr, "--dump-ram wants FRAME,FRAME,...:prefix\n"); return 0; }
        *colon = 0;
        g_dump_frames = spec;
        g_dump_prefix = colon + 1;
        return 1;
    }
    return 0;
}

static void write_ram(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (fp) { fwrite(rt_ptr(0x08800000u), 1, 0x01800000u, fp); fclose(fp); }
    rt_log("[debug] RAM dump %s%s\n", path, fp ? "" : " failed");
}

/* --dump-ram: user RAM (0x08800000-0x0A000000) at the first controller read of each frame. */
static void dump_ram(uint64_t frame) {
    if (frame == g_last_dump) return;
    for (const char *p = g_dump_frames; *p;) {
        char *end;
        uint64_t f = strtoull(p, &end, 0);
        if (end != p && f == frame) {
            char path[600];
            snprintf(path, sizeof(path), "%s_%06llu.bin", g_dump_prefix, (unsigned long long)frame);
            write_ram(path);
            g_last_dump = frame;
            return;
        }
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
}

static const char *guest_str(uint32_t addr, char *buf, size_t n) {
    size_t i = 0;
    for (; i + 1 < n && rt_r8(addr + (uint32_t)i); i++) buf[i] = (char)rt_r8(addr + (uint32_t)i);
    buf[i] = 0;
    return buf;
}

static void log_levels(void) {
    uint32_t mgr = rt_r32(COO_LEVELMGR_PTR), game = rt_r32(COO_GAME_PTR);
    if (!mgr) return;
    char s[1024], name[64];
    int n = 0;
    uint32_t count = rt_r32(mgr + MGR_COUNT), arr = rt_r32(mgr + MGR_LEVELS), cur = rt_r32(mgr + MGR_CURRENT);
    for (uint32_t k = 0; k < count && k < 16 && n < (int)sizeof(s) - 100; k++) {
        uint32_t lvl = rt_r32(arr + 4 * k);
        if (!lvl) continue;
        n += snprintf(s + n, sizeof(s) - n, " %s%s(%u)", lvl == cur ? "*" : "", guest_str(lvl + LEVEL_NAME, name, sizeof(name)),
                      rt_r8(lvl + LEVEL_STATE));
    }
    if (game)
        n += snprintf(s + n, sizeof(s) - n, " | game: %s spawn %u of %u%s", guest_str(game + GAME_LEVEL_NAME, name, sizeof(name)),
                      rt_r32(game + GAME_SPAWN_INDEX), cur ? rt_r32(cur + LEVEL_SPAWNS) : 0,
                      rt_r32(game + GAME_CHALLENGE) ? " challenge" : "");
    if (strcmp(s, g_last_levels)) {
        memcpy(g_last_levels, s, sizeof(s));
        rt_log("[level] loaded:%s\n", s);
    }
}

static void warp(void) {
    g_warp_done = 1;
    if (!is_coo()) { rt_log("[debug] --warp is only known for UCUS98653; ignored\n"); return; }
    uint32_t game = rt_r32(COO_GAME_PTR);
    if (!game || !k_cur) { rt_log("[debug] --warp: the game object does not exist yet\n"); return; }
    size_t len = strlen(g_warp) + 1;
    uint32_t name = k_alloc((uint32_t)len, 1, 16);
    if (!name) { rt_log("[debug] --warp: no guest memory for the level name\n"); return; }
    memcpy(rt_ptr(name), g_warp, len);
    rt_log("[debug] warp to %s (thread %s)\n", g_warp, k_cur->name);
    uint32_t args[2] = { 0, name };
    rt_call_guest(&k_cur->ctx, COO_START_AT_LEVEL, 2, args);
    if (strncmp(g_warp, "cog_", 4)) rt_w32(game + GAME_CHALLENGE, 0);  /* story mode, except the challenges */
    /* the level loads later (the call only queues it), and Kratos is placed at this spawn point then */
    if (g_warp_spawn >= 0) rt_w32(game + GAME_SPAWN_INDEX, (uint32_t)g_warp_spawn);
}

static void load_level(void) {
    g_load_done = 1;
    uint32_t mgr = rt_r32(COO_LEVELMGR_PTR);
    if (!is_coo() || !mgr || !k_cur) { rt_log("[debug] --load-level: no level manager yet\n"); return; }
    size_t len = strlen(g_load_name) + 1;
    uint32_t name = k_alloc((uint32_t)len, 1, 16);
    if (!name) { rt_log("[debug] --load-level: no guest memory for the level name\n"); return; }
    memcpy(rt_ptr(name), g_load_name, len);
    rt_log("[debug] load level %s (deferred flag %u)\n", g_load_name, rt_r8(0x08B2F268u));
    uint32_t args[4] = { mgr, name, 1, 0 };
    rt_call_guest(&k_cur->ctx, COO_LOAD_LEVEL, 4, args);
}

static int ram_ptr(uint32_t p) { return p >= 0x08800000u && p < 0x0A000000u - 0x100u; }
static float rt_rf(uint32_t a) { uint32_t v = rt_r32(a); float f; memcpy(&f, &v, 4); return f; }

/* --god: Kratos's health back to its maximum at every controller read. */
static void god(void) {
    uint32_t game = rt_r32(COO_GAME_PTR), hero = ram_ptr(game) ? rt_r32(game + GAME_HERO) : 0;
    uint32_t comps = ram_ptr(hero) ? rt_r32(hero + HERO_COMPONENTS) : 0;
    if (!ram_ptr(comps)) return;
    for (uint32_t k = 0; k < 64; k++) {
        uint32_t c = rt_r32(comps + 4 * k);
        if (!ram_ptr(c) || rt_r32(c + 0xC) != HEALTH_VTABLE) continue;
        float cur = rt_rf(c + HEALTH_CUR), max = rt_rf(c + HEALTH_MAX);
        if (cur <= 0.0f && max > 0.0f && !g_deaths++) rt_log("[debug] Kratos died (health %.1f) despite --god\n", cur);
        if (max > 0.0f && cur < max) {
            memcpy(rt_ptr(c + HEALTH_CUR), &max, 4);
            if (!g_god_refills++) rt_log("[debug] god mode: health %.1f -> %.1f (component %08X)\n", cur, max, c);
        }
        return;
    }
}

void rt_debug_input(uint64_t frame) {
    if (g_god && is_coo()) god();
    if (g_warp && !g_warp_done && frame >= g_warp_frame) warp();
    if (g_load_name[0] && !g_load_done && frame >= g_load_frame) load_level();
    if (g_level_log && is_coo()) log_levels();
    if (g_dump_frames) dump_ram(frame);
}

void rt_debug_report(void) {
    if (g_dump_frames && strstr(g_dump_frames, "stop")) {
        char path[600];
        snprintf(path, sizeof(path), "%s_stop.bin", g_dump_prefix);
        write_ram(path);
    }
    if (g_warp && !g_warp_done) printf("[debug] the warp to %s never ran (frame %u not reached)\n", g_warp, g_warp_frame);
    if (g_god) printf("[debug] god mode refilled Kratos's health %u times; health reached 0 %u times\n", g_god_refills, g_deaths);
}
