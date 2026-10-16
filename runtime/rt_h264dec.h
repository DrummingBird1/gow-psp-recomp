/* H.264 access-unit decoder: a C interface over OpenH264's decoder (Cisco, BSD-2-Clause). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RtH264Dec RtH264Dec;

/* A decoded picture in I420: planes stay valid until the next call on the decoder. */
typedef struct {
    const uint8_t *y, *u, *v;
    int stride_y, stride_uv;
    int width, height;
} RtH264Pic;

RtH264Dec *rt_h264dec_open(void);
void rt_h264dec_close(RtH264Dec *d);
/* Decodes one access unit (Annex B byte stream, start codes included). Returns 1 with the
 * picture in *pic, 0 when the unit gave no picture, -1 on a decode error. */
int rt_h264dec_decode(RtH264Dec *d, const uint8_t *au, int size, RtH264Pic *pic);

#ifdef __cplusplus
}
#endif
