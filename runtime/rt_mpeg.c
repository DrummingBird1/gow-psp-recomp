/* sceMpeg: PSMF movie playback. Video is H.264 decoded by OpenH264 (rt_h264dec.cpp), audio is
 * ATRAC3+ decoded by at3_standalone (rt_at3dec.cpp), the same decoder sceAtrac3plus uses.
 *
 * What the game can see follows PPSSPP (Core/HLE/sceMpeg.cpp, v1.20.4): sizes, the ring buffer
 * fields, the access unit (AU) structures and the delays of each call, which let virtual time
 * advance as the PSP's media engine takes it. The demuxer is our own, written for how PSMF is
 * stored: an MPEG-2 program stream of 2048-byte packs, each holding one PES packet (video
 * 0xE0, ATRAC3+ in private stream 1 0xBD with a 4-byte header: channel, then 3 bytes), padding
 * or the system header. Packets leave the ring only when an AU is asked for, so the game sees
 * the ring drain at the pace the movie plays and refills it from its file (through its own
 * ring buffer callback); a movie is over once the file is read and the ring is empty.
 *
 *  - Video AUs start with an access unit delimiter (NAL type 9), which every PSMF picture has.
 *    Only some PES packets carry a PTS (one every 15-18 pictures in Chains of Olympus); as in
 *    MPEG, it belongs to the first AU starting in that packet, and the other AUs get -1, which
 *    the game's player expects (it counts on from the last timestamp).
 *  - ATRAC3+ frames in the stream carry an 8-byte header, 0F D0 c1 c2 00 00 00 00: c1 gives the
 *    channels ((c1 >> 2) & 7), and the frame is ((c1 & 3) << 8 | c2) * 8 + 8 bytes after it.
 *    sceMpegAtracDecode gives 2048 stereo samples (8192 bytes).
 *  - Pictures go to the game's buffer as sceMpegAvcDecode asks: frame width in pixels, pixel
 *    mode (8888 unless sceMpegAvcDecodeMode says otherwise), BT.601 limited range, alpha 0 as
 *    PPSSPP and the PSP write it.
 *
 * Structures in PSP memory (PPSSPP sceMpeg.h):
 *   SceMpegRingBuffer: packets, packetsRead, packetsWritePos, packetsAvail, packetSize,
 *     data, callback_addr, callback_args, dataUpperBound, semaID, mpeg, gp
 *   SceMpegAu: pts (s64, high word first), dts (s64), esBuffer, esSize */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_at3dec.h"
#include "rt_frames.h"
#include "rt_ge.h"
#include "rt_h264dec.h"
#include "rt_kernel.h"

#define MPEG_MEMSIZE 0x10000u
#define MPEG_AVC_ES_SIZE 2048u
#define MPEG_ATRAC_ES_SIZE 2112u
#define MPEG_ATRAC_ES_OUTPUT_SIZE 8192u
#define MPEG_ES_BUFFERS 2
#define PACKET 2048u
#define PSMF_MAGIC 0x464D5350u  /* "PSMF" */
#define ERR_NO_DATA 0x80618001u
#define ERR_INVALID_VALUE 0x806101FEu
#define ERR_BAD_VERSION 0x80610002u
#define ERR_NO_MEMORY 0x80610022u
#define ERR_AVC_DECODE_FATAL 0x80628002u
#define NO_TS ((int64_t)-1)

/* PPSSPP's delays, in microseconds of virtual time */
#define DELAY_GET_AU 100
#define DELAY_ATRAC_DECODE 3000
#define DELAY_AVC_FIRST 3600
#define DELAY_AVC_DECODE 5400
#define DELAY_AVC_EMPTY 320

enum { RB_PACKETS = 0, RB_READ = 4, RB_WRITEPOS = 8, RB_AVAIL = 12, RB_PACKETSIZE = 16, RB_DATA = 20,
       RB_CALLBACK = 24, RB_CBARG = 28, RB_UPPER = 32, RB_MPEG = 40 };
enum { STREAM_AVC = 0, STREAM_ATRAC = 1, STREAM_PCM = 2, STREAM_DATA = 3, STREAM_AUDIO = 15 };
enum { PIX_565 = 0, PIX_5551 = 1, PIX_4444 = 2, PIX_8888 = 3 };

/* elementary stream bytes; base is the stream position of p[0] */
typedef struct { uint8_t *p; size_t len, cap; uint64_t base; } Es;
/* a PES packet that carried a timestamp: its payload is [pos, end) of the elementary stream */
typedef struct { uint64_t pos, end; int64_t pts, dts; } Mark;
typedef struct { Mark m[64]; int head, n; } Marks;

#define MAX_STREAMS 8
typedef struct { int sid, type, num; } Stream;

typedef struct Mpeg {
    int used;
    uint32_t handle, rb;
    uint32_t frame_width;      /* sceMpegCreate's frame width (used when a call passes 0) */
    int pixel_mode;
    int es_used[MPEG_ES_BUFFERS];
    Stream streams[MAX_STREAMS];
    /* PSMF header (sceMpegQueryStreamOffset) */
    uint32_t stream_offset, stream_size;
    int64_t first_ts, last_ts;
    int width, height;
    /* ring buffer: packets the game put and packets demuxed so far */
    uint32_t put, taken;
    int end_code;              /* the program end code was seen */
    const uint8_t *(*next_packet)(struct Mpeg *m);
    const uint8_t *file;       /* --movie-test: the stream comes from a file */
    /* demuxer */
    int video_id, audio_ch;    /* PES id of the video stream; ATRAC3+ channel (-1: none registered) */
    Es ves, aes;
    Marks vmarks, amarks;
    int vsync;                 /* the video ES starts at an access unit delimiter */
    size_t vscan;              /* where to look for the next delimiter */
    /* the AUs sceMpegGet*Au found, until sceMpegAvcDecode / sceMpegAtracDecode take them */
    Es vau, aau;
    int have_vau, have_aau;
    int64_t vau_pts, vau_dts, aau_pts;
    RtH264Dec *h264;
    RtAt3Dec *at3;
    int at3_channels, at3_align;
    uint32_t video_frames, audio_frames, errors;
    int logged_end;
} Mpeg;

static Mpeg g_mpeg;
static uint32_t g_stream_id = 1;
static uint64_t g_total_video, g_total_audio, g_total_errors, g_movies;

static void delay_us(uint64_t us) { k_wait(WT_DELAY, 0, us); }

static void write_au(uint32_t addr, int64_t pts, int64_t dts, uint32_t es_buffer, uint32_t es_size) {
    rt_w32(addr, (uint32_t)((uint64_t)pts >> 32)); rt_w32(addr + 4, (uint32_t)pts);
    rt_w32(addr + 8, (uint32_t)((uint64_t)dts >> 32)); rt_w32(addr + 12, (uint32_t)dts);
    rt_w32(addr + 16, es_buffer); rt_w32(addr + 20, es_size);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int64_t ts48(const uint8_t *p) { return (int64_t)((uint64_t)be32(p) << 16 | (uint64_t)p[4] << 8 | p[5]); }

/* PSP memory the decoder may write: main RAM or VRAM */
static int valid_range(uint32_t addr, uint32_t size) {
    uint32_t a = addr & 0x1FFFFFFFu;
    if (a >= 0x04000000u && a + size <= 0x04200000u) return 1;
    return a >= 0x08000000u && a + size <= 0x0A000000u;
}

/* --- elementary stream buffers ------------------------------------------------------ */
static void es_append(Es *e, const uint8_t *p, size_t n) {
    if (e->len + n > e->cap) {
        size_t cap = e->cap ? e->cap : 65536;
        while (cap < e->len + n) cap *= 2;
        e->p = (uint8_t *)realloc(e->p, cap);
        e->cap = cap;
    }
    memcpy(e->p + e->len, p, n);
    e->len += n;
}

static void es_drop(Es *e, size_t n) {
    if (n > e->len) n = e->len;
    memmove(e->p, e->p + n, e->len - n);
    e->len -= n;
    e->base += n;
}

static void es_reset(Es *e) { e->len = 0; e->base = 0; }
static void es_free(Es *e) { free(e->p); memset(e, 0, sizeof(*e)); }

static void mark_add(Marks *k, uint64_t pos, uint64_t end, int64_t pts, int64_t dts) {
    if (k->n == 64) { k->head = (k->head + 1) % 64; k->n--; }  /* never claimed: drop the oldest */
    Mark *m = &k->m[(k->head + k->n++) % 64];
    m->pos = pos; m->end = end; m->pts = pts; m->dts = dts;
}

/* The timestamp of the AU starting at stream position pos: that of the PES packet it starts in,
 * if the packet had one and no earlier AU took it. */
static int64_t mark_take(Marks *k, uint64_t pos, int64_t *dts) {
    *dts = NO_TS;
    while (k->n && k->m[k->head].end <= pos) { k->head = (k->head + 1) % 64; k->n--; }
    if (!k->n || k->m[k->head].pos > pos) return NO_TS;
    Mark *m = &k->m[k->head];
    k->head = (k->head + 1) % 64;
    k->n--;
    *dts = m->dts;
    return m->pts;
}

/* --- demuxer ---------------------------------------------------------------------------- */
static int64_t read_ts(const uint8_t *p) {
    return (int64_t)((uint64_t)((p[0] >> 1) & 7) << 30 | (uint64_t)p[1] << 22 | (uint64_t)(p[2] >> 1) << 15 |
                     (uint64_t)p[3] << 7 | (uint64_t)(p[4] >> 1));
}

/* One PES packet (id, the n bytes after its length field) */
static void demux_pes(Mpeg *m, int id, const uint8_t *b, size_t n) {
    int64_t pts = NO_TS, dts = NO_TS;
    size_t i = 0;
    if (n >= 3 && (b[0] & 0xC0) == 0x80) {  /* MPEG-2 */
        int flags = b[1];
        size_t hl = b[2];
        if ((flags & 0x80) && hl >= 5 && n >= 8) pts = dts = read_ts(b + 3);
        if ((flags & 0x40) && hl >= 10 && n >= 13) dts = read_ts(b + 8);
        i = 3 + hl;
    } else {  /* MPEG-1: stuffing, buffer size, timestamps */
        while (i < n && b[i] == 0xFF) i++;
        if (i < n && (b[i] & 0xC0) == 0x40) i += 2;
        if (i < n && (b[i] & 0xE0) == 0x20 && i + 5 <= n) {
            pts = dts = read_ts(b + i);
            if ((b[i] & 0x10) && i + 10 <= n) { dts = read_ts(b + i + 5); i += 5; }
            i += 5;
        } else if (i < n) i++;
    }
    if (i >= n) return;
    const uint8_t *pay = b + i;
    size_t len = n - i;
    Es *e;
    Marks *k;
    if (id == 0xBD) {
        if (len < 4 || pay[0] != m->audio_ch) return;  /* another language or a PCM/data stream */
        pay += 4; len -= 4;
        e = &m->aes; k = &m->amarks;
    } else {
        e = &m->ves; k = &m->vmarks;
    }
    if (pts != NO_TS) mark_add(k, e->base + e->len, e->base + e->len + len, pts, dts);
    es_append(e, pay, len);
}

/* One 2048-byte packet: a pack header, then PES packets (or the program end code) */
static void demux_packet(Mpeg *m, const uint8_t *p) {
    size_t i = 0;
    while (i + 4 <= PACKET) {
        if (p[i] || p[i + 1] || p[i + 2] != 1) { i++; continue; }
        int id = p[i + 3];
        if (id == 0xBA) {  /* pack header: MPEG-2 (14 bytes and stuffing) or MPEG-1 (12) */
            if (i + 14 > PACKET) break;
            i += (p[i + 4] & 0xC0) == 0x40 ? 14 + (size_t)(p[i + 13] & 7) : 12;
            continue;
        }
        if (id == 0xB9) { m->end_code = 1; break; }
        if (id < 0xBB) { i += 4; continue; }
        if (i + 6 > PACKET) break;
        size_t len = (size_t)p[i + 4] << 8 | p[i + 5], end = i + 6 + len;
        if (end > PACKET) end = PACKET;
        if (id == m->video_id || id == 0xBD) demux_pes(m, id, p + i + 6, end - i - 6);
        i = end;
    }
}

/* The next packet from the game's ring buffer, or NULL when it holds none */
static const uint8_t *ring_packet(Mpeg *m) {
    if (!m->rb || m->taken >= m->put) return NULL;
    uint32_t packets = rt_r32(m->rb + RB_PACKETS);
    if (!packets) return NULL;
    uint32_t addr = rt_r32(m->rb + RB_DATA) + (m->taken % packets) * PACKET;
    m->taken++;
    rt_w32(m->rb + RB_AVAIL, m->put - m->taken);
    return rt_ptr(addr);
}

static const uint8_t *file_packet(Mpeg *m) {
    if ((uint64_t)m->taken * PACKET >= m->stream_size) return NULL;
    return m->file + m->stream_offset + (size_t)m->taken++ * PACKET;
}

/* Is everything the stream holds already demuxed? */
static int stream_done(const Mpeg *m) {
    return m->end_code || (m->stream_size && (uint64_t)m->taken * PACKET >= m->stream_size);
}

static size_t find_aud(const Es *e, size_t from) {
    for (size_t i = from; i + 4 <= e->len; i++)
        if (!e->p[i] && !e->p[i + 1] && e->p[i + 2] == 1 && (e->p[i + 3] & 0x1F) == 9) return i;
    return (size_t)-1;
}

static void take_video_au(Mpeg *m, size_t n) {
    Es *e = &m->ves;
    if (n < e->len && n > 4 && !e->p[n - 1]) n--;  /* a 4-byte start code's zero goes with the next AU */
    m->vau.len = 0;
    es_append(&m->vau, e->p, n);
    m->vau_pts = mark_take(&m->vmarks, e->base, &m->vau_dts);
    es_drop(e, n);
    m->vscan = 4;
    m->have_vau = 1;
}

/* Finds the next video AU (into m->vau), demuxing packets as needed; 0 when the ring runs out first. */
static int next_video_au(Mpeg *m) {
    Es *e = &m->ves;
    for (;;) {
        if (!m->vsync) {
            size_t a = find_aud(e, 0);
            if (a != (size_t)-1) { es_drop(e, a); m->vsync = 1; m->vscan = 4; }
            else if (e->len > 3) es_drop(e, e->len - 3);
        }
        if (m->vsync) {
            size_t b = find_aud(e, m->vscan);
            if (b != (size_t)-1) { take_video_au(m, b); return 1; }
            m->vscan = e->len > 7 ? e->len - 3 : 4;
        }
        const uint8_t *pk = m->next_packet(m);
        if (pk) { demux_packet(m, pk); continue; }
        if (m->vsync && stream_done(m) && e->len > 4) { take_video_au(m, e->len); return 1; }
        return 0;
    }
}

/* Finds the next ATRAC3+ frame (header included, into m->aau); 0 when the ring runs out first. */
static int next_audio_au(Mpeg *m) {
    Es *e = &m->aes;
    for (;;) {
        size_t i = 0;
        while (i + 1 < e->len && !(e->p[i] == 0x0F && e->p[i + 1] == 0xD0)) i++;
        if (i + 1 >= e->len && e->len && e->p[e->len - 1] != 0x0F) i = e->len;
        if (i) es_drop(e, i);
        if (e->len >= 8) {
            size_t size = 8 + ((((size_t)e->p[2] & 3) << 8 | e->p[3]) * 8 + 8);
            if (e->len >= size) {
                int64_t dts;
                m->aau.len = 0;
                es_append(&m->aau, e->p, size);
                m->aau_pts = mark_take(&m->amarks, e->base, &dts);
                es_drop(e, size);
                m->have_aau = 1;
                return 1;
            }
        }
        const uint8_t *pk = m->next_packet(m);
        if (!pk) return 0;
        demux_packet(m, pk);
    }
}

static void demux_reset(Mpeg *m) {
    es_reset(&m->ves); es_reset(&m->aes);
    memset(&m->vmarks, 0, sizeof(m->vmarks));
    memset(&m->amarks, 0, sizeof(m->amarks));
    m->vsync = 0; m->vscan = 4;
    m->have_vau = m->have_aau = 0;
    m->put = m->taken = 0;
    m->end_code = 0;
    m->logged_end = 0;
    rt_h264dec_close(m->h264); m->h264 = NULL;
    rt_at3dec_close(m->at3); m->at3 = NULL;
}

static void mpeg_free(Mpeg *m) {
    demux_reset(m);
    es_free(&m->ves); es_free(&m->aes); es_free(&m->vau); es_free(&m->aau);
    memset(m, 0, sizeof(*m));
}

/* --- decoding ----------------------------------------------------------------------- */
static uint8_t clamp255(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* One output row from I420: BT.601 limited range (as PPSSPP's swscale setup), chroma upsampled
 * bilinearly for the H.264 default siting (co-sited horizontally, between rows vertically). */
static void yuv_row(const RtH264Pic *pic, int y, int w, int mode, uint8_t *out) {
    const uint8_t *py = pic->y + (size_t)y * pic->stride_y;
    int ch = (pic->height + 1) / 2, cy = y >> 1, cy2 = (y & 1) ? cy + 1 : cy - 1;
    if (cy2 < 0) cy2 = 0;
    if (cy2 >= ch) cy2 = ch - 1;
    const uint8_t *u0 = pic->u + (size_t)cy * pic->stride_uv, *u1 = pic->u + (size_t)cy2 * pic->stride_uv;
    const uint8_t *v0 = pic->v + (size_t)cy * pic->stride_uv, *v1 = pic->v + (size_t)cy2 * pic->stride_uv;
    int cw = (pic->width + 1) / 2;
    for (int x = 0; x < w; x++) {
        int cx = x >> 1, cx2 = (x & 1) && cx + 1 < cw ? cx + 1 : cx;
        /* vertical 3:1, then horizontal 1:1 for odd columns: weights out of 8 */
        int u = (3 * u0[cx] + u1[cx]) + (3 * u0[cx2] + u1[cx2]);
        int v = (3 * v0[cx] + v1[cx]) + (3 * v0[cx2] + v1[cx2]);
        int d = ((u + 4) >> 3) - 128, e = ((v + 4) >> 3) - 128, c = (py[x] - 16) * 76309;
        uint8_t r = clamp255((c + 104597 * e + 32768) >> 16);
        uint8_t g = clamp255((c - 25675 * d - 53279 * e + 32768) >> 16);
        uint8_t b = clamp255((c + 132201 * d + 32768) >> 16);
        switch (mode) {
        case PIX_565: { uint16_t p = (uint16_t)(r >> 3 | (g >> 2) << 5 | (b >> 3) << 11); memcpy(out + x * 2, &p, 2); break; }
        case PIX_5551: { uint16_t p = (uint16_t)(r >> 3 | (g >> 3) << 5 | (b >> 3) << 10); memcpy(out + x * 2, &p, 2); break; }
        case PIX_4444: { uint16_t p = (uint16_t)(r >> 4 | (g >> 4) << 4 | (b >> 4) << 8); memcpy(out + x * 2, &p, 2); break; }
        default: out[x * 4] = r; out[x * 4 + 1] = g; out[x * 4 + 2] = b; out[x * 4 + 3] = 0; break;
        }
    }
}

/* Decodes the AU in m->vau; 1 with a picture */
static int decode_video(Mpeg *m, RtH264Pic *pic) {
    if (!m->h264) m->h264 = rt_h264dec_open();
    int r = m->h264 ? rt_h264dec_decode(m->h264, m->vau.p, (int)m->vau.len, pic) : -1;
    m->have_vau = 0;
    if (r < 0) { m->errors++; g_total_errors++; }
    if (r != 1) return 0;
    m->video_frames++;
    g_total_video++;
    return 1;
}

/* Decodes the frame in m->aau into 2048 stereo samples; returns the number of samples */
static int decode_audio(Mpeg *m, int16_t *out) {
    m->have_aau = 0;
    const uint8_t *f = m->aau.p;
    int channels = (f[2] >> 2) & 7, align = (int)m->aau.len - 8;
    if (channels < 1 || channels > 2) channels = 2;
    if (!m->at3 || channels != m->at3_channels || align != m->at3_align) {
        rt_at3dec_close(m->at3);
        m->at3 = rt_at3dec_open(1, channels, align);
        m->at3_channels = channels;
        m->at3_align = align;
    }
    int n = 0;
    if (!m->at3 || rt_at3dec_decode(m->at3, f + 8, out, 2, &n) < 0) { m->errors++; g_total_errors++; return 0; }
    m->audio_frames++;
    g_total_audio++;
    return n;
}

/* --- context lookup ------------------------------------------------------------------- */
static Mpeg *ctx_of(uint32_t mpeg_addr) {
    if (!g_mpeg.used || !valid_range(mpeg_addr, 4) || rt_r32(mpeg_addr) != g_mpeg.handle) return NULL;
    return &g_mpeg;
}

static Stream *find_stream(Mpeg *m, uint32_t sid) {
    for (int i = 0; i < MAX_STREAMS; i++) if (m->streams[i].sid && (uint32_t)m->streams[i].sid == sid) return &m->streams[i];
    return NULL;
}

static void log_end(Mpeg *m) {
    if (m->logged_end++) return;
    rt_log("[mpeg] movie ended: %u pictures, %u audio frames decoded%s\n", m->video_frames, m->audio_frames,
           m->errors ? " (with decode errors)" : "");
}

/* --- HLE ---------------------------------------------------------------------------- */
static void hle_MpegInit(RtCtx *c) { delay_us(750); RET(0); }
static void hle_MpegFinish(RtCtx *c) { delay_us(250); RET(0); }
static void hle_MpegQueryMemSize(RtCtx *c) { RET(MPEG_MEMSIZE); }
static void hle_MpegRingbufferQueryMemSize(RtCtx *c) { RET(A0 * (104 + 2048)); }

/* (ringbuffer, numPackets, data, size, callback, callbackArg) */
static void hle_MpegRingbufferConstruct(RtCtx *c) {
    uint32_t rb = A0, packets = A1, data = A2, size = A3;
    if ((int32_t)size < 0) RET(ERR_NO_MEMORY);
    if (packets * (104 + 2048) > size && packets < 0x00100000) RET(ERR_NO_MEMORY);
    rt_w32(rb + RB_PACKETS, packets);
    rt_w32(rb + RB_READ, 0);
    rt_w32(rb + RB_WRITEPOS, 0);
    rt_w32(rb + RB_AVAIL, 0);
    rt_w32(rb + RB_PACKETSIZE, 2048);
    rt_w32(rb + RB_DATA, data);
    rt_w32(rb + RB_CALLBACK, T0);
    rt_w32(rb + RB_CBARG, c->r[9]);
    rt_w32(rb + RB_UPPER, data + packets * 2048);
    rt_w32(rb + RB_MPEG, 0);
    RET(0);
}

static void hle_MpegRingbufferDestruct(RtCtx *c) { RET(0); }

/* (mpeg, data, size, ringbuffer, frameWidth, mode, ddrTop): the handle lives at data+0x30 */
static void hle_MpegCreate(RtCtx *c) {
    uint32_t mpeg = A0, data = A1, size = A2, rb = A3;
    if (size < MPEG_MEMSIZE) RET(ERR_NO_MEMORY);
    if (rb) {
        uint32_t psize = rt_r32(rb + RB_PACKETSIZE);
        rt_w32(rb + RB_AVAIL, psize ? rt_r32(rb + RB_PACKETS) - (rt_r32(rb + RB_UPPER) - rt_r32(rb + RB_DATA)) / psize : 0);
        rt_w32(rb + RB_MPEG, mpeg);
    }
    uint32_t h = data + 0x30;
    rt_w32(mpeg, h);
    memcpy(rt_ptr(h), "LIBMPEG\0" "001\0", 12);
    rt_w32(h + 12, 0xFFFFFFFFu);
    if (rb) { rt_w32(h + 16, rb); rt_w32(h + 20, rt_r32(rb + RB_UPPER)); }
    if (g_mpeg.used) mpeg_free(&g_mpeg);
    Mpeg *m = &g_mpeg;
    m->used = 1;
    m->handle = h;
    m->rb = rb;
    m->frame_width = T0;
    m->pixel_mode = PIX_8888;
    m->video_id = 0xE0;
    m->audio_ch = -1;
    m->next_packet = ring_packet;
    m->vscan = 4;
    g_movies++;
    delay_us(29000);
    RET(0);
}

static void hle_MpegDelete(RtCtx *c) {
    Mpeg *m = ctx_of(A0);
    if (!m) RET(-1);
    if (m->video_frames || m->audio_frames) log_end(m);
    mpeg_free(m);
    delay_us(40000);
    RET(0);
}

/* (mpeg, streamType, streamNum) */
static void hle_MpegRegistStream(RtCtx *c) {
    Mpeg *m = ctx_of(A0);
    if (!m) RET(-1);
    int type = (int)A1, num = (int)A2;
    if (type == STREAM_AVC) m->video_id = 0xE0 | (num & 15);
    if (type == STREAM_ATRAC || type == STREAM_AUDIO) m->audio_ch = num & 15;
    for (int i = 0; i < MAX_STREAMS; i++)
        if (!m->streams[i].sid) { m->streams[i].sid = (int)g_stream_id; m->streams[i].type = type; m->streams[i].num = num; break; }
    RET(g_stream_id++);
}

static void hle_MpegUnRegistStream(RtCtx *c) {
    Mpeg *m = ctx_of(A0);
    if (!m) RET(-1);
    Stream *s = find_stream(m, A1);
    if (s) memset(s, 0, sizeof(*s));
    RET(0);
}

static void hle_MpegMallocAvcEsBuf(RtCtx *c) {
    Mpeg *m = ctx_of(A0);
    if (!m) RET(-1);
    for (int i = 0; i < MPEG_ES_BUFFERS; i++)
        if (!m->es_used[i]) { m->es_used[i] = 1; RET(i + 1); }
    RET(0);
}

static void hle_MpegFreeAvcEsBuf(RtCtx *c) {
    Mpeg *m = ctx_of(A0);
    if (!m) RET(-1);
    if (A1 == 0) RET(ERR_INVALID_VALUE);
    if (A1 >= 1 && A1 <= MPEG_ES_BUFFERS) m->es_used[A1 - 1] = 0;
    RET(0);
}

/* (mpeg, esBuffer, au) */
static void hle_MpegInitAu(RtCtx *c) {
    Mpeg *m = ctx_of(A0);
    if (!m) RET(-1);
    if (A1 >= 1 && A1 <= MPEG_ES_BUFFERS && m->es_used[A1 - 1]) write_au(A2, 0, 0, 0, MPEG_AVC_ES_SIZE);
    else write_au(A2, 0, NO_TS, 0, MPEG_ATRAC_ES_SIZE);
    RET(0);
}

static void hle_MpegQueryAtracEsSize(RtCtx *c) {
    rt_w32(A1, MPEG_ATRAC_ES_SIZE);
    rt_w32(A2, MPEG_ATRAC_ES_OUTPUT_SIZE);
    RET(0);
}

/* PSMF header: magic, version, stream offset (big-endian), stream size (big-endian), first and
 * last timestamps (48 bits at 0x54 and 0x5A), picture size in macroblocks at 142 and 143 */
static int psmf_version_ok(uint32_t v) { return v == 0x32313030u || v == 0x33313030u || v == 0x34313030u || v == 0x35313030u; }

static void analyze_header(Mpeg *m, const uint8_t *h) {
    m->stream_offset = be32(h + 8);
    m->stream_size = be32(h + 12);
    m->first_ts = ts48(h + 0x54);
    m->last_ts = ts48(h + 0x5A);
    m->width = h[142] * 16;
    m->height = h[143] * 16;
}

/* (mpeg, buffer, offsetOut): starts a new stream */
static void hle_MpegQueryStreamOffset(RtCtx *c) {
    Mpeg *m = ctx_of(A0);
    if (!m) RET(-1);
    const uint8_t *h = rt_ptr(A1);
    uint32_t off = be32(h + 8);
    if (rt_r32(A1) != PSMF_MAGIC || (off & 2047) || off == 0) { rt_w32(A2, 0); RET(ERR_INVALID_VALUE); }
    if (!psmf_version_ok(rt_r32(A1 + 4))) { rt_w32(A2, 0); RET(ERR_BAD_VERSION); }
    demux_reset(m);
    analyze_header(m, h);
    rt_w32(A2, off);
    rt_log("[mpeg] movie: %dx%d, %u bytes of stream, %.2f s (timestamps %lld..%lld)\n", m->width, m->height, m->stream_size,
           (double)(m->last_ts - m->first_ts) / 90000.0, (long long)m->first_ts, (long long)m->last_ts);
    RET(0);
}

static void hle_MpegQueryStreamSize(RtCtx *c) {
    uint32_t buf = A0, out = A1, off = be32(rt_ptr(buf + 8));
    if (rt_r32(buf) != PSMF_MAGIC || (off & 2047)) { rt_w32(out, 0); RET(ERR_INVALID_VALUE); }
    rt_w32(out, be32(rt_ptr(buf + 12)));
    RET(0);
}

/* Players poll this in a busy loop (Chains of Olympus between pictures, with sceDisplayGetVcount):
 * as in PPSSPP it takes 2020 CPU cycles (9 us at 222 MHz) and lets other threads run. */
static void hle_MpegRingbufferAvailableSize(RtCtx *c) {
    uint32_t free_packets = rt_r32(A0 + RB_PACKETS) - rt_r32(A0 + RB_AVAIL);
    k_eat(9);
    RET(free_packets);
}

/* (ringbuffer, numPackets, available): the game's callback reads the data into the ring */
static void hle_MpegRingbufferPut(RtCtx *c) {
    uint32_t rb = A0;
    int32_t packets = (int32_t)rt_r32(rb + RB_PACKETS), avail = (int32_t)rt_r32(rb + RB_AVAIL);
    int32_t n = (int32_t)A1 < (int32_t)A2 ? (int32_t)A1 : (int32_t)A2;
    if (n > packets - avail) n = packets - avail;
    if (n <= 0 || packets <= 0) RET(0);
    Mpeg *m = ctx_of(rt_r32(rb + RB_MPEG));
    uint32_t cb = rt_r32(rb + RB_CALLBACK), arg = rt_r32(rb + RB_CBARG), data = rt_r32(rb + RB_DATA);
    if (!cb) RET(0);
    int32_t total = 0, first_err = 0;
    int32_t write = (int32_t)rt_r32(rb + RB_WRITEPOS) % packets;
    while (n > 0) {
        int32_t round = n < packets - write ? n : packets - write;
        uint32_t args[3] = { data + (uint32_t)write * 2048, (uint32_t)round, arg };
        int32_t added = (int32_t)rt_call_guest(c, cb, 3, args);
        if (added < 0 && !total && !first_err) first_err = added;
        if (added > 0) {
            avail = (int32_t)rt_r32(rb + RB_AVAIL);
            if (added > packets - avail) added = packets - avail;
            rt_w32(rb + RB_READ, rt_r32(rb + RB_READ) + added);
            rt_w32(rb + RB_WRITEPOS, rt_r32(rb + RB_WRITEPOS) + added);
            rt_w32(rb + RB_AVAIL, avail + added);
            if (m && m->rb == rb) m->put += (uint32_t)added;
            total += added;
        }
        if (added < round) break;  /* end of the file (or an error) */
        n -= round;
        write = (write + round) % packets;
    }
    RET(first_err && !total ? (uint32_t)first_err : (uint32_t)total);
}

/* (mpeg, streamId, au, attr) */
static void hle_MpegGetAvcAu(RtCtx *c) {
    Mpeg *m = ctx_of(A0);
    if (!m) RET(-1);
    if (!find_stream(m, A1)) RET(-1);
    if (!m->have_vau && !next_video_au(m)) {
        /* the ring holds no complete AU yet, or the movie is over */
        write_au(A2, NO_TS, NO_TS, rt_r32(A2 + 16), rt_r32(A2 + 20));
        if (stream_done(m)) log_end(m);
        delay_us(DELAY_GET_AU);
        RET(ERR_NO_DATA);
    }
    write_au(A2, m->vau_pts, m->vau_dts, rt_r32(A2 + 16), (uint32_t)m->vau.len);
    if (A3) rt_w32(A3, 1);
    delay_us(DELAY_GET_AU);
    RET(0);
}

static void hle_MpegGetAtracAu(RtCtx *c) {
    Mpeg *m = ctx_of(A0);
    if (!m) RET(-1);
    if (!m->have_aau && !next_audio_au(m)) { delay_us(DELAY_GET_AU); RET(ERR_NO_DATA); }
    write_au(A2, m->aau_pts, NO_TS, rt_r32(A2 + 16), (uint32_t)m->aau.len);
    if (A3) rt_w32(A3, 0);
    delay_us(DELAY_GET_AU);
    RET(0);
}

/* (mpeg, au, frameWidth, buffer, init): decodes the AU sceMpegGetAvcAu found */
static void hle_MpegAvcDecode(RtCtx *c) {
    Mpeg *m = ctx_of(A0);
    if (!m) RET(-1);
    uint32_t fw = A2 ? A2 : m->frame_width ? m->frame_width : (uint32_t)m->width, init = T0;
    uint32_t buffer = rt_r32(A3);
    if (!m->have_vau && !next_video_au(m)) {
        if (init) rt_w32(init, 0);
        if (stream_done(m)) { delay_us(DELAY_AVC_EMPTY); RET(ERR_AVC_DECODE_FATAL); }
        delay_us(DELAY_AVC_DECODE);
        RET(0);
    }
    RtH264Pic pic;
    int got = decode_video(m, &pic);
    if (got) {
        int bpp = m->pixel_mode == PIX_8888 ? 4 : 2, w = pic.width < (int)fw ? pic.width : (int)fw;
        uint32_t pitch = fw * (uint32_t)bpp;
        if (fw <= 2048 && valid_range(buffer, pitch * (uint32_t)pic.height)) {
            for (int y = 0; y < pic.height; y++) yuv_row(&pic, y, w, m->pixel_mode, rt_ptr(buffer + (uint32_t)y * pitch));
            rt_ge_cpu_wrote(buffer, pitch, w, pic.height, bpp);
        }
    }
    if (init) rt_w32(init, got ? 1 : 0);
    delay_us(m->video_frames <= 1 ? DELAY_AVC_FIRST : DELAY_AVC_DECODE);
    RET(0);
}

/* (mpeg, frameWidth, buffer, status): no picture is held back */
static void hle_MpegAvcDecodeStop(RtCtx *c) { if (A3) rt_w32(A3, 0); RET(0); }
static void hle_MpegAvcDecodeFlush(RtCtx *c) { RET(0); }

static void hle_MpegFlushAllStream(RtCtx *c) {
    Mpeg *m = ctx_of(A0);
    if (!m) RET(-1);
    uint32_t rb = m->rb;
    demux_reset(m);
    if (rb) { rt_w32(rb + RB_AVAIL, 0); rt_w32(rb + RB_READ, 0); rt_w32(rb + RB_WRITEPOS, 0); }
    RET(0);
}

/* (mpeg, au, buffer, init): 2048 stereo samples of the frame sceMpegGetAtracAu found */
static void hle_MpegAtracDecode(RtCtx *c) {
    Mpeg *m = ctx_of(A0);
    if (!m) RET(-1);
    if (!valid_range(A2, MPEG_ATRAC_ES_OUTPUT_SIZE)) RET(-1);
    memset(rt_ptr(A2), 0, MPEG_ATRAC_ES_OUTPUT_SIZE);
    if (m->have_aau || next_audio_au(m)) {
        int16_t pcm[4096 * 2];
        int n = decode_audio(m, pcm);
        if (n > 2048) n = 2048;
        memcpy(rt_ptr(A2), pcm, (size_t)n * 4);
    }
    delay_us(DELAY_ATRAC_DECODE);
    RET(0);
}

void rt_mpeg_report(void) {
    if (g_movies)
        printf("[mpeg] movies played: %llu; %llu pictures and %llu audio frames decoded, %llu failed\n", (unsigned long long)g_movies,
               (unsigned long long)g_total_video, (unsigned long long)g_total_audio, (unsigned long long)g_total_errors);
}

/* --movie-test: demuxes and decodes a .pmf file through the code above, without the game.
 * Writes out.yuv (I420 pictures, for comparison with ffmpeg), out.wav (the ATRAC3+ channel,
 * 44.1 kHz stereo) and PNGs of the pictures converted as sceMpegAvcDecode does (every 300th). */
int rt_mpeg_test(const char *pmf, const char *out, int audio_ch) {
    size_t size = 0;
    uint8_t *file = rt_read_file(pmf, &size);
    if (!file || size < 2048 || be32(file) != 0x50534D46u) { fprintf(stderr, "rt: %s is not a PSMF file\n", pmf); return 1; }
    Mpeg *m = (Mpeg *)calloc(1, sizeof(Mpeg));
    analyze_header(m, file);
    if (m->stream_offset + (uint64_t)m->stream_size > size) m->stream_size = (uint32_t)(size - m->stream_offset);
    m->file = file;
    m->next_packet = file_packet;
    m->video_id = 0xE0;
    m->audio_ch = audio_ch;
    m->vscan = 4;
    char path[1024];
    snprintf(path, sizeof(path), "%s.yuv", out);
    FILE *fy = fopen(path, "wb");
    snprintf(path, sizeof(path), "%s.wav", out);
    FILE *fw = fopen(path, "wb");
    if (!fy || !fw) { fprintf(stderr, "rt: cannot write %s.*\n", out); return 1; }
    uint8_t wav[44] = { 0 };
    fwrite(wav, 1, 44, fw);
    uint8_t *rgba = (uint8_t *)calloc(480 * 272, 4);
    int16_t pcm[4096 * 2];
    uint32_t samples = 0, with_pts = 0, apts = 0;
    int64_t first_vpts = NO_TS, first_apts = NO_TS;
    for (;;) {
        int v = next_video_au(m), a = 0;
        if (v) {
            if (m->vau_pts != NO_TS) { with_pts++; if (first_vpts == NO_TS) first_vpts = m->vau_pts; }
            RtH264Pic pic;
            if (decode_video(m, &pic)) {
                for (int y = 0; y < pic.height; y++) fwrite(pic.y + (size_t)y * pic.stride_y, 1, (size_t)pic.width, fy);
                for (int y = 0; y < (pic.height + 1) / 2; y++) fwrite(pic.u + (size_t)y * pic.stride_uv, 1, (size_t)(pic.width + 1) / 2, fy);
                for (int y = 0; y < (pic.height + 1) / 2; y++) fwrite(pic.v + (size_t)y * pic.stride_uv, 1, (size_t)(pic.width + 1) / 2, fy);
                if (m->video_frames % 300 == 1 && pic.width <= 480 && pic.height <= 272) {
                    for (int y = 0; y < pic.height; y++) yuv_row(&pic, y, pic.width, PIX_8888, rgba + (size_t)y * 480 * 4);
                    snprintf(path, sizeof(path), "%s_%05u.png", out, m->video_frames);
                    rt_frames_write_png_rgba(path, rgba);
                }
            }
        }
        /* audio as far as the video went (the game keeps both going) */
        while (m->have_aau || next_audio_au(m)) {
            a = 1;
            if (m->aau_pts != NO_TS) { apts++; if (first_apts == NO_TS) first_apts = m->aau_pts; }
            int n = decode_audio(m, pcm);
            fwrite(pcm, 4, (size_t)n, fw);
            samples += (uint32_t)n;
            if (v) break;
        }
        if (!v && !a) break;
    }
    uint32_t bytes = samples * 4;
    memcpy(wav, "RIFF", 4); uint32_t riff = 36 + bytes; memcpy(wav + 4, &riff, 4);
    memcpy(wav + 8, "WAVEfmt ", 8); uint32_t v32 = 16; memcpy(wav + 16, &v32, 4);
    uint16_t v16 = 1; memcpy(wav + 20, &v16, 2); v16 = 2; memcpy(wav + 22, &v16, 2);
    v32 = 44100; memcpy(wav + 24, &v32, 4); v32 = 44100 * 4; memcpy(wav + 28, &v32, 4);
    v16 = 4; memcpy(wav + 32, &v16, 2); v16 = 16; memcpy(wav + 34, &v16, 2);
    memcpy(wav + 36, "data", 4); memcpy(wav + 40, &bytes, 4);
    fseek(fw, 0, SEEK_SET);
    fwrite(wav, 1, 44, fw);
    fclose(fw);
    fclose(fy);
    printf("[mpeg] %s: %dx%d, %u pictures (%u with a PTS, first %lld), %u audio frames on channel %d (%u with a PTS, first %lld), "
           "%.2f s of sound, %u decode errors\n", pmf, m->width, m->height, m->video_frames, with_pts, (long long)first_vpts,
           m->audio_frames, audio_ch, apts, (long long)first_apts, samples / 44100.0, m->errors);
    mpeg_free(m);
    free(m);
    free(rgba);
    free(file);
    return 0;
}

const HleEntry rt_hle_mpeg[] = {
    {"sceMpegInit", hle_MpegInit}, {"sceMpegFinish", hle_MpegFinish},
    {"sceMpegQueryMemSize", hle_MpegQueryMemSize}, {"sceMpegRingbufferQueryMemSize", hle_MpegRingbufferQueryMemSize},
    {"sceMpegRingbufferConstruct", hle_MpegRingbufferConstruct}, {"sceMpegRingbufferDestruct", hle_MpegRingbufferDestruct},
    {"sceMpegCreate", hle_MpegCreate}, {"sceMpegDelete", hle_MpegDelete},
    {"sceMpegRegistStream", hle_MpegRegistStream}, {"sceMpegUnRegistStream", hle_MpegUnRegistStream},
    {"sceMpegMallocAvcEsBuf", hle_MpegMallocAvcEsBuf}, {"sceMpegFreeAvcEsBuf", hle_MpegFreeAvcEsBuf},
    {"sceMpegInitAu", hle_MpegInitAu}, {"sceMpegQueryAtracEsSize", hle_MpegQueryAtracEsSize},
    {"sceMpegQueryStreamOffset", hle_MpegQueryStreamOffset}, {"sceMpegQueryStreamSize", hle_MpegQueryStreamSize},
    {"sceMpegRingbufferAvailableSize", hle_MpegRingbufferAvailableSize}, {"sceMpegRingbufferPut", hle_MpegRingbufferPut},
    {"sceMpegGetAvcAu", hle_MpegGetAvcAu}, {"sceMpegGetAtracAu", hle_MpegGetAtracAu},
    {"sceMpegAvcDecode", hle_MpegAvcDecode}, {"sceMpegAvcDecodeStop", hle_MpegAvcDecodeStop},
    {"sceMpegAvcDecodeFlush", hle_MpegAvcDecodeFlush}, {"sceMpegFlushAllStream", hle_MpegFlushAllStream},
    {"sceMpegAtracDecode", hle_MpegAtracDecode},
    {NULL, NULL},
};
