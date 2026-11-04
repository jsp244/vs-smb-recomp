/*
 * spc700.h - the SNES sound unit: the SPC700 CPU with its timers and ports, and the S-DSP
 * (8 voices of BRR samples, ADSR / GAIN envelopes, noise, pitch modulation, echo with its FIR
 * filter). Super Mario All-Stars' music runs on it (runtime/allstars.c loads its sound program).
 * No boot ROM: programs are written into its RAM and started directly.
 */
#ifndef SPC700_H
#define SPC700_H

#include <stdint.h>

#define SPC_CLOCK   1024000         /* SPC700 cycles a second */
#define SPC_RATE    32000           /* DSP samples a second: one every 32 cycles */

typedef struct {
    int16_t  buf[19];               /* the block's 16 samples after the 3 they continue from */
    int      pos;                   /* next sample in buf, 3 .. 18 */
    uint16_t addr, loop;            /* current BRR block, the sample's loop point */
    int      frac;                  /* position between samples, 12 bits */
    int16_t  hist[4];               /* the 4 samples interpolated between, oldest first */
    int16_t  p1, p2;                /* the decoder's last two outputs */
    int      env, mode;             /* envelope 0..0x7FF; 0 attack, 1 decay, 2 sustain, 3 release, 4 gain */
    int      kon_delay;
    int      end_pending;           /* the block just used ended the sample (no loop) */
    int      out;                   /* last output, for the next voice's pitch modulation */
} spc_voice_t;

typedef struct {
    uint8_t  ram[0x10000];
    /* CPU */
    uint8_t  a, x, y, sp, psw;
    uint16_t pc;
    int      stopped;
    /* I/O */
    uint8_t  control, dspaddr;
    uint8_t  in[4];                 /* what the main CPU wrote to $2140-$2143 */
    uint8_t  out[4];                /* what it reads there */
    uint8_t  t_target[3], t_stage[3], t_out[3];
    int      t_cycles[3];           /* cycles into the timer's current tick */
    int      dsp_cycles;            /* cycles into the current DSP sample */
    int      debt;                  /* cycles the last run went over */
    /* DSP */
    uint8_t  dsp[128];
    uint8_t  kon_new;
    spc_voice_t v[8];
    int      counter;               /* the envelope / noise rate counter */
    uint16_t noise;
    uint16_t echo_pos;
    int16_t  fir[2][8];
    int      fir_pos;
    /* output: stereo samples at SPC_RATE */
    int16_t *outbuf;
    int      outcap, outn;
} spc_t;

void    spc_reset(spc_t *s);
/* writes a program block into its RAM / starts the CPU at pc */
void    spc_load(spc_t *s, uint16_t addr, const uint8_t *data, int len);
void    spc_start(spc_t *s, uint16_t pc);
/* runs `cycles` CPU cycles; the DSP's samples go to s->outbuf (L, R pairs) while there is room */
void    spc_run(spc_t *s, int cycles);

#endif
