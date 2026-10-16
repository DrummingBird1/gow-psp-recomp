/* Frame output: PNG snapshots, ffmpeg video recording and a host frame-time log.
 * Two recordings are possible: on the game clock (frames taken on virtual vblanks, so the
 * video plays at the game's own speed whatever the host speed) and on the host clock (the
 * frame on screen once per 1/59.94 s of real time, so the video shows how smoothly this
 * machine runs the game). The CSV records how long the host took for each frame. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define popen _popen
#define pclose _pclose
#else
#include <time.h>
#endif

#include "psp_rt.h"
#include "rt_frames.h"
#include "rt_gpu.h"

#define DISP_W 480
#define DISP_H 272
#define VBLANK_US 16683u

/* A recording: frames go to ffmpeg once per 1/59.94 s of its clock (game or host time). */
typedef struct { FILE *pipe; uint64_t out; int host_clock; } Stream;

uint32_t rt_max_frames;
static FrameOptions g_o;
static int listed(const char *list, uint64_t frame);
static void capture(uint64_t frame, uint32_t addr, uint32_t stride, uint32_t format);
static uint64_t g_now;  /* virtual time of the frame being shown */
static Stream g_streams[2];
static FILE *g_csv;
static uint8_t g_shown[DISP_W * DISP_H * 3];  /* frame currently on screen, RGB */
static int g_have_shown;
static uint64_t g_presents, g_t0, g_tprev;
static double g_ft_sum, g_ft_max;
#define FT_BUCKETS 2000
static uint32_t g_ft_hist[FT_BUCKETS];  /* host frame times in 1 ms buckets (last = 1999 ms and more) */

static uint64_t host_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000.0 / f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

/* PSP framebuffer formats: 0 = 565, 1 = 5551, 2 = 4444, 3 = 8888 (red in the low bits) */
static void to_rgb(uint8_t *out, uint32_t addr, uint32_t stride, uint32_t format) {
    for (int y = 0; y < DISP_H; y++) {
        uint8_t *o = out + y * DISP_W * 3;
        if (format == 3) {
            const uint8_t *p = rt_ptr(addr + (uint32_t)y * stride * 4);
            for (int x = 0; x < DISP_W; x++, p += 4, o += 3) { o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; }
            continue;
        }
        const uint8_t *p = rt_ptr(addr + (uint32_t)y * stride * 2);
        for (int x = 0; x < DISP_W; x++, p += 2, o += 3) {
            uint32_t v = p[0] | (p[1] << 8), r, g, b;
            if (format == 0) { r = v & 31; g = (v >> 5) & 63; b = (v >> 11) & 31; r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2); }
            else if (format == 1) { r = v & 31; g = (v >> 5) & 31; b = (v >> 10) & 31; r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2); }
            else { r = (v & 15) * 17; g = ((v >> 4) & 15) * 17; b = ((v >> 8) & 15) * 17; }
            o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)b;
        }
    }
}

/* --- PNG writer (zlib stored blocks: no compression library needed) ----------- */
static uint32_t crc_table[256];
static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t h[8];
    put32(h, n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t c = crc32_update(0xFFFFFFFFu, h + 4, 4);
    c = crc32_update(c, data, n) ^ 0xFFFFFFFFu;
    put32(h, c);
    fwrite(h, 1, 4, f);
}
static int write_png_rgb(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t raw_n = (size_t)h * (1 + w * 3), blocks = (raw_n + 65534) / 65535;
    size_t z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t *raw = malloc(raw_n), *z = malloc(z_n), ihdr[13];
    for (int y = 0; y < h; y++) {
        raw[y * (1 + w * 3)] = 0;  /* filter: none */
        memcpy(raw + y * (1 + w * 3) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t pos = 0; pos < raw_n; pos += 65535) {
        uint32_t n = (uint32_t)(raw_n - pos < 65535 ? raw_n - pos : 65535);
        z[o++] = pos + n == raw_n;
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + pos, n);
        o += n;
    }
    put32(z + o, (b << 16) | a);
    o += 4;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    put32(ihdr, (uint32_t)w); put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;  /* 8-bit RGB */
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)o);
    chunk(f, "IEND", NULL, 0);
    free(raw); free(z);
    return fclose(f) == 0;
}

int rt_frames_write_png(const char *path, uint32_t addr, uint32_t stride, uint32_t format) {
    static uint8_t rgb[DISP_W * DISP_H * 3];
    to_rgb(rgb, addr, stride, format);
    return write_png_rgb(path, rgb, DISP_W, DISP_H);
}

int rt_frames_write_png_rgba(const char *path, const uint8_t *rgba) {
    static uint8_t rgb[DISP_W * DISP_H * 3];
    for (int i = 0; i < DISP_W * DISP_H; i++) { rgb[3 * i] = rgba[4 * i]; rgb[3 * i + 1] = rgba[4 * i + 1]; rgb[3 * i + 2] = rgba[4 * i + 2]; }
    return write_png_rgb(path, rgb, DISP_W, DISP_H);
}

void rt_frames_compare_gpu(uint64_t frame, uint32_t addr, uint32_t stride, uint32_t format, const char *dir) {
    static uint8_t gpu[DISP_W * DISP_H * 4], soft[DISP_W * DISP_H * 3], out[DISP_W * 3 * DISP_H * 3];
    if (!rt_gpu_read_target(addr, stride, format, gpu)) {
        printf("[compare] frame %llu: the GPU has no target for %08X\n", (unsigned long long)frame, addr);
        return;
    }
    to_rgb(soft, addr, stride, format);
    double sum = 0, sq = 0;
    int over8 = 0, over32 = 0;
    for (int y = 0; y < DISP_H; y++)
        for (int x = 0; x < DISP_W; x++) {
            int i = y * DISP_W + x, worst = 0;
            uint8_t *o = out + ((size_t)y * DISP_W * 3 + x) * 3;
            for (int ch = 0; ch < 3; ch++) {
                int s = soft[3 * i + ch], g = gpu[4 * i + ch], d = abs(s - g);
                sum += d;
                sq += (double)d * d;
                if (d > worst) worst = d;
                o[ch] = (uint8_t)s;
                o[DISP_W * 3 + ch] = (uint8_t)g;
                o[DISP_W * 6 + ch] = (uint8_t)(d * 4 > 255 ? 255 : d * 4);
            }
            over8 += worst > 8;
            over32 += worst > 32;
        }
    double n = (double)DISP_W * DISP_H, mse = sq / (n * 3);
    char path[1024];
    snprintf(path, sizeof(path), "%s/compare_%06llu.png", dir ? dir : ".", (unsigned long long)frame);
    write_png_rgb(path, out, DISP_W * 3, DISP_H);
    printf("[compare] frame %llu: mean |diff| %.2f, %.2f%% of pixels differ by >8, %.2f%% by >32, PSNR %.1f dB -> %s\n",
           (unsigned long long)frame, sum / (n * 3), 100.0 * over8 / n, 100.0 * over32 / n,
           mse > 0 ? 10.0 * log10(255.0 * 255.0 / mse) : 99.0, path);
}

void rt_frames_after_present(uint64_t frame, uint32_t addr, uint32_t stride, uint32_t format) {
    if (g_o.from_gpu) capture(frame, addr, stride, format);
    if (g_o.gpu_compare_at && listed(g_o.gpu_compare_at, frame) && addr && stride)
        rt_frames_compare_gpu(frame, addr, stride, format, g_o.png_dir);
}

static int listed(const char *list, uint64_t frame) {
    for (const char *p = list; p && *p;) {
        char *end;
        unsigned long long v = strtoull(p, &end, 10);
        if (end == p) break;
        if (v == frame) return 1;
        p = *end == ',' ? end + 1 : end;
    }
    return 0;
}

static void open_stream(Stream *st, const char *path, int host_clock) {
    char cmd[1024];
    st->host_clock = host_clock;
    if (!path) return;
    /* the outer quotes keep cmd.exe from stripping the quotes around the paths */
    snprintf(cmd, sizeof(cmd),
             "\"\"%s\" -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgb24 -s %dx%d -framerate 60000/1001 -i - "
             "-vf scale=%d:%d:flags=neighbor -c:v libx264 -preset veryfast -crf 16 -pix_fmt yuv420p \"%s\"\"",
             g_o.ffmpeg ? g_o.ffmpeg : "ffmpeg", DISP_W, DISP_H, DISP_W * 2, DISP_H * 2, path);
    st->pipe = popen(cmd, "wb");
    if (!st->pipe) fprintf(stderr, "[frames] could not start ffmpeg for %s\n", path);
}

void rt_frames_init(const FrameOptions *o) {
    g_o = *o;
    open_stream(&g_streams[0], g_o.video, 0);
    open_stream(&g_streams[1], g_o.video_host, 1);
    if (g_o.perf_csv && (g_csv = fopen(g_o.perf_csv, "w")) != NULL)
        fprintf(g_csv, "frame,virtual_ms,host_ms,host_frame_ms\n");
    g_t0 = g_tprev = host_us();
}

/* emits the frame on screen once per 1/59.94 s that has passed on each stream's clock */
static void video_catch_up(uint64_t game_us) {
    for (int i = 0; i < 2; i++) {
        Stream *st = &g_streams[i];
        uint64_t due = (st->host_clock ? host_us() - g_t0 : game_us) / VBLANK_US;
        if (!st->pipe || !g_have_shown) { st->out = due; continue; }
        for (; st->out < due; st->out++) fwrite(g_shown, 1, sizeof(g_shown), st->pipe);
    }
}

void rt_frames_present(uint64_t frame, uint64_t now_us, uint32_t addr, uint32_t stride, uint32_t format) {
    uint64_t t = host_us();
    double ft = (t - g_tprev) / 1000.0;
    g_tprev = t;
    if (g_presents++) {
        g_ft_sum += ft;
        if (ft > g_ft_max) g_ft_max = ft;
        g_ft_hist[ft >= FT_BUCKETS - 1 ? FT_BUCKETS - 1 : (int)ft]++;
    }
    if (g_csv) fprintf(g_csv, "%llu,%.3f,%.3f,%.3f\n", (unsigned long long)frame, now_us / 1000.0, (t - g_t0) / 1000.0, ft);
    g_now = now_us;
    if (g_o.from_gpu) return;  /* captured in rt_frames_after_present, once the GPU drew it */
    capture(frame, addr, stride, format);
}

static void capture(uint64_t frame, uint32_t addr, uint32_t stride, uint32_t format) {
    if (!addr || !stride) return;
    int want_png = g_o.png_dir && ((g_o.png_every && frame % g_o.png_every == 0) || listed(g_o.png_at, frame));
    if (!g_streams[0].pipe && !g_streams[1].pipe && !want_png) return;
    video_catch_up(g_now);
    if (g_o.from_gpu) {
        static uint8_t rgba[DISP_W * DISP_H * 4];
        if (!rt_gpu_read_target(addr, stride, format, rgba)) return;
        for (int i = 0; i < DISP_W * DISP_H; i++)
            for (int ch = 0; ch < 3; ch++) g_shown[3 * i + ch] = rgba[4 * i + ch];
    } else {
        to_rgb(g_shown, addr, stride, format);
    }
    g_have_shown = 1;
    if (want_png) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/frame_%06llu.png", g_o.png_dir, (unsigned long long)frame);
        if (!write_png_rgb(path, g_shown, DISP_W, DISP_H)) fprintf(stderr, "[frames] could not write %s\n", path);
    }
}

void rt_frames_close(uint64_t now_us) {
    video_catch_up(now_us);
    for (int i = 0; i < 2; i++)
        if (g_streams[i].pipe) { pclose(g_streams[i].pipe); g_streams[i].pipe = NULL; }
    if (g_csv) { fclose(g_csv); g_csv = NULL; }
    if (g_presents > 1) {
        uint64_t n = g_presents - 1, seen = 0;
        double p50 = 0, p99 = 0;
        for (int i = 0; i < FT_BUCKETS; i++) {
            seen += g_ft_hist[i];
            if (!p50 && seen * 2 >= n) p50 = i + 1;
            if (!p99 && seen * 100 >= n * 99) p99 = i + 1;
        }
        double host_s = (g_tprev - g_t0) / 1e6;
        printf("[perf] %llu frames in %.2f s host time: %.1f fps average (frame time avg %.2f ms, p50 <%.0f ms, p99 <%.0f ms, max %.1f ms); "
               "virtual/host speed %.2fx\n",
               (unsigned long long)g_presents, host_s, n / host_s, g_ft_sum / n, p50, p99, g_ft_max,
               host_s > 0 ? now_us / 1e6 / host_s : 0);
    }
}
