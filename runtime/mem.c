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

uint8_t mem_read_slow(uint16_t a) {
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
