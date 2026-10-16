/* Sampling profiler for the boot runs (--profile). */
#ifndef RT_PROF_H
#define RT_PROF_H
#include <stdint.h>

void rt_prof_start(void);
void rt_prof_report(int top);

#endif
