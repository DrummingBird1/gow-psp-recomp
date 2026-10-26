/* Host side of the port: the game window (SDL3), real-time pacing and controller input.
 *
 * Without --window the runtime stays headless and deterministic: virtual time runs as fast
 * as the host allows and input comes only from --input scripts. With --window, frames are
 * shown as the game displays them, virtual time is held to the wall clock (when the host
 * falls behind, the game slows down instead of skipping; at a high frame rate, --fps above
 * 60, virtual time skips ahead instead, see rt_host_idle), and the keyboard and gamepads
 * drive sceCtrl.
 *
 * Sound is mixed in rt_audio.c; in the window it is handed to the audio device from
 * rt_host_idle, right before the runtime sleeps until the wall clock catches up.
 *
 * Keys: arrows = D-pad, Z = cross, X = circle, A = square, S = triangle, Q/W = L/R,
 * Space or Enter = start, V = select, I/J/K/L = analog stick; F11 or Alt+Enter = fullscreen,
 * F12 = screenshot, Esc = quit. Gamepads use the PSP layout (south button = cross).
 *
 * Replays: the window samples the keyboard and gamepads once per displayed frame (and once
 * per vblank while no frames are shown, as in movies), so the game reads the same state all
 * through a frame. --record-input writes every change of what the game reads, keyed by the
 * displayed frame (and virtual time for changes between frames), in the --input format;
 * --input with that file replays the session, headless too, read for read. A recording also
 * holds the display refresh (fps=), the virtual-time skips (skip=) of a high frame rate, and
 * the movies the player skipped (movie=). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_internal.h"
#include "rt_audio.h"
#include "rt_debug.h"
#include "rt_frames.h"
#include "rt_gpu.h"
#include "rt_host.h"
#include "rt_kernel.h"

#define DISP_W 480
#define DISP_H 272

static HostOptions g_o;

/* --- scripted input --------------------------------------------------------------- */
/* by displayed frame, or by virtual time ("t" prefix: seconds) when no frames are shown, e.g.
 * in movies, or between two (frame, virtual microseconds) keys ("F@US:buttons:F@US", what
 * --record-input writes for changes within a frame) */
typedef struct { uint64_t frame, until, t0, t1; int by_time, keyed; uint32_t buttons; int lx, ly; } ScriptPress;
static ScriptPress *g_script;
static int g_nscript;
static uint64_t g_script_end;  /* "end=N" of a recording: where the recorded session stopped */
static double g_script_fps;     /* "fps=N" of a recording: the display refresh it was played at */
/* "skip=NOW:NEXT:US" of a recording: at a high frame rate the window fell behind the wall clock
 * when virtual time went from NOW to NEXT, and virtual time skipped US further (rt_host_idle) */
typedef struct { uint64_t now, next, us; } Skip;
static Skip *g_skips;
static int g_nskips, g_skip_at;
/* "movie=US" of a recording: the player skipped the movie that was playing at virtual time US */
static uint64_t *g_movie_skips;
static int g_nmovie_skips, g_movie_skip_at;

static const struct { const char *name; uint32_t bits; int lx, ly; } k_names[] = {
    { "select", PSP_SELECT }, { "start", PSP_START }, { "up", PSP_UP }, { "right", PSP_RIGHT }, { "down", PSP_DOWN },
    { "left", PSP_LEFT }, { "l", PSP_LTRIGGER }, { "r", PSP_RTRIGGER }, { "triangle", PSP_TRIANGLE },
    { "circle", PSP_CIRCLE }, { "cross", PSP_CROSS }, { "square", PSP_SQUARE },
    { "aup", 0, 128, 0 }, { "adown", 0, 128, 255 }, { "aleft", 0, 0, 128 }, { "aright", 0, 255, 128 },
};

static int parse_key(const char *s, uint64_t *frame, uint64_t *us) {
    char *end;
    *frame = strtoull(s, &end, 10);
    if (*end != '@') return 0;
    *us = strtoull(end + 1, &end, 10);
    return *end == 0;
}

/* "frame:buttons[:frames]", "tSECONDS:buttons[:seconds]" or "F@US:buttons:F@US" entries
 * separated by commas; buttons joined with '+', the stick also as lx=N / ly=N (0-255).
 * "rtc=SECONDS" sets the guest's wall clock at virtual time 0 (rt_wall_set_epoch); "end=N"
 * is the displayed frame at which a recorded session stopped. */
static int parse_script(const char *spec) {
    for (const char *p = spec; p && *p;) {
        char item[256];
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n >= sizeof(item)) n = sizeof(item) - 1;
        memcpy(item, p, n);
        item[n] = 0;
        p = end ? end + 1 : NULL;
        if (!item[0]) continue;
        if (!strncmp(item, "rtc=", 4)) { rt_wall_set_epoch(strtoll(item + 4, NULL, 10)); continue; }
        if (!strncmp(item, "end=", 4)) { g_script_end = strtoull(item + 4, NULL, 10); continue; }
        if (!strncmp(item, "fps=", 4)) { g_script_fps = strtod(item + 4, NULL); continue; }
        if (!strncmp(item, "skip=", 5)) {
            Skip s;
            char *e;
            s.now = strtoull(item + 5, &e, 10);
            s.next = *e == ':' ? strtoull(e + 1, &e, 10) : 0;
            s.us = *e == ':' ? strtoull(e + 1, &e, 10) : 0;
            if (!s.next || !s.us) { fprintf(stderr, "[host] bad --input entry '%s' (skip=NOW:NEXT:US)\n", item); return 0; }
            g_skips = realloc(g_skips, sizeof(*g_skips) * (g_nskips + 1));
            g_skips[g_nskips++] = s;
            continue;
        }
        if (!strncmp(item, "movie=", 6)) {
            g_movie_skips = realloc(g_movie_skips, sizeof(*g_movie_skips) * (g_nmovie_skips + 1));
            g_movie_skips[g_nmovie_skips++] = strtoull(item + 6, NULL, 10);
            continue;
        }
        char *f = strtok(item, ":"), *b = strtok(NULL, ":"), *d = strtok(NULL, ":");
        if (!f || !b) { fprintf(stderr, "[host] bad --input entry '%s'\n", item); return 0; }
        ScriptPress sp = { 0, 0, 0, 0, f[0] == 't', 0, 0, 128, 128 };
        if (strchr(f, '@')) {
            sp.keyed = 1;
            if (!d || !parse_key(f, &sp.frame, &sp.t0) || !parse_key(d, &sp.until, &sp.t1)) {
                fprintf(stderr, "[host] bad --input entry '%s' (F@US:buttons:F@US)\n", f);
                return 0;
            }
        } else if (sp.by_time) {
            sp.frame = (uint64_t)(strtod(f + 1, NULL) * 1e6);
            sp.until = sp.frame + (uint64_t)((d ? strtod(d, NULL) : 0.1) * 1e6);
        } else {
            sp.frame = strtoull(f, NULL, 10);
            sp.until = sp.frame + (d ? strtoull(d, NULL, 10) : 6);
        }
        for (char *name = strtok(b, "+"); name; name = strtok(NULL, "+")) {
            int found = 0;
            if (!strncmp(name, "lx=", 3)) { sp.lx = atoi(name + 3); found = 1; }
            else if (!strncmp(name, "ly=", 3)) { sp.ly = atoi(name + 3); found = 1; }
            for (size_t i = 0; i < sizeof(k_names) / sizeof(k_names[0]) && !found; i++)
                if (!strcmp(name, k_names[i].name)) {
                    sp.buttons |= k_names[i].bits;
                    if (k_names[i].lx || k_names[i].ly) { sp.lx = k_names[i].lx; sp.ly = k_names[i].ly; }
                    found = 1;
                }
            if (!found) { fprintf(stderr, "[host] unknown button '%s' in --input\n", name); return 0; }
        }
        g_script = realloc(g_script, sizeof(*g_script) * (g_nscript + 1));
        g_script[g_nscript++] = sp;
    }
    return 1;
}

/* --input takes the entries themselves or a file of them (any number per line, '#' starts
 * a comment), such as a recording made with --record-input. */
static int load_script(const char *arg) {
    FILE *f = fopen(arg, "rb");
    if (!f) return parse_script(arg);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = (char *)malloc((size_t)n + 1);
    size_t got = fread(text, 1, (size_t)n, f);
    fclose(f);
    text[got] = 0;
    int comment = 0;
    for (char *c = text; *c; c++) {
        if (*c == '#') comment = 1;
        if (*c == '\n') comment = 0;
        if (comment || *c == '\n' || *c == '\r' || *c == ' ' || *c == '\t') *c = ',';
    }
    int ok = parse_script(text);
    free(text);
    if (ok) printf("[host] input from %s: %d entries\n", arg, g_nscript);
    /* a replay stops where the session did; in the window it may go on with live input */
    if (ok && g_script_end && !g_o.window && !rt_max_frames) {
        rt_max_frames = (uint32_t)g_script_end;
        printf("[host] the recorded session stopped at frame %llu: so does this run\n", (unsigned long long)g_script_end);
    }
    return ok;
}

static uint32_t script_buttons(uint64_t frame, uint64_t now_us, int *lx, int *ly) {
    uint32_t b = 0;
    for (int i = 0; i < g_nscript; i++) {
        const ScriptPress *e = &g_script[i];
        int on;
        if (e->keyed)
            on = (frame > e->frame || (frame == e->frame && now_us >= e->t0)) &&
                 (frame < e->until || (frame == e->until && now_us < e->t1));
        else {
            uint64_t at = e->by_time ? now_us : frame;
            on = at >= e->frame && at < e->until;
        }
        if (on) {
            b |= e->buttons;
            if (e->lx != 128 || e->ly != 128) { *lx = e->lx; *ly = e->ly; }
        }
    }
    return b;
}

/* --- recording (--record-input) ------------------------------------------------------- */
/* What the game read is written as segments of unchanged input: "F:buttons:frames" when the
 * change happened at the first read of a frame and lasted to the first read of a later one,
 * "F@US:buttons:F@US" otherwise. A segment is written when it ends; the one still open is
 * closed when the run stops or crashes (rt_host_record_close). */
static FILE *g_rec;
static uint64_t g_rec_frame, g_rec_us, g_rec_last_frame, g_read_frame = UINT64_MAX, g_rec_entries;
static int g_rec_aligned = 1, g_rec_lx = 128, g_rec_ly = 128;
static uint32_t g_rec_b;

static void rec_segment(uint64_t end_frame, uint64_t end_us, int end_aligned) {
    if (!g_rec || (!g_rec_b && g_rec_lx == 128 && g_rec_ly == 128)) return;
    char names[256] = "";
    for (size_t i = 0; i < 12; i++)
        if (g_rec_b & k_names[i].bits) {
            if (names[0]) strcat(names, "+");
            strcat(names, k_names[i].name);
        }
    if (g_rec_lx != 128 || g_rec_ly != 128)
        snprintf(names + strlen(names), sizeof(names) - strlen(names), "%slx=%d+ly=%d", names[0] ? "+" : "", g_rec_lx, g_rec_ly);
    if (g_rec_aligned && end_aligned)
        fprintf(g_rec, "%llu:%s:%llu\n", (unsigned long long)g_rec_frame, names, (unsigned long long)(end_frame - g_rec_frame));
    else
        fprintf(g_rec, "%llu@%llu:%s:%llu@%llu\n", (unsigned long long)g_rec_frame, (unsigned long long)g_rec_us, names,
                (unsigned long long)end_frame, (unsigned long long)end_us);
    fflush(g_rec);
    g_rec_entries++;
}

static void rec_read(uint64_t frame, uint64_t now_us, int first_in_frame, uint32_t b, int lx, int ly) {
    g_rec_last_frame = frame;
    if (b == g_rec_b && lx == g_rec_lx && ly == g_rec_ly) return;
    rec_segment(frame, now_us, first_in_frame);
    g_rec_frame = frame;
    g_rec_us = now_us;
    g_rec_aligned = first_in_frame;
    g_rec_b = b;
    g_rec_lx = lx;
    g_rec_ly = ly;
}

void rt_host_record_close(void) {
    if (!g_rec) return;
    rec_segment(g_rec_last_frame + 1, 0, 1);  /* whatever is held lasts to the end */
    fprintf(g_rec, "end=%llu\n# %llu segments\n", (unsigned long long)g_rec_last_frame + 1, (unsigned long long)g_rec_entries);
    fclose(g_rec);
    g_rec = NULL;
    printf("[host] input recording closed at frame %llu (%llu segments)\n", (unsigned long long)g_rec_last_frame,
           (unsigned long long)g_rec_entries);
}

static int rec_open(const char *path) {
    g_rec = fopen(path, "w");
    if (!g_rec) { fprintf(stderr, "[host] cannot write %s\n", path); return 0; }
    fprintf(g_rec, "# God of War PSP native port: input recording (--record-input). Replay with --input <this file>,\n"
                   "# starting from the same --ms0 contents. Entries: frame:buttons:frames, or frame@us:buttons:frame@us\n"
                   "# (us = virtual time in microseconds) for changes within a displayed frame.\n"
                   "rtc=%lld\n", (long long)rt_wall_epoch());
    if (rt_display_hz() != 59.94) fprintf(g_rec, "fps=%.6f\n", rt_display_hz());
    fflush(g_rec);
    atexit(rt_host_record_close);
    printf("[host] recording input to %s\n", path);
    return 1;
}

/* the display refresh: --fps, else the one a recording was played at */
static void setup_refresh(void) {
    double hz = rt_gpu_opt.fps > 0 ? rt_gpu_opt.fps : g_script_fps;
    if (hz <= 0) return;
    rt_display_set_hz(hz);
    printf("[host] display refresh %.3f Hz (vblank every %.0f us)\n", rt_display_hz(), 1e6 / rt_display_hz());
}

int rt_host_wants_idle(void) { return g_nskips > 0; }

/* headless replay: the virtual-time skips the window made, at the same points */
static void replay_skips(uint64_t now, uint64_t next) {
    while (g_skip_at < g_nskips && g_skips[g_skip_at].now < now) g_skip_at++;
    if (g_skip_at < g_nskips && g_skips[g_skip_at].now == now && g_skips[g_skip_at].next == next) k_idle_skip = g_skips[g_skip_at++].us;
}

/* replay: the recording's next movie skip, once virtual time gets to it (the movie code asks at the
 * same virtual times as in the recorded session, so the first ask at or after it is the one) */
static int replay_movie_skip(void) {
    if (g_movie_skip_at >= g_nmovie_skips || g_movie_skips[g_movie_skip_at] > k_now) return 0;
    g_movie_skip_at++;
    printf("[host] movie skipped as in the recording\n");
    return 1;
}

static int live_input(uint32_t *b, int *lx, int *ly);  /* keyboard and gamepads, when there is a window */

uint32_t rt_host_buttons(uint64_t frame, uint64_t now_us, uint8_t *lx, uint8_t *ly) {
    int ax = 128, ay = 128;
    rt_debug_input(frame);
    int first_in_frame = frame != g_read_frame;
    g_read_frame = frame;
    uint32_t b = script_buttons(frame, now_us, &ax, &ay);
    /* the devices are sampled once per frame, or per vblank while no frames are shown */
    static uint64_t latch_us;
    static uint32_t live_b;
    static int live_x = 128, live_y = 128, have_live, latched;
    if (first_in_frame || now_us - latch_us >= 16683 || !latched) {
        latched = 1;
        latch_us = now_us;
        live_b = 0;
        live_x = live_y = 128;
        have_live = live_input(&live_b, &live_x, &live_y);
    }
    if (have_live) {
        b |= live_b;
        if (live_x != 128) ax = live_x;
        if (live_y != 128) ay = live_y;
    }
    rec_read(frame, now_us, first_in_frame, b, ax, ay);
    *lx = (uint8_t)ax;
    *ly = (uint8_t)ay;
    return b;
}

#ifdef GOW_HAVE_SDL
#define SDL_MAIN_HANDLED  /* the runtime keeps its own main() */
#include <SDL3/SDL.h>
#if defined(__ANDROID__) || defined(GOW_ANDROID)
bool gow_host_poll(SDL_Event *e);
bool gow_host_wait(SDL_Event *e, Sint32 timeout_ms);
#define SDL_PollEvent(e) gow_host_poll(e)
#define SDL_WaitEventTimeout(e, timeout_ms) gow_host_wait(e, timeout_ms)
#define SDL_HideCursor() true
#define SDL_ShowCursor() true
#define SDL_SetWindowFullscreen(w, f) true
#endif
static inline bool gow_set_title(SDL_Window *w, const char *t) {
#if defined(__ANDROID__) || defined(GOW_ANDROID)
    (void)w; (void)t;
    return true;
#else
    return SDL_SetWindowTitle(w, t);
#endif
}

#include <SDL3/SDL_main.h>

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static SDL_Gamepad *g_pads[8];
static int g_quit, g_fullscreen;
static uint64_t g_wall0, g_virt0, g_last_pump, g_title_t, g_title_frame, g_last_frame;
static uint64_t g_skips_made, g_skipped_us;  /* virtual-time skips at a high frame rate (rt_host_idle) */
static uint64_t g_skips_capped;              /* skips shortened (or left out) to keep the sound whole */
static int g_anchored;
/* PSP buttons the game must not see until they are released: the press that skipped a movie or
 * answered the quit question (otherwise it would also reach the menu behind it) */
static uint32_t g_eat;
static int g_movie_on, g_skip_req;
static uint64_t g_quit_hold;  /* when gamepad Back+Start started being held together */
static uint32_t g_rgba[DISP_W * DISP_H];
static uint32_t g_shot_addr, g_shot_stride, g_shot_format;

static uint64_t now_us(void) { return SDL_GetTicksNS() / 1000; }

/* --- controls ------------------------------------------------------------------------
 * Bindings: up to 4 keys and 4 gamepad buttons per PSP button and stick direction. The defaults
 * are below; --controls FILE replaces the bindings of each button it names, one line each:
 *   cross = Z, pad:a        keys by SDL name (Z, Up, Space, Return, Left Shift...), gamepad buttons
 *   stick_up = I            as pad:<SDL name> (a b x y back start leftshoulder rightshoulder
 *                           lefttrigger righttrigger dpup dpdown dpleft dpright leftstick rightstick)
 *   l = Q, Mouse Right      mouse buttons as Mouse Left, Mouse Right, Mouse Middle, Mouse 4, Mouse 5
 * '#' starts a comment; the comma key is "Comma". Unknown names are reported and skipped; the game keeps running. */
enum { B_STICK_UP = 12, B_STICK_DOWN, B_STICK_LEFT, B_STICK_RIGHT, NBIND };
#define PAD_LT 100  /* the triggers are axes: pseudo button numbers */
#define PAD_RT 101
#define MOUSE_KEY 1000  /* mouse buttons are "keys" MOUSE_KEY + SDL button number (1 left .. 5 X2) */
static const char *const b_names[NBIND] = { "up", "down", "left", "right", "cross", "circle", "square", "triangle",
                                            "l", "r", "start", "select", "stick_up", "stick_down", "stick_left", "stick_right" };
static const uint32_t b_bits[12] = { PSP_UP, PSP_DOWN, PSP_LEFT, PSP_RIGHT, PSP_CROSS, PSP_CIRCLE, PSP_SQUARE, PSP_TRIANGLE,
                                     PSP_LTRIGGER, PSP_RTRIGGER, PSP_START, PSP_SELECT };
static const char *const mouse_names[6] = { NULL, "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse 4", "Mouse 5" };
static struct { int nk, np; int k[4]; int p[4]; } g_bind[NBIND] = {
    { 1, 1, { SDL_SCANCODE_UP }, { SDL_GAMEPAD_BUTTON_DPAD_UP } },
    { 1, 1, { SDL_SCANCODE_DOWN }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN } },
    { 1, 1, { SDL_SCANCODE_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_LEFT } },
    { 1, 1, { SDL_SCANCODE_RIGHT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT } },
    { 1, 1, { SDL_SCANCODE_Z }, { SDL_GAMEPAD_BUTTON_SOUTH } },
    { 1, 1, { SDL_SCANCODE_X }, { SDL_GAMEPAD_BUTTON_EAST } },
    { 1, 1, { SDL_SCANCODE_A }, { SDL_GAMEPAD_BUTTON_WEST } },
    { 1, 1, { SDL_SCANCODE_S }, { SDL_GAMEPAD_BUTTON_NORTH } },
    { 1, 2, { SDL_SCANCODE_Q }, { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_LT } },
    { 1, 2, { SDL_SCANCODE_W }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_RT } },
    { 2, 1, { SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN }, { SDL_GAMEPAD_BUTTON_START } },
    { 1, 1, { SDL_SCANCODE_V }, { SDL_GAMEPAD_BUTTON_BACK } },
    { 1, 0, { SDL_SCANCODE_I } }, { 1, 0, { SDL_SCANCODE_K } }, { 1, 0, { SDL_SCANCODE_J } }, { 1, 0, { SDL_SCANCODE_L } },
};

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static void load_controls(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[host] --controls %s: cannot open it; default controls\n", path); return; }
    char line[512];
    int n = 0, bad = 0;
    while (fgets(line, sizeof(line), f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *name = trim(line);
        int b = -1;
        for (int i = 0; i < NBIND; i++) if (!SDL_strcasecmp(name, b_names[i])) b = i;
        if (b < 0) { fprintf(stderr, "[host] --controls: unknown button '%s'\n", name); bad++; continue; }
        g_bind[b].nk = g_bind[b].np = 0;
        for (char *tok = strtok(eq + 1, ","); tok; tok = strtok(NULL, ",")) {
            char *t = trim(tok);
            if (!*t) continue;
            if (!SDL_strncasecmp(t, "pad:", 4)) {
                int p = !SDL_strcasecmp(t + 4, "lefttrigger") ? PAD_LT : !SDL_strcasecmp(t + 4, "righttrigger") ? PAD_RT
                                                                                                                 : (int)SDL_GetGamepadButtonFromString(t + 4);
                if (p == SDL_GAMEPAD_BUTTON_INVALID) { fprintf(stderr, "[host] --controls: unknown gamepad button '%s'\n", t); bad++; }
                else if (g_bind[b].np < 4) g_bind[b].p[g_bind[b].np++] = p;
            } else {
                int sc = !SDL_strcasecmp(t, "Comma") ? SDL_SCANCODE_COMMA : SDL_GetScancodeFromName(t);  /* "," would split the line */
                for (int m = 1; m < 6; m++) if (!SDL_strcasecmp(t, mouse_names[m])) sc = MOUSE_KEY + m;
                if (sc == SDL_SCANCODE_UNKNOWN) { fprintf(stderr, "[host] --controls: unknown key '%s'\n", t); bad++; }
                else if (g_bind[b].nk < 4) g_bind[b].k[g_bind[b].nk++] = sc;
            }
        }
        n++;
    }
    fclose(f);
    printf("[host] controls from %s: %d buttons set%s\n", path, n, bad ? " (some names were unknown: see above)" : "");
    /* a key on two buttons presses both (in a bug report, the likely reason why "the controls don't work") */
    for (int a = 0; a < NBIND; a++)
        for (int i = 0; i < g_bind[a].nk; i++)
            for (int b2 = a + 1; b2 < NBIND; b2++)
                for (int j = 0; j < g_bind[b2].nk; j++)
                    if (g_bind[a].k[i] == g_bind[b2].k[j])
                        printf("[host] controls: one key is on both %s and %s: it presses both\n", b_names[a], b_names[b2]);
}

static int pad_down(SDL_Gamepad *gp, int p) {
    if (p == PAD_LT) return SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000;
    if (p == PAD_RT) return SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
    return SDL_GetGamepadButton(gp, (SDL_GamepadButton)p);
}

/* The mouse pointer hides itself in fullscreen when it has not moved for 2 s. */
static uint64_t g_mouse_t, g_focus_t;
static int g_cursor_hidden;
static void update_cursor(void) {
    int hide = g_fullscreen && now_us() - g_mouse_t > 2000000;
    if (hide == g_cursor_hidden) return;
    g_cursor_hidden = hide;
    if (hide) SDL_HideCursor(); else SDL_ShowCursor();
}

void rt_host_rumble(float strength, int ms) {
    Uint16 lo = (Uint16)(strength * 0xFFFF), hi = (Uint16)(strength * 0xC000);
    for (int i = 0; i < 8; i++) if (g_pads[i]) SDL_RumbleGamepad(g_pads[i], lo, hi, (Uint32)ms);
}

static void open_pad(SDL_JoystickID id) {
    for (int i = 0; i < 8; i++)
        if (!g_pads[i]) {
            g_pads[i] = SDL_OpenGamepad(id);
            if (g_pads[i]) printf("[host] gamepad connected: %s\n", SDL_GetGamepadName(g_pads[i]));
            return;
        }
}

static void close_pad(SDL_JoystickID id) {
    for (int i = 0; i < 8; i++)
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) { SDL_CloseGamepad(g_pads[i]); g_pads[i] = NULL; }
}

static void screenshot(void) {
    char path[512];
    static int n;
    if (!g_shot_addr) return;
    snprintf(path, sizeof(path), "%s/window_frame_%06llu_%d.png", g_o.shot_dir ? g_o.shot_dir : ".", (unsigned long long)g_last_frame, n++);
    int ok;
    if (rt_gpu_active()) {
        ok = rt_frames_write_png_gpu(path, g_shot_addr, g_shot_stride, g_shot_format);  /* at the internal resolution */
    } else {
        ok = rt_frames_write_png(path, g_shot_addr, g_shot_stride, g_shot_format);
    }
    if (ok) printf("[host] screenshot %s\n", path);
}

void rt_host_movie_playing(int on) {
    if (on != g_movie_on) g_skip_req = 0;
    g_movie_on = on;
}

/* A skip the player asked for is written to the input recording ("movie=US"), so that its replay
 * skips the same movie at the same point; a replay takes the recording's skips. */
int rt_host_take_movie_skip(void) {
    if (replay_movie_skip()) return 1;
    int r = g_skip_req || g_o.skip_movies;  /* --skip-movies: every movie ends as it starts */
    if (r && !g_skip_req) printf("[host] movie skipped (--skip-movies)\n");
    g_skip_req = 0;
    if (r && g_rec) {
        fprintf(g_rec, "movie=%llu\n", (unsigned long long)k_now);
        fflush(g_rec);
    }
    return r;
}

static void movie_skip_press(uint32_t bits) {
    if (!g_movie_on || g_skip_req) return;
    g_skip_req = 1;
    g_eat |= bits;  /* only the press that skipped is kept from the game */
    printf("[host] movie skipped by the player\n");
}

/* the PSP buttons a key (or MOUSE_KEY + mouse button) or gamepad button is bound to (--controls), for the movie skip */
static uint32_t key_bits(int sc) {
    uint32_t b = 0;
    for (int i = 0; i < 12; i++)
        for (int j = 0; j < g_bind[i].nk; j++) if (g_bind[i].k[j] == sc) b |= b_bits[i];
    return b;
}

static uint32_t pad_bits(int button) {
    uint32_t b = 0;
    for (int i = 0; i < 12; i++)
        for (int j = 0; j < g_bind[i].np; j++) if (g_bind[i].p[j] == button) b |= b_bits[i];
    return b;
}

/* "Quit the game?" over the game window. The game (and its sound) waits while it is open:
 * this runs on the game's own thread. Enter / Y / gamepad Cross: yes; Esc / N / Circle: no. */
static void draw_confirm(SDL_Renderer *r, int w, int h) {
    SDL_SetRenderDrawColor(r, 16, 16, 16, 255);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 200, 160, 60, 255);
    SDL_FRect fr = { 2, 2, (float)w - 4, (float)h - 4 };
    SDL_RenderRect(r, &fr);
    SDL_SetRenderScale(r, 2, 2);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_RenderDebugText(r, (w / 2 - 15 * 16) / 4.0f + 30, 12, "Quit the game?");
    SDL_SetRenderScale(r, 1, 1);
    SDL_SetRenderDrawColor(r, 220, 220, 220, 255);
    SDL_RenderDebugText(r, 20, 64, "Yes: Enter or gamepad Cross (A)");
    SDL_RenderDebugText(r, 20, 84, "No:  Esc or gamepad Circle (B)");
    SDL_RenderPresent(r);
}

static int confirm_quit(void) {
    printf("[host] quit question opened\n");
    rt_audio_pause(1);
    int ww = 0, wh = 0, w = 300, h = 110, answer = -1;
    SDL_GetWindowSize(g_win, &ww, &wh);
    SDL_Window *pop = SDL_CreatePopupWindow(g_win, (ww - w) / 2, (wh - h) / 2, w, h, SDL_WINDOW_POPUP_MENU);
    SDL_Renderer *r = pop ? SDL_CreateRenderer(pop, NULL) : NULL;
    if (!r) {  /* no popup: fall back to the system message box (keyboard / mouse only) */
        if (pop) SDL_DestroyWindow(pop);
        pop = NULL;
        const SDL_MessageBoxButtonData b[] = {
            { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes" }, { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No" } };
        const SDL_MessageBoxData d = { SDL_MESSAGEBOX_INFORMATION, g_win, "God of War", "Quit the game?", 2, b, NULL };
        int id = 0;
        answer = SDL_ShowMessageBox(&d, &id) && id == 1;
    }
    while (answer < 0) {
        draw_confirm(r, w, h);
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        do {
            switch (e.type) {
            case SDL_EVENT_QUIT: answer = 1; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.repeat) break;
                if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_Y) answer = 1;
                else if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_N) answer = 0;
                else if (e.key.key == SDLK_F4 && (e.key.mod & SDL_KMOD_ALT)) answer = 1;
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) answer = 1;
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) answer = 0;
                break;
            case SDL_EVENT_GAMEPAD_ADDED: open_pad(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: close_pad(e.gdevice.which); break;
            default: break;
            }
        } while (answer < 0 && SDL_PollEvent(&e));
    }
    if (r) SDL_DestroyRenderer(r);
    if (pop) SDL_DestroyWindow(pop);
    g_eat = 0xFFFFFFFFu;  /* whatever is held when the box closes waits for its release */
    g_anchored = 0;       /* the wall clock went on without the game: start pacing afresh */
    rt_audio_pause(0);
    printf("[host] quit question: %s\n", answer ? "yes, quitting" : "no, back to the game");
    return answer;
}

static void pump(void) {
    SDL_Event e;
    int ask = 0;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT: g_quit = 1; break;  /* the close button, and Alt+F4 */
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break;
            if (e.key.key == SDLK_F4 && (e.key.mod & SDL_KMOD_ALT)) g_quit = 1;
            else if (e.key.key == SDLK_ESCAPE) ask = 1;
            else if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
                g_fullscreen = !g_fullscreen;
                SDL_SetWindowFullscreen(g_win, g_fullscreen);
            } else if (e.key.key == SDLK_F12) screenshot();
            else if (e.key.key == SDLK_F3) rt_gpu_opt.show_fps = !rt_gpu_opt.show_fps;  /* the frame rate in the corner */
            else {
                /* a movie skips on the keys bound to Start or Cross, and on Enter and Space; the press is kept
                 * from the game only for the buttons that key is bound to */
                uint32_t kb = key_bits(e.key.scancode);
                if ((kb & (PSP_START | PSP_CROSS)) || e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE)
                    movie_skip_press(kb);
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: g_focus_t = now_us(); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            g_mouse_t = now_us();
            if (g_mouse_t - g_focus_t > 300000) movie_skip_press(key_bits(MOUSE_KEY + e.button.button));  /* not the click that brought the window to the front */
            else g_eat |= key_bits(MOUSE_KEY + e.button.button);  /* nor does that click reach the game */
            break;
        case SDL_EVENT_MOUSE_MOTION: g_mouse_t = now_us(); break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            uint32_t pb = pad_bits(e.gbutton.button);
            if (pb & (PSP_START | PSP_CROSS)) movie_skip_press(pb);
            break;
        }
        case SDL_EVENT_GAMEPAD_ADDED: open_pad(e.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_REMOVED: close_pad(e.gdevice.which); break;
        default: break;
        }
    }
    /* gamepad: hold Back + Start for 2 s to quit */
    int held = 0;
    for (int i = 0; i < 8; i++)
        if (g_pads[i] && SDL_GetGamepadButton(g_pads[i], SDL_GAMEPAD_BUTTON_BACK) && SDL_GetGamepadButton(g_pads[i], SDL_GAMEPAD_BUTTON_START)) held = 1;
    if (!held) g_quit_hold = 0;
    else if (!g_quit_hold) g_quit_hold = now_us();
    else if (now_us() - g_quit_hold >= 2000000 && !g_quit) { printf("[host] Back+Start held: quitting\n"); g_quit = 1; }
    if (ask && !g_quit && confirm_quit()) g_quit = 1;
    update_cursor();
    g_last_pump = now_us();
}

int rt_host_init(const HostOptions *o) {
    g_o = *o;
    if (g_o.input && !load_script(g_o.input)) return 0;
    setup_refresh();
    if (g_o.record_input && !rec_open(g_o.record_input)) return 0;
    SDL_SetMainReady();
    if (!g_o.window) return g_o.gpu ? rt_gpu_init(NULL, g_o.gpu_driver) : 1;
    if (g_o.controls) load_controls(g_o.controls);
    SDL_SetHint(SDL_HINT_APP_NAME, "God of War PSP native port");
    SDL_SetHint(SDL_HINT_WINDOWS_CLOSE_ON_ALT_F4, "1");  /* Alt+F4 closes the window, as in any Windows program */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { fprintf(stderr, "[host] SDL_Init: %s\n", SDL_GetError()); return 0; }
    int scale = g_o.scale > 0 ? g_o.scale : 2;
    int win_w = g_o.gpu && rt_gpu_opt.aspect > 0 ? (int)(DISP_H * scale * rt_gpu_opt.aspect + 0.5f) : DISP_W * scale;  /* --aspect */
    g_win = SDL_CreateWindow("God of War (PSP native port)", win_w, DISP_H * scale, SDL_WINDOW_RESIZABLE);
    if (!g_win) { fprintf(stderr, "[host] SDL_CreateWindow: %s\n", SDL_GetError()); return 0; }
    if (g_o.icon) {  /* otherwise Windows takes the exe's own icon (runtime/gow_rt.rc) */
        SDL_Surface *ic = SDL_LoadPNG(g_o.icon);
        if (ic) { SDL_SetWindowIcon(g_win, ic); SDL_DestroySurface(ic); }
        else fprintf(stderr, "[host] --icon %s: %s\n", g_o.icon, SDL_GetError());
    }
    if (g_o.gpu) {
        /* the hardware renderer presents to the window itself */
        rt_gpu_opt.nearest = g_o.nearest;
        if (!rt_gpu_init(g_win, g_o.gpu_driver)) return 0;
        if (g_o.fullscreen) { g_fullscreen = 1; SDL_SetWindowFullscreen(g_win, true); }
        printf("[host] window %dx%d (SDL GPU renderer); keys: arrows, Z/X/A/S, Q/W, Space/Enter = start, V = select, "
               "IJKL = stick, F11 fullscreen, F12 screenshot, F3 frame rate, Esc quit (asks), Alt+F4 quit, Enter/Space/click skips a movie\n", win_w, DISP_H * scale);
        g_title_t = now_us();
        pump();
        return 1;
    }
    g_ren = SDL_CreateRenderer(g_win, NULL);
    if (!g_ren) { fprintf(stderr, "[host] SDL_CreateRenderer: %s\n", SDL_GetError()); return 0; }
    SDL_SetRenderVSync(g_ren, 0);  /* pacing follows the game's own vblanks */
    SDL_SetRenderLogicalPresentation(g_ren, DISP_W, DISP_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, DISP_W, DISP_H);
    if (!g_tex) { fprintf(stderr, "[host] SDL_CreateTexture: %s\n", SDL_GetError()); return 0; }
    SDL_SetTextureScaleMode(g_tex, g_o.nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    if (g_o.fullscreen) { g_fullscreen = 1; SDL_SetWindowFullscreen(g_win, true); }
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    SDL_RenderPresent(g_ren);
    printf("[host] window %dx%d (%s renderer); keys: arrows, Z/X/A/S, Q/W, Space/Enter = start, V = select, "
           "IJKL = stick, F11 fullscreen, F12 screenshot, F3 frame rate, Esc quit (asks), Alt+F4 quit, Enter/Space/click skips a movie\n", DISP_W * scale, DISP_H * scale, SDL_GetRendererName(g_ren));
    g_title_t = now_us();
    pump();
    return 1;
}

void rt_host_shutdown(void) {
    rt_host_record_close();
    rt_gpu_shutdown();
    if (g_skips_made)
        printf("[host] %llu virtual-time skips (the window fell behind at %.0f Hz): %.1f ms in all; %llu shortened to keep the sound whole\n",
               (unsigned long long)g_skips_made, rt_display_hz(), g_skipped_us / 1000.0, (unsigned long long)g_skips_capped);
    if (!g_win) return;
    for (int i = 0; i < 8; i++) if (g_pads[i]) SDL_CloseGamepad(g_pads[i]);
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    g_win = NULL;
}

/* PSP framebuffer -> texture. 8888 is uploaded directly (red in the low byte = XBGR8888). */
int rt_host_present(uint64_t frame, uint32_t addr, uint32_t stride, uint32_t format) {
    g_last_frame = frame;
    if (rt_gpu_active()) {
        uint64_t t0 = rt_frames_host_us();
        rt_gpu_present(addr, stride, format);
        rt_perf_us[RT_PERF_PRESENT] += rt_frames_host_us() - t0;
    }
    if (!g_win) return 0;
    if (addr && stride) { g_shot_addr = addr; g_shot_stride = stride; g_shot_format = format; }
    if (!g_ren) {
        pump();
    } else if (addr && stride) {
        if (format == 3) {
            SDL_UpdateTexture(g_tex, NULL, rt_ptr(addr), (int)stride * 4);
        } else {
            for (int y = 0; y < DISP_H; y++) {
                const uint8_t *p = rt_ptr(addr + (uint32_t)y * stride * 2);
                uint32_t *o = g_rgba + y * DISP_W;
                for (int x = 0; x < DISP_W; x++, p += 2) {
                    uint32_t v = p[0] | (p[1] << 8), r, g, b;
                    if (format == 0) { r = v & 31; g = (v >> 5) & 63; b = (v >> 11) & 31; r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2); }
                    else if (format == 1) { r = v & 31; g = (v >> 5) & 31; b = (v >> 10) & 31; r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2); }
                    else { r = (v & 15) * 17; g = ((v >> 4) & 15) * 17; b = ((v >> 8) & 15) * 17; }
                    o[x] = r | g << 8 | b << 16;
                }
            }
            SDL_UpdateTexture(g_tex, NULL, g_rgba, DISP_W * 4);
        }
    }
    if (g_ren) {
        SDL_RenderClear(g_ren);
        SDL_RenderTexture(g_ren, g_tex, NULL, NULL);
        SDL_RenderPresent(g_ren);
        pump();
    }
    uint64_t t = now_us();
    if (t - g_title_t >= 1000000) {
        char title[160];
        double elapsed_s = (double)(t - g_title_t) / 1e6;
        double fps = (double)(frame - g_title_frame) / elapsed_s;
        snprintf(title, sizeof(title), "God of War (PSP native port)  |  %.1f fps  |  frame %llu", fps, (unsigned long long)frame);
        gow_set_title(g_win, title);
#if defined(__ANDROID__) || defined(GOW_ANDROID)
        /* Android has no visible desktop window title in fullscreen. Keep a low-cost
         * once-per-second performance sample in the app log for device-side diagnosis. */
        fprintf(stderr, "[gow-perf] fps=%.1f frame=%llu frame_ms=%.2f video_driver=%s gpu_backend=%s\n",
                fps, (unsigned long long)frame, fps > 0.0 ? 1000.0 / fps : 0.0,
                SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "unknown",
                rt_gpu_active() ? "vulkan" : "inactive");
#endif
        g_title_t = t;
        g_title_frame = frame;
    }
    return g_quit;
}

/* High frame rates (--fps above 60): when the host falls behind the wall clock by more than
 * SKIP_MIN_US, virtual time skips ahead by that much instead of the game slowing down. The
 * games step their logic by elapsed time, so they keep their speed and show fewer frames, as a
 * PC game does. Audio keeps going: the game keeps one block (~23 ms) queued ahead of the
 * "hardware", so a skip within that margin only delays the next block; a longer one leaves a
 * short gap in a channel. Beyond SKIP_MAX_US (a hitch, e.g. loading) the lost time is dropped,
 * as at 60 Hz. Skips are written to an input recording, so that its replay takes the same ones.
 * SKIP_HEADROOM_US stays unskipped before the end of a channel's queue: the thread that feeds it
 * wakes when its block starts but takes a few ms of virtual time to queue the next one (CoO's
 * music: 2-4.5 ms), so a skip to the very end left a gap after almost every 46 ms block. */
#define SKIP_MIN_US 4000
#define SKIP_MAX_US 100000
#define SKIP_HEADROOM_US 10000

static void rec_skip(uint64_t now, uint64_t next, uint64_t us) {
    if (!g_rec) return;
    fprintf(g_rec, "skip=%llu:%llu:%llu\n", (unsigned long long)now, (unsigned long long)next, (unsigned long long)us);
}

int rt_host_idle(uint64_t now_virt, uint64_t next_virt) {
    if (!g_win) { replay_skips(now_virt, next_virt); return 0; }
    /* every thread waits until next_virt, so nothing can still be queued before it: hand that
     * audio to the device now, before sleeping until the wall clock gets there */
    rt_audio_advance(next_virt);
    uint64_t wall = now_us();
    if (!g_anchored) { g_wall0 = wall; g_virt0 = now_virt; g_anchored = 1; }
    int64_t ahead = (int64_t)(g_wall0 + (next_virt - g_virt0)) - (int64_t)wall;
    if (ahead > 0) {
        /* keep the window responsive during long waits */
        while (ahead > 0 && !g_quit) {
            uint64_t slice = ahead > 8000 ? 8000 : (uint64_t)ahead;
            SDL_DelayPrecise(slice * 1000);
            pump();
            ahead = (int64_t)(g_wall0 + (next_virt - g_virt0)) - (int64_t)now_us();
        }
    } else if (rt_display_hz() > 61 && next_virt > now_virt && -ahead >= SKIP_MIN_US && -ahead <= SKIP_MAX_US) {
        /* never skip past the sound a channel still has queued: its next block would start late and
         * leave a dropout (heard as choppy sound over the music); what can't be skipped now is
         * skipped later or, past SKIP_MAX_US, dropped */
        uint64_t want = (uint64_t)-ahead, budget = rt_audio_skip_budget_us(next_virt);  /* the skip comes on top of next_virt */
        budget = budget > SKIP_HEADROOM_US ? budget - SKIP_HEADROOM_US : 0;
        uint64_t sk = want < budget ? want : budget;
        if (sk < want) g_skips_capped++;
        if (sk >= SKIP_MIN_US) {
            k_idle_skip = sk;
            rec_skip(now_virt, next_virt, k_idle_skip);
            g_skips_made++;
            g_skipped_us += k_idle_skip;
        }
    } else if (ahead < -100000) {
        /* more than 100 ms behind: run on from here rather than racing to catch up */
        g_wall0 = wall;
        g_virt0 = next_virt;
    }
    rt_audio_host_sync();
    if (now_us() - g_last_pump > 16000) pump();
    return g_quit;
}

/* RT_TEST_KEYS="FRAME:NAME[:N],...": for tests of the bindings without a focused window, key NAME (an SDL key
 * name or Mouse Left...) counts as held from shown frame FRAME for N frames (default 6), as if it were pressed. */
static void test_keys(const bool **k, SDL_MouseButtonFlags *mb) {
    static bool keys[SDL_SCANCODE_COUNT];
    static const char *spec;
    static int checked;
    if (!checked) { checked = 1; spec = getenv("RT_TEST_KEYS"); }
    if (!spec) return;
    memcpy(keys, *k, sizeof(keys));
    char buf[512];
    SDL_strlcpy(buf, spec, sizeof(buf));
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        char name[64] = "";
        unsigned long long f = 0;
        int n = 6;
        if (sscanf(tok, "%llu:%63[^:]:%d", &f, name, &n) < 2 || g_last_frame < f || g_last_frame >= f + (unsigned)n) continue;
        int m = 0;
        for (int i = 1; i < 6; i++) if (!SDL_strcasecmp(name, mouse_names[i])) m = i;
        if (m) *mb |= SDL_BUTTON_MASK(m);
        else if (SDL_GetScancodeFromName(name) != SDL_SCANCODE_UNKNOWN) keys[SDL_GetScancodeFromName(name)] = true;
    }
    *k = keys;
}

static int live_input(uint32_t *out_b, int *lx, int *ly) {
    if (!g_win) return 0;
    uint32_t b = 0;
    int ax = 128, ay = 128;
    {
        const bool *k = SDL_GetKeyboardState(NULL);
        SDL_MouseButtonFlags mb = SDL_GetWindowFlags(g_win) & SDL_WINDOW_INPUT_FOCUS ? SDL_GetMouseState(NULL, NULL) : 0;
        test_keys(&k, &mb);
        int stick[4] = { 0 };
        for (int i = 0; i < NBIND; i++) {
            int on = 0;
            for (int j = 0; j < g_bind[i].nk; j++) {
                int sc = g_bind[i].k[j];
                on |= sc >= MOUSE_KEY ? (mb & SDL_BUTTON_MASK(sc - MOUSE_KEY)) != 0 : k[sc];
            }
            for (int p = 0; p < 8; p++)
                if (g_pads[p]) for (int j = 0; j < g_bind[i].np; j++) on |= pad_down(g_pads[p], g_bind[i].p[j]);
            if (!on) continue;
            if (i < 12) b |= b_bits[i];
            else stick[i - B_STICK_UP] = 1;
        }
        if (b & PSP_START && (SDL_GetModState() & SDL_KMOD_ALT)) b &= ~(uint32_t)PSP_START;  /* Alt+Enter is fullscreen */
        if (stick[2]) ax = 0;
        if (stick[3]) ax = 255;
        if (stick[0]) ay = 0;
        if (stick[1]) ay = 255;
        for (int i = 0; i < 8; i++) {
            SDL_Gamepad *gp = g_pads[i];
            if (!gp) continue;
            int x = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY);
            if (abs(x) > 6000 || abs(y) > 6000) { ax = (x + 32768) >> 8; ay = (y + 32768) >> 8; }
            /* the right stick evades, as in the PS2 games: the PSP's evade is L + R + the stick in that direction */
            if (!g_o.no_stick_evade) {
                int rx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX), ry = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY);
                if (rx * rx + ry * ry > 16000 * 16000) {
                    b |= PSP_LTRIGGER | PSP_RTRIGGER;
                    ax = (rx + 32768) >> 8;
                    ay = (ry + 32768) >> 8;
                }
            }
        }
    }
    g_eat &= b;  /* a released button is the game's again */
    b &= ~g_eat;
    *out_b = b;
    *lx = ax;
    *ly = ay;
    return 1;
}

#else  /* built without SDL: headless only, scripted input */

int rt_host_init(const HostOptions *o) {
    g_o = *o;
    if (g_o.input && !load_script(g_o.input)) return 0;
    setup_refresh();
    if (g_o.record_input && !rec_open(g_o.record_input)) return 0;
    if (g_o.window || g_o.gpu) { fprintf(stderr, "[host] this build has no window or GPU support (SDL3 not linked)\n"); return 0; }
    return 1;
}
void rt_host_shutdown(void) { rt_host_record_close(); }
int rt_host_present(uint64_t frame, uint32_t addr, uint32_t stride, uint32_t format) { (void)frame; (void)addr; (void)stride; (void)format; return 0; }
int rt_host_idle(uint64_t now_us, uint64_t next_us) { replay_skips(now_us, next_us); return 0; }
static int live_input(uint32_t *b, int *lx, int *ly) { (void)b; (void)lx; (void)ly; return 0; }
void rt_host_movie_playing(int on) { (void)on; }
int rt_host_take_movie_skip(void) { return replay_movie_skip(); }
void rt_host_rumble(float strength, int ms) { (void)strength; (void)ms; }
#endif
