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
    const char *gpu_compare_at;  /* displayed frames at which GPU and software output are compared */
    int from_gpu;           /* snapshots and video are read back from the GPU (--renderer gpu) */
} FrameOptions;

/* --perf-csv: host time spent in parts of the runtime since the last displayed frame (us) */
enum { RT_PERF_GE, RT_PERF_PRESENT, RT_PERF_IO, RT_PERF_PARTS };
extern uint64_t rt_perf_us[RT_PERF_PARTS];
uint64_t rt_frames_host_us(void);

extern uint32_t rt_max_frames;  /* --max-frames: the run stops after this displayed frame */
extern uint32_t rt_fast_forward;  /* --fast-forward: nothing is drawn before this displayed frame */

void rt_frames_init(const FrameOptions *o);
/* Called when the game shows a framebuffer (sceDisplaySetFrameBuf). now_us is virtual time. */
void rt_frames_present(uint64_t frame, uint64_t now_us, uint32_t addr, uint32_t stride, uint32_t format);
void rt_frames_close(uint64_t now_us);
/* After the frame was shown (and the GPU frame submitted): GPU comparisons. */
void rt_frames_after_present(uint64_t frame, uint32_t addr, uint32_t stride, uint32_t format);
/* Writes the 480x272 display area of a PSP framebuffer to a PNG; returns 0 on failure. */
int rt_frames_write_png(const char *path, uint32_t addr, uint32_t stride, uint32_t format);
/* Writes a 480x272 RGBA image (e.g. read back from the GPU) to a PNG. */
int rt_frames_write_png_rgba(const char *path, const uint8_t *rgba);
/* Compares the GPU's frame with the software renderer's (PSP memory); writes soft | gpu | diff. */
void rt_frames_compare_gpu(uint64_t frame, uint32_t addr, uint32_t stride, uint32_t format, const char *dir);

#endif
