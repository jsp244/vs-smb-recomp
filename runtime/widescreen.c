/*
 * widescreen.c - the level beyond the edges of the picture, from the game's
 * own nametables.
 *
 * The NES picture is 256 pixels wide, but VS. Super Mario Bros. (like the NES
 * game) keeps a 512-pixel window of the level in its two nametables: the area
 * parser renders two columns of metatiles for every 32 pixels the screen
 * scrolls and stays 90-128 pixels ahead of the right edge, and what scrolled
 * out on the left stays there until the parser comes round again (measured
 * with the harness's RAM and VRAM dumps). So the bands of a widescreen view
 * need no decoding: they are those nametable columns, drawn with the scroll,
 * control register and CHR bank each line was drawn with and the current
 * palette, which joins them to the real picture without a seam. The status
 * bar's lines (scroll 0) show the blank tiles beside it.
 *
 * What the nametables hold is a level only during play and the attract demo:
 * the area parser's frontier and the screen position in RAM say how far the
 * level is valid, and a frame whose scroll does not agree with that position
 * (a text screen, the intermission, a menu) gets black around the picture.
 * Objects do not exist beyond the screen until the game spawns them at its
 * edge, so the bands show the level's background only.
 */
#include "widescreen.h"
#include "nes.h"
#include <string.h>

/* RAM locations of the engine (as in the NES Super Mario Bros.) */
#define RAM_SCREEN_LEFT_PAGE 0x071A    /* the screen's left edge: page (256 px) */
#define RAM_SCREEN_LEFT_X    0x071C    /* and pixel in it */
#define RAM_PARSER_PAGE      0x0725    /* the area parser's next column: page */
#define RAM_PARSER_COLUMN    0x0726    /* and metatile column (16 px) in it */
#define MARGIN 16                      /* keep off the column being rendered, and off the oldest one */

/* the index of a black colour for the loaded board's PPU */
static uint8_t black_index(void) {
    /* RP2C04-0004 has black at $14 (VS. Super Mario Bros. uses it); the 2C02 and 2C03 at $0F */
    return (cart.vs && cart.vs_ppu == 5) ? 0x14 : 0x0F;
}

/* the line's background origin in the 512 x 480 nametable space */
static int line_origin_x(int ly) {
    uint16_t v = ppu.line_v[ly];
    return ((v & 0x400) ? 256 : 0) + (v & 0x1F) * 8 + ppu.line_fx[ly];
}
static int line_origin_y(int ly) {
    uint16_t v = ppu.line_v[ly];
    return ((v & 0x800) ? 240 : 0) + ((v >> 5) & 0x1F) * 8 + ((v >> 12) & 7);
}

/* the picture's columns [lo, hi) that hold the level (lo < 0 and hi > 256 reach into the bands);
   0 when this frame shows no level */
static int level_window(int *lo, int *hi) {
    int left = ram[RAM_SCREEN_LEFT_PAGE] * 256 + ram[RAM_SCREEN_LEFT_X];
    int front = ram[RAM_PARSER_PAGE] * 256 + ram[RAM_PARSER_COLUMN] * 16;
    int ahead = front - (left + 256);
    if (ahead < 0 || ahead > 192) return 0;
    /* the playfield's scroll (a line low in the picture, below the status bar) must be that position,
       give or take the frame of scrolling between the picture and the RAM */
    int ly = 239;
    while (ly >= 64 && !(ppu.line_emph[ly] & 0x08)) ly--;
    if (ly < 64) return 0;
    int d = (line_origin_x(ly) - left) & 511;
    if (d > 8 && d < 512 - 8) return 0;
    *lo = front - 512 + MARGIN - left;
    if (*lo < -left) *lo = -left;        /* nothing before the start of the area */
    *hi = front - MARGIN - left;
    return 1;
}

/* one background pixel of line ly at picture column sx, from the nametables */
static uint8_t bg_pixel(int ly, int sx, int ox, int oy) {
    int wx = (ox + sx) & 511, wy = oy % 480;
    int nt = ((wy >= 240) ? 2 : 0) + (wx >> 8);
    int ty = (wy % 240) >> 3, tx = (wx & 255) >> 3;
    const uint8_t *ntp = ppu.vram + ppu.nt_offset[nt];
    uint8_t tile = ntp[ty * 32 + tx];
    uint8_t at = ntp[0x3C0 + (ty >> 2) * 8 + (tx >> 2)];
    int pal = (at >> (((ty & 2) ? 4 : 0) + ((tx & 2) ? 2 : 0))) & 3;
    uint16_t a = (uint16_t)(((ppu.line_ctrl[ly] & 0x10) ? 0x1000 : 0) + tile * 16 + (wy & 7));
    const uint8_t *chr = mapper_chr_bank_ptr(ppu.frame_chr_bank[a >> 10]);
    uint8_t p0 = chr[a & 0x3FF], p1 = chr[(a + 8) & 0x3FF];
    int bit = 7 - (wx & 7);
    int pix = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1);
    uint8_t c = pix ? ppu.palette[pal * 4 + pix] : ppu.palette[0];
    if (ppu.line_emph[ly] & 1) c &= 0x30;
    return c;
}

int widescreen_render(uint8_t *out, uint8_t *emph, int x, int y, int w, int h) {
    int px0 = x - WS_BAND, py0 = y - WS_VBAND;   /* the rectangle in the picture's coordinates */
    int lo = 0, hi = 256, level = level_window(&lo, &hi);
    uint8_t black = black_index();
    for (int r = 0; r < h; r++) {
        int ly = py0 + r;
        uint8_t *dst = out + (size_t)r * w;
        if (ly < 0 || ly >= 240) { memset(dst, black, (size_t)w); emph[r] = 0; continue; }
        emph[r] = ppu.line_emph[ly];
        int drawn = (ppu.line_emph[ly] & 0x08) != 0;
        int ox = line_origin_x(ly), oy = line_origin_y(ly);
        for (int c = 0; c < w; c++) {
            int sx = px0 + c;
            if (sx >= 0 && sx < 256) dst[c] = ppu.fb[ly * 256 + sx];
            else if (!level || sx < lo || sx >= hi) dst[c] = black;
            else if (!drawn) dst[c] = (ppu.line_emph[ly] & 1) ? (uint8_t)(ppu.palette[0] & 0x30) : ppu.palette[0];
            else dst[c] = bg_pixel(ly, sx, ox, oy);
        }
    }
    return level;
}

int widescreen_check(int *bad_pixels, int *total_pixels) {
    int lo, hi;
    *bad_pixels = *total_pixels = 0;
    if (!level_window(&lo, &hi)) return -1;
    for (int ly = 0; ly < 240; ly++) {
        if (!(ppu.line_emph[ly] & 0x08)) continue;
        int ox = line_origin_x(ly), oy = line_origin_y(ly);
        for (int sx = 0; sx < 256; sx++) {
            if (bg_pixel(ly, sx, ox, oy) != ppu.fb[ly * 256 + sx]) (*bad_pixels)++;
            (*total_pixels)++;
        }
    }
    return 0;
}

int widescreen_visible_width(int lines, double pixel_aspect, double ratio) {
    int w = (int)(lines * ratio / pixel_aspect + 0.5);
    if (w > WS_W) w = WS_W;
    if (w < 256) w = 256;
    return w;
}
