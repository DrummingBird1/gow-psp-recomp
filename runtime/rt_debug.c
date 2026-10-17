/* Debug features for automated testing (docs/phase11-level-tour.md, docs/phase12-gos-level-tour.md).
 * Chains of Olympus (UCUS98653) and Ghost of Sparta (UCUS98737); each game has its own addresses
 * (Game below), and in any other build the options only log that they are not known.
 *
 *   --warp LEVEL[:SPAWN][@FRAME]
 *                         starts a story game in LEVEL (a file name in disc0:/PSP_GAME/USRDIR/data
 *                         without ".bin", lower case), from the main menu, at the first controller
 *                         read of displayed frame FRAME (default 1000, the main menu after a boot
 *                         with an empty ms0), with Kratos at the level's spawn point 0 or SPAWN.
 *                         CoO: calls the game's own "start at level" function, the one the Challenge
 *                         of the Gods menu calls with "cog_01".."cog_05" (0x08976FE8): new game state,
 *                         load LEVEL, spawn point game + 0x120, close the menu. The challenge flag it
 *                         sets (game + 0x9C) is cleared again, except for the challenge levels (cog_*).
 *                         GoS: goes through the main menu's own New Game code. LEVEL is written into
 *                         the game's "first level" setting (a char[64] holding "att_010_stern"), the
 *                         menu preloads it (0x08A19BD0) and is put in its "start the game" state (5),
 *                         as the New Game event does; once LEVEL is loaded, the menu's exit code
 *                         (0x08A1A250) starts the game there. SPAWN goes to game + 0x130 before the
 *                         start, which places Kratos there; most levels set it back to 0 as they
 *                         start (and --warp-via's level change always does), and then Kratos is
 *                         removed (0x088E785C) and placed again at SPAWN (0x088E81E4), as the
 *                         game's own level change does.
 *   --warp-via LEVEL      GoS: the game starts in this LEVEL first, then changes to the --warp level
 *                         through its own hard level change (the one Load Game takes): every level is
 *                         unloaded before the new one is read. For levels too big to be loaded next to
 *                         the main menu's start_screen (cog_020_gauntlet, 7.6 MB: the level pool's
 *                         allocator stops at a break).
 *   --god                keeps Kratos's health at its maximum (his health component, found
 *                         through the game object). Falls and instant kills still happen; they
 *                         are logged ("Kratos died").
 *   --level-log           logs the level manager's loaded levels (name and load state) and
 *                         the game's current level whenever they change.
 *   --load-level NAME@FRAME
 *                         at frame FRAME, asks the level manager to load NAME the way the game
 *                         streams a neighbouring level (LoadLevel(mgr, NAME, 1, 0)), e.g. after a
 *                         --warp into the level next to it.
 *   --dump-ram N,..:PFX   writes user RAM (0x08800000, 24 MB) to PFX_<frame>.bin at frames N;
 *                         "stop" as a frame writes PFX_stop.bin when the run ends (a trap, too).
 *
 * The calls run on the reading thread (user_main) through rt_call_guest, which restores
 * every register afterwards, so the controller read itself is unaffected. Everything here
 * happens at fixed displayed frames, so runs stay deterministic. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_debug.h"
#include "rt_kernel.h"

/* The two games' level managers and game objects have the same layout, at other offsets. */
typedef struct {
    const char *id;
    uint32_t game_ptr;      /* the game object */
    uint32_t mgr_ptr;       /* the level manager: +0x24 level pointers, +0x38 count, +0x40 current */
    uint32_t load_level;    /* LoadLevel(mgr, const char *level, char, char) */
    uint32_t defer_flag;    /* byte: 1 while the game updates (level loads are deferred) */
    uint32_t level_state;   /* level: low byte = load state, 5 and up = loaded */
    uint32_t level_spawns;  /* level: number of spawn points */
    uint32_t game_level;    /* game: name of the level Kratos is in */
    uint32_t game_spawn;    /* game: spawn point in that level */
    uint32_t game_mode;     /* game: CoO 1 in Challenge of the Gods; GoS 0 story, 1-4 the other modes */
    uint32_t game_hero;     /* game: Kratos */
    uint32_t health_vtable; /* Kratos's health component (in hero + 0x24's list): vtable at +0xC */
    uint32_t health_cur, health_max;  /* floats in it */
} Game;

static const Game COO = {
    "UCUS98653",
    0x08B3655Cu,  /* FUN_0022bfb4 */
    0x08B3CB44u,  /* FUN_00282d1c */
    0x088347B8u,  /* FUN_000307b8 */
    0x08B2F268u,
    0x12Cu, 0x148u,  /* spawn points: 0x1C bytes each at +0x134 */
    0xA0u, 0x120u, 0x9Cu,
    0x84u,        /* FUN_000bdd18 spawns him */
    0x08B3E248u, 0x64u, 0x70u,
};

static const Game GOS = {
    "UCUS98737",
    0x08C17FACu,  /* FUN_002feeac */
    0x08C20378u,  /* FUN_00374988 */
    0x088294FCu,  /* FUN_000254fc */
    0x08C0F600u,
    0x1B4u, 0x1F4u,  /* spawn points: 0x1C bytes each at +0x1E0 (FUN_000e81ec) */
    0xB0u, 0x130u, 0xACu,
    0x80u,        /* FUN_000e41e4 places him */
    0x08C21C08u, 0x64u, 0x70u,  /* found in RAM dumps: 100 / 100 at the start, as in CoO */
};

#define COO_START_AT_LEVEL 0x08976FE8u  /* FUN_00172fe8(unused, const char *level) */

#define GOS_FIRST_LEVEL    0x08C061B0u  /* char[64] "att_010_stern" (FUN_002165b8 returns it) */
#define GOS_MENU_VTABLE    0x08C37350u  /* the main menu screen: +0x14 OnEnter, +0x1C OnExit, +0x4C Update */
#define GOS_MENU_STATE     0x7Cu        /* 2 idle, 5 starting the game once the level is loaded, 6 started */
#define GOS_MENU_PRELOAD   0x08A19BD0u  /* FUN_00215bd0(menu, const char *level) */
#define GOS_PLACE_KRATOS   0x088E81E4u  /* FUN_000e41e4(game, level, 0): at spawn point game + 0x130 */
#define GOS_REMOVE_KRATOS  0x088E785Cu  /* FUN_000e385c(game) */
#define GOS_DIFFICULTY     0x08C0FB24u  /* the New Game menu's choice */
#define GOS_FLAGS          0x6Cu        /* game: 0x40 a level change is pending, 0x80 a hard one (unload everything) */
#define GOS_NEXT_LEVEL     0x134u       /* game: char[64], the pending level change's level */

static const Game *g;  /* this build's game, or NULL */

static const char *g_warp, *g_warp_via;
static uint32_t g_warp_frame = 1000;
static int g_warp_spawn = -1;
static int g_warp_done, g_level_log, g_god;
static int g_gos_stage;           /* GoS warp: 1 waiting for the start, 4 for --warp-via's level change,
                                     2 placing Kratos at SPAWN, 3 done */
static uint32_t g_gos_menu;
static uint32_t g_god_refills, g_deaths;
static char g_last_levels[1024];
static const char *g_dump_frames, *g_dump_prefix;  /* --dump-ram N,N,...:prefix */
static uint64_t g_last_dump;
static char g_load_name[64];                         /* --load-level NAME@FRAME */
static uint32_t g_load_frame;
static int g_load_done;

static const Game *game_info(void) {
    if (!strcmp(RT_GAME_ID, COO.id)) return &COO;
    if (!strcmp(RT_GAME_ID, GOS.id)) return &GOS;
    return NULL;
}
static int is_coo(void) { return g == &COO; }

int rt_debug_option(int argc, char **argv, int *i) {
    const char *a = argv[*i];
    g = game_info();
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
    if (!strcmp(a, "--warp-via") && *i + 1 < argc) { g_warp_via = argv[++*i]; return 1; }
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

static int ram_ptr(uint32_t p) { return p >= 0x08800000u && p < 0x0A000000u - 0x100u; }
static float rt_rf(uint32_t a) { uint32_t v = rt_r32(a); float f; memcpy(&f, &v, 4); return f; }

static void log_levels(void) {
    uint32_t mgr = rt_r32(g->mgr_ptr), game = rt_r32(g->game_ptr);
    if (!ram_ptr(mgr)) return;
    char s[1024], name[64];
    int n = 0;
    uint32_t count = rt_r32(mgr + 0x38), arr = rt_r32(mgr + 0x24), cur = rt_r32(mgr + 0x40);
    for (uint32_t k = 0; k < count && k < 16 && n < (int)sizeof(s) - 100; k++) {
        uint32_t lvl = rt_r32(arr + 4 * k);
        if (!lvl) continue;
        n += snprintf(s + n, sizeof(s) - n, " %s%s(%u)", lvl == cur ? "*" : "", guest_str(lvl + 0xB4, name, sizeof(name)),
                      rt_r8(lvl + g->level_state));
    }
    if (ram_ptr(game)) {
        uint32_t mode = rt_r32(game + g->game_mode);
        n += snprintf(s + n, sizeof(s) - n, " | game: %s spawn %u of %u", guest_str(game + g->game_level, name, sizeof(name)),
                      rt_r32(game + g->game_spawn), cur ? rt_r32(cur + g->level_spawns) : 0);
        if (mode && is_coo()) n += snprintf(s + n, sizeof(s) - n, " challenge");
        else if (mode) n += snprintf(s + n, sizeof(s) - n, " mode %u", mode);
    }
    if (strcmp(s, g_last_levels)) {
        memcpy(g_last_levels, s, sizeof(s));
        rt_log("[level] loaded:%s\n", s);
    }
}

static uint32_t guest_name(const char *s) {
    size_t len = strlen(s) + 1;
    uint32_t a = k_alloc((uint32_t)len, 1, 16);
    if (a) memcpy(rt_ptr(a), s, len);
    return a;
}

static void warp_coo(void) {
    g_warp_done = 1;
    uint32_t game = rt_r32(COO.game_ptr);
    if (!game || !k_cur) { rt_log("[debug] --warp: the game object does not exist yet\n"); return; }
    uint32_t name = guest_name(g_warp);
    if (!name) { rt_log("[debug] --warp: no guest memory for the level name\n"); return; }
    rt_log("[debug] warp to %s (thread %s)\n", g_warp, k_cur->name);
    uint32_t args[2] = { 0, name };
    rt_call_guest(&k_cur->ctx, COO_START_AT_LEVEL, 2, args);
    if (strncmp(g_warp, "cog_", 4)) rt_w32(game + COO.game_mode, 0);  /* story mode, except the challenges */
    /* the level loads later (the call only queues it), and Kratos is placed at this spawn point then */
    if (g_warp_spawn >= 0) rt_w32(game + COO.game_spawn, (uint32_t)g_warp_spawn);
}

/* GoS: the main menu screen object (its vtable at +0xC), found in user RAM. */
static uint32_t gos_find_menu(void) {
    const uint32_t *p = (const uint32_t *)rt_ptr(0x08800000u);
    for (uint32_t k = 3; k < 0x01800000u / 4; k++)
        if (p[k] == GOS_MENU_VTABLE) return 0x08800000u + 4 * (k - 3);
    return 0;
}

static uint32_t gos_find_level(const char *want) {
    uint32_t mgr = rt_r32(GOS.mgr_ptr);
    if (!ram_ptr(mgr)) return 0;
    char name[64];
    for (uint32_t k = 0, n = rt_r32(mgr + 0x38); k < n && k < 16; k++) {
        uint32_t lvl = rt_r32(rt_r32(mgr + 0x24) + 4 * k);
        if (ram_ptr(lvl) && !strcmp(guest_str(lvl + 0xB4, name, sizeof(name)), want)) return lvl;
    }
    return 0;
}

/* GoS: LEVEL is loaded, current, and the level the game says Kratos is in. */
static int gos_in_level(uint32_t game, const char *want) {
    char name[64];
    uint32_t mgr = rt_r32(GOS.mgr_ptr), lvl = gos_find_level(want);
    return lvl && rt_r32(mgr + 0x40) == lvl && !strcmp(guest_str(game + GOS.game_level, name, sizeof(name)), want);
}

/* GoS --warp, a few steps over several frames: the main menu must be idle (state 2) first, then the
 * menu's own code loads and starts the level; the spawn point is applied once Kratos is in it. */
static void warp_gos(uint64_t frame) {
    uint32_t game = rt_r32(GOS.game_ptr);
    const char *first = g_warp_via ? g_warp_via : g_warp;
    if (g_gos_stage == 0) {
        static uint64_t scanned;
        if (!g_gos_menu && frame != scanned) { scanned = frame; g_gos_menu = gos_find_menu(); }
        if (!g_gos_menu || !ram_ptr(game) || !k_cur || rt_r32(g_gos_menu + GOS_MENU_STATE) != 2) return;
        if (strlen(g_warp) >= 64 || strlen(first) >= 64) { rt_log("[debug] --warp: level name too long\n"); g_warp_done = 1; return; }
        rt_log("[debug] warp to %s%s%s (frame %llu, thread %s, menu %08X, difficulty %u)\n", g_warp,
               g_warp_via ? " via " : "", g_warp_via ? g_warp_via : "", (unsigned long long)frame, k_cur->name, g_gos_menu,
               rt_r32(GOS_DIFFICULTY));
        memset(rt_ptr(GOS_FIRST_LEVEL), 0, 64);
        memcpy(rt_ptr(GOS_FIRST_LEVEL), first, strlen(first));
        uint32_t args[2] = { g_gos_menu, GOS_FIRST_LEVEL };
        rt_call_guest(&k_cur->ctx, GOS_MENU_PRELOAD, 2, args);
        rt_w32(g_gos_menu + GOS_MENU_STATE, 5);
        /* the start places Kratos at this spawn point (the game reset, FUN_002cf054, keeps it); the
         * level may set it back to 0 as it starts, which step 1 checks */
        if (g_warp_spawn >= 0 && !g_warp_via) rt_w32(game + GOS.game_spawn, (uint32_t)g_warp_spawn);
        g_gos_stage = 1;
        return;
    }
    if (g_gos_stage == 1) {
        if (!gos_in_level(game, first)) return;
        rt_log("[debug] warp: the game started in %s at frame %llu\n", first, (unsigned long long)frame);
        if (g_warp_spawn > 0 && !g_warp_via) {
            uint32_t got = rt_r32(game + GOS.game_spawn), n = rt_r32(gos_find_level(first) + GOS.level_spawns);
            if (got == (uint32_t)g_warp_spawn) {
                rt_log("[debug] warp: spawn point %u of %u\n", got, n);
                g_warp_done = 1;
                return;
            }
            rt_log("[debug] warp: the start reset the spawn point to %u; placing Kratos again\n", got);
        }
        if (g_warp_via) {
            /* the game's own hard level change (FUN_000e4bfc, run from its update by FUN_000e4fdc), the one
             * Load Game takes: every level is unloaded, LEVEL is loaded at once and Kratos is put at spawn 0 */
            memset(rt_ptr(game + GOS_NEXT_LEVEL), 0, 64);
            memcpy(rt_ptr(game + GOS_NEXT_LEVEL), g_warp, strlen(g_warp));
            rt_w32(game + GOS_FLAGS, rt_r32(game + GOS_FLAGS) | 0xC0u);
            g_gos_stage = 4;
            return;
        }
        g_gos_stage = 2;
        if (g_warp_spawn <= 0) { g_warp_done = 1; return; }
    }
    if (g_gos_stage == 4) {
        if (!gos_in_level(game, g_warp)) return;
        rt_log("[debug] warp: the game changed to %s at frame %llu\n", g_warp, (unsigned long long)frame);
        g_gos_stage = 2;
        if (g_warp_spawn <= 0) { g_warp_done = 1; return; }
    }
    if (g_gos_stage == 2) {
        uint32_t lvl = gos_find_level(g_warp);
        if (!lvl || !ram_ptr(rt_r32(game + GOS.game_hero))) return;
        uint32_t n = rt_r32(lvl + GOS.level_spawns);
        rt_log("[debug] warp: spawn point %d of %u (Kratos removed and placed again)\n", g_warp_spawn, n);
        /* as the game's own level change does (FUN_000e4bfc): remove Kratos, then place him; placing him
         * without removing him first leaves the old Kratos in memory */
        uint32_t args1[1] = { game };
        rt_call_guest(&k_cur->ctx, GOS_REMOVE_KRATOS, 1, args1);
        rt_w32(game + GOS.game_spawn, (uint32_t)g_warp_spawn);
        uint32_t args[3] = { game, lvl, 0 };
        rt_call_guest(&k_cur->ctx, GOS_PLACE_KRATOS, 3, args);
        g_gos_stage = 3;
        g_warp_done = 1;
    }
}

static void load_level(void) {
    g_load_done = 1;
    uint32_t mgr = rt_r32(g->mgr_ptr);
    if (!ram_ptr(mgr) || !k_cur) { rt_log("[debug] --load-level: no level manager yet\n"); return; }
    uint32_t name = guest_name(g_load_name);
    if (!name) { rt_log("[debug] --load-level: no guest memory for the level name\n"); return; }
    rt_log("[debug] load level %s (deferred flag %u)\n", g_load_name, rt_r8(g->defer_flag));
    uint32_t args[4] = { mgr, name, 1, 0 };
    rt_call_guest(&k_cur->ctx, g->load_level, 4, args);
}

/* --god: Kratos's health back to its maximum at every controller read. */
static void god(void) {
    if (!g->health_vtable) return;
    uint32_t game = rt_r32(g->game_ptr), hero = ram_ptr(game) ? rt_r32(game + g->game_hero) : 0;
    uint32_t comps = ram_ptr(hero) ? rt_r32(hero + 0x24) : 0;
    if (!ram_ptr(comps)) return;
    for (uint32_t k = 0; k < 64; k++) {
        uint32_t c = rt_r32(comps + 4 * k);
        if (!ram_ptr(c) || rt_r32(c + 0xC) != g->health_vtable) continue;
        float cur = rt_rf(c + g->health_cur), max = rt_rf(c + g->health_max);
        if (cur <= 0.0f && max > 0.0f && !g_deaths++) rt_log("[debug] Kratos died (health %.1f) despite --god\n", cur);
        if (max > 0.0f && cur < max) {
            memcpy(rt_ptr(c + g->health_cur), &max, 4);
            if (!g_god_refills++) rt_log("[debug] god mode: health %.1f -> %.1f (component %08X)\n", cur, max, c);
        }
        return;
    }
}

void rt_debug_input(uint64_t frame) {
    if (!g) {
        if ((g_warp || g_god || g_level_log || g_load_name[0]) && !g_warp_done) {
            rt_log("[debug] --warp, --god, --level-log and --load-level are not known for %s; ignored\n", RT_GAME_ID);
            g_warp_done = 1;
        }
        if (g_dump_frames) dump_ram(frame);
        return;
    }
    if (g_god) god();
    if (g_warp && !g_warp_done && frame >= g_warp_frame) {
        if (is_coo()) warp_coo();
        else warp_gos(frame);
    }
    if (g_load_name[0] && !g_load_done && frame >= g_load_frame) load_level();
    if (g_level_log) log_levels();
    if (g_dump_frames) dump_ram(frame);
}

void rt_debug_report(void) {
    if (g_dump_frames && strstr(g_dump_frames, "stop")) {
        char path[600];
        snprintf(path, sizeof(path), "%s_stop.bin", g_dump_prefix);
        write_ram(path);
    }
    if (g_warp && !g_warp_done && g_gos_stage)
        printf("[debug] the warp to %s did not finish (step %d: %s)\n", g_warp, g_gos_stage,
               g_gos_stage == 1 ? "the game never started there" :
               g_gos_stage == 4 ? "the level change never happened" : "Kratos never appeared");
    else if (g_warp && !g_warp_done) printf("[debug] the warp to %s never ran (frame %u not reached)\n", g_warp, g_warp_frame);
    if (g_god) printf("[debug] god mode refilled Kratos's health %u times; health reached 0 %u times\n", g_god_refills, g_deaths);
}
