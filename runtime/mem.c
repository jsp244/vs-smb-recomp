/*
 * mem.c - CPU bus: PPU/APU registers, controllers, OAM DMA, PRG RAM,
 * mapper registers. RAM and PRG ROM fast paths live inline in nes.h.
 */
#include "nes.h"

sys_t sys;

static uint8_t pad_bit(int n) {
    uint8_t bit;
    if (sys.strobe) {
        bit = sys.pad[n] & 1;
    } else {
        bit = sys.pad_shift[n] & 1;
        sys.pad_shift[n] = (uint8_t)((sys.pad_shift[n] >> 1) | 0x80);
    }
    return bit;
}

static uint8_t pad_read(int n) {
    uint8_t bit = pad_bit(n);
    if (!cart.vs) return (uint8_t)(0x40 | bit);   /* the upper bits are open bus on a console */
    /* Vs. System: $4016 = serial data, service button (bit 2), DIP 1-2 (3-4), coin slots 1-2 (5-6)
       and 0 in bit 7 (the main CPU of a DualSystem); $4017 = serial data, DIP 3-8 (bits 2-7) */
    if (n == 0)
        return (uint8_t)(bit | (sys.vs_coin & 4) | ((sys.vs_dip & 3) << 3) | ((sys.vs_coin & 3) << 5));
    return (uint8_t)(bit | (sys.vs_dip & 0xFC));
}

/* cpu_sandbox_run's own view of the PPU's data port: VS. Super Mario Bros. reads its level data out of
   CHR ROM through $2006 / $2007 when an area starts. The address latch, read buffer, increment and the
   board's CHR bank are the sandbox's; CHR ROM and the nametables are only read. Any other register
   read ends the run (sys.sandbox_fault); writes go nowhere. */
static struct { uint16_t addr; uint8_t w, buf, ctrl, latch; } sbp;

void mem_sandbox_begin(void) {
    sbp.addr = ppu.v & 0x3FFF; sbp.w = 0; sbp.buf = ppu.read_buffer; sbp.ctrl = ppu.ctrl; sbp.latch = mapper.vs_latch;
}

static uint8_t sandbox_ppu_peek(uint16_t a) {
    a &= 0x3FFF;
    if (a < 0x2000) {
        uint32_t bank = cart.mapper == 99 ? (uint32_t)((sbp.latch >> 2) & 1) * 8 + (a >> 10) : mapper.chr_bank[a >> 10];
        return mapper_chr_bank_ptr(bank)[a & 0x3FF];
    }
    if (a < 0x3F00) { a = (uint16_t)((a - 0x2000) & 0x0FFF); return ppu.vram[ppu.nt_offset[a >> 10] + (a & 0x3FF)]; }
    return ppu.palette[a & 0x1F];
}

static uint8_t sandbox_read(uint16_t a) {
    if (a >= 0x2000 && a < 0x4000) {
        int reg = a & 7;
        if (reg == 2) { sbp.w = 0; return 0; }
        if (reg == 7) {
            uint8_t v = sbp.buf;
            sbp.buf = sandbox_ppu_peek(sbp.addr);
            sbp.addr = (uint16_t)((sbp.addr + ((sbp.ctrl & 4) ? 32 : 1)) & 0x3FFF);
            return v;
        }
    }
    sys.sandbox_fault = true;
    return 0;
}

static void sandbox_write(uint16_t a, uint8_t v) {
    if (a >= 0x2000 && a < 0x4000) {
        int reg = a & 7;
        if (reg == 0) sbp.ctrl = v;
        else if (reg == 5) sbp.w ^= 1;
        else if (reg == 6) {
            sbp.addr = sbp.w ? (uint16_t)((sbp.addr & 0xFF00) | v) : (uint16_t)((sbp.addr & 0x00FF) | ((v & 0x3F) << 8));
            sbp.w ^= 1;
        }
    } else if (a == 0x4016) {
        sbp.latch = v;   /* the Vs. board's CHR bank (bit 2), for the reads above */
    }
}

uint8_t mem_read_slow(uint16_t a) {
    if (sys.sandbox && a < 0x6000) return sandbox_read(a);   /* see cpu_sandbox_run */
    if (a < 0x4000) {
        return ppu_reg_read(a & 7);
    }
    if (a < 0x4020) {
        switch (a) {
        case 0x4015: return apu_read_status();
        case 0x4016: return pad_read(0);
        case 0x4017: return pad_read(1);
        default:     return cpu.open_bus;
        }
    }
    if (a < 0x8000) return mapper_read_wram(a);
    return mapper.prg_ptr[(a >> 13) & 3][a & 0x1fff];
}

/* read without side effects: no register access, no open-bus update */
uint8_t nes_peek(uint16_t a) {
    if (a < 0x2000) return ram[a & 0x7ff];
    if (a >= 0x8000) return mapper.prg_ptr[(a >> 13) & 3][a & 0x1fff];
    if (a >= 0x6000) {
        if (cart.vs) return cart.wram[a & 0x7ff];
        if (cart.mapper == 4 && cart.mmc6) return (a >= 0x7000) ? cart.wram[a & 0x3ff] : 0;
        return cart.wram[a & 0x1fff];
    }
    return 0;
}

void mem_write_slow(uint16_t a, uint8_t v) {
    if (a < 0x2000) { ram[a & 0x7ff] = v; return; }
    if (sys.sandbox && (a < 0x6000 || a >= 0x8000)) { sandbox_write(a, v); return; }
    cpu.open_bus = v;
    if (a < 0x4000) { ppu_reg_write(a & 7, v); return; }
    if (a < 0x4020) {
        if (a == 0x4014) { ppu_oam_dma(v); return; }
        if (a == 0x4016) {
            bool s = (v & 1) != 0;
            if (sys.strobe && !s) {
                sys.pad_shift[0] = sys.pad[0];
                sys.pad_shift[1] = sys.pad[1];
            }
            sys.strobe = s;
            if (s) { sys.pad_shift[0] = sys.pad[0]; sys.pad_shift[1] = sys.pad[1]; }
            if (cart.mapper == 99) {   /* bit 2 selects the CHR bank */
                uint8_t old = mapper.vs_latch;
                mapper.vs_latch = v;
                if ((old ^ v) & 0x04) mapper_update_pointers();
            }
            return;
        }
        apu_write(a, v);
        return;
    }
    if (a == 0x4020 && cart.vs) {   /* the coin counter: one pulse per coin (bit 0 high, then low) */
        if ((v & 1) && !(sys.vs_coin_meter & 0x80000000u)) sys.vs_coin_meter = (sys.vs_coin_meter + 1) | 0x80000000u;
        else if (!(v & 1)) sys.vs_coin_meter &= 0x7FFFFFFFu;
        return;
    }
    if (a < 0x8000) { mapper_write_wram(a, v); return; }
    mapper_write_rom(a, v);
}
