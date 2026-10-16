/* PSP GE (graphics engine): display-list interpreter and software renderer. */
#pragma once
#include <stdint.h>

/* Executes a display list synchronously (lists are processed when enqueued).
 * stall = 0 means "run to the end of the list". */
void rt_ge_run(uint32_t list, uint32_t stall);
void rt_ge_report(void);

/* Writes the 480x272 image shown by sceDisplaySetFrameBuf to a PNG file. */
