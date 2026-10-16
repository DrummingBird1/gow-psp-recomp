/* H.264 access-unit decoder over OpenH264 v2.6.0 (codec/decoder, built from source without
 * its assembly). One thread and no frame delay: the PSMF movies are Main profile without
 * B-frames (pic_order_cnt_type 2), so every picture comes out of the unit that carries it.
 * H.264 decoding is exact, so the pictures are the same as any conforming decoder's. */
#include <cstdlib>
#include <cstring>

#include "codec_api.h"
#include "rt_h264dec.h"

struct RtH264Dec {
    ISVCDecoder *dec;
};

extern "C" RtH264Dec *rt_h264dec_open(void) {
    ISVCDecoder *dec = nullptr;
    if (WelsCreateDecoder(&dec) != 0 || !dec) return nullptr;
    int level = WELS_LOG_QUIET, threads = 0;
    dec->SetOption(DECODER_OPTION_TRACE_LEVEL, &level);
    dec->SetOption(DECODER_OPTION_NUM_OF_THREADS, &threads);
    SDecodingParam p;
    memset(&p, 0, sizeof(p));
    p.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    p.eEcActiveIdc = ERROR_CON_SLICE_COPY;  /* a damaged slice keeps the previous picture's pixels */
    if (dec->Initialize(&p) != 0) {
        WelsDestroyDecoder(dec);
        return nullptr;
    }
    RtH264Dec *d = (RtH264Dec *)calloc(1, sizeof(RtH264Dec));
    d->dec = dec;
    return d;
}

extern "C" void rt_h264dec_close(RtH264Dec *d) {
    if (!d) return;
    d->dec->Uninitialize();
    WelsDestroyDecoder(d->dec);
    free(d);
}

extern "C" int rt_h264dec_decode(RtH264Dec *d, const uint8_t *au, int size, RtH264Pic *pic) {
    unsigned char *planes[3] = { nullptr, nullptr, nullptr };
    SBufferInfo info;
    memset(&info, 0, sizeof(info));
    DECODING_STATE st = d->dec->DecodeFrameNoDelay(au, size, planes, &info);
    /* Outside Baseline, OpenH264 holds each picture back until the next one in case B-slices
     * need reordering; without B-slices output order is decode order, so take it now. */
    if (info.iBufferStatus != 1) d->dec->FlushFrame(planes, &info);
    if (info.iBufferStatus != 1) return st != dsErrorFree ? -1 : 0;
    pic->y = planes[0];
    pic->u = planes[1];
    pic->v = planes[2];
    pic->stride_y = info.UsrData.sSystemBuffer.iStride[0];
    pic->stride_uv = info.UsrData.sSystemBuffer.iStride[1];
    pic->width = info.UsrData.sSystemBuffer.iWidth;
    pic->height = info.UsrData.sSystemBuffer.iHeight;
    return 1;
}
