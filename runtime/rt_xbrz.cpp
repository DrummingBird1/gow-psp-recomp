/* Texture upscaling for the hardware renderer (--tex-upscale N): a C interface over xBRZ (Zenju, GPL-3.0).
 * The renderer's textures are RGBA bytes; xBRZ works on 0xAARRGGBB words, so red and blue are swapped on the way
 * in and out (its color distance weighs red and blue differently). */
#include <stdint.h>
#include <string.h>
#include <vector>

#include "rt_xbrz.h"
#include "xbrz.h"

static inline uint32_t swap_rb(uint32_t p) { return (p & 0xFF00FF00u) | (p >> 16 & 0xFFu) | (p & 0xFFu) << 16; }

extern "C" void rt_xbrz_scale(int factor, const uint32_t *rgba, int w, int h, uint32_t *out) {
    std::vector<uint32_t> src((size_t)w * h);
    for (size_t i = 0; i < src.size(); i++) src[i] = swap_rb(rgba[i]);
    xbrz::scale((size_t)factor, src.data(), out, w, h, xbrz::ColorFormat::ARGB);
    size_t n = (size_t)w * h * factor * factor;
    for (size_t i = 0; i < n; i++) out[i] = swap_rb(out[i]);
}
