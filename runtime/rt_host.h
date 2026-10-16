/* Host side of the port: the game window, real-time pacing and controller input. */
#ifndef RT_HOST_H
#define RT_HOST_H
#include <stdint.h>

typedef struct {
    int window;              /* --window: show the frames in a window and run in real time */
    int scale;               /* initial window size: 480x272 times this */
    int fullscreen;
    int nearest;             /* nearest-neighbour scaling instead of linear */
    const char *input;       /* --input: scripted presses "frame:buttons[:frames]" or "tSEC:buttons[:sec]", comma-separated */
    const char *shot_dir;    /* F12 screenshots go here */
    int gpu;                 /* draw with the SDL GPU renderer (rt_gpu.c) */
    const char *gpu_driver;  /* "vulkan", "direct3d12" or NULL (SDL's choice) */
} HostOptions;

/* PSP controller buttons (SceCtrlData.Buttons) */
enum {
    PSP_SELECT = 0x1, PSP_START = 0x8, PSP_UP = 0x10, PSP_RIGHT = 0x20, PSP_DOWN = 0x40, PSP_LEFT = 0x80,
    PSP_LTRIGGER = 0x100, PSP_RTRIGGER = 0x200, PSP_TRIANGLE = 0x1000, PSP_CIRCLE = 0x2000, PSP_CROSS = 0x4000,
    PSP_SQUARE = 0x8000,
};

int rt_host_init(const HostOptions *o);  /* 0 on failure */
void rt_host_shutdown(void);
/* A frame is shown (sceDisplaySetFrameBuf). Returns nonzero when the user closed the window. */
int rt_host_present(uint64_t frame, uint32_t addr, uint32_t stride, uint32_t format);
/* The scheduler is about to move virtual time from now_us to next_us (every thread waits).
 * In real-time mode this sleeps until the wall clock catches up. Returns nonzero to stop. */
int rt_host_idle(uint64_t now_us, uint64_t next_us);
/* Controller state at displayed frame `frame` / virtual time now_us: buttons, analog stick (128 = centre). */
uint32_t rt_host_buttons(uint64_t frame, uint64_t now_us, uint8_t *lx, uint8_t *ly);

#endif
