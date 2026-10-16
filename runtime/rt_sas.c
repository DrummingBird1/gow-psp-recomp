/* sceSasCore: the PSP's software mixer on the Media Engine, which the game's sound engine (Sony
 * Scream) drives for its sound effects: up to 32 voices of VAG (Sony ADPCM) samples or noise,
 * each with an ADSR envelope, a pitch, dry left/right volumes and a send to the reverb.
 * __sceSasCore mixes one grain (256 frames in Chains of Olympus) into a stereo buffer the game
 * then plays through sceAudio.
 *
 * A C port of PPSSPP's SasAudio.cpp, SasReverb.cpp and sceSas.cpp (v1.20.4): the VAG decoder,
 * envelope curves and rates, key-on delay, linear-interpolation resampler and API checks match
 * PPSSPP, including the time a mix takes on the calling thread. Two deliberate differences:
 *  - noise voices (__sceSasSetNoise) are silent in PPSSPP; here they get the PSX SPU's noise
 *    generator, with the 6-bit frequency split into the SPU's shift and step;
 *  - the reverb runs at 22.05 kHz (PSX SPU algorithm and presets); PPSSPP writes its output to
 *    every other 44.1 kHz sample and zero-fills the rest. Here the gaps are interpolated (same
 *    level, without the mirror image near 22 kHz). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_kernel.h"

#define VOICES 32
#define VOL_MAX 0x1000
#define MAX_GRAIN 2048
#define PITCH_BASE 0x1000
#define PITCH_MASK 0xFFF
#define PITCH_SHIFT 12
#define PITCH_MAX 0x4000
#define HEIGHT_MAX 0x40000000LL

enum {
    SAS_ERR_INVALID_GRAIN = 0x80420001, SAS_ERR_INVALID_MAX_VOICES = 0x80420002, SAS_ERR_INVALID_OUTPUT_MODE = 0x80420003,
    SAS_ERR_INVALID_SAMPLE_RATE = 0x80420004, SAS_ERR_BAD_ADDRESS = 0x80420005, SAS_ERR_INVALID_VOICE = 0x80420010,
    SAS_ERR_INVALID_NOISE_FREQ = 0x80420011, SAS_ERR_INVALID_PITCH = 0x80420012, SAS_ERR_INVALID_ADSR_CURVE_MODE = 0x80420013,
    SAS_ERR_INVALID_PARAMETER = 0x80420014, SAS_ERR_INVALID_LOOP_POS = 0x80420015, SAS_ERR_VOICE_PAUSED = 0x80420016,
    SAS_ERR_INVALID_VOLUME = 0x80420018, SAS_ERR_INVALID_ADSR_RATE = 0x80420019, SAS_ERR_REV_INVALID_TYPE = 0x80420020,
    SAS_ERR_REV_INVALID_FEEDBACK = 0x80420021, SAS_ERR_REV_INVALID_DELAY = 0x80420022, SAS_ERR_REV_INVALID_VOLUME = 0x80420023,
    KERNEL_ERR_BAD_ARGUMENT = 0x80020066,
};

enum { CURVE_LINEAR_INC, CURVE_LINEAR_DEC, CURVE_LINEAR_BENT, CURVE_EXP_DEC, CURVE_EXP_INC, CURVE_DIRECT };
enum { ENV_KEYON_STEP = -42, ENV_KEYON = -2, ENV_OFF = -1, ENV_ATTACK = 0, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
enum { VT_OFF, VT_VAG, VT_NOISE };

static int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* --- VAG (Sony ADPCM): 28 samples per 16-byte block ---------------------------------------- */
typedef struct {
    int16_t samples[28];
    int cur;
    uint32_t data, read;
    int block, loop_block, nblocks;
    int s1, s2;
    int loop, loop_next, end;
} Vag;

/* predictor filters; entries 5-15 come from PPSSPP's hardware tests */
static const uint8_t k_vag_f[16][2] = {
    { 0, 0 }, { 60, 0 }, { 115, 52 }, { 98, 55 }, { 122, 60 }, { 0, 0 }, { 0, 0 }, { 52, 0 },
    { 55, 2 }, { 60, 125 }, { 0, 0 }, { 0, 91 }, { 0, 0 }, { 2, 216 }, { 125, 6 }, { 0, 151 },
};

static void vag_start(Vag *v, uint32_t data, uint32_t size, int loop) {
    v->loop = loop;
    v->loop_next = 0;
    v->loop_block = -1;
    v->nblocks = (int)(size / 16);
    v->end = 0;
    v->data = v->read = data;
    v->cur = 28;
    v->block = -1;
    v->s1 = v->s2 = 0;
}

static void vag_block(Vag *v) {
    if (v->block == v->nblocks - 1) { v->end = 1; return; }
    const uint8_t *p = rt_ptr(v->read);
    int predict = p[0] >> 4, shift = p[0] & 15, flags = p[1];
    if (flags == 7) { v->end = 1; return; }
    if (flags == 6) v->loop_block = v->block;
    else if (flags == 3 && v->loop) v->loop_next = 1;
    int c1 = k_vag_f[predict][0], c2 = -(int)k_vag_f[predict][1];
    int s1 = v->s1, s2 = v->s2;
    for (int i = 0; i < 28; i += 2) {
        uint8_t d = p[2 + i / 2];
        int a = (int16_t)((d & 0xF) << 12) >> shift, b = (int16_t)((d & 0xF0) << 8) >> shift;
        s2 = clamp16(a + ((s1 * c1 + s2 * c2) >> 6));
        s1 = clamp16(b + ((s2 * c1 + s1 * c2) >> 6));
        v->samples[i] = (int16_t)s2;
        v->samples[i + 1] = (int16_t)s1;
    }
    v->s1 = s1;
    v->s2 = s2;
    v->cur = 0;
    v->block++;
    v->read += 16;
}

static void vag_get(Vag *v, int16_t *out, int n) {
    if (v->end) { memset(out, 0, (size_t)n * 2); return; }
    for (int i = 0; i < n; i++) {
        if (v->cur == 28) {
            if (v->loop_next) {
                v->read = v->data + 16u * (uint32_t)v->loop_block + 16;
                v->block = v->loop_block;
                v->loop_next = 0;
            }
            vag_block(v);
            if (v->end) { memset(out + i, 0, (size_t)(n - i) * 2); return; }
        }
        out[i] = v->samples[v->cur++];
    }
}

/* --- ADSR envelope -------------------------------------------------------------------------- */
typedef struct {
    int attack_rate, decay_rate, sustain_rate, sustain_level, release_rate;
    int attack_type, decay_type, sustain_type, release_type;
    int state;
    int64_t height;
} Env;

static void env_set_state(Env *e, int s) {
    if (e->height > HEIGHT_MAX) e->height = HEIGHT_MAX;
    e->state = s;
}

static void env_walk(Env *e, int type, int rate) {
    int64_t d;
    switch (type) {
    case CURVE_LINEAR_INC: e->height += rate; break;
    case CURVE_LINEAR_DEC: e->height -= rate; break;
    case CURVE_LINEAR_BENT: e->height += e->height <= HEIGHT_MAX * 3 / 4 ? rate : rate / 4; break;
    case CURVE_EXP_DEC:
        d = e->height - HEIGHT_MAX;
        d += (-d * rate) >> 32;
        e->height = d + HEIGHT_MAX - (int64_t)(((uint64_t)(uint32_t)rate + 3) / 4);
        break;
    case CURVE_EXP_INC:
        d = e->height - HEIGHT_MAX;
        d += (-d * rate) >> 32;
        e->height = d + 0x4000 + HEIGHT_MAX;
        break;
    case CURVE_DIRECT: e->height = rate; break;
    }
}

static void env_step(Env *e) {
    switch (e->state) {
    case ENV_ATTACK:
        env_walk(e, e->attack_type, e->attack_rate);
        if (e->height >= HEIGHT_MAX || e->height < 0) env_set_state(e, ENV_DECAY);
        break;
    case ENV_DECAY:
        env_walk(e, e->decay_type, e->decay_rate);
        if (e->height < e->sustain_level) env_set_state(e, ENV_SUSTAIN);
        break;
    case ENV_SUSTAIN:
        env_walk(e, e->sustain_type, e->sustain_rate);
        if (e->height <= 0) { e->height = 0; env_set_state(e, ENV_RELEASE); }
        break;
    case ENV_RELEASE:
        env_walk(e, e->release_type, e->release_rate);
        if (e->height <= 0) { e->height = 0; env_set_state(e, ENV_OFF); }
        break;
    case ENV_KEYON:
        e->height = 0;
        env_set_state(e, ENV_KEYON_STEP);
        break;
    case ENV_KEYON_STEP:  /* the PSP keeps a new voice silent for 32 samples */
        if (++e->height >= 31) { e->height = 0; env_set_state(e, ENV_ATTACK); }
        break;
    default: break;
    }
}

static int env_height(const Env *e) { return (int)(e->height > HEIGHT_MAX ? HEIGHT_MAX : e->height); }

/* sceSasSetSimpleADSR bit fields (jpcsp / PPSSPP) */
static int simple_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    int r = ((7 - (n & 3)) << 26) >> (n >> 2);
    return r ? r : 1;
}
static int exponent_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    int r = ((7 - (n & 3)) << 24) >> (n >> 2);
    return r ? r : 1;
}

static void env_simple(Env *e, uint32_t a1, uint32_t a2) {
    e->attack_rate = simple_rate((int)(a1 >> 8));
    e->attack_type = (a1 & 0x8000) ? CURVE_LINEAR_BENT : CURVE_LINEAR_INC;
    int dn = (a1 >> 4) & 15;
    e->decay_rate = dn ? (int)(0x80000000u >> dn) : 0x7FFFFFFF;
    e->decay_type = CURVE_EXP_DEC;
    e->sustain_type = (a2 >> 14) & 3;
    e->sustain_rate = e->sustain_type == CURVE_EXP_DEC ? exponent_rate((int)(a2 >> 6)) : simple_rate((int)(a2 >> 6));
    e->release_type = (a2 & 0x20) ? CURVE_EXP_DEC : CURVE_LINEAR_DEC;
    int rn = a2 & 31;
    if (rn == 31) e->release_rate = 0;
    else if (e->release_type == CURVE_LINEAR_DEC) e->release_rate = rn == 30 ? 0x40000000 : rn == 29 ? 1 : 0x10000000 >> rn;
    else e->release_rate = rn ? (int)(0x80000000u >> rn) : 0x7FFFFFFF;
    e->sustain_level = (int)(((a1 & 15) + 1) << 26);
}

/* --- voices and the mixer ---------------------------------------------------------------------- */
typedef struct {
    int playing, paused, on, type;
    uint32_t vag_addr, vag_size;
    uint32_t frac;
    int pitch, loop, noise_freq;
    int vol_l, vol_r, fx_l, fx_r;
    int16_t hist[2];
    uint32_t noise_level;
    int noise_timer;
    Env env;
    Vag vag;
} Voice;

typedef struct {
    int init, grain, output_mode;
    int32_t mix[MAX_GRAIN * 2], send[MAX_GRAIN * 2];
    int16_t send22[MAX_GRAIN], wet[MAX_GRAIN * 2];
    int16_t temp[MAX_GRAIN * 4 + 2 + 16];
    Voice v[VOICES];
    int rev_type, rev_delay, rev_feedback, rev_vol_l, rev_vol_r, dry_on, wet_on;
    int16_t *rev_buf;
    int rev_pos;
    int32_t rev_prev[2];
    uint64_t mixes, keyons;
} Sas;

static Sas g_sas;

/* PSX SPU noise: a 16-bit LFSR clocked by a timer; the PSP's 6-bit frequency is taken as the
 * SPU's 4-bit shift and 2-bit step. */
static void noise_get(Voice *v, int16_t *out, int n) {
    int shift = (v->noise_freq >> 2) & 15, step = 4 + (v->noise_freq & 3);
    for (int i = 0; i < n; i++) {
        v->noise_timer -= step;
        uint32_t l = v->noise_level;
        uint32_t parity = ((l >> 15) ^ (l >> 12) ^ (l >> 11) ^ (l >> 10) ^ 1) & 1;
        if (v->noise_timer < 0) { v->noise_level = ((l << 1) | parity) & 0xFFFF; v->noise_timer += 0x20000 >> shift; }
        if (v->noise_timer < 0) v->noise_timer += 0x20000 >> shift;
        out[i] = (int16_t)v->noise_level;
    }
}

static void voice_read(Voice *v, int16_t *out, int n) {
    if (v->type == VT_VAG) vag_get(&v->vag, out, n);
    else if (v->type == VT_NOISE) noise_get(v, out, n);
    else memset(out, 0, (size_t)n * 2);
}

static void mix_voice(Sas *s, Voice *v) {
    if (v->type == VT_VAG && !v->vag_addr) return;
    const int grain = s->grain;
    int delay = 0;
    if (v->env.state == ENV_KEYON) {
        /* the first 32 samples after a key-on are silent (33 for VAG), scaled by the pitch */
        delay = (int)((32u * (uint32_t)v->pitch) >> PITCH_SHIFT);
        if (v->type == VT_VAG) delay++;
    }
    s->temp[0] = v->hist[0];
    s->temp[1] = v->hist[1];
    uint32_t frac = v->frac;
    int to_read = (int)((frac + (uint32_t)v->pitch * (uint32_t)(grain - delay > 0 ? grain - delay : 0)) >> PITCH_SHIFT);
    const int cap = (int)(sizeof(s->temp) / sizeof(s->temp[0])) - 2;
    if (to_read > cap) to_read = cap;
    int read_pos = 2;
    if (v->env.state == ENV_KEYON) { read_pos = 0; to_read += 2; }
    voice_read(v, s->temp + read_pos, to_read);
    const int temp_pos = read_pos + to_read;
    for (int i = 0; i < delay; i++) env_step(&v->env);

    const int interp = v->pitch != PITCH_BASE || (frac & PITCH_MASK) != 0;
    for (int i = delay; i < grain; i++) {
        const int16_t *p = s->temp + (frac >> PITCH_SHIFT);
        int sample = p[0];
        if (interp) {
            int f = (int)(frac & PITCH_MASK);
            sample = (p[0] * (PITCH_MASK - f) + p[1] * f) >> PITCH_SHIFT;
        }
        frac += (uint32_t)v->pitch;
        int env = env_height(&v->env);
        env_step(&v->env);
        env = (env + (1 << 14)) >> 15;
        sample = (sample * env + (1 << 14)) >> 15;
        s->mix[i * 2] += (sample * v->vol_l) >> 12;
        s->mix[i * 2 + 1] += (sample * v->vol_r) >> 12;
        s->send[i * 2] += sample * v->fx_l >> 12;
        s->send[i * 2 + 1] += sample * v->fx_r >> 12;
    }
    v->hist[0] = s->temp[temp_pos - 2];
    v->hist[1] = s->temp[temp_pos - 1];
    v->frac = frac - (uint32_t)(temp_pos - 2) * PITCH_BASE;
    if (v->type == VT_VAG && v->vag.end) { env_set_state(&v->env, ENV_OFF); v->env.height = 0; }
    if (v->env.state == ENV_OFF) { v->playing = 0; v->on = 0; }
}

/* --- reverb: the PSX SPU reverb (problemkaputt.de/psx-spx.htm#spureverbformula) at 22.05 kHz ---- */
#define REV_BUF 0x20000
typedef struct {
    int32_t size;
    int16_t dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1,
        mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1,
        mRAPF1, mLAPF2, mRAPF2;
} RevPreset;

#define S(x) ((int16_t)(x))
static const RevPreset k_rev[9] = {
    { 0x26C0, 0x007D, 0x005B, 0x6D80, 0x54B8, S(0xBED0), 0x0000, 0x0000, S(0xBA80), 0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227,
      0x0374, 0x01EF, 0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C },
    { 0x1F40, 0x0033, 0x0025, 0x70F0, 0x4FA8, S(0xBCE0), 0x4410, S(0xC0F0), S(0x9C00), 0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF,
      0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026 },
    { 0x4840, 0x00B1, 0x007F, 0x70F0, 0x4FA8, S(0xBCE0), 0x4510, S(0xBEF0), S(0xB4C0), 0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F,
      0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080 },
    { 0x6FE0, 0x00E3, 0x00A9, 0x6F60, 0x4FA8, S(0xBCE0), 0x4510, S(0xBEF0), S(0xA680), 0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C,
      0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA },
    { 0xADE0, 0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, S(0xB800), S(0xBC00), S(0xC000), 0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD,
      0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A },
    { 0xF6C0, 0x033D, 0x0231, 0x7E00, 0x5000, S(0xB400), S(0xB000), 0x4C00, S(0xB000), 0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B,
      0x1BC2, 0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232 },
    { 0x18040, 0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, S(0xC080), 0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005, 0x0005,
      0x0000, 0x0000, 0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002 },
    { 0x18040, 0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005, 0x0005,
      0x0000, 0x0000, 0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002 },
    { 0x3C00, 0x0017, 0x0013, 0x70F0, 0x4FA8, S(0xBCE0), 0x4510, S(0xBEF0), S(0x8500), 0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF,
      0x02B0, 0x01D7, 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059, 0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014 },
};
#undef S

static void rev_set_type(Sas *s, int type) {
    if (type == s->rev_type) return;
    s->rev_type = type;
    if (!s->rev_buf) s->rev_buf = (int16_t *)calloc(REV_BUF, sizeof(int16_t));
    if (type >= 0) { s->rev_pos = REV_BUF - k_rev[type].size; memset(s->rev_buf, 0, REV_BUF * sizeof(int16_t)); }
    else s->rev_pos = 0;
}

/* The work area is the top `size` entries of the buffer, addressed relative to the moving position. */
static int16_t *rb(Sas *s, int size, int i) {
    int a = s->rev_pos + i;
    if (a >= REV_BUF) a -= size;
    if (a < REV_BUF - size) a += size;
    return &s->rev_buf[a];
}

/* in: n stereo frames at 22.05 kHz; out: 2n stereo frames at 44.1 kHz. */
static void rev_process(Sas *s, int16_t *out, const int16_t *in, int n, int vol_l, int vol_r) {
    if (s->rev_type < 0) {  /* "off" passes the send through */
        for (int i = 0; i < n; i++) {
            out[i * 4 + 0] = out[i * 4 + 2] = clamp16(in[i * 2] * vol_l >> 15);
            out[i * 4 + 1] = out[i * 4 + 3] = clamp16(in[i * 2 + 1] * vol_r >> 15);
        }
        return;
    }
    const RevPreset *d = &k_rev[s->rev_type];
    const int z = d->size;
    for (int i = 0; i < n; i++) {
        int16_t Lin = (int16_t)(in[i * 2] >> 1), Rin = (int16_t)(in[i * 2 + 1] >> 1);  /* PPSSPP's level hack */
        *rb(s, z, d->mLSAME) = clamp16(Lin + (*rb(s, z, d->dLSAME) * d->vWALL >> 15) - (*rb(s, z, d->mLSAME - 1) * d->vIIR >> 15) + *rb(s, z, d->mLSAME - 1));
        *rb(s, z, d->mRSAME) = clamp16(Rin + (*rb(s, z, d->dRSAME) * d->vWALL >> 15) - (*rb(s, z, d->mRSAME - 1) * d->vIIR >> 15) + *rb(s, z, d->mRSAME - 1));
        *rb(s, z, d->mLDIFF) = clamp16(Lin + (*rb(s, z, d->dRDIFF) * d->vWALL >> 15) - (*rb(s, z, d->mLDIFF - 1) * d->vIIR >> 15) + *rb(s, z, d->mLDIFF - 1));
        *rb(s, z, d->mRDIFF) = clamp16(Rin + (*rb(s, z, d->dLDIFF) * d->vWALL >> 15) - (*rb(s, z, d->mRDIFF - 1) * d->vIIR >> 15) + *rb(s, z, d->mRDIFF - 1));
        int32_t L = (d->vCOMB1 * *rb(s, z, d->mLCOMB1) + d->vCOMB2 * *rb(s, z, d->mLCOMB2) + d->vCOMB3 * *rb(s, z, d->mLCOMB3) + d->vCOMB4 * *rb(s, z, d->mLCOMB4)) >> 15;
        int32_t R = (d->vCOMB1 * *rb(s, z, d->mRCOMB1) + d->vCOMB2 * *rb(s, z, d->mRCOMB2) + d->vCOMB3 * *rb(s, z, d->mRCOMB3) + d->vCOMB4 * *rb(s, z, d->mRCOMB4)) >> 15;
        *rb(s, z, d->mLAPF1) = clamp16(L - (d->vAPF1 * *rb(s, z, d->mLAPF1 - d->dAPF1) >> 15));
        L = *rb(s, z, d->mLAPF1 - d->dAPF1) + (*rb(s, z, d->mLAPF1) * d->vAPF1 >> 15);
        *rb(s, z, d->mRAPF1) = clamp16(R - (d->vAPF1 * *rb(s, z, d->mRAPF1 - d->dAPF1) >> 15));
        R = *rb(s, z, d->mRAPF1 - d->dAPF1) + (*rb(s, z, d->mRAPF1) * d->vAPF1 >> 15);
        *rb(s, z, d->mLAPF2) = clamp16(L - (d->vAPF2 * *rb(s, z, d->mLAPF2 - d->dAPF2) >> 15));
        L = *rb(s, z, d->mLAPF2 - d->dAPF2) + (*rb(s, z, d->mLAPF2) * d->vAPF2 >> 15);
        *rb(s, z, d->mRAPF2) = clamp16(R - (d->vAPF2 * *rb(s, z, d->mRAPF2 - d->dAPF2) >> 15));
        R = *rb(s, z, d->mRAPF2 - d->dAPF2) + (*rb(s, z, d->mRAPF2) * d->vAPF2 >> 15);
        /* 22.05 -> 44.1 kHz: PPSSPP's level is out/2 on average (every other sample zero);
         * interpolate at that level instead, half a sample late */
        int32_t ol = clamp16((L * vol_l) >> 15), or_ = clamp16((R * vol_r) >> 15);
        out[i * 4 + 0] = (int16_t)((s->rev_prev[0] + ol) >> 2);
        out[i * 4 + 1] = (int16_t)((s->rev_prev[1] + or_) >> 2);
        out[i * 4 + 2] = (int16_t)(ol >> 1);
        out[i * 4 + 3] = (int16_t)(or_ >> 1);
        s->rev_prev[0] = ol;
        s->rev_prev[1] = or_;
        if (++s->rev_pos >= REV_BUF) s->rev_pos -= z;
    }
}

static void sas_mix(Sas *s, uint32_t out_addr, uint32_t in_addr, int lvol, int rvol) {
    const int g = s->grain;
    for (int i = 0; i < VOICES; i++) if (s->v[i].playing && !s->v[i].paused) mix_voice(s, &s->v[i]);
    int16_t *out = (int16_t *)rt_ptr(out_addr);
    if (s->output_mode == 0) {
        const int dry = s->dry_on, wet = s->wet_on;
        if (wet) {
            for (int i = 0; i < g / 2; i++) {
                s->send22[i * 2] = clamp16(s->send[i * 4]);
                s->send22[i * 2 + 1] = clamp16(s->send[i * 4 + 1]);
            }
            rev_process(s, s->wet, s->send22, g / 2, s->rev_vol_l << 3, s->rev_vol_r << 3);
        }
        const int16_t *in = in_addr ? (const int16_t *)rt_ptr(in_addr) : NULL;
        for (int i = 0; i < g * 2; i += 2) {
            int l = 0, r = 0;
            if (in) { l = in[i] * lvol >> 12; r = in[i + 1] * rvol >> 12; }
            if (dry) { l += s->mix[i]; r += s->mix[i + 1]; }
            if (wet) { l += s->wet[i]; r += s->wet[i + 1]; }
            out[i] = clamp16(l);
            out[i + 1] = clamp16(r);
        }
    } else {  /* raw: dry L, dry R, send L, send R planes */
        for (int i = 0; i < g; i++) {
            out[i] = clamp16(s->mix[i * 2]);
            out[g + i] = clamp16(s->mix[i * 2 + 1]);
            out[g * 2 + i] = clamp16(s->send[i * 2]);
            out[g * 3 + i] = clamp16(s->send[i * 2 + 1]);
        }
    }
    memset(s->mix, 0, sizeof(int32_t) * (size_t)g * 2);
    memset(s->send, 0, sizeof(int32_t) * (size_t)g * 2);
    s->mixes++;
}

/* The mix runs on the Media Engine; PPSSPP charges the caller this long (sceSas.cpp). */
static int mix_us(const Sas *s) {
    int n = 0;
    for (int i = 0; i < VOICES; i++) if (s->v[i].playing && !s->v[i].paused) n++;
    int us = 20 + n * 68 + (s->grain * 60) / 100;
    return us < 1200 ? us : 1200;
}

/* --- __sceSas* ------------------------------------------------------------------------------- */
#define VOICE_ARG(v) if ((int32_t)(v) < 0 || (int32_t)(v) >= VOICES) RET(SAS_ERR_INVALID_VOICE)

static void hle_SasInit(RtCtx *c) {
    Sas *s = &g_sas;
    uint32_t grain = A1, voices = A2, mode = A3, rate = T0;
    if (!A0 || (A0 & 0x3F)) RET(SAS_ERR_BAD_ADDRESS);
    if (voices == 0 || voices > VOICES) RET(SAS_ERR_INVALID_MAX_VOICES);
    if (grain < 0x40 || grain > 0x800 || (grain & 0x1F)) RET(SAS_ERR_INVALID_GRAIN);
    if (mode != 0 && mode != 1) RET(SAS_ERR_INVALID_OUTPUT_MODE);
    if (rate != 44100) RET(SAS_ERR_INVALID_SAMPLE_RATE);
    int16_t *rev = s->rev_buf;
    memset(s, 0, sizeof(*s));
    s->rev_buf = rev;
    s->init = 1;
    s->grain = (int)grain;
    s->output_mode = (int)mode;
    s->rev_type = -2;
    rev_set_type(s, -1);
    s->dry_on = 1;
    for (int i = 0; i < VOICES; i++) {
        Voice *v = &s->v[i];
        v->pitch = PITCH_BASE;
        v->vol_l = v->vol_r = v->fx_l = v->fx_r = VOL_MAX;
        v->env.state = ENV_OFF;
        v->env.decay_type = v->env.sustain_type = v->env.release_type = CURVE_LINEAR_DEC;
        v->vag.end = 1;
        v->noise_level = 1;
    }
    rt_log("[sas] init: grain %u, %u voices, output mode %u\n", grain, voices, mode);
    RET(0);
}

static void sas_core(RtCtx *c, uint32_t out, uint32_t in, int lvol, int rvol) {
    Sas *s = &g_sas;
    if (!s->init) RET(0x80420100);  /* NOT_INIT */
    sas_mix(s, out, in, lvol, rvol);
    int32_t r = k_wait(WT_DELAY, 0, (uint64_t)mix_us(s));
    RET(r < 0 ? (uint32_t)r : 0);
}
static void hle_SasCore(RtCtx *c) {
    if (!A1) RET(SAS_ERR_INVALID_PARAMETER);
    sas_core(c, A1, 0, 0, 0);
}
static void hle_SasCoreWithMix(RtCtx *c) {
    if (!A1) RET(SAS_ERR_INVALID_PARAMETER);
    if (g_sas.output_mode == 1) RET(KERNEL_ERR_BAD_ARGUMENT);
    sas_core(c, A1, A1, (int)A2, (int)A3);
}

static void hle_SasGetEndFlag(RtCtx *c) {
    uint32_t f = 0;
    for (int i = 0; i < VOICES; i++) if (!g_sas.v[i].playing) f |= 1u << i;
    RET(f);
}

static void hle_SasSetVoice(RtCtx *c) {
    VOICE_ARG(A1);
    int32_t size = (int32_t)A3;
    int loop = (int)T0;
    if (size == 0 || (size & 0xF)) RET(SAS_ERR_INVALID_PARAMETER);
    if (loop != 0 && loop != 1) RET(SAS_ERR_INVALID_LOOP_POS);
    if (!A2) RET(0);
    if (size < 0) size = 0;  /* accepted, but plays nothing (PPSSPP) */
    Voice *v = &g_sas.v[A1];
    int reset = v->type != VT_VAG || v->vag_addr != A2 || v->vag_size != (uint32_t)size || v->loop != loop;
    v->type = VT_VAG;
    v->vag_addr = A2;
    v->vag_size = (uint32_t)size;
    v->loop = loop;
    if (v->on) v->playing = 1;
    if (reset) vag_start(&v->vag, A2, (uint32_t)size, loop);
    RET(0);
}

static void hle_SasSetNoise(RtCtx *c) {
    VOICE_ARG(A1);
    if ((int32_t)A2 < 0 || (int32_t)A2 >= 64) RET(SAS_ERR_INVALID_NOISE_FREQ);
    Voice *v = &g_sas.v[A1];
    v->type = VT_NOISE;
    v->noise_freq = (int)A2;
    RET(0);
}

static void hle_SasSetVolume(RtCtx *c) {
    VOICE_ARG(A1);
    int l = (int)A2, r = (int)A3, el = (int)T0, er = (int)T1;
    if (abs(l) > VOL_MAX || abs(r) > VOL_MAX || abs(el) > VOL_MAX || abs(er) > VOL_MAX) RET(SAS_ERR_INVALID_VOLUME);
    Voice *v = &g_sas.v[A1];
    v->vol_l = l; v->vol_r = r; v->fx_l = el; v->fx_r = er;
    RET(0);
}

static void hle_SasSetPitch(RtCtx *c) {
    VOICE_ARG(A1);
    if ((int32_t)A2 < 0 || (int32_t)A2 > PITCH_MAX) RET(SAS_ERR_INVALID_PITCH);
    g_sas.v[A1].pitch = (int)A2;
    RET(0);
}

static void hle_SasSetKeyOn(RtCtx *c) {
    VOICE_ARG(A1);
    Voice *v = &g_sas.v[A1];
    if (v->paused || v->on) RET(SAS_ERR_VOICE_PAUSED);
    env_set_state(&v->env, ENV_KEYON);
    if (v->type == VT_VAG) {
        if (!v->vag_addr) RET(0);
        vag_start(&v->vag, v->vag_addr, v->vag_size, v->loop);
    }
    v->playing = v->on = 1;
    v->paused = 0;
    v->frac = 0;
    g_sas.keyons++;
    RET(0);
}

/* A key-off can also start a sound that only plays its release phase. */
static void hle_SasSetKeyOff(RtCtx *c) {
    VOICE_ARG(A1);
    Voice *v = &g_sas.v[A1];
    if (v->paused || !v->on) RET(SAS_ERR_VOICE_PAUSED);
    v->on = 0;
    env_set_state(&v->env, ENV_RELEASE);
    RET(0);
}

static void hle_SasSetPause(RtCtx *c) {
    uint32_t bits = A1;
    for (int i = 0; bits && i < VOICES; i++, bits >>= 1) if (bits & 1) g_sas.v[i].paused = A2 != 0;
    RET(0);
}

static void hle_SasGetPauseFlag(RtCtx *c) {
    uint32_t f = 0;
    for (int i = 0; i < VOICES; i++) if (g_sas.v[i].paused) f |= 1u << i;
    RET(f);
}

static void hle_SasSetSL(RtCtx *c) {
    VOICE_ARG(A1);
    g_sas.v[A1].env.sustain_level = (int)A2;
    RET(0);
}

static void hle_SasSetADSR(RtCtx *c) {
    VOICE_ARG(A1);
    int flag = (int)A2, a = (int)A3, d = (int)T0, s = (int)T1, r = (int)T2;
    int invalid = (a < 0 ? 1 : 0) | (d < 0 ? 2 : 0) | (s < 0 ? 4 : 0) | (r < 0 ? 8 : 0);
    if (invalid & flag) RET(SAS_ERR_INVALID_ADSR_RATE);
    Env *e = &g_sas.v[A1].env;
    if (flag & 1) e->attack_rate = a;
    if (flag & 2) e->decay_rate = d;
    if (flag & 4) e->sustain_rate = s;
    if (flag & 8) e->release_rate = r;
    RET(0);
}

static void hle_SasSetADSRmode(RtCtx *c) {
    VOICE_ARG(A1);
    int flag = (int)A2;
    int a = (int)(A3 & 0x7FFFFFFF), d = (int)(T0 & 0x7FFFFFFF), s = (int)(T1 & 0x7FFFFFFF), r = (int)(T2 & 0x7FFFFFFF);
    int invalid = 0;
    if (a > 5 || (a & 1) != 0) invalid |= 1;
    if (d > 5 || (d & 1) != 1) invalid |= 2;
    if (s > 5) invalid |= 4;
    if (r > 5 || (r & 1) != 1) invalid |= 8;
    if (invalid & flag) RET(SAS_ERR_INVALID_ADSR_CURVE_MODE);
    Env *e = &g_sas.v[A1].env;
    if (flag & 1) e->attack_type = a;
    if (flag & 2) e->decay_type = d;
    if (flag & 4) e->sustain_type = s;
    if (flag & 8) e->release_type = r;
    RET(0);
}

static void hle_SasSetSimpleADSR(RtCtx *c) {
    VOICE_ARG(A1);
    if ((A3 >> 13) & 1) RET(SAS_ERR_INVALID_ADSR_CURVE_MODE);
    env_simple(&g_sas.v[A1].env, A2 & 0xFFFF, A3 & 0xFFFF);
    RET(0);
}

static void hle_SasGetEnvelopeHeight(RtCtx *c) {
    VOICE_ARG(A1);
    RET(env_height(&g_sas.v[A1].env));
}

static void hle_SasGetAllEnvelopeHeights(RtCtx *c) {
    if (!A1) RET(SAS_ERR_INVALID_PARAMETER);
    for (int i = 0; i < VOICES; i++) rt_w32(A1 + (uint32_t)i * 4, (uint32_t)env_height(&g_sas.v[i].env));
    RET(0);
}

static void hle_SasRevType(RtCtx *c) {
    if ((int32_t)A1 < -1 || (int32_t)A1 > 8) RET(SAS_ERR_REV_INVALID_TYPE);
    rev_set_type(&g_sas, (int)A1);
    RET(0);
}

static void hle_SasRevParam(RtCtx *c) {
    if ((int32_t)A1 < 0 || (int32_t)A1 >= 128) RET(SAS_ERR_REV_INVALID_DELAY);
    if ((int32_t)A2 < 0 || (int32_t)A2 >= 128) RET(SAS_ERR_REV_INVALID_FEEDBACK);
    g_sas.rev_delay = (int)A1;     /* stored; the presets have fixed delays (as in PPSSPP) */
    g_sas.rev_feedback = (int)A2;
    RET(0);
}

static void hle_SasRevEVOL(RtCtx *c) {
    if (A1 > 0x1000 || A2 > 0x1000) RET(SAS_ERR_REV_INVALID_VOLUME);
    g_sas.rev_vol_l = (int)A1;
    g_sas.rev_vol_r = (int)A2;
    RET(0);
}

static void hle_SasRevVON(RtCtx *c) {
    g_sas.dry_on = A1 != 0;
    g_sas.wet_on = A2 != 0;
    RET(0);
}

static void hle_SasGetGrain(RtCtx *c) { RET(g_sas.grain); }
static void hle_SasSetGrain(RtCtx *c) {
    if ((int32_t)A1 < 0x40 || A1 > MAX_GRAIN) RET(SAS_ERR_INVALID_GRAIN);
    g_sas.grain = (int)A1;
    RET(0);
}
static void hle_SasGetOutputmode(RtCtx *c) { RET(g_sas.output_mode); }
static void hle_SasSetOutputmode(RtCtx *c) {
    if (A1 != 0 && A1 != 1) RET(SAS_ERR_INVALID_OUTPUT_MODE);
    g_sas.output_mode = (int)A1;
    RET(0);
}

void rt_sas_report(void) {
    if (!g_sas.mixes) return;
    printf("[sas] %llu grains mixed (%.1f s), %llu key-ons, reverb %d (wet %s)\n", (unsigned long long)g_sas.mixes,
           (double)g_sas.mixes * g_sas.grain / 44100, (unsigned long long)g_sas.keyons, g_sas.rev_type, g_sas.wet_on ? "on" : "off");
}

const HleEntry rt_hle_sas[] = {
    {"__sceSasInit", hle_SasInit}, {"__sceSasCore", hle_SasCore}, {"__sceSasCoreWithMix", hle_SasCoreWithMix},
    {"__sceSasGetEndFlag", hle_SasGetEndFlag}, {"__sceSasSetVoice", hle_SasSetVoice}, {"__sceSasSetNoise", hle_SasSetNoise},
    {"__sceSasSetVolume", hle_SasSetVolume}, {"__sceSasSetPitch", hle_SasSetPitch}, {"__sceSasSetKeyOn", hle_SasSetKeyOn},
    {"__sceSasSetKeyOff", hle_SasSetKeyOff}, {"__sceSasSetPause", hle_SasSetPause}, {"__sceSasGetPauseFlag", hle_SasGetPauseFlag},
    {"__sceSasSetSL", hle_SasSetSL}, {"__sceSasSetADSR", hle_SasSetADSR}, {"__sceSasSetADSRmode", hle_SasSetADSRmode},
    {"__sceSasSetSimpleADSR", hle_SasSetSimpleADSR}, {"__sceSasGetEnvelopeHeight", hle_SasGetEnvelopeHeight},
    {"__sceSasGetAllEnvelopeHeights", hle_SasGetAllEnvelopeHeights}, {"__sceSasRevType", hle_SasRevType},
    {"__sceSasRevParam", hle_SasRevParam}, {"__sceSasRevEVOL", hle_SasRevEVOL}, {"__sceSasRevVON", hle_SasRevVON},
    {"__sceSasGetGrain", hle_SasGetGrain}, {"__sceSasSetGrain", hle_SasSetGrain},
    {"__sceSasGetOutputmode", hle_SasGetOutputmode}, {"__sceSasSetOutputmode", hle_SasSetOutputmode},
    {NULL, NULL},
};
