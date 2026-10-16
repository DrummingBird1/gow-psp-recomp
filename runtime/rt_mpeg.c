/* sceMpeg without a video decoder: movies end as soon as their data has been read.
 *
 * This follows what PPSSPP does when it is built without FFmpeg (Core/HLE/sceMpeg.cpp,
 * v1.20.4), a state games are known to cope with: the ring buffer is filled through the
 * game's own read callback (so its file I/O runs normally), and every request for an
 * access unit reports SCE_MPEG_ERROR_NO_DATA and empties the buffer, so the player sees
 * the end of the stream once the file has been read. The delays match PPSSPP's so that
 * players polling for data let virtual time advance.
 *
 * Structures in PSP memory (PPSSPP sceMpeg.h):
 *   SceMpegRingBuffer: packets, packetsRead, packetsWritePos, packetsAvail, packetSize,
 *     data, callback_addr, callback_args, dataUpperBound, semaID, mpeg, gp
 *   SceMpegAu: pts (s64, high word first), dts (s64), esBuffer, esSize */
#include <stdio.h>
#include <string.h>

#include "rt_kernel.h"

#define MPEG_MEMSIZE 0x10000u
#define MPEG_AVC_ES_SIZE 2048u
#define MPEG_ATRAC_ES_SIZE 2112u
#define MPEG_ATRAC_ES_OUTPUT_SIZE 8192u
#define MPEG_ES_BUFFERS 2
#define PSMF_MAGIC 0x464D5350u  /* "PSMF" */
#define ERR_NO_DATA 0x80618001u
#define ERR_INVALID_VALUE 0x806101FEu
#define ERR_NO_MEMORY 0x80610022u
/* PPSSPP waits 100 ms after "no data"; with nothing to show, the player loop is let run fast
 * so a movie takes about as long as reading its file. */
#define NO_DATA_DELAY_US 2000

enum { RB_PACKETS = 0, RB_READ = 4, RB_WRITEPOS = 8, RB_AVAIL = 12, RB_PACKETSIZE = 16, RB_DATA = 20,
       RB_CALLBACK = 24, RB_CBARG = 28, RB_UPPER = 32, RB_MPEG = 40 };

static uint32_t g_stream_id = 1;
static int g_es_used[MPEG_ES_BUFFERS];
static uint32_t g_ringbuffer;  /* ring buffer of the most recent sceMpegCreate */
static int g_logged_movie;

static void delay_us(uint64_t us) { k_wait(WT_DELAY, 0, us); }

static void write_au(uint32_t addr, int64_t pts, int64_t dts, uint32_t es_buffer, uint32_t es_size) {
    rt_w32(addr, (uint32_t)((uint64_t)pts >> 32)); rt_w32(addr + 4, (uint32_t)pts);
    rt_w32(addr + 8, (uint32_t)((uint64_t)dts >> 32)); rt_w32(addr + 12, (uint32_t)dts);
    rt_w32(addr + 16, es_buffer); rt_w32(addr + 20, es_size);
}

static uint32_t be32(uint32_t addr) {
    uint32_t v = rt_r32(addr);
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

static void hle_MpegInit(RtCtx *c) { delay_us(750); RET(0); }
static void hle_MpegFinish(RtCtx *c) { RET(0); }
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
    g_ringbuffer = rb;
    g_es_used[0] = g_es_used[1] = 0;
    if (!g_logged_movie++) rt_log("[mpeg] movie playback started: no video decoder yet, movies end once their data is read\n");
    delay_us(29000);
    RET(0);
}

static void hle_MpegDelete(RtCtx *c) { delay_us(40000); RET(0); }
static void hle_MpegRegistStream(RtCtx *c) { RET(g_stream_id++); }
static void hle_MpegUnRegistStream(RtCtx *c) { RET(0); }

static void hle_MpegMallocAvcEsBuf(RtCtx *c) {
    for (int i = 0; i < MPEG_ES_BUFFERS; i++)
        if (!g_es_used[i]) { g_es_used[i] = 1; RET(i + 1); }
    RET(0);
}

static void hle_MpegFreeAvcEsBuf(RtCtx *c) {
    if (A1 == 0) RET(ERR_INVALID_VALUE);
    if (A1 >= 1 && A1 <= MPEG_ES_BUFFERS) g_es_used[A1 - 1] = 0;
    RET(0);
}

/* (mpeg, esBuffer, au) */
static void hle_MpegInitAu(RtCtx *c) {
    if (A1 >= 1 && A1 <= MPEG_ES_BUFFERS && g_es_used[A1 - 1]) write_au(A2, 0, 0, 0, MPEG_AVC_ES_SIZE);
    else write_au(A2, 0, -1, 0, MPEG_ATRAC_ES_SIZE);
    RET(0);
}

static void hle_MpegQueryAtracEsSize(RtCtx *c) {
    rt_w32(A1, MPEG_ATRAC_ES_SIZE);
    rt_w32(A2, MPEG_ATRAC_ES_OUTPUT_SIZE);
    RET(0);
}

/* PSMF header: magic, version, stream offset (big-endian), stream size (big-endian) */
static void hle_MpegQueryStreamOffset(RtCtx *c) {
    uint32_t buf = A1, out = A2, off = be32(buf + 8);
    if (rt_r32(buf) != PSMF_MAGIC || (off & 2047) || off == 0) { rt_w32(out, 0); RET(ERR_INVALID_VALUE); }
    rt_w32(out, off);
    RET(0);
}

static void hle_MpegQueryStreamSize(RtCtx *c) {
    uint32_t buf = A0, out = A1, off = be32(buf + 8);
    if (rt_r32(buf) != PSMF_MAGIC || (off & 2047)) { rt_w32(out, 0); RET(ERR_INVALID_VALUE); }
    rt_w32(out, be32(buf + 12));
    RET(0);
}

static void hle_MpegRingbufferAvailableSize(RtCtx *c) {
    RET(rt_r32(A0 + RB_PACKETS) - rt_r32(A0 + RB_AVAIL));
}

/* (ringbuffer, numPackets, available): the game's callback reads the data into the ring */
static void hle_MpegRingbufferPut(RtCtx *c) {
    uint32_t rb = A0;
    int32_t packets = (int32_t)rt_r32(rb + RB_PACKETS), avail = (int32_t)rt_r32(rb + RB_AVAIL);
    int32_t n = (int32_t)A1 < (int32_t)A2 ? (int32_t)A1 : (int32_t)A2;
    if (n > packets - avail) n = packets - avail;
    if (n <= 0 || packets <= 0) RET(0);
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
            total += added;
        }
        if (added < round) break;  /* end of the file (or an error) */
        n -= round;
        write = (write + round) % packets;
    }
    RET(first_err && !total ? (uint32_t)first_err : (uint32_t)total);
}

/* (mpeg, streamId, au, attr): with no decoder the stream is at its end */
static void hle_MpegGetAvcAu(RtCtx *c) {
    uint32_t rb = g_ringbuffer;
    write_au(A2, -1, -1, rt_r32(A2 + 16), rt_r32(A2 + 20));
    if (rb) rt_w32(rb + RB_AVAIL, 0);
    if (A3) rt_w32(A3, 1);
    delay_us(NO_DATA_DELAY_US);
    RET(ERR_NO_DATA);
}

static void hle_MpegGetAtracAu(RtCtx *c) {
    uint32_t rb = g_ringbuffer;
    if (rb) rt_w32(rb + RB_AVAIL, 0);
    if (A3) rt_w32(A3, 0);
    delay_us(NO_DATA_DELAY_US);
    RET(ERR_NO_DATA);
}

/* (mpeg, au, frameWidth, buffer, init): no frame */
static void hle_MpegAvcDecode(RtCtx *c) {
    if (T0) rt_w32(T0, 0);
    delay_us(5400);
    RET(0);
}

/* (mpeg, frameWidth, buffer, status) */
static void hle_MpegAvcDecodeStop(RtCtx *c) { if (A3) rt_w32(A3, 0); RET(0); }
static void hle_MpegAvcDecodeFlush(RtCtx *c) { RET(0); }

static void hle_MpegFlushAllStream(RtCtx *c) {
    uint32_t rb = g_ringbuffer;
    if (rb) { rt_w32(rb + RB_AVAIL, 0); rt_w32(rb + RB_READ, 0); rt_w32(rb + RB_WRITEPOS, 0); }
    RET(0);
}

/* (mpeg, au, buffer, init): silence */
static void hle_MpegAtracDecode(RtCtx *c) {
    memset(rt_ptr(A2), 0, MPEG_ATRAC_ES_OUTPUT_SIZE);
    delay_us(3000);
    RET(0);
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
