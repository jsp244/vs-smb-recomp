/*
 * cpu816.c - 65C816 interpreter (cpu816.h).
 *
 * Native and emulation modes, every opcode including the block moves and decimal arithmetic.
 * Widths follow M (accumulator and memory) and X (index registers); in emulation mode both are
 * forced to 8 bits and the stack lives in page 1. Data addresses are 24-bit and carry into the next
 * bank; direct page, stack and the pointers read from them stay in bank 0; instruction fetches
 * wrap inside the program bank. Cycle counts are the usual base counts, plus one per 16-bit
 * memory operand: good enough for a step budget, not for timing hardware.
 */
#include "cpu816.h"

#define FL(f) (c->p & (f))
#define M8 (c->e || FL(P816_M))
#define X8 (c->e || FL(P816_X))

static inline uint8_t rd(cpu816_t *c, uint32_t a) { return c->read(c->ctx, a & 0xFFFFFF); }
static inline void wr(cpu816_t *c, uint32_t a, uint8_t v) { c->write(c->ctx, a & 0xFFFFFF, v); }
static inline uint16_t rd16(cpu816_t *c, uint32_t a) { return (uint16_t)(rd(c, a) | (rd(c, a + 1) << 8)); }
/* bank-0 words (direct page, stack, vectors) wrap inside the bank */
static inline uint16_t rd16b0(cpu816_t *c, uint16_t a) { return (uint16_t)(rd(c, a) | (rd(c, (uint16_t)(a + 1)) << 8)); }

static inline uint8_t fetch8(cpu816_t *c) {
    uint8_t v = rd(c, ((uint32_t)c->pb << 16) | c->pc);
    c->pc++;
    return v;
}
static inline uint16_t fetch16(cpu816_t *c) { uint16_t lo = fetch8(c); return (uint16_t)(lo | (fetch8(c) << 8)); }
static inline uint32_t fetch24(cpu816_t *c) { uint32_t lo = fetch16(c); return lo | ((uint32_t)fetch8(c) << 16); }

static inline void setnz8(cpu816_t *c, uint8_t v) {
    c->p = (uint8_t)((c->p & ~(P816_N | P816_Z)) | (v & 0x80) | (v ? 0 : P816_Z));
}
static inline void setnz16(cpu816_t *c, uint16_t v) {
    c->p = (uint8_t)((c->p & ~(P816_N | P816_Z)) | ((v >> 8) & 0x80) | (v ? 0 : P816_Z));
}

/* ---- stack ---- */
static inline void push8(cpu816_t *c, uint8_t v) {
    wr(c, c->s, v);
    if (c->e) c->s = (uint16_t)(0x100 | ((c->s - 1) & 0xFF));
    else c->s--;
}
static inline uint8_t pull8(cpu816_t *c) {
    if (c->e) c->s = (uint16_t)(0x100 | ((c->s + 1) & 0xFF));
    else c->s++;
    return rd(c, c->s);
}
static inline void push16(cpu816_t *c, uint16_t v) { push8(c, (uint8_t)(v >> 8)); push8(c, (uint8_t)v); }
static inline uint16_t pull16(cpu816_t *c) { uint16_t lo = pull8(c); return (uint16_t)(lo | (pull8(c) << 8)); }

static void fix_widths(cpu816_t *c) {
    if (c->e) { c->p |= P816_M | P816_X; c->s = (uint16_t)(0x100 | (c->s & 0xFF)); }
    if (X8) { c->x &= 0xFF; c->y &= 0xFF; }
}

/* ---- effective addresses ---- */
static inline uint16_t dp_addr(cpu816_t *c, uint8_t off, uint16_t idx) {
    /* emulation mode with the direct page on a page boundary wraps indexed accesses in the page */
    if (c->e && !(c->d & 0xFF)) return (uint16_t)(c->d | (uint8_t)(off + idx));
    return (uint16_t)(c->d + off + idx);
}
static inline uint32_t ea_dp(cpu816_t *c) { return dp_addr(c, fetch8(c), 0); }
static inline uint32_t ea_dpx(cpu816_t *c) { return dp_addr(c, fetch8(c), c->x); }
static inline uint32_t ea_dpy(cpu816_t *c) { return dp_addr(c, fetch8(c), c->y); }
static inline uint32_t ea_ind(cpu816_t *c) { uint16_t p = dp_addr(c, fetch8(c), 0); return ((uint32_t)c->db << 16) | rd16b0(c, p); }
static inline uint32_t ea_indx(cpu816_t *c) { uint16_t p = dp_addr(c, fetch8(c), c->x); return ((uint32_t)c->db << 16) | rd16b0(c, p); }
static inline uint32_t ea_indy(cpu816_t *c) {
    uint16_t p = dp_addr(c, fetch8(c), 0);
    return ((((uint32_t)c->db << 16) | rd16b0(c, p)) + c->y) & 0xFFFFFF;
}
static inline uint32_t ea_indl(cpu816_t *c) {
    uint16_t p = (uint16_t)(c->d + fetch8(c));
    return rd16b0(c, p) | ((uint32_t)rd(c, (uint16_t)(p + 2)) << 16);
}
static inline uint32_t ea_indly(cpu816_t *c) { return (ea_indl(c) + c->y) & 0xFFFFFF; }
static inline uint32_t ea_abs(cpu816_t *c) { return ((uint32_t)c->db << 16) | fetch16(c); }
static inline uint32_t ea_absx(cpu816_t *c) { return ((((uint32_t)c->db << 16) | fetch16(c)) + c->x) & 0xFFFFFF; }
static inline uint32_t ea_absy(cpu816_t *c) { return ((((uint32_t)c->db << 16) | fetch16(c)) + c->y) & 0xFFFFFF; }
static inline uint32_t ea_long(cpu816_t *c) { return fetch24(c); }
static inline uint32_t ea_longx(cpu816_t *c) { return (fetch24(c) + c->x) & 0xFFFFFF; }
static inline uint32_t ea_sr(cpu816_t *c) { return (uint16_t)(c->s + fetch8(c)); }
static inline uint32_t ea_sriy(cpu816_t *c) {
    uint16_t p = (uint16_t)(c->s + fetch8(c));
    return ((((uint32_t)c->db << 16) | rd16b0(c, p)) + c->y) & 0xFFFFFF;
}

/* a direct-page or stack address is a bank-0 address whose second byte wraps in bank 0 */
#define BANK0 0x1000000u
static inline uint32_t mark0(uint32_t a) { return a | BANK0; }
static inline uint16_t rdm16(cpu816_t *c, uint32_t a) {
    if (a & BANK0) return rd16b0(c, (uint16_t)a);
    return rd16(c, a);
}
static inline void wrm16(cpu816_t *c, uint32_t a, uint16_t v) {
    if (a & BANK0) { wr(c, (uint16_t)a, (uint8_t)v); wr(c, (uint16_t)(a + 1), (uint8_t)(v >> 8)); }
    else { wr(c, a, (uint8_t)v); wr(c, a + 1, (uint8_t)(v >> 8)); }
}

/* ---- arithmetic ---- */
static void adc8(cpu816_t *c, uint8_t m) {
    unsigned a = c->a & 0xFF, cin = c->p & P816_C, r;
    if (!FL(P816_D)) {
        r = a + m + cin;
        c->p = (uint8_t)((c->p & ~P816_V) | ((~(a ^ m) & (a ^ r) & 0x80) ? P816_V : 0));
    } else {
        r = (a & 0x0F) + (m & 0x0F) + cin;
        if (r > 0x09) r += 0x06;
        unsigned carry = r > 0x0F;
        r = (a & 0xF0) + (m & 0xF0) + (carry << 4) + (r & 0x0F);
        c->p = (uint8_t)((c->p & ~P816_V) | ((~(a ^ m) & (a ^ r) & 0x80) ? P816_V : 0));
        if (r > 0x9F) r += 0x60;
    }
    c->p = (uint8_t)((c->p & ~P816_C) | (r > 0xFF ? P816_C : 0));
    c->a = (uint16_t)((c->a & 0xFF00) | (r & 0xFF));
    setnz8(c, (uint8_t)r);
}
static void sbc8(cpu816_t *c, uint8_t m) {
    int a = c->a & 0xFF, cin = c->p & P816_C, d = (uint8_t)~m, r;
    if (!FL(P816_D)) {
        r = a + d + cin;
        c->p = (uint8_t)((c->p & ~P816_V) | ((~(a ^ d) & (a ^ r) & 0x80) ? P816_V : 0));
    } else {
        r = (a & 0x0F) + (d & 0x0F) + cin;
        if (r <= 0x0F) r -= 0x06;
        int carry = r > 0x0F;
        r = (a & 0xF0) + (d & 0xF0) + (carry << 4) + (r & 0x0F);
        c->p = (uint8_t)((c->p & ~P816_V) | ((~(a ^ d) & (a ^ r) & 0x80) ? P816_V : 0));
        if (r <= 0xFF) r -= 0x60;
    }
    c->p = (uint8_t)((c->p & ~P816_C) | ((r > 0xFF) ? P816_C : 0));
    c->a = (uint16_t)((c->a & 0xFF00) | (r & 0xFF));
    setnz8(c, (uint8_t)r);
}
static void adc16(cpu816_t *c, uint16_t m) {
    unsigned a = c->a, cin = c->p & P816_C, r;
    if (!FL(P816_D)) {
        r = a + m + cin;
        c->p = (uint8_t)((c->p & ~P816_V) | ((~(a ^ m) & (a ^ r) & 0x8000) ? P816_V : 0));
    } else {
        unsigned carry;
        r = (a & 0x000F) + (m & 0x000F) + cin;
        if (r > 0x0009) r += 0x0006;
        carry = r > 0x000F;
        r = (a & 0x00F0) + (m & 0x00F0) + (carry << 4) + (r & 0x000F);
        if (r > 0x009F) r += 0x0060;
        carry = r > 0x00FF;
        r = (a & 0x0F00) + (m & 0x0F00) + (carry << 8) + (r & 0x00FF);
        if (r > 0x09FF) r += 0x0600;
        carry = r > 0x0FFF;
        r = (a & 0xF000) + (m & 0xF000) + (carry << 12) + (r & 0x0FFF);
        c->p = (uint8_t)((c->p & ~P816_V) | ((~(a ^ m) & (a ^ r) & 0x8000) ? P816_V : 0));
        if (r > 0x9FFF) r += 0x6000;
    }
    c->p = (uint8_t)((c->p & ~P816_C) | (r > 0xFFFF ? P816_C : 0));
    c->a = (uint16_t)r;
    setnz16(c, c->a);
}
static void sbc16(cpu816_t *c, uint16_t m) {
    int a = c->a, cin = c->p & P816_C, d = (uint16_t)~m, r;
    if (!FL(P816_D)) {
        r = a + d + cin;
        c->p = (uint8_t)((c->p & ~P816_V) | ((~(a ^ d) & (a ^ r) & 0x8000) ? P816_V : 0));
    } else {
        int carry;
        r = (a & 0x000F) + (d & 0x000F) + cin;
        if (r <= 0x000F) r -= 0x0006;
        carry = r > 0x000F;
        r = (a & 0x00F0) + (d & 0x00F0) + (carry << 4) + (r & 0x000F);
        if (r <= 0x00FF) r -= 0x0060;
        carry = r > 0x00FF;
        r = (a & 0x0F00) + (d & 0x0F00) + (carry << 8) + (r & 0x00FF);
        if (r <= 0x0FFF) r -= 0x0600;
        carry = r > 0x0FFF;
        r = (a & 0xF000) + (d & 0xF000) + (carry << 12) + (r & 0x0FFF);
        c->p = (uint8_t)((c->p & ~P816_V) | ((~(a ^ d) & (a ^ r) & 0x8000) ? P816_V : 0));
        if (r <= 0xFFFF) r -= 0x6000;
    }
    c->p = (uint8_t)((c->p & ~P816_C) | ((r > 0xFFFF) ? P816_C : 0));
    c->a = (uint16_t)r;
    setnz16(c, c->a);
}
static void cmp8(cpu816_t *c, uint8_t r, uint8_t m) {
    c->p = (uint8_t)((c->p & ~P816_C) | (r >= m ? P816_C : 0));
    setnz8(c, (uint8_t)(r - m));
}
static void cmp16(cpu816_t *c, uint16_t r, uint16_t m) {
    c->p = (uint8_t)((c->p & ~P816_C) | (r >= m ? P816_C : 0));
    setnz16(c, (uint16_t)(r - m));
}

/* ---- read-modify-write on memory or the accumulator, at the M width ---- */
enum { RMW_ASL, RMW_ROL, RMW_LSR, RMW_ROR, RMW_INC, RMW_DEC, RMW_TSB, RMW_TRB };
static uint16_t rmw_op(cpu816_t *c, int op, uint16_t v, int wide) {
    uint16_t top = wide ? 0x8000 : 0x80, mask = wide ? 0xFFFF : 0xFF;
    unsigned cin = c->p & P816_C;
    switch (op) {
    case RMW_ASL: c->p = (uint8_t)((c->p & ~P816_C) | ((v & top) ? P816_C : 0)); v = (uint16_t)((v << 1) & mask); break;
    case RMW_ROL: c->p = (uint8_t)((c->p & ~P816_C) | ((v & top) ? P816_C : 0)); v = (uint16_t)(((v << 1) | cin) & mask); break;
    case RMW_LSR: c->p = (uint8_t)((c->p & ~P816_C) | (v & 1)); v = (uint16_t)(v >> 1); break;
    case RMW_ROR: c->p = (uint8_t)((c->p & ~P816_C) | (v & 1)); v = (uint16_t)((v >> 1) | (cin ? top : 0)); break;
    case RMW_INC: v = (uint16_t)((v + 1) & mask); break;
    case RMW_DEC: v = (uint16_t)((v - 1) & mask); break;
    case RMW_TSB: case RMW_TRB: {
        uint16_t a = c->a & mask;
        c->p = (uint8_t)((c->p & ~P816_Z) | ((a & v) ? 0 : P816_Z));
        return op == RMW_TSB ? (uint16_t)(v | a) : (uint16_t)(v & ~a & mask); }
    }
    if (wide) setnz16(c, v); else setnz8(c, (uint8_t)v);
    return v;
}
static void rmw_mem(cpu816_t *c, int op, uint32_t a) {
    if (M8) { uint8_t v = rd(c, a); wr(c, a, (uint8_t)rmw_op(c, op, v, 0)); }
    else { uint16_t v = rdm16(c, a); wrm16(c, a, rmw_op(c, op, v, 1)); c->cycles += 2; }
}
static void rmw_acc(cpu816_t *c, int op) {
    if (M8) c->a = (uint16_t)((c->a & 0xFF00) | rmw_op(c, op, c->a & 0xFF, 0));
    else c->a = rmw_op(c, op, c->a, 1);
}

/* ---- loads, stores and ALU operations at the M or X width ---- */
enum { OP_ORA, OP_AND, OP_EOR, OP_ADC, OP_LDA, OP_CMP, OP_SBC, OP_BIT };
static void alu_m(cpu816_t *c, int op, uint16_t v) {
    if (M8) {
        uint8_t m = (uint8_t)v, a = (uint8_t)c->a;
        switch (op) {
        case OP_ORA: a |= m; c->a = (uint16_t)((c->a & 0xFF00) | a); setnz8(c, a); break;
        case OP_AND: a &= m; c->a = (uint16_t)((c->a & 0xFF00) | a); setnz8(c, a); break;
        case OP_EOR: a ^= m; c->a = (uint16_t)((c->a & 0xFF00) | a); setnz8(c, a); break;
        case OP_ADC: adc8(c, m); break;
        case OP_LDA: c->a = (uint16_t)((c->a & 0xFF00) | m); setnz8(c, m); break;
        case OP_CMP: cmp8(c, a, m); break;
        case OP_SBC: sbc8(c, m); break;
        case OP_BIT:
            c->p = (uint8_t)((c->p & ~(P816_N | P816_V | P816_Z)) | (m & 0xC0) | ((a & m) ? 0 : P816_Z));
            break;
        }
    } else {
        uint16_t m = v;
        switch (op) {
        case OP_ORA: c->a |= m; setnz16(c, c->a); break;
        case OP_AND: c->a &= m; setnz16(c, c->a); break;
        case OP_EOR: c->a ^= m; setnz16(c, c->a); break;
        case OP_ADC: adc16(c, m); break;
        case OP_LDA: c->a = m; setnz16(c, m); break;
        case OP_CMP: cmp16(c, c->a, m); break;
        case OP_SBC: sbc16(c, m); break;
        case OP_BIT:
            c->p = (uint8_t)((c->p & ~(P816_N | P816_V | P816_Z)) | ((m >> 8) & 0xC0) | ((c->a & m) ? 0 : P816_Z));
            break;
        }
    }
}
static inline uint16_t rd_m(cpu816_t *c, uint32_t a) {
    if (M8) return rd(c, a);
    c->cycles++;
    return rdm16(c, a);
}
static inline uint16_t rd_x(cpu816_t *c, uint32_t a) {
    if (X8) return rd(c, a);
    c->cycles++;
    return rdm16(c, a);
}
static inline void wr_m(cpu816_t *c, uint32_t a, uint16_t v) {
    if (M8) wr(c, a & 0xFFFFFF, (uint8_t)v);
    else { wrm16(c, a, v); c->cycles++; }
}
static inline void wr_x(cpu816_t *c, uint32_t a, uint16_t v) {
    if (X8) wr(c, a & 0xFFFFFF, (uint8_t)v);
    else { wrm16(c, a, v); c->cycles++; }
}
static inline uint16_t imm_m(cpu816_t *c) { return M8 ? fetch8(c) : fetch16(c); }
static inline uint16_t imm_x(cpu816_t *c) { return X8 ? fetch8(c) : fetch16(c); }

static void ld_x(cpu816_t *c, uint16_t *r, uint16_t v) {
    if (X8) { *r = (uint8_t)v; setnz8(c, (uint8_t)v); } else { *r = v; setnz16(c, v); }
}
static void cp_x(cpu816_t *c, uint16_t r, uint16_t v) {
    if (X8) cmp8(c, (uint8_t)r, (uint8_t)v); else cmp16(c, r, v);
}

static void branch(cpu816_t *c, int cond) {
    int8_t off = (int8_t)fetch8(c);
    if (cond) { c->pc = (uint16_t)(c->pc + off); c->cycles++; }
}

static void interrupt(cpu816_t *c, uint16_t native_vec, uint16_t emu_vec, int brk) {
    if (!c->e) {
        push8(c, c->pb);
        push16(c, c->pc);
        push8(c, c->p);
    } else {
        push16(c, c->pc);
        push8(c, (uint8_t)((c->p & ~0x10) | (brk ? 0x10 : 0)));   /* the B bit sits where X does */
    }
    c->p = (uint8_t)((c->p | P816_I) & ~P816_D);
    c->pb = 0;
    c->pc = rd16b0(c, c->e ? emu_vec : native_vec);
    c->cycles += 7;
}

void cpu816_interrupt(cpu816_t *c, int nmi) {
    c->stopped = 0;
    if (nmi) interrupt(c, 0xFFEA, 0xFFFA, 0);
    else interrupt(c, 0xFFEE, 0xFFFE, 0);
}

void cpu816_reset(cpu816_t *c) {
    c->e = 1;
    c->p = P816_M | P816_X | P816_I;
    c->d = 0; c->db = 0; c->pb = 0;
    c->s = 0x1FF;
    c->x &= 0xFF; c->y &= 0xFF;
    c->stopped = 0;
    c->pc = rd16b0(c, 0xFFFC);
}

/* base cycles per opcode (65C816, 8-bit widths, direct page on a page boundary) */
static const uint8_t cyc[256] = {
    7,6,7,4,5,3,5,6,3,2,2,4,6,4,6,5, 2,5,5,7,5,4,6,6,2,4,2,2,6,4,7,5,
    6,6,8,4,3,3,5,6,4,2,2,5,4,4,6,5, 2,5,5,7,4,4,6,6,2,4,2,2,4,4,7,5,
    6,6,2,4,7,3,5,6,3,2,2,3,3,4,6,5, 2,5,5,7,7,4,6,6,2,4,3,2,4,4,7,5,
    6,6,6,4,3,3,5,6,4,2,2,6,5,4,6,5, 2,5,5,7,4,4,6,6,2,4,4,2,6,4,7,5,
    3,6,4,4,3,3,3,6,2,2,2,3,4,4,4,5, 2,6,5,7,4,4,4,6,2,5,2,2,4,5,5,5,
    2,6,2,4,3,3,3,6,2,2,2,4,4,4,4,5, 2,5,5,7,4,4,4,6,2,4,2,2,4,4,4,5,
    2,6,3,4,3,3,5,6,2,2,2,3,4,4,6,5, 2,5,5,7,6,4,6,6,2,4,3,3,6,4,7,5,
    2,6,3,4,3,3,5,6,2,2,2,3,4,4,6,5, 2,5,5,7,5,4,6,6,2,4,4,2,8,4,7,5,
};

void cpu816_step(cpu816_t *c) {
    uint8_t op = fetch8(c);

    c->cycles += cyc[op];
    switch (op) {
    /* ---- ORA AND EOR ADC STA LDA CMP SBC: the eight addressing-mode groups ---- */
#define ALU_GROUP(base, OP) \
    case base + 0x01: alu_m(c, OP, rd_m(c, ea_indx(c))); break;          \
    case base + 0x03: alu_m(c, OP, rd_m(c, mark0(ea_sr(c)))); break;     \
    case base + 0x05: alu_m(c, OP, rd_m(c, mark0(ea_dp(c)))); break;     \
    case base + 0x07: alu_m(c, OP, rd_m(c, ea_indl(c))); break;          \
    case base + 0x09: alu_m(c, OP, imm_m(c)); break;                     \
    case base + 0x0D: alu_m(c, OP, rd_m(c, ea_abs(c))); break;           \
    case base + 0x0F: alu_m(c, OP, rd_m(c, ea_long(c))); break;          \
    case base + 0x11: alu_m(c, OP, rd_m(c, ea_indy(c))); break;          \
    case base + 0x12: alu_m(c, OP, rd_m(c, ea_ind(c))); break;           \
    case base + 0x13: alu_m(c, OP, rd_m(c, ea_sriy(c))); break;          \
    case base + 0x15: alu_m(c, OP, rd_m(c, mark0(ea_dpx(c)))); break;    \
    case base + 0x17: alu_m(c, OP, rd_m(c, ea_indly(c))); break;         \
    case base + 0x19: alu_m(c, OP, rd_m(c, ea_absy(c))); break;          \
    case base + 0x1D: alu_m(c, OP, rd_m(c, ea_absx(c))); break;          \
    case base + 0x1F: alu_m(c, OP, rd_m(c, ea_longx(c))); break;
    ALU_GROUP(0x00, OP_ORA)
    ALU_GROUP(0x20, OP_AND)
    ALU_GROUP(0x40, OP_EOR)
    ALU_GROUP(0x60, OP_ADC)
    ALU_GROUP(0xA0, OP_LDA)
    ALU_GROUP(0xC0, OP_CMP)
    ALU_GROUP(0xE0, OP_SBC)
#undef ALU_GROUP
    /* STA (no immediate) */
    case 0x81: wr_m(c, ea_indx(c), c->a); break;
    case 0x83: wr_m(c, mark0(ea_sr(c)), c->a); break;
    case 0x85: wr_m(c, mark0(ea_dp(c)), c->a); break;
    case 0x87: wr_m(c, ea_indl(c), c->a); break;
    case 0x8D: wr_m(c, ea_abs(c), c->a); break;
    case 0x8F: wr_m(c, ea_long(c), c->a); break;
    case 0x91: wr_m(c, ea_indy(c), c->a); break;
    case 0x92: wr_m(c, ea_ind(c), c->a); break;
    case 0x93: wr_m(c, ea_sriy(c), c->a); break;
    case 0x95: wr_m(c, mark0(ea_dpx(c)), c->a); break;
    case 0x97: wr_m(c, ea_indly(c), c->a); break;
    case 0x99: wr_m(c, ea_absy(c), c->a); break;
    case 0x9D: wr_m(c, ea_absx(c), c->a); break;
    case 0x9F: wr_m(c, ea_longx(c), c->a); break;
    /* STZ */
    case 0x64: wr_m(c, mark0(ea_dp(c)), 0); break;
    case 0x74: wr_m(c, mark0(ea_dpx(c)), 0); break;
    case 0x9C: wr_m(c, ea_abs(c), 0); break;
    case 0x9E: wr_m(c, ea_absx(c), 0); break;
    /* BIT */
    case 0x24: alu_m(c, OP_BIT, rd_m(c, mark0(ea_dp(c)))); break;
    case 0x2C: alu_m(c, OP_BIT, rd_m(c, ea_abs(c))); break;
    case 0x34: alu_m(c, OP_BIT, rd_m(c, mark0(ea_dpx(c)))); break;
    case 0x3C: alu_m(c, OP_BIT, rd_m(c, ea_absx(c))); break;
    case 0x89: {   /* BIT #: Z only */
        uint16_t m = imm_m(c), r = M8 ? (uint16_t)(c->a & m & 0xFF) : (uint16_t)(c->a & m);
        c->p = (uint8_t)((c->p & ~P816_Z) | (r ? 0 : P816_Z));
        break; }
    /* read-modify-write */
#define RMW_GROUP(OP, acc, dp, abs, dpx, absx) \
    case acc: rmw_acc(c, OP); break;                       \
    case dp: rmw_mem(c, OP, mark0(ea_dp(c))); break;       \
    case abs: rmw_mem(c, OP, ea_abs(c)); break;            \
    case dpx: rmw_mem(c, OP, mark0(ea_dpx(c))); break;     \
    case absx: rmw_mem(c, OP, ea_absx(c)); break;
    RMW_GROUP(RMW_ASL, 0x0A, 0x06, 0x0E, 0x16, 0x1E)
    RMW_GROUP(RMW_ROL, 0x2A, 0x26, 0x2E, 0x36, 0x3E)
    RMW_GROUP(RMW_LSR, 0x4A, 0x46, 0x4E, 0x56, 0x5E)
    RMW_GROUP(RMW_ROR, 0x6A, 0x66, 0x6E, 0x76, 0x7E)
    RMW_GROUP(RMW_INC, 0x1A, 0xE6, 0xEE, 0xF6, 0xFE)
    RMW_GROUP(RMW_DEC, 0x3A, 0xC6, 0xCE, 0xD6, 0xDE)
#undef RMW_GROUP
    case 0x04: rmw_mem(c, RMW_TSB, mark0(ea_dp(c))); break;
    case 0x0C: rmw_mem(c, RMW_TSB, ea_abs(c)); break;
    case 0x14: rmw_mem(c, RMW_TRB, mark0(ea_dp(c))); break;
    case 0x1C: rmw_mem(c, RMW_TRB, ea_abs(c)); break;
    /* index registers */
    case 0xA2: ld_x(c, &c->x, imm_x(c)); break;
    case 0xA6: ld_x(c, &c->x, rd_x(c, mark0(ea_dp(c)))); break;
    case 0xAE: ld_x(c, &c->x, rd_x(c, ea_abs(c))); break;
    case 0xB6: ld_x(c, &c->x, rd_x(c, mark0(ea_dpy(c)))); break;
    case 0xBE: ld_x(c, &c->x, rd_x(c, ea_absy(c))); break;
    case 0xA0: ld_x(c, &c->y, imm_x(c)); break;
    case 0xA4: ld_x(c, &c->y, rd_x(c, mark0(ea_dp(c)))); break;
    case 0xAC: ld_x(c, &c->y, rd_x(c, ea_abs(c))); break;
    case 0xB4: ld_x(c, &c->y, rd_x(c, mark0(ea_dpx(c)))); break;
    case 0xBC: ld_x(c, &c->y, rd_x(c, ea_absx(c))); break;
    case 0x86: wr_x(c, mark0(ea_dp(c)), c->x); break;
    case 0x8E: wr_x(c, ea_abs(c), c->x); break;
    case 0x96: wr_x(c, mark0(ea_dpy(c)), c->x); break;
    case 0x84: wr_x(c, mark0(ea_dp(c)), c->y); break;
    case 0x8C: wr_x(c, ea_abs(c), c->y); break;
    case 0x94: wr_x(c, mark0(ea_dpx(c)), c->y); break;
    case 0xE0: cp_x(c, c->x, imm_x(c)); break;
    case 0xE4: cp_x(c, c->x, rd_x(c, mark0(ea_dp(c)))); break;
    case 0xEC: cp_x(c, c->x, rd_x(c, ea_abs(c))); break;
    case 0xC0: cp_x(c, c->y, imm_x(c)); break;
    case 0xC4: cp_x(c, c->y, rd_x(c, mark0(ea_dp(c)))); break;
    case 0xCC: cp_x(c, c->y, rd_x(c, ea_abs(c))); break;
    case 0xE8: ld_x(c, &c->x, (uint16_t)(c->x + 1)); break;
    case 0xCA: ld_x(c, &c->x, (uint16_t)(c->x - 1)); break;
    case 0xC8: ld_x(c, &c->y, (uint16_t)(c->y + 1)); break;
    case 0x88: ld_x(c, &c->y, (uint16_t)(c->y - 1)); break;
    /* transfers */
    case 0xAA: ld_x(c, &c->x, c->a); break;   /* TAX */
    case 0xA8: ld_x(c, &c->y, c->a); break;   /* TAY */
    case 0x8A: alu_m(c, OP_LDA, c->x); break; /* TXA */
    case 0x98: alu_m(c, OP_LDA, c->y); break; /* TYA */
    case 0x9B: ld_x(c, &c->y, c->x); break;   /* TXY */
    case 0xBB: ld_x(c, &c->x, c->y); break;   /* TYX */
    case 0xBA: ld_x(c, &c->x, c->s); break;   /* TSX */
    case 0x9A: c->s = c->e ? (uint16_t)(0x100 | (c->x & 0xFF)) : c->x; break;   /* TXS */
    case 0x1B: c->s = c->e ? (uint16_t)(0x100 | (c->a & 0xFF)) : c->a; break;   /* TCS */
    case 0x3B: c->a = c->s; setnz16(c, c->a); break;    /* TSC */
    case 0x5B: c->d = c->a; setnz16(c, c->d); break;    /* TCD */
    case 0x7B: c->a = c->d; setnz16(c, c->a); break;    /* TDC */
    case 0xEB: c->a = (uint16_t)((c->a >> 8) | (c->a << 8)); setnz8(c, (uint8_t)c->a); break;   /* XBA */
    /* stack */
    case 0x48: if (M8) push8(c, (uint8_t)c->a); else push16(c, c->a); break;
    case 0x68: if (M8) { uint8_t v = pull8(c); c->a = (uint16_t)((c->a & 0xFF00) | v); setnz8(c, v); } else { c->a = pull16(c); setnz16(c, c->a); } break;
    case 0xDA: if (X8) push8(c, (uint8_t)c->x); else push16(c, c->x); break;
    case 0xFA: if (X8) { c->x = pull8(c); setnz8(c, (uint8_t)c->x); } else { c->x = pull16(c); setnz16(c, c->x); } break;
    case 0x5A: if (X8) push8(c, (uint8_t)c->y); else push16(c, c->y); break;
    case 0x7A: if (X8) { c->y = pull8(c); setnz8(c, (uint8_t)c->y); } else { c->y = pull16(c); setnz16(c, c->y); } break;
    case 0x8B: push8(c, c->db); break;
    case 0xAB: c->db = pull8(c); setnz8(c, c->db); break;
    case 0x0B: push16(c, c->d); break;
    case 0x2B: c->d = pull16(c); setnz16(c, c->d); break;
    case 0x4B: push8(c, c->pb); break;
    case 0x08: push8(c, c->e ? (uint8_t)(c->p | 0x30) : c->p); break;
    case 0x28: c->p = pull8(c); fix_widths(c); break;
    case 0xF4: push16(c, fetch16(c)); break;                                        /* PEA */
    case 0xD4: { uint16_t p = (uint16_t)(c->d + fetch8(c)); push16(c, rd16b0(c, p)); break; }   /* PEI */
    case 0x62: { int16_t off = (int16_t)fetch16(c); push16(c, (uint16_t)(c->pc + off)); break; } /* PER */
    /* flags */
    case 0x18: c->p &= ~P816_C; break;
    case 0x38: c->p |= P816_C; break;
    case 0x58: c->p &= ~P816_I; break;
    case 0x78: c->p |= P816_I; break;
    case 0xB8: c->p &= ~P816_V; break;
    case 0xD8: c->p &= ~P816_D; break;
    case 0xF8: c->p |= P816_D; break;
    case 0xC2: c->p &= (uint8_t)~fetch8(c); fix_widths(c); break;   /* REP */
    case 0xE2: c->p |= fetch8(c); fix_widths(c); break;             /* SEP */
    case 0xFB: {   /* XCE */
        uint8_t carry = c->p & P816_C;
        c->p = (uint8_t)((c->p & ~P816_C) | (c->e ? P816_C : 0));
        c->e = carry ? 1 : 0;
        fix_widths(c);
        break; }
    /* branches and jumps */
    case 0x10: branch(c, !FL(P816_N)); break;
    case 0x30: branch(c, FL(P816_N)); break;
    case 0x50: branch(c, !FL(P816_V)); break;
    case 0x70: branch(c, FL(P816_V)); break;
    case 0x90: branch(c, !FL(P816_C)); break;
    case 0xB0: branch(c, FL(P816_C)); break;
    case 0xD0: branch(c, !FL(P816_Z)); break;
    case 0xF0: branch(c, FL(P816_Z)); break;
    case 0x80: branch(c, 1); break;
    case 0x82: { int16_t off = (int16_t)fetch16(c); c->pc = (uint16_t)(c->pc + off); break; }   /* BRL */
    case 0x4C: c->pc = fetch16(c); break;
    case 0x5C: { uint32_t t = fetch24(c); c->pc = (uint16_t)t; c->pb = (uint8_t)(t >> 16); break; }   /* JML long */
    case 0x6C: c->pc = rd16b0(c, fetch16(c)); break;                                                     /* JMP (abs) */
    case 0x7C: { uint16_t p = (uint16_t)(fetch16(c) + c->x); c->pc = rd16(c, ((uint32_t)c->pb << 16) | p); break; }   /* JMP (abs,x) */
    case 0xDC: { uint16_t p = fetch16(c); c->pc = rd16b0(c, p); c->pb = rd(c, (uint16_t)(p + 2)); break; }       /* JML [abs] */
    case 0x20: { uint16_t t = fetch16(c); push16(c, (uint16_t)(c->pc - 1)); c->pc = t; break; }
    case 0xFC: {   /* JSR (abs,x) */
        uint16_t p = fetch16(c);
        push16(c, (uint16_t)(c->pc - 1));
        c->pc = rd16(c, ((uint32_t)c->pb << 16) | (uint16_t)(p + c->x));
        break; }
    case 0x22: {   /* JSL */
        uint16_t lo = fetch16(c);
        uint8_t bank = fetch8(c);
        push8(c, c->pb);
        push16(c, (uint16_t)(c->pc - 1));
        c->pc = lo; c->pb = bank;
        break; }
    case 0x60: c->pc = (uint16_t)(pull16(c) + 1); break;
    case 0x6B: c->pc = (uint16_t)(pull16(c) + 1); c->pb = pull8(c); break;
    case 0x40:
        c->p = pull8(c);
        fix_widths(c);
        c->pc = pull16(c);
        if (!c->e) c->pb = pull8(c);
        break;
    /* block moves: the whole block at once */
    case 0x54: case 0x44: {
        uint8_t dst = fetch8(c), src = fetch8(c);
        c->db = dst;
        do {
            uint8_t v = rd(c, ((uint32_t)src << 16) | c->x);
            wr(c, ((uint32_t)dst << 16) | c->y, v);
            if (op == 0x54) { c->x++; c->y++; } else { c->x--; c->y--; }
            if (X8) { c->x &= 0xFF; c->y &= 0xFF; }
            c->a--;
            c->cycles += 7;
        } while (c->a != 0xFFFF);
        break; }
    /* the rest */
    case 0xEA: break;                    /* NOP */
    case 0x42: fetch8(c); break;         /* WDM */
    case 0x00: fetch8(c); interrupt(c, 0xFFE6, 0xFFFE, 1); c->trap = 1; break;   /* BRK */
    case 0x02: fetch8(c); interrupt(c, 0xFFE4, 0xFFF4, 0); c->trap = 1; break;   /* COP */
    case 0xCB: c->stopped = 1; break;    /* WAI */
    case 0xDB: c->stopped = 1; c->pc--; break;   /* STP */
    default: break;
    }
}

int cpu816_call(cpu816_t *c, uint32_t addr, int rts, int max_steps) {
    /* the return address points at a sentinel the routine never reaches by itself: the stack depth
       says when it has returned */
    uint16_t s0 = c->s;
    if (rts) { push16(c, 0xFFFF); }
    else { push8(c, 0xFF); push16(c, 0xFFFF); }
    c->pc = (uint16_t)addr;
    c->pb = (uint8_t)(addr >> 16);
    c->stopped = 0;
    c->trap = 0;
    for (int n = 0; n < max_steps; n++) {
        cpu816_step(c);
        if (c->stopped || c->trap) { c->s = s0; return -1; }
        if (c->s == s0 && c->pc == 0x0000 && (rts || c->pb == 0xFF)) return n + 1;
    }
    c->stopped = 1;
    c->s = s0;
    return -1;
}
