/*
 * cpu.c - 6502 interpreter (fallback path and reference model).
 *
 * The recompiled game code uses the same helpers from nes.h, so this
 * interpreter doubles as the oracle for cosimulation tests.
 */
#include "nes.h"

cpu_t cpu;
uint8_t ram[2048];

static const uint8_t base_cycles[256] = {
    7,6,2,8,3,3,5,5,3,2,2,2,4,4,6,6,
    2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6,
    2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    6,6,2,8,3,3,5,5,3,2,2,2,3,4,6,6,
    2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    6,6,2,8,3,3,5,5,4,2,2,2,5,4,6,6,
    2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
    2,6,2,6,4,4,4,4,2,5,2,5,5,5,5,5,
    2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
    2,5,2,5,4,4,4,4,2,4,2,4,4,4,4,4,
    2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
    2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
    2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7
};

void cpu_power(void) {
    memset(&cpu, 0, sizeof cpu);
    cpu.sp = 0xFD;
    cpu_set_p(0x24);
    cpu.pc = mem_read16(0xFFFC);
    cpu.cycles = 7;
}

/* RESET button: registers survive, SP drops by 3, I is set */
void cpu_reset(void) {
    cpu.sp = (uint8_t)(cpu.sp - 3);
    cpu.i = 1;
    cpu.pc = mem_read16(0xFFFC);
    cpu.cycles += 7;
    cpu.nmi_pending = 0;
    cpu.nmi_line = 0;
    cpu.irq_line = 0;
    cpu.jammed = 0;
    cpu.irq_delay_active = 0;
    cpu.int_pending = 0;
}

int cpu_interrupt_pending(void) { return cpu.int_pending; }

void cpu_take_interrupt(void) {
    if (!cpu.int_pending) return;
    cpu.int_pending = 0;
    if (cpu.nmi_pending && cpu.nmi_edge_cycle < cpu.cycles - 1) {
        cpu.nmi_pending = 0;
        cpu_do_interrupt(0xFFFA, 0);
    } else {
        cpu_do_interrupt(0xFFFE, 0);
        sys.irq_count++;
    }
    sys_sync();
}

/* ---- addressing helpers ---- */
static inline uint16_t fetch16(void) {
    uint16_t v = mem_read16(cpu.pc);
    cpu.pc += 2;
    return v;
}
static inline uint8_t fetch8(void) { return mem_read(cpu.pc++); }

static inline uint16_t addr_absx_rd(void) {
    uint16_t b = fetch16(), a = (uint16_t)(b + cpu.x);
    if ((b ^ a) & 0x100) { mem_read((uint16_t)((b & 0xff00) | (a & 0xff))); cpu.cycles++; }
    return a;
}
static inline uint16_t addr_absy_rd(void) {
    uint16_t b = fetch16(), a = (uint16_t)(b + cpu.y);
    if ((b ^ a) & 0x100) { mem_read((uint16_t)((b & 0xff00) | (a & 0xff))); cpu.cycles++; }
    return a;
}
static inline uint16_t addr_absx_wr(void) {
    uint16_t b = fetch16(), a = (uint16_t)(b + cpu.x);
    mem_read((uint16_t)((b & 0xff00) | (a & 0xff)));
    return a;
}
static inline uint16_t addr_absy_wr(void) {
    uint16_t b = fetch16(), a = (uint16_t)(b + cpu.y);
    mem_read((uint16_t)((b & 0xff00) | (a & 0xff)));
    return a;
}
static inline uint16_t addr_indx(void) {
    uint8_t z = (uint8_t)(fetch8() + cpu.x);
    return zp_read16(z);
}
static inline uint16_t addr_indy_rd(void) {
    uint16_t b = zp_read16(fetch8()), a = (uint16_t)(b + cpu.y);
    if ((b ^ a) & 0x100) { mem_read((uint16_t)((b & 0xff00) | (a & 0xff))); cpu.cycles++; }
    return a;
}
static inline uint16_t addr_indy_wr(void) {
    uint16_t b = zp_read16(fetch8()), a = (uint16_t)(b + cpu.y);
    mem_read((uint16_t)((b & 0xff00) | (a & 0xff)));
    return a;
}

#define RD_IMM()  fetch8()
#define RD_ZP()   ram[fetch8()]
#define RD_ZPX()  ram[(uint8_t)(fetch8() + cpu.x)]
#define RD_ZPY()  ram[(uint8_t)(fetch8() + cpu.y)]
#define RD_ABS()  mem_read(fetch16())
#define RD_ABSX() mem_read(addr_absx_rd())
#define RD_ABSY() mem_read(addr_absy_rd())
#define RD_INDX() mem_read(addr_indx())
#define RD_INDY() mem_read(addr_indy_rd())

/* read-modify-write on an arbitrary address: dummy write of old value first */
#define RMW(ADDR, FN) do { uint16_t a_ = (ADDR); uint8_t v_ = mem_read(a_); mem_write(a_, v_); v_ = FN(v_); mem_write(a_, v_); } while (0)
#define RMW_ZP(ADDR, FN) do { uint8_t a_ = (ADDR); ram[a_] = FN(ram[a_]); } while (0)

static inline void branch(int cond) {
    int8_t rel = (int8_t)fetch8();
    if (cond) {
        uint16_t t = (uint16_t)(cpu.pc + rel);
        cpu.cycles += 1 + (((t ^ cpu.pc) & 0x100) ? 1 : 0);
        cpu.pc = t;
    }
}

static inline void coverage_mark(uint16_t pc) {
    if (pc >= 0x8000) {
        int slot = (pc >> 13) & 3;
        uint32_t idx = (uint32_t)mapper.prg_bank[slot] * 8192u + (pc & 0x1fff);
        if (idx < cart.prg_size) sys.cov[idx] |= (uint8_t)(1 << slot);
    } else {
        /* code executed from RAM/WRAM: record in the extra region */
        sys.cov[cart.prg_size + (pc & 0x7fff)] |= 0x10;
    }
}

void cpu_step(void) {
    if (sys.trace_hook) sys.trace_hook();
    if (sys.hook && (cpu.pc == sys.hook_pc[0] || cpu.pc == sys.hook_pc[1])) sys.hook(cpu.pc);
    sys.instructions++;
    if (cpu.jammed) { cpu.cycles += 2; if (!sys.sandbox) sys_sync(); return; }
    if (sys.cov) coverage_mark(cpu.pc);

    uint8_t op = fetch8();
    cpu.cycles += base_cycles[op];

    switch (op) {
    /* ---- ORA ---- */
    case 0x09: cpu.a |= RD_IMM(); set_nz(cpu.a); break;
    case 0x05: cpu.a |= RD_ZP(); set_nz(cpu.a); break;
    case 0x15: cpu.a |= RD_ZPX(); set_nz(cpu.a); break;
    case 0x0D: cpu.a |= RD_ABS(); set_nz(cpu.a); break;
    case 0x1D: cpu.a |= RD_ABSX(); set_nz(cpu.a); break;
    case 0x19: cpu.a |= RD_ABSY(); set_nz(cpu.a); break;
    case 0x01: cpu.a |= RD_INDX(); set_nz(cpu.a); break;
    case 0x11: cpu.a |= RD_INDY(); set_nz(cpu.a); break;
    /* ---- AND ---- */
    case 0x29: cpu.a &= RD_IMM(); set_nz(cpu.a); break;
    case 0x25: cpu.a &= RD_ZP(); set_nz(cpu.a); break;
    case 0x35: cpu.a &= RD_ZPX(); set_nz(cpu.a); break;
    case 0x2D: cpu.a &= RD_ABS(); set_nz(cpu.a); break;
    case 0x3D: cpu.a &= RD_ABSX(); set_nz(cpu.a); break;
    case 0x39: cpu.a &= RD_ABSY(); set_nz(cpu.a); break;
    case 0x21: cpu.a &= RD_INDX(); set_nz(cpu.a); break;
    case 0x31: cpu.a &= RD_INDY(); set_nz(cpu.a); break;
    /* ---- EOR ---- */
    case 0x49: cpu.a ^= RD_IMM(); set_nz(cpu.a); break;
    case 0x45: cpu.a ^= RD_ZP(); set_nz(cpu.a); break;
    case 0x55: cpu.a ^= RD_ZPX(); set_nz(cpu.a); break;
    case 0x4D: cpu.a ^= RD_ABS(); set_nz(cpu.a); break;
    case 0x5D: cpu.a ^= RD_ABSX(); set_nz(cpu.a); break;
    case 0x59: cpu.a ^= RD_ABSY(); set_nz(cpu.a); break;
    case 0x41: cpu.a ^= RD_INDX(); set_nz(cpu.a); break;
    case 0x51: cpu.a ^= RD_INDY(); set_nz(cpu.a); break;
    /* ---- ADC ---- */
    case 0x69: op_adc(RD_IMM()); break;
    case 0x65: op_adc(RD_ZP()); break;
    case 0x75: op_adc(RD_ZPX()); break;
    case 0x6D: op_adc(RD_ABS()); break;
    case 0x7D: op_adc(RD_ABSX()); break;
    case 0x79: op_adc(RD_ABSY()); break;
    case 0x61: op_adc(RD_INDX()); break;
    case 0x71: op_adc(RD_INDY()); break;
    /* ---- SBC ---- */
    case 0xE9: case 0xEB: op_sbc(RD_IMM()); break;
    case 0xE5: op_sbc(RD_ZP()); break;
    case 0xF5: op_sbc(RD_ZPX()); break;
    case 0xED: op_sbc(RD_ABS()); break;
    case 0xFD: op_sbc(RD_ABSX()); break;
    case 0xF9: op_sbc(RD_ABSY()); break;
    case 0xE1: op_sbc(RD_INDX()); break;
    case 0xF1: op_sbc(RD_INDY()); break;
    /* ---- CMP / CPX / CPY ---- */
    case 0xC9: op_cmp(cpu.a, RD_IMM()); break;
    case 0xC5: op_cmp(cpu.a, RD_ZP()); break;
    case 0xD5: op_cmp(cpu.a, RD_ZPX()); break;
    case 0xCD: op_cmp(cpu.a, RD_ABS()); break;
    case 0xDD: op_cmp(cpu.a, RD_ABSX()); break;
    case 0xD9: op_cmp(cpu.a, RD_ABSY()); break;
    case 0xC1: op_cmp(cpu.a, RD_INDX()); break;
    case 0xD1: op_cmp(cpu.a, RD_INDY()); break;
    case 0xE0: op_cmp(cpu.x, RD_IMM()); break;
    case 0xE4: op_cmp(cpu.x, RD_ZP()); break;
    case 0xEC: op_cmp(cpu.x, RD_ABS()); break;
    case 0xC0: op_cmp(cpu.y, RD_IMM()); break;
    case 0xC4: op_cmp(cpu.y, RD_ZP()); break;
    case 0xCC: op_cmp(cpu.y, RD_ABS()); break;
    /* ---- loads ---- */
    case 0xA9: cpu.a = RD_IMM(); set_nz(cpu.a); break;
    case 0xA5: cpu.a = RD_ZP(); set_nz(cpu.a); break;
    case 0xB5: cpu.a = RD_ZPX(); set_nz(cpu.a); break;
    case 0xAD: cpu.a = RD_ABS(); set_nz(cpu.a); break;
    case 0xBD: cpu.a = RD_ABSX(); set_nz(cpu.a); break;
    case 0xB9: cpu.a = RD_ABSY(); set_nz(cpu.a); break;
    case 0xA1: cpu.a = RD_INDX(); set_nz(cpu.a); break;
    case 0xB1: cpu.a = RD_INDY(); set_nz(cpu.a); break;
    case 0xA2: cpu.x = RD_IMM(); set_nz(cpu.x); break;
    case 0xA6: cpu.x = RD_ZP(); set_nz(cpu.x); break;
    case 0xB6: cpu.x = RD_ZPY(); set_nz(cpu.x); break;
    case 0xAE: cpu.x = RD_ABS(); set_nz(cpu.x); break;
    case 0xBE: cpu.x = RD_ABSY(); set_nz(cpu.x); break;
    case 0xA0: cpu.y = RD_IMM(); set_nz(cpu.y); break;
    case 0xA4: cpu.y = RD_ZP(); set_nz(cpu.y); break;
    case 0xB4: cpu.y = RD_ZPX(); set_nz(cpu.y); break;
    case 0xAC: cpu.y = RD_ABS(); set_nz(cpu.y); break;
    case 0xBC: cpu.y = RD_ABSX(); set_nz(cpu.y); break;
    /* LAX */
    case 0xA7: cpu.a = cpu.x = RD_ZP(); set_nz(cpu.a); break;
    case 0xB7: cpu.a = cpu.x = RD_ZPY(); set_nz(cpu.a); break;
    case 0xAF: cpu.a = cpu.x = RD_ABS(); set_nz(cpu.a); break;
    case 0xBF: cpu.a = cpu.x = RD_ABSY(); set_nz(cpu.a); break;
    case 0xA3: cpu.a = cpu.x = RD_INDX(); set_nz(cpu.a); break;
    case 0xB3: cpu.a = cpu.x = RD_INDY(); set_nz(cpu.a); break;
    /* ---- stores ---- */
    case 0x85: ram[fetch8()] = cpu.a; break;
    case 0x95: ram[(uint8_t)(fetch8() + cpu.x)] = cpu.a; break;
    case 0x8D: mem_write(fetch16(), cpu.a); break;
    case 0x9D: mem_write(addr_absx_wr(), cpu.a); break;
    case 0x99: mem_write(addr_absy_wr(), cpu.a); break;
    case 0x81: mem_write(addr_indx(), cpu.a); break;
    case 0x91: mem_write(addr_indy_wr(), cpu.a); break;
    case 0x86: ram[fetch8()] = cpu.x; break;
    case 0x96: ram[(uint8_t)(fetch8() + cpu.y)] = cpu.x; break;
    case 0x8E: mem_write(fetch16(), cpu.x); break;
    case 0x84: ram[fetch8()] = cpu.y; break;
    case 0x94: ram[(uint8_t)(fetch8() + cpu.x)] = cpu.y; break;
    case 0x8C: mem_write(fetch16(), cpu.y); break;
    /* SAX */
    case 0x87: ram[fetch8()] = cpu.a & cpu.x; break;
    case 0x97: ram[(uint8_t)(fetch8() + cpu.y)] = cpu.a & cpu.x; break;
    case 0x8F: mem_write(fetch16(), cpu.a & cpu.x); break;
    case 0x83: mem_write(addr_indx(), cpu.a & cpu.x); break;
    /* unstable stores */
    case 0x9C: { uint16_t b = fetch16(); op_unstable_store(b, cpu.x, cpu.y); break; }          /* SHY abs,X */
    case 0x9E: { uint16_t b = fetch16(); op_unstable_store(b, cpu.y, cpu.x); break; }          /* SHX abs,Y */
    case 0x9F: { uint16_t b = fetch16(); op_unstable_store(b, cpu.y, cpu.a & cpu.x); break; }  /* SHA abs,Y */
    case 0x93: { uint16_t b = zp_read16(fetch8()); op_unstable_store(b, cpu.y, cpu.a & cpu.x); break; } /* SHA (ind),Y */
    case 0x9B: { uint16_t b = fetch16(); cpu.sp = cpu.a & cpu.x; op_unstable_store(b, cpu.y, cpu.sp); break; } /* TAS abs,Y */
    case 0xBB: op_las(RD_ABSY()); break;                                                       /* LAS abs,Y */
    /* ---- register transfers / stack ---- */
    case 0xAA: cpu.x = cpu.a; set_nz(cpu.x); break;
    case 0xA8: cpu.y = cpu.a; set_nz(cpu.y); break;
    case 0x8A: cpu.a = cpu.x; set_nz(cpu.a); break;
    case 0x98: cpu.a = cpu.y; set_nz(cpu.a); break;
    case 0xBA: cpu.x = cpu.sp; set_nz(cpu.x); break;
    case 0x9A: cpu.sp = cpu.x; break;
    case 0x48: push8(cpu.a); break;
    case 0x08: push8(cpu_get_p(1)); break;
    case 0x68: cpu.a = pop8(); set_nz(cpu.a); break;
    case 0x28: { uint8_t old_i = cpu.i; cpu_set_p(pop8()); cpu.irq_delay_i = old_i; cpu.irq_delay_active = 1; break; }
    /* ---- flags ---- */
    case 0x18: cpu.c = 0; break;
    case 0x38: cpu.c = 1; break;
    case 0x58: cpu.irq_delay_i = cpu.i; cpu.irq_delay_active = 1; cpu.i = 0; break;
    case 0x78: cpu.irq_delay_i = cpu.i; cpu.irq_delay_active = 1; cpu.i = 1; break;
    case 0xB8: cpu.v = 0; break;
    case 0xD8: cpu.d = 0; break;
    case 0xF8: cpu.d = 1; break;
    /* ---- inc/dec registers ---- */
    case 0xE8: cpu.x++; set_nz(cpu.x); break;
    case 0xCA: cpu.x--; set_nz(cpu.x); break;
    case 0xC8: cpu.y++; set_nz(cpu.y); break;
    case 0x88: cpu.y--; set_nz(cpu.y); break;
    /* ---- shifts on A ---- */
    case 0x0A: cpu.a = op_asl(cpu.a); break;
    case 0x4A: cpu.a = op_lsr(cpu.a); break;
    case 0x2A: cpu.a = op_rol(cpu.a); break;
    case 0x6A: cpu.a = op_ror(cpu.a); break;
    /* ---- RMW memory ---- */
    case 0x06: RMW_ZP(fetch8(), op_asl); break;
    case 0x16: RMW_ZP((uint8_t)(fetch8() + cpu.x), op_asl); break;
    case 0x0E: RMW(fetch16(), op_asl); break;
    case 0x1E: RMW(addr_absx_wr(), op_asl); break;
    case 0x46: RMW_ZP(fetch8(), op_lsr); break;
    case 0x56: RMW_ZP((uint8_t)(fetch8() + cpu.x), op_lsr); break;
    case 0x4E: RMW(fetch16(), op_lsr); break;
    case 0x5E: RMW(addr_absx_wr(), op_lsr); break;
    case 0x26: RMW_ZP(fetch8(), op_rol); break;
    case 0x36: RMW_ZP((uint8_t)(fetch8() + cpu.x), op_rol); break;
    case 0x2E: RMW(fetch16(), op_rol); break;
    case 0x3E: RMW(addr_absx_wr(), op_rol); break;
    case 0x66: RMW_ZP(fetch8(), op_ror); break;
    case 0x76: RMW_ZP((uint8_t)(fetch8() + cpu.x), op_ror); break;
    case 0x6E: RMW(fetch16(), op_ror); break;
    case 0x7E: RMW(addr_absx_wr(), op_ror); break;
    case 0xE6: RMW_ZP(fetch8(), op_inc); break;
    case 0xF6: RMW_ZP((uint8_t)(fetch8() + cpu.x), op_inc); break;
    case 0xEE: RMW(fetch16(), op_inc); break;
    case 0xFE: RMW(addr_absx_wr(), op_inc); break;
    case 0xC6: RMW_ZP(fetch8(), op_dec); break;
    case 0xD6: RMW_ZP((uint8_t)(fetch8() + cpu.x), op_dec); break;
    case 0xCE: RMW(fetch16(), op_dec); break;
    case 0xDE: RMW(addr_absx_wr(), op_dec); break;
    /* unofficial RMW */
    case 0x07: RMW_ZP(fetch8(), fn_slo); break;
    case 0x17: RMW_ZP((uint8_t)(fetch8() + cpu.x), fn_slo); break;
    case 0x0F: RMW(fetch16(), fn_slo); break;
    case 0x1F: RMW(addr_absx_wr(), fn_slo); break;
    case 0x1B: RMW(addr_absy_wr(), fn_slo); break;
    case 0x03: RMW(addr_indx(), fn_slo); break;
    case 0x13: RMW(addr_indy_wr(), fn_slo); break;
    case 0x27: RMW_ZP(fetch8(), fn_rla); break;
    case 0x37: RMW_ZP((uint8_t)(fetch8() + cpu.x), fn_rla); break;
    case 0x2F: RMW(fetch16(), fn_rla); break;
    case 0x3F: RMW(addr_absx_wr(), fn_rla); break;
    case 0x3B: RMW(addr_absy_wr(), fn_rla); break;
    case 0x23: RMW(addr_indx(), fn_rla); break;
    case 0x33: RMW(addr_indy_wr(), fn_rla); break;
    case 0x47: RMW_ZP(fetch8(), fn_sre); break;
    case 0x57: RMW_ZP((uint8_t)(fetch8() + cpu.x), fn_sre); break;
    case 0x4F: RMW(fetch16(), fn_sre); break;
    case 0x5F: RMW(addr_absx_wr(), fn_sre); break;
    case 0x5B: RMW(addr_absy_wr(), fn_sre); break;
    case 0x43: RMW(addr_indx(), fn_sre); break;
    case 0x53: RMW(addr_indy_wr(), fn_sre); break;
    case 0x67: RMW_ZP(fetch8(), fn_rra); break;
    case 0x77: RMW_ZP((uint8_t)(fetch8() + cpu.x), fn_rra); break;
    case 0x6F: RMW(fetch16(), fn_rra); break;
    case 0x7F: RMW(addr_absx_wr(), fn_rra); break;
    case 0x7B: RMW(addr_absy_wr(), fn_rra); break;
    case 0x63: RMW(addr_indx(), fn_rra); break;
    case 0x73: RMW(addr_indy_wr(), fn_rra); break;
    case 0xC7: RMW_ZP(fetch8(), fn_dcp); break;
    case 0xD7: RMW_ZP((uint8_t)(fetch8() + cpu.x), fn_dcp); break;
    case 0xCF: RMW(fetch16(), fn_dcp); break;
    case 0xDF: RMW(addr_absx_wr(), fn_dcp); break;
    case 0xDB: RMW(addr_absy_wr(), fn_dcp); break;
    case 0xC3: RMW(addr_indx(), fn_dcp); break;
    case 0xD3: RMW(addr_indy_wr(), fn_dcp); break;
    case 0xE7: RMW_ZP(fetch8(), fn_isc); break;
    case 0xF7: RMW_ZP((uint8_t)(fetch8() + cpu.x), fn_isc); break;
    case 0xEF: RMW(fetch16(), fn_isc); break;
    case 0xFF: RMW(addr_absx_wr(), fn_isc); break;
    case 0xFB: RMW(addr_absy_wr(), fn_isc); break;
    case 0xE3: RMW(addr_indx(), fn_isc); break;
    case 0xF3: RMW(addr_indy_wr(), fn_isc); break;
    /* ---- BIT ---- */
    case 0x24: op_bit(RD_ZP()); break;
    case 0x2C: op_bit(RD_ABS()); break;
    /* ---- immediate unofficial ---- */
    case 0x0B: case 0x2B: op_anc(RD_IMM()); break;
    case 0x4B: op_alr(RD_IMM()); break;
    case 0x6B: op_arr(RD_IMM()); break;
    case 0xCB: op_axs(RD_IMM()); break;
    case 0x8B: op_xaa(RD_IMM()); break;
    case 0xAB: op_lxa(RD_IMM()); break;
    /* ---- NOPs ---- */
    case 0xEA: case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: break;
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: cpu.pc++; break;
    case 0x04: case 0x44: case 0x64: (void)RD_ZP(); break;
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: (void)RD_ZPX(); break;
    case 0x0C: (void)RD_ABS(); break;
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: (void)RD_ABSX(); break;
    /* ---- jumps ---- */
    case 0x4C: cpu.pc = fetch16(); break;
    case 0x6C: {
        uint16_t p = fetch16();
        uint16_t lo = mem_read(p);
        uint16_t hi = mem_read((uint16_t)((p & 0xff00) | ((p + 1) & 0xff)));
        cpu.pc = (uint16_t)(lo | (hi << 8));
        break; }
    case 0x20: {
        uint16_t t = fetch16();
        push16((uint16_t)(cpu.pc - 1));
        cpu.pc = t;
        break; }
    case 0x60: cpu.pc = (uint16_t)(pop16() + 1); break;
    case 0x40: cpu_set_p(pop8()); cpu.pc = pop16(); break;
    case 0x00: {
        cpu.pc++;
        push16(cpu.pc);
        push8(cpu_get_p(1));
        cpu.i = 1;
        /* NMI hijack: if an NMI is pending it takes over the vector */
        cpu.pc = mem_read16(cpu.nmi_pending ? 0xFFFA : 0xFFFE);
        if (cpu.nmi_pending) cpu.nmi_pending = 0;
        break; }
    /* ---- branches ---- */
    case 0x10: branch(!cpu.n); break;
    case 0x30: branch(cpu.n); break;
    case 0x50: branch(!cpu.v); break;
    case 0x70: branch(cpu.v); break;
    case 0x90: branch(!cpu.c); break;
    case 0xB0: branch(cpu.c); break;
    case 0xD0: branch(!cpu.z); break;
    case 0xF0: branch(cpu.z); break;
    /* ---- KIL ---- */
    case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52: case 0x62: case 0x72:
    case 0x92: case 0xB2: case 0xD2: case 0xF2:
        cpu.jammed = 1; cpu.pc--; break;
    default: break;
    }
    if (!sys.sandbox) sys_sync();
}

int cpu_sandbox_run(cpu_t *regs, uint16_t stop, uint8_t *sram, uint8_t *swram, int max_insns) {
    static uint8_t ram_save[2048], wram_save[2048];
    cpu_t cpu_save = cpu;
    sys_t sys_save = sys;
    memcpy(ram_save, ram, sizeof ram);
    memcpy(wram_save, cart.wram, sizeof wram_save);
    memcpy(ram, sram, sizeof ram);
    memcpy(cart.wram, swram, sizeof wram_save);
    cpu = *regs;
    sys.sandbox = true;
    sys.sandbox_fault = false;
    sys.hook = NULL;
    sys.trace_hook = NULL;
    sys.cov = NULL;
    int n = 0;
    while (cpu.pc != stop && n < max_insns && !sys.sandbox_fault && !cpu.jammed) { cpu_step(); n++; }
    int ok = cpu.pc == stop && !sys.sandbox_fault && !cpu.jammed;
    memcpy(sram, ram, sizeof ram);
    memcpy(swram, cart.wram, sizeof wram_save);
    memcpy(ram, ram_save, sizeof ram);
    memcpy(cart.wram, wram_save, sizeof wram_save);
    *regs = cpu;
    cpu = cpu_save;
    sys = sys_save;
    return ok ? n : -1;
}
