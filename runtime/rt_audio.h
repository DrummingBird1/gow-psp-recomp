/* Audio output: the PSP's sceAudio channels mixed on the virtual clock, played through SDL3
 * in the window and/or written to a WAV file. */
#ifndef RT_AUDIO_H
#define RT_AUDIO_H
#include <stdint.h>

typedef struct {
    const char *wav;   /* --wav: the whole mix, in game time from virtual time 0 */
    int realtime;      /* play through the default SDL audio device (window mode) */
} AudioOptions;

int rt_audio_init(const AudioOptions *o);  /* 0 on failure */
/* Everything the game queued before virtual time upto_us is final: mix it out to the sinks. */
void rt_audio_advance(uint64_t upto_us);
/* Window mode, called once the wall clock has caught up with virtual time: keeps the device
 * queue at its target latency (refills after an underrun, trims clock drift). */
void rt_audio_host_sync(void);
void rt_audio_shutdown(uint64_t now_us);
void rt_audio_report(void);
void rt_sas_report(void);    /* rt_sas.c */
void rt_atrac_report(void);  /* rt_atrac.c */

#endif
