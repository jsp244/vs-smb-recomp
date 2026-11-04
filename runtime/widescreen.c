/*
 * widescreen.c - the level beyond the edges of the picture, from the game's
 * own nametables, with the objects the game keeps there.
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
 *
 * An area entered at its halfway point (after a death) or from a pipe is drawn
 * from that page on, so the columns to the left of the screen were never
 * rendered. That page is drawn here as the game draws an area: InitializeArea,
 * from a copy of RAM made at its first instruction (a hook), runs again in the
 * CPU sandbox told to start a page earlier, and the area parser renders that
 * page's eight column sets; the nametable writes it queues for the NMI go into
 * a copy of the nametables the band reads instead.
 *
 * Objects: the game creates an enemy when it comes within 48 pixels of the
 * screen's right edge and erases it 72 pixels past either edge, so the bands
 * hold live objects, but the game only builds sprites for what is on its
 * 256-pixel screen (OAM has no room for more) and hides the 8-pixel sprite
 * columns that straddle an edge. To draw them, the engine's own frame code is
 * run again: at GameEngine's first instruction (a hook) RAM is copied, and when
 * a band is drawn that copy runs GameEngine once more in the CPU sandbox with
 * the camera moved 112 pixels towards the band. Every object is processed and
 * drawn exactly as in the real frame, only the screen it is placed on is
 * wider; the sprites it builds for the band (and for the hidden edge columns)
 * go onto the canvas with the PPU's priority rules. The engine copy used is
 * the one whose sprites the PPU shows: the run of the previous frame, whose
 * OAM was DMA'd at that frame's end.
 */
#include "widescreen.h"
#include "allstars.h"
#include "nes.h"
#include <limits.h>
#include <string.h>

/* RAM locations of the engine (as in the NES Super Mario Bros.) */
#define RAM_SCREEN_LEFT_PAGE  0x071A   /* the screen's left edge: page (256 px) */
#define RAM_SCREEN_RIGHT_PAGE 0x071B   /* and right edge */
#define RAM_SCREEN_LEFT_X     0x071C   /* pixel in the page */
#define RAM_SCREEN_RIGHT_X    0x071D
#define RAM_PARSER_PAGE       0x0725   /* the area parser's next column: page */
#define RAM_PARSER_COLUMN     0x0726   /* and metatile column (16 px) in it */
#define RAM_PARSER_TASKNUM    0x071F
#define RAM_VRAM_BUF2_OFS     0x0340   /* the buffer of nametable writes the renderer fills (the NMI empties it) */
#define RAM_VRAM_BUF2         0x0341
#define RAM_OAM               0x0200   /* the sprite page the NMI copies to the PPU */
#define RAM_BLOCK_BUFFER      0x0500   /* the metatiles objects collide with: 13 rows of 16 columns for the
                                          even pages, then 13 for the odd ones */
#define MARGIN 16                      /* keep off the column being rendered, and off the oldest one */

/* the engine's code (VS. Super Mario Bros.: the NES game's engine at other addresses) */
#define ENGINE_START        0xADAB     /* GameEngine: processes and draws every object of the frame */
#define ENGINE_SPRITES_DONE 0xAE14     /* past its last sprite update (the player's star palette) */
#define AREA_INIT           0x965C     /* InitializeArea: sets the parser up to draw the area from a start page */
#define AREA_START_PAGE     0x9681     /* its STA ScreenLeft_PageLoc, with the start page in A */
#define PARSER_TASK         0xA18B     /* AreaParserTaskHandler: one task; 8 of them render a 32-pixel column set */
#define PARSER_CORE         0xA2D7     /* AreaParserCore: one column of metatiles (allstars.c follows it) */
#define SANDBOX_RETURN      0x0000     /* a call made in the sandbox returns here */
#define CAMERA_SHIFT 112               /* objects up to 128 px beside the screen land well inside the moved one */
#define EDGE 8                         /* the sprite columns the game hides at each edge */
#define FILL_W 256                     /* the page drawn before an area's start page */
#define FILL_SOLID_W 128               /* the part of it next to the start page that also gets its collision */
static const char vs_smb_sha1[] = "b21aa940728ed80c72ee23c251c96e42cc84b2d6";
/* what tells one area from another: world, level, area number, area type and the offset of its data
   (not the area pointer, which the enemy data's pipe objects change to their destination during play) */
static const uint16_t area_id_addr[5] = { 0x075F, 0x075C, 0x0760, 0x074E, 0x074F };

bool widescreen_sprites = true;
bool widescreen_fill = true;

/* the index of a black colour for the loaded board's PPU */
static uint8_t black_index(void) {
    /* RP2C04-0004 has black at $14 (VS. Super Mario Bros. uses it); the 2C02 and 2C03 at $0F */
    return (cart.vs && cart.vs_ppu == 5) ? 0x14 : 0x0F;
}

/* ---- copies of the machine made by the hooks ---- */
typedef struct {
    uint64_t frame;     /* ppu.frame when it was made */
    uint32_t gen;       /* counts every copy, so a cached result can tell them apart */
    bool     valid;
    cpu_t    cpu;
    uint8_t  ram[2048], wram[2048];
} engine_copy_t;
static engine_copy_t copies[4];       /* GameEngine's starts, by frame number */
static engine_copy_t area_copy;       /* the latest start of an area (InitializeArea's first instruction) */
static uint32_t copy_gen;

static void engine_hook(uint16_t pc) {
    engine_copy_t *s;
    allstars_hook(pc);   /* the remastered graphics follow the same instructions */
    if (pc == ENGINE_START) s = &copies[ppu.frame & 3];
    else if (pc == AREA_INIT) s = &area_copy;
    else return;
    s->frame = ppu.frame;
    s->gen = ++copy_gen;
    s->valid = true;
    s->cpu = cpu;
    s->cpu.pc = pc;
    memcpy(s->ram, ram, sizeof s->ram);
    memcpy(s->wram, cart.wram, sizeof s->wram);
}

void widescreen_attach(void) {
    if (cart.vs && strcmp(cart.sha1_hex, vs_smb_sha1) == 0) {
        sys.hook = engine_hook;
        sys.hook_pc[0] = ENGINE_START;
        sys.hook_pc[1] = AREA_INIT;
        sys.hook_pc[2] = PARSER_CORE;
        bandwalk_attach(1);
    } else {
        if (sys.hook == engine_hook) sys.hook = NULL;
        bandwalk_attach(0);
    }
}

/* sets up a call of the routine at pc that returns to SANDBOX_RETURN */
static void sandbox_call(cpu_t *r, uint8_t *sram, uint16_t pc) {
    uint16_t ret = (uint16_t)(SANDBOX_RETURN - 1);   /* RTS adds one */
    sram[0x100 | r->sp] = (uint8_t)(ret >> 8); r->sp--;
    sram[0x100 | r->sp] = (uint8_t)ret; r->sp--;
    r->pc = pc;
}

/* the engine run whose sprites the PPU shows now: the previous frame's (its OAM went to the PPU at that
   frame's end); right after a state load, which has none, this frame's run, a frame ahead, unless exact */
static const engine_copy_t *shown_copy(int exact) {
    const engine_copy_t *s = &copies[(ppu.frame - 1) & 3];
    if (s->valid && s->frame == ppu.frame - 1) return s;
    s = &copies[ppu.frame & 3];
    if (!exact && s->valid && s->frame == ppu.frame) return s;
    return NULL;
}

const uint8_t *widescreen_shown_ram(int *ahead) {
    const engine_copy_t *s = shown_copy(0);
    if (ahead) *ahead = s && s->frame == ppu.frame;
    return s ? s->ram : NULL;
}

int widescreen_level_window(int *lo, int *hi);

/* the 64 sprites that run builds with the camera moved dx pixels to the right; 0 if unavailable */
static int engine_oam(int dx, uint8_t oam[256], int exact) {
    static struct { uint32_t gen; int dx, ok; uint8_t oam[256]; } cache[3];
    static uint8_t sram[2048], swram[2048];
    const engine_copy_t *s = shown_copy(exact);
    if (!s) return 0;
    int slot = dx < 0 ? 0 : dx > 0 ? 2 : 1;
    if (cache[slot].gen == s->gen && cache[slot].dx == dx) {
        memcpy(oam, cache[slot].oam, 256);
        return cache[slot].ok;
    }
    memcpy(sram, s->ram, sizeof sram);
    memcpy(swram, s->wram, sizeof swram);
    unsigned l = ((unsigned)sram[RAM_SCREEN_LEFT_PAGE] << 8 | sram[RAM_SCREEN_LEFT_X]) + (unsigned)dx;
    unsigned r = ((unsigned)sram[RAM_SCREEN_RIGHT_PAGE] << 8 | sram[RAM_SCREEN_RIGHT_X]) + (unsigned)dx;
    sram[RAM_SCREEN_LEFT_PAGE] = (uint8_t)(l >> 8); sram[RAM_SCREEN_LEFT_X] = (uint8_t)l;
    sram[RAM_SCREEN_RIGHT_PAGE] = (uint8_t)(r >> 8); sram[RAM_SCREEN_RIGHT_X] = (uint8_t)r;
    cpu_t regs = s->cpu;
    mem_sandbox_begin();
    int ok = cpu_sandbox_run(&regs, ENGINE_SPRITES_DONE, sram, swram, 200000) >= 0;
    if (ok) memcpy(oam, sram + RAM_OAM, 256);
    cache[slot].gen = s->gen; cache[slot].dx = dx; cache[slot].ok = ok;
    memcpy(cache[slot].oam, oam, 256);
    return ok;
}

int widescreen_band_oam(int side, uint8_t oam[256], int *dx) {
    if (!widescreen_sprites) return 0;
    *dx = side ? CAMERA_SHIFT : -CAMERA_SHIFT;
    return engine_oam(*dx, oam, 0);
}

/* ---- the page before an area's start page ---- */
static struct {
    uint32_t gen;               /* the area copy it was made from */
    int start;                  /* the world pixel the game's own rendering of the area began at, -1 = unknown */
    int ok;                     /* vram holds the page before start */
    uint8_t id[5];              /* the area */
    uint8_t vram[4096];         /* the nametables with that page rendered */
    uint8_t block[0x1A0];       /* and the block buffer */
    uint32_t solid;             /* the copy whose page went into the game's block buffer (widescreen_fill_collision) */
} fill = { .start = -1 };

static void nt_write(uint8_t *vram, uint16_t a, uint8_t v) {
    a &= 0x3FFF;
    if (a < 0x2000 || a >= 0x3F00) return;     /* the palette, or not a nametable */
    a = (uint16_t)((a - 0x2000) & 0x0FFF);
    vram[ppu.nt_offset[a >> 10] + (a & 0x3FF)] = v;
}

/* the renderer's nametable writes (in the format the NMI's screen update reads) into vram, then empties the buffer */
static void flush_vram_buffer(uint8_t *sram, uint8_t *vram) {
    int i = RAM_VRAM_BUF2;
    while (i + 3 < 0x800 && sram[i]) {
        uint16_t a = (uint16_t)(sram[i] << 8 | sram[i + 1]);
        uint8_t ctl = sram[i + 2];
        int inc = (ctl & 0x80) ? 32 : 1, repeat = (ctl & 0x40) != 0, len = ctl & 0x3F;
        i += 3;
        for (int k = 0; k < len && i + (repeat ? 0 : k) < 0x800; k++) {
            nt_write(vram, a, sram[i + (repeat ? 0 : k)]);
            a = (uint16_t)(a + inc);
        }
        i += repeat ? 1 : len;
    }
    sram[RAM_VRAM_BUF2_OFS] = 0;
    sram[RAM_VRAM_BUF2] = 0;
}

static void make_fill(void) {
    static uint8_t sram[2048], swram[2048];
    fill.gen = area_copy.gen;
    fill.ok = 0;
    fill.start = -1;
    memcpy(sram, area_copy.ram, sizeof sram);
    memcpy(swram, area_copy.wram, sizeof swram);
    cpu_t r = area_copy.cpu;
    sandbox_call(&r, sram, AREA_INIT);
    mem_sandbox_begin();
    if (cpu_sandbox_run(&r, AREA_START_PAGE, sram, swram, 100000) < 0) return;
    int page = r.a;
    if (page > 0) r.a = (uint8_t)(page - 1);            /* start a page earlier */
    if (cpu_sandbox_run(&r, SANDBOX_RETURN, sram, swram, 200000) < 0) return;   /* the rest of it */
    fill.start = page * 256;
    for (int k = 0; k < 5; k++) fill.id[k] = sram[area_id_addr[k]];
    if (page == 0) return;                                /* drawn from its beginning: nothing comes before it */
    memcpy(fill.vram, ppu.vram, sizeof fill.vram);
    for (int set = 0; set < FILL_W / 32; set++) {
        for (int task = 0; task < 8; task++) {
            cpu_t c = r;
            sandbox_call(&c, sram, PARSER_TASK);
            if (cpu_sandbox_run(&c, SANDBOX_RETURN, sram, swram, 100000) < 0) return;
            flush_vram_buffer(sram, fill.vram);
            if (sram[RAM_PARSER_TASKNUM] == 0) break;     /* a column set and its attributes are done */
        }
    }
    memcpy(fill.block, sram + RAM_BLOCK_BUFFER, sizeof fill.block);
    fill.ok = 1;
}

/* An area started part-way is rendered by the game from its start page on, after InitializeArea cleared
   the whole block buffer, so the page before it, which the band draws, would have nothing to stand on.
   Called as the area starts (bandwalk.c, with the start page), this gives the game that page's
   collision from the band's rendering of it, for the columns the player can reach beside the screen:
   its last 128 pixels, whose place in the block buffer the next page's last columns take only once
   the parser is 512 pixels past them, out of reach. (The first 128 share theirs with the next page's
   first columns, which the game renders at once.) */
int widescreen_fill_collision(int start_page) {
    if (!area_copy.valid) return 0;
    if (fill.gen != area_copy.gen) make_fill();
    if (!fill.ok || fill.start != start_page * 256) return 0;
    for (int k = 0; k < 3; k++)   /* world, level, area number: set before the start (the rest of the id is not yet) */
        if (fill.id[k] != ram[area_id_addr[k]]) return 0;
    int half = ((start_page - 1) & 1) * 0xD0, col = (FILL_W - FILL_SOLID_W) / 16;
    for (int row = 0; row < 13; row++) {
        int o = half + row * 16 + col;
        memcpy(ram + RAM_BLOCK_BUFFER + o, fill.block + o, (size_t)(16 - col));
    }
    fill.solid = fill.gen;
    return 1;
}

/* ---- the background ---- */
/* the line's background origin in the 512 x 480 nametable space */
static int line_origin_x(int ly) {
    uint16_t v = ppu.line_v[ly];
    return ((v & 0x400) ? 256 : 0) + (v & 0x1F) * 8 + ppu.line_fx[ly];
}
static int line_origin_y(int ly) {
    uint16_t v = ppu.line_v[ly];
    return ((v & 0x800) ? 240 : 0) + ((v >> 5) & 0x1F) * 8 + ((v >> 12) & 7);
}

/* this frame: the world pixel of each playfield line's left edge (INT_MIN on the status bar's lines,
   which do not scroll), the screen's, and whether the fill belongs to the area on screen */
static int line_world[240];
static int frame_left;
static int fill_on;

static void frame_area(int level) {
    int left = frame_left = ram[RAM_SCREEN_LEFT_PAGE] * 256 + ram[RAM_SCREEN_LEFT_X];
    if (level && area_copy.valid && fill.gen != area_copy.gen) make_fill();
    fill_on = widescreen_fill && level && fill.start >= 0 && left >= fill.start;
    for (int k = 0; fill_on && k < 5; k++)
        if (ram[area_id_addr[k]] != fill.id[k]) fill_on = 0;
    for (int ly = 0; ly < 240; ly++) {
        int d = (line_origin_x(ly) - left) & 511;
        if (d > 256) d -= 512;
        line_world[ly] = (d >= -8 && d <= 8) ? left + d : INT_MIN;
    }
}

/* the picture's columns [lo, hi) that hold the level (lo < 0 and hi > 256 reach into the bands);
   0 when this frame shows no level */
static int level_window(int *lo, int *hi);
int widescreen_level_window(int *lo, int *hi) { return level_window(lo, hi); }
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

/* The band's extent to the left of the screen as the RAM gives it (bandwalk.c's wall): the oldest
   nametable column (as level_window, without the PPU's lines, which are not machine state), which the
   block buffer holds as well; after a part-way start nothing before the start page unless the page
   before it went into the block buffer (widescreen_fill_collision), and then only that part of it;
   nothing before the area's start */
int widescreen_level_left(void) {
    int left = ram[RAM_SCREEN_LEFT_PAGE] * 256 + ram[RAM_SCREEN_LEFT_X];
    int front = ram[RAM_PARSER_PAGE] * 256 + ram[RAM_PARSER_COLUMN] * 16;
    int ahead = front - (left + 256);
    if (ahead < 0 || ahead > 192) return 0;
    int leftmost = front - 512 + MARGIN;
    if (leftmost < 0) leftmost = 0;
    if (!sys.sandbox && area_copy.valid && fill.gen != area_copy.gen) make_fill();
    int fill_here = widescreen_fill && fill.start >= 0 && left >= fill.start;
    for (int k = 0; fill_here && k < 5; k++)
        if (ram[area_id_addr[k]] != fill.id[k]) fill_here = 0;
    if (fill_here) {
        int solid = (fill.ok && fill.solid == fill.gen) ? fill.start - FILL_SOLID_W : fill.start;
        if (leftmost < solid) leftmost = solid;
    }
    return left > leftmost ? left - leftmost : 0;
}

/* one background pixel of line ly at picture column sx from the nametables in vram; *opaque = not colour 0 */
static uint8_t nt_pixel(const uint8_t *vram, int ly, int sx, int ox, int oy, int *opaque) {
    int wx = (ox + sx) & 511, wy = oy % 480;
    int nt = ((wy >= 240) ? 2 : 0) + (wx >> 8);
    int ty = (wy % 240) >> 3, tx = (wx & 255) >> 3;
    const uint8_t *ntp = vram + ppu.nt_offset[nt];
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
    *opaque = pix != 0;
    return c;
}

static uint8_t bg_pixel(int ly, int sx, int ox, int oy) {
    int opaque;
    return nt_pixel(ppu.vram, ly, sx, ox, oy, &opaque);
}

/* the background beside the picture at column sx of line ly: 1 with *c and *opaque where there is level
   (the game's nametables, or the page drawn before an area's start page), 0 where the band is black */
static int band_bg(int ly, int sx, int ox, int oy, int lo, int hi, uint8_t *c, int *opaque) {
    /* a status bar line has no world position of its own: the screen's decides, and the fill's copy of
       the nametables still holds the blank tiles beside the status bar there */
    int wx = (line_world[ly] != INT_MIN ? line_world[ly] : frame_left) + sx;
    if (fill_on && wx < fill.start) {
        if (!fill.ok || wx < fill.start - FILL_W) return 0;
        *c = nt_pixel(fill.vram, ly, sx, ox, oy, opaque);
        return 1;
    }
    if (sx < lo || sx >= hi) return 0;
    *c = nt_pixel(ppu.vram, ly, sx, ox, oy, opaque);
    return 1;
}

/* ---- the objects ---- */
/* the sprites beside the picture: for each side, the engine's sprites with the camera moved towards it,
   on the picture columns [c0, c1) of that side (the band within the level, and the edge columns the game
   hides), behind or in front of the background as the PPU would put them */
static void draw_sprites(uint8_t *out, int px0, int py0, int w, int h, int lo, int hi) {
    uint8_t layer[WS_W];   /* per column: 0 = none, else 0x40 | 0x80 if behind the background | colour */
    for (int side = 0; side < 2; side++) {
        int dx = side ? CAMERA_SHIFT : -CAMERA_SHIFT;
        int c0 = side ? 256 - EDGE : lo, c1 = side ? hi : EDGE;
        if (c0 < px0) c0 = px0;
        if (c1 > px0 + w) c1 = px0 + w;
        if (c0 >= c1) continue;
        uint8_t oam[256];
        if (!engine_oam(dx, oam, 0)) continue;
        /* only the sprites the camera placed: those that differ from the real frame's (sprite 0 does not).
           One the real frame shows that lands elsewhere has wrapped round the moved screen's 8-bit X:
           the player, whom the game never hides at the sides because he cannot leave the screen. */
        int list[64], n = 0;
        for (int i = 0; i < 64; i++) {
            const uint8_t *e = oam + i * 4, *p = ppu.oam + i * 4;
            if (e[0] >= 0xEF || !memcmp(e, p, 4)) continue;
            if (p[0] < 0xEF && e[0] == p[0] && e[1] == p[1] && e[2] == p[2] && e[3] + dx != p[3]) continue;
            list[n++] = i;
        }
        if (!n) continue;
        for (int r = 0; r < h; r++) {
            int ly = py0 + r;
            if (ly < 0 || ly >= 240 || !(ppu.line_emph[ly] & 0x10)) continue;
            int tall = (ppu.line_ctrl[ly] & 0x20) != 0, any = 0;
            memset(layer, 0, (size_t)(c1 - c0));
            for (int k = n - 1; k >= 0; k--) {   /* the lowest-numbered sprite wins, so it goes last */
                const uint8_t *e = oam + list[k] * 4;
                int row = ly - 1 - e[0];
                if (row < 0 || row >= (tall ? 16 : 8)) continue;
                int sx0 = e[3] + dx;   /* the camera was dx to the right: the sprite is dx further right on the picture */
                if (sx0 + 8 <= c0 || sx0 >= c1) continue;
                uint8_t tile = e[1], attr = e[2];
                if (attr & 0x80) row = (tall ? 15 : 7) - row;
                uint16_t table;
                if (tall) { table = (tile & 1) ? 0x1000 : 0x0000; tile &= 0xFE; if (row >= 8) { tile++; row -= 8; } }
                else table = (ppu.line_ctrl[ly] & 0x08) ? 0x1000 : 0x0000;
                uint16_t a = (uint16_t)(table + tile * 16 + row);
                const uint8_t *chr = mapper_chr_bank_ptr(ppu.frame_chr_bank[a >> 10]);
                uint8_t p0 = chr[a & 0x3FF], p1 = chr[(a + 8) & 0x3FF];
                for (int b = 0; b < 8; b++) {
                    int sx = sx0 + b;
                    if (sx < c0 || sx >= c1) continue;
                    int bit = (attr & 0x40) ? b : 7 - b;
                    int pix = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1);
                    if (!pix) continue;
                    layer[sx - c0] = (uint8_t)(0x40 | ((attr & 0x20) ? 0x80 : 0) | (ppu.palette[0x10 + (attr & 3) * 4 + pix] & 0x3F));
                    any = 1;
                }
            }
            if (!any) continue;
            int ox = line_origin_x(ly), oy = line_origin_y(ly);
            int bg_on = (ppu.line_emph[ly] & 0x08) != 0;
            uint8_t *dst = out + (size_t)r * w;
            for (int sx = c0; sx < c1; sx++) {
                uint8_t s = layer[sx - c0];
                if (!s) continue;
                uint8_t *d = dst + (sx - px0);
                int opaque = 0;
                uint8_t bg = ppu.palette[0];
                if (sx >= 0 && sx < 256) {
                    if (sx < 8 && (ppu.line_emph[ly] & 0x06) != 0x06) continue;   /* left 8 pixels clipped */
                    if (bg_on) bg = nt_pixel(ppu.vram, ly, sx, ox, oy, &opaque);
                    if (*d != bg) continue;                                      /* the PPU drew a sprite there itself */
                } else {
                    uint8_t c;
                    if (!band_bg(ly, sx, ox, oy, lo, hi, &c, &opaque)) continue; /* no sprites over the black */
                    if (!bg_on) opaque = 0;
                }
                if ((s & 0x80) && opaque) continue;   /* behind an opaque background pixel */
                uint8_t c = s & 0x3F;
                if (ppu.line_emph[ly] & 1) c &= 0x30;
                *d = c;
            }
        }
    }
}

int widescreen_render(uint8_t *out, uint8_t *emph, int x, int y, int w, int h) {
    if (sys.hook != engine_hook) widescreen_attach();
    int px0 = x - WS_BAND, py0 = y - WS_VBAND;   /* the rectangle in the picture's coordinates */
    int lo = 0, hi = 256, level = level_window(&lo, &hi);
    frame_area(level);
    uint8_t black = black_index();
    for (int r = 0; r < h; r++) {
        int ly = py0 + r;
        uint8_t *dst = out + (size_t)r * w;
        if (ly < 0 || ly >= 240) { memset(dst, black, (size_t)w); emph[r] = 0; continue; }
        emph[r] = ppu.line_emph[ly];
        int drawn = (ppu.line_emph[ly] & 0x08) != 0;
        uint8_t backdrop = (ppu.line_emph[ly] & 1) ? (uint8_t)(ppu.palette[0] & 0x30) : ppu.palette[0];
        int ox = line_origin_x(ly), oy = line_origin_y(ly);
        for (int c = 0; c < w; c++) {
            int sx = px0 + c, opaque;
            uint8_t v;
            if (sx >= 0 && sx < 256) dst[c] = ppu.fb[ly * 256 + sx];
            else if (!level || !band_bg(ly, sx, ox, oy, lo, hi, &v, &opaque)) dst[c] = black;
            else dst[c] = drawn ? v : backdrop;
        }
    }
    if (level && widescreen_sprites) draw_sprites(out, px0, py0, w, h, lo, hi);
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

int widescreen_sprite_check(void) {
    if (sys.hook != engine_hook) widescreen_attach();
    uint8_t oam[256];
    if (!engine_oam(0, oam, 1)) return -1;
    return memcmp(oam, ppu.oam, 256) == 0;
}

int widescreen_visible_width(int lines, double pixel_aspect, double ratio) {
    int w = (int)(lines * ratio / pixel_aspect + 0.5);
    if (w > WS_W) w = WS_W;
    if (w < 256) w = 256;
    return w;
}
