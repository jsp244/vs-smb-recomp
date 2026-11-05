/*
 * snesppu.h - the SNES picture, drawn one line at a time from VRAM, CGRAM, a sprite list and the
 * PPU's registers: modes 0 and 1 (8x8 tiles), sprites of every OBSEL size, priorities, the BG3
 * priority bit, both windows, color math against the subscreen or the fixed color, brightness.
 *
 * allstars.c fills it in the way Super Mario All-Stars would for the frame the game is showing,
 * and draws the remastered picture with it; the widescreen bands are the same lines drawn from
 * further left or right (x0 below), the tilemaps being 512 pixels wide like the NES nametables.
 */
#ifndef SNESPPU_H
#define SNESPPU_H
#include <stdint.h>

typedef struct snes_sprite {
    int16_t  x, y;          /* top-left corner on the screen (any range) */
    uint16_t tile;          /* 9-bit tile number (bit 8 = second name table) */
    uint8_t  attr;          /* OAM attribute byte: v h oo ppp N (N is in tile too) */
    uint8_t  large;         /* the OBSEL large size */
} snes_sprite_t;

typedef struct snesppu {
    uint8_t  vram[65536];
    uint16_t cgram[256];
    /* registers, as written to $2100-$2133 */
    uint8_t  inidisp, obsel, bgmode, mosaic;
    uint8_t  bgsc[4], bgnba[2];
    uint16_t hofs[4], vofs[4];
    uint8_t  w12sel, w34sel, wobjsel, wh[4], wbglog, wobjlog;
    uint8_t  tm, ts, tmw, tsw, cgwsel, cgadsub, setini;
    uint8_t  fixed_r, fixed_g, fixed_b;   /* $2132, 0-31 each */
    /* sprites: drawn in list order, the first one in front among equal priorities (like OAM) */
    snes_sprite_t spr[128 + 64];
    int      nspr;
    uint16_t obj_name_base, obj_name_select;   /* word addresses, from OBSEL */
} snesppu_t;

/* Draws line y (0-238) of the screen into out: pixels x0 .. x0+w-1 of the screen (x0 may be
   negative or beyond 255: the background layers wrap round their tilemaps, sprites are placed
   by their own x, the windows are as at the nearest edge column). Pixels are 0xAARRGGBB. */
void snesppu_line(const snesppu_t *p, int y, int x0, int w, uint32_t *out);
void snesppu_obsel(snesppu_t *p);    /* derive the sprite name bases from obsel */
void snesppu_sprite_size(const snesppu_t *p, int large, int *w, int *h);
/* the OAM (544 bytes) as a sprite list */
void snesppu_load_oam(snesppu_t *p, const uint8_t *oam);

#endif
