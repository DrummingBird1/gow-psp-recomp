/* ATRAC3 / ATRAC3+ frame decoder over at3_standalone (PPSSPP v1.20.4, ext/at3_standalone:
 * FFmpeg's decoders extracted to build alone, LGPL-2.1+). Mirrors PPSSPP's Atrac3Audio:
 * float output is scaled by 32767 and clamped, mono streams are duplicated for stereo output,
 * and ATRAC3 gets the 14-byte codec header PPSSPP builds (joint stereo from the frame size). */
#include <cstdlib>
#include <cstring>

#include "at3_decoders.h"
#include "rt_at3dec.h"

struct RtAt3Dec {
    int plus, channels, block_align;
    ATRAC3PContext *p;
    ATRAC3Context *a;
    float *buf[2];
};

static int16_t clamp16(float f) {
    if (f >= 1.0f) return 32767;
    if (f <= -1.0f) return -32767;
    return (int16_t)(f * 32767);
}

/* PPSSPP sceAudiocodec.cpp at3HeaderMap: only 66 kbps stereo uses joint stereo. */
static int at3_joint_stereo(int block_align, int channels) { return block_align == 0xC0 && channels == 2; }

extern "C" RtAt3Dec *rt_at3dec_open(int plus, int channels, int block_align) {
    RtAt3Dec *d = (RtAt3Dec *)calloc(1, sizeof(RtAt3Dec));
    d->plus = plus;
    d->channels = channels;
    d->block_align = block_align;
    if (plus) {
        d->p = atrac3p_alloc(channels, &d->block_align);
    } else {
        uint8_t extra[14] = { 0 };
        int js = at3_joint_stereo(block_align, channels);
        extra[0] = 1;
        extra[3] = (uint8_t)(channels << 3);
        extra[6] = (uint8_t)js;
        extra[8] = (uint8_t)js;
        extra[10] = 1;
        d->a = atrac3_alloc(channels, &d->block_align, extra, (int)sizeof(extra));
    }
    if (!d->p && !d->a) { free(d); return NULL; }
    for (int i = 0; i < 2; i++) d->buf[i] = (float *)calloc(4096, sizeof(float));
    return d;
}

extern "C" void rt_at3dec_close(RtAt3Dec *d) {
    if (!d) return;
    if (d->p) atrac3p_free(d->p);
    if (d->a) atrac3_free(d->a);
    for (int i = 0; i < 2; i++) free(d->buf[i]);
    free(d);
}

extern "C" void rt_at3dec_flush(RtAt3Dec *d) {
    if (d->p) atrac3p_flush_buffers(d->p);
    if (d->a) atrac3_flush_buffers(d->a);
}

extern "C" int rt_at3dec_decode(RtAt3Dec *d, const uint8_t *in, int16_t *out, int out_channels, int *samples) {
    int n = 0;
    int r = d->p ? atrac3p_decode_frame(d->p, d->buf, &n, in, d->block_align)
                 : atrac3_decode_frame(d->a, d->buf, &n, in, d->block_align);
    *samples = n;
    if (r < 0) return -1;
    if (out && n > 0) {
        const float *l = d->buf[0], *rt = d->channels == 2 ? d->buf[1] : d->buf[0];
        if (out_channels == 2) {
            for (int i = 0; i < n; i++) { out[i * 2] = clamp16(l[i]); out[i * 2 + 1] = clamp16(rt[i]); }
        } else {
            for (int i = 0; i < n; i++) out[i] = clamp16(l[i]);
        }
    }
    return 0;
}
