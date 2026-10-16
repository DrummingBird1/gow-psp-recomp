/* PSP GE (graphics engine): display-list interpreter and software renderer. */
#pragma once
#include <stdint.h>

/* Executes a display list synchronously (lists are processed when enqueued).
 * stall = 0 means "run to the end of the list". */
void rt_ge_run(uint32_t list, uint32_t stall);
void rt_ge_report(void);
/* which renderers draw: software (into PSP memory), GPU (rt_gpu.c), or both */
void rt_ge_set_renderer(int soft, int gpu);
/* --fast-forward: primitives are skipped (lists, state, palettes and transfers still run),
 * which leaves the game's own state as it would be; set per frame in sceDisplaySetFrameBuf */
extern int rt_ge_skip_draw;
/* PSP memory may have changed (e.g. a new frame): GPU texture contents are re-checked */
void ge_memory_changed(void);
/* The CPU side (e.g. the video decoder) wrote w x h pixels of bpp bytes at addr, rows pitch bytes apart */
void rt_ge_cpu_wrote(uint32_t addr, uint32_t pitch, int w, int h, int bpp);

/* Writes the 480x272 image shown by sceDisplaySetFrameBuf to a PNG file. */
