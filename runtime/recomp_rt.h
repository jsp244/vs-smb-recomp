/*
 * recomp_rt.h - helpers used by the C code emitted by recomp/recomp.py.
 *
 * Every 6502 instruction becomes a `case` label inside a routine function,
 * so execution can resume at any instruction boundary (after an interrupt,
 * a cross-bank call, a bank switch or an interpreter fallback).
 */
#ifndef RECOMP_RT_H
#define RECOMP_RT_H
#include "nes.h"

#define A  cpu.a
#define X  cpu.x
#define Y  cpu.y
#define SP cpu.sp

#define CYC(n) (cpu.cycles += (n))
/* end of instruction: advance the PPU/APU; leave the routine if an interrupt or the frame end is pending */
#define SYNC(next) do { if (sys_sync()) { cpu.pc = (next); return; } } while (0)
/* leave the routine unconditionally (control transfer the dispatcher must resolve) */
#define EXIT(next) do { sys_sync(); cpu.pc = (next); return; } while (0)

/* an instruction marked "hook" in recomp/symbols.txt: the runtime may look at the machine before it runs */
#define HOOK(addr) do { if (sys.hook) sys.hook(addr); } while (0)
/* one marked "patch": the runtime may change the machine before it runs (see sys.patch); the generated
   code keeps no register copies of its own, so what the patch writes is what the instruction sees */
#define PATCH(addr) do { if (sys.patch) sys.patch(addr); } while (0)

#ifdef RECOMP_TRACE
#define INSN(addr) do { cpu.pc = (addr); sys.instructions++; if (sys.trace_hook) sys.trace_hook(); } while (0)
#else
#define INSN(addr) (sys.instructions++)
#endif

/* indexed read: dummy read + 1 cycle on page crossing */
static inline uint8_t rd_idx(uint16_t b, uint8_t idx) {
    uint16_t a = (uint16_t)(b + idx);
    if ((b ^ a) & 0x100) { mem_read((uint16_t)((b & 0xff00) | (a & 0xff))); cpu.cycles++; }
    return mem_read(a);
}
/* same, when base..base+255 is known to be RAM */
static inline uint8_t rd_idx_ram(uint16_t b, uint8_t idx) {
    uint16_t a = (uint16_t)(b + idx);
    cpu.cycles += ((b ^ a) >> 8) & 1;
    return ram[a & 0x7ff];
}
/* effective address for indexed writes/RMW: always performs the dummy read */
static inline uint16_t addr_idx_wr(uint16_t b, uint8_t idx) {
    uint16_t a = (uint16_t)(b + idx);
    mem_read((uint16_t)((b & 0xff00) | (a & 0xff)));
    return a;
}
/* read-modify-write with the 6502's double write */
#define RMW_AT(addr, FN) do { uint16_t rmw_a = (addr); uint8_t rmw_v = mem_read(rmw_a); mem_write(rmw_a, rmw_v); rmw_v = FN(rmw_v); mem_write(rmw_a, rmw_v); } while (0)

static inline uint16_t jmp_ind(uint16_t p) {
    uint16_t lo = mem_read(p);
    uint16_t hi = mem_read((uint16_t)((p & 0xff00) | ((p + 1) & 0xff)));
    return (uint16_t)(lo | (hi << 8));
}

typedef void (*recomp_fn)(void);
typedef struct {
    const uint16_t *entry;      /* per 8 KiB offset: routine index + 1, 0 = not translated */
    const recomp_fn *funcs;
} recomp_unit_t;

#endif
