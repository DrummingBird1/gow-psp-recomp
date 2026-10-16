/* Debug features for automated testing (rt_debug.c): --warp, --god, --level-log, --dump-ram. */
#ifndef RT_DEBUG_H
#define RT_DEBUG_H
#include <stdint.h>

/* Parses a debug option at argv[*i] (consuming its value); returns 0 if it is not one. */
int rt_debug_option(int argc, char **argv, int *i);
/* Every controller read (rt_host_buttons): runs the warp and god mode, logs level changes. */
void rt_debug_input(uint64_t frame);
void rt_debug_report(void);

#endif
