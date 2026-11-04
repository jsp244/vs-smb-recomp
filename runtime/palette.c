/*
 * palette.c - NTSC NES palette generated from the composite video model
 * (the well-known 12-phase square wave decoding), for all 8 emphasis
 * combinations, and the RGB palettes of the Vs. System's PPUs (the
 * hardware's 3-bit DAC levels, as documented on the NESdev wiki and in
 * MAME). Nothing here is taken from the ROM.
 */
#include "palette.h"
#include <math.h>

/* RGB PPU colours as three 3-bit DAC levels (red, green, blue): 0x1C0 = red 7 */
#define DAC(r, g, b) ((r) << 6 | (g) << 3 | (b))

/* the 2C03 / 2C05 palette, in the 2C02's order */
static const uint16_t rgb_2c03[64] = {
    DAC(3,3,3), DAC(0,1,4), DAC(0,0,6), DAC(3,2,6), DAC(4,0,3), DAC(5,0,3), DAC(5,1,0), DAC(4,2,0),
    DAC(3,2,0), DAC(1,2,0), DAC(0,3,1), DAC(0,4,0), DAC(0,2,2), DAC(0,0,0), DAC(0,0,0), DAC(0,0,0),
    DAC(5,5,5), DAC(0,3,6), DAC(0,2,7), DAC(4,0,7), DAC(5,0,7), DAC(7,0,4), DAC(7,0,0), DAC(6,3,0),
    DAC(4,3,0), DAC(1,4,0), DAC(0,4,0), DAC(0,5,3), DAC(0,4,4), DAC(0,0,0), DAC(0,0,0), DAC(0,0,0),
    DAC(7,7,7), DAC(3,5,7), DAC(4,4,7), DAC(6,3,7), DAC(7,0,7), DAC(7,3,7), DAC(7,4,0), DAC(7,5,0),
    DAC(6,6,0), DAC(3,6,0), DAC(0,7,0), DAC(2,7,6), DAC(0,7,7), DAC(0,0,0), DAC(0,0,0), DAC(0,0,0),
    DAC(7,7,7), DAC(5,6,7), DAC(6,5,7), DAC(7,5,7), DAC(7,4,7), DAC(7,5,5), DAC(7,6,4), DAC(7,7,2),
    DAC(7,7,3), DAC(5,7,2), DAC(4,7,3), DAC(2,7,6), DAC(4,6,7), DAC(0,0,0), DAC(0,0,0), DAC(0,0,0),
};

/* the master palette all four 2C04s share, in an order close to the 2C03's (no 2C04 uses this
   order: each sees it through its own table below) */
static const uint16_t rgb_2c04[64] = {
    DAC(3,3,3), DAC(0,1,4), DAC(0,0,6), DAC(3,2,6), DAC(4,0,3), DAC(5,0,3), DAC(5,1,0), DAC(4,2,0),
    DAC(3,2,0), DAC(1,2,0), DAC(0,3,1), DAC(0,4,0), DAC(0,2,2), DAC(1,1,1), DAC(0,0,3), DAC(0,2,0),
    DAC(5,5,5), DAC(0,3,6), DAC(0,2,7), DAC(4,0,7), DAC(5,0,7), DAC(7,0,4), DAC(7,0,0), DAC(6,3,0),
    DAC(4,3,0), DAC(1,4,0), DAC(0,4,0), DAC(0,5,3), DAC(0,4,4), DAC(2,2,2), DAC(2,0,0), DAC(3,1,0),
    DAC(7,7,7), DAC(3,5,7), DAC(4,4,7), DAC(6,3,7), DAC(7,0,7), DAC(7,3,7), DAC(7,4,0), DAC(7,5,0),
    DAC(6,6,0), DAC(3,6,0), DAC(0,7,0), DAC(2,7,6), DAC(0,7,7), DAC(4,4,4), DAC(0,0,0), DAC(0,0,0),
    DAC(7,7,7), DAC(5,6,7), DAC(6,5,7), DAC(7,5,7), DAC(7,4,7), DAC(7,5,5), DAC(7,6,4), DAC(7,7,0),
    DAC(7,7,3), DAC(5,7,2), DAC(4,7,3), DAC(2,7,6), DAC(4,6,7), DAC(6,6,6), DAC(6,5,3), DAC(7,6,0),
};

/* the RP2C04-0001 .. -0004 scrambles: palette index -> entry of rgb_2c04 */
static const uint8_t lut_2c04[4][64] = {
    { 0x35,0x23,0x16,0x22,0x1C,0x09,0x1D,0x15,0x20,0x00,0x27,0x05,0x04,0x28,0x08,0x20,
      0x21,0x3E,0x1F,0x29,0x3C,0x32,0x36,0x12,0x3F,0x2B,0x2E,0x1E,0x3D,0x2D,0x24,0x01,
      0x0E,0x31,0x33,0x2A,0x2C,0x0C,0x1B,0x14,0x2E,0x07,0x34,0x06,0x13,0x02,0x26,0x2E,
      0x2E,0x19,0x10,0x0A,0x39,0x03,0x37,0x17,0x0F,0x11,0x0B,0x0D,0x38,0x25,0x18,0x3A },
    { 0x2E,0x27,0x18,0x39,0x3A,0x25,0x1C,0x31,0x16,0x13,0x38,0x34,0x20,0x23,0x3C,0x0B,
      0x0F,0x21,0x06,0x3D,0x1B,0x29,0x1E,0x22,0x1D,0x24,0x0E,0x2B,0x32,0x08,0x2E,0x03,
      0x04,0x36,0x26,0x33,0x11,0x1F,0x10,0x02,0x14,0x3F,0x00,0x09,0x12,0x2E,0x28,0x20,
      0x3E,0x0D,0x2A,0x17,0x0C,0x01,0x15,0x19,0x2E,0x2C,0x07,0x37,0x35,0x05,0x0A,0x2D },
    { 0x14,0x25,0x3A,0x10,0x0B,0x20,0x31,0x09,0x01,0x2E,0x36,0x08,0x15,0x3D,0x3E,0x3C,
      0x22,0x1C,0x05,0x12,0x19,0x18,0x17,0x1B,0x00,0x03,0x2E,0x02,0x16,0x06,0x34,0x35,
      0x23,0x0F,0x0E,0x37,0x0D,0x27,0x26,0x20,0x29,0x04,0x21,0x24,0x11,0x2D,0x2E,0x1F,
      0x2C,0x1E,0x39,0x33,0x07,0x2A,0x28,0x1D,0x0A,0x2E,0x32,0x38,0x13,0x2B,0x3F,0x0C },
    { 0x18,0x03,0x1C,0x28,0x2E,0x35,0x01,0x17,0x10,0x1F,0x2A,0x0E,0x36,0x37,0x0B,0x39,
      0x25,0x1E,0x12,0x34,0x2E,0x1D,0x06,0x26,0x3E,0x1B,0x22,0x19,0x04,0x2E,0x3A,0x21,
      0x05,0x0A,0x07,0x02,0x13,0x14,0x00,0x15,0x0C,0x3D,0x11,0x0F,0x0D,0x38,0x2D,0x24,
      0x33,0x20,0x08,0x16,0x3F,0x2B,0x20,0x3C,0x2E,0x27,0x23,0x31,0x29,0x32,0x2C,0x09 },
};

static double gammafix(double f, double gamma) { return f <= 0 ? 0 : pow(f, 2.2 / gamma); }
static double clampd(double v) { return v > 255 ? 255 : (v < 0 ? 0 : v); }

static uint32_t ntsc_color(int pixel, double saturation, double hue_shift, double gamma) {
    static const double levels[8] = { 0.350, 0.518, 0.962, 1.550, 1.094, 1.506, 1.962, 1.962 };
    const double black = 0.518, white = 1.962;
    int color = pixel & 0x0F;
    int level = (pixel >> 4) & 3;
    if (color > 13) level = 1;
    double lo = levels[level], hi = levels[level + 4];
    if (color == 0) lo = hi;
    if (color > 12) hi = lo;
    double y = 0, i = 0, q = 0;
    for (int p = 0; p < 12; p++) {
        int in_phase = ((color + p + 8) % 12) < 6;   /* +8 rotates hue so $x6 is red, $x2 blue, $xA green */
        double spot = in_phase ? hi : lo;
        if (((pixel & 0x40) && (((0xC + p + 8) % 12) < 6)) ||
            ((pixel & 0x80) && (((0x4 + p + 8) % 12) < 6)) ||
            ((pixel & 0x100) && (((0x8 + p + 8) % 12) < 6)))
            spot *= 0.746;
        double v = (spot - black) / (white - black) / 12.0;
        y += v;
        i += v * cos(M_PI * (p + hue_shift) / 6.0) * saturation;
        q += v * sin(M_PI * (p + hue_shift) / 6.0) * saturation;
    }
    double r = clampd(255.95 * gammafix(y + 0.946882 * i + 0.623557 * q, gamma));
    double g = clampd(255.95 * gammafix(y - 0.274788 * i - 0.635691 * q, gamma));
    double b = clampd(255.95 * gammafix(y - 1.108545 * i + 1.709007 * q, gamma));
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void nes_palette_build(uint32_t *out) {
    for (int emph = 0; emph < 8; emph++)
        for (int c = 0; c < 64; c++)
            out[emph * 64 + c] = ntsc_color(c | (emph << 6), 1.0, 0.0, 2.0);
}

static int is_2c04(int vs_ppu) { return vs_ppu >= 2 && vs_ppu <= 5; }

/* the DAC levels the PPU puts out for palette index c */
static uint16_t vs_dac(int vs_ppu, int c) {
    return is_2c04(vs_ppu) ? rgb_2c04[lut_2c04[vs_ppu - 2][c & 63]] : rgb_2c03[c & 63];
}

void nes_palette_build_rgb(uint32_t *out, int vs_ppu) {
    for (int emph = 0; emph < 8; emph++)
        for (int c = 0; c < 64; c++) {
            uint16_t d = vs_dac(vs_ppu, c);
            int r = (d >> 6) & 7, g = (d >> 3) & 7, b = d & 7;
            /* $2001 bits 5-7: red, green and blue at full level instead of darkening the rest */
            if (emph & 1) r = 7;
            if (emph & 2) g = 7;
            if (emph & 4) b = 7;
            out[emph * 64 + c] = (uint32_t)(255 * r / 7) << 16 | (uint32_t)(255 * g / 7) << 8 | (uint32_t)(255 * b / 7);
        }
}

void nes_palette_build_vs_composite(uint32_t *out, int vs_ppu) {
    /* the master palette's columns $0-$C are the 2C02's colours at the same index; its extra
       greys and browns ($D-$F) become the 2C02 colour nearest to them */
    uint32_t ntsc[64];
    for (int c = 0; c < 64; c++) ntsc[c] = ntsc_color(c, 1.0, 0.0, 2.0);
    for (int c = 0; c < 64; c++) {
        int k = is_2c04(vs_ppu) ? lut_2c04[vs_ppu - 2][c] : c;
        if ((k & 0x0F) > 0x0C) {
            uint16_t d = is_2c04(vs_ppu) ? rgb_2c04[k] : rgb_2c03[k];
            int r = 255 * ((d >> 6) & 7) / 7, g = 255 * ((d >> 3) & 7) / 7, b = 255 * (d & 7) / 7;
            int best = 0x0F;
            long best_d = -1;
            for (int n = 0; n < 64; n++) {
                if ((n & 0x0F) == 0x0D) continue;   /* "blacker than black" */
                int dr = (int)(ntsc[n] >> 16 & 255) - r, dg = (int)(ntsc[n] >> 8 & 255) - g, db = (int)(ntsc[n] & 255) - b;
                long dd = (long)dr * dr + (long)dg * dg + (long)db * db;
                if (best_d < 0 || dd < best_d) { best_d = dd; best = n; }
            }
            k = best;
        }
        for (int emph = 0; emph < 8; emph++) out[emph * 64 + c] = ntsc_color(k | (emph << 6), 1.0, 0.0, 2.0);
    }
}
