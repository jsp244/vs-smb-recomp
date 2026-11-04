/*
 * cpu816.h - a 65C816 interpreter (the SNES CPU's core), used to run routines of the player's own
 * Super Mario All-Stars ROM in a sandbox (allstars.c): the code that builds a level's backgrounds,
 * palettes and graphics there, so the remastered picture comes from that game's own logic.
 *
 * The bus is the caller's: read and write get 24-bit addresses. No timing beyond an approximate
 * cycle count, no SNES hardware: that belongs to the caller's bus.
 */
#ifndef CPU816_H
#define CPU816_H
#include <stdint.h>

typedef struct cpu816 {
    uint16_t a, x, y, s, d, pc;
    uint8_t  db, pb;
    uint8_t  p;          /* N V M X D I Z C */
    uint8_t  e;          /* emulation mode */
    uint8_t  stopped;    /* STP or WAI ran, or cpu816_call ran out of steps */
    uint8_t  trap;       /* BRK or COP ran */
    uint64_t cycles;
    void    *ctx;
    uint8_t (*read)(void *ctx, uint32_t addr);
    void    (*write)(void *ctx, uint32_t addr, uint8_t v);
} cpu816_t;

#define P816_C 0x01
#define P816_Z 0x02
#define P816_I 0x04
#define P816_D 0x08
#define P816_X 0x10
#define P816_M 0x20
#define P816_V 0x40
#define P816_N 0x80

void cpu816_reset(cpu816_t *c);            /* emulation mode, PC from the reset vector */
void cpu816_step(cpu816_t *c);             /* one instruction */
void cpu816_interrupt(cpu816_t *c, int nmi);  /* take an NMI (1) or IRQ (0), whatever the I flag says */

/* Calls the routine at pb:pc as a JSL would (or, with rts set, as a JSR from the same bank) and runs
   until it returns, at most max_steps instructions. Returns the instructions run, or -1 when the
   routine did not return in time or stopped (STP, WAI, BRK, COP). */
int cpu816_call(cpu816_t *c, uint32_t addr, int rts, int max_steps);

#endif
