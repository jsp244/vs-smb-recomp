/*
 * snesppu.c - the SNES picture, one line at a time (snesppu.h).
 *
 * Every layer is drawn into a line buffer (CGRAM index, 0 = transparent, and the tile's priority),
 * then the main screen takes, per pixel, the frontmost layer in the mode's priority order, the
 * subscreen likewise, and color math mixes them. Sprites compete among themselves first, the
 * earliest one in the list with an opaque pixel winning whatever its priority (as on the console);
 * that winner's priority then places it among the backgrounds.
 */
#include "snesppu.h"
#include <string.h>

#define MAXW 1024

typedef struct { uint8_t idx, pri; } lpix_t;

static void bg_line(const snesppu_t *p, int bg, int bpp, int pal_base, int y, int x0, int w, lpix_t *out) {
    uint8_t sc = p->bgsc[bg];
    uint32_t map = (uint32_t)(sc & 0xFC) << 8;          /* word address */
    int wide = sc & 1, tall = (sc >> 1) & 1;
    uint32_t chr = (uint32_t)((bg < 2 ? p->bgnba[0] >> (bg * 4) : p->bgnba[1] >> ((bg - 2) * 4)) & 0x0F) << 12;
    int sy = (y + p->vofs[bg]) & 0x3FF;
    int ty = sy >> 3, row = sy & 7;
    int words = bpp * 4;                                  /* words per tile */
    for (int i = 0; i < w; i++) {
        int sx = (x0 + i + p->hofs[bg]) & 0x3FF;
        int tx = sx >> 3;
        uint32_t scr = (uint32_t)((wide ? (tx >> 5) & 1 : 0) + (tall ? ((ty >> 5) & 1) * (wide ? 2 : 1) : 0));
        uint32_t ea = (map + scr * 0x400 + (uint32_t)(ty & 31) * 32 + (uint32_t)(tx & 31)) & 0x7FFF;
        uint16_t e = (uint16_t)(p->vram[ea * 2] | (p->vram[ea * 2 + 1] << 8));
        int r = (e & 0x8000) ? 7 - row : row;
        int col = sx & 7;
        if (e & 0x4000) col = 7 - col;
        uint32_t ta = (chr + (uint32_t)(e & 0x3FF) * (uint32_t)words + (uint32_t)r) & 0x7FFF;
        int bit = 7 - col, v;
        v = ((p->vram[ta * 2] >> bit) & 1) | (((p->vram[ta * 2 + 1] >> bit) & 1) << 1);
        if (bpp == 4) {
            uint32_t tb = (ta + 8) & 0x7FFF;
            v |= (((p->vram[tb * 2] >> bit) & 1) << 2) | (((p->vram[tb * 2 + 1] >> bit) & 1) << 3);
        }
        out[i].idx = v ? (uint8_t)(pal_base + ((e >> 10) & 7) * (bpp == 4 ? 16 : 4) + v) : 0;
        out[i].pri = (uint8_t)((e >> 13) & 1);
    }
}

static const uint8_t obj_sizes[8][2][2] = {
    { { 8, 8 }, { 16, 16 } }, { { 8, 8 }, { 32, 32 } }, { { 8, 8 }, { 64, 64 } }, { { 16, 16 }, { 32, 32 } },
    { { 16, 16 }, { 64, 64 } }, { { 32, 32 }, { 64, 64 } }, { { 16, 32 }, { 32, 64 } }, { { 16, 32 }, { 32, 32 } },
};

void snesppu_sprite_size(const snesppu_t *p, int large, int *w, int *h) {
    *w = obj_sizes[(p->obsel >> 5) & 7][large ? 1 : 0][0];
    *h = obj_sizes[(p->obsel >> 5) & 7][large ? 1 : 0][1];
}

void snesppu_obsel(snesppu_t *p) {
    p->obj_name_base = (uint16_t)((p->obsel & 7) << 13);
    p->obj_name_select = (uint16_t)((((p->obsel >> 3) & 3) + 1) << 12);
}

void snesppu_load_oam(snesppu_t *p, const uint8_t *oam) {
    p->nspr = 0;
    for (int i = 0; i < 128; i++) {
        const uint8_t *o = oam + i * 4;
        uint8_t hi = (uint8_t)(oam[512 + i / 4] >> ((i & 3) * 2));
        snes_sprite_t *s = &p->spr[p->nspr++];
        s->x = (int16_t)(o[0] | ((hi & 1) << 8));
        if (s->x >= 256) s->x = (int16_t)(s->x - 512);
        s->y = o[1] >= 240 ? o[1] - 256 : o[1];   /* the top of the 256-line space wraps to above the screen */
        s->tile = (uint16_t)(o[2] | ((o[3] & 1) << 8));
        s->attr = o[3];
        s->large = (hi >> 1) & 1;
    }
}

static void obj_line(const snesppu_t *p, int y, int x0, int w, lpix_t *out, uint8_t *math) {
    memset(out, 0, sizeof(lpix_t) * (size_t)w);
    memset(math, 0, (size_t)w);
    /* the list's earliest sprite wins: draw from the back of the list to the front */
    for (int n = p->nspr - 1; n >= 0; n--) {
        const snes_sprite_t *s = &p->spr[n];
        int sw, sh;
        snesppu_sprite_size(p, s->large, &sw, &sh);
        int sy = y - s->y;
        if (sy < 0 || sy >= sh) continue;
        if (s->x >= x0 + w || s->x + sw <= x0) continue;
        int r = (s->attr & 0x80) ? sh - 1 - sy : sy;
        uint8_t pal = (uint8_t)(128 + ((s->attr >> 1) & 7) * 16);
        uint8_t pri = (uint8_t)((s->attr >> 4) & 3);
        for (int px = 0; px < sw; px++) {
            int sx = s->x + px - x0;
            if (sx < 0 || sx >= w) continue;
            int c = (s->attr & 0x40) ? sw - 1 - px : px;
            int t = s->tile;
            int sub = (t & 0x100) | ((t + (r >> 3) * 16) & 0xF0) | ((t + (c >> 3)) & 0x0F);
            uint32_t base = (uint32_t)p->obj_name_base + ((sub & 0x100) ? p->obj_name_select : 0);
            uint32_t ta = (base + (uint32_t)(sub & 0xFF) * 16 + (uint32_t)(r & 7)) & 0x7FFF, tb = (ta + 8) & 0x7FFF;
            int bit = 7 - (c & 7);
            int v = ((p->vram[ta * 2] >> bit) & 1) | (((p->vram[ta * 2 + 1] >> bit) & 1) << 1) |
                    (((p->vram[tb * 2] >> bit) & 1) << 2) | (((p->vram[tb * 2 + 1] >> bit) & 1) << 3);
            if (!v) continue;
            out[sx].idx = (uint8_t)(pal + v);
            out[sx].pri = pri;
            math[sx] = ((s->attr >> 1) & 7) >= 4;
        }
    }
}

/* window masks for one layer (0-3 BGs, 4 OBJ, 5 color): 1 = inside the combined window. The
   windows cover the screen's 256 columns; a pixel of a widescreen band, beyond them, is as the
   nearest edge column, so a window spanning the width goes on beside the picture (under water
   All-Stars allows color math only inside window 2, $00-$FF below the surface, $04:8312: the
   water's tint and gradient) */
static void window_line(const snesppu_t *p, int layer, int x0, int w, uint8_t *mask) {
    uint8_t sel;
    int logic;
    switch (layer) {
    case 0: sel = p->w12sel & 15; logic = p->wbglog & 3; break;
    case 1: sel = p->w12sel >> 4; logic = (p->wbglog >> 2) & 3; break;
    case 2: sel = p->w34sel & 15; logic = (p->wbglog >> 4) & 3; break;
    case 3: sel = p->w34sel >> 4; logic = (p->wbglog >> 6) & 3; break;
    case 4: sel = p->wobjsel & 15; logic = p->wobjlog & 3; break;
    default: sel = p->wobjsel >> 4; logic = (p->wobjlog >> 2) & 3; break;
    }
    int e1 = (sel >> 1) & 1, i1 = sel & 1, e2 = (sel >> 3) & 1, i2 = (sel >> 2) & 1;
    for (int i = 0; i < w; i++) {
        int x = x0 + i < 0 ? 0 : x0 + i > 255 ? 255 : x0 + i;
        if (!e1 && !e2) { mask[i] = 0; continue; }
        int in1 = (x >= p->wh[0] && x <= p->wh[1]) ^ i1;
        int in2 = (x >= p->wh[2] && x <= p->wh[3]) ^ i2;
        int m;
        if (e1 && !e2) m = in1;
        else if (e2 && !e1) m = in2;
        else switch (logic) {
            case 0: m = in1 | in2; break;
            case 1: m = in1 & in2; break;
            case 2: m = in1 ^ in2; break;
            default: m = !(in1 ^ in2); break;
        }
        mask[i] = (uint8_t)m;
    }
}

static inline uint32_t rgb15(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (r << 16) | (g << 8) | b;   /* 5-bit channels, packed for the math below */
}

void snesppu_line(const snesppu_t *p, int y, int x0, int w, uint32_t *out) {
    static lpix_t bg[4][MAXW], obj[MAXW];
    static uint8_t objmath[MAXW], win[6][MAXW];
    if (w > MAXW) w = MAXW;
    if (p->inidisp & 0x80) { for (int i = 0; i < w; i++) out[i] = 0xFF000000u; return; }
    int mode = p->bgmode & 7;
    int nbg = mode == 0 ? 4 : mode == 1 ? 3 : 2;
    uint8_t layers = (uint8_t)(p->tm | p->ts);
    for (int b = 0; b < nbg; b++) {
        if (!(layers & (1 << b))) { memset(bg[b], 0, sizeof(lpix_t) * (size_t)w); continue; }
        int bpp = mode == 0 ? 2 : (mode == 1 && b == 2) ? 2 : 4;
        int pal_base = mode == 0 ? b * 32 : 0;
        bg_line(p, b, bpp, pal_base, y, x0, w, bg[b]);
    }
    if (layers & 0x10) obj_line(p, y, x0, w, obj, objmath);
    else { memset(obj, 0, sizeof(lpix_t) * (size_t)w); memset(objmath, 0, (size_t)w); }
    for (int l = 0; l < 6; l++) window_line(p, l, x0, w, win[l]);

    /* priority ranks, higher in front; index: layer*2 + priority for BGs, 8 + priority for sprites */
    int rank[12] = { 0 };
    if (mode == 1) {
        int bg3top = (p->bgmode & 8) != 0;
        rank[8 + 3] = 11; rank[0 * 2 + 1] = 10; rank[1 * 2 + 1] = 9; rank[8 + 2] = 8;
        rank[0 * 2 + 0] = 7; rank[1 * 2 + 0] = 6; rank[8 + 1] = 5; rank[2 * 2 + 1] = bg3top ? 12 : 4;
        rank[8 + 0] = 3; rank[2 * 2 + 0] = 2;
    } else {   /* mode 0 (and anything else drawn as such) */
        rank[8 + 3] = 12; rank[0 * 2 + 1] = 11; rank[1 * 2 + 1] = 10; rank[8 + 2] = 9;
        rank[0 * 2 + 0] = 8; rank[1 * 2 + 0] = 7; rank[8 + 1] = 6; rank[2 * 2 + 1] = 5;
        rank[3 * 2 + 1] = 4; rank[8 + 0] = 3; rank[2 * 2 + 0] = 2; rank[3 * 2 + 0] = 1;
    }
    int bright = p->inidisp & 15;
    uint32_t fixed = (uint32_t)((p->fixed_r << 16) | (p->fixed_g << 8) | p->fixed_b);
    for (int i = 0; i < w; i++) {
        /* main screen and subscreen: the frontmost layer of each */
        int best[2] = { -1, -1 }, bestr[2] = { 0, 0 };
        uint8_t bidx[2] = { 0, 0 };
        int bmath[2] = { 0, 0 };
        for (int scr = 0; scr < 2; scr++) {
            uint8_t on = scr ? p->ts : p->tm, wmask = scr ? p->tsw : p->tmw;
            for (int b = 0; b < nbg; b++) {
                if (!(on & (1 << b)) || !bg[b][i].idx) continue;
                if ((wmask & (1 << b)) && win[b][i]) continue;
                int r = rank[b * 2 + bg[b][i].pri];
                if (r > bestr[scr]) { bestr[scr] = r; best[scr] = b; bidx[scr] = bg[b][i].idx; }
            }
            if ((on & 0x10) && obj[i].idx && !((wmask & 0x10) && win[4][i])) {
                int r = rank[8 + obj[i].pri];
                if (r > bestr[scr]) { bestr[scr] = r; best[scr] = 4; bidx[scr] = obj[i].idx; }
            }
            bmath[scr] = best[scr] < 0 ? 5 : best[scr];
        }
        uint32_t main = best[0] < 0 ? rgb15(p->cgram[0]) : rgb15(p->cgram[bidx[0]]);
        /* color window: force black / prevent math */
        int cw = win[5][i];
        int black = 0, nomath = 0;
        switch ((p->cgwsel >> 6) & 3) { case 1: black = !cw; break; case 2: black = cw; break; case 3: black = 1; break; }
        switch ((p->cgwsel >> 4) & 3) { case 1: nomath = !cw; break; case 2: nomath = cw; break; case 3: nomath = 1; break; }
        if (black) main = 0;
        int layer = bmath[0];
        int enabled = (p->cgadsub >> layer) & 1;
        if (layer == 4 && !objmath[i]) enabled = 0;
        if (enabled && !nomath) {
            uint32_t sub;
            int half = (p->cgadsub >> 6) & 1;
            if (p->cgwsel & 2) {
                if (best[1] < 0) { sub = fixed; half = 0; }
                else sub = rgb15(p->cgram[bidx[1]]);
            } else sub = fixed;
            if (black) half = 0;
            uint32_t res = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                int a = (int)((main >> sh) & 31), b = (int)((sub >> sh) & 31), v;
                if (p->cgadsub & 0x80) v = a - b; else v = a + b;
                if (half) v >>= 1;
                if (v < 0) v = 0;
                if (v > 31) v = 31;
                res |= (uint32_t)v << sh;
            }
            main = res;
        }
        uint32_t o = 0xFF000000u;
        for (int sh = 0; sh <= 16; sh += 8) {
            uint32_t v = (main >> sh) & 31;
            v = (v << 3) | (v >> 2);
            v = v * (uint32_t)(bright + 1) / 16;
            o |= v << sh;
        }
        out[i] = o;
    }
}
