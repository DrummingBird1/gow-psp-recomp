/* Host side of the port: the game window (SDL3), real-time pacing and controller input.
 *
 * Without --window the runtime stays headless and deterministic: virtual time runs as fast
 * as the host allows and input comes only from --input scripts. With --window, frames are
 * shown as the game displays them, virtual time is held to the wall clock (when the host
 * falls behind, the game slows down instead of skipping), and the keyboard and gamepads
 * drive sceCtrl.
 *
 * Keys: arrows = D-pad, Z = cross, X = circle, A = square, S = triangle, Q/W = L/R,
 * Space or Enter = start, V = select, I/J/K/L = analog stick; F11 or Alt+Enter = fullscreen,
 * F12 = screenshot, Esc = quit. Gamepads use the PSP layout (south button = cross). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psp_rt.h"
#include "rt_frames.h"
#include "rt_host.h"

#define DISP_W 480
#define DISP_H 272

static HostOptions g_o;

/* --- scripted input --------------------------------------------------------------- */
/* by displayed frame, or by virtual time ("t" prefix: seconds) when no frames are shown, e.g. in movies */
typedef struct { uint64_t frame, until; int by_time; uint32_t buttons; int lx, ly; } ScriptPress;
static ScriptPress *g_script;
static int g_nscript;

static const struct { const char *name; uint32_t bits; int lx, ly; } k_names[] = {
    { "select", PSP_SELECT }, { "start", PSP_START }, { "up", PSP_UP }, { "right", PSP_RIGHT }, { "down", PSP_DOWN },
    { "left", PSP_LEFT }, { "l", PSP_LTRIGGER }, { "r", PSP_RTRIGGER }, { "triangle", PSP_TRIANGLE },
    { "circle", PSP_CIRCLE }, { "cross", PSP_CROSS }, { "square", PSP_SQUARE },
    { "aup", 0, 128, 0 }, { "adown", 0, 128, 255 }, { "aleft", 0, 0, 128 }, { "aright", 0, 255, 128 },
};

/* "frame:buttons[:frames]" or "tSECONDS:buttons[:seconds]" entries separated by commas;
 * buttons joined with '+'. */
static int parse_script(const char *spec) {
    for (const char *p = spec; p && *p;) {
        char item[128];
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n >= sizeof(item)) n = sizeof(item) - 1;
        memcpy(item, p, n);
        item[n] = 0;
        p = end ? end + 1 : NULL;
        char *f = strtok(item, ":"), *b = strtok(NULL, ":"), *d = strtok(NULL, ":");
        if (!f || !b) { fprintf(stderr, "[host] bad --input entry '%s'\n", item); return 0; }
        ScriptPress sp = { 0, 0, f[0] == 't', 0, 128, 128 };
        if (sp.by_time) {
            sp.frame = (uint64_t)(strtod(f + 1, NULL) * 1e6);
            sp.until = sp.frame + (uint64_t)((d ? strtod(d, NULL) : 0.1) * 1e6);
        } else {
            sp.frame = strtoull(f, NULL, 10);
            sp.until = sp.frame + (d ? strtoull(d, NULL, 10) : 6);
        }
        for (char *name = strtok(b, "+"); name; name = strtok(NULL, "+")) {
            int found = 0;
            for (size_t i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++)
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

static uint32_t script_buttons(uint64_t frame, uint64_t now_us, int *lx, int *ly) {
    uint32_t b = 0;
    for (int i = 0; i < g_nscript; i++) {
        uint64_t at = g_script[i].by_time ? now_us : frame;
        if (at >= g_script[i].frame && at < g_script[i].until) {
            b |= g_script[i].buttons;
            if (g_script[i].lx != 128 || g_script[i].ly != 128) { *lx = g_script[i].lx; *ly = g_script[i].ly; }
        }
    }
    return b;
}

#ifdef GOW_HAVE_SDL
#define SDL_MAIN_HANDLED  /* the runtime keeps its own main() */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static SDL_Gamepad *g_pads[8];
static int g_quit, g_fullscreen;
static uint64_t g_wall0, g_virt0, g_last_pump, g_title_t, g_title_frame, g_last_frame;
static int g_anchored;
static uint32_t g_rgba[DISP_W * DISP_H];
static uint32_t g_shot_addr, g_shot_stride, g_shot_format;

static uint64_t now_us(void) { return SDL_GetTicksNS() / 1000; }

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
    if (rt_frames_write_png(path, g_shot_addr, g_shot_stride, g_shot_format)) printf("[host] screenshot %s\n", path);
}

static void pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT: g_quit = 1; break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break;
            if (e.key.key == SDLK_ESCAPE) g_quit = 1;
            else if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
                g_fullscreen = !g_fullscreen;
                SDL_SetWindowFullscreen(g_win, g_fullscreen);
            } else if (e.key.key == SDLK_F12) screenshot();
            break;
        case SDL_EVENT_GAMEPAD_ADDED: open_pad(e.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_REMOVED: close_pad(e.gdevice.which); break;
        default: break;
        }
    }
    g_last_pump = now_us();
}

int rt_host_init(const HostOptions *o) {
    g_o = *o;
    if (g_o.input && !parse_script(g_o.input)) return 0;
    if (!g_o.window) return 1;
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_APP_NAME, "God of War PSP native port");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { fprintf(stderr, "[host] SDL_Init: %s\n", SDL_GetError()); return 0; }
    int scale = g_o.scale > 0 ? g_o.scale : 2;
    g_win = SDL_CreateWindow("God of War (PSP native port)", DISP_W * scale, DISP_H * scale, SDL_WINDOW_RESIZABLE);
    if (!g_win) { fprintf(stderr, "[host] SDL_CreateWindow: %s\n", SDL_GetError()); return 0; }
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
           "IJKL = stick, F11 fullscreen, F12 screenshot, Esc quit\n", DISP_W * scale, DISP_H * scale, SDL_GetRendererName(g_ren));
    g_title_t = now_us();
    pump();
    return 1;
}

void rt_host_shutdown(void) {
    if (!g_win) return;
    for (int i = 0; i < 8; i++) if (g_pads[i]) SDL_CloseGamepad(g_pads[i]);
    SDL_DestroyTexture(g_tex);
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    g_win = NULL;
}

/* PSP framebuffer -> texture. 8888 is uploaded directly (red in the low byte = XBGR8888). */
int rt_host_present(uint64_t frame, uint32_t addr, uint32_t stride, uint32_t format) {
    g_last_frame = frame;
    if (!g_win) return 0;
    if (addr && stride) {
        g_shot_addr = addr; g_shot_stride = stride; g_shot_format = format;
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
    SDL_RenderClear(g_ren);
    SDL_RenderTexture(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
    pump();
    uint64_t t = now_us();
    if (t - g_title_t >= 1000000) {
        char title[160];
        double fps = (frame - g_title_frame) * 1e6 / (double)(t - g_title_t);
        snprintf(title, sizeof(title), "God of War (PSP native port)  |  %.1f fps  |  frame %llu", fps, (unsigned long long)frame);
        SDL_SetWindowTitle(g_win, title);
        g_title_t = t;
        g_title_frame = frame;
    }
    return g_quit;
}

int rt_host_idle(uint64_t now_virt, uint64_t next_virt) {
    if (!g_win) return 0;
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
    } else if (ahead < -100000) {
        /* more than 100 ms behind: run on from here rather than racing to catch up */
        g_wall0 = wall;
        g_virt0 = next_virt;
    }
    if (now_us() - g_last_pump > 16000) pump();
    return g_quit;
}

uint32_t rt_host_buttons(uint64_t frame, uint64_t now_us, uint8_t *lx, uint8_t *ly) {
    int ax = 128, ay = 128;
    uint32_t b = script_buttons(frame, now_us, &ax, &ay);
    if (g_win) {
        const bool *k = SDL_GetKeyboardState(NULL);
        static const struct { SDL_Scancode sc; uint32_t bit; } keys[] = {
            { SDL_SCANCODE_UP, PSP_UP }, { SDL_SCANCODE_DOWN, PSP_DOWN }, { SDL_SCANCODE_LEFT, PSP_LEFT },
            { SDL_SCANCODE_RIGHT, PSP_RIGHT }, { SDL_SCANCODE_Z, PSP_CROSS }, { SDL_SCANCODE_X, PSP_CIRCLE },
            { SDL_SCANCODE_A, PSP_SQUARE }, { SDL_SCANCODE_S, PSP_TRIANGLE }, { SDL_SCANCODE_Q, PSP_LTRIGGER },
            { SDL_SCANCODE_W, PSP_RTRIGGER }, { SDL_SCANCODE_SPACE, PSP_START }, { SDL_SCANCODE_RETURN, PSP_START },
            { SDL_SCANCODE_V, PSP_SELECT },
        };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) if (k[keys[i].sc]) b |= keys[i].bit;
        if (b & PSP_START && (SDL_GetModState() & SDL_KMOD_ALT)) b &= ~(uint32_t)PSP_START;  /* Alt+Enter is fullscreen */
        if (k[SDL_SCANCODE_J]) ax = 0;
        if (k[SDL_SCANCODE_L]) ax = 255;
        if (k[SDL_SCANCODE_I]) ay = 0;
        if (k[SDL_SCANCODE_K]) ay = 255;
        static const struct { SDL_GamepadButton gb; uint32_t bit; } pad[] = {
            { SDL_GAMEPAD_BUTTON_SOUTH, PSP_CROSS }, { SDL_GAMEPAD_BUTTON_EAST, PSP_CIRCLE },
            { SDL_GAMEPAD_BUTTON_WEST, PSP_SQUARE }, { SDL_GAMEPAD_BUTTON_NORTH, PSP_TRIANGLE },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PSP_LTRIGGER }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PSP_RTRIGGER },
            { SDL_GAMEPAD_BUTTON_START, PSP_START }, { SDL_GAMEPAD_BUTTON_BACK, PSP_SELECT },
            { SDL_GAMEPAD_BUTTON_DPAD_UP, PSP_UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, PSP_DOWN },
            { SDL_GAMEPAD_BUTTON_DPAD_LEFT, PSP_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PSP_RIGHT },
        };
        for (int i = 0; i < 8; i++) {
            SDL_Gamepad *gp = g_pads[i];
            if (!gp) continue;
            for (size_t j = 0; j < sizeof(pad) / sizeof(pad[0]); j++) if (SDL_GetGamepadButton(gp, pad[j].gb)) b |= pad[j].bit;
            if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) b |= PSP_LTRIGGER;
            if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) b |= PSP_RTRIGGER;
            int x = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY);
            if (abs(x) > 6000 || abs(y) > 6000) { ax = (x + 32768) >> 8; ay = (y + 32768) >> 8; }
        }
    }
    *lx = (uint8_t)ax;
    *ly = (uint8_t)ay;
    return b;
}

#else  /* built without SDL: headless only, scripted input */

int rt_host_init(const HostOptions *o) {
    g_o = *o;
    if (g_o.input && !parse_script(g_o.input)) return 0;
    if (g_o.window) { fprintf(stderr, "[host] this build has no window support (SDL3 not linked)\n"); return 0; }
    return 1;
}
void rt_host_shutdown(void) {}
int rt_host_present(uint64_t frame, uint32_t addr, uint32_t stride, uint32_t format) { (void)frame; (void)addr; (void)stride; (void)format; return 0; }
int rt_host_idle(uint64_t now_us, uint64_t next_us) { (void)now_us; (void)next_us; return 0; }
uint32_t rt_host_buttons(uint64_t frame, uint64_t now_us, uint8_t *lx, uint8_t *ly) {
    int ax = 128, ay = 128;
    uint32_t b = script_buttons(frame, now_us, &ax, &ay);
    *lx = (uint8_t)ax; *ly = (uint8_t)ay;
    return b;
}
#endif
