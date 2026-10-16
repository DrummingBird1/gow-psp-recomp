/* Frame output: PNG snapshots of displayed frames, video recording through ffmpeg and a
 * per-frame host timing log (for tracking performance across versions). */
#ifndef RT_FRAMES_H
#define RT_FRAMES_H
#include <stdint.h>

typedef struct {
    const char *png_dir;    /* PNG snapshots go here (frame_NNNNNN.png) */
    uint32_t png_every;     /* every Nth displayed frame (0 = off) */
    const char *png_at;     /* comma-separated displayed-frame numbers */
    const char *video;      /* MP4 path, game clock: frames are piped to ffmpeg */
    const char *video_host; /* MP4 path, host clock: shows how smoothly this machine runs it */
    const char *ffmpeg;     /* ffmpeg executable (default "ffmpeg") */
    const char *perf_csv;   /* per-frame timing log */
} FrameOptions;

extern uint32_t rt_max_frames;  /* --max-frames: the run stops after this displayed frame */

void rt_frames_init(const FrameOptions *o);
/* Called when the game shows a framebuffer (sceDisplaySetFrameBuf). now_us is virtual time. */
void rt_frames_present(uint64_t frame, uint64_t now_us, uint32_t addr, uint32_t stride, uint32_t format);
void rt_frames_close(uint64_t now_us);
/* Writes the 480x272 display area of a PSP framebuffer to a PNG; returns 0 on failure. */
int rt_frames_write_png(const char *path, uint32_t addr, uint32_t stride, uint32_t format);

#endif
