/* sceAudio and the audio output.
 *
 * The PSP mixes eight sceAudio channels and the Output2/SRC channel in hardware at 44.1 kHz.
 * Here every channel keeps the virtual-time position (in 44.1 kHz frames) where its queued data
 * ends. An output call mixes its block into a ring buffer starting at max(now, end of the
 * channel's queue) and, like the PSP (and PPSSPP's __AudioEnqueue), a blocking call that finds
 * data still queued waits until that data has played, i.e. until its own block starts. The game
 * therefore stays one block ahead of the "hardware", and all timing comes from the virtual clock:
 * nothing on the host side feeds back into the game, so headless runs stay deterministic.
 *
 * Frames before the current virtual time can no longer change; rt_audio_advance mixes them out
 * to the sinks: a WAV file (--wav, game time) and, in the window, an SDL3 audio stream. In the
 * window, virtual time is held to the wall clock (rt_host_idle), so the stream receives audio at
 * the rate it plays it. rt_audio_host_sync keeps the device queue near a fixed latency: after an
 * underrun (the host fell behind) it refills with silence, and a slow controller on the stream's
 * playback ratio (at most +-0.5%) absorbs the drift between the audio device and the wall clock. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_audio.h"
#include "rt_kernel.h"

#define RING (1u << 18)            /* mix ring, in stereo frames (~5.9 s) */
#define NCH 9                      /* 8 sceAudio channels + Output2 */
#define CH_OUT2 8
#define FMT_MONO 0x10
#define SCE_AUDIO_CHANNEL_NOT_INIT 0x80260001u
#define SCE_AUDIO_CHANNEL_BUSY 0x80260002u
#define SCE_AUDIO_INVALID_CHANNEL 0x80260003u
#define SCE_AUDIO_NO_CHANNELS 0x80260005u
#define SCE_AUDIO_SIZE_NOT_ALIGNED 0x80260006u
#define SCE_AUDIO_INVALID_FORMAT 0x80260007u
#define SCE_AUDIO_NOT_RESERVED 0x80260008u
#define SCE_AUDIO_INVALID_VOLUME 0x8026000Bu
#define SCE_AUDIO_ALREADY_RESERVED 0x80268002u
#define SCE_KERNEL_INVALID_SIZE 0x80000104u

typedef struct {
    int reserved;
    uint32_t samples, format;
    uint32_t lvol, rvol;
    uint64_t end;          /* frame position where this channel's queued data ends */
    uint64_t blocks;
} AudioCh;

static AudioCh g_ch[NCH];
static int32_t g_ring[RING * 2];
static uint64_t g_flushed;     /* frames before this are final and have gone to the sinks */
static uint64_t g_nonzero, g_clipped;   /* nonzero / clipped output samples */
static FILE *g_wav;
static uint64_t g_wav_frames;

static uint64_t us_to_pos(uint64_t us) { return us * 441 / 10000; }
static uint64_t pos_to_us(uint64_t pos) { return (pos * 10000 + 440) / 441; }

/* --- sinks -------------------------------------------------------------------------- */
static void wav_header(FILE *f, uint32_t frames) {
    uint32_t data = frames * 4;
    uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                      0x44, 0xAC, 0, 0, 0x10, 0xB1, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a' };
    uint32_t riff = 36 + data;
    memcpy(h + 4, &riff, 4);
    fwrite(h, 1, 40, f);
    fwrite(&data, 4, 1, f);
}

#ifdef GOW_HAVE_SDL
#include <SDL3/SDL.h>
#define TARGET_FRAMES 2205u        /* 50 ms of device queue */
#define LOW_FRAMES 441u            /* below 10 ms the device is about to run dry: refill */
static SDL_AudioStream *g_stream;
static double g_level = TARGET_FRAMES, g_ratio = 1.0;
static uint64_t g_ratio_t, g_underruns;
static int g_primed;
static double g_underrun_at[8];  /* game time of the first underruns, for the report */

static int sdl_open(void) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "[audio] SDL audio: %s\n", SDL_GetError()); return 0; }
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, 44100 };
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!g_stream) { fprintf(stderr, "[audio] cannot open the audio device: %s\n", SDL_GetError()); return 0; }
    SDL_ResumeAudioStreamDevice(g_stream);
    printf("[audio] playing through the default device (44100 Hz stereo, %u ms latency)\n", TARGET_FRAMES * 1000 / 44100);
    return 1;
}

static void sdl_put(const int16_t *s, uint32_t frames) { if (g_stream) SDL_PutAudioStreamData(g_stream, s, (int)(frames * 4)); }

static void sdl_silence(uint32_t frames) {
    static const int16_t zero[2048] = { 0 };
    while (frames) {
        uint32_t n = frames > 1024 ? 1024 : frames;
        SDL_PutAudioStreamData(g_stream, zero, (int)(n * 4));
        frames -= n;
    }
}

void rt_audio_host_sync(void) {
    if (!g_stream) return;
    int bytes = SDL_GetAudioStreamQueued(g_stream);
    uint32_t queued = bytes > 0 ? (uint32_t)bytes / 4 : 0;
    if (queued < LOW_FRAMES) {
        /* the host fell behind and the device ran (nearly) dry: rebuild the latency cushion,
         * otherwise every later block would arrive just in time and crackle */
        sdl_silence(TARGET_FRAMES - queued);
        if (g_primed && g_nonzero) {  /* running dry before the first sound is inaudible */
            if (g_underruns < 8) g_underrun_at[g_underruns] = (double)g_flushed / 44100;
            g_underruns++;
        }
        g_primed = 1;
        g_level = TARGET_FRAMES;
        return;
    }
    g_primed = 1;
    g_level += (queued - g_level) * 0.02;
    uint64_t t = SDL_GetTicks();
    if (t - g_ratio_t >= 200) {
        g_ratio_t = t;
        double r = 1.0 + 0.01 * (g_level - TARGET_FRAMES) / TARGET_FRAMES;
        r = r < 0.995 ? 0.995 : r > 1.005 ? 1.005 : r;
        if (r != g_ratio) { g_ratio = r; SDL_SetAudioStreamFrequencyRatio(g_stream, (float)r); }
    }
}

void rt_audio_pause(int on) {
    if (!g_stream) return;
    if (on) SDL_PauseAudioStreamDevice(g_stream);
    else SDL_ResumeAudioStreamDevice(g_stream);
}

static void sdl_close(void) {
    if (!g_stream) return;
    SDL_DestroyAudioStream(g_stream);
    g_stream = NULL;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
#else
static int sdl_open(void) { fprintf(stderr, "[audio] this build has no SDL3: no sound in real time\n"); return 1; }
static void sdl_put(const int16_t *s, uint32_t frames) { (void)s; (void)frames; }
void rt_audio_host_sync(void) {}
void rt_audio_pause(int on) { (void)on; }
static void sdl_close(void) {}
#endif

int rt_audio_init(const AudioOptions *o) {
    if (o->wav) {
        g_wav = fopen(o->wav, "wb");
        if (!g_wav) { fprintf(stderr, "[audio] cannot write %s\n", o->wav); return 0; }
        wav_header(g_wav, 0x3FFFFFF0u);  /* open-ended until shutdown, so a run cut off by the watchdog stays readable */
        printf("[audio] writing the mix to %s (game time)\n", o->wav);
    }
    if (o->realtime && !sdl_open()) return 1;  /* no device: run on without sound */
    return 1;
}

void rt_audio_advance(uint64_t upto_us) {
    uint64_t to = us_to_pos(upto_us);
    static int16_t out[4096 * 2];
    while (g_flushed < to) {
        uint32_t at = (uint32_t)(g_flushed & (RING - 1));
        uint32_t n = (uint32_t)(to - g_flushed);
        if (n > 4096) n = 4096;
        if (n > RING - at) n = RING - at;
        int32_t *m = g_ring + at * 2;
        for (uint32_t i = 0; i < n * 2; i++) {
            int32_t v = m[i];
            if (v > 32767) { v = 32767; g_clipped++; }
            else if (v < -32768) { v = -32768; g_clipped++; }
            if (v) g_nonzero++;
            out[i] = (int16_t)v;
            m[i] = 0;
        }
        if (g_wav) { fwrite(out, 4, n, g_wav); g_wav_frames += n; }
        sdl_put(out, n);
        g_flushed += n;
    }
}

void rt_audio_shutdown(uint64_t now_us) {
    rt_audio_advance(now_us);
    if (g_wav) {
        fseek(g_wav, 0, SEEK_SET);
        wav_header(g_wav, (uint32_t)g_wav_frames);
        fclose(g_wav);
        g_wav = NULL;
    }
    sdl_close();
}

void rt_audio_report(void) {
    printf("[audio] %.1f s mixed, %.1f s of it non-silent, %llu clipped samples", (double)g_flushed / 44100,
           (double)g_nonzero / 2 / 44100, (unsigned long long)g_clipped);
#ifdef GOW_HAVE_SDL
    if (g_primed) {
        printf(", %llu device underruns", (unsigned long long)g_underruns);
        for (uint64_t i = 0; i < g_underruns && i < 8; i++) printf("%s%.1f s", i ? ", " : " (game time ", g_underrun_at[i]);
        printf("%s, device queue %.0f ms, playback ratio %.4f", g_underruns ? (g_underruns > 8 ? ", ...)" : ")") : "",
               g_level * 1000 / 44100, g_ratio);
    }
#endif
    printf("\n  blocks per channel:");
    for (int i = 0; i < NCH; i++) if (g_ch[i].blocks) printf(" %s%d=%llu", i == CH_OUT2 ? "out2/" : "", i, (unsigned long long)g_ch[i].blocks);
    printf("\n");
}

/* --- channels ------------------------------------------------------------------------ */
/* Mixes one block into the ring and returns the virtual time (us) at which it starts playing. */
static uint64_t ch_queue(AudioCh *ch, uint32_t addr) {
    uint64_t now = us_to_pos(k_now);
    rt_audio_advance(k_now);
    uint64_t start = ch->end > now ? ch->end : now;
    uint32_t n = ch->samples;
    if (addr && n) {
        int mono = ch->format == FMT_MONO;
        int64_t lv = ch->lvol, rv = ch->rvol;  /* 0x8000 = unity; Output2 allows up to 0xFFFFF */
        const int16_t *s = (const int16_t *)rt_ptr(addr);
        if (start + n > g_flushed + RING) n = (uint32_t)(g_flushed + RING - start);
        for (uint32_t i = 0; i < n; i++) {
            int32_t *m = g_ring + ((start + i) & (RING - 1)) * 2;
            int32_t l = mono ? s[i] : s[i * 2], r = mono ? s[i] : s[i * 2 + 1];
            m[0] += (int32_t)((l * lv) >> 15);
            m[1] += (int32_t)((r * rv) >> 15);
        }
        ch->end = start + ch->samples;
        ch->blocks++;
    }
    return start > now ? pos_to_us(start) : k_now;
}

/* Blocking output: the block is queued at once; the call returns when the data queued before it
 * has played. Non-blocking output refuses while anything is still queued. */
static uint32_t ch_output(AudioCh *ch, int id, uint32_t addr, int blocking) {
    if (!blocking && ch->end > us_to_pos(k_now)) return SCE_AUDIO_CHANNEL_BUSY;
    uint64_t at = ch_queue(ch, addr);
    if (at > k_now) {
        int32_t r = k_wait(WT_AUDIO, id + 1, at - k_now);
        if (r < 0) return (uint32_t)r;
    }
    return ch->samples;
}

static int ch_valid(uint32_t n) { return n < 8; }

static void hle_AudioChReserve(RtCtx *c) {
    int ch = (int)A0;
    if (ch < 0) {
        for (ch = 7; ch > 0 && g_ch[ch].reserved; ch--) {}  /* PPSSPP GetFreeChannel: highest free first, never 0 */
        if (ch == 0) RET(SCE_AUDIO_NO_CHANNELS);
    }
    if (ch >= 8) RET(SCE_AUDIO_INVALID_CHANNEL);
    if ((A1 & 63) || !A1 || A1 > 65536 - 64) RET(SCE_AUDIO_SIZE_NOT_ALIGNED);
    if (A2 != 0 && A2 != FMT_MONO) RET(SCE_AUDIO_INVALID_FORMAT);
    if (g_ch[ch].reserved) RET(SCE_AUDIO_INVALID_CHANNEL);
    AudioCh *a = &g_ch[ch];
    a->reserved = 1;
    a->samples = A1;
    a->format = A2;
    a->lvol = a->rvol = 0;
    RET(ch);
}

static void hle_AudioChRelease(RtCtx *c) {
    if (!ch_valid(A0)) RET(SCE_AUDIO_INVALID_CHANNEL);
    if (!g_ch[A0].reserved) RET(SCE_AUDIO_CHANNEL_NOT_INIT);
    g_ch[A0].reserved = 0;
    RET(0);
}

static void hle_AudioOutputBlocking(RtCtx *c) {
    if ((int32_t)A1 > 0xFFFF) RET(SCE_AUDIO_INVALID_VOLUME);
    if (!ch_valid(A0)) RET(SCE_AUDIO_INVALID_CHANNEL);
    AudioCh *a = &g_ch[A0];
    if (!a->reserved) RET(SCE_AUDIO_CHANNEL_NOT_INIT);
    if ((int32_t)A1 >= 0) a->lvol = a->rvol = A1;
    RET(ch_output(a, (int)A0, A2, 1));
}

static void panned(RtCtx *c, int blocking) {
    if (A1 > 0xFFFF || A2 > 0xFFFF) RET(SCE_AUDIO_INVALID_VOLUME);
    if (!ch_valid(A0)) RET(SCE_AUDIO_INVALID_CHANNEL);
    AudioCh *a = &g_ch[A0];
    if (!a->reserved) RET(SCE_AUDIO_CHANNEL_NOT_INIT);
    a->lvol = A1;
    a->rvol = A2;
    RET(ch_output(a, (int)A0, A3, blocking));
}
static void hle_AudioOutputPannedBlocking(RtCtx *c) { panned(c, 1); }
static void hle_AudioOutputPanned(RtCtx *c) { panned(c, 0); }

static void hle_AudioSetChannelDataLen(RtCtx *c) {
    if (!ch_valid(A0)) RET(SCE_AUDIO_INVALID_CHANNEL);
    if (!g_ch[A0].reserved) RET(SCE_AUDIO_CHANNEL_NOT_INIT);
    if ((A1 & 63) || !A1 || A1 > 65536 - 64) RET(SCE_AUDIO_SIZE_NOT_ALIGNED);
    g_ch[A0].samples = A1;
    RET(0);
}

static void hle_AudioChangeChannelConfig(RtCtx *c) {
    if (!ch_valid(A0)) RET(SCE_AUDIO_INVALID_CHANNEL);
    if (!g_ch[A0].reserved) RET(SCE_AUDIO_NOT_RESERVED);
    g_ch[A0].format = A1;
    RET(0);
}

static void hle_AudioChangeChannelVolume(RtCtx *c) {
    if (A1 > 0xFFFF || A2 > 0xFFFF) RET(SCE_AUDIO_INVALID_VOLUME);
    if (!ch_valid(A0)) RET(SCE_AUDIO_INVALID_CHANNEL);
    if (!g_ch[A0].reserved) RET(SCE_AUDIO_NOT_RESERVED);
    g_ch[A0].lvol = A1;
    g_ch[A0].rvol = A2;
    RET(0);
}

static void hle_AudioGetChannelRestLength(RtCtx *c) {
    if (!ch_valid(A0)) RET(SCE_AUDIO_INVALID_CHANNEL);
    uint64_t now = us_to_pos(k_now);
    RET(g_ch[A0].end > now ? (uint32_t)(g_ch[A0].end - now) : 0);
}

/* Output2: one stereo channel at 44.1 kHz (sceAudioSRC without rate conversion). */
static void hle_AudioOutput2Reserve(RtCtx *c) {
    AudioCh *a = &g_ch[CH_OUT2];
    uint32_t n = A0 & 0x7FFFFFFFu;
    if (n < 17 || n > 4111) RET(SCE_KERNEL_INVALID_SIZE);
    if (a->reserved) RET(SCE_AUDIO_ALREADY_RESERVED);
    a->reserved = 1;
    a->samples = n;
    a->format = 0;
    RET(0);
}

static void hle_AudioOutput2OutputBlocking(RtCtx *c) {
    if (A0 > 0xFFFFF) RET(SCE_AUDIO_INVALID_VOLUME);
    AudioCh *a = &g_ch[CH_OUT2];
    if (!a->reserved) RET(SCE_AUDIO_NOT_RESERVED);
    a->lvol = a->rvol = A0;
    uint32_t r = ch_output(a, CH_OUT2, A1, 1);
    RET(A1 ? r : 0);
}

static void hle_AudioOutput2ChangeLength(RtCtx *c) {
    if (!g_ch[CH_OUT2].reserved) RET(SCE_AUDIO_NOT_RESERVED);
    g_ch[CH_OUT2].samples = A0;
    RET(0);
}

static void hle_AudioOutput2Release(RtCtx *c) {
    AudioCh *a = &g_ch[CH_OUT2];
    if (!a->reserved) RET(SCE_AUDIO_NOT_RESERVED);
    if (a->end > us_to_pos(k_now)) RET(SCE_AUDIO_ALREADY_RESERVED);  /* output busy */
    a->reserved = 0;
    RET(0);
}

const HleEntry rt_hle_audio[] = {
    {"sceAudioChReserve", hle_AudioChReserve}, {"sceAudioChRelease", hle_AudioChRelease},
    {"sceAudioOutputBlocking", hle_AudioOutputBlocking}, {"sceAudioOutputPannedBlocking", hle_AudioOutputPannedBlocking},
    {"sceAudioOutputPanned", hle_AudioOutputPanned}, {"sceAudioSetChannelDataLen", hle_AudioSetChannelDataLen},
    {"sceAudioChangeChannelConfig", hle_AudioChangeChannelConfig}, {"sceAudioChangeChannelVolume", hle_AudioChangeChannelVolume},
    {"sceAudioGetChannelRestLength", hle_AudioGetChannelRestLength}, {"sceAudioGetChannelRestLen", hle_AudioGetChannelRestLength},
    {"sceAudioOutput2Reserve", hle_AudioOutput2Reserve}, {"sceAudioOutput2OutputBlocking", hle_AudioOutput2OutputBlocking},
    {"sceAudioOutput2ChangeLength", hle_AudioOutput2ChangeLength}, {"sceAudioOutput2Release", hle_AudioOutput2Release},
    {NULL, NULL},
};
