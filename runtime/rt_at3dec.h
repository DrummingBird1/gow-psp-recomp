/* ATRAC3 / ATRAC3+ frame decoder: a C interface over PPSSPP's at3_standalone (FFmpeg's decoders). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RtAt3Dec RtAt3Dec;

/* plus: ATRAC3+ (codec 0x1000) or ATRAC3 (0x1001); channels: encoded channels (1 or 2). */
RtAt3Dec *rt_at3dec_open(int plus, int channels, int block_align);
void rt_at3dec_close(RtAt3Dec *d);
void rt_at3dec_flush(RtAt3Dec *d);
/* Decodes one frame of block_align bytes into interleaved s16 with out_channels (1 or 2) channels;
 * out may be NULL (the frame is decoded and dropped). Returns 0 on success, -1 on a decode error
 * (*samples is then still set to the frame length, so playback can step over a bad frame). */
int rt_at3dec_decode(RtAt3Dec *d, const uint8_t *in, int16_t *out, int out_channels, int *samples);

#ifdef __cplusplus
}
#endif
