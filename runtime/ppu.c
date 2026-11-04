/*
 * ppu.c - dot-based NTSC PPU.
 *
 * Runs in catch-up mode: the CPU advances its cycle counter and calls
 * ppu_run_to(), which steps the PPU dot by dot. Every pattern/nametable
 * fetch is reported to the mapper so MMC3/MMC6 scanline IRQs come from
 * real A12 edges rather than a scanline heuristic.
 */
#include "nes.h"
#include <stdio.h>

ppu_t ppu;
bool ppu_no_sprite_limit;

static inline bool rendering_enabled(void) { return (ppu.mask & 0x18) != 0; }

static inline int pal_index(uint16_t a) {
    a &= 0x1F;
    if ((a & 0x13) == 0x10) a &= 0x0F;
    return a;
}

static inline uint8_t ppu_mem_read(uint16_t a) {
    a &= 0x3FFF;
    if (a < 0x2000) return mapper.chr_ptr[a >> 10][a & 0x3FF];
    if (a < 0x3F00) return ppu.vram[ppu.nt_offset[(a >> 10) & 3] + (a & 0x3FF)];
    return ppu.palette[pal_index(a)];
}

static inline void ppu_mem_write(uint16_t a, uint8_t v) {
    a &= 0x3FFF;
    if (a < 0x2000) { if (mapper.chr_writable) mapper.chr_ptr[a >> 10][a & 0x3FF] = v; return; }
    if (a < 0x3F00) { ppu.vram[ppu.nt_offset[(a >> 10) & 3] + (a & 0x3FF)] = v; return; }
    ppu.palette[pal_index(a)] = v & 0x3F;
}

/* a fetch performed by the rendering pipeline: visible on the PPU address bus */
static inline uint8_t fetch(uint16_t a) {
    mapper_ppu_addr(a);
    return ppu_mem_read(a);
}

void ppu_update_nmi(void) {
    uint8_t level = ((ppu.status & 0x80) && (ppu.ctrl & 0x80)) ? 1 : 0;
    if (level && !cpu.nmi_line) { cpu.nmi_pending = 1; cpu.nmi_edge_cycle = ppu.dots / 3; }
    cpu.nmi_line = level;
}

void ppu_reset(void) {
    memset(&ppu, 0, sizeof ppu);
    ppu.scanline = 0;
    ppu.dot = 0;
    ppu.odd_frame = false;
    mapper_set_mirroring(cart.four_screen ? 4 : (cart.mirroring ? 1 : 0));
}

/* ---- scroll helpers (loopy) ---- */
static inline void inc_x(void) {
    if ((ppu.v & 0x001F) == 31) { ppu.v &= ~0x001F; ppu.v ^= 0x0400; }
    else ppu.v++;
}
static inline void inc_y(void) {
    if ((ppu.v & 0x7000) != 0x7000) { ppu.v += 0x1000; return; }
    ppu.v &= ~0x7000;
    int y = (ppu.v & 0x03E0) >> 5;
    if (y == 29) { y = 0; ppu.v ^= 0x0800; }
    else if (y == 31) { y = 0; }
    else y++;
    ppu.v = (uint16_t)((ppu.v & ~0x03E0) | (y << 5));
}
static inline void copy_x(void) { ppu.v = (uint16_t)((ppu.v & ~0x041F) | (ppu.t & 0x041F)); }
static inline void copy_y(void) { ppu.v = (uint16_t)((ppu.v & ~0x7BE0) | (ppu.t & 0x7BE0)); }

static inline void reload_shifters(void) {
    ppu.bg_shift_lo = (uint16_t)((ppu.bg_shift_lo & 0xFF00) | ppu.pt_lo_latch);
    ppu.bg_shift_hi = (uint16_t)((ppu.bg_shift_hi & 0xFF00) | ppu.pt_hi_latch);
    ppu.at_shift_lo = (uint16_t)((ppu.at_shift_lo & 0xFF00) | ((ppu.at_latch & 1) ? 0xFF : 0x00));
    ppu.at_shift_hi = (uint16_t)((ppu.at_shift_hi & 0xFF00) | ((ppu.at_latch & 2) ? 0xFF : 0x00));
}

static inline void shift_bg(void) {
    ppu.bg_shift_lo <<= 1; ppu.bg_shift_hi <<= 1;
    ppu.at_shift_lo <<= 1; ppu.at_shift_hi <<= 1;
}

/* background fetch step for dots 1..256 and 321..336 (phase = dot & 7) */
static inline void bg_fetch_step(int dot) {
    switch (dot & 7) {
    case 2:
        ppu.nt_latch = fetch((uint16_t)(0x2000 | (ppu.v & 0x0FFF)));
        break;
    case 4: {
        uint8_t at = fetch((uint16_t)(0x23C0 | (ppu.v & 0x0C00) | ((ppu.v >> 4) & 0x38) | ((ppu.v >> 2) & 0x07)));
        if (ppu.v & 0x40) at >>= 4;
        if (ppu.v & 0x02) at >>= 2;
        ppu.at_latch = at & 3;
        break; }
    case 6: {
        uint16_t a = (uint16_t)(((ppu.ctrl & 0x10) ? 0x1000 : 0) + ppu.nt_latch * 16 + ((ppu.v >> 12) & 7));
        ppu.pt_lo_latch = fetch(a);
        break; }
    case 0: {
        uint16_t a = (uint16_t)(((ppu.ctrl & 0x10) ? 0x1000 : 0) + ppu.nt_latch * 16 + ((ppu.v >> 12) & 7) + 8);
        ppu.pt_hi_latch = fetch(a);
        if (dot == 256) inc_y();
        inc_x();
        break; }
    default: break;
    }
}

static inline uint8_t reverse8(uint8_t b) {
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

/* evaluate sprites for the next scanline (simplified, done at dot 257) */
static void sprite_evaluate(int line) {
    int h = (ppu.ctrl & 0x20) ? 16 : 8;
    int limit = ppu_no_sprite_limit ? 64 : 8;
    int n = 0;
    ppu.spr0_next = false;
    memset(ppu.soam, 0xFF, sizeof ppu.soam);
    for (int i = 0; i < 64; i++) {
        int y = ppu.oam[i * 4];
        int row = line - y;
        if (row < 0 || row >= h) continue;
        if (n == 8) ppu.status |= 0x20;        /* the overflow flag behaves as on hardware either way */
        if (n >= limit) break;
        memcpy(ppu.soam + n * 4, ppu.oam + i * 4, 4);
        if (i == 0) ppu.spr0_next = true;
        n++;
    }
    ppu.spr_count_next = (uint8_t)n;
}

/* sprite pattern address for unit i on the evaluation line */
static inline uint16_t sprite_pattern_addr(int i, int line, int hi) {
    uint8_t y = ppu.soam[i * 4], tile = ppu.soam[i * 4 + 1], attr = ppu.soam[i * 4 + 2];
    uint16_t table;
    int row;
    if (i >= ppu.spr_count_next) { y = 0xFF; tile = 0xFF; attr = 0; row = 0; }
    else row = line - y;
    if (ppu.ctrl & 0x20) {
        table = (tile & 1) ? 0x1000 : 0x0000;
        tile &= 0xFE;
        if (attr & 0x80) row = 15 - row;
        if (row >= 8) { tile++; row -= 8; }
    } else {
        table = (ppu.ctrl & 0x08) ? 0x1000 : 0x0000;
        if (attr & 0x80) row = 7 - row;
    }
    return (uint16_t)(table + tile * 16 + (row & 7) + (hi ? 8 : 0));
}

static inline void sprite_fetch_step(int dot, int line) {
    int i = (dot - 257) >> 3;
    switch ((dot - 257) & 7) {
    case 1: fetch((uint16_t)(0x2000 | (ppu.v & 0x0FFF))); break;      /* garbage NT */
    case 3: fetch((uint16_t)(0x23C0 | (ppu.v & 0x0C00) | ((ppu.v >> 4) & 0x38) | ((ppu.v >> 2) & 0x07))); break;
    case 5: {
        uint8_t v = fetch(sprite_pattern_addr(i, line, 0));
        uint8_t attr = ppu.soam[i * 4 + 2];
        if (attr & 0x40) v = reverse8(v);
        ppu.spr_pat_lo[i] = (i < ppu.spr_count_next) ? v : 0;
        break; }
    case 7: {
        uint8_t v = fetch(sprite_pattern_addr(i, line, 1));
        uint8_t attr = ppu.soam[i * 4 + 2];
        if (attr & 0x40) v = reverse8(v);
        ppu.spr_pat_hi[i] = (i < ppu.spr_count_next) ? v : 0;
        ppu.spr_attr[i] = attr;
        ppu.spr_x[i] = ppu.soam[i * 4 + 3];
        break; }
    default: break;
    }
}

/* no-sprite-limit mode: sprites 9..64 of the line are loaded without bus activity
   (the hardware never fetches them, so the mapper must not see these reads) */
static void load_extra_sprites(int line) {
    for (int i = 8; i < ppu.spr_count_next; i++) {
        uint8_t attr = ppu.soam[i * 4 + 2];
        uint8_t lo = ppu_mem_read(sprite_pattern_addr(i, line, 0));
        uint8_t hi = ppu_mem_read(sprite_pattern_addr(i, line, 1));
        if (attr & 0x40) { lo = reverse8(lo); hi = reverse8(hi); }
        ppu.spr_pat_lo[i] = lo; ppu.spr_pat_hi[i] = hi;
        ppu.spr_attr[i] = attr;
        ppu.spr_x[i] = ppu.soam[i * 4 + 3];
    }
}

static inline void render_pixel(int sl, int dot) {
    int x = dot - 1;
    uint8_t bg_pixel = 0, bg_pal = 0;
    if ((ppu.mask & 0x08) && (x >= 8 || (ppu.mask & 0x02))) {
        int bit = 15 - ppu.x;
        bg_pixel = (uint8_t)(((ppu.bg_shift_lo >> bit) & 1) | (((ppu.bg_shift_hi >> bit) & 1) << 1));
        bg_pal = (uint8_t)(((ppu.at_shift_lo >> bit) & 1) | (((ppu.at_shift_hi >> bit) & 1) << 1));
    }
    uint8_t spr_pixel = 0, spr_pal = 0;
    bool spr_behind = false, is_spr0 = false;
    for (int i = 0; i < ppu.spr_count; i++) {
        if (ppu.spr_x[i] > 0) { ppu.spr_x[i]--; continue; }
        uint8_t p = (uint8_t)((ppu.spr_pat_lo[i] >> 7) | ((ppu.spr_pat_hi[i] >> 7) << 1));
        ppu.spr_pat_lo[i] <<= 1; ppu.spr_pat_hi[i] <<= 1;
        if (p && !spr_pixel) {
            spr_pixel = p;
            spr_pal = ppu.spr_attr[i] & 3;
            spr_behind = (ppu.spr_attr[i] & 0x20) != 0;
            is_spr0 = (i == 0 && ppu.spr0_line);
        }
    }
    if (!(ppu.mask & 0x10) || (x < 8 && !(ppu.mask & 0x04))) spr_pixel = 0;

    if (is_spr0 && spr_pixel && bg_pixel && x != 255 && (ppu.mask & 0x18) == 0x18)
        ppu.status |= 0x40;

    uint8_t color;
    if (spr_pixel && (bg_pixel == 0 || !spr_behind)) color = ppu.palette[0x10 + spr_pal * 4 + spr_pixel];
    else if (bg_pixel) color = ppu.palette[bg_pal * 4 + bg_pixel];
    else color = ppu.palette[0];
    if (ppu.mask & 1) color &= 0x30;
    ppu.fb[sl * 256 + x] = color;
}

static inline void ppu_tick(void) {
    int sl = ppu.scanline, dot = ppu.dot;
    bool render = rendering_enabled();
    bool visible = sl < 240;
    bool prerender = sl == 261;

    if (visible || prerender) {
        if (prerender && dot == 1) {
            ppu.status &= 0x1F;
            ppu_update_nmi();
        }
        if (visible && dot == 1) {
            ppu.line_emph[sl] = ppu.mask;
            if (sl == 0) {   /* what this frame's background is drawn with */
                /* t, not v: v has already been advanced by the two prefetched tiles */
                ppu.frame_t = ppu.t; ppu.frame_x = ppu.x; ppu.frame_ctrl = ppu.ctrl;
                memcpy(ppu.frame_chr_bank, mapper.chr_bank, sizeof ppu.frame_chr_bank);
            }
        }

        if (render) {
            if (dot >= 1 && dot <= 256) {
                if (visible) render_pixel(sl, dot);
                else {
                    /* keep sprite x counters moving on the pre-render line too */
                    for (int i = 0; i < ppu.spr_count; i++) if (ppu.spr_x[i] > 0) ppu.spr_x[i]--;
                }
                bg_fetch_step(dot);
                shift_bg();
                if ((dot & 7) == 0) reload_shifters();
            } else if (dot == 257) {
                copy_x();
                if (visible) sprite_evaluate(sl);
                else { ppu.spr_count_next = 0; ppu.spr0_next = false; memset(ppu.soam, 0xFF, sizeof ppu.soam); }
                sprite_fetch_step(dot, sl);
            } else if (dot >= 258 && dot <= 320) {
                ppu.oam_addr = 0;
                if (prerender && dot >= 280 && dot <= 304) copy_y();
                sprite_fetch_step(dot, sl);
                if (dot == 320) {
                    if (ppu.spr_count_next > 8) load_extra_sprites(sl);
                    ppu.spr_count = ppu.spr_count_next; ppu.spr0_line = ppu.spr0_next;
                    /* the next line's background starts here: v before the two prefetched tiles */
                    int next = prerender ? 0 : sl + 1;
                    if (next < 240) { ppu.line_v[next] = ppu.v; ppu.line_fx[next] = ppu.x; ppu.line_ctrl[next] = ppu.ctrl; }
                }
            } else if (dot >= 321 && dot <= 336) {
                bg_fetch_step(dot);
                shift_bg();
                if ((dot & 7) == 0) reload_shifters();
            } else if (dot == 338 || dot == 340) {
                fetch((uint16_t)(0x2000 | (ppu.v & 0x0FFF)));
            }
        } else if (visible && dot >= 1 && dot <= 256) {
            /* rendering disabled: output the backdrop (or the palette entry v points at) */
            uint8_t color = ((ppu.v & 0x3F00) == 0x3F00) ? ppu.palette[pal_index(ppu.v)] : ppu.palette[0];
            if (ppu.mask & 1) color &= 0x30;
            ppu.fb[sl * 256 + (dot - 1)] = color;
        }
    } else if (sl == 241 && dot == 1) {
        if (!ppu.suppress_vbl) { ppu.status |= 0x80; ppu_update_nmi(); }
        ppu.suppress_vbl = false;
    } else if (sl == 240 && dot == 0) {
        ppu.frame_done = true;
    }

    /* advance */
    if (prerender && dot == 339 && ppu.odd_frame && render) {
        ppu.dot = 0; ppu.scanline = 0; ppu.frame++; ppu.odd_frame = !ppu.odd_frame;
        return;
    }
    if (++ppu.dot > 340) {
        ppu.dot = 0;
        if (++ppu.scanline > 261) { ppu.scanline = 0; ppu.frame++; ppu.odd_frame = !ppu.odd_frame; }
    }
}

void ppu_run_to(uint64_t target) {
    while (ppu.dots < target) {
        ppu_tick();
        ppu.dots++;
    }
}

/* ---- CPU-visible registers ---- */
uint8_t ppu_reg_read(int reg) {
    ppu_run_to(cpu.cycles * 3);
    uint8_t r = ppu.open_bus;
    switch (reg) {
    case 2:
        r = (uint8_t)((ppu.status & 0xE0) | (ppu.open_bus & 0x1F));
        /* reading one dot before the flag is set suppresses it for this frame;
           reading on the set dot or just after clears it and suppresses the NMI */
        if (ppu.scanline == 241) {
            if (ppu.dot == 1) ppu.suppress_vbl = true;
            else if (ppu.dot == 2 || ppu.dot == 3) cpu.nmi_pending = 0;
        }
        ppu.status &= 0x7F;
        ppu.w = 0;
        ppu_update_nmi();
        break;
    case 4:
        r = ppu.oam[ppu.oam_addr];
        if ((ppu.oam_addr & 3) == 2) r &= 0xE3;
        break;
    case 7: {
        uint16_t a = ppu.v & 0x3FFF;
        if (a >= 0x3F00) {
            r = ppu.palette[pal_index(a)];
            if (ppu.mask & 1) r &= 0x30;
            ppu.read_buffer = ppu.vram[ppu.nt_offset[(a >> 10) & 3] + (a & 0x3FF)];
        } else {
            r = ppu.read_buffer;
            ppu.read_buffer = ppu_mem_read(a);
        }
        if (rendering_enabled() && (ppu.scanline < 240 || ppu.scanline == 261)) { inc_x(); inc_y(); }
        else ppu.v = (uint16_t)((ppu.v + ((ppu.ctrl & 4) ? 32 : 1)) & 0x7FFF);
        mapper_ppu_addr(ppu.v);
        break; }
    default:
        break;
    }
    ppu.open_bus = r;
    return r;
}

void ppu_reg_write(int reg, uint8_t v) {
    ppu_run_to(cpu.cycles * 3);
    ppu.open_bus = v;
    switch (reg) {
    case 0:
        if (sys.debug_log && v != ppu.ctrl)
            fprintf((FILE *)sys.debug_log, "frame %llu sl %d dot %d: $2000 = $%02X\n", (unsigned long long)ppu.frame, ppu.scanline, ppu.dot, v);
        ppu.ctrl = v;
        ppu.t = (uint16_t)((ppu.t & 0xF3FF) | ((v & 3) << 10));
        ppu_update_nmi();
        break;
    case 1:
        if (sys.debug_log && v != ppu.mask)
            fprintf((FILE *)sys.debug_log, "frame %llu sl %d dot %d: $2001 = $%02X\n", (unsigned long long)ppu.frame, ppu.scanline, ppu.dot, v);
        ppu.mask = v;
        break;
    case 3:
        ppu.oam_addr = v;
        break;
    case 4:
        ppu.oam[ppu.oam_addr++] = v;
        break;
    case 5:
        if (!ppu.w) {
            ppu.x = v & 7;
            ppu.t = (uint16_t)((ppu.t & 0xFFE0) | (v >> 3));
            ppu.w = 1;
        } else {
            ppu.t = (uint16_t)((ppu.t & 0x8C1F) | ((v & 7) << 12) | ((v & 0xF8) << 2));
            ppu.w = 0;
        }
        break;
    case 6:
        if (!ppu.w) {
            ppu.t = (uint16_t)((ppu.t & 0x00FF) | ((v & 0x3F) << 8));
            ppu.w = 1;
        } else {
            ppu.t = (uint16_t)((ppu.t & 0xFF00) | v);
            ppu.v = ppu.t;
            mapper_ppu_addr(ppu.v);
            ppu.w = 0;
        }
        break;
    case 7:
        ppu_mem_write(ppu.v, v);
        if (rendering_enabled() && (ppu.scanline < 240 || ppu.scanline == 261)) { inc_x(); inc_y(); }
        else ppu.v = (uint16_t)((ppu.v + ((ppu.ctrl & 4) ? 32 : 1)) & 0x7FFF);
        mapper_ppu_addr(ppu.v);
        break;
    default:
        break;
    }
}

void ppu_oam_dma(uint8_t page) {
    uint16_t base = (uint16_t)(page << 8);
    for (int i = 0; i < 256; i++)
        ppu.oam[(ppu.oam_addr + i) & 0xFF] = mem_read((uint16_t)(base + i));
    cpu.cycles += 513 + (cpu.cycles & 1);
}
