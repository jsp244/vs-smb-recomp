/*
 * spc700.c - the SNES sound unit: SPC700, timers, ports, S-DSP. See spc700.h.
 *
 * The CPU runs instruction by instruction with each instruction's cycle count; the timers
 * (8 kHz, 8 kHz, 64 kHz) and the DSP (a stereo sample every 32 cycles) follow those cycles.
 * The DSP is modelled per sample, not per cycle: what the music needs (BRR decoding with
 * its filters, the Gaussian-like interpolation, envelopes at the hardware's rates, noise,
 * pitch modulation, echo) without the sub-sample timing of register accesses.
 */
#include "spc700.h"
#include <math.h>
#include <string.h>

enum { FC = 0x01, FZ = 0x02, FI = 0x04, FH = 0x08, FB = 0x10, FP = 0x20, FV = 0x40, FN = 0x80 };

/* ------------------------------------------------------------------ */
/* DSP                                                                 */

/* envelope / noise rates: the counter period of each of the 32 settings, and its offset */
static const uint16_t rate_period[32] = {
    0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
    64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1 };
static const uint16_t rate_offset[32] = {
    0, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
    0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 0, 0, 0 };

/* interpolation weights: 4 taps of a Gaussian kernel, 11 bits, for 256 positions between samples */
static int16_t gauss[256][4];
static int gauss_ready;

static void make_gauss(void) {
    const double sigma = 0.635;
    for (int i = 0; i < 256; i++) {
        double f = i / 256.0, w[4], sum = 0;
        const double d[4] = { f + 1, f, 1 - f, 2 - f };   /* the taps' distances from the point */
        for (int k = 0; k < 4; k++) { w[k] = exp(-d[k] * d[k] / (2 * sigma * sigma)); sum += w[k]; }
        for (int k = 0; k < 4; k++) gauss[i][k] = (int16_t)(w[k] / sum * 2040 + 0.5);   /* about the hardware table's 2040 */
    }
    gauss_ready = 1;
}

static inline int clamp16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

#define VREG(v, r) s->dsp[((v) << 4) | (r)]

static int rate_fires(spc_t *s, int rate) {
    if (!rate) return 0;
    return ((unsigned)(s->counter + rate_offset[rate]) % rate_period[rate]) == 0;
}

/* the next BRR block of voice n: 16 samples from its 9 bytes, END / LOOP handled at its end */
static void brr_decode(spc_t *s, spc_voice_t *v) {
    uint8_t hdr = s->ram[v->addr];
    int shift = hdr >> 4, filter = (hdr >> 2) & 3;
    for (int i = 0; i < 3; i++) v->buf[i] = v->buf[16 + i];
    for (int i = 0; i < 16; i++) {
        uint8_t byte = s->ram[(uint16_t)(v->addr + 1 + (i >> 1))];
        int n = (i & 1) ? (byte & 15) : (byte >> 4);
        n = (n ^ 8) - 8;                                  /* signed 4 bits */
        int smp = shift <= 12 ? (n << shift) >> 1 : (n < 0 ? -2048 : 0);
        int p1 = v->p1, p2 = v->p2;
        switch (filter) {
        case 1: smp += p1 + ((-p1) >> 4); break;                                   /* 15/16 */
        case 2: smp += p1 * 2 + ((-p1 * 3) >> 5) - p2 + (p2 >> 4); break;          /* 61/32, -15/16 */
        case 3: smp += p1 * 2 + ((-p1 * 13) >> 6) - p2 + ((p2 * 3) >> 4); break;   /* 115/64, -13/16 */
        }
        smp = clamp16(smp);
        smp = (int16_t)(smp << 1) >> 1;                   /* 15-bit wrap */
        v->p2 = v->p1; v->p1 = (int16_t)smp;
        v->buf[3 + i] = (int16_t)(smp << 1);
    }
    v->pos = 3;
    if (hdr & 1) {                                        /* END */
        s->dsp[0x7C] |= (uint8_t)(1 << (v - s->v));
        if (hdr & 2) v->addr = v->loop;                   /* LOOP */
        else { v->end_pending = 1; v->addr = (uint16_t)(v->addr + 9); }
    } else v->addr = (uint16_t)(v->addr + 9);
}

static void voice_kon(spc_t *s, int n) {
    spc_voice_t *v = &s->v[n];
    uint16_t dir = (uint16_t)(s->dsp[0x5D] << 8), e = (uint16_t)(dir + VREG(n, 4) * 4);
    v->addr = (uint16_t)(s->ram[e] | (s->ram[(uint16_t)(e + 1)] << 8));
    v->loop = (uint16_t)(s->ram[(uint16_t)(e + 2)] | (s->ram[(uint16_t)(e + 3)] << 8));
    v->p1 = v->p2 = 0;
    memset(v->buf, 0, sizeof v->buf);
    memset(v->hist, 0, sizeof v->hist);
    v->frac = 0;
    v->env = 0;
    v->mode = (VREG(n, 5) & 0x80) ? 0 : 4;
    v->kon_delay = 5;
    v->end_pending = 0;
    s->dsp[0x7C] &= (uint8_t)~(1 << n);
    brr_decode(s, v);
}

static void envelope(spc_t *s, int n) {
    spc_voice_t *v = &s->v[n];
    uint8_t adsr1 = VREG(n, 5), adsr2 = VREG(n, 6), gain = VREG(n, 7);
    if (v->mode == 3) {                                   /* release: 8 a sample */
        v->env -= 8;
        if (v->env < 0) v->env = 0;
        return;
    }
    if (adsr1 & 0x80) {
        if (v->mode == 4) v->mode = 0;
        if (v->mode == 0) {
            int rate = (adsr1 & 15) * 2 + 1;
            if (rate_fires(s, rate)) {
                v->env += rate == 31 ? 0x400 : 0x20;
                if (v->env >= 0x7FF) { v->env = 0x7FF; v->mode = 1; }
            }
        } else if (v->mode == 1) {
            int rate = ((adsr1 >> 4) & 7) * 2 + 16;
            if (rate_fires(s, rate)) {
                v->env -= ((v->env - 1) >> 8) + 1;
                if ((v->env >> 8) <= (adsr2 >> 5)) v->mode = 2;
            }
        } else {
            if (rate_fires(s, adsr2 & 31)) v->env -= ((v->env - 1) >> 8) + 1;
        }
    } else {
        v->mode = 4;
        if (!(gain & 0x80)) v->env = (gain & 0x7F) << 4;
        else if (rate_fires(s, gain & 31)) {
            switch ((gain >> 5) & 3) {
            case 0: v->env -= 32; break;
            case 1: v->env -= ((v->env - 1) >> 8) + 1; break;
            case 2: v->env += 32; break;
            case 3: v->env += v->env < 0x600 ? 32 : 8; break;
            }
        }
    }
    if (v->env < 0) v->env = 0;
    if (v->env > 0x7FF) v->env = 0x7FF;
}

static void dsp_sample(spc_t *s) {
    if (!gauss_ready) make_gauss();
    uint8_t flg = s->dsp[0x6C];
    /* key on (latched writes) and off */
    if (s->kon_new) {
        for (int n = 0; n < 8; n++) if (s->kon_new & (1 << n)) voice_kon(s, n);
        s->kon_new = 0;
    }
    uint8_t koff = s->dsp[0x5C];
    for (int n = 0; n < 8; n++) if ((koff & (1 << n)) && s->v[n].kon_delay == 0) s->v[n].mode = 3;
    if (flg & 0x80) for (int n = 0; n < 8; n++) { s->v[n].mode = 3; s->v[n].env = 0; }
    /* the shared rate counter */
    if (--s->counter < 0) s->counter = 0x77FF;
    if (rate_fires(s, flg & 31)) {
        int fb = ((s->noise << 14) ^ (s->noise << 13)) & 0x4000;
        s->noise = (uint16_t)((s->noise >> 1) | fb);
    }
    int ml = 0, mr = 0, el = 0, er = 0, prev = 0;
    uint8_t pmon = s->dsp[0x2D], non = s->dsp[0x3D], eon = s->dsp[0x4D];
    for (int n = 0; n < 8; n++) {
        spc_voice_t *v = &s->v[n];
        int out = 0;
        if (v->kon_delay > 0) {
            v->kon_delay--;
        } else {
            envelope(s, n);
            int pitch = ((VREG(n, 3) & 0x3F) << 8) | VREG(n, 2);
            if ((pmon & (1 << n)) && n > 0) pitch += ((prev >> 5) * pitch) >> 10;
            if (pitch < 0) pitch = 0;
            if (pitch > 0x3FFF) pitch = 0x3FFF;
            /* interpolate between the 4 samples around the position */
            const int16_t *g = gauss[(v->frac >> 4) & 0xFF];
            int smp = (g[0] * v->hist[0] >> 11) + (g[1] * v->hist[1] >> 11) + (g[2] * v->hist[2] >> 11) + (g[3] * v->hist[3] >> 11);
            smp = clamp16(smp);
            if (non & (1 << n)) smp = (int16_t)(s->noise << 1);
            out = (smp * v->env) >> 11;
            /* move on */
            v->frac += pitch;
            while (v->frac >= 0x1000) {
                v->frac -= 0x1000;
                if (v->pos >= 19) {
                    if (v->end_pending) { v->mode = 3; v->env = 0; v->end_pending = 0; }
                    brr_decode(s, v);
                }
                v->hist[0] = v->hist[1]; v->hist[1] = v->hist[2]; v->hist[2] = v->hist[3];
                v->hist[3] = v->buf[v->pos++];
            }
        }
        v->out = out;
        prev = out;
        VREG(n, 8) = (uint8_t)(v->env >> 4);
        VREG(n, 9) = (uint8_t)(out >> 8);
        int l = (out * (int8_t)VREG(n, 0)) >> 7, r = (out * (int8_t)VREG(n, 1)) >> 7;
        ml += l; mr += r;
        if (eon & (1 << n)) { el += l; er += r; }
    }
    ml = clamp16(ml); mr = clamp16(mr);
    el = clamp16(el); er = clamp16(er);
    /* echo: read the delayed sample, filter it, write the new one back */
    uint16_t ebase = (uint16_t)(s->dsp[0x6D] << 8);
    unsigned elen = (s->dsp[0x7D] & 15) * 0x800u;
    if (!elen) elen = 4;
    uint16_t ea = (uint16_t)(ebase + s->echo_pos);
    int rl = (int16_t)(s->ram[ea] | (s->ram[(uint16_t)(ea + 1)] << 8)) >> 1;
    int rr = (int16_t)(s->ram[(uint16_t)(ea + 2)] | (s->ram[(uint16_t)(ea + 3)] << 8)) >> 1;
    s->fir_pos = (s->fir_pos + 1) & 7;
    s->fir[0][s->fir_pos] = (int16_t)rl;
    s->fir[1][s->fir_pos] = (int16_t)rr;
    int fl = 0, fr = 0;
    for (int k = 0; k < 8; k++) {
        int c = (int8_t)s->dsp[(k << 4) | 0x0F];
        int h = (s->fir_pos + 1 + k) & 7;               /* C0 on the oldest, C7 on the newest */
        fl += (s->fir[0][h] * c) >> 6;
        fr += (s->fir[1][h] * c) >> 6;
    }
    fl = clamp16(fl); fr = clamp16(fr);
    int ol = clamp16(((ml * (int8_t)s->dsp[0x0C]) >> 7) + ((fl * (int8_t)s->dsp[0x2C]) >> 7));
    int orr = clamp16(((mr * (int8_t)s->dsp[0x1C]) >> 7) + ((fr * (int8_t)s->dsp[0x3C]) >> 7));
    if (!(flg & 0x20)) {
        int efb = (int8_t)s->dsp[0x0D];
        int wl = clamp16(el + ((fl * efb) >> 7)) & ~1, wr = clamp16(er + ((fr * efb) >> 7)) & ~1;
        s->ram[ea] = (uint8_t)wl; s->ram[(uint16_t)(ea + 1)] = (uint8_t)(wl >> 8);
        s->ram[(uint16_t)(ea + 2)] = (uint8_t)wr; s->ram[(uint16_t)(ea + 3)] = (uint8_t)(wr >> 8);
    }
    s->echo_pos = (uint16_t)(s->echo_pos + 4);
    if (s->echo_pos >= elen) s->echo_pos = 0;
    if (flg & 0x40) ol = orr = 0;                        /* mute */
    if (s->outbuf && s->outn + 2 <= s->outcap) {
        s->outbuf[s->outn++] = (int16_t)ol;
        s->outbuf[s->outn++] = (int16_t)orr;
    }
}

static void dsp_write(spc_t *s, uint8_t a, uint8_t v) {
    if (a >= 0x80) return;
    if (a == 0x4C) s->kon_new |= v;
    if (a == 0x7C) v = 0;                                /* writing ENDX clears it */
    s->dsp[a] = v;
}

/* ------------------------------------------------------------------ */
/* CPU                                                                 */

static void tick(spc_t *s, int cycles) {
    static const int period[3] = { 128, 128, 16 };
    for (int t = 0; t < 3; t++) {
        s->t_cycles[t] += cycles;
        while (s->t_cycles[t] >= period[t]) {
            s->t_cycles[t] -= period[t];
            if (s->control & (1 << t)) {
                if (++s->t_stage[t] == s->t_target[t]) {   /* target 0 is 256: the 8-bit stage wraps to it */
                    s->t_stage[t] = 0;
                    s->t_out[t] = (uint8_t)((s->t_out[t] + 1) & 15);
                }
            }
        }
    }
    s->dsp_cycles += cycles;
    while (s->dsp_cycles >= 32) { s->dsp_cycles -= 32; dsp_sample(s); }
}

static uint8_t rd(spc_t *s, uint16_t a) {
    if ((a & 0xFFF0) == 0x00F0) {
        switch (a) {
        case 0xF2: return s->dspaddr;
        case 0xF3: return s->dsp[s->dspaddr & 0x7F];
        case 0xF4: case 0xF5: case 0xF6: case 0xF7: return s->in[a - 0xF4];
        case 0xF8: case 0xF9: return s->ram[a];
        case 0xFD: case 0xFE: case 0xFF: { uint8_t v = s->t_out[a - 0xFD]; s->t_out[a - 0xFD] = 0; return v; }
        default: return 0;
        }
    }
    return s->ram[a];
}

static void wr(spc_t *s, uint16_t a, uint8_t v) {
    if ((a & 0xFFF0) == 0x00F0) {
        switch (a) {
        case 0xF1:
            for (int t = 0; t < 3; t++)
                if ((v & (1 << t)) && !(s->control & (1 << t))) { s->t_stage[t] = 0; s->t_out[t] = 0; }
            if (v & 0x10) s->in[0] = s->in[1] = 0;
            if (v & 0x20) s->in[2] = s->in[3] = 0;
            s->control = v;
            return;
        case 0xF2: s->dspaddr = v; return;
        case 0xF3: dsp_write(s, s->dspaddr, v); return;
        case 0xF4: case 0xF5: case 0xF6: case 0xF7: s->out[a - 0xF4] = v; return;
        case 0xFA: case 0xFB: case 0xFC: s->t_target[a - 0xFA] = v; return;
        default: break;
        }
    }
    s->ram[a] = v;
}

#define DP(o)       ((uint16_t)(((s->psw & FP) ? 0x100 : 0) | (uint8_t)(o)))
#define IMM()       rd(s, s->pc++)
#define SETNZ(v)    (s->psw = (uint8_t)((s->psw & ~(FN | FZ)) | ((v) & 0x80) | (((uint8_t)(v)) ? 0 : FZ)))
#define SETF(f, c)  (s->psw = (uint8_t)((c) ? (s->psw | (f)) : (s->psw & ~(f))))

static uint16_t rd16dp(spc_t *s, uint8_t o) { return (uint16_t)(rd(s, DP(o)) | (rd(s, DP((uint8_t)(o + 1))) << 8)); }
static void push(spc_t *s, uint8_t v) { wr(s, (uint16_t)(0x100 | s->sp--), v); }
static uint8_t pop(spc_t *s) { return rd(s, (uint16_t)(0x100 | ++s->sp)); }
static uint16_t abs16(spc_t *s) { uint16_t lo = IMM(); return (uint16_t)(lo | (IMM() << 8)); }

/* the operand address of the common addressing modes (the group's low 5 bits) */
static uint16_t ea_mode(spc_t *s, int mode) {
    switch (mode) {
    case 0x04: return DP(IMM());                                        /* d */
    case 0x05: return abs16(s);                                         /* !a */
    case 0x06: return DP(s->x);                                         /* (X) */
    case 0x07: { uint8_t o = (uint8_t)(IMM() + s->x); return (uint16_t)(rd(s, DP(o)) | (rd(s, DP((uint8_t)(o + 1))) << 8)); }   /* [d+X] */
    case 0x14: return DP((uint8_t)(IMM() + s->x));                      /* d+X */
    case 0x15: return (uint16_t)(abs16(s) + s->x);                      /* !a+X */
    case 0x16: return (uint16_t)(abs16(s) + s->y);                      /* !a+Y */
    case 0x17: { uint8_t o = IMM(); return (uint16_t)(rd16dp(s, o) + s->y); }   /* [d]+Y */
    }
    return 0;
}

static uint8_t op_adc(spc_t *s, uint8_t a, uint8_t b) {
    int r = a + b + (s->psw & FC);
    SETF(FV, (~(a ^ b) & (a ^ r)) & 0x80);
    SETF(FH, (a ^ b ^ r) & 0x10);
    SETF(FC, r > 0xFF);
    SETNZ(r);
    return (uint8_t)r;
}
static uint8_t op_sbc(spc_t *s, uint8_t a, uint8_t b) { return op_adc(s, a, (uint8_t)~b); }
static void op_cmp(spc_t *s, uint8_t a, uint8_t b) { int r = a - b; SETF(FC, r >= 0); SETNZ(r); }
static uint8_t op_asl(spc_t *s, uint8_t v) { SETF(FC, v & 0x80); v = (uint8_t)(v << 1); SETNZ(v); return v; }
static uint8_t op_lsr(spc_t *s, uint8_t v) { SETF(FC, v & 1); v >>= 1; SETNZ(v); return v; }
static uint8_t op_rol(spc_t *s, uint8_t v) { int c = s->psw & FC; SETF(FC, v & 0x80); v = (uint8_t)((v << 1) | c); SETNZ(v); return v; }
static uint8_t op_ror(spc_t *s, uint8_t v) { int c = s->psw & FC; SETF(FC, v & 1); v = (uint8_t)((v >> 1) | (c << 7)); SETNZ(v); return v; }

/* the ALU group: 0 OR, 1 AND, 2 EOR, 3 CMP, 4 ADC, 5 SBC */
static uint8_t alu(spc_t *s, int op, uint8_t a, uint8_t b) {
    switch (op) {
    case 0: a |= b; SETNZ(a); return a;
    case 1: a &= b; SETNZ(a); return a;
    case 2: a ^= b; SETNZ(a); return a;
    case 3: op_cmp(s, a, b); return a;
    case 4: return op_adc(s, a, b);
    default: return op_sbc(s, a, b);
    }
}

static int branch(spc_t *s, int cond) {
    int8_t rel = (int8_t)IMM();
    if (!cond) return 0;
    s->pc = (uint16_t)(s->pc + rel);
    return 2;
}

static const uint8_t cycles_tab[256] = {
/*        0 1 2 3 4 5 6 7 8 9 A B C D E F */
/* 0 */   2,8,4,5,3,4,3,6,2,6,5,4,5,4,6,8,
/* 1 */   2,8,4,5,4,5,5,6,5,5,6,5,2,2,4,6,
/* 2 */   2,8,4,5,3,4,3,6,2,6,5,4,5,4,5,4,
/* 3 */   2,8,4,5,4,5,5,6,5,5,6,5,2,2,3,8,
/* 4 */   2,8,4,5,3,4,3,6,2,6,4,4,5,4,6,6,
/* 5 */   2,8,4,5,4,5,5,6,5,5,4,5,2,2,4,3,
/* 6 */   2,8,4,5,3,4,3,6,2,6,4,4,5,4,5,5,
/* 7 */   2,8,4,5,4,5,5,6,5,5,5,5,2,2,3,6,
/* 8 */   2,8,4,5,3,4,3,6,2,6,5,4,5,2,4,5,
/* 9 */   2,8,4,5,4,5,5,6,5,5,5,5,2,2,12,5,
/* A */   3,8,4,5,3,4,3,6,2,6,4,4,5,2,4,4,
/* B */   2,8,4,5,4,5,5,6,5,5,5,5,2,2,3,4,
/* C */   3,8,4,5,4,5,4,7,2,5,6,4,5,2,4,9,
/* D */   2,8,4,5,5,6,6,7,4,5,5,5,2,2,6,3,
/* E */   2,8,4,5,3,4,3,6,2,4,5,3,4,3,4,3,
/* F */   2,8,4,5,4,5,5,6,3,4,5,4,2,2,4,2,
};

static int step(spc_t *s) {
    if (s->stopped) return 2;
    uint8_t op = IMM();
    int extra = 0, lo = op & 0x1F, hi = op >> 5;
    /* the ALU rows: OR AND EOR CMP ADC SBC A, <mode> */
    if (hi <= 5 && (lo == 0x04 || lo == 0x05 || lo == 0x06 || lo == 0x07 || lo == 0x14 || lo == 0x15 || lo == 0x16 || lo == 0x17)) {
        uint8_t m = rd(s, ea_mode(s, lo));
        s->a = alu(s, hi, s->a, m);
        return cycles_tab[op];
    }
    if (hi <= 5 && lo == 0x08) { s->a = alu(s, hi, s->a, IMM()); return cycles_tab[op]; }                 /* A, #i */
    if (hi <= 5 && lo == 0x09) {                                                                          /* d, d */
        uint8_t m = rd(s, DP(IMM())); uint16_t d = DP(IMM());
        uint8_t r = alu(s, hi, rd(s, d), m);
        if (hi != 3) wr(s, d, r);
        return cycles_tab[op];
    }
    if (hi <= 5 && lo == 0x18) {                                                                          /* d, #i */
        uint8_t m = IMM(); uint16_t d = DP(IMM());
        uint8_t r = alu(s, hi, rd(s, d), m);
        if (hi != 3) wr(s, d, r);
        return cycles_tab[op];
    }
    if (hi <= 5 && lo == 0x19) {                                                                          /* (X), (Y) */
        uint8_t m = rd(s, DP(s->y)); uint16_t d = DP(s->x);
        uint8_t r = alu(s, hi, rd(s, d), m);
        if (hi != 3) wr(s, d, r);
        return cycles_tab[op];
    }
    if ((op & 0x0F) == 0x01) {                                                                            /* TCALL n */
        int n = op >> 4;
        push(s, (uint8_t)(s->pc >> 8)); push(s, (uint8_t)s->pc);
        uint16_t va = (uint16_t)(0xFFDE - 2 * n);
        s->pc = (uint16_t)(rd(s, va) | (rd(s, (uint16_t)(va + 1)) << 8));
        return 8;
    }
    if ((op & 0x0F) == 0x02) {                                                                            /* SET1 / CLR1 d.b */
        uint16_t d = DP(IMM()); int b = op >> 5;
        uint8_t v = rd(s, d);
        wr(s, d, (op & 0x10) ? (uint8_t)(v & ~(1 << b)) : (uint8_t)(v | (1 << b)));
        return 4;
    }
    if ((op & 0x0F) == 0x03) {                                                                            /* BBS / BBC d.b, r */
        uint8_t v = rd(s, DP(IMM())); int b = op >> 5;
        int set = (v >> b) & 1;
        return 5 + branch(s, (op & 0x10) ? !set : set);
    }
    switch (op) {
    case 0x00: break;                                                                                     /* NOP */
    /* shifts and rotates */
    case 0x0B: { uint16_t d = DP(IMM()); wr(s, d, op_asl(s, rd(s, d))); break; }
    case 0x0C: { uint16_t d = abs16(s); wr(s, d, op_asl(s, rd(s, d))); break; }
    case 0x1B: { uint16_t d = DP((uint8_t)(IMM() + s->x)); wr(s, d, op_asl(s, rd(s, d))); break; }
    case 0x1C: s->a = op_asl(s, s->a); break;
    case 0x2B: { uint16_t d = DP(IMM()); wr(s, d, op_rol(s, rd(s, d))); break; }
    case 0x2C: { uint16_t d = abs16(s); wr(s, d, op_rol(s, rd(s, d))); break; }
    case 0x3B: { uint16_t d = DP((uint8_t)(IMM() + s->x)); wr(s, d, op_rol(s, rd(s, d))); break; }
    case 0x3C: s->a = op_rol(s, s->a); break;
    case 0x4B: { uint16_t d = DP(IMM()); wr(s, d, op_lsr(s, rd(s, d))); break; }
    case 0x4C: { uint16_t d = abs16(s); wr(s, d, op_lsr(s, rd(s, d))); break; }
    case 0x5B: { uint16_t d = DP((uint8_t)(IMM() + s->x)); wr(s, d, op_lsr(s, rd(s, d))); break; }
    case 0x5C: s->a = op_lsr(s, s->a); break;
    case 0x6B: { uint16_t d = DP(IMM()); wr(s, d, op_ror(s, rd(s, d))); break; }
    case 0x6C: { uint16_t d = abs16(s); wr(s, d, op_ror(s, rd(s, d))); break; }
    case 0x7B: { uint16_t d = DP((uint8_t)(IMM() + s->x)); wr(s, d, op_ror(s, rd(s, d))); break; }
    case 0x7C: s->a = op_ror(s, s->a); break;
    /* increments and decrements */
    case 0x8B: { uint16_t d = DP(IMM()); uint8_t v = (uint8_t)(rd(s, d) - 1); SETNZ(v); wr(s, d, v); break; }
    case 0x8C: { uint16_t d = abs16(s); uint8_t v = (uint8_t)(rd(s, d) - 1); SETNZ(v); wr(s, d, v); break; }
    case 0x9B: { uint16_t d = DP((uint8_t)(IMM() + s->x)); uint8_t v = (uint8_t)(rd(s, d) - 1); SETNZ(v); wr(s, d, v); break; }
    case 0x9C: s->a--; SETNZ(s->a); break;
    case 0x1D: s->x--; SETNZ(s->x); break;
    case 0xDC: s->y--; SETNZ(s->y); break;
    case 0xAB: { uint16_t d = DP(IMM()); uint8_t v = (uint8_t)(rd(s, d) + 1); SETNZ(v); wr(s, d, v); break; }
    case 0xAC: { uint16_t d = abs16(s); uint8_t v = (uint8_t)(rd(s, d) + 1); SETNZ(v); wr(s, d, v); break; }
    case 0xBB: { uint16_t d = DP((uint8_t)(IMM() + s->x)); uint8_t v = (uint8_t)(rd(s, d) + 1); SETNZ(v); wr(s, d, v); break; }
    case 0xBC: s->a++; SETNZ(s->a); break;
    case 0x3D: s->x++; SETNZ(s->x); break;
    case 0xFC: s->y++; SETNZ(s->y); break;
    /* 16-bit */
    case 0x1A: case 0x3A: {                                                                               /* DECW / INCW d */
        uint8_t o = IMM();
        uint16_t w = (uint16_t)(rd16dp(s, o) + (op == 0x3A ? 1 : -1));
        wr(s, DP(o), (uint8_t)w); wr(s, DP((uint8_t)(o + 1)), (uint8_t)(w >> 8));
        SETF(FN, w & 0x8000); SETF(FZ, w == 0);
        break;
    }
    case 0x5A: {                                                                                          /* CMPW YA, d */
        uint16_t w = rd16dp(s, IMM()), ya = (uint16_t)((s->y << 8) | s->a);
        int r = ya - w;
        SETF(FC, r >= 0); SETF(FN, r & 0x8000); SETF(FZ, (r & 0xFFFF) == 0);
        break;
    }
    case 0x7A: case 0x9A: {                                                                               /* ADDW / SUBW YA, d */
        uint16_t w = rd16dp(s, IMM()), ya = (uint16_t)((s->y << 8) | s->a);
        int r;
        if (op == 0x7A) {
            r = ya + w;
            SETF(FC, r > 0xFFFF);
            SETF(FV, (~(ya ^ w) & (ya ^ r)) & 0x8000);
            SETF(FH, (ya ^ w ^ r) & 0x1000);
        } else {
            r = ya - w;
            SETF(FC, r >= 0);
            SETF(FV, ((ya ^ w) & (ya ^ r)) & 0x8000);
            SETF(FH, !((ya ^ w ^ r) & 0x1000));
        }
        s->a = (uint8_t)r; s->y = (uint8_t)(r >> 8);
        SETF(FN, r & 0x8000); SETF(FZ, (r & 0xFFFF) == 0);
        break;
    }
    case 0xBA: {                                                                                          /* MOVW YA, d */
        uint8_t o = IMM();
        s->a = rd(s, DP(o)); s->y = rd(s, DP((uint8_t)(o + 1)));
        SETF(FN, s->y & 0x80); SETF(FZ, !(s->a | s->y));
        break;
    }
    case 0xDA: { uint8_t o = IMM(); wr(s, DP(o), s->a); wr(s, DP((uint8_t)(o + 1)), s->y); break; }      /* MOVW d, YA */
    case 0xCF: { unsigned r = (unsigned)s->y * s->a; s->a = (uint8_t)r; s->y = (uint8_t)(r >> 8); SETNZ(s->y); break; }   /* MUL YA */
    case 0x9E: {                                                                                          /* DIV YA, X */
        unsigned ya = (unsigned)((s->y << 8) | s->a), x = s->x;
        SETF(FH, (s->y & 15) >= (x & 15));
        SETF(FV, s->y >= x);
        if (s->y < (x << 1)) { s->a = (uint8_t)(ya / x); s->y = (uint8_t)(ya % x); }
        else { s->a = (uint8_t)(255 - (ya - (x << 9)) / (256 - x)); s->y = (uint8_t)(x + (ya - (x << 9)) % (256 - x)); }
        SETNZ(s->a);
        break;
    }
    case 0xDF:                                                                                            /* DAA */
        if ((s->psw & FC) || s->a > 0x99) { s->a = (uint8_t)(s->a + 0x60); s->psw |= FC; }
        if ((s->psw & FH) || (s->a & 15) > 9) s->a = (uint8_t)(s->a + 6);
        SETNZ(s->a);
        break;
    case 0xBE:                                                                                            /* DAS */
        if (!(s->psw & FC) || s->a > 0x99) { s->a = (uint8_t)(s->a - 0x60); s->psw &= (uint8_t)~FC; }
        if (!(s->psw & FH) || (s->a & 15) > 9) s->a = (uint8_t)(s->a - 6);
        SETNZ(s->a);
        break;
    case 0x9F: s->a = (uint8_t)((s->a >> 4) | (s->a << 4)); SETNZ(s->a); break;                          /* XCN */
    /* compares with X and Y */
    case 0x1E: op_cmp(s, s->x, rd(s, abs16(s))); break;
    case 0x3E: op_cmp(s, s->x, rd(s, DP(IMM()))); break;
    case 0xC8: op_cmp(s, s->x, IMM()); break;
    case 0x5E: op_cmp(s, s->y, rd(s, abs16(s))); break;
    case 0x7E: op_cmp(s, s->y, rd(s, DP(IMM()))); break;
    case 0xAD: op_cmp(s, s->y, IMM()); break;
    /* loads */
    case 0xE4: s->a = rd(s, DP(IMM())); SETNZ(s->a); break;
    case 0xE5: s->a = rd(s, abs16(s)); SETNZ(s->a); break;
    case 0xE6: s->a = rd(s, DP(s->x)); SETNZ(s->a); break;
    case 0xE7: s->a = rd(s, ea_mode(s, 0x07)); SETNZ(s->a); break;
    case 0xE8: s->a = IMM(); SETNZ(s->a); break;
    case 0xF4: s->a = rd(s, ea_mode(s, 0x14)); SETNZ(s->a); break;
    case 0xF5: s->a = rd(s, ea_mode(s, 0x15)); SETNZ(s->a); break;
    case 0xF6: s->a = rd(s, ea_mode(s, 0x16)); SETNZ(s->a); break;
    case 0xF7: s->a = rd(s, ea_mode(s, 0x17)); SETNZ(s->a); break;
    case 0xBF: s->a = rd(s, DP(s->x)); s->x++; SETNZ(s->a); break;                                        /* MOV A, (X)+ */
    case 0xCD: s->x = IMM(); SETNZ(s->x); break;
    case 0xE9: s->x = rd(s, abs16(s)); SETNZ(s->x); break;
    case 0xF8: s->x = rd(s, DP(IMM())); SETNZ(s->x); break;
    case 0xF9: s->x = rd(s, DP((uint8_t)(IMM() + s->y))); SETNZ(s->x); break;
    case 0x8D: s->y = IMM(); SETNZ(s->y); break;
    case 0xEC: s->y = rd(s, abs16(s)); SETNZ(s->y); break;
    case 0xEB: s->y = rd(s, DP(IMM())); SETNZ(s->y); break;
    case 0xFB: s->y = rd(s, DP((uint8_t)(IMM() + s->x))); SETNZ(s->y); break;
    /* stores (the CPU reads the target first; only the I/O counters would notice) */
    case 0xC4: wr(s, DP(IMM()), s->a); break;
    case 0xC5: wr(s, abs16(s), s->a); break;
    case 0xC6: wr(s, DP(s->x), s->a); break;
    case 0xC7: wr(s, ea_mode(s, 0x07), s->a); break;
    case 0xD4: wr(s, ea_mode(s, 0x14), s->a); break;
    case 0xD5: wr(s, ea_mode(s, 0x15), s->a); break;
    case 0xD6: wr(s, ea_mode(s, 0x16), s->a); break;
    case 0xD7: wr(s, ea_mode(s, 0x17), s->a); break;
    case 0xAF: wr(s, DP(s->x), s->a); s->x++; break;                                                      /* MOV (X)+, A */
    case 0xC9: wr(s, abs16(s), s->x); break;
    case 0xD8: wr(s, DP(IMM()), s->x); break;
    case 0xD9: wr(s, DP((uint8_t)(IMM() + s->y)), s->x); break;
    case 0xCB: wr(s, DP(IMM()), s->y); break;
    case 0xCC: wr(s, abs16(s), s->y); break;
    case 0xDB: wr(s, DP((uint8_t)(IMM() + s->x)), s->y); break;
    case 0x8F: { uint8_t v = IMM(); wr(s, DP(IMM()), v); break; }                                        /* MOV d, #i */
    case 0xFA: { uint8_t v = rd(s, DP(IMM())); wr(s, DP(IMM()), v); break; }                              /* MOV d, d */
    /* transfers */
    case 0x5D: s->x = s->a; SETNZ(s->x); break;
    case 0x7D: s->a = s->x; SETNZ(s->a); break;
    case 0xDD: s->a = s->y; SETNZ(s->a); break;
    case 0xFD: s->y = s->a; SETNZ(s->y); break;
    case 0x9D: s->x = s->sp; SETNZ(s->x); break;
    case 0xBD: s->sp = s->x; break;
    /* stack */
    case 0x0D: push(s, s->psw); break;
    case 0x2D: push(s, s->a); break;
    case 0x4D: push(s, s->x); break;
    case 0x6D: push(s, s->y); break;
    case 0x8E: s->psw = pop(s); break;
    case 0xAE: s->a = pop(s); break;
    case 0xCE: s->x = pop(s); break;
    case 0xEE: s->y = pop(s); break;
    /* branches */
    case 0x10: extra = branch(s, !(s->psw & FN)); break;
    case 0x30: extra = branch(s, s->psw & FN); break;
    case 0x50: extra = branch(s, !(s->psw & FV)); break;
    case 0x70: extra = branch(s, s->psw & FV); break;
    case 0x90: extra = branch(s, !(s->psw & FC)); break;
    case 0xB0: extra = branch(s, s->psw & FC); break;
    case 0xD0: extra = branch(s, !(s->psw & FZ)); break;
    case 0xF0: extra = branch(s, s->psw & FZ); break;
    case 0x2F: branch(s, 1); break;                                                                       /* BRA (its 4 cycles are the table's) */
    case 0x2E: { uint8_t v = rd(s, DP(IMM())); extra = branch(s, s->a != v); break; }                     /* CBNE d, r */
    case 0xDE: { uint8_t v = rd(s, DP((uint8_t)(IMM() + s->x))); extra = branch(s, s->a != v); break; }   /* CBNE d+X, r */
    case 0x6E: { uint16_t d = DP(IMM()); uint8_t v = (uint8_t)(rd(s, d) - 1); wr(s, d, v); extra = branch(s, v != 0); break; }   /* DBNZ d, r */
    case 0xFE: s->y--; extra = branch(s, s->y != 0); break;                                               /* DBNZ Y, r */
    /* jumps and calls */
    case 0x5F: s->pc = abs16(s); break;
    case 0x1F: { uint16_t p = (uint16_t)(abs16(s) + s->x); s->pc = (uint16_t)(rd(s, p) | (rd(s, (uint16_t)(p + 1)) << 8)); break; }
    case 0x3F: { uint16_t t = abs16(s); push(s, (uint8_t)(s->pc >> 8)); push(s, (uint8_t)s->pc); s->pc = t; break; }
    case 0x4F: { uint8_t u = IMM(); push(s, (uint8_t)(s->pc >> 8)); push(s, (uint8_t)s->pc); s->pc = (uint16_t)(0xFF00 | u); break; }
    case 0x6F: { uint16_t l = pop(s); s->pc = (uint16_t)(l | (pop(s) << 8)); break; }                     /* RET */
    case 0x7F: { s->psw = pop(s); uint16_t l = pop(s); s->pc = (uint16_t)(l | (pop(s) << 8)); break; }  /* RETI */
    case 0x0F:                                                                                            /* BRK */
        push(s, (uint8_t)(s->pc >> 8)); push(s, (uint8_t)s->pc); push(s, s->psw);
        s->psw = (uint8_t)((s->psw | FB) & ~FI);
        s->pc = (uint16_t)(rd(s, 0xFFDE) | (rd(s, 0xFFDF) << 8));
        break;
    /* flags */
    case 0x20: s->psw &= (uint8_t)~FP; break;
    case 0x40: s->psw |= FP; break;
    case 0x60: s->psw &= (uint8_t)~FC; break;
    case 0x80: s->psw |= FC; break;
    case 0xED: s->psw ^= FC; break;
    case 0xE0: s->psw &= (uint8_t)~(FV | FH); break;
    case 0xA0: s->psw |= FI; break;
    case 0xC0: s->psw &= (uint8_t)~FI; break;
    /* bits of memory */
    case 0x0E: case 0x4E: {                                                                               /* TSET1 / TCLR1 !a */
        uint16_t d = abs16(s); uint8_t v = rd(s, d);
        SETNZ((uint8_t)(s->a - v));
        wr(s, d, op == 0x0E ? (uint8_t)(v | s->a) : (uint8_t)(v & ~s->a));
        break;
    }
    case 0x0A: case 0x2A: case 0x4A: case 0x6A: case 0x8A: case 0xAA: case 0xCA: case 0xEA: {            /* the m.b group */
        uint16_t w = abs16(s), d = (uint16_t)(w & 0x1FFF);
        int b = w >> 13, bit = (rd(s, d) >> b) & 1, c = s->psw & FC;
        switch (op) {
        case 0x0A: SETF(FC, c | bit); break;
        case 0x2A: SETF(FC, c | !bit); break;
        case 0x4A: SETF(FC, c & bit); break;
        case 0x6A: SETF(FC, c & !bit); break;
        case 0x8A: SETF(FC, c ^ bit); break;
        case 0xAA: SETF(FC, bit); break;
        case 0xCA: { uint8_t v = rd(s, d); wr(s, d, c ? (uint8_t)(v | (1 << b)) : (uint8_t)(v & ~(1 << b))); break; }
        case 0xEA: { uint8_t v = rd(s, d); wr(s, d, (uint8_t)(v ^ (1 << b))); break; }
        }
        break;
    }
    case 0xEF: case 0xFF: s->stopped = 1; break;                                                          /* SLEEP, STOP */
    default: break;
    }
    return cycles_tab[op] + extra;
}

/* ------------------------------------------------------------------ */

void spc_reset(spc_t *s) {
    int16_t *buf = s->outbuf; int cap = s->outcap;
    memset(s, 0, sizeof *s);
    s->outbuf = buf; s->outcap = cap;
    s->sp = 0xEF;
    s->dsp[0x6C] = 0xE0;                                 /* FLG: reset, mute, echo off */
    s->noise = 0x4000;
    s->counter = 0;
    for (int t = 0; t < 3; t++) s->t_target[t] = 0;
}

void spc_load(spc_t *s, uint16_t addr, const uint8_t *data, int len) {
    for (int i = 0; i < len; i++) s->ram[(uint16_t)(addr + i)] = data[i];
}

void spc_start(spc_t *s, uint16_t pc) {
    s->pc = pc;
    s->a = s->x = s->y = 0;
    s->sp = 0xEF;
    s->psw = 0x02;
    s->stopped = 0;
}

void spc_run(spc_t *s, int cycles) {
    cycles -= s->debt;
    while (cycles > 0) {
        int c = step(s);
        tick(s, c);
        cycles -= c;
    }
    s->debt = -cycles;
}
