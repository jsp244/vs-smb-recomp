/*
 * mapper.c - NROM (0), MMC1 (1), MMC3/MMC6 (4) and Vs. System (99) support.
 *
 * MMC6 is a variant of the MMC3: 1 KiB of battery RAM with
 * per-half read/write enables instead of the MMC3's 8 KiB (kept for the
 * mapper test ROMs). The Vs. System board has fixed PRG, an 8 KiB CHR bank
 * chosen by bit 2 of the $4016 writes (see mem.c) and 2 KiB of work RAM at
 * $6000-$7FFF.
 */
#include "nes.h"
#include <stdio.h>

mapper_t mapper;

static uint8_t *prg_bank_ptr(uint32_t bank8k) {
    uint32_t count = cart.prg_size / 8192;
    return cart.prg + (bank8k % count) * 8192;
}

uint8_t *mapper_chr_bank_ptr(uint32_t bank1k) {
    if (cart.chr_size == 0) return cart.chr_ram + (bank1k % 8) * 1024;
    uint32_t count = cart.chr_size / 1024;
    return cart.chr + (bank1k % count) * 1024;
}

void mapper_set_mirroring(int mode) {
    static const uint16_t tables[5][4] = {
        { 0x000, 0x000, 0x400, 0x400 },   /* horizontal */
        { 0x000, 0x400, 0x000, 0x400 },   /* vertical */
        { 0x000, 0x000, 0x000, 0x000 },   /* single screen, lower */
        { 0x400, 0x400, 0x400, 0x400 },   /* single screen, upper */
        { 0x000, 0x400, 0x800, 0xC00 },   /* four screen */
    };
    for (int i = 0; i < 4; i++) ppu.nt_offset[i] = tables[mode][i];
}

void mapper_update_pointers(void) {
    uint32_t prg8k = cart.prg_size / 8192;
    mapper.chr_writable = (cart.chr_size == 0);
    switch (cart.mapper) {
    case 0: {
        mapper.prg_bank[0] = 0; mapper.prg_bank[1] = 1;
        mapper.prg_bank[2] = (uint16_t)((prg8k >= 4) ? 2 : 0);
        mapper.prg_bank[3] = (uint16_t)((prg8k >= 4) ? 3 : 1);
        for (int i = 0; i < 8; i++) mapper.chr_bank[i] = (uint16_t)i;
        break; }
    case 1: {
        uint32_t prg16k = cart.prg_size / 16384;
        uint8_t mode = (mapper.mmc1_ctrl >> 2) & 3;
        uint32_t bank = mapper.mmc1_prg & 0x0F;
        uint32_t lo, hi;
        if (mode <= 1) { lo = (bank & ~1u) % prg16k; hi = (lo + 1) % prg16k; }
        else if (mode == 2) { lo = 0; hi = bank % prg16k; }
        else { lo = bank % prg16k; hi = prg16k - 1; }
        mapper.prg_bank[0] = (uint16_t)(lo * 2); mapper.prg_bank[1] = (uint16_t)(lo * 2 + 1);
        mapper.prg_bank[2] = (uint16_t)(hi * 2); mapper.prg_bank[3] = (uint16_t)(hi * 2 + 1);
        if (mapper.mmc1_ctrl & 0x10) {
            for (int i = 0; i < 4; i++) mapper.chr_bank[i] = (uint16_t)(mapper.mmc1_chr0 * 4 + i);
            for (int i = 0; i < 4; i++) mapper.chr_bank[4 + i] = (uint16_t)(mapper.mmc1_chr1 * 4 + i);
        } else {
            for (int i = 0; i < 8; i++) mapper.chr_bank[i] = (uint16_t)((mapper.mmc1_chr0 & ~1) * 4 + i);
        }
        switch (mapper.mmc1_ctrl & 3) {
        case 0: mapper_set_mirroring(2); break;
        case 1: mapper_set_mirroring(3); break;
        case 2: mapper_set_mirroring(1); break;
        default: mapper_set_mirroring(0); break;
        }
        break; }
    case 4: {
        uint8_t *r = mapper.regs;
        bool prg_mode = (mapper.bank_select & 0x40) != 0;
        bool chr_mode = (mapper.bank_select & 0x80) != 0;
        uint16_t last = (uint16_t)(prg8k - 1), second_last = (uint16_t)(prg8k - 2);
        if (!prg_mode) {
            mapper.prg_bank[0] = r[6]; mapper.prg_bank[1] = r[7];
            mapper.prg_bank[2] = second_last; mapper.prg_bank[3] = last;
        } else {
            mapper.prg_bank[0] = second_last; mapper.prg_bank[1] = r[7];
            mapper.prg_bank[2] = r[6]; mapper.prg_bank[3] = last;
        }
        uint16_t c2k0 = (uint16_t)(r[0] & 0xFE), c2k1 = (uint16_t)(r[1] & 0xFE);
        if (!chr_mode) {
            mapper.chr_bank[0] = c2k0; mapper.chr_bank[1] = (uint16_t)(c2k0 + 1);
            mapper.chr_bank[2] = c2k1; mapper.chr_bank[3] = (uint16_t)(c2k1 + 1);
            mapper.chr_bank[4] = r[2]; mapper.chr_bank[5] = r[3];
            mapper.chr_bank[6] = r[4]; mapper.chr_bank[7] = r[5];
        } else {
            mapper.chr_bank[0] = r[2]; mapper.chr_bank[1] = r[3];
            mapper.chr_bank[2] = r[4]; mapper.chr_bank[3] = r[5];
            mapper.chr_bank[4] = c2k0; mapper.chr_bank[5] = (uint16_t)(c2k0 + 1);
            mapper.chr_bank[6] = c2k1; mapper.chr_bank[7] = (uint16_t)(c2k1 + 1);
        }
        if (cart.four_screen) mapper_set_mirroring(4);
        else mapper_set_mirroring((mapper.mirroring & 1) ? 0 : 1);
        break; }
    case 99: {
        /* 32 KiB of PRG fixed at $8000 (the extra bank of the 40 KiB boards is not supported) */
        for (int i = 0; i < 4; i++) mapper.prg_bank[i] = (uint16_t)(i % prg8k);
        uint16_t chr8k = (uint16_t)((mapper.vs_latch >> 2) & 1);
        for (int i = 0; i < 8; i++) mapper.chr_bank[i] = (uint16_t)(chr8k * 8 + i);
        break; }
    default:
        for (int i = 0; i < 4; i++) mapper.prg_bank[i] = (uint16_t)(i % prg8k);
        for (int i = 0; i < 8; i++) mapper.chr_bank[i] = (uint16_t)i;
        break;
    }
    for (int i = 0; i < 4; i++) mapper.prg_ptr[i] = prg_bank_ptr(mapper.prg_bank[i]);
    for (int i = 0; i < 8; i++) mapper.chr_ptr[i] = mapper_chr_bank_ptr(mapper.chr_bank[i]);
}

void mapper_init(void) {
    memset(&mapper, 0, sizeof mapper);
    mapper_reset();
}

void mapper_reset(void) {
    mapper.bank_select = 0;
    for (int i = 0; i < 8; i++) mapper.regs[i] = 0;
    mapper.regs[0] = 0; mapper.regs[1] = 2; mapper.regs[2] = 4; mapper.regs[3] = 5;
    mapper.regs[4] = 6; mapper.regs[5] = 7; mapper.regs[6] = 0; mapper.regs[7] = 1;
    mapper.mirroring = cart.mirroring ? 0 : 1;   /* $A000 bit0: 0 = vertical, 1 = horizontal */
    mapper.wram_ctrl = cart.mmc6 ? 0 : 0x80;   /* plain MMC3: RAM enabled at power-on for compatibility */
    mapper.irq_latch = mapper.irq_counter = 0;
    mapper.irq_reload = mapper.irq_enabled = mapper.irq_asserted = false;
    mapper.a12_low = false;
    mapper.a12_low_since = 0;
    mapper.mmc1_shift = 0; mapper.mmc1_count = 0;
    mapper.mmc1_ctrl = 0x0C; mapper.mmc1_chr0 = mapper.mmc1_chr1 = mapper.mmc1_prg = 0;
    mapper.vs_latch = 0;
    cpu.irq_line &= (uint8_t)~IRQ_MAPPER;
    if (cart.mapper == 0 || cart.mapper == 1 || cart.mapper == 99) mapper_set_mirroring(cart.four_screen ? 4 : (cart.mirroring ? 1 : 0));
    mapper_update_pointers();
}

/* ---- PRG RAM ---- */
static inline bool mmc6_ram_enabled(void) { return (mapper.bank_select & 0x20) != 0; }

uint8_t mapper_read_wram(uint16_t a) {
    if (a < 0x6000) return cpu.open_bus;
    if (cart.vs) return cart.wram[a & 0x7FF];    /* 2 KiB, mirrored through $7FFF */
    if (cart.mapper == 4 && cart.mmc6) {
        if (a < 0x7000 || !mmc6_ram_enabled()) return cpu.open_bus;
        uint16_t off = a & 0x3FF;
        bool half_hi = (off & 0x200) != 0;
        bool rd_lo = (mapper.wram_ctrl & 0x20) != 0, rd_hi = (mapper.wram_ctrl & 0x80) != 0;
        if (!rd_lo && !rd_hi) return cpu.open_bus;
        if (half_hi ? !rd_hi : !rd_lo) return 0;
        return cart.wram[off];
    }
    if (cart.mapper == 4) {
        if (!(mapper.wram_ctrl & 0x80)) return cpu.open_bus;
        return cart.wram[a & 0x1FFF];
    }
    if (cart.mapper == 1) {
        if (mapper.mmc1_prg & 0x10) return cpu.open_bus;
        return cart.wram[a & 0x1FFF];
    }
    return cart.wram[a & 0x1FFF];
}

void mapper_write_wram(uint16_t a, uint8_t v) {
    if (a < 0x6000) return;
    if (cart.vs) { cart.wram[a & 0x7FF] = v; return; }
    if (cart.mapper == 4 && cart.mmc6) {
        if (a < 0x7000 || !mmc6_ram_enabled()) return;
        uint16_t off = a & 0x3FF;
        bool half_hi = (off & 0x200) != 0;
        bool wr_lo = (mapper.wram_ctrl & 0x10) != 0, wr_hi = (mapper.wram_ctrl & 0x40) != 0;
        if (half_hi ? !wr_hi : !wr_lo) return;
        cart.wram[off] = v;
        return;
    }
    if (cart.mapper == 4) {
        if ((mapper.wram_ctrl & 0xC0) != 0x80) return;
        cart.wram[a & 0x1FFF] = v;
        return;
    }
    if (cart.mapper == 1) {
        if (mapper.mmc1_prg & 0x10) return;
        cart.wram[a & 0x1FFF] = v;
        return;
    }
    cart.wram[a & 0x1FFF] = v;
}

/* ---- mapper registers ---- */
static void mmc3_write(uint16_t a, uint8_t v) {
    if (sys.debug_log)
        fprintf((FILE *)sys.debug_log, "frame %llu sl %d dot %d: mapper write $%04X = $%02X (counter %d latch %d en %d)\n",
                (unsigned long long)ppu.frame, ppu.scanline, ppu.dot, a, v, mapper.irq_counter, mapper.irq_latch, mapper.irq_enabled);
    switch (a & 0xE001) {
    case 0x8000:
        mapper.bank_select = v;
        mapper_update_pointers();
        break;
    case 0x8001:
        mapper.regs[mapper.bank_select & 7] = v;
        mapper_update_pointers();
        break;
    case 0xA000:
        mapper.mirroring = v;
        mapper_update_pointers();
        break;
    case 0xA001:
        mapper.wram_ctrl = v;
        break;
    case 0xC000:
        mapper.irq_latch = v;
        break;
    case 0xC001:
        mapper.irq_reload = true;
        break;
    case 0xE000:
        mapper.irq_enabled = false;
        mapper.irq_asserted = false;
        cpu.irq_line &= (uint8_t)~IRQ_MAPPER;
        break;
    case 0xE001:
        mapper.irq_enabled = true;
        break;
    }
}

static void mmc1_write(uint16_t a, uint8_t v) {
    if (v & 0x80) {
        mapper.mmc1_shift = 0; mapper.mmc1_count = 0;
        mapper.mmc1_ctrl |= 0x0C;
        mapper_update_pointers();
        return;
    }
    mapper.mmc1_shift |= (uint8_t)((v & 1) << mapper.mmc1_count);
    mapper.mmc1_count++;
    if (mapper.mmc1_count == 5) {
        uint8_t val = mapper.mmc1_shift & 0x1F;
        switch ((a >> 13) & 3) {
        case 0: mapper.mmc1_ctrl = val; break;
        case 1: mapper.mmc1_chr0 = val; break;
        case 2: mapper.mmc1_chr1 = val; break;
        case 3: mapper.mmc1_prg = val; break;
        }
        mapper.mmc1_shift = 0; mapper.mmc1_count = 0;
        mapper_update_pointers();
    }
}

void mapper_write_rom(uint16_t a, uint8_t v) {
    switch (cart.mapper) {
    case 1: mmc1_write(a, v); break;
    case 4: mmc3_write(a, v); break;
    default: break;
    }
}

/* ---- MMC3 scanline counter: PPU A12 rising edge detection ---- */
static void mmc3_clock_irq(void) {
    sys.a12_clocks++;
    if (mapper.irq_counter == 0 || mapper.irq_reload) {
        mapper.irq_counter = mapper.irq_latch;
        mapper.irq_reload = false;
    } else {
        mapper.irq_counter--;
    }
    if (mapper.irq_counter == 0 && mapper.irq_enabled) {
        mapper.irq_asserted = true;
        cpu_raise_irq(IRQ_MAPPER, ppu.dots / 3);
    }
}

void mapper_ppu_addr(uint16_t addr) {
    if (cart.mapper != 4) return;
    if (addr & 0x1000) {
        if (mapper.a12_low) {
            if (ppu.dots - mapper.a12_low_since >= 10) mmc3_clock_irq();
            mapper.a12_low = false;
        }
    } else if (!mapper.a12_low) {
        mapper.a12_low = true;
        mapper.a12_low_since = ppu.dots;
    }
}
