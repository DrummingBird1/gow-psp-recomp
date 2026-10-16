/* sceAtrac3plus: ATRAC3 / ATRAC3+ playback from a buffer the game fills (music, MAINMENU.AT3 etc.).
 *
 * The buffer state machine is a C port of PPSSPP's Atrac2 (Core/HLE/AtracCtx2.cpp, v1.20.4), which
 * was written against hardware tests: the game owns the buffer and streams the file into it
 * (GetStreamDataInfo / AddStreamData); the context tracks the decode position, the file offset of
 * the next packet, the bytes buffered, loops and the frames that must be decoded and dropped after
 * a seek. API results and errors follow PPSSPP's sceAtrac.cpp, including the delays its HLE adds
 * (decoding takes 2.3 ms of the calling thread's time). Frames are decoded by at3_standalone
 * (rt_at3dec.cpp). Only what Chains of Olympus imports is exposed, plus the common setters.
 *
 * The contexts live on the host (the module's own context struct in PSP memory is not needed:
 * the game never reads it through _sceAtracGetContextAddress). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_at3dec.h"
#include "rt_kernel.h"

#define CODEC_AT3PLUS 0x1000
#define CODEC_AT3 0x1001
#define MAX_IDS 6

enum {
    ERR_API_FAIL = 0x80630002, ERR_NO_ATRACID = 0x80630003, ERR_BAD_ATRACID = 0x80630005,
    ERR_UNKNOWN_FORMAT = 0x80630006, ERR_BAD_CODEC_PARAMS = 0x80630008, ERR_ALL_DATA_LOADED = 0x80630009,
    ERR_NO_DATA = 0x80630010, ERR_SIZE_TOO_SMALL = 0x80630011, ERR_SECOND_BUFFER_NEEDED = 0x80630012,
    ERR_INCORRECT_READ_SIZE = 0x80630013, ERR_BAD_ALIGNMENT = 0x80630014, ERR_BAD_SAMPLE = 0x80630015,
    ERR_BAD_FIRST_RESET_SIZE = 0x80630016, ERR_BAD_SECOND_RESET_SIZE = 0x80630017, ERR_ADD_DATA_IS_TOO_BIG = 0x80630018,
    ERR_NO_LOOP_INFORMATION = 0x80630021, ERR_SECOND_BUFFER_NOT_NEEDED = 0x80630022, ERR_BUFFER_IS_EMPTY = 0x80630023,
    ERR_ALL_DATA_DECODED = 0x80630024, ERR_IS_LOW_LEVEL = 0x80630031, ERR_IS_FOR_SCESAS = 0x80630040,
    ERR_AA3_OTHER_FAILURE = 0x80631002, ERR_AA3_INVALID_DATA = 0x80631003, ERR_AA3_SIZE_TOO_SMALL = 0x80631004,
    ERR_AA3_BAD_CODEC_PARAMS = 0x80631005, ERR_KERNEL_ILLEGAL_ADDR = 0x800200D3,
};

enum {
    ST_NO_DATA = 1, ST_ALL_DATA_LOADED = 2, ST_HALFWAY_BUFFER = 3, ST_STREAMED_WITHOUT_LOOP = 4,
    ST_STREAMED_LOOP_FROM_END = 5, ST_STREAMED_LOOP_WITH_TRAILER = 6, ST_LOW_LEVEL = 8, ST_FOR_SCESAS = 16,
};
#define IS_STREAMING(s) (((s) & 4) != 0)

#define REMAIN_ALL_ON_MEMORY (-1)
#define REMAIN_NONLOOP_ON_MEMORY (-2)
#define REMAIN_LOOP_ON_MEMORY (-3)

/* SceAtracIdInfo (PPSSPP AtracBase.h), kept on the host. */
typedef struct {
    int decodePos, endSample, loopStart, loopEnd, firstValidSample;
    int numSkipFrames, state, curBuffer, numChan, sampleSize, codec;
    int dataOff, curFileOff, fileDataEnd, loopNum, streamDataByte, streamOff, secondStreamOff;
    uint32_t buffer, secondBuffer, bufferByte, secondBufferByte;
} AtracInfo;

typedef struct {
    int used;
    AtracInfo info;
    int outChannels;
    uint32_t codecErr;
    RtAt3Dec *dec;
    int16_t temp[2048 * 2];
    uint64_t frames;
} Atrac;

/* PPSSPP __AtracInit: two ATRAC3+ contexts, then two ATRAC3, then two unassigned. */
static const int g_types[MAX_IDS] = { CODEC_AT3PLUS, CODEC_AT3PLUS, CODEC_AT3, CODEC_AT3, 0, 0 };
static Atrac g_at[MAX_IDS];
static uint64_t g_decoded_frames, g_decode_errors;

typedef struct {
    int numChans, blockAlign, sampleSizeMaybe, tailFlag;
    int dataOff, endSample, waveDataSize, firstSampleOffset, loopStart, loopEnd;
} TrackInfo;

static int samples_per_frame(const AtracInfo *i) { return i->codec == CODEC_AT3PLUS ? 2048 : 1024; }
static int frame_mask(const AtracInfo *i) { return samples_per_frame(i) - 1; }
static int skip_samples(const AtracInfo *i) { return i->codec == CODEC_AT3PLUS ? 0x170 : 0x45; }
static int imax(int a, int b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }
static int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* --- file headers (PPSSPP Core/Util/AtracTrack.cpp) ----------------------------------------- */
static uint32_t rd16(const uint8_t *p, int *o) { uint32_t v = p[*o] | p[*o + 1] << 8; *o += 2; return v; }
static uint32_t rd32(const uint8_t *p, int *o) { uint32_t v; memcpy(&v, p + *o, 4); *o += 4; return v; }

static const uint8_t g_at3plus_guid_tail[16] = {
    0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44, 0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62,
};

/* Returns the codec type, or an error. */
static int parse_wave(const uint8_t *data, uint32_t len, TrackInfo *t) {
    t->loopStart = t->loopEnd = -1;
    t->firstSampleOffset = 0;
    t->endSample = 0;
    t->waveDataSize = 0;
    int ret = (int)ERR_UNKNOWN_FORMAT, o = 0;
    for (;;) {  /* find the RIFF WAVE header */
        if ((uint32_t)o + 12 >= len) return (int)ERR_SIZE_TOO_SMALL;
        if (rd32(data, &o) != 0x46464952u) return (int)ERR_UNKNOWN_FORMAT;
        uint32_t size = (rd32(data, &o) + 1) & ~1u;
        if (rd32(data, &o) == 0x45564157u) break;
        o += (int)size - 4;
    }
    int modified_offset = 0;
    for (;;) {
        if ((uint32_t)o + 8 >= len) return (int)ERR_SIZE_TOO_SMALL;
        uint32_t id = rd32(data, &o);
        int size = (int)((rd32(data, &o) + 1) & ~1u), next = o + size;
        if ((uint32_t)(o + size) > len && id != 0x61746164u) return (int)ERR_SIZE_TOO_SMALL;
        switch (id) {
        case 0x61746164u:  /* data: lasts to the end of the file */
            t->waveDataSize = size;
            t->dataOff = o;
            if (!t->firstSampleOffset) t->firstSampleOffset = ret == CODEC_AT3 ? 0x400 : 0x800;
            if (modified_offset && ret == CODEC_AT3PLUS) {
                t->firstSampleOffset -= 0xB8;
                if (t->loopEnd != -1) { t->loopEnd -= 0xB8; t->loopStart -= 0xB8; }
            }
            return ret;
        case 0x20746D66u: {  /* fmt */
            if (ret != (int)ERR_UNKNOWN_FORMAT || size < 0x20) return (int)ERR_UNKNOWN_FORMAT;
            uint32_t tag = rd16(data, &o);
            t->numChans = (int)rd16(data, &o);
            if (t->numChans != 1 && t->numChans != 2) return (int)ERR_UNKNOWN_FORMAT;
            if (rd32(data, &o) != 44100) return (int)ERR_UNKNOWN_FORMAT;
            o += 4;
            t->blockAlign = (int)rd16(data, &o);
            if (!t->blockAlign) return (int)ERR_UNKNOWN_FORMAT;
            if (tag == 0x270) {
                o += 4;
                if (rd16(data, &o) != 1) return (int)ERR_UNKNOWN_FORMAT;  /* joint stereo field */
                o += 4;
                uint32_t sst = rd16(data, &o);
                t->sampleSizeMaybe = sst & 0xFF;
                t->tailFlag = sst >> 8;
                if (rd16(data, &o) != sst || rd32(data, &o) != 1) return (int)ERR_UNKNOWN_FORMAT;
                ret = CODEC_AT3;
            } else if (tag == 0xFFFE) {
                if (size < 0x34 || memcmp(data + o + 10, g_at3plus_guid_tail, 16)) return (int)ERR_UNKNOWN_FORMAT;
                t->sampleSizeMaybe = data[o + 0x1C];
                t->tailFlag = data[o + 0x1D];
                if ((int)(((uint32_t)t->sampleSizeMaybe << 27) >> 29) != t->numChans) return (int)ERR_UNKNOWN_FORMAT;
                ret = CODEC_AT3PLUS;
            } else {
                return (int)ERR_UNKNOWN_FORMAT;
            }
            break;
        }
        case 0x6C706D73u:  /* smpl: the first loop */
            if (t->loopStart < 0) {
                if (size < 0x20) return (int)ERR_UNKNOWN_FORMAT;
                o += 0x1C;
                if (rd32(data, &o) != 0) {
                    if (size < 0x34) return (int)ERR_SIZE_TOO_SMALL;
                    o += 0xC;
                    t->loopStart = (int)rd32(data, &o);
                    t->loopEnd = (int)rd32(data, &o);
                    if (t->loopEnd <= t->loopStart) return (int)ERR_BAD_CODEC_PARAMS;
                }
            }
            break;
        case 0x74636166u:  /* fact: sample count and encoder delay */
            if (size < 4) return (int)ERR_UNKNOWN_FORMAT;
            t->endSample = (int)rd32(data, &o);
            if (size - 4 == 4) t->firstSampleOffset = (int)rd32(data, &o);
            else if (size - 4 >= 8) { o += 4; t->firstSampleOffset = (int)rd32(data, &o); modified_offset = 1; }
            break;
        default: break;
        }
        o = next;
    }
}

static uint32_t be(const uint8_t *p, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = v << 8 | p[i]; return v; }

static int parse_aa3(const uint8_t *d, uint32_t len, uint32_t file_size, TrackInfo *t) {
    if (len < 9) return (int)ERR_AA3_SIZE_TOO_SMALL;
    int o = 0, data_off = 0;
    uint32_t id = be(d, 3);
    o = 3;
    if (id == 0x656133 || id == 0x494433) {  /* "ea3" / "ID3" tag first */
        if (d[3] != 3 || d[4] != 0) return (int)ERR_AA3_OTHER_FAILURE;
        o = 6;
        uint32_t tag = ((d[6] & 0x7F) << 21) | ((d[7] & 0x7F) << 14) | ((d[8] & 0x7F) << 7) | (d[9] & 0x7F);
        o = 10 + (int)tag;
        if ((uint32_t)o >= len) return (int)ERR_AA3_SIZE_TOO_SMALL;
        if (d[o] == 0) o += 0x10;
        data_off = o;
    } else {
        o = 0;
    }
    if ((uint32_t)o + 0x22 > len) return (int)ERR_AA3_SIZE_TOO_SMALL;
    if (be(d + o, 3) != 0x454133) return (int)ERR_AA3_INVALID_DATA;
    o += 4;
    int extra = (int)be(d + o, 2);
    o += 2;
    if (be(d + o, 2) != 0xFFFF) return (int)ERR_AA3_INVALID_DATA;
    o += 2 + 24;
    int type = d[o];
    uint32_t params = be(d + o, 4);
    if (type > 1) return (int)ERR_AA3_INVALID_DATA;
    t->dataOff = data_off + extra;
    t->waveDataSize = (int)(file_size - (uint32_t)t->dataOff);
    t->loopStart = t->loopEnd = -1;
    t->numChans = 2;
    t->endSample = 0;
    if (type == 0) {
        if ((params & 0xE000) != 0x2000) return (int)ERR_AA3_BAD_CODEC_PARAMS;
        t->firstSampleOffset = 0x400;
        t->blockAlign = (int)((params & 0x3FF) << 3);
        t->sampleSizeMaybe = (params & 0x20000) ? 1 : 0;
        t->tailFlag = 0;
        return CODEC_AT3;
    }
    if ((params & 0x1C00) != 0x800 || (params & 0xE000) != 0x2000) return (int)ERR_AA3_BAD_CODEC_PARAMS;
    t->firstSampleOffset = 0x800;
    t->blockAlign = (int)((params & 0x3FF) * 8 + 8);
    t->sampleSizeMaybe = (int)(((params >> 8) & 3) | 0x28);
    t->tailFlag = (int)(params & 0xFF);
    return CODEC_AT3PLUS;
}

/* --- Atrac2 state machine -------------------------------------------------------------------- */
static int round_down(int size, int grain) { return size - size % grain; }
static int round_down_offset(int offset, int size, int grain) {
    return size > offset ? ((size - offset) / grain) * grain + offset : size;
}
static int compute_skip_frames(const AtracInfo *i, int pos) { return (pos & frame_mask(i)) < skip_samples(i) ? 2 : 1; }
static int compute_file_offset(const AtracInfo *i, int pos) {
    int off = ((pos / samples_per_frame(i)) - 1) * i->sampleSize;
    if ((pos & frame_mask(i)) < skip_samples(i) && off != 0) off -= i->sampleSize;
    return off + i->dataOff;
}
static int compute_loop_end_file_offset(const AtracInfo *i, int pos) {
    return (pos / samples_per_frame(i) + 1) * i->sampleSize + i->dataOff;
}

static int compute_space_used(const AtracInfo *i) {
    if (i->decodePos > i->loopEnd && i->curBuffer == 1) {
        int space = (int)i->secondBufferByte;
        if (i->secondStreamOff < space) space = round_down_offset(i->secondStreamOff, (int)i->secondBufferByte, i->sampleSize);
        if (i->secondStreamOff <= space && space - i->secondStreamOff < i->streamDataByte)
            return i->streamDataByte - (space - i->secondStreamOff);
        return 0;
    }
    return i->streamDataByte;
}

static int remain_stream(const AtracInfo *i) {
    if (i->streamDataByte >= i->fileDataEnd - i->curFileOff) return REMAIN_NONLOOP_ON_MEMORY;
    return imax(0, i->streamDataByte / i->sampleSize - i->numSkipFrames);
}

static int remain_looped(const AtracInfo *i) {
    const int ls = compute_file_offset(i, i->loopStart), le = compute_loop_end_file_offset(i, i->loopEnd);
    const int write = i->curFileOff + i->streamDataByte, left = write - le;
    int remain;
    if (write <= le) {
        remain = i->streamDataByte / i->sampleSize;
    } else {
        const int skip = compute_skip_frames(i, i->loopStart), first = le - ls, second = left % first;
        remain = (le - i->curFileOff) / i->sampleSize + (left / first) * (first / i->sampleSize - skip);
        if (second > skip * i->sampleSize) remain += second / i->sampleSize - skip;
    }
    remain = imax(0, remain - i->numSkipFrames);
    if (i->loopNum < 0) return remain;
    const int end = i->curFileOff + i->streamDataByte;
    if (end >= le && i->loopNum <= (end - le) / (le - ls)) return REMAIN_LOOP_ON_MEMORY;
    return remain;
}

static int remaining_frames(const AtracInfo *i) {
    switch (i->state) {
    case ST_NO_DATA: return 0;
    case ST_ALL_DATA_LOADED: return REMAIN_ALL_ON_MEMORY;
    case ST_HALFWAY_BUFFER: {
        const int write = i->dataOff + i->streamDataByte;
        return i->curFileOff < write ? imax(0, (write - i->curFileOff) / i->sampleSize - i->numSkipFrames) : 0;
    }
    case ST_STREAMED_WITHOUT_LOOP: return remain_stream(i);
    case ST_STREAMED_LOOP_FROM_END: return remain_looped(i);
    case ST_STREAMED_LOOP_WITH_TRAILER: return i->decodePos <= i->loopEnd ? remain_looped(i) : remain_stream(i);
    default: return (int)ERR_BAD_ATRACID;
    }
}

static int init_from_track(Atrac *a, const TrackInfo *w, uint32_t buf, int read_size, int buf_size) {
    AtracInfo *i = &a->info;
    const int off = i->codec == CODEC_AT3 ? 0x45 : 0x170, shift = (0x100B - i->codec) & 31;
    i->numChan = w->numChans;
    i->sampleSize = w->blockAlign;
    /* InitLengthAndLoop */
    const int first_valid = w->firstSampleOffset + off;
    i->firstValidSample = first_valid;
    i->decodePos = first_valid;
    i->loopNum = 0;
    i->endSample = (w->endSample == 0 ? (w->waveDataSize / i->sampleSize) << shift : w->endSample + first_valid) - 1;
    i->numSkipFrames = (first_valid >> shift) & 0xFF;
    if (w->loopStart > -1) { i->loopEnd = w->loopEnd + off; i->loopStart = w->loopStart + off; }
    else { i->loopEnd = 0; i->loopStart = 0; }

    i->streamDataByte = read_size - w->dataOff;
    i->buffer = buf;
    i->curFileOff = w->dataOff;
    i->dataOff = w->dataOff;
    i->fileDataEnd = w->waveDataSize + w->dataOff;
    i->curBuffer = 0;
    i->bufferByte = (uint32_t)buf_size;
    i->streamOff = w->dataOff;
    if (i->loopEnd > i->endSample) return (int)ERR_BAD_CODEC_PARAMS;
    if (!((uint32_t)(i->endSample >> shift) * (uint32_t)i->sampleSize < (uint32_t)w->waveDataSize)) return (int)ERR_BAD_CODEC_PARAMS;

    /* ComputeAtracStateAndInitSecondBuffer */
    if ((uint32_t)buf_size < (uint32_t)i->fileDataEnd) {
        if (i->streamDataByte < i->sampleSize * 2) return (int)ERR_SIZE_TOO_SMALL;
        if (i->loopEnd == 0) {
            i->state = ST_STREAMED_WITHOUT_LOOP;
        } else if (i->loopEnd == i->endSample) {
            i->state = ST_STREAMED_LOOP_FROM_END;
        } else {
            const int trailer = compute_loop_end_file_offset(i, i->loopEnd) - i->dataOff + 1;
            i->state = ST_STREAMED_LOOP_WITH_TRAILER;
            if (trailer < i->streamDataByte) i->streamDataByte = trailer;
            i->secondStreamOff = 0;
            i->secondBuffer = 0;
            i->secondBufferByte = 0;
        }
    } else {
        i->state = (uint32_t)read_size >= (uint32_t)i->fileDataEnd ? ST_ALL_DATA_LOADED : ST_HALFWAY_BUFFER;
    }
    return 0;
}

static int next_samples(const AtracInfo *i) {
    const int end_of_frame = i->decodePos | frame_mask(i);
    const int over = imax(0, end_of_frame - i->endSample);
    return imax(0, samples_per_frame(i) - ((i->decodePos & frame_mask(i)) + over));
}

/* One packet: decode it (into out, or dropped while frames are being skipped), then advance the
 * decode position, loop, and the streaming offsets. */
static uint32_t decode_internal(Atrac *a, uint32_t out_addr, int *num_samples, int *finish) {
    AtracInfo *i = &a->info;
    const int n = next_samples(i), spf = samples_per_frame(i);
    if (i->curFileOff + i->sampleSize > i->fileDataEnd || i->decodePos > i->endSample) { *finish = 1; return ERR_ALL_DATA_DECODED; }
    if (IS_STREAMING(i->state) && i->streamDataByte < i->sampleSize) { *finish = 0; return ERR_BUFFER_IS_EMPTY; }
    if (i->state == ST_HALFWAY_BUFFER && i->dataOff + i->streamDataByte < i->curFileOff + i->sampleSize) { *finish = 0; return ERR_BUFFER_IS_EMPTY; }

    uint32_t in;
    if (!IS_STREAMING(i->state)) in = i->buffer + (uint32_t)i->curFileOff;
    else if ((i->curBuffer & 1) == 0) in = i->buffer + (uint32_t)i->streamOff;
    else in = i->secondBuffer + (uint32_t)i->secondStreamOff;

    int got = 0;
    int16_t *dst = out_addr && i->numSkipFrames == 0 && n == spf ? (int16_t *)rt_ptr(out_addr) : a->temp;
    if (!a->dec || rt_at3dec_decode(a->dec, rt_ptr(in), dst, a->outChannels, &got) < 0) {
        *finish = 0;
        a->codecErr = 0x20B;
        g_decode_errors++;
        return ERR_API_FAIL;
    }
    a->codecErr = 0;
    a->frames++;
    g_decoded_frames++;
    i->curFileOff += i->sampleSize;

    if (i->numSkipFrames == 0) {
        if (num_samples) *num_samples = n;
        *finish = i->endSample < i->decodePos + n ? i->loopNum == 0 : 0;
        if (out_addr && n != spf && n) {
            /* a partial frame (the first one after a seek or loop, or the last): PPSSPP hands out
             * the frame's first n samples; we take them from the decode position on, so playback
             * stays continuous across the cut */
            const int from = i->decodePos & frame_mask(i);
            memcpy(rt_ptr(out_addr), a->temp + from * a->outChannels, (size_t)n * a->outChannels * 2);
        }
        i->decodePos += n;
        if (i->loopEnd != 0 && i->loopNum != 0 && i->decodePos > i->loopEnd) {
            i->curFileOff = compute_file_offset(i, i->loopStart);
            i->numSkipFrames = compute_skip_frames(i, i->loopStart);
            i->decodePos = i->loopStart;
            if (i->loopNum > 0) i->loopNum--;
        }
    } else {
        i->numSkipFrames--;
    }

    if (IS_STREAMING(i->state)) {
        i->streamDataByte -= i->sampleSize;
        if (i->curBuffer == 1) {
            const int next = i->secondStreamOff + i->sampleSize;
            if ((int)i->secondBufferByte < next + i->sampleSize) { i->streamOff = 0; i->secondStreamOff = 0; i->curBuffer = 2; }
            else i->secondStreamOff = next;
        } else {
            const int next = i->streamOff + i->sampleSize;
            i->streamOff = next + i->sampleSize > (int)i->bufferByte ? 0 : next;
            if (i->state == ST_STREAMED_LOOP_WITH_TRAILER && i->curBuffer == 0 &&
                (i->loopEnd == 0 || (i->loopNum == 0 && i->loopEnd < i->decodePos)) &&
                i->curFileOff >= compute_loop_end_file_offset(i, i->loopEnd)) {
                /* switch to the second buffer; its trailing partial packet moves to the main buffer */
                i->curBuffer = 1;
                i->streamDataByte = (int)i->secondBufferByte;
                i->secondStreamOff = 0;
                uint32_t part = i->secondBufferByte % (uint32_t)i->sampleSize;
                memmove(rt_ptr(i->buffer), rt_ptr(i->secondBuffer + (i->secondBufferByte - part)), part);
            }
        }
    }
    return 0;
}

static uint32_t skip_frames(Atrac *a, int *count) {
    int fin;
    *count = 0;
    while (a->info.numSkipFrames) {
        uint32_t r = decode_internal(a, 0, NULL, &fin);
        if (r) { if (r == ERR_API_FAIL) (*count)++; return r; }
        (*count)++;
    }
    return 0;
}

/* After SetData in a streaming state, a packet split by the buffer end is copied to its start. */
static void wrap_last_packet(Atrac *a) {
    AtracInfo *i = &a->info;
    if (!IS_STREAMING(i->state)) return;
    int to_end = round_down((int)i->bufferByte - i->streamOff, i->sampleSize);
    if (i->streamDataByte < to_end) {
        memset(rt_ptr(i->buffer), 0, 128);
    } else {
        int start = i->streamOff + to_end;
        memmove(rt_ptr(i->buffer), rt_ptr(i->buffer + (uint32_t)start), i->bufferByte - (uint32_t)start);
    }
}

static int set_data(Atrac *a, int aa3, uint32_t buf, uint32_t read_size, uint32_t buf_size, uint32_t file_size, int out_channels) {
    TrackInfo t;
    memset(&t, 0, sizeof(t));
    int codec = aa3 ? parse_aa3(rt_ptr(buf), read_size, file_size, &t) : parse_wave(rt_ptr(buf), read_size, &t);
    if (codec < 0) return codec;
    if (codec != a->info.codec) return 0x80630007;  /* WRONG_CODECTYPE */
    int r = init_from_track(a, &t, buf, (int)read_size, (int)buf_size);
    if (r < 0) return r;
    rt_at3dec_close(a->dec);
    a->dec = rt_at3dec_open(codec == CODEC_AT3PLUS, a->info.numChan, a->info.sampleSize);
    a->outChannels = out_channels;
    char loop[48] = "no loop";
    if (a->info.loopEnd)
        snprintf(loop, sizeof(loop), "loop %d-%d", a->info.loopStart - a->info.firstValidSample, a->info.loopEnd - a->info.firstValidSample);
    rt_log("[atrac] id %d: %s, %d ch, %d-byte frames, %d samples, %s, %s (buffer %08X, %u of %u bytes)\n",
           (int)(a - g_at), codec == CODEC_AT3PLUS ? "ATRAC3+" : "ATRAC3", a->info.numChan, a->info.sampleSize,
           a->info.endSample - a->info.firstValidSample + 1, loop,
           a->info.state == ST_ALL_DATA_LOADED ? "all data loaded" : a->info.state == ST_HALFWAY_BUFFER ? "halfway buffer" : "streamed",
           buf, read_size, buf_size);
    int skipped;
    r = (int)skip_frames(a, &skipped);
    wrap_last_packet(a);
    return r;
}

/* --- IDs ------------------------------------------------------------------------------------------ */
static int codec_of(uint32_t buf, uint32_t size, int aa3, uint32_t file_size) {
    TrackInfo t;
    if (!aa3 && size < 72) return (int)ERR_SIZE_TOO_SMALL;
    return aa3 ? parse_aa3(rt_ptr(buf), size, file_size, &t) : parse_wave(rt_ptr(buf), size, &t);
}

static int alloc_id(int codec) {
    for (int id = 0; id < MAX_IDS; id++)
        if (g_types[id] == codec && !g_at[id].used) {
            Atrac *a = &g_at[id];
            memset(&a->info, 0, sizeof(a->info));
            a->used = 1;
            a->info.codec = codec;
            a->info.state = ST_NO_DATA;
            a->codecErr = 0;
            a->frames = 0;
            return id;
        }
    return (int)ERR_NO_ATRACID;
}

static void free_id(int id) {
    rt_at3dec_close(g_at[id].dec);
    g_at[id].dec = NULL;
    g_at[id].used = 0;
}

static Atrac *get(uint32_t id) { return id < MAX_IDS && g_at[id].used ? &g_at[id] : NULL; }
static uint32_t validate_data(Atrac *a) { return !a ? ERR_BAD_ATRACID : a->info.state == ST_NO_DATA ? ERR_NO_DATA : 0; }
static uint32_t validate_managed(Atrac *a) {
    uint32_t e = validate_data(a);
    if (e) return e;
    if (a->info.state == ST_LOW_LEVEL) return ERR_IS_LOW_LEVEL;
    if (a->info.state == ST_FOR_SCESAS) return ERR_IS_FOR_SCESAS;
    return 0;
}

/* PPSSPP's hleDelayResult: the result arrives after the calling thread has waited a while. */
static void delay_ret(RtCtx *c, uint32_t v, uint64_t us) { k_wait(WT_DELAY, 0, us); c->r[2] = v; }

static void set_data_and_get_id(RtCtx *c, uint32_t buf, uint32_t read_size, uint32_t buf_size, int aa3, uint32_t file_size) {
    int codec = codec_of(buf, read_size, aa3, file_size);
    if (codec < 0) RET(codec);
    int id = alloc_id(codec);
    if (id < 0) RET(id);
    int r = set_data(&g_at[id], aa3, buf, read_size, buf_size, file_size, 2);
    if (r < 0) { free_id(id); RET(r); }
    delay_ret(c, (uint32_t)id, 100);
}

static void hle_AtracSetDataAndGetID(RtCtx *c) {
    uint32_t size = (int32_t)A1 < 0 ? 0x10000000u : A1;
    set_data_and_get_id(c, A0, size, size, 0, 0);
}
static void hle_AtracSetHalfwayBufferAndGetID(RtCtx *c) {
    if (A1 > A2) RET(ERR_INCORRECT_READ_SIZE);
    set_data_and_get_id(c, A0, A1, A2, 0, 0);
}
static void hle_AtracSetAA3DataAndGetID(RtCtx *c) { set_data_and_get_id(c, A0, A1, A1, 1, A2); }

static void hle_AtracSetData(RtCtx *c) {
    Atrac *a = get(A0);
    if (!a) RET(ERR_BAD_ATRACID);
    int r = set_data(a, 0, A1, A2, A2, 0, 2);
    if (r < 0) RET(r);
    delay_ret(c, (uint32_t)r, 100);
}

static void hle_AtracReleaseAtracID(RtCtx *c) {
    if (!get(A0)) RET(ERR_BAD_ATRACID);
    free_id((int)A0);
    RET(0);
}

static void hle_AtracDecodeData(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_data(a);
    if (e) RET(e);
    uint32_t out = A1, num_addr = A2, finish_addr = A3, remain_addr = T0;
    if (out & 1) RET(ERR_BAD_ALIGNMENT);
    int n = 0, finish = 0;
    uint32_t r = 0;
    for (int t = a->info.numSkipFrames + 1; t > 0 && !r; t--) r = decode_internal(a, out, &n, &finish);
    if (r) n = 0;
    if (num_addr) rt_w32(num_addr, (uint32_t)n);
    if (finish_addr) rt_w32(finish_addr, (uint32_t)finish);
    if (!r && remain_addr) rt_w32(remain_addr, (uint32_t)remaining_frames(&a->info));
    if (r == 0 || r == ERR_API_FAIL) delay_ret(c, r, 2300);
    else RET(r);
}

static void hle_AtracGetStreamDataInfo(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_managed(a);
    if (e) RET(e);
    const AtracInfo *i = &a->info;
    uint32_t wp = i->buffer, wb = 0, ro = 0;
    if (i->state == ST_HALFWAY_BUFFER) {
        const int fo = i->dataOff + i->streamDataByte;
        wp = i->buffer + (uint32_t)fo;
        wb = (uint32_t)(i->fileDataEnd - fo);
        ro = (uint32_t)fo;
    } else if (i->state != ST_ALL_DATA_LOADED) {
        /* ComputeStreamBufferDataInfo */
        const int so = i->curBuffer != 1 ? i->streamOff : 0;
        const int used = compute_space_used(i);
        const int after = round_down_offset(so, (int)i->bufferByte, i->sampleSize);
        const int pos = so + used;
        int space = pos >= after ? after - used : after - pos;
        if (space < 0) space = 0;
        const int ls = compute_file_offset(i, i->loopStart), le = compute_loop_end_file_offset(i, i->loopEnd);
        const int sfo = i->curFileOff + i->streamDataByte;
        int looped = i->state == ST_STREAMED_LOOP_FROM_END || (i->state == ST_STREAMED_LOOP_WITH_TRAILER && i->decodePos <= i->loopEnd);
        if (looped) {
            int writable = sfo >= le ? (le - ls) - (sfo - le) % (le - ls) : le - sfo;
            wb = (uint32_t)imin(writable, space);
            ro = (uint32_t)(sfo >= le ? ls + (sfo - le) % (le - ls) : sfo);
        } else {
            wb = (uint32_t)iclamp(i->fileDataEnd - sfo, 0, space);
            ro = sfo < i->fileDataEnd ? (uint32_t)sfo : 0;
        }
        int wrap_from = i->streamOff, wrap_add = i->streamDataByte;
        if (i->state == ST_STREAMED_LOOP_WITH_TRAILER && i->decodePos > i->loopEnd && i->curBuffer == 1) { wrap_from = 0; wrap_add = used; }
        if (i->state == ST_STREAMED_WITHOUT_LOOP && sfo >= i->fileDataEnd) {
            wp = i->buffer;
        } else {
            int size = round_down_offset(wrap_from, (int)i->bufferByte, i->sampleSize), sum = wrap_from + wrap_add;
            wp = i->buffer + (uint32_t)(size <= sum ? sum - size : sum);
        }
    }
    if (A1) rt_w32(A1, wp);
    if (A2) rt_w32(A2, wb);
    if (A3) rt_w32(A3, ro);
    RET(0);
}

static void hle_AtracAddStreamData(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_managed(a);
    if (e) RET(e);
    AtracInfo *i = &a->info;
    if (i->state == ST_ALL_DATA_LOADED) RET(ERR_ALL_DATA_LOADED);
    if (i->state == ST_HALFWAY_BUFFER) {
        const int nfo = i->streamDataByte + i->dataOff + (int)A1;
        if (nfo == i->fileDataEnd) i->state = ST_ALL_DATA_LOADED;
        else if (nfo > i->fileDataEnd) RET(ERR_ADD_DATA_IS_TOO_BIG);
    }
    i->streamDataByte += (int)A1;
    RET(0);
}

static void hle_AtracGetRemainFrame(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_managed(a);
    if (e) RET(e);
    if (!A1) RET(ERR_KERNEL_ILLEGAL_ADDR);
    rt_w32(A1, (uint32_t)remaining_frames(&a->info));
    RET(0);
}

static void hle_AtracGetNextSample(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_data(a);
    if (e) RET(e);
    if (A1) rt_w32(A1, (uint32_t)next_samples(&a->info));
    RET(0);
}

static void hle_AtracGetNextDecodePosition(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_data(a);
    if (e) RET(e);
    const AtracInfo *i = &a->info;
    if (i->decodePos > i->endSample || i->fileDataEnd - i->curFileOff < i->sampleSize) RET(ERR_ALL_DATA_DECODED);
    if (A1) rt_w32(A1, (uint32_t)(i->decodePos - i->firstValidSample));
    RET(0);
}

static void hle_AtracGetSoundSample(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_managed(a);
    if (e) RET(e);
    const AtracInfo *i = &a->info;
    if (A1) rt_w32(A1, (uint32_t)(i->endSample - i->firstValidSample));
    if (A2) rt_w32(A2, i->loopEnd ? (uint32_t)(i->loopStart - i->firstValidSample) : 0xFFFFFFFFu);
    if (A3) rt_w32(A3, i->loopEnd ? (uint32_t)(i->loopEnd - i->firstValidSample) : 0xFFFFFFFFu);
    RET(0);
}

static void hle_AtracSetLoopNum(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_data(a);
    if (e) RET(e);
    if (a->info.loopEnd <= 0) RET(ERR_NO_LOOP_INFORMATION);
    a->info.loopNum = (int)A1;
    RET(0);
}

static void hle_AtracGetLoopStatus(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_data(a);
    if (e) RET(e);
    const AtracInfo *i = &a->info;
    if (A1) rt_w32(A1, (uint32_t)i->loopNum);
    int st = i->loopEnd == 0 ? 0 : i->loopNum != 0 ? 1 : i->decodePos <= i->loopEnd;
    if (A2) rt_w32(A2, (uint32_t)st);
    RET(0);
}

static void hle_AtracGetInternalErrorInfo(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_data(a);
    if (e) RET(e);
    if (A1) rt_w32(A1, a->codecErr);
    RET(0);
}

static void hle_AtracGetSecondBufferInfo(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_managed(a);
    if (e) RET(e);
    if (!A1 || !A2) RET(ERR_KERNEL_ILLEGAL_ADDR);
    const AtracInfo *i = &a->info;
    if (i->state != ST_STREAMED_LOOP_WITH_TRAILER) { rt_w32(A1, 0); rt_w32(A2, 0); RET(ERR_SECOND_BUFFER_NOT_NEEDED); }
    const int le = compute_loop_end_file_offset(i, i->loopEnd);
    rt_w32(A1, (uint32_t)le);
    rt_w32(A2, (uint32_t)(i->fileDataEnd - le));
    RET(0);
}

static void hle_AtracSetSecondBuffer(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_managed(a);
    if (e) RET(e);
    AtracInfo *i = &a->info;
    const int le = compute_loop_end_file_offset(i, i->loopEnd);
    if (!(i->sampleSize * 3 <= (int)A2 || i->fileDataEnd - le <= (int)A2)) RET(ERR_SIZE_TOO_SMALL);
    if (i->state != ST_STREAMED_LOOP_WITH_TRAILER) RET(ERR_SECOND_BUFFER_NOT_NEEDED);
    i->secondBuffer = A1;
    i->secondBufferByte = A2;
    i->secondStreamOff = 0;
    RET(0);
}

/* Seeking: what the game must load (GetBufferInfoForResetting), then the seek itself. */
typedef struct { uint32_t ptr, writable, min, file_pos; } ResetInfo;

static void reset_info(const AtracInfo *i, int pos, ResetInfo *r) {
    r->ptr = i->buffer; r->writable = 0; r->min = 0; r->file_pos = 0;
    switch (i->state) {
    case ST_HALFWAY_BUFFER: {
        const int sp = i->dataOff + i->streamDataByte, fo = i->dataOff + (pos / samples_per_frame(i) + 1) * i->sampleSize;
        r->ptr = i->buffer + (uint32_t)sp;
        r->writable = (uint32_t)(i->fileDataEnd - sp);
        r->file_pos = (uint32_t)sp;
        r->min = (uint32_t)imax(0, fo - sp);
        break;
    }
    case ST_STREAMED_WITHOUT_LOOP:
    case ST_STREAMED_LOOP_FROM_END: {
        const int fo = compute_file_offset(i, pos), end = round_down((int)i->bufferByte, i->sampleSize);
        r->writable = (uint32_t)imin(i->fileDataEnd - fo, end);
        r->min = (uint32_t)((compute_skip_frames(i, pos) + 1) * i->sampleSize);
        r->file_pos = (uint32_t)fo;
        break;
    }
    case ST_STREAMED_LOOP_WITH_TRAILER: {
        const int sfo = compute_file_offset(i, pos), le = compute_loop_end_file_offset(i, i->loopEnd) - 1;
        const int end = round_down((int)i->bufferByte, i->sampleSize), skip = (compute_skip_frames(i, pos) + 1) * i->sampleSize;
        const int second_end = round_down((int)i->secondBufferByte, i->sampleSize);
        if (sfo < le) {
            const int before = le - sfo + 1;
            r->writable = (uint32_t)imin(end, before);
            r->min = (uint32_t)imin(skip, before);
            r->file_pos = (uint32_t)sfo;
        } else if (le + second_end <= sfo) {
            r->writable = (uint32_t)imin(i->fileDataEnd - sfo, end);
            r->min = (uint32_t)skip;
            r->file_pos = (uint32_t)sfo;
        } else if (le + (int)i->secondBufferByte + 1 < i->fileDataEnd) {
            const int eo = le + second_end + 1;
            r->writable = (uint32_t)imin(i->fileDataEnd - eo, end);
            r->min = (uint32_t)imax(0, sfo + skip - eo);
            r->file_pos = (uint32_t)eo;
        }
        break;
    }
    default: break;
    }
}

static void hle_AtracGetBufferInfoForResetting(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_managed(a);
    if (e) RET(e);
    if (!A2) RET(ERR_KERNEL_ILLEGAL_ADDR);
    AtracInfo *i = &a->info;
    if (i->state == ST_STREAMED_LOOP_WITH_TRAILER && !i->secondBufferByte) RET(ERR_SECOND_BUFFER_NEEDED);
    int pos = (int)A1 + i->firstValidSample;
    if ((uint32_t)pos > (uint32_t)i->endSample) RET(ERR_BAD_SAMPLE);
    ResetInfo r;
    reset_info(i, pos, &r);
    uint32_t w[8] = { r.ptr, r.writable, r.min, r.file_pos, i->buffer, 0, 0, 0 };
    memcpy(rt_ptr(A2), w, sizeof(w));
    int skipped;
    uint32_t ret = skip_frames(a, &skipped);  /* yes, this skips frames (PPSSPP, hardware tests) */
    if (skipped) delay_ret(c, ret, 300);
    else RET(ret);
}

static void hle_AtracResetPlayPosition(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_managed(a);
    if (e) RET(e);
    AtracInfo *i = &a->info;
    if (i->state == ST_STREAMED_LOOP_WITH_TRAILER && !i->secondBufferByte) RET(ERR_SECOND_BUFFER_NEEDED);
    int pos = (int)A1 + i->firstValidSample, first = (int)A2, second = (int)A3;
    if ((uint32_t)pos > (uint32_t)i->endSample) RET(ERR_BAD_SAMPLE);
    ResetInfo r;
    reset_info(i, pos, &r);
    if ((uint32_t)first < r.min || (uint32_t)first > r.writable) RET(ERR_BAD_FIRST_RESET_SIZE);
    if (second != 0) RET(ERR_BAD_SECOND_RESET_SIZE);  /* a reset never asks for second-buffer data */
    i->decodePos = pos;
    i->numSkipFrames = compute_skip_frames(i, pos);
    i->loopNum = 0;
    i->curFileOff = compute_file_offset(i, pos);
    a->codecErr = 0x20B;
    switch (i->state) {
    case ST_HALFWAY_BUFFER:
        i->streamDataByte += first;
        if (i->dataOff + i->streamDataByte >= i->fileDataEnd) i->state = ST_ALL_DATA_LOADED;
        break;
    case ST_STREAMED_WITHOUT_LOOP:
    case ST_STREAMED_LOOP_FROM_END:
        i->streamDataByte = first;
        i->curBuffer = 0;
        i->streamOff = 0;
        break;
    case ST_STREAMED_LOOP_WITH_TRAILER: {
        const int le = compute_loop_end_file_offset(i, i->loopEnd);
        if (i->curFileOff >= le) {
            const int sb = round_down((int)i->secondBufferByte, i->sampleSize);
            if (i->curFileOff < le + sb) {
                i->streamDataByte = le + sb - i->curFileOff + first;
                i->curBuffer = 1;
                i->secondStreamOff = i->curFileOff - le;
            } else {
                i->streamDataByte = first;
                i->curBuffer = 2;
                i->streamOff = 0;
            }
        } else {
            i->streamDataByte = first;
            i->curBuffer = 0;
            i->streamOff = 0;
        }
        break;
    }
    default: break;
    }
    rt_at3dec_flush(a->dec);
    int skipped;
    uint32_t ret = skip_frames(a, &skipped);
    if ((int32_t)ret < 0) { if (skipped) delay_ret(c, ret, 200); else RET(ret); return; }
    delay_ret(c, ret, 3000);
}

static void hle_AtracGetBitrate(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_data(a);
    if (e) RET(e);
    int br = a->info.sampleSize * 352800 / 1000;
    br = a->info.codec == CODEC_AT3PLUS ? ((br >> 11) + 8) & 0xFFFFFFF0 : (br + 511) >> 10;
    if (A1) rt_w32(A1, (uint32_t)br);
    RET(0);
}

static void hle_AtracGetChannel(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_data(a);
    if (e) RET(e);
    if (A1) rt_w32(A1, (uint32_t)a->info.numChan);
    RET(0);
}

static void hle_AtracGetMaxSample(RtCtx *c) {
    Atrac *a = get(A0);
    uint32_t e = validate_data(a);
    if (e) RET(e);
    if (A1) rt_w32(A1, (uint32_t)samples_per_frame(&a->info));
    RET(0);
}

static void hle_AtracGetAtracID(RtCtx *c) {
    if (A0 != CODEC_AT3 && A0 != CODEC_AT3PLUS) RET(0x80630004);  /* INVALID_CODECTYPE */
    RET(alloc_id((int)A0));
}

static void hle_Zero(RtCtx *c) { RET(0); }

void rt_atrac_report(void) {
    if (g_decoded_frames || g_decode_errors)
        printf("[atrac] %llu frames decoded, %llu failed\n", (unsigned long long)g_decoded_frames, (unsigned long long)g_decode_errors);
}

const HleEntry rt_hle_atrac[] = {
    {"sceAtracSetDataAndGetID", hle_AtracSetDataAndGetID}, {"sceAtracSetHalfwayBufferAndGetID", hle_AtracSetHalfwayBufferAndGetID},
    {"sceAtracSetAA3DataAndGetID", hle_AtracSetAA3DataAndGetID}, {"sceAtracSetData", hle_AtracSetData},
    {"sceAtracReleaseAtracID", hle_AtracReleaseAtracID}, {"sceAtracDecodeData", hle_AtracDecodeData},
    {"sceAtracGetStreamDataInfo", hle_AtracGetStreamDataInfo}, {"sceAtracAddStreamData", hle_AtracAddStreamData},
    {"sceAtracGetRemainFrame", hle_AtracGetRemainFrame}, {"sceAtracGetNextSample", hle_AtracGetNextSample},
    {"sceAtracGetNextDecodePosition", hle_AtracGetNextDecodePosition}, {"sceAtracGetSoundSample", hle_AtracGetSoundSample},
    {"sceAtracSetLoopNum", hle_AtracSetLoopNum}, {"sceAtracGetLoopStatus", hle_AtracGetLoopStatus},
    {"sceAtracGetInternalErrorInfo", hle_AtracGetInternalErrorInfo}, {"sceAtracGetSecondBufferInfo", hle_AtracGetSecondBufferInfo},
    {"sceAtracSetSecondBuffer", hle_AtracSetSecondBuffer}, {"sceAtracGetBufferInfoForResetting", hle_AtracGetBufferInfoForResetting},
    {"sceAtracResetPlayPosition", hle_AtracResetPlayPosition}, {"sceAtracGetBitrate", hle_AtracGetBitrate},
    {"sceAtracGetChannel", hle_AtracGetChannel}, {"sceAtracGetMaxSample", hle_AtracGetMaxSample},
    {"sceAtracGetAtracID", hle_AtracGetAtracID}, {"sceAtracReleaseResources", hle_Zero},
    {NULL, NULL},
};
