#ifndef PALETTE_H
#define PALETTE_H
#include <stdint.h>
/* out: 8 emphasis sets x 64 colours, 0x00RRGGBB */
void nes_palette_build(uint32_t *out);
/* the Vs. System's RGB PPUs, by NES 2.0 Vs. PPU type (0-1 and 6-12: the 2C03 / 2C05 palette,
   2-5: RP2C04-0001 .. -0004, the scrambled ones); emphasis turns a channel fully on */
void nes_palette_build_rgb(uint32_t *out, int vs_ppu);
/* the same picture in the colours a NES (2C02, composite video) shows: each index through the
   Vs. PPU's table to the 2C02 colour at the same place of the palette */
void nes_palette_build_vs_composite(uint32_t *out, int vs_ppu);
#endif
