/*
 * nes.h - shared hardware state for the VS. Super Mario Bros. native port runtime.
 *
 * Everything the recompiled game code and the fallback interpreter need
 * lives here: CPU registers, RAM, PPU, APU, mapper and cartridge state,
 * plus the inline helpers that implement 6502 instruction semantics so
 * that both execution paths behave identically.
 *
 * No game data is stored in this repository: the PRG/CHR ROM images are
 * loaded at run time from the user's own .nes file (see cart.c).
 */
#ifndef NES_H
#define NES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Cartridge                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t *prg;           /* PRG ROM image (owned) */
    uint32_t prg_size;
    uint8_t *chr;           /* CHR ROM image (owned), or NULL when CHR RAM */
    uint32_t chr_size;
    uint8_t  chr_ram[8192];
    uint8_t  wram[8192];    /* PRG RAM. MMC6 uses the first 1 KiB, the Vs. System the first 2 KiB. */
    int      mapper;
    int      submapper;
    bool     nes2;
    bool     battery;
    bool     mmc6;          /* MMC6 (StarTropics / Zoda's Revenge) semantics */
    bool     vs;            /* Vs. System: coin and DIP switch inputs, 2 KiB of work RAM at $6000 */
    int      vs_ppu;        /* NES 2.0 Vs. PPU type (5 = RP2C04-0004), -1 = a 2C02 */
    bool     four_screen;
    int      mirroring;     /* 0 = horizontal, 1 = vertical (header value) */
    uint8_t  sha1[20];      /* SHA-1 of the ROM image without the header */
    char     sha1_hex[41];
    uint8_t  md5[16];       /* MD5 of the same data, for display and cross-checking against databases */
    char     md5_hex[33];
    uint32_t crc32;         /* CRC32 of the ROM image without the header */
    char     path[1024];
} cart_t;

/* ------------------------------------------------------------------ */
/* CPU                                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  a, x, y, sp;
    uint16_t pc;
    uint8_t  c, z, i, d, v, n;   /* flags, each 0 or 1 */
    uint64_t cycles;             /* CPU cycles since power-on */
    uint8_t  nmi_pending;        /* latched NMI edge, cleared when taken */
    uint8_t  nmi_line;           /* current NMI input level */
    uint8_t  irq_line;           /* bitmask of active IRQ sources */
    uint8_t  jammed;
    uint8_t  open_bus;
    uint8_t  irq_delay_i;        /* I flag value to use for the next interrupt poll (CLI/SEI/PLP delay) */
    uint8_t  irq_delay_active;
    uint8_t  int_pending;        /* result of the last interrupt poll */
    uint64_t nmi_edge_cycle;     /* CPU cycle at which the NMI edge was detected */
    uint64_t irq_edge_cycle;     /* CPU cycle at which the IRQ line last rose */
} cpu_t;

#define IRQ_MAPPER     0x01
#define IRQ_APU_FRAME  0x02
#define IRQ_APU_DMC    0x04

/* ------------------------------------------------------------------ */
/* PPU                                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  ctrl, mask, status, oam_addr;
    uint16_t v, t;
    uint8_t  x, w;
    uint8_t  read_buffer;
    uint8_t  open_bus;
    uint8_t  oam[256];
    uint8_t  soam[256];         /* secondary OAM: 8 sprites, or up to 64 with ppu_no_sprite_limit */
    uint8_t  palette[32];
    uint8_t  vram[4096];
    uint16_t nt_offset[4];      /* nametable slot -> offset into vram */
    int      scanline;          /* 0..261 (261 = pre-render) */
    int      dot;               /* 0..340 */
    uint64_t dots;              /* total PPU dots since power-on */
    uint64_t frame;
    bool     odd_frame;
    bool     frame_done;        /* set when scanline 240 begins */
    bool     suppress_vbl;      /* $2002 read raced the flag: skip setting it this frame */
    /* background pipeline */
    uint16_t bg_shift_lo, bg_shift_hi;
    uint16_t at_shift_lo, at_shift_hi;
    uint8_t  nt_latch, at_latch, pt_lo_latch, pt_hi_latch;
    /* sprites for the line being rendered */
    uint8_t  spr_count;
    uint8_t  spr_pat_lo[64], spr_pat_hi[64], spr_attr[64], spr_x[64];
    bool     spr0_line;         /* sprite 0 is among this line's sprites */
    /* sprites evaluated for the next line */
    uint8_t  spr_count_next;
    bool     spr0_next;
    uint8_t  line_emph[240];    /* $2001 (mask) at the start of each visible line: emphasis, grayscale, enables */
    /* scroll (loopy t and fine x), control register and CHR banks at the start of the
       visible frame, for tools that re-render the frame background */
    uint16_t frame_t;
    uint8_t  frame_x, frame_ctrl;
    uint16_t frame_chr_bank[8];
    uint8_t  fb[256 * 240];     /* 6-bit palette index per pixel */
    /* where each visible line's background starts (v and fine x after the previous line's
       horizontal copy, with the line's vertical position) and $2000 then, for re-rendering the
       background beside the picture (widescreen.c). Like fb, rewritten every frame and left out
       of save states. */
    uint16_t line_v[240];
    uint8_t  line_fx[240];
    uint8_t  line_ctrl[240];
} ppu_t;

/* ------------------------------------------------------------------ */
/* APU                                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    bool     enabled;
    uint8_t  duty;
    bool     length_halt;       /* also envelope loop */
    bool     const_volume;
    uint8_t  volume;            /* envelope period / constant volume */
    bool     env_start;
    uint8_t  env_divider, env_decay;
    bool     sweep_enabled, sweep_negate, sweep_reload;
    uint8_t  sweep_period, sweep_shift, sweep_divider;
    uint16_t timer_period, timer;
    uint8_t  seq_pos;
    uint8_t  length;
} apu_pulse_t;

typedef struct {
    bool     enabled;
    bool     control;           /* length halt / linear control */
    uint8_t  linear_load, linear;
    bool     linear_reload;
    uint16_t timer_period, timer;
    uint8_t  seq_pos;
    uint8_t  length;
} apu_tri_t;

typedef struct {
    bool     enabled;
    bool     length_halt;
    bool     const_volume;
    uint8_t  volume;
    bool     env_start;
    uint8_t  env_divider, env_decay;
    bool     mode;
    uint16_t timer_period, timer;
    uint16_t shift;
    uint8_t  length;
} apu_noise_t;

typedef struct {
    bool     enabled;
    bool     irq_enable, loop;
    uint8_t  rate_index;
    uint16_t timer_period, timer;
    uint8_t  output;
    uint16_t sample_addr, sample_len;
    uint16_t cur_addr, bytes_remaining;
    uint8_t  sample_buffer;
    bool     sample_buffer_empty;
    uint8_t  shift, bits_remaining;
    bool     silence;
    bool     irq_flag;
} apu_dmc_t;

#define APU_RING_SIZE 65536

typedef struct {
    apu_pulse_t pulse[2];
    apu_tri_t   tri;
    apu_noise_t noise;
    apu_dmc_t   dmc;
    uint64_t cycles;            /* CPU cycles processed */
    bool     odd_cycle;
    /* frame counter */
    bool     mode5;
    bool     irq_inhibit;
    bool     frame_irq;
    uint32_t frame_counter;     /* CPU cycles into the frame sequence */
    int      frame_reset_delay; /* cycles until a $4017 write takes effect, 0 = none */
    uint8_t  frame_reset_value;
    /* output */
    uint32_t sample_rate;
    uint64_t sample_phase;      /* fixed point accumulator */
    double   acc;
    uint32_t acc_n;
    double   hp90_prev_in, hp90_prev_out;
    double   hp440_prev_in, hp440_prev_out;
    double   lp_prev;
    /* the sample ring itself lives outside the struct (apu.c) so save states and rewind
       snapshots stay small; the indices are state, so a restore discards later samples */
    uint32_t ring_write, ring_read;
    uint64_t samples_produced;
} apu_t;

/* ------------------------------------------------------------------ */
/* Mapper (NROM / MMC1 / MMC3 / MMC6)                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    /* MMC3 / MMC6 */
    uint8_t  bank_select;
    uint8_t  regs[8];
    uint8_t  mirroring;         /* $A000 value */
    uint8_t  wram_ctrl;         /* $A001 value */
    uint8_t  irq_latch, irq_counter;
    bool     irq_reload, irq_enabled;
    bool     irq_asserted;
    bool     a12_low;
    uint64_t a12_low_since;
    /* MMC1 */
    uint8_t  mmc1_shift, mmc1_count;
    uint8_t  mmc1_ctrl, mmc1_chr0, mmc1_chr1, mmc1_prg;
    /* resolved mapping */
    uint8_t *prg_ptr[4];        /* 8 KiB slots at $8000/$A000/$C000/$E000 */
    uint8_t *chr_ptr[8];        /* 1 KiB slots at $0000..$1C00 */
    uint16_t prg_bank[4];       /* 8 KiB bank index per slot */
    uint16_t chr_bank[8];       /* 1 KiB bank index per slot */
    bool     chr_writable;
    /* Vs. System (mapper 99): the last $4016 write; bit 2 selects the 8 KiB CHR bank */
    uint8_t  vs_latch;
} mapper_t;

/* ------------------------------------------------------------------ */
/* System glue                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  pad[2];            /* current button state: A B Sel Start U D L R = bits 0..7 */
    uint8_t  pad_shift[2];
    bool     strobe;
    /* Vs. System cabinet inputs. vs_coin is machine input like pad[] (bit 0 coin slot 1, bit 1
       coin slot 2, bit 2 service button); vs_dip is a setting, not machine state (bit k = DIP
       switch k+1, on = 1), read by the game when it starts */
    uint8_t  vs_coin;
    uint8_t  vs_dip;
    uint32_t vs_coin_meter;     /* pulses the game sent to the coin counter ($4020 bit 0) */
    bool     use_recomp;        /* run recompiled code (with interpreter fallback) */
    uint64_t fallback_steps;    /* instructions executed by the interpreter in recomp mode */
    uint64_t native_blocks;     /* routine entries executed natively */
    uint64_t instructions;
    bool     trace;             /* print a trace line per instruction (interpreter) */
    void   (*trace_hook)(void);
    /* called just before an instruction at hook_pc[] runs, by the interpreter and by the recompiled
       code (the instructions recomp/symbols.txt marks "hook"); it may look at the machine, never change it */
    void   (*hook)(uint16_t pc);
    uint16_t hook_pc[3];
    /* called just before an instruction whose bit is set in sys_patch_map runs (the instructions
       recomp/symbols.txt marks "patch"), by the interpreter, the recompiled code and the sandbox
       alike; unlike a hook it may change registers, flags and memory (a change to the game's rules
       that some setting asks for), so it must depend only on the machine and the settings */
    void   (*patch)(uint16_t pc);
    bool     sandbox;           /* cpu_sandbox_run is running: hardware writes are dropped, reads refused, no time passes */
    bool     sandbox_fault;     /* the sandboxed code tried to read the hardware */
    bool     coverage;
    uint8_t *cov;               /* per (bank, offset) executed flag, prg_size bytes, plus RAM region */
    void    *debug_log;         /* FILE*: when set, mapper/PPU control writes are logged */
    uint64_t a12_clocks;        /* MMC3 IRQ counter clocks observed (statistics) */
    uint64_t irq_count;         /* IRQs serviced */
} sys_t;

/* Global hardware state */
extern cpu_t    cpu;
extern ppu_t    ppu;
extern apu_t    apu;
extern mapper_t mapper;
extern cart_t   cart;
extern sys_t    sys;
extern uint8_t  ram[2048];
extern uint8_t  sys_patch_map[8192];   /* one bit per CPU address: sys.patch runs before that instruction */

/* ------------------------------------------------------------------ */
/* Function prototypes                                                 */
/* ------------------------------------------------------------------ */
/* cart.c */
int  cart_load(const char *path);
void cart_free(void);
int  cart_load_battery(const char *path);
int  cart_save_battery(const char *path);
const char *cart_known_name(void);   /* "VS. Super Mario Bros." etc. for known dumps, else NULL */
size_t cart_battery_size(void);       /* bytes of battery RAM for the loaded cartridge */
#include <stdio.h>
FILE *nes_fopen(const char *path, const char *mode);  /* UTF-8 path aware fopen */
void rom_sha1(const uint8_t *data, size_t len, uint8_t out[20]);   /* the hashes of the ROM checks */
void rom_md5(const uint8_t *data, size_t len, uint8_t out[16]);
uint32_t rom_crc32(const uint8_t *data, size_t len);

/* mapper.c */
void    mapper_init(void);
void    mapper_reset(void);
uint8_t mapper_read_wram(uint16_t a);
void    mapper_write_wram(uint16_t a, uint8_t v);
void    mapper_write_rom(uint16_t a, uint8_t v);
void    mapper_ppu_addr(uint16_t addr);
void    mapper_update_pointers(void);
void    mapper_set_mirroring(int mode);    /* 0=H 1=V 2=single lo 3=single hi 4=four */
uint8_t *mapper_chr_bank_ptr(uint32_t bank1k); /* data of a 1 KiB CHR bank (ROM or RAM) */

/* ppu.c */
void    ppu_reset(void);
void    ppu_run_to(uint64_t target_dots);
uint8_t ppu_reg_read(int reg);
void    ppu_reg_write(int reg, uint8_t v);
void    ppu_oam_dma(uint8_t page);
void    ppu_update_nmi(void);
extern bool ppu_no_sprite_limit;   /* option, not machine state: draw every sprite on a line instead of the hardware's 8 (no flicker) */

/* apu.c */
void    apu_reset(void);
void    apu_run_to(uint64_t target_cycles);
uint8_t apu_read_status(void);
void    apu_write(uint16_t a, uint8_t v);
uint32_t apu_samples_available(void);
uint32_t apu_read_samples(int16_t *out, uint32_t max);
void    apu_set_sample_rate(uint32_t hz);
void    apu_flush_samples(void);   /* drop the samples not yet read (after a state load that is not run-ahead) */
/* which channels are heard, asked after each catch-up (bit 0 square 1, 1 square 2, 2 triangle, 3 noise,
   4 DMC; NULL: all): output only, not machine state */
extern uint8_t (*apu_mask_hook)(void);

/* mem.c */
uint8_t mem_read_slow(uint16_t a);
void    mem_write_slow(uint16_t a, uint8_t v);
uint8_t nes_peek(uint16_t a);   /* side-effect-free read (RAM, PRG RAM, ROM; 0 for registers) for tools and achievements */
void    mem_sandbox_begin(void);/* the sandbox's PPU data port (cpu_sandbox_run) takes the PPU's state; it then keeps its own across runs */

/* cpu.c */
void    cpu_power(void);
void    cpu_reset(void);
void    cpu_step(void);          /* interpret one instruction (interrupts checked first) */
void    cpu_take_interrupt(void);/* service a pending NMI/IRQ if any */
int     cpu_interrupt_pending(void);
/* Runs game code from regs->pc with private copies of RAM (sram) and work RAM (swram, 2 KiB each)
   until the program counter reaches stop, leaving the machine as it was: writes to the PPU, APU,
   controller port and mapper are dropped, a read of one of them ends the run (its value would
   come from the machine) except the PPU's data port, which reads CHR ROM and the nametables through
   a latch and CHR bank of the sandbox's own (mem.c); no time passes. sram, swram and regs hold the
   result. Returns the number of instructions run, or -1 when the code read the hardware or did not
   reach stop within max_insns. */
int     cpu_sandbox_run(cpu_t *regs, uint16_t stop, uint8_t *sram, uint8_t *swram, int max_insns);

/* sys.c */
int     nes_load(const char *rom_path);
void    nes_reset(void);
void    nes_power(void);
void    nes_run_frame(void);
void    nes_set_pad(int pad, uint8_t buttons);
void    nes_set_vs_coin(uint8_t bits);   /* Vs. System coin slots / service button, see sys_t */
size_t  nes_state_size(void);
int     nes_state_save(void *buf, size_t size);
int     nes_state_load(const void *buf, size_t size);

/* recompiled code entry (generated/ or stubs) */
int     recomp_run(void);        /* run native code from cpu.pc; returns 0 if no native routine for cpu.pc */
extern const char *recomp_rom_sha1; /* sha1 the native code was generated from, or NULL */

/* ------------------------------------------------------------------ */
/* Inline memory access                                                */
/* ------------------------------------------------------------------ */
static inline uint8_t mem_read(uint16_t a) {
    if (a < 0x2000) return ram[a & 0x7ff];
    if (a >= 0x8000) return mapper.prg_ptr[(a >> 13) & 3][a & 0x1fff];
    return mem_read_slow(a);
}

static inline void mem_write(uint16_t a, uint8_t v) {
    if (a < 0x2000) { ram[a & 0x7ff] = v; return; }
    mem_write_slow(a, v);
}

static inline uint16_t mem_read16(uint16_t a) {
    return (uint16_t)(mem_read(a) | (mem_read((uint16_t)(a + 1)) << 8));
}

/* zero-page pointer read with wraparound inside page zero */
static inline uint16_t zp_read16(uint8_t a) {
    return (uint16_t)(ram[a] | (ram[(uint8_t)(a + 1)] << 8));
}

/* ------------------------------------------------------------------ */
/* Synchronisation: advance PPU/APU to the CPU clock and poll          */
/* interrupts. Returns nonzero when an interrupt must be serviced.     */
/* ------------------------------------------------------------------ */
static inline int sys_sync(void) {
    ppu_run_to(cpu.cycles * 3);
    apu_run_to(cpu.cycles);
    uint8_t i = cpu.i;
    if (cpu.irq_delay_active) { i = cpu.irq_delay_i; cpu.irq_delay_active = 0; }
    /* an interrupt asserted during the final cycle of an instruction is only seen by the next one */
    uint64_t last = cpu.cycles - 1;
    cpu.int_pending = (cpu.nmi_pending && cpu.nmi_edge_cycle < last) ||
                      (cpu.irq_line && !i && cpu.irq_edge_cycle < last);
    return cpu.int_pending | ppu.frame_done;
}

static inline void cpu_raise_irq(uint8_t mask, uint64_t cycle) {
    if (!cpu.irq_line) cpu.irq_edge_cycle = cycle;
    cpu.irq_line |= mask;
}

/* ------------------------------------------------------------------ */
/* Flag helpers and ALU semantics shared by interpreter + native code  */
/* ------------------------------------------------------------------ */
static inline void set_nz(uint8_t v) { cpu.z = (v == 0); cpu.n = v >> 7; }

static inline uint8_t cpu_get_p(int b) {
    return (uint8_t)((cpu.n << 7) | (cpu.v << 6) | 0x20 | (b << 4) | (cpu.d << 3) | (cpu.i << 2) | (cpu.z << 1) | cpu.c);
}

static inline void cpu_set_p(uint8_t p) {
    cpu.n = (p >> 7) & 1; cpu.v = (p >> 6) & 1; cpu.d = (p >> 3) & 1;
    cpu.i = (p >> 2) & 1; cpu.z = (p >> 1) & 1; cpu.c = p & 1;
}

static inline void push8(uint8_t v) { ram[0x100 | cpu.sp] = v; cpu.sp--; }
static inline uint8_t pop8(void) { cpu.sp++; return ram[0x100 | cpu.sp]; }
static inline void push16(uint16_t v) { push8((uint8_t)(v >> 8)); push8((uint8_t)v); }
static inline uint16_t pop16(void) { uint8_t lo = pop8(); uint8_t hi = pop8(); return (uint16_t)(lo | (hi << 8)); }

static inline void op_adc(uint8_t m) {
    unsigned sum = cpu.a + m + cpu.c;
    cpu.v = ((~(cpu.a ^ m) & (cpu.a ^ sum)) >> 7) & 1;
    cpu.c = sum >> 8;
    cpu.a = (uint8_t)sum;
    set_nz(cpu.a);
}
static inline void op_sbc(uint8_t m) { op_adc((uint8_t)~m); }
static inline void op_cmp(uint8_t r, uint8_t m) { unsigned d = r - m; cpu.c = (r >= m); set_nz((uint8_t)d); }
static inline void op_bit(uint8_t m) { cpu.z = ((cpu.a & m) == 0); cpu.n = m >> 7; cpu.v = (m >> 6) & 1; }
static inline uint8_t op_asl(uint8_t v) { cpu.c = v >> 7; v = (uint8_t)(v << 1); set_nz(v); return v; }
static inline uint8_t op_lsr(uint8_t v) { cpu.c = v & 1; v >>= 1; set_nz(v); return v; }
static inline uint8_t op_rol(uint8_t v) { uint8_t c = cpu.c; cpu.c = v >> 7; v = (uint8_t)((v << 1) | c); set_nz(v); return v; }
static inline uint8_t op_ror(uint8_t v) { uint8_t c = cpu.c; cpu.c = v & 1; v = (uint8_t)((v >> 1) | (c << 7)); set_nz(v); return v; }
static inline uint8_t op_inc(uint8_t v) { v++; set_nz(v); return v; }
static inline uint8_t op_dec(uint8_t v) { v--; set_nz(v); return v; }
/* unofficial */
static inline void op_anc(uint8_t m) { cpu.a &= m; set_nz(cpu.a); cpu.c = cpu.n; }
static inline void op_alr(uint8_t m) { cpu.a &= m; cpu.a = op_lsr(cpu.a); }
static inline void op_arr(uint8_t m) {
    cpu.a &= m;
    cpu.a = (uint8_t)((cpu.a >> 1) | (cpu.c << 7));
    set_nz(cpu.a);
    cpu.c = (cpu.a >> 6) & 1;
    cpu.v = ((cpu.a >> 6) ^ (cpu.a >> 5)) & 1;
}
static inline void op_axs(uint8_t m) { unsigned t = (cpu.a & cpu.x); cpu.c = (t >= m); cpu.x = (uint8_t)(t - m); set_nz(cpu.x); }
static inline void op_xaa(uint8_t m) { cpu.a = (uint8_t)((cpu.a | 0xEE) & cpu.x & m); set_nz(cpu.a); }
static inline void op_lxa(uint8_t m) { cpu.a = cpu.x = m; set_nz(cpu.a); }
/* combined read-modify-write unofficial ops */
static inline uint8_t fn_slo(uint8_t v) { v = op_asl(v); cpu.a |= v; set_nz(cpu.a); return v; }
static inline uint8_t fn_rla(uint8_t v) { v = op_rol(v); cpu.a &= v; set_nz(cpu.a); return v; }
static inline uint8_t fn_sre(uint8_t v) { v = op_lsr(v); cpu.a ^= v; set_nz(cpu.a); return v; }
static inline uint8_t fn_rra(uint8_t v) { v = op_ror(v); op_adc(v); return v; }
static inline uint8_t fn_dcp(uint8_t v) { v--; op_cmp(cpu.a, v); return v; }
static inline uint8_t fn_isc(uint8_t v) { v++; op_sbc(v); return v; }
/* SHA/SHX/SHY/TAS: store (value & (high byte + 1)) with the page-crossing address corruption */
static inline void op_unstable_store(uint16_t b, uint8_t idx, uint8_t val) {
    uint16_t a = (uint16_t)(b + idx);
    uint8_t v = (uint8_t)(val & (uint8_t)((b >> 8) + 1));
    if ((b ^ a) & 0x100) a = (uint16_t)((v << 8) | (a & 0xff));
    mem_write(a, v);
}
static inline void op_las(uint8_t m) { cpu.a = cpu.x = cpu.sp = (uint8_t)(m & cpu.sp); set_nz(cpu.a); }

/* Interrupt entry sequence shared by interpreter and dispatcher */
static inline void cpu_do_interrupt(uint16_t vector, int brk) {
    push16(cpu.pc);
    push8(cpu_get_p(brk));
    cpu.i = 1;
    cpu.pc = mem_read16(vector);
    cpu.cycles += 7;
}

#ifdef __cplusplus
}
#endif
#endif /* NES_H */
