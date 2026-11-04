/*
 * spc700.c - the SNES sound unit: SPC700, timers, ports, S-DSP. See spc700.h.
 *
 * This is LakeSnes's emulation (github.com/angelo-wf/LakeSnes, commit 9db90b8): its snes/spc.c and
 * snes/dsp.c below as they are there, renamed to keep them inside this file (Spc -> spc_cpu_t,
 * spc_* -> cpu_*, Dsp -> spc_dsp_t), and its snes/apu.c (the timers, ports and the CPU's memory map)
 * at the end, rewritten over spc_t. Left out: the boot ROM (programs are written into RAM and
 * started directly, so it is never needed), save states, and the resampling of the DSP's output to
 * a frame rate: its samples go to the caller's buffer as they are made, 32,000 a second.
 * From the same SPC snapshot of All-Stars' music its output matches blargg's snes_spc (the core of most
 * SNES players) to a correlation of 0.999.
 *
 * LakeSnes's licence:
 *
 * MIT License
 *
 * Copyright (c) 2021-2023 angelo_wf and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "spc700.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* the CPU's bus: one call a cycle (apu.c's apu_spcRead / apu_spcWrite / apu_spcIdle) */
static uint8_t smp_cpuRead(spc_t *s, uint16_t adr);
static void smp_cpuWrite(spc_t *s, uint16_t adr, uint8_t val);
static void smp_cpuIdle(spc_t *s);

/* ---- the SPC700 (LakeSnes snes/spc.c) ---- */
static uint8_t cpu_read(spc_cpu_t* spc, uint16_t adr);
static void cpu_write(spc_cpu_t* spc, uint16_t adr, uint8_t val);
static void cpu_idle(spc_cpu_t* spc);
static void cpu_idleWait(spc_cpu_t* spc);
static uint8_t cpu_readOpcode(spc_cpu_t* spc);
static uint16_t cpu_readOpcodeWord(spc_cpu_t* spc);
static uint8_t cpu_getFlags(spc_cpu_t* spc);
static void cpu_setFlags(spc_cpu_t* spc, uint8_t value);
static void cpu_setZN(spc_cpu_t* spc, uint8_t value);
static void cpu_doBranch(spc_cpu_t* spc, uint8_t value, bool check);
static uint8_t cpu_pullByte(spc_cpu_t* spc);
static void cpu_pushByte(spc_cpu_t* spc, uint8_t value);
static uint16_t cpu_pullWord(spc_cpu_t* spc);
static void cpu_pushWord(spc_cpu_t* spc, uint16_t value);
static uint16_t cpu_readWord(spc_cpu_t* spc, uint16_t adrl, uint16_t adrh);
static void cpu_writeWord(spc_cpu_t* spc, uint16_t adrl, uint16_t adrh, uint16_t value);
static void cpu_doOpcode(spc_cpu_t* spc, uint8_t opcode);


static void cpu_reset(spc_cpu_t* spc, bool hard) {
  if(hard) {
    spc->a = 0;
    spc->x = 0;
    spc->y = 0;
    spc->sp = 0;
    spc->pc = 0;
    spc->c = false;
    spc->z = false;
    spc->v = false;
    spc->n = false;
    spc->i = false;
    spc->h = false;
    spc->p = false;
    spc->b = false;
  }
  spc->stopped = false;
  spc->resetWanted = true;
}

static void cpu_runOpcode(spc_cpu_t* spc) {
  if(spc->resetWanted) {
    // based on 6502, brk without writes
    spc->resetWanted = false;
    cpu_read(spc, spc->pc);
    cpu_read(spc, spc->pc);
    cpu_read(spc, 0x100 | spc->sp--);
    cpu_read(spc, 0x100 | spc->sp--);
    cpu_read(spc, 0x100 | spc->sp--);
    cpu_idle(spc);
    spc->i = false;
    spc->pc = cpu_readWord(spc, 0xfffe, 0xffff);
    return;
  }
  if(spc->stopped) {
    cpu_idleWait(spc);
    return;
  }
  uint8_t opcode = cpu_readOpcode(spc);
  cpu_doOpcode(spc, opcode);
}

static uint8_t cpu_read(spc_cpu_t* spc, uint16_t adr) {
  return smp_cpuRead((spc_t*) spc->mem, adr);
}

static void cpu_write(spc_cpu_t* spc, uint16_t adr, uint8_t val) {
  smp_cpuWrite((spc_t*) spc->mem, adr, val);
}

static void cpu_idle(spc_cpu_t* spc) {
  smp_cpuIdle((spc_t*) spc->mem);
}

static void cpu_idleWait(spc_cpu_t* spc) {
  smp_cpuIdle((spc_t*) spc->mem);
}

static uint8_t cpu_readOpcode(spc_cpu_t* spc) {
  return cpu_read(spc, spc->pc++);
}

static uint16_t cpu_readOpcodeWord(spc_cpu_t* spc) {
  uint8_t low = cpu_readOpcode(spc);
  return low | (cpu_readOpcode(spc) << 8);
}

static uint8_t cpu_getFlags(spc_cpu_t* spc) {
  uint8_t val = spc->n << 7;
  val |= spc->v << 6;
  val |= spc->p << 5;
  val |= spc->b << 4;
  val |= spc->h << 3;
  val |= spc->i << 2;
  val |= spc->z << 1;
  val |= spc->c;
  return val;
}

static void cpu_setFlags(spc_cpu_t* spc, uint8_t val) {
  spc->n = val & 0x80;
  spc->v = val & 0x40;
  spc->p = val & 0x20;
  spc->b = val & 0x10;
  spc->h = val & 8;
  spc->i = val & 4;
  spc->z = val & 2;
  spc->c = val & 1;
}

static void cpu_setZN(spc_cpu_t* spc, uint8_t value) {
  spc->z = value == 0;
  spc->n = value & 0x80;
}

static void cpu_doBranch(spc_cpu_t* spc, uint8_t value, bool check) {
  if(check) {
    // taken branch: 2 extra cycles
    cpu_idle(spc);
    cpu_idle(spc);
    spc->pc += (int8_t) value;
  }
}

static uint8_t cpu_pullByte(spc_cpu_t* spc) {
  spc->sp++;
  return cpu_read(spc, 0x100 | spc->sp);
}

static void cpu_pushByte(spc_cpu_t* spc, uint8_t value) {
  cpu_write(spc, 0x100 | spc->sp, value);
  spc->sp--;
}

static uint16_t cpu_pullWord(spc_cpu_t* spc) {
  uint8_t value = cpu_pullByte(spc);
  return value | (cpu_pullByte(spc) << 8);
}

static void cpu_pushWord(spc_cpu_t* spc, uint16_t value) {
  cpu_pushByte(spc, value >> 8);
  cpu_pushByte(spc, value & 0xff);
}

static uint16_t cpu_readWord(spc_cpu_t* spc, uint16_t adrl, uint16_t adrh) {
  uint8_t value = cpu_read(spc, adrl);
  return value | (cpu_read(spc, adrh) << 8);
}

static void cpu_writeWord(spc_cpu_t* spc, uint16_t adrl, uint16_t adrh, uint16_t value) {
  cpu_write(spc, adrl, value & 0xff);
  cpu_write(spc, adrh, value >> 8);
}

// adressing modes

static uint16_t cpu_adrDp(spc_cpu_t* spc) {
  return cpu_readOpcode(spc) | (spc->p << 8);
}

static uint16_t cpu_adrAbs(spc_cpu_t* spc) {
  return cpu_readOpcodeWord(spc);
}

static uint16_t cpu_adrInd(spc_cpu_t* spc) {
  cpu_read(spc, spc->pc);
  return spc->x | (spc->p << 8);
}

static uint16_t cpu_adrIdx(spc_cpu_t* spc) {
  uint8_t pointer = cpu_readOpcode(spc);
  cpu_idle(spc);
  return cpu_readWord(spc, ((pointer + spc->x) & 0xff) | (spc->p << 8), ((pointer + spc->x + 1) & 0xff) | (spc->p << 8));
}

static uint16_t cpu_adrImm(spc_cpu_t* spc) {
  return spc->pc++;
}

static uint16_t cpu_adrDpx(spc_cpu_t* spc) {
  uint16_t res = ((cpu_readOpcode(spc) + spc->x) & 0xff) | (spc->p << 8);
  cpu_idle(spc);
  return res;
}

static uint16_t cpu_adrDpy(spc_cpu_t* spc) {
  uint16_t res = ((cpu_readOpcode(spc) + spc->y) & 0xff) | (spc->p << 8);
  cpu_idle(spc);
  return res;
}

static uint16_t cpu_adrAbx(spc_cpu_t* spc) {
  uint16_t res = (cpu_readOpcodeWord(spc) + spc->x) & 0xffff;
  cpu_idle(spc);
  return res;
}

static uint16_t cpu_adrAby(spc_cpu_t* spc) {
  uint16_t res = (cpu_readOpcodeWord(spc) + spc->y) & 0xffff;
  cpu_idle(spc);
  return res;
}

static uint16_t cpu_adrIdy(spc_cpu_t* spc) {
  uint8_t pointer = cpu_readOpcode(spc);
  uint16_t adr = cpu_readWord(spc, pointer | (spc->p << 8), ((pointer + 1) & 0xff) | (spc->p << 8));
  cpu_idle(spc);
  return (adr + spc->y) & 0xffff;
}

static uint16_t cpu_adrDpDp(spc_cpu_t* spc, uint8_t* srcVal) {
  *srcVal = cpu_read(spc, cpu_readOpcode(spc) | (spc->p << 8));
  return cpu_readOpcode(spc) | (spc->p << 8);
}

static uint16_t cpu_adrDpImm(spc_cpu_t* spc, uint8_t* srcVal) {
  *srcVal = cpu_readOpcode(spc);
  return cpu_readOpcode(spc) | (spc->p << 8);
}

static uint16_t cpu_adrIndInd(spc_cpu_t* spc, uint8_t* srcVal) {
  cpu_read(spc, spc->pc);
  *srcVal = cpu_read(spc, spc->y | (spc->p << 8));
  return spc->x | (spc->p << 8);
}

static uint8_t cpu_adrAbsBit(spc_cpu_t* spc, uint16_t* adr) {
  uint16_t adrBit = cpu_readOpcodeWord(spc);
  *adr = adrBit & 0x1fff;
  return adrBit >> 13;
}

static uint16_t cpu_adrDpWord(spc_cpu_t* spc, uint16_t* low) {
  uint8_t adr = cpu_readOpcode(spc);
  *low = adr | (spc->p << 8);
  return ((adr + 1) & 0xff) | (spc->p << 8);
}

static uint16_t cpu_adrIndP(spc_cpu_t* spc) {
  cpu_read(spc, spc->pc);
  return spc->x++ | (spc->p << 8);
}

// opcode functions

static void cpu_and(spc_cpu_t* spc, uint16_t adr) {
  spc->a &= cpu_read(spc, adr);
  cpu_setZN(spc, spc->a);
}

static void cpu_andm(spc_cpu_t* spc, uint16_t dst, uint8_t value) {
  uint8_t result = cpu_read(spc, dst) & value;
  cpu_write(spc, dst, result);
  cpu_setZN(spc, result);
}

static void cpu_or(spc_cpu_t* spc, uint16_t adr) {
  spc->a |= cpu_read(spc, adr);
  cpu_setZN(spc, spc->a);
}

static void cpu_orm(spc_cpu_t* spc, uint16_t dst, uint8_t value) {
  uint8_t result = cpu_read(spc, dst) | value;
  cpu_write(spc, dst, result);
  cpu_setZN(spc, result);
}

static void cpu_eor(spc_cpu_t* spc, uint16_t adr) {
  spc->a ^= cpu_read(spc, adr);
  cpu_setZN(spc, spc->a);
}

static void cpu_eorm(spc_cpu_t* spc, uint16_t dst, uint8_t value) {
  uint8_t result = cpu_read(spc, dst) ^ value;
  cpu_write(spc, dst, result);
  cpu_setZN(spc, result);
}

static void cpu_adc(spc_cpu_t* spc, uint16_t adr) {
  uint8_t value = cpu_read(spc, adr);
  int result = spc->a + value + spc->c;
  spc->v = (spc->a & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
  spc->h = ((spc->a & 0xf) + (value & 0xf) + spc->c) > 0xf;
  spc->c = result > 0xff;
  spc->a = result;
  cpu_setZN(spc, spc->a);
}

static void cpu_adcm(spc_cpu_t* spc, uint16_t dst, uint8_t value) {
  uint8_t applyOn = cpu_read(spc, dst);
  int result = applyOn + value + spc->c;
  spc->v = (applyOn & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
  spc->h = ((applyOn & 0xf) + (value & 0xf) + spc->c) > 0xf;
  spc->c = result > 0xff;
  cpu_write(spc, dst, result);
  cpu_setZN(spc, result);
}

static void cpu_sbc(spc_cpu_t* spc, uint16_t adr) {
  uint8_t value = cpu_read(spc, adr) ^ 0xff;
  int result = spc->a + value + spc->c;
  spc->v = (spc->a & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
  spc->h = ((spc->a & 0xf) + (value & 0xf) + spc->c) > 0xf;
  spc->c = result > 0xff;
  spc->a = result;
  cpu_setZN(spc, spc->a);
}

static void cpu_sbcm(spc_cpu_t* spc, uint16_t dst, uint8_t value) {
  value ^= 0xff;
  uint8_t applyOn = cpu_read(spc, dst);
  int result = applyOn + value + spc->c;
  spc->v = (applyOn & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
  spc->h = ((applyOn & 0xf) + (value & 0xf) + spc->c) > 0xf;
  spc->c = result > 0xff;
  cpu_write(spc, dst, result);
  cpu_setZN(spc, result);
}

static void cpu_cmp(spc_cpu_t* spc, uint16_t adr) {
  uint8_t value = cpu_read(spc, adr) ^ 0xff;
  int result = spc->a + value + 1;
  spc->c = result > 0xff;
  cpu_setZN(spc, result);
}

static void cpu_cmpx(spc_cpu_t* spc, uint16_t adr) {
  uint8_t value = cpu_read(spc, adr) ^ 0xff;
  int result = spc->x + value + 1;
  spc->c = result > 0xff;
  cpu_setZN(spc, result);
}

static void cpu_cmpy(spc_cpu_t* spc, uint16_t adr) {
  uint8_t value = cpu_read(spc, adr) ^ 0xff;
  int result = spc->y + value + 1;
  spc->c = result > 0xff;
  cpu_setZN(spc, result);
}

static void cpu_cmpm(spc_cpu_t* spc, uint16_t dst, uint8_t value) {
  value ^= 0xff;
  int result = cpu_read(spc, dst) + value + 1;
  spc->c = result > 0xff;
  cpu_idle(spc);
  cpu_setZN(spc, result);
}

static void cpu_mov(spc_cpu_t* spc, uint16_t adr) {
  spc->a = cpu_read(spc, adr);
  cpu_setZN(spc, spc->a);
}

static void cpu_movx(spc_cpu_t* spc, uint16_t adr) {
  spc->x = cpu_read(spc, adr);
  cpu_setZN(spc, spc->x);
}

static void cpu_movy(spc_cpu_t* spc, uint16_t adr) {
  spc->y = cpu_read(spc, adr);
  cpu_setZN(spc, spc->y);
}

static void cpu_movs(spc_cpu_t* spc, uint16_t adr) {
  cpu_read(spc, adr);
  cpu_write(spc, adr, spc->a);
}

static void cpu_movsx(spc_cpu_t* spc, uint16_t adr) {
  cpu_read(spc, adr);
  cpu_write(spc, adr, spc->x);
}

static void cpu_movsy(spc_cpu_t* spc, uint16_t adr) {
  cpu_read(spc, adr);
  cpu_write(spc, adr, spc->y);
}

static void cpu_asl(spc_cpu_t* spc, uint16_t adr) {
  uint8_t val = cpu_read(spc, adr);
  spc->c = val & 0x80;
  val <<= 1;
  cpu_write(spc, adr, val);
  cpu_setZN(spc, val);
}

static void cpu_lsr(spc_cpu_t* spc, uint16_t adr) {
  uint8_t val = cpu_read(spc, adr);
  spc->c = val & 1;
  val >>= 1;
  cpu_write(spc, adr, val);
  cpu_setZN(spc, val);
}

static void cpu_rol(spc_cpu_t* spc, uint16_t adr) {
  uint8_t val = cpu_read(spc, adr);
  bool newC = val & 0x80;
  val = (val << 1) | spc->c;
  spc->c = newC;
  cpu_write(spc, adr, val);
  cpu_setZN(spc, val);
}

static void cpu_ror(spc_cpu_t* spc, uint16_t adr) {
  uint8_t val = cpu_read(spc, adr);
  bool newC = val & 1;
  val = (val >> 1) | (spc->c << 7);
  spc->c = newC;
  cpu_write(spc, adr, val);
  cpu_setZN(spc, val);
}

static void cpu_inc(spc_cpu_t* spc, uint16_t adr) {
  uint8_t val = cpu_read(spc, adr) + 1;
  cpu_write(spc, adr, val);
  cpu_setZN(spc, val);
}

static void cpu_dec(spc_cpu_t* spc, uint16_t adr) {
  uint8_t val = cpu_read(spc, adr) - 1;
  cpu_write(spc, adr, val);
  cpu_setZN(spc, val);
}

static void cpu_doOpcode(spc_cpu_t* spc, uint8_t opcode) {
  switch(opcode) {
    case 0x00: { // nop imp
      cpu_read(spc, spc->pc);
      // no operation
      break;
    }
    case 0x01:
    case 0x11:
    case 0x21:
    case 0x31:
    case 0x41:
    case 0x51:
    case 0x61:
    case 0x71:
    case 0x81:
    case 0x91:
    case 0xa1:
    case 0xb1:
    case 0xc1:
    case 0xd1:
    case 0xe1:
    case 0xf1: { // tcall imp
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      cpu_pushWord(spc, spc->pc);
      cpu_idle(spc);
      uint16_t adr = 0xffde - (2 * (opcode >> 4));
      spc->pc = cpu_readWord(spc, adr, adr + 1);
      break;
    }
    case 0x02:
    case 0x22:
    case 0x42:
    case 0x62:
    case 0x82:
    case 0xa2:
    case 0xc2:
    case 0xe2: { // set1 dp
      uint16_t adr = cpu_adrDp(spc);
      cpu_write(spc, adr, cpu_read(spc, adr) | (1 << (opcode >> 5)));
      break;
    }
    case 0x12:
    case 0x32:
    case 0x52:
    case 0x72:
    case 0x92:
    case 0xb2:
    case 0xd2:
    case 0xf2: { // clr1 dp
      uint16_t adr = cpu_adrDp(spc);
      cpu_write(spc, adr, cpu_read(spc, adr) & ~(1 << (opcode >> 5)));
      break;
    }
    case 0x03:
    case 0x23:
    case 0x43:
    case 0x63:
    case 0x83:
    case 0xa3:
    case 0xc3:
    case 0xe3: { // bbs dp, rel
      uint8_t val = cpu_read(spc, cpu_adrDp(spc));
      cpu_idle(spc);
      cpu_doBranch(spc, cpu_readOpcode(spc), val & (1 << (opcode >> 5)));
      break;
    }
    case 0x13:
    case 0x33:
    case 0x53:
    case 0x73:
    case 0x93:
    case 0xb3:
    case 0xd3:
    case 0xf3: { // bbc dp, rel
      uint8_t val = cpu_read(spc, cpu_adrDp(spc));
      cpu_idle(spc);
      cpu_doBranch(spc, cpu_readOpcode(spc), (val & (1 << (opcode >> 5))) == 0);
      break;
    }
    case 0x04: { // or  dp
      cpu_or(spc, cpu_adrDp(spc));
      break;
    }
    case 0x05: { // or  abs
      cpu_or(spc, cpu_adrAbs(spc));
      break;
    }
    case 0x06: { // or  ind
      cpu_or(spc, cpu_adrInd(spc));
      break;
    }
    case 0x07: { // or  idx
      cpu_or(spc, cpu_adrIdx(spc));
      break;
    }
    case 0x08: { // or  imm
      cpu_or(spc, cpu_adrImm(spc));
      break;
    }
    case 0x09: { // orm dp, dp
      uint8_t src = 0;
      uint16_t dst = cpu_adrDpDp(spc, &src);
      cpu_orm(spc, dst, src);
      break;
    }
    case 0x0a: { // or1 abs.bit
      uint16_t adr = 0;
      uint8_t bit = cpu_adrAbsBit(spc, &adr);
      spc->c = spc->c | ((cpu_read(spc, adr) >> bit) & 1);
      cpu_idle(spc);
      break;
    }
    case 0x0b: { // asl dp
      cpu_asl(spc, cpu_adrDp(spc));
      break;
    }
    case 0x0c: { // asl abs
      cpu_asl(spc, cpu_adrAbs(spc));
      break;
    }
    case 0x0d: { // pushp imp
      cpu_read(spc, spc->pc);
      cpu_pushByte(spc, cpu_getFlags(spc));
      cpu_idle(spc);
      break;
    }
    case 0x0e: { // tset1 abs
      uint16_t adr = cpu_adrAbs(spc);
      uint8_t val = cpu_read(spc, adr);
      cpu_read(spc, adr);
      uint8_t result = spc->a + (val ^ 0xff) + 1;
      cpu_setZN(spc, result);
      cpu_write(spc, adr, val | spc->a);
      break;
    }
    case 0x0f: { // brk imp
      cpu_read(spc, spc->pc);
      cpu_pushWord(spc, spc->pc);
      cpu_pushByte(spc, cpu_getFlags(spc));
      cpu_idle(spc);
      spc->i = false;
      spc->b = true;
      spc->pc = cpu_readWord(spc, 0xffde, 0xffdf);
      break;
    }
    case 0x10: { // bpl rel
      cpu_doBranch(spc, cpu_readOpcode(spc), !spc->n);
      break;
    }
    case 0x14: { // or  dpx
      cpu_or(spc, cpu_adrDpx(spc));
      break;
    }
    case 0x15: { // or  abx
      cpu_or(spc, cpu_adrAbx(spc));
      break;
    }
    case 0x16: { // or  aby
      cpu_or(spc, cpu_adrAby(spc));
      break;
    }
    case 0x17: { // or  idy
      cpu_or(spc, cpu_adrIdy(spc));
      break;
    }
    case 0x18: { // orm dp, imm
      uint8_t src = 0;
      uint16_t dst = cpu_adrDpImm(spc, &src);
      cpu_orm(spc, dst, src);
      break;
    }
    case 0x19: { // orm ind, ind
      uint8_t src = 0;
      uint16_t dst = cpu_adrIndInd(spc, &src);
      cpu_orm(spc, dst, src);
      break;
    }
    case 0x1a: { // decw dp
      uint16_t low = 0;
      uint16_t high = cpu_adrDpWord(spc, &low);
      uint16_t value = cpu_read(spc, low) - 1;
      cpu_write(spc, low, value & 0xff);
      value += cpu_read(spc, high) << 8;
      cpu_write(spc, high, value >> 8);
      spc->z = value == 0;
      spc->n = value & 0x8000;
      break;
    }
    case 0x1b: { // asl dpx
      cpu_asl(spc, cpu_adrDpx(spc));
      break;
    }
    case 0x1c: { // asla imp
      cpu_read(spc, spc->pc);
      spc->c = spc->a & 0x80;
      spc->a <<= 1;
      cpu_setZN(spc, spc->a);
      break;
    }
    case 0x1d: { // decx imp
      cpu_read(spc, spc->pc);
      spc->x--;
      cpu_setZN(spc, spc->x);
      break;
    }
    case 0x1e: { // cmpx abs
      cpu_cmpx(spc, cpu_adrAbs(spc));
      break;
    }
    case 0x1f: { // jmp iax
      uint16_t pointer = cpu_readOpcodeWord(spc);
      cpu_idle(spc);
      spc->pc = cpu_readWord(spc, (pointer + spc->x) & 0xffff, (pointer + spc->x + 1) & 0xffff);
      break;
    }
    case 0x20: { // clrp imp
      cpu_read(spc, spc->pc);
      spc->p = false;
      break;
    }
    case 0x24: { // and dp
      cpu_and(spc, cpu_adrDp(spc));
      break;
    }
    case 0x25: { // and abs
      cpu_and(spc, cpu_adrAbs(spc));
      break;
    }
    case 0x26: { // and ind
      cpu_and(spc, cpu_adrInd(spc));
      break;
    }
    case 0x27: { // and idx
      cpu_and(spc, cpu_adrIdx(spc));
      break;
    }
    case 0x28: { // and imm
      cpu_and(spc, cpu_adrImm(spc));
      break;
    }
    case 0x29: { // andm dp, dp
      uint8_t src = 0;
      uint16_t dst = cpu_adrDpDp(spc, &src);
      cpu_andm(spc, dst, src);
      break;
    }
    case 0x2a: { // or1n abs.bit
      uint16_t adr = 0;
      uint8_t bit = cpu_adrAbsBit(spc, &adr);
      spc->c = spc->c | (~(cpu_read(spc, adr) >> bit) & 1);
      cpu_idle(spc);
      break;
    }
    case 0x2b: { // rol dp
      cpu_rol(spc, cpu_adrDp(spc));
      break;
    }
    case 0x2c: { // rol abs
      cpu_rol(spc, cpu_adrAbs(spc));
      break;
    }
    case 0x2d: { // pusha imp
      cpu_read(spc, spc->pc);
      cpu_pushByte(spc, spc->a);
      cpu_idle(spc);
      break;
    }
    case 0x2e: { // cbne dp, rel
      uint8_t val = cpu_read(spc, cpu_adrDp(spc)) ^ 0xff;
      cpu_idle(spc);
      uint8_t result = spc->a + val + 1;
      cpu_doBranch(spc, cpu_readOpcode(spc), result != 0);
      break;
    }
    case 0x2f: { // bra rel
      cpu_doBranch(spc, cpu_readOpcode(spc), true);
      break;
    }
    case 0x30: { // bmi rel
      cpu_doBranch(spc, cpu_readOpcode(spc), spc->n);
      break;
    }
    case 0x34: { // and dpx
      cpu_and(spc, cpu_adrDpx(spc));
      break;
    }
    case 0x35: { // and abx
      cpu_and(spc, cpu_adrAbx(spc));
      break;
    }
    case 0x36: { // and aby
      cpu_and(spc, cpu_adrAby(spc));
      break;
    }
    case 0x37: { // and idy
      cpu_and(spc, cpu_adrIdy(spc));
      break;
    }
    case 0x38: { // andm dp, imm
      uint8_t src = 0;
      uint16_t dst = cpu_adrDpImm(spc, &src);
      cpu_andm(spc, dst, src);
      break;
    }
    case 0x39: { // andm ind, ind
      uint8_t src = 0;
      uint16_t dst = cpu_adrIndInd(spc, &src);
      cpu_andm(spc, dst, src);
      break;
    }
    case 0x3a: { // incw dp
      uint16_t low = 0;
      uint16_t high = cpu_adrDpWord(spc, &low);
      uint16_t value = cpu_read(spc, low) + 1;
      cpu_write(spc, low, value & 0xff);
      value += cpu_read(spc, high) << 8;
      cpu_write(spc, high, value >> 8);
      spc->z = value == 0;
      spc->n = value & 0x8000;
      break;
    }
    case 0x3b: { // rol dpx
      cpu_rol(spc, cpu_adrDpx(spc));
      break;
    }
    case 0x3c: { // rola imp
      cpu_read(spc, spc->pc);
      bool newC = spc->a & 0x80;
      spc->a = (spc->a << 1) | spc->c;
      spc->c = newC;
      cpu_setZN(spc, spc->a);
      break;
    }
    case 0x3d: { // incx imp
      cpu_read(spc, spc->pc);
      spc->x++;
      cpu_setZN(spc, spc->x);
      break;
    }
    case 0x3e: { // cmpx dp
      cpu_cmpx(spc, cpu_adrDp(spc));
      break;
    }
    case 0x3f: { // call abs
      uint16_t dst = cpu_readOpcodeWord(spc);
      cpu_idle(spc);
      cpu_pushWord(spc, spc->pc);
      cpu_idle(spc);
      cpu_idle(spc);
      spc->pc = dst;
      break;
    }
    case 0x40: { // setp imp
      cpu_read(spc, spc->pc);
      spc->p = true;
      break;
    }
    case 0x44: { // eor dp
      cpu_eor(spc, cpu_adrDp(spc));
      break;
    }
    case 0x45: { // eor abs
      cpu_eor(spc, cpu_adrAbs(spc));
      break;
    }
    case 0x46: { // eor ind
      cpu_eor(spc, cpu_adrInd(spc));
      break;
    }
    case 0x47: { // eor idx
      cpu_eor(spc, cpu_adrIdx(spc));
      break;
    }
    case 0x48: { // eor imm
      cpu_eor(spc, cpu_adrImm(spc));
      break;
    }
    case 0x49: { // eorm dp, dp
      uint8_t src = 0;
      uint16_t dst = cpu_adrDpDp(spc, &src);
      cpu_eorm(spc, dst, src);
      break;
    }
    case 0x4a: { // and1 abs.bit
      uint16_t adr = 0;
      uint8_t bit = cpu_adrAbsBit(spc, &adr);
      spc->c = spc->c & ((cpu_read(spc, adr) >> bit) & 1);
      break;
    }
    case 0x4b: { // lsr dp
      cpu_lsr(spc, cpu_adrDp(spc));
      break;
    }
    case 0x4c: { // lsr abs
      cpu_lsr(spc, cpu_adrAbs(spc));
      break;
    }
    case 0x4d: { // pushx imp
      cpu_read(spc, spc->pc);
      cpu_pushByte(spc, spc->x);
      cpu_idle(spc);
      break;
    }
    case 0x4e: { // tclr1 abs
      uint16_t adr = cpu_adrAbs(spc);
      uint8_t val = cpu_read(spc, adr);
      cpu_read(spc, adr);
      uint8_t result = spc->a + (val ^ 0xff) + 1;
      cpu_setZN(spc, result);
      cpu_write(spc, adr, val & ~spc->a);
      break;
    }
    case 0x4f: { // pcall dp
      uint8_t dst = cpu_readOpcode(spc);
      cpu_idle(spc);
      cpu_pushWord(spc, spc->pc);
      cpu_idle(spc);
      spc->pc = 0xff00 | dst;
      break;
    }
    case 0x50: { // bvc rel
      cpu_doBranch(spc, cpu_readOpcode(spc), !spc->v);
      break;
    }
    case 0x54: { // eor dpx
      cpu_eor(spc, cpu_adrDpx(spc));
      break;
    }
    case 0x55: { // eor abx
      cpu_eor(spc, cpu_adrAbx(spc));
      break;
    }
    case 0x56: { // eor aby
      cpu_eor(spc, cpu_adrAby(spc));
      break;
    }
    case 0x57: { // eor idy
      cpu_eor(spc, cpu_adrIdy(spc));
      break;
    }
    case 0x58: { // eorm dp, imm
      uint8_t src = 0;
      uint16_t dst = cpu_adrDpImm(spc, &src);
      cpu_eorm(spc, dst, src);
      break;
    }
    case 0x59: { // eorm ind, ind
      uint8_t src = 0;
      uint16_t dst = cpu_adrIndInd(spc, &src);
      cpu_eorm(spc, dst, src);
      break;
    }
    case 0x5a: { // cmpw dp
      uint16_t low = 0;
      uint16_t high = cpu_adrDpWord(spc, &low);
      uint16_t value = cpu_readWord(spc, low, high) ^ 0xffff;
      uint16_t ya = spc->a | (spc->y << 8);
      int result = ya + value + 1;
      spc->c = result > 0xffff;
      spc->z = (result & 0xffff) == 0;
      spc->n = result & 0x8000;
      break;
    }
    case 0x5b: { // lsr dpx
      cpu_lsr(spc, cpu_adrDpx(spc));
      break;
    }
    case 0x5c: { // lsra imp
      cpu_read(spc, spc->pc);
      spc->c = spc->a & 1;
      spc->a >>= 1;
      cpu_setZN(spc, spc->a);
      break;
    }
    case 0x5d: { // movxa imp
      cpu_read(spc, spc->pc);
      spc->x = spc->a;
      cpu_setZN(spc, spc->x);
      break;
    }
    case 0x5e: { // cmpy abs
      cpu_cmpy(spc, cpu_adrAbs(spc));
      break;
    }
    case 0x5f: { // jmp abs
      spc->pc = cpu_readOpcodeWord(spc);
      break;
    }
    case 0x60: { // clrc imp
      cpu_read(spc, spc->pc);
      spc->c = false;
      break;
    }
    case 0x64: { // cmp dp
      cpu_cmp(spc, cpu_adrDp(spc));
      break;
    }
    case 0x65: { // cmp abs
      cpu_cmp(spc, cpu_adrAbs(spc));
      break;
    }
    case 0x66: { // cmp ind
      cpu_cmp(spc, cpu_adrInd(spc));
      break;
    }
    case 0x67: { // cmp idx
      cpu_cmp(spc, cpu_adrIdx(spc));
      break;
    }
    case 0x68: { // cmp imm
      cpu_cmp(spc, cpu_adrImm(spc));
      break;
    }
    case 0x69: { // cmpm dp, dp
      uint8_t src = 0;
      uint16_t dst = cpu_adrDpDp(spc, &src);
      cpu_cmpm(spc, dst, src);
      break;
    }
    case 0x6a: { // and1n abs.bit
      uint16_t adr = 0;
      uint8_t bit = cpu_adrAbsBit(spc, &adr);
      spc->c = spc->c & (~(cpu_read(spc, adr) >> bit) & 1);
      break;
    }
    case 0x6b: { // ror dp
      cpu_ror(spc, cpu_adrDp(spc));
      break;
    }
    case 0x6c: { // ror abs
      cpu_ror(spc, cpu_adrAbs(spc));
      break;
    }
    case 0x6d: { // pushy imp
      cpu_read(spc, spc->pc);
      cpu_pushByte(spc, spc->y);
      cpu_idle(spc);
      break;
    }
    case 0x6e: { // dbnz dp, rel
      uint16_t adr = cpu_adrDp(spc);
      uint8_t result = cpu_read(spc, adr) - 1;
      cpu_write(spc, adr, result);
      cpu_doBranch(spc, cpu_readOpcode(spc), result != 0);
      break;
    }
    case 0x6f: { // ret imp
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      spc->pc = cpu_pullWord(spc);
      break;
    }
    case 0x70: { // bvs rel
      cpu_doBranch(spc, cpu_readOpcode(spc), spc->v);
      break;
    }
    case 0x74: { // cmp dpx
      cpu_cmp(spc, cpu_adrDpx(spc));
      break;
    }
    case 0x75: { // cmp abx
      cpu_cmp(spc, cpu_adrAbx(spc));
      break;
    }
    case 0x76: { // cmp aby
      cpu_cmp(spc, cpu_adrAby(spc));
      break;
    }
    case 0x77: { // cmp idy
      cpu_cmp(spc, cpu_adrIdy(spc));
      break;
    }
    case 0x78: { // cmpm dp, imm
      uint8_t src = 0;
      uint16_t dst = cpu_adrDpImm(spc, &src);
      cpu_cmpm(spc, dst, src);
      break;
    }
    case 0x79: { // cmpm ind, ind
      uint8_t src = 0;
      uint16_t dst = cpu_adrIndInd(spc, &src);
      cpu_cmpm(spc, dst, src);
      break;
    }
    case 0x7a: { // addw dp
      uint16_t low = 0;
      uint16_t high = cpu_adrDpWord(spc, &low);
      uint8_t vall = cpu_read(spc, low);
      cpu_idle(spc);
      uint16_t value = vall | (cpu_read(spc, high) << 8);
      uint16_t ya = spc->a | (spc->y << 8);
      int result = ya + value;
      spc->v = (ya & 0x8000) == (value & 0x8000) && (value & 0x8000) != (result & 0x8000);
      spc->h = ((ya & 0xfff) + (value & 0xfff)) > 0xfff;
      spc->c = result > 0xffff;
      spc->z = (result & 0xffff) == 0;
      spc->n = result & 0x8000;
      spc->a = result & 0xff;
      spc->y = result >> 8;
      break;
    }
    case 0x7b: { // ror dpx
      cpu_ror(spc, cpu_adrDpx(spc));
      break;
    }
    case 0x7c: { // rora imp
      cpu_read(spc, spc->pc);
      bool newC = spc->a & 1;
      spc->a = (spc->a >> 1) | (spc->c << 7);
      spc->c = newC;
      cpu_setZN(spc, spc->a);
      break;
    }
    case 0x7d: { // movax imp
      cpu_read(spc, spc->pc);
      spc->a = spc->x;
      cpu_setZN(spc, spc->a);
      break;
    }
    case 0x7e: { // cmpy dp
      cpu_cmpy(spc, cpu_adrDp(spc));
      break;
    }
    case 0x7f: { // reti imp
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      cpu_setFlags(spc, cpu_pullByte(spc));
      spc->pc = cpu_pullWord(spc);
      break;
    }
    case 0x80: { // setc imp
      cpu_read(spc, spc->pc);
      spc->c = true;
      break;
    }
    case 0x84: { // adc dp
      cpu_adc(spc, cpu_adrDp(spc));
      break;
    }
    case 0x85: { // adc abs
      cpu_adc(spc, cpu_adrAbs(spc));
      break;
    }
    case 0x86: { // adc ind
      cpu_adc(spc, cpu_adrInd(spc));
      break;
    }
    case 0x87: { // adc idx
      cpu_adc(spc, cpu_adrIdx(spc));
      break;
    }
    case 0x88: { // adc imm
      cpu_adc(spc, cpu_adrImm(spc));
      break;
    }
    case 0x89: { // adcm dp, dp
      uint8_t src = 0;
      uint16_t dst = cpu_adrDpDp(spc, &src);
      cpu_adcm(spc, dst, src);
      break;
    }
    case 0x8a: { // eor1 abs.bit
      uint16_t adr = 0;
      uint8_t bit = cpu_adrAbsBit(spc, &adr);
      spc->c = spc->c ^ ((cpu_read(spc, adr) >> bit) & 1);
      cpu_idle(spc);
      break;
    }
    case 0x8b: { // dec dp
      cpu_dec(spc, cpu_adrDp(spc));
      break;
    }
    case 0x8c: { // dec abs
      cpu_dec(spc, cpu_adrAbs(spc));
      break;
    }
    case 0x8d: { // movy imm
      cpu_movy(spc, cpu_adrImm(spc));
      break;
    }
    case 0x8e: { // popp imp
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      cpu_setFlags(spc, cpu_pullByte(spc));
      break;
    }
    case 0x8f: { // movm dp, imm
      uint8_t val = 0;
      uint16_t dst = cpu_adrDpImm(spc, &val);
      cpu_read(spc, dst);
      cpu_write(spc, dst, val);
      break;
    }
    case 0x90: { // bcc rel
      cpu_doBranch(spc, cpu_readOpcode(spc), !spc->c);
      break;
    }
    case 0x94: { // adc dpx
      cpu_adc(spc, cpu_adrDpx(spc));
      break;
    }
    case 0x95: { // adc abx
      cpu_adc(spc, cpu_adrAbx(spc));
      break;
    }
    case 0x96: { // adc aby
      cpu_adc(spc, cpu_adrAby(spc));
      break;
    }
    case 0x97: { // adc idy
      cpu_adc(spc, cpu_adrIdy(spc));
      break;
    }
    case 0x98: { // adcm dp, imm
      uint8_t src = 0;
      uint16_t dst = cpu_adrDpImm(spc, &src);
      cpu_adcm(spc, dst, src);
      break;
    }
    case 0x99: { // adcm ind, ind
      uint8_t src = 0;
      uint16_t dst = cpu_adrIndInd(spc, &src);
      cpu_adcm(spc, dst, src);
      break;
    }
    case 0x9a: { // subw dp
      uint16_t low = 0;
      uint16_t high = cpu_adrDpWord(spc, &low);
      uint8_t vall = cpu_read(spc, low);
      cpu_idle(spc);
      uint16_t value = (vall | (cpu_read(spc, high) << 8)) ^ 0xffff;
      uint16_t ya = spc->a | (spc->y << 8);
      int result = ya + value + 1;
      spc->v = (ya & 0x8000) == (value & 0x8000) && (value & 0x8000) != (result & 0x8000);
      spc->h = ((ya & 0xfff) + (value & 0xfff) + 1) > 0xfff;
      spc->c = result > 0xffff;
      spc->z = (result & 0xffff) == 0;
      spc->n = result & 0x8000;
      spc->a = result & 0xff;
      spc->y = result >> 8;
      break;
    }
    case 0x9b: { // dec dpx
      cpu_dec(spc, cpu_adrDpx(spc));
      break;
    }
    case 0x9c: { // deca imp
      cpu_read(spc, spc->pc);
      spc->a--;
      cpu_setZN(spc, spc->a);
      break;
    }
    case 0x9d: { // movxp imp
      cpu_read(spc, spc->pc);
      spc->x = spc->sp;
      cpu_setZN(spc, spc->x);
      break;
    }
    case 0x9e: { // div imp
      cpu_read(spc, spc->pc);
      for(int i = 0; i < 10; i++) cpu_idle(spc);
      spc->h = (spc->x & 0xf) <= (spc->y & 0xf);
      int yva = (spc->y << 8) | spc->a;
      int x = spc->x << 9;
      for(int i = 0; i < 9; i++) {
        yva <<= 1;
        yva |= (yva & 0x20000) ? 1 : 0;
        yva &= 0x1ffff;
        if(yva >= x) yva ^= 1;
        if(yva & 1) yva -= x;
        yva &= 0x1ffff;
      }
      spc->y = yva >> 9;
      spc->v = yva & 0x100;
      spc->a = yva & 0xff;
      cpu_setZN(spc, spc->a);
      break;
    }
    case 0x9f: { // xcn imp
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      cpu_idle(spc);
      cpu_idle(spc);
      spc->a = (spc->a >> 4) | (spc->a << 4);
      cpu_setZN(spc, spc->a);
      break;
    }
    case 0xa0: { // ei  imp
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      spc->i = true;
      break;
    }
    case 0xa4: { // sbc dp
      cpu_sbc(spc, cpu_adrDp(spc));
      break;
    }
    case 0xa5: { // sbc abs
      cpu_sbc(spc, cpu_adrAbs(spc));
      break;
    }
    case 0xa6: { // sbc ind
      cpu_sbc(spc, cpu_adrInd(spc));
      break;
    }
    case 0xa7: { // sbc idx
      cpu_sbc(spc, cpu_adrIdx(spc));
      break;
    }
    case 0xa8: { // sbc imm
      cpu_sbc(spc, cpu_adrImm(spc));
      break;
    }
    case 0xa9: { // sbcm dp, dp
      uint8_t src = 0;
      uint16_t dst = cpu_adrDpDp(spc, &src);
      cpu_sbcm(spc, dst, src);
      break;
    }
    case 0xaa: { // mov1 abs.bit
      uint16_t adr = 0;
      uint8_t bit = cpu_adrAbsBit(spc, &adr);
      spc->c = (cpu_read(spc, adr) >> bit) & 1;
      break;
    }
    case 0xab: { // inc dp
      cpu_inc(spc, cpu_adrDp(spc));
      break;
    }
    case 0xac: { // inc abs
      cpu_inc(spc, cpu_adrAbs(spc));
      break;
    }
    case 0xad: { // cmpy imm
      cpu_cmpy(spc, cpu_adrImm(spc));
      break;
    }
    case 0xae: { // popa imp
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      spc->a = cpu_pullByte(spc);
      break;
    }
    case 0xaf: { // movs ind+
      uint16_t adr = cpu_adrIndP(spc);
      cpu_idle(spc);
      cpu_write(spc, adr, spc->a);
      break;
    }
    case 0xb0: { // bcs rel
      cpu_doBranch(spc, cpu_readOpcode(spc), spc->c);
      break;
    }
    case 0xb4: { // sbc dpx
      cpu_sbc(spc, cpu_adrDpx(spc));
      break;
    }
    case 0xb5: { // sbc abx
      cpu_sbc(spc, cpu_adrAbx(spc));
      break;
    }
    case 0xb6: { // sbc aby
      cpu_sbc(spc, cpu_adrAby(spc));
      break;
    }
    case 0xb7: { // sbc idy
      cpu_sbc(spc, cpu_adrIdy(spc));
      break;
    }
    case 0xb8: { // sbcm dp, imm
      uint8_t src = 0;
      uint16_t dst = cpu_adrDpImm(spc, &src);
      cpu_sbcm(spc, dst, src);
      break;
    }
    case 0xb9: { // sbcm ind, ind
      uint8_t src = 0;
      uint16_t dst = cpu_adrIndInd(spc, &src);
      cpu_sbcm(spc, dst, src);
      break;
    }
    case 0xba: { // movw dp
      uint16_t low = 0;
      uint16_t high = cpu_adrDpWord(spc, &low);
      uint8_t vall = cpu_read(spc, low);
      cpu_idle(spc);
      uint16_t val = vall | (cpu_read(spc, high) << 8);
      spc->a = val & 0xff;
      spc->y = val >> 8;
      spc->z = val == 0;
      spc->n = val & 0x8000;
      break;
    }
    case 0xbb: { // inc dpx
      cpu_inc(spc, cpu_adrDpx(spc));
      break;
    }
    case 0xbc: { // inca imp
      cpu_read(spc, spc->pc);
      spc->a++;
      cpu_setZN(spc, spc->a);
      break;
    }
    case 0xbd: { // movpx imp
      cpu_read(spc, spc->pc);
      spc->sp = spc->x;
      break;
    }
    case 0xbe: { // das imp
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      if(spc->a > 0x99 || !spc->c) {
        spc->a -= 0x60;
        spc->c = false;
      }
      if((spc->a & 0xf) > 9 || !spc->h) {
        spc->a -= 6;
      }
      cpu_setZN(spc, spc->a);
      break;
    }
    case 0xbf: { // mov ind+
      uint16_t adr = cpu_adrIndP(spc);
      spc->a = cpu_read(spc, adr);
      cpu_idle(spc);
      cpu_setZN(spc, spc->a);
      break;
    }
    case 0xc0: { // di  imp
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      spc->i = false;
      break;
    }
    case 0xc4: { // movs dp
      cpu_movs(spc, cpu_adrDp(spc));
      break;
    }
    case 0xc5: { // movs abs
      cpu_movs(spc, cpu_adrAbs(spc));
      break;
    }
    case 0xc6: { // movs ind
      cpu_movs(spc, cpu_adrInd(spc));
      break;
    }
    case 0xc7: { // movs idx
      cpu_movs(spc, cpu_adrIdx(spc));
      break;
    }
    case 0xc8: { // cmpx imm
      cpu_cmpx(spc, cpu_adrImm(spc));
      break;
    }
    case 0xc9: { // movsx abs
      cpu_movsx(spc, cpu_adrAbs(spc));
      break;
    }
    case 0xca: { // mov1s abs.bit
      uint16_t adr = 0;
      uint8_t bit = cpu_adrAbsBit(spc, &adr);
      uint8_t result = (cpu_read(spc, adr) & (~(1 << bit))) | (spc->c << bit);
      cpu_idle(spc);
      cpu_write(spc, adr, result);
      break;
    }
    case 0xcb: { // movsy dp
      cpu_movsy(spc, cpu_adrDp(spc));
      break;
    }
    case 0xcc: { // movsy abs
      cpu_movsy(spc, cpu_adrAbs(spc));
      break;
    }
    case 0xcd: { // movx imm
      cpu_movx(spc, cpu_adrImm(spc));
      break;
    }
    case 0xce: { // popx imp
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      spc->x = cpu_pullByte(spc);
      break;
    }
    case 0xcf: { // mul imp
      cpu_read(spc, spc->pc);
      for(int i = 0; i < 7; i++) cpu_idle(spc);
      uint16_t result = spc->a * spc->y;
      spc->a = result & 0xff;
      spc->y = result >> 8;
      cpu_setZN(spc, spc->y);
      break;
    }
    case 0xd0: { // bne rel
      cpu_doBranch(spc, cpu_readOpcode(spc), !spc->z);
      break;
    }
    case 0xd4: { // movs dpx
      cpu_movs(spc, cpu_adrDpx(spc));
      break;
    }
    case 0xd5: { // movs abx
      cpu_movs(spc, cpu_adrAbx(spc));
      break;
    }
    case 0xd6: { // movs aby
      cpu_movs(spc, cpu_adrAby(spc));
      break;
    }
    case 0xd7: { // movs idy
      cpu_movs(spc, cpu_adrIdy(spc));
      break;
    }
    case 0xd8: { // movsx dp
      cpu_movsx(spc, cpu_adrDp(spc));
      break;
    }
    case 0xd9: { // movsx dpy
      cpu_movsx(spc, cpu_adrDpy(spc));
      break;
    }
    case 0xda: { // movws dp
      uint16_t low = 0;
      uint16_t high = cpu_adrDpWord(spc, &low);
      cpu_read(spc, low);
      cpu_write(spc, low, spc->a);
      cpu_write(spc, high, spc->y);
      break;
    }
    case 0xdb: { // movsy dpx
      cpu_movsy(spc, cpu_adrDpx(spc));
      break;
    }
    case 0xdc: { // decy imp
      cpu_read(spc, spc->pc);
      spc->y--;
      cpu_setZN(spc, spc->y);
      break;
    }
    case 0xdd: { // movay imp
      cpu_read(spc, spc->pc);
      spc->a = spc->y;
      cpu_setZN(spc, spc->a);
      break;
    }
    case 0xde: { // cbne dpx, rel
      uint8_t val = cpu_read(spc, cpu_adrDpx(spc)) ^ 0xff;
      cpu_idle(spc);
      uint8_t result = spc->a + val + 1;
      cpu_doBranch(spc, cpu_readOpcode(spc), result != 0);
      break;
    }
    case 0xdf: { // daa imp
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      if(spc->a > 0x99 || spc->c) {
        spc->a += 0x60;
        spc->c = true;
      }
      if((spc->a & 0xf) > 9 || spc->h) {
        spc->a += 6;
      }
      cpu_setZN(spc, spc->a);
      break;
    }
    case 0xe0: { // clrv imp
      cpu_read(spc, spc->pc);
      spc->v = false;
      spc->h = false;
      break;
    }
    case 0xe4: { // mov dp
      cpu_mov(spc, cpu_adrDp(spc));
      break;
    }
    case 0xe5: { // mov abs
      cpu_mov(spc, cpu_adrAbs(spc));
      break;
    }
    case 0xe6: { // mov ind
      cpu_mov(spc, cpu_adrInd(spc));
      break;
    }
    case 0xe7: { // mov idx
      cpu_mov(spc, cpu_adrIdx(spc));
      break;
    }
    case 0xe8: { // mov imm
      cpu_mov(spc, cpu_adrImm(spc));
      break;
    }
    case 0xe9: { // movx abs
      cpu_movx(spc, cpu_adrAbs(spc));
      break;
    }
    case 0xea: { // not1 abs.bit
      uint16_t adr = 0;
      uint8_t bit = cpu_adrAbsBit(spc, &adr);
      uint8_t result = cpu_read(spc, adr) ^ (1 << bit);
      cpu_write(spc, adr, result);
      break;
    }
    case 0xeb: { // movy dp
      cpu_movy(spc, cpu_adrDp(spc));
      break;
    }
    case 0xec: { // movy abs
      cpu_movy(spc, cpu_adrAbs(spc));
      break;
    }
    case 0xed: { // notc imp
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      spc->c = !spc->c;
      break;
    }
    case 0xee: { // popy imp
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      spc->y = cpu_pullByte(spc);
      break;
    }
    case 0xef: { // sleep imp
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      spc->stopped = true; // no interrupts, so sleeping stops as well
      break;
    }
    case 0xf0: { // beq rel
      cpu_doBranch(spc, cpu_readOpcode(spc), spc->z);
      break;
    }
    case 0xf4: { // mov dpx
      cpu_mov(spc, cpu_adrDpx(spc));
      break;
    }
    case 0xf5: { // mov abx
      cpu_mov(spc, cpu_adrAbx(spc));
      break;
    }
    case 0xf6: { // mov aby
      cpu_mov(spc, cpu_adrAby(spc));
      break;
    }
    case 0xf7: { // mov idy
      cpu_mov(spc, cpu_adrIdy(spc));
      break;
    }
    case 0xf8: { // movx dp
      cpu_movx(spc, cpu_adrDp(spc));
      break;
    }
    case 0xf9: { // movx dpy
      cpu_movx(spc, cpu_adrDpy(spc));
      break;
    }
    case 0xfa: { // movm dp, dp
      uint8_t val = 0;
      uint16_t dst = cpu_adrDpDp(spc, &val);
      cpu_write(spc, dst, val);
      break;
    }
    case 0xfb: { // movy dpx
      cpu_movy(spc, cpu_adrDpx(spc));
      break;
    }
    case 0xfc: { // incy imp
      cpu_read(spc, spc->pc);
      spc->y++;
      cpu_setZN(spc, spc->y);
      break;
    }
    case 0xfd: { // movya imp
      cpu_read(spc, spc->pc);
      spc->y = spc->a;
      cpu_setZN(spc, spc->y);
      break;
    }
    case 0xfe: { // dbnzy rel
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      spc->y--;
      cpu_doBranch(spc, cpu_readOpcode(spc), spc->y != 0);
      break;
    }
    case 0xff: { // stop imp
      cpu_read(spc, spc->pc);
      cpu_idle(spc);
      spc->stopped = true;
      break;
    }
  }
}

/* ---- the S-DSP (LakeSnes snes/dsp.c) ---- */
static const int rateValues[32] = {
  0, 2048, 1536, 1280, 1024, 768, 640, 512,
  384, 320, 256, 192, 160, 128, 96, 80,
  64, 48, 40, 32, 24, 20, 16, 12,
  10, 8, 6, 5, 4, 3, 2, 1
};

static const int rateOffsets[32] = {
  0, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
  0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0
};

static const int gaussValues[512] = {
  0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
  0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x002, 0x002, 0x002, 0x002, 0x002,
  0x002, 0x002, 0x003, 0x003, 0x003, 0x003, 0x003, 0x004, 0x004, 0x004, 0x004, 0x004, 0x005, 0x005, 0x005, 0x005,
  0x006, 0x006, 0x006, 0x006, 0x007, 0x007, 0x007, 0x008, 0x008, 0x008, 0x009, 0x009, 0x009, 0x00a, 0x00a, 0x00a,
  0x00b, 0x00b, 0x00b, 0x00c, 0x00c, 0x00d, 0x00d, 0x00e, 0x00e, 0x00f, 0x00f, 0x00f, 0x010, 0x010, 0x011, 0x011,
  0x012, 0x013, 0x013, 0x014, 0x014, 0x015, 0x015, 0x016, 0x017, 0x017, 0x018, 0x018, 0x019, 0x01a, 0x01b, 0x01b,
  0x01c, 0x01d, 0x01d, 0x01e, 0x01f, 0x020, 0x020, 0x021, 0x022, 0x023, 0x024, 0x024, 0x025, 0x026, 0x027, 0x028,
  0x029, 0x02a, 0x02b, 0x02c, 0x02d, 0x02e, 0x02f, 0x030, 0x031, 0x032, 0x033, 0x034, 0x035, 0x036, 0x037, 0x038,
  0x03a, 0x03b, 0x03c, 0x03d, 0x03e, 0x040, 0x041, 0x042, 0x043, 0x045, 0x046, 0x047, 0x049, 0x04a, 0x04c, 0x04d,
  0x04e, 0x050, 0x051, 0x053, 0x054, 0x056, 0x057, 0x059, 0x05a, 0x05c, 0x05e, 0x05f, 0x061, 0x063, 0x064, 0x066,
  0x068, 0x06a, 0x06b, 0x06d, 0x06f, 0x071, 0x073, 0x075, 0x076, 0x078, 0x07a, 0x07c, 0x07e, 0x080, 0x082, 0x084,
  0x086, 0x089, 0x08b, 0x08d, 0x08f, 0x091, 0x093, 0x096, 0x098, 0x09a, 0x09c, 0x09f, 0x0a1, 0x0a3, 0x0a6, 0x0a8,
  0x0ab, 0x0ad, 0x0af, 0x0b2, 0x0b4, 0x0b7, 0x0ba, 0x0bc, 0x0bf, 0x0c1, 0x0c4, 0x0c7, 0x0c9, 0x0cc, 0x0cf, 0x0d2,
  0x0d4, 0x0d7, 0x0da, 0x0dd, 0x0e0, 0x0e3, 0x0e6, 0x0e9, 0x0ec, 0x0ef, 0x0f2, 0x0f5, 0x0f8, 0x0fb, 0x0fe, 0x101,
  0x104, 0x107, 0x10b, 0x10e, 0x111, 0x114, 0x118, 0x11b, 0x11e, 0x122, 0x125, 0x129, 0x12c, 0x130, 0x133, 0x137,
  0x13a, 0x13e, 0x141, 0x145, 0x148, 0x14c, 0x150, 0x153, 0x157, 0x15b, 0x15f, 0x162, 0x166, 0x16a, 0x16e, 0x172,
  0x176, 0x17a, 0x17d, 0x181, 0x185, 0x189, 0x18d, 0x191, 0x195, 0x19a, 0x19e, 0x1a2, 0x1a6, 0x1aa, 0x1ae, 0x1b2,
  0x1b7, 0x1bb, 0x1bf, 0x1c3, 0x1c8, 0x1cc, 0x1d0, 0x1d5, 0x1d9, 0x1dd, 0x1e2, 0x1e6, 0x1eb, 0x1ef, 0x1f3, 0x1f8,
  0x1fc, 0x201, 0x205, 0x20a, 0x20f, 0x213, 0x218, 0x21c, 0x221, 0x226, 0x22a, 0x22f, 0x233, 0x238, 0x23d, 0x241,
  0x246, 0x24b, 0x250, 0x254, 0x259, 0x25e, 0x263, 0x267, 0x26c, 0x271, 0x276, 0x27b, 0x280, 0x284, 0x289, 0x28e,
  0x293, 0x298, 0x29d, 0x2a2, 0x2a6, 0x2ab, 0x2b0, 0x2b5, 0x2ba, 0x2bf, 0x2c4, 0x2c9, 0x2ce, 0x2d3, 0x2d8, 0x2dc,
  0x2e1, 0x2e6, 0x2eb, 0x2f0, 0x2f5, 0x2fa, 0x2ff, 0x304, 0x309, 0x30e, 0x313, 0x318, 0x31d, 0x322, 0x326, 0x32b,
  0x330, 0x335, 0x33a, 0x33f, 0x344, 0x349, 0x34e, 0x353, 0x357, 0x35c, 0x361, 0x366, 0x36b, 0x370, 0x374, 0x379,
  0x37e, 0x383, 0x388, 0x38c, 0x391, 0x396, 0x39b, 0x39f, 0x3a4, 0x3a9, 0x3ad, 0x3b2, 0x3b7, 0x3bb, 0x3c0, 0x3c5,
  0x3c9, 0x3ce, 0x3d2, 0x3d7, 0x3dc, 0x3e0, 0x3e5, 0x3e9, 0x3ed, 0x3f2, 0x3f6, 0x3fb, 0x3ff, 0x403, 0x408, 0x40c,
  0x410, 0x415, 0x419, 0x41d, 0x421, 0x425, 0x42a, 0x42e, 0x432, 0x436, 0x43a, 0x43e, 0x442, 0x446, 0x44a, 0x44e,
  0x452, 0x455, 0x459, 0x45d, 0x461, 0x465, 0x468, 0x46c, 0x470, 0x473, 0x477, 0x47a, 0x47e, 0x481, 0x485, 0x488,
  0x48c, 0x48f, 0x492, 0x496, 0x499, 0x49c, 0x49f, 0x4a2, 0x4a6, 0x4a9, 0x4ac, 0x4af, 0x4b2, 0x4b5, 0x4b7, 0x4ba,
  0x4bd, 0x4c0, 0x4c3, 0x4c5, 0x4c8, 0x4cb, 0x4cd, 0x4d0, 0x4d2, 0x4d5, 0x4d7, 0x4d9, 0x4dc, 0x4de, 0x4e0, 0x4e3,
  0x4e5, 0x4e7, 0x4e9, 0x4eb, 0x4ed, 0x4ef, 0x4f1, 0x4f3, 0x4f5, 0x4f6, 0x4f8, 0x4fa, 0x4fb, 0x4fd, 0x4ff, 0x500,
  0x502, 0x503, 0x504, 0x506, 0x507, 0x508, 0x50a, 0x50b, 0x50c, 0x50d, 0x50e, 0x50f, 0x510, 0x511, 0x511, 0x512,
  0x513, 0x514, 0x514, 0x515, 0x516, 0x516, 0x517, 0x517, 0x517, 0x518, 0x518, 0x518, 0x518, 0x518, 0x519, 0x519
};

static int clamp16(int val);
static int clip16(int val);
static bool dsp_checkCounter(spc_dsp_t* dsp, int rate);
static void dsp_cycleChannel(spc_dsp_t* dsp, int ch);
static void dsp_handleEcho(spc_dsp_t* dsp);
static void dsp_handleGain(spc_dsp_t* dsp, int ch);
static void dsp_decodeBrr(spc_dsp_t* dsp, int ch);
static int16_t dsp_getSample(spc_dsp_t* dsp, int ch);
static void dsp_handleNoise(spc_dsp_t* dsp);

static void dsp_reset(spc_dsp_t* dsp) {
  memset(dsp->ram, 0, sizeof(dsp->ram));
  dsp->ram[0x7c] = 0xff; // set ENDx
  for(int i = 0; i < 8; i++) {
    dsp->channel[i].pitch = 0;
    dsp->channel[i].pitchCounter = 0;
    dsp->channel[i].pitchModulation = false;
    memset(dsp->channel[i].decodeBuffer, 0, sizeof(dsp->channel[i].decodeBuffer));
    dsp->channel[i].bufferOffset = 0;
    dsp->channel[i].srcn = 0;
    dsp->channel[i].decodeOffset = 0;
    dsp->channel[i].blockOffset = 0;
    dsp->channel[i].brrHeader = 0;
    dsp->channel[i].useNoise = false;
    dsp->channel[i].startDelay = 0;
    memset(dsp->channel[i].adsrRates, 0, sizeof(dsp->channel[i].adsrRates));
    dsp->channel[i].adsrState = 0;
    dsp->channel[i].sustainLevel = 0;
    dsp->channel[i].gainSustainLevel = 0;
    dsp->channel[i].useGain = false;
    dsp->channel[i].gainMode = 0;
    dsp->channel[i].directGain = false;
    dsp->channel[i].gainValue = 0;
    dsp->channel[i].preclampGain = 0;
    dsp->channel[i].gain = 0;
    dsp->channel[i].keyOn = false;
    dsp->channel[i].keyOff = false;
    dsp->channel[i].sampleOut = 0;
    dsp->channel[i].volumeL = 0;
    dsp->channel[i].volumeR = 0;
    dsp->channel[i].echoEnable = false;
  }
  dsp->counter = 0;
  dsp->dirPage = 0;
  dsp->evenCycle = true;
  dsp->mute = true;
  dsp->reset = true;
  dsp->masterVolumeL = 0;
  dsp->masterVolumeR = 0;
  dsp->sampleOutL = 0;
  dsp->sampleOutR = 0;
  dsp->echoOutL = 0;
  dsp->echoOutR = 0;
  dsp->noiseSample = 0x4000;
  dsp->noiseRate = 0;
  dsp->echoWrites = false;
  dsp->echoVolumeL = 0;
  dsp->echoVolumeR = 0;
  dsp->feedbackVolume = 0;
  dsp->echoBufferAdr = 0;
  dsp->echoDelay = 0;
  dsp->echoLength = 0;
  dsp->echoBufferIndex = 0;
  dsp->firBufferIndex = 0;
  memset(dsp->firValues, 0, sizeof(dsp->firValues));
  memset(dsp->firBufferL, 0, sizeof(dsp->firBufferL));
  memset(dsp->firBufferR, 0, sizeof(dsp->firBufferR));
}

static void dsp_cycle(spc_dsp_t* dsp) {
  dsp->sampleOutL = 0;
  dsp->sampleOutR = 0;
  dsp->echoOutL = 0;
  dsp->echoOutR = 0;
  for(int i = 0; i < 8; i++) {
    dsp_cycleChannel(dsp, i);
  }
  dsp_handleEcho(dsp); // also applies master volume
  dsp->counter = dsp->counter == 0 ? 30720 : dsp->counter - 1;
  dsp_handleNoise(dsp);
  dsp->evenCycle = !dsp->evenCycle;
  // handle mute flag
  if(dsp->mute) {
    dsp->sampleOutL = 0;
    dsp->sampleOutR = 0;
  }
}

static int clamp16(int val) {
  return val < -0x8000 ? -0x8000 : (val > 0x7fff ? 0x7fff : val);
}

static int clip16(int val) {
  return (int16_t) (val & 0xffff);
}

static bool dsp_checkCounter(spc_dsp_t* dsp, int rate) {
  if(rate == 0) return false;
  return ((dsp->counter + rateOffsets[rate]) % rateValues[rate]) == 0;
}

static void dsp_handleEcho(spc_dsp_t* dsp) {
  // increment fir buffer index
  dsp->firBufferIndex++;
  dsp->firBufferIndex &= 0x7;
  // get value out of ram
  uint16_t adr = dsp->echoBufferAdr + dsp->echoBufferIndex;
  int16_t ramSample = dsp->aram[adr] | (dsp->aram[(adr + 1) & 0xffff] << 8);
  dsp->firBufferL[dsp->firBufferIndex] = ramSample >> 1;
  ramSample = dsp->aram[(adr + 2) & 0xffff] | (dsp->aram[(adr + 3) & 0xffff] << 8);
  dsp->firBufferR[dsp->firBufferIndex] = ramSample >> 1;
  // calculate FIR-sum
  int sumL = 0, sumR = 0;
  for(int i = 0; i < 8; i++) {
    sumL += (dsp->firBufferL[(dsp->firBufferIndex + i + 1) & 0x7] * dsp->firValues[i]) >> 6;
    sumR += (dsp->firBufferR[(dsp->firBufferIndex + i + 1) & 0x7] * dsp->firValues[i]) >> 6;
    if(i == 6) {
      // clip to 16-bit before last addition
      sumL = clip16(sumL);
      sumR = clip16(sumR);
    }
  }
  sumL = clamp16(sumL) & ~1;
  sumR = clamp16(sumR) & ~1;
  // apply master volume and modify output with sum
  dsp->sampleOutL = clamp16(((dsp->sampleOutL * dsp->masterVolumeL) >> 7) + ((sumL * dsp->echoVolumeL) >> 7));
  dsp->sampleOutR = clamp16(((dsp->sampleOutR * dsp->masterVolumeR) >> 7) + ((sumR * dsp->echoVolumeR) >> 7));
  // get echo value
  int echoL = clamp16(dsp->echoOutL + clip16((sumL * dsp->feedbackVolume) >> 7)) & ~1;
  int echoR = clamp16(dsp->echoOutR + clip16((sumR * dsp->feedbackVolume) >> 7)) & ~1;
  // write it to ram
  if(dsp->echoWrites) {
    dsp->aram[adr] = echoL & 0xff;
    dsp->aram[(adr + 1) & 0xffff] = echoL >> 8;
    dsp->aram[(adr + 2) & 0xffff] = echoR & 0xff;
    dsp->aram[(adr + 3) & 0xffff] = echoR >> 8;
  }
  // handle indexes
  if(dsp->echoBufferIndex == 0) {
    dsp->echoLength = dsp->echoDelay * 4;
  }
  dsp->echoBufferIndex += 4;
  if(dsp->echoBufferIndex >= dsp->echoLength) {
    dsp->echoBufferIndex = 0;
  }
}

static void dsp_cycleChannel(spc_dsp_t* dsp, int ch) {
  // handle pitch counter
  int pitch = dsp->channel[ch].pitch;
  if(ch > 0 && dsp->channel[ch].pitchModulation) {
    pitch += ((dsp->channel[ch - 1].sampleOut >> 5) * pitch) >> 10;
  }
  // get current brr header and get sample address
  dsp->channel[ch].brrHeader = dsp->aram[dsp->channel[ch].decodeOffset];
  uint16_t samplePointer = dsp->dirPage + 4 * dsp->channel[ch].srcn;
  if(dsp->channel[ch].startDelay == 0) samplePointer += 2;
  uint16_t sampleAdr = dsp->aram[samplePointer] | (dsp->aram[(samplePointer + 1) & 0xffff] << 8);
  // handle starting of sample
  if(dsp->channel[ch].startDelay > 0) {
    if(dsp->channel[ch].startDelay == 5) {
      // first keyed on
      dsp->channel[ch].decodeOffset = sampleAdr;
      dsp->channel[ch].blockOffset = 1;
      dsp->channel[ch].bufferOffset = 0;
      dsp->channel[ch].brrHeader = 0;
      dsp->ram[0x7c] &= ~(1 << ch); // clear ENDx
    }
    dsp->channel[ch].gain = 0;
    dsp->channel[ch].startDelay--;
    dsp->channel[ch].pitchCounter = 0;
    if(dsp->channel[ch].startDelay > 0 && dsp->channel[ch].startDelay < 4) {
      dsp->channel[ch].pitchCounter = 0x4000;
    }
    pitch = 0;
  }
  // get sample
  int sample = 0;
  if(dsp->channel[ch].useNoise) {
    sample = clip16(dsp->noiseSample * 2);
  } else {
    sample = dsp_getSample(dsp, ch);
  }
  sample = ((sample * dsp->channel[ch].gain) >> 11) & ~1;
  // handle reset and release
  if(dsp->reset || (dsp->channel[ch].brrHeader & 0x03) == 1) {
    dsp->channel[ch].adsrState = 3; // go to release
    dsp->channel[ch].gain = 0;
  }
  // handle keyon/keyoff
  if(dsp->evenCycle) {
    if(dsp->channel[ch].keyOff) {
      dsp->channel[ch].adsrState = 3; // go to release
    }
    if(dsp->channel[ch].keyOn) {
      dsp->channel[ch].startDelay = 5;
      dsp->channel[ch].adsrState = 0; // go to attack
      dsp->channel[ch].keyOn = false;
    }
  }
  // handle envelope
  if(dsp->channel[ch].startDelay == 0) {
    dsp_handleGain(dsp, ch);
  }
  // decode new brr samples if needed and update offsets
  if(dsp->channel[ch].pitchCounter >= 0x4000) {
    dsp_decodeBrr(dsp, ch);
    if(dsp->channel[ch].blockOffset >= 7) {
      if(dsp->channel[ch].brrHeader & 0x1) {
        dsp->channel[ch].decodeOffset = sampleAdr;
        dsp->ram[0x7c] |= 1 << ch; // set ENDx
      } else {
        dsp->channel[ch].decodeOffset += 9;
      }
      dsp->channel[ch].blockOffset = 1;
    } else {
      dsp->channel[ch].blockOffset += 2;
    }
  }
  // update pitch counter
  dsp->channel[ch].pitchCounter &= 0x3fff;
  dsp->channel[ch].pitchCounter += pitch;
  if(dsp->channel[ch].pitchCounter > 0x7fff) dsp->channel[ch].pitchCounter = 0x7fff;
  // set outputs
  dsp->ram[(ch << 4) | 8] = dsp->channel[ch].gain >> 4;
  dsp->ram[(ch << 4) | 9] = sample >> 8;
  dsp->channel[ch].sampleOut = sample;
  dsp->sampleOutL = clamp16(dsp->sampleOutL + ((sample * dsp->channel[ch].volumeL) >> 7));
  dsp->sampleOutR = clamp16(dsp->sampleOutR + ((sample * dsp->channel[ch].volumeR) >> 7));
  if(dsp->channel[ch].echoEnable) {
    dsp->echoOutL = clamp16(dsp->echoOutL + ((sample * dsp->channel[ch].volumeL) >> 7));
    dsp->echoOutR = clamp16(dsp->echoOutR + ((sample * dsp->channel[ch].volumeR) >> 7));
  }
}

static void dsp_handleGain(spc_dsp_t* dsp, int ch) {
  int newGain = dsp->channel[ch].gain;
  int rate = 0;
  // handle gain mode
  if(dsp->channel[ch].adsrState == 3) { // release
    rate = 31;
    newGain -= 8;
  } else {
    if(!dsp->channel[ch].useGain) {
      rate = dsp->channel[ch].adsrRates[dsp->channel[ch].adsrState];
      switch(dsp->channel[ch].adsrState) {
        case 0: newGain += rate == 31 ? 1024 : 32; break; // attack
        case 1: newGain -= ((newGain - 1) >> 8) + 1; break; // decay
        case 2: newGain -= ((newGain - 1) >> 8) + 1; break; // sustain
      }
    } else {
      if(!dsp->channel[ch].directGain) {
        rate = dsp->channel[ch].adsrRates[3];
        switch(dsp->channel[ch].gainMode) {
          case 0: newGain -= 32; break; // linear decrease
          case 1: newGain -= ((newGain - 1) >> 8) + 1; break; // exponential decrease
          case 2: newGain += 32; break; // linear increase
          case 3: newGain += (dsp->channel[ch].preclampGain < 0x600) ? 32 : 8; break; // bent increase
        }
      } else { // direct gain
        rate = 31;
        newGain = dsp->channel[ch].gainValue;
      }
    }
  }
  // use sustain level according to mode
  int sustainLevel = dsp->channel[ch].useGain ? dsp->channel[ch].gainSustainLevel : dsp->channel[ch].sustainLevel;
  if(dsp->channel[ch].adsrState == 1 && (newGain >> 8) == sustainLevel) {
    dsp->channel[ch].adsrState = 2; // go to sustain
  }
  // store pre-clamped gain (for bent increase)
  dsp->channel[ch].preclampGain = newGain & 0xffff;
  // clamp gain
  if(newGain < 0 || newGain > 0x7ff) {
    newGain = newGain < 0 ? 0 : 0x7ff;
    if(dsp->channel[ch].adsrState == 0) {
      dsp->channel[ch].adsrState = 1; // go to decay
    }
  }
  // store new value
  if(dsp_checkCounter(dsp, rate)) dsp->channel[ch].gain = newGain;
}

static int16_t dsp_getSample(spc_dsp_t* dsp, int ch) {
  int pos = (dsp->channel[ch].pitchCounter >> 12) + dsp->channel[ch].bufferOffset;
  int offset = (dsp->channel[ch].pitchCounter >> 4) & 0xff;
  int16_t news = dsp->channel[ch].decodeBuffer[(pos + 3) % 12];
  int16_t olds = dsp->channel[ch].decodeBuffer[(pos + 2) % 12];
  int16_t olders = dsp->channel[ch].decodeBuffer[(pos + 1) % 12];
  int16_t oldests = dsp->channel[ch].decodeBuffer[pos % 12];
  int out = (gaussValues[0xff - offset] * oldests) >> 11;
  out += (gaussValues[0x1ff - offset] * olders) >> 11;
  out += (gaussValues[0x100 + offset] * olds) >> 11;
  out = clip16(out) + ((gaussValues[offset] * news) >> 11);
  return clamp16(out) & ~1;
}

static void dsp_decodeBrr(spc_dsp_t* dsp, int ch) {
  int shift = dsp->channel[ch].brrHeader >> 4;
  int filter = (dsp->channel[ch].brrHeader & 0xc) >> 2;
  int bOff = dsp->channel[ch].bufferOffset;
  int old = dsp->channel[ch].decodeBuffer[bOff == 0 ? 11 : bOff - 1] >> 1;
  int older = dsp->channel[ch].decodeBuffer[bOff == 0 ? 10 : bOff - 2] >> 1;
  uint8_t curByte = 0;
  for(int i = 0; i < 4; i++) {
    int s = 0;
    if(i & 1) {
      s = curByte & 0xf;
    } else {
      curByte = dsp->aram[(dsp->channel[ch].decodeOffset + dsp->channel[ch].blockOffset + (i >> 1)) & 0xffff];
      s = curByte >> 4;
    }
    if(s > 7) s -= 16;
    if(shift <= 0xc) {
      s = (s << shift) >> 1;
    } else {
      s = (s >> 3) << 12;
    }
    switch(filter) {
      case 1: s += old + (-old >> 4); break;
      case 2: s += 2 * old + ((3 * -old) >> 5) - older + (older >> 4); break;
      case 3: s += 2 * old + ((13 * -old) >> 6) - older + ((3 * older) >> 4); break;
    }
    dsp->channel[ch].decodeBuffer[bOff + i] = clamp16(s) * 2; // cuts off bit 15
    older = old;
    old = dsp->channel[ch].decodeBuffer[bOff + i] >> 1;
  }
  dsp->channel[ch].bufferOffset += 4;
  if(dsp->channel[ch].bufferOffset >= 12) dsp->channel[ch].bufferOffset = 0;
}

static void dsp_handleNoise(spc_dsp_t* dsp) {
  if(dsp_checkCounter(dsp, dsp->noiseRate)) {
    int bit = (dsp->noiseSample & 1) ^ ((dsp->noiseSample >> 1) & 1);
    dsp->noiseSample = ((dsp->noiseSample >> 1) & 0x3fff) | (bit << 14);
  }
}

static uint8_t dsp_read(spc_dsp_t* dsp, uint8_t adr) {
  return dsp->ram[adr];
}

static void dsp_write(spc_dsp_t* dsp, uint8_t adr, uint8_t val) {
  int ch = adr >> 4;
  switch(adr) {
    case 0x00: case 0x10: case 0x20: case 0x30: case 0x40: case 0x50: case 0x60: case 0x70: {
      dsp->channel[ch].volumeL = val;
      break;
    }
    case 0x01: case 0x11: case 0x21: case 0x31: case 0x41: case 0x51: case 0x61: case 0x71: {
      dsp->channel[ch].volumeR = val;
      break;
    }
    case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52: case 0x62: case 0x72: {
      dsp->channel[ch].pitch = (dsp->channel[ch].pitch & 0x3f00) | val;
      break;
    }
    case 0x03: case 0x13: case 0x23: case 0x33: case 0x43: case 0x53: case 0x63: case 0x73: {
      dsp->channel[ch].pitch = ((dsp->channel[ch].pitch & 0x00ff) | (val << 8)) & 0x3fff;
      break;
    }
    case 0x04: case 0x14: case 0x24: case 0x34: case 0x44: case 0x54: case 0x64: case 0x74: {
      dsp->channel[ch].srcn = val;
      break;
    }
    case 0x05: case 0x15: case 0x25: case 0x35: case 0x45: case 0x55: case 0x65: case 0x75: {
      dsp->channel[ch].adsrRates[0] = (val & 0xf) * 2 + 1;
      dsp->channel[ch].adsrRates[1] = ((val & 0x70) >> 4) * 2 + 16;
      dsp->channel[ch].useGain = (val & 0x80) == 0;
      break;
    }
    case 0x06: case 0x16: case 0x26: case 0x36: case 0x46: case 0x56: case 0x66: case 0x76: {
      dsp->channel[ch].adsrRates[2] = val & 0x1f;
      dsp->channel[ch].sustainLevel = (val & 0xe0) >> 5;
      break;
    }
    case 0x07: case 0x17: case 0x27: case 0x37: case 0x47: case 0x57: case 0x67: case 0x77: {
      dsp->channel[ch].directGain = (val & 0x80) == 0;
      dsp->channel[ch].gainMode = (val & 0x60) >> 5;
      dsp->channel[ch].adsrRates[3] = val & 0x1f;
      dsp->channel[ch].gainValue = (val & 0x7f) * 16;
      dsp->channel[ch].gainSustainLevel = (val & 0xe0) >> 5;
      break;
    }
    case 0x0c: {
      dsp->masterVolumeL = val;
      break;
    }
    case 0x1c: {
      dsp->masterVolumeR = val;
      break;
    }
    case 0x2c: {
      dsp->echoVolumeL = val;
      break;
    }
    case 0x3c: {
      dsp->echoVolumeR = val;
      break;
    }
    case 0x4c: {
      for(int i = 0; i < 8; i++) {
        dsp->channel[i].keyOn = val & (1 << i);
      }
      break;
    }
    case 0x5c: {
      for(int i = 0; i < 8; i++) {
        dsp->channel[i].keyOff = val & (1 << i);
      }
      break;
    }
    case 0x6c: {
      dsp->reset = val & 0x80;
      dsp->mute = val & 0x40;
      dsp->echoWrites = (val & 0x20) == 0;
      dsp->noiseRate = val & 0x1f;
      break;
    }
    case 0x7c: {
      val = 0; // any write clears ENDx
      break;
    }
    case 0x0d: {
      dsp->feedbackVolume = val;
      break;
    }
    case 0x2d: {
      for(int i = 0; i < 8; i++) {
        dsp->channel[i].pitchModulation = val & (1 << i);
      }
      break;
    }
    case 0x3d: {
      for(int i = 0; i < 8; i++) {
        dsp->channel[i].useNoise = val & (1 << i);
      }
      break;
    }
    case 0x4d: {
      for(int i = 0; i < 8; i++) {
        dsp->channel[i].echoEnable = val & (1 << i);
      }
      break;
    }
    case 0x5d: {
      dsp->dirPage = val << 8;
      break;
    }
    case 0x6d: {
      dsp->echoBufferAdr = val << 8;
      break;
    }
    case 0x7d: {
      dsp->echoDelay = (val & 0xf) * 512; // 2048-byte steps, stereo sample is 4 bytes
      break;
    }
    case 0x0f: case 0x1f: case 0x2f: case 0x3f: case 0x4f: case 0x5f: case 0x6f: case 0x7f: {
      dsp->firValues[ch] = val;
      break;
    }
  }
  dsp->ram[adr] = val;
}

/* ---- the unit around them (LakeSnes snes/apu.c) ---- */

static void smp_cycle(spc_t *s) {
  if((s->cycles & 0x1f) == 0) {
    // every 32 cycles
    dsp_cycle(&s->dsp);
    if(s->outbuf && s->outn + 2 <= s->outcap) {
      s->outbuf[s->outn++] = s->dsp.sampleOutL;
      s->outbuf[s->outn++] = s->dsp.sampleOutR;
    }
  }

  // handle timers
  for(int i = 0; i < 3; i++) {
    if(s->timer[i].cycles == 0) {
      s->timer[i].cycles = i == 2 ? 16 : 128;
      if(s->timer[i].enabled) {
        s->timer[i].divider++;
        if(s->timer[i].divider == s->timer[i].target) {
          s->timer[i].divider = 0;
          s->timer[i].counter++;
          s->timer[i].counter &= 0xf;
        }
      }
    }
    s->timer[i].cycles--;
  }

  s->cycles++;
}

static uint8_t smp_read(spc_t *s, uint16_t adr) {
  switch(adr) {
    case 0xf0:
    case 0xf1:
    case 0xfa:
    case 0xfb:
    case 0xfc: {
      return 0;
    }
    case 0xf2: {
      return s->dspAdr;
    }
    case 0xf3: {
      return dsp_read(&s->dsp, s->dspAdr & 0x7f);
    }
    case 0xf4:
    case 0xf5:
    case 0xf6:
    case 0xf7:
    case 0xf8:
    case 0xf9: {
      return s->in[adr - 0xf4];
    }
    case 0xfd:
    case 0xfe:
    case 0xff: {
      uint8_t ret = s->timer[adr - 0xfd].counter;
      s->timer[adr - 0xfd].counter = 0;
      return ret;
    }
  }
  return s->ram[adr];   /* no boot ROM at $FFC0: RAM there too */
}

static void smp_write(spc_t *s, uint16_t adr, uint8_t val) {
  switch(adr) {
    case 0xf0: {
      break; // test register
    }
    case 0xf1: {
      for(int i = 0; i < 3; i++) {
        if(!s->timer[i].enabled && (val & (1 << i))) {
          s->timer[i].divider = 0;
          s->timer[i].counter = 0;
        }
        s->timer[i].enabled = val & (1 << i);
      }
      if(val & 0x10) {
        s->in[0] = 0;
        s->in[1] = 0;
      }
      if(val & 0x20) {
        s->in[2] = 0;
        s->in[3] = 0;
      }
      break;
    }
    case 0xf2: {
      s->dspAdr = val;
      break;
    }
    case 0xf3: {
      if(s->dspAdr < 0x80) dsp_write(&s->dsp, s->dspAdr, val);
      break;
    }
    case 0xf4:
    case 0xf5:
    case 0xf6:
    case 0xf7: {
      s->out[adr - 0xf4] = val;
      break;
    }
    case 0xf8:
    case 0xf9: {
      s->in[adr - 0xf4] = val;
      break;
    }
    case 0xfa:
    case 0xfb:
    case 0xfc: {
      s->timer[adr - 0xfa].target = val;
      break;
    }
  }
  s->ram[adr] = val;
}

static uint8_t smp_cpuRead(spc_t *s, uint16_t adr) {
  smp_cycle(s);
  return smp_read(s, adr);
}

static void smp_cpuWrite(spc_t *s, uint16_t adr, uint8_t val) {
  smp_cycle(s);
  smp_write(s, adr, val);
}

static void smp_cpuIdle(spc_t *s) {
  smp_cycle(s);
}

/* ---- this port's interface ---- */

void spc_reset(spc_t *s) {
  int16_t *buf = s->outbuf; int cap = s->outcap;
  memset(s, 0, sizeof *s);
  s->outbuf = buf; s->outcap = cap;
  s->cpu.mem = s;
  s->dsp.aram = s->ram;
  cpu_reset(&s->cpu, true);
  dsp_reset(&s->dsp);
}

void spc_load(spc_t *s, uint16_t addr, const uint8_t *data, int len) {
  for (int i = 0; i < len; i++) s->ram[(uint16_t)(addr + i)] = data[i];
}

/* as the boot ROM leaves the CPU when it jumps to a program it has loaded */
void spc_start(spc_t *s, uint16_t pc) {
  spc_cpu_t *c = &s->cpu;
  c->pc = pc;
  c->a = c->x = c->y = 0;
  c->sp = 0xEF;
  c->c = c->v = c->n = c->i = c->h = c->p = c->b = false;
  c->z = true;
  c->stopped = false;
  c->resetWanted = false;
}

void spc_run(spc_t *s, int cycles) {
  s->cpu.mem = s;          /* the struct may have been copied */
  s->dsp.aram = s->ram;
  cycles -= s->debt;
  while (cycles > 0) {
    uint32_t start = s->cycles;
    cpu_runOpcode(&s->cpu);
    cycles -= (int)(s->cycles - start);
  }
  s->debt = -cycles;
}
