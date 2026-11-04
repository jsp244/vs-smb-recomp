/*
 * spc700.h - the SNES sound unit: the SPC700 CPU with its timers and ports, and the S-DSP (8 voices of
 * BRR samples, ADSR / GAIN envelopes, noise, pitch modulation, echo with its FIR filter), as LakeSnes
 * emulates them (spc700.c; its licence is there). Super Mario All-Stars' music runs on it
 * (runtime/allstars.c loads its sound program). No boot ROM: programs are written into its RAM and
 * started directly.
 */
#ifndef SPC700_H
#define SPC700_H

#include <stdbool.h>
#include <stdint.h>

#define SPC_CLOCK   1024000         /* SPC700 cycles a second */
#define SPC_RATE    32000           /* DSP samples a second: one every 32 cycles */

/* LakeSnes's state (its snes/spc.h, dsp.h and apu.h) */
typedef struct {
    void*    mem;                   /* the spc_t it belongs to */
    uint8_t  a, x, y, sp;
    uint16_t pc;
    bool     c, z, v, n, i, h, p, b;
    bool     stopped;
    bool     resetWanted;
} spc_cpu_t;

typedef struct {
    uint16_t pitch, pitchCounter;
    bool     pitchModulation;
    int16_t  decodeBuffer[12];
    uint8_t  bufferOffset, srcn;
    uint16_t decodeOffset;
    uint8_t  blockOffset, brrHeader;
    bool     useNoise;
    uint8_t  startDelay;
    uint8_t  adsrRates[4];          /* attack, decay, sustain, gain */
    uint8_t  adsrState;             /* 0 attack, 1 decay, 2 sustain, 3 release */
    uint8_t  sustainLevel, gainSustainLevel;
    bool     useGain;
    uint8_t  gainMode;
    bool     directGain;
    uint16_t gainValue, preclampGain, gain;
    bool     keyOn, keyOff;
    int16_t  sampleOut;
    int8_t   volumeL, volumeR;
    bool     echoEnable;
} spc_voice_t;

typedef struct {
    uint8_t*   aram;                /* the unit's 64 KiB of RAM */
    uint8_t    ram[0x80];           /* the registers */
    spc_voice_t channel[8];
    uint16_t   counter, dirPage;
    bool       evenCycle, mute, reset;
    int8_t     masterVolumeL, masterVolumeR;
    int16_t    sampleOutL, sampleOutR, echoOutL, echoOutR;
    int16_t    noiseSample;
    uint8_t    noiseRate;
    bool       echoWrites;
    int8_t     echoVolumeL, echoVolumeR, feedbackVolume;
    uint16_t   echoBufferAdr, echoDelay, echoLength, echoBufferIndex;
    uint8_t    firBufferIndex;
    int8_t     firValues[8];
    int16_t    firBufferL[8], firBufferR[8];
} spc_dsp_t;

typedef struct {
    uint8_t cycles, divider, target, counter;
    bool    enabled;
} spc_timer_t;

typedef struct {
    uint8_t     ram[0x10000];
    uint8_t     in[6];              /* what the main CPU wrote to $2140-$2143; then $F8/$F9 */
    uint8_t     out[4];             /* what it reads there */
    uint8_t     dspAdr;
    uint32_t    cycles;
    spc_timer_t timer[3];
    spc_cpu_t   cpu;
    spc_dsp_t   dsp;
    int         debt;               /* cycles the last run went over */
    /* output: stereo samples at SPC_RATE */
    int16_t    *outbuf;
    int         outcap, outn;
} spc_t;

void    spc_reset(spc_t *s);
/* writes a program block into its RAM / starts the CPU at pc */
void    spc_load(spc_t *s, uint16_t addr, const uint8_t *data, int len);
void    spc_start(spc_t *s, uint16_t pc);
/* runs `cycles` CPU cycles; the DSP's samples go to s->outbuf (L, R pairs) while there is room */
void    spc_run(spc_t *s, int cycles);

#endif
