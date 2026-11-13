/* Texture upscaling with xBRZ (runtime/rt_xbrz.cpp). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scales a w x h RGBA image by factor (2..6) into out, which holds w*factor x h*factor pixels. */
void rt_xbrz_scale(int factor, const uint32_t *rgba, int w, int h, uint32_t *out);

#ifdef __cplusplus
}
#endif
