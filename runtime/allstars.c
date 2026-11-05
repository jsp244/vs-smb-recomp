/*
 * allstars.c - the remastered graphics (allstars.h): VS. Super Mario Bros. as Super Mario All-Stars
 * draws Super Mario Bros.
 *
 * All-Stars' Super Mario Bros. (banks $03-$0C of its ROM) is the NES engine rewritten for the SNES:
 * the same RAM layout for most of the game, the same metatile and object logic, new graphics. Its
 * level picture is built by its own code, so this file runs that code instead of describing what
 * it does: a sandbox SNES (the 65C816 of cpu816.c, 128 KiB of work RAM, VRAM, CGRAM, DMA and HDMA,
 * the PPU registers into a snesppu_t) runs
 *
 *   - the game's start-up ($03:8000, up to its wait for the first NMI): base tiles, registers;
 *   - for each area VS. starts, All-Stars' InitializeArea ($03:9E0C) and its area graphics setup
 *     ($04:956B: tilesets, palettes, the parallax hills of layer 2 and clouds of layer 3, the sky's
 *     gradient in HDMA), for the All-Stars area that is most like VS.'s (VS. numbers its areas
 *     differently and some of its levels are its own: see map_areas), then its NMI ($03:8003),
 *     which uploads what the setup queued;
 *   - All-Stars' AreaParserCore ($03:A436) for every column VS.'s own parser renders, on VS.'s level
 *     data (copied from VS.'s work RAM and converted to All-Stars' format: objects in row 15 have a
 *     third byte there), so the level's metatiles are All-Stars' own choice, with its variants (the
 *     bushes one row lower, bridge rails with ends, castle walls in two patterns...).
 *
 * The rest of a frame comes from VS.'s machine: the camera (layer 1 scrolls with it, layer 2 at half
 * and layer 3 at a quarter of its speed, as All-Stars does), the blocks the game changes during play
 * (its block buffer), the sprites (VS.'s OAM: enemies and items use the NES tile numbers in All-Stars
 * too, each object type with All-Stars' palette; the player's frame is the 8 tiles All-Stars keeps
 * per pose in bank $0A, by the same pose number, in All-Stars' colours for Mario, Luigi, fire and
 * the star; fireballs, coins, hammers, lifts, bubbles and bouncing blocks get the tiles and sizes
 * All-Stars' own handlers use), and the status bar (VS.'s nametable, in All-Stars' font on layer 3).
 * snesppu.c then draws the lines, with the HDMA and the IRQ split of the status bar replayed from the
 * sandbox's registers; in the widescreen canvas the bands get the sprites the game builds beside the
 * picture (widescreen_band_oam) and black beyond the level it holds.
 *
 * The screens that are not a level run in a second sandbox: the intermission is All-Stars' own
 * (its player head, world picture and text routines), the title is All-Stars' banner (its
 * DrawTitleScreen) moved to VS.'s place with "VS." put in front, and the screens only the arcade has
 * (PUSH BUTTON, the continue countdown, game over, the coin and credit lines) are VS.'s text in
 * All-Stars' fonts on the intermission's backdrop. What All-Stars has no art for is made from the
 * nearest art it has: the V from its U (make_v), the cabinet's buttons and the timer's box from the
 * arcade's pixels, lit and outlined as All-Stars draws (screen_icons).
 *
 * Addresses below are those of the USA ROM (and the Wii and Switch Online copies, whose banks $03-$0C
 * are the same); they are knowledge about the program, not data from it.
 */
#include "allstars.h"
#include "widescreen.h"
#include "cpu816.h"
#include "snesppu.h"
#include "spc700.h"
#include "nes.h"
#include "palette.h"
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool allstars_enabled = true;

/* ---- the ROM ---- */
static const struct { const char *sha1; const char *name; } known[] = {
    { "c05817c5b7df2fbfe631563e0b37237156a8f6b6", "Super Mario All-Stars (USA)" },
    { "aea8b8a6ebb6603831ea02409757e76ee4c62377", "Super Mario All-Stars (USA) (Wii)" },
    { "9779d804befecd7f79933a947f4d5843d50dd652", "Super Mario All-Stars (USA, Europe) (Switch Online)" },
    { NULL, NULL }
};

static struct {
    uint8_t *rom;
    size_t   size;
    char     path[1024], name[96], sha1_hex[41], md5_hex[33];
    uint32_t crc32;
} as;

/* All-Stars addresses (USA) */
#define SMB1_START       0x038000   /* JMP to the game's start-up */
#define SMB1_START_WAIT  0x0381A9   /* the start-up's wait for the first NMI */
#define SMB1_NMI         0x038003   /* JMP to the NMI routine, which returns with RTL */
#define INIT_AREA        0x039E0C   /* InitializeArea (JSR) */
#define AREA_GRAPHICS    0x04956B   /* the area's tilesets, palettes, layers 2 and 3, HDMA (JSL) */
#define AREA_COLORS      0x049A88   /* the sky's color into the fixed color (color 0 black), the player's palette */
#define PARSER_CORE      0x03A436   /* AreaParserCore (JSR) */
#define CLEAR_SCREEN     0x048000   /* layers 1 and 2 to tile $24, layer 3 to $28 (JSL) */
#define INTER_PLAYER     0x03F861   /* the intermission's player (the head icon) and the world picture's objects (JSR) */
#define INTER_PICTURE    0x0492E7   /* the intermission's world picture (JSL) */
#define WRITE_TEXT       0x0390FC   /* WriteGameText, A = 1: the world and the lives (JSR) */
#define INTER_LIVES      0x039224   /* the lives' digits (JSR) */
#define SPR_OFFSETS      0x039D70   /* the OAM offsets InitializeArea gives the player and the objects ($0B45-$0B61) */
#define OAM_BUFFER       0x0800     /* where All-Stars builds its OAM (544 bytes) */
#define OAM_SIZES        0x05DD7A   /* each sprite's size and X bit 8 ($0C00 + 4n) into the OAM's high table, once a frame ($03:82D4) (JSL) */
#define MAP16_LO         0x039438   /* low bytes of the four metatile groups' tables (bank $03) */
#define MAP16_HI         0x03943C
#define FLOATEY_TILES    0x038B0C   /* the floating points' two tiles per value (its FloateyNumTileData) */
#define AS_USED_BLOCK    0x03BEC6   /* PlayerHeadCollision: LDY #used block */
#define AS_BUMP_PLACE    0x03BED1   /* ... LDA #metatile / STA ($06),y: the place of a bouncing block */
#define AS_VINE_CELL     0x03BA99   /* the vine: LDA #metatile / STA ($06),y, the cells it climbs */
#define LEVEL_AREA_PTR   0x04C01F   /* LoadAreaPointer's lookup: the level's first area pointer in A, by world and area number (JSL) */
#define AREA_TYPE_OFS    0x04C190   /* first area index of each area type */
#define AREA_PTR_LO      0x04C194   /* area data pointers by area index (bank $04) */
#define AREA_PTR_HI      0x04C1B6
#define PLAYER_COLORS    0x0499FD   /* the player's 16 colours: Mario, Luigi, then fiery Mario and Luigi ($20 bytes each) */
#define STAR_COLORS      0x05ED40   /* the star's cycle: 4 sets of 16 colours, Luigi's a set later */
#define BOWSER_TILES     0x05CF20   /* Bowser's 7 frames of 6 tiles facing right, then the 7 facing left ($05:D055) */
#define FLAME_TILES      0x05CDE2   /* Bowser's flame: 3 frames ($05:CD01), then its spark's 3 */
#define BULLET_TILES     0x05CB38   /* a Bullet Bill's 10 frames ($05:CB42) */
#define SHELL_TILES      0x05E9E8   /* shells: 4 turns of 2x2 tiles, Koopas', Buzzy Beetles', then upside down ($05:E879) */
#define SHELL_FLIPS      0x05EA28   /* the turns' mirroring */
#define CLOUD_TILES      0x05CEDB   /* the cloud lift: 4 frames of 8 tiles, their attributes and places ($05:CDEB) */
#define CLOUD_ATTRS      0x05CEFB
#define CLOUD_X          0x05CF1B
#define PRINCESS_TILES   0x04DD4E   /* the princess's poses ($04:DCBB) */
#define FIREWORK_TILES   0x03F53B   /* a firework's 4 rings ($03:F548), then the palettes it cycles through */
#define FIREWORK_PALS    0x03F53F
#define RETAINER_GFX     0x1EC000   /* the sack's OBJ tiles, then the princess's ($04:ECCA) */
#define RETAINER_SETS    0x04ED5A   /* each world's second set: Toad's ($04:ED07) */
#define RETAINER_DRAW    0x04ED7A   /* Toad's sack, then each world's scene */
#define PLAYER_FRAMES    0x0A8000   /* 256 bytes (8 tiles) per pose number / 8; Luigi +$4000 */
#define ENEMY_ATTR       0x03EEA9   /* sprite attributes per enemy type */
#define ENEMY_GFX        0x03ED8C   /* EnemyGraphicsTable: 6 tiles per frame, NES numbers mostly */
#define POWERUP_GFX      0x03ECEF   /* PowerUpGfxTable: 4 tiles per power-up */
#define BG3_MAP16        0x04BD09   /* layer 3's 16x16 tiles: 4 tilemap words each (top left, bottom left, top right, bottom right) */
#define ANIM_TILES       0x06C000   /* 8 frames of $800 bytes; the first $600 go to BG tiles $C0-$EF */
#define ANIM_HUD_TILES   0x0CFB00   /* 8 frames of $40 bytes for BG3 tiles $2C-$2F (the status bar's coin) */
#define N_SMAS_AREAS     0x22

/* VS. Super Mario Bros. addresses */
#define VS_ENEMY_ATTR    0xE7B8     /* EnemyAttributeData */
#define VS_PLAYER_GFX    0xED74     /* PlayerGraphicsTable: the 8 tiles of each of the 26 poses */
#define VS_ENEMY_GFX     0xE69B     /* EnemyGraphicsTable */
#define VS_BB_BOUNDS     0xA3DF     /* BlockBuffLowBounds: per metatile group, the first one stored in the block buffer */
#define VS_POWERUP_GFX   0xE61B     /* PowerUpGfxTable */
#define VS_FLOATEY       0x867D     /* FloateyNumTileData */
#define VS_LOOP_WORLD    0xBF84     /* LoopCmdWorldNumber, then LoopCmdPageNumber */
#define VS_LOOP_PAGE     0xBF8F
#define VS_LOOP_OFS      0x6000     /* AreaDataOfsLoopback: ExecGameLoopback ($BFA5) reads it from PRG RAM */
#define N_LOOPS          11
#define ENEMY_GFX_LEN    0x102
#define VS_METATILE_LO   0x8D09     /* MetatileGraphics_Low/High */
#define VS_AREA_OFS      0xACF5     /* area data: first index of each type; CHR addresses at $ACF9 */
#define VS_AREA_ADDR     0xACF9
#define VS_ENEMY_ADDR    0xACA7     /* enemy data CHR addresses: as many as there are areas */

/* RAM of the engine (both games) */
#define RAM_AREA_PTR     0x0750
#define RAM_WORLD        0x075F
#define RAM_LEVEL        0x075C
#define RAM_AREA_NUM     0x0760
#define RAM_AREA_TYPE    0x074E
#define RAM_PAGE         0x0725     /* the parser's page and column */
#define RAM_COLUMN       0x0726
#define RAM_BACKLOAD     0x0728
#define RAM_OBJ_PAGE     0x072A     /* AreaObjectPageLoc, AreaObjectPageSel, AreaDataOffset */
#define RAM_OBJ_PAGE_SEL 0x072B
#define RAM_DATA_OFS     0x072C
#define RAM_BB_COLUMN    0x06A0     /* BlockBufferColumnPos */
#define RAM_MT_BUFFER    0x06A1     /* MetatileBuffer: the column's 13 metatiles */
#define RAM_AS_TYPE      0x005C     /* All-Stars': the area's type, and the style its graphics take (3 a castle's) */
#define RAM_AS_STYLE     0x00BA

static uint8_t rom8(uint32_t a) {
    uint32_t off = ((a >> 16) & 0x7F) * 0x8000u + (a & 0x7FFF);
    return off < as.size ? as.rom[off] : 0;
}
static uint16_t rom16(uint32_t a) { return (uint16_t)(rom8(a) | (rom8(a + 1) << 8)); }

/* ---- the sandbox SNES ---- */
typedef struct {
    cpu816_t  cpu;
    uint8_t   wram[0x20000];
    uint8_t   sram[0x2000];
    snesppu_t ppu;
    uint8_t   oam[544];
    uint16_t  oamaddr, oamaddr_reload;
    uint8_t   oam_latch;
    uint16_t  vaddr, vread;
    uint8_t   vmain;
    uint8_t   cgaddr, cg_latch, cg_hi;
    uint8_t   ofs_latch, hofs_latch;
    uint32_t  wmaddr;
    uint8_t   dma[8][16];
    uint8_t   hdma_en;
    uint8_t   apu[4];
    uint8_t   wrmpya;
    uint16_t  wrdiv, rddiv, rdmpy;
    int16_t   m7a;
    uint8_t   m7b_prev;
    int32_t   mpy;
} sbx_t;

static sbx_t *sb, *sb_base, *sb_screen;   /* the area's, the start-up's, the text screens' */

static uint8_t sb_io_read(sbx_t *s, uint16_t a);
static void sb_io_write(sbx_t *s, uint16_t a, uint8_t v);

static uint8_t sb_read(void *ctx, uint32_t a) {
    sbx_t *s = ctx;
    uint8_t bank = (uint8_t)(a >> 16);
    uint16_t off = (uint16_t)a;
    if (bank == 0x7E || bank == 0x7F) return s->wram[a - 0x7E0000];
    if ((bank & 0x7F) < 0x40) {
        if (off < 0x2000) return s->wram[off];
        if (off < 0x8000) return sb_io_read(s, off);
        return rom8(a);
    }
    if ((bank & 0x7F) >= 0x70 && (bank & 0x7F) < 0x7E && off < 0x8000) return s->sram[off & 0x1FFF];
    return rom8(a);
}

static void sb_write(void *ctx, uint32_t a, uint8_t v) {
    sbx_t *s = ctx;
    uint8_t bank = (uint8_t)(a >> 16);
    uint16_t off = (uint16_t)a;
    if (bank == 0x7E || bank == 0x7F) { s->wram[a - 0x7E0000] = v; return; }
    if ((bank & 0x7F) < 0x40) {
        if (off < 0x2000) s->wram[off] = v;
        else if (off < 0x8000) sb_io_write(s, off, v);
        return;
    }
    if ((bank & 0x7F) >= 0x70 && (bank & 0x7F) < 0x7E && off < 0x8000) s->sram[off & 0x1FFF] = v;
}

static uint16_t vremap(const sbx_t *s, uint16_t a) {
    switch ((s->vmain >> 2) & 3) {
    case 1: return (uint16_t)((a & 0xFF00) | ((a & 0x001F) << 3) | ((a >> 5) & 7));
    case 2: return (uint16_t)((a & 0xFE00) | ((a & 0x003F) << 3) | ((a >> 6) & 7));
    case 3: return (uint16_t)((a & 0xFC00) | ((a & 0x007F) << 3) | ((a >> 7) & 7));
    }
    return a;
}
static uint16_t vinc(const sbx_t *s) { static const uint16_t step[4] = { 1, 32, 128, 128 }; return step[s->vmain & 3]; }
static void vprefetch(sbx_t *s) {
    uint16_t w = vremap(s, s->vaddr) & 0x7FFF;
    s->vread = (uint16_t)(s->ppu.vram[w * 2] | (s->ppu.vram[w * 2 + 1] << 8));
}

static uint8_t sb_io_read(sbx_t *s, uint16_t a) {
    if (a >= 0x2140 && a < 0x2180) return s->apu[a & 3];   /* the sound CPU echoes */
    switch (a) {
    case 0x2134: return (uint8_t)s->mpy;
    case 0x2135: return (uint8_t)(s->mpy >> 8);
    case 0x2136: return (uint8_t)(s->mpy >> 16);
    case 0x2138: { uint8_t v = s->oam[s->oamaddr % 544]; s->oamaddr = (uint16_t)((s->oamaddr + 1) % 544); return v; }
    case 0x2139: { uint8_t v = (uint8_t)s->vread; if (!(s->vmain & 0x80)) { vprefetch(s); s->vaddr += vinc(s); } return v; }
    case 0x213A: { uint8_t v = (uint8_t)(s->vread >> 8); if (s->vmain & 0x80) { vprefetch(s); s->vaddr += vinc(s); } return v; }
    case 0x213B: {
        uint16_t c = s->ppu.cgram[s->cgaddr];
        uint8_t v = s->cg_hi ? (uint8_t)(c >> 8) : (uint8_t)c;
        if (s->cg_hi) s->cgaddr++;
        s->cg_hi ^= 1;
        return v; }
    case 0x213E: return 0x01;
    case 0x213F: return 0x03;
    case 0x2180: { uint8_t v = s->wram[s->wmaddr & 0x1FFFF]; s->wmaddr = (s->wmaddr + 1) & 0x1FFFF; return v; }
    case 0x4016: case 0x4017: return 0x01;   /* a controller is connected, nothing pressed */
    case 0x4210: return 0x82;                /* vblank has started */
    case 0x4211: return 0x00;
    case 0x4212: return 0xC0;                /* in vblank and hblank, joypad read done */
    case 0x4214: return (uint8_t)s->rddiv;
    case 0x4215: return (uint8_t)(s->rddiv >> 8);
    case 0x4216: return (uint8_t)s->rdmpy;
    case 0x4217: return (uint8_t)(s->rdmpy >> 8);
    }
    if (a >= 0x4300 && a < 0x4380) return s->dma[(a >> 4) & 7][a & 15];
    return 0;
}

static void vram_write(sbx_t *s, int hi, uint8_t v) {
    uint16_t w = vremap(s, s->vaddr) & 0x7FFF;
    s->ppu.vram[w * 2 + hi] = v;
    if (((s->vmain & 0x80) != 0) == hi) s->vaddr += vinc(s);
}

static void sb_dma(sbx_t *s, int ch);

/* the PPU registers that hold no memory ($2100-$2114, $2123-$2133): into p, with the scroll
   registers' write latches */
static int snes_reg_write(snesppu_t *p, uint8_t *ofs_latch, uint8_t *hofs_latch, uint16_t a, uint8_t v) {
    switch (a) {
    case 0x2100: p->inidisp = v; return 1;
    case 0x2101: p->obsel = v; snesppu_obsel(p); return 1;
    case 0x2105: p->bgmode = v; return 1;
    case 0x2106: p->mosaic = v; return 1;
    case 0x2107: case 0x2108: case 0x2109: case 0x210A: p->bgsc[a - 0x2107] = v; return 1;
    case 0x210B: p->bgnba[0] = v; return 1;
    case 0x210C: p->bgnba[1] = v; return 1;
    case 0x210D: case 0x210F: case 0x2111: case 0x2113: {
        int b = (a - 0x210D) / 2;
        p->hofs[b] = (uint16_t)(((v << 8) | (*ofs_latch & ~7) | (*hofs_latch & 7)) & 0x3FF);
        *ofs_latch = v; *hofs_latch = v;
        return 1; }
    case 0x210E: case 0x2110: case 0x2112: case 0x2114: {
        int b = (a - 0x210E) / 2;
        p->vofs[b] = (uint16_t)(((v << 8) | *ofs_latch) & 0x3FF);
        *ofs_latch = v;
        return 1; }
    case 0x2123: p->w12sel = v; return 1;
    case 0x2124: p->w34sel = v; return 1;
    case 0x2125: p->wobjsel = v; return 1;
    case 0x2126: case 0x2127: case 0x2128: case 0x2129: p->wh[a - 0x2126] = v; return 1;
    case 0x212A: p->wbglog = v; return 1;
    case 0x212B: p->wobjlog = v; return 1;
    case 0x212C: p->tm = v; return 1;
    case 0x212D: p->ts = v; return 1;
    case 0x212E: p->tmw = v; return 1;
    case 0x212F: p->tsw = v; return 1;
    case 0x2130: p->cgwsel = v; return 1;
    case 0x2131: p->cgadsub = v; return 1;
    case 0x2132:
        if (v & 0x20) p->fixed_r = v & 31;
        if (v & 0x40) p->fixed_g = v & 31;
        if (v & 0x80) p->fixed_b = v & 31;
        return 1;
    case 0x2133: p->setini = v; return 1;
    }
    return 0;
}

static void sb_io_write(sbx_t *s, uint16_t a, uint8_t v) {
    snesppu_t *p = &s->ppu;
    if (a >= 0x2140 && a < 0x2180) { s->apu[a & 3] = v; return; }
    if (snes_reg_write(p, &s->ofs_latch, &s->hofs_latch, a, v)) return;
    switch (a) {
    case 0x2102: s->oamaddr_reload = (uint16_t)((s->oamaddr_reload & 0x100) | v); s->oamaddr = (uint16_t)(s->oamaddr_reload << 1); return;
    case 0x2103: s->oamaddr_reload = (uint16_t)((s->oamaddr_reload & 0xFF) | ((v & 1) << 8)); s->oamaddr = (uint16_t)(s->oamaddr_reload << 1); return;
    case 0x2104:
        if (s->oamaddr >= 512) s->oam[512 + (s->oamaddr & 31)] = v;
        else if (!(s->oamaddr & 1)) s->oam_latch = v;
        else { s->oam[s->oamaddr - 1] = s->oam_latch; s->oam[s->oamaddr] = v; }
        s->oamaddr = (uint16_t)((s->oamaddr + 1) % 544);
        return;
    case 0x2115: s->vmain = v; return;
    case 0x2116: s->vaddr = (uint16_t)((s->vaddr & 0xFF00) | v); vprefetch(s); return;
    case 0x2117: s->vaddr = (uint16_t)((s->vaddr & 0x00FF) | (v << 8)); vprefetch(s); return;
    case 0x2118: vram_write(s, 0, v); return;
    case 0x2119: vram_write(s, 1, v); return;
    case 0x211B: s->m7a = (int16_t)((v << 8) | s->m7b_prev); s->m7b_prev = v; s->mpy = (int32_t)s->m7a * (int8_t)s->m7b_prev; return;
    case 0x211C: s->m7b_prev = v; s->mpy = (int32_t)s->m7a * (int8_t)v; return;
    case 0x2121: s->cgaddr = v; s->cg_hi = 0; return;
    case 0x2122:
        if (!s->cg_hi) s->cg_latch = v;
        else { p->cgram[s->cgaddr] = (uint16_t)((s->cg_latch | (v << 8)) & 0x7FFF); s->cgaddr++; }
        s->cg_hi ^= 1;
        return;
    case 0x2180: s->wram[s->wmaddr & 0x1FFFF] = v; s->wmaddr = (s->wmaddr + 1) & 0x1FFFF; return;
    case 0x2181: s->wmaddr = (s->wmaddr & 0x1FF00) | v; return;
    case 0x2182: s->wmaddr = (s->wmaddr & 0x100FF) | ((uint32_t)v << 8); return;
    case 0x2183: s->wmaddr = (s->wmaddr & 0x0FFFF) | ((uint32_t)(v & 1) << 16); return;
    case 0x4202: s->wrmpya = v; return;
    case 0x4203: s->rdmpy = (uint16_t)(s->wrmpya * v); return;
    case 0x4204: s->wrdiv = (uint16_t)((s->wrdiv & 0xFF00) | v); return;
    case 0x4205: s->wrdiv = (uint16_t)((s->wrdiv & 0x00FF) | (v << 8)); return;
    case 0x4206:
        if (v) { s->rddiv = (uint16_t)(s->wrdiv / v); s->rdmpy = (uint16_t)(s->wrdiv % v); }
        else { s->rddiv = 0xFFFF; s->rdmpy = s->wrdiv; }
        return;
    case 0x420B: for (int ch = 0; ch < 8; ch++) if (v & (1 << ch)) sb_dma(s, ch); return;
    case 0x420C: s->hdma_en = v; return;
    }
    if (a >= 0x4300 && a < 0x4380) s->dma[(a >> 4) & 7][a & 15] = v;
}

static const uint8_t dma_pattern[8][4] = { {0,0,0,0},{0,1,0,1},{0,0,0,0},{0,0,1,1},{0,1,2,3},{0,1,0,1},{0,0,0,0},{0,0,1,1} };
static const uint8_t dma_units[8] = { 1, 2, 2, 4, 4, 4, 2, 4 };

static void sb_dma(sbx_t *s, int ch) {
    uint8_t *r = s->dma[ch];
    uint8_t mode = r[0] & 7;
    uint32_t src = (uint32_t)(r[2] | (r[3] << 8)), bank = r[4];
    uint32_t count = (uint32_t)(r[5] | (r[6] << 8));
    if (!count) count = 0x10000;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t b = (uint16_t)(0x2100 | (uint8_t)(r[1] + dma_pattern[mode][i & 3]));
        uint32_t aa = (bank << 16) | src;
        if (r[0] & 0x80) sb_write(s, aa, sb_io_read(s, b));
        else sb_io_write(s, b, sb_read(s, aa));
        if (!(r[0] & 0x08)) src = (r[0] & 0x10) ? (src - 1) & 0xFFFF : (src + 1) & 0xFFFF;
    }
    r[2] = (uint8_t)src; r[3] = (uint8_t)(src >> 8);
    r[5] = 0; r[6] = 0;
}

/* HDMA, for drawing: the channels' state through a frame, on a copy of their registers */
typedef struct { uint8_t r[8][16]; uint8_t active, again[8]; } hdma_t;

static void hdma_start(const sbx_t *s, hdma_t *h) {
    memcpy(h->r, s->dma, sizeof h->r);
    h->active = s->hdma_en;
    for (int ch = 0; ch < 8; ch++) {
        if (!(h->active & (1 << ch))) continue;
        uint8_t *r = h->r[ch];
        uint32_t bank = r[4];
        uint16_t tab = (uint16_t)(r[2] | (r[3] << 8));
        uint8_t n = sb_read((void *)s, (bank << 16) | tab); tab++;
        r[10] = n;
        if (r[0] & 0x40) {
            r[5] = sb_read((void *)s, (bank << 16) | tab);
            r[6] = sb_read((void *)s, (bank << 16) | (uint16_t)(tab + 1));
            tab += 2;
        }
        r[8] = (uint8_t)tab; r[9] = (uint8_t)(tab >> 8);
        if (!n) h->active &= (uint8_t)~(1 << ch);
        h->again[ch] = 1;
    }
}

/* one line of HDMA: its writes go to the PPU registers in p */
static void hdma_line(const sbx_t *s, hdma_t *h, snesppu_t *p, uint8_t *latch) {
    for (int ch = 0; ch < 8; ch++) {
        if (!(h->active & (1 << ch))) continue;
        uint8_t *r = h->r[ch];
        uint8_t mode = r[0] & 7;
        uint32_t bank = r[4];
        uint16_t tab = (uint16_t)(r[8] | (r[9] << 8));
        if (h->again[ch]) {
            for (int i = 0; i < dma_units[mode]; i++) {
                uint8_t v;
                if (r[0] & 0x40) {
                    uint16_t ia = (uint16_t)(r[5] | (r[6] << 8));
                    v = sb_read((void *)s, ((uint32_t)r[7] << 16) | ia);
                    ia++;
                    r[5] = (uint8_t)ia; r[6] = (uint8_t)(ia >> 8);
                } else v = sb_read((void *)s, (bank << 16) | tab++);
                snes_reg_write(p, &latch[0], &latch[1], (uint16_t)(0x2100 | (uint8_t)(r[1] + dma_pattern[mode][i])), v);
            }
        }
        r[10]--;
        h->again[ch] = (r[10] & 0x80) != 0;
        if (!(r[10] & 0x7F)) {
            uint8_t n = sb_read((void *)s, (bank << 16) | tab); tab++;
            r[10] = n;
            if (r[0] & 0x40) {
                r[5] = sb_read((void *)s, (bank << 16) | tab);
                r[6] = sb_read((void *)s, (bank << 16) | (uint16_t)(tab + 1));
                tab += 2;
            }
            if (!n) h->active &= (uint8_t)~(1 << ch);
            h->again[ch] = 1;
        }
        r[8] = (uint8_t)tab; r[9] = (uint8_t)(tab >> 8);
    }
}

static int sb_call(sbx_t *s, uint32_t addr, int rts, int max_steps) {
    s->cpu.ctx = s; s->cpu.read = sb_read; s->cpu.write = sb_write;
    s->cpu.e = 0;
    s->cpu.p = (uint8_t)((s->cpu.p | P816_M | P816_X | P816_I) & ~P816_D);
    s->cpu.d = 0;
    s->cpu.db = (uint8_t)(addr >> 16);
    s->cpu.x &= 0xFF; s->cpu.y &= 0xFF;
    s->cpu.s = 0x01FF;
    return cpu816_call(&s->cpu, addr, rts, max_steps);
}

/* ---- the game's start-up ---- */
static int startup(void) {
    if (!sb_base) sb_base = calloc(1, sizeof *sb_base);
    if (!sb) sb = calloc(1, sizeof *sb);
    if (!sb_screen) sb_screen = calloc(1, sizeof *sb_screen);
    if (!sb_base || !sb || !sb_screen) return -1;
    memset(sb_base, 0, sizeof *sb_base);
    sbx_t *s = sb_base;
    s->cpu.ctx = s; s->cpu.read = sb_read; s->cpu.write = sb_write;
    s->cpu.e = 0; s->cpu.p = 0x34; s->cpu.s = 0x01FF; s->cpu.pc = (uint16_t)SMB1_START; s->cpu.pb = (uint8_t)(SMB1_START >> 16);
    s->cpu.db = 0;
    for (int n = 0; n < 2000000; n++) {
        if ((((uint32_t)s->cpu.pb << 16) | s->cpu.pc) == SMB1_START_WAIT) {
            s->wram[0x0154] = 0;
            return 0;
        }
        cpu816_step(&s->cpu);
        if (s->cpu.stopped || s->cpu.trap) break;
    }
    fprintf(stderr, "allstars: the game's start-up did not reach its NMI wait (pc %02X:%04X)\n", s->cpu.pb, s->cpu.pc);
    return -1;
}

/* ---- areas: VS.'s to All-Stars' ---- */

/* VS. keeps its level data in CHR ROM bank 1; this is its area data (header and objects up to $FD) */
static int vs_area_data(int type, int num, uint8_t *out, int max) {
    if (!cart.chr || cart.chr_size < 0x4000 || cart.prg_size < 0x8000) return 0;
    uint16_t idx = (uint16_t)((cart.prg[VS_AREA_OFS - 0x8000 + type] + num) * 2);
    uint16_t a = (uint16_t)(cart.prg[VS_AREA_ADDR - 0x8000 + idx] | (cart.prg[VS_AREA_ADDR - 0x8000 + idx + 1] << 8));
    int n = 0;
    while (n < max) {
        uint32_t off = 0x2000u + (a & 0x1FFFu) + (uint32_t)n;
        if (off >= cart.chr_size) break;
        out[n] = cart.chr[off];
        if (out[n++] == 0xFD) break;
    }
    return n;
}

/* the NES format's area objects as All-Stars stores them: an object in row 15 gets a third byte, its
   type and page flag, and keeps its length in the second byte's low nibble with $F0 above it */
static int to_allstars_format(const uint8_t *in, int n, uint8_t *out, int max) {
    int i = 2, o = 2;
    if (n < 2 || max < 3) return 0;
    out[0] = in[0]; out[1] = in[1];
    while (i + 1 < n && in[i] != 0xFD && o + 3 < max) {
        uint8_t b1 = in[i], b2 = in[i + 1];
        out[o++] = b1;
        if ((b1 & 0x0F) == 0x0F) { out[o++] = (uint8_t)(0xF0 | (b2 & 0x0F)); out[o++] = (uint8_t)(b2 & 0xF0); }
        else out[o++] = b2;
        i += 2;
    }
    out[o++] = 0xFD;
    return o;
}

/* an area's objects as 16-bit keys (All-Stars format in; three-byte objects keep their first two) */
static int object_keys(const uint8_t *d, int n, uint16_t *keys, int max) {
    int i = 2, k = 0;
    while (i + 1 < n && d[i] != 0xFD && k < max) {
        int three = (d[i] & 0x0F) == 0x0F;
        keys[k++] = (uint16_t)((d[i] << 8) | (three ? (d[i + 2] | (d[i + 1] & 0x0F)) : d[i + 1]));
        i += three ? 3 : 2;
    }
    return k;
}

static int lcs(const uint16_t *a, int na, const uint16_t *b, int nb) {
    static int row[2][260];
    if (na > 256) na = 256;
    if (nb > 256) nb = 256;
    memset(row, 0, sizeof row);
    for (int i = 1; i <= na; i++) {
        int *cur = row[i & 1], *prev = row[(i - 1) & 1];
        for (int j = 1; j <= nb; j++)
            cur[j] = a[i - 1] == b[j - 1] ? prev[j - 1] + 1 : (prev[j] > cur[j - 1] ? prev[j] : cur[j - 1]);
    }
    return row[na & 1][nb];
}

static uint8_t area_map[4][32];   /* VS. area (type, number) -> All-Stars area index */

static int smas_area_data(int db, uint8_t *out, int max) {
    uint32_t a = 0x040000u | rom8(AREA_PTR_LO + (uint32_t)db) | ((uint32_t)rom8(AREA_PTR_HI + (uint32_t)db) << 8);
    int n = 0;
    while (n < max) {
        out[n] = rom8(a + (uint32_t)n);
        if (out[n++] == 0xFD) break;
    }
    return n;
}

/* an area header's look: the background control (night, snow: the first byte's low 3 bits from 4 up)
   in bits 8-15, the style (mushrooms, cannons, clouds) in bits 0-7 */
static int header_look(const uint8_t *h) {
    int bg = h[0] & 7;
    return ((bg >= 4 ? bg : 0) << 8) | (h[1] >> 6);
}

static void map_areas(void) {
    static uint8_t vs_raw[1024], vs_conv[1536], sm[1536];
    static uint16_t ka[512], kb[512];
    uint8_t first[5];
    for (int t = 0; t < 4; t++) first[t] = rom8(AREA_TYPE_OFS + (uint32_t)t);
    first[4] = N_SMAS_AREAS;
    memset(area_map, 0, sizeof area_map);
    for (int t = 0; t < 4; t++) {
        int vs_first = cart.prg[VS_AREA_OFS - 0x8000 + t];
        /* the last type's count: the area table has as many entries as the enemy table before it */
        int vs_total = (VS_AREA_OFS - VS_ENEMY_ADDR) / 2;
        int vs_count = t < 3 ? cart.prg[VS_AREA_OFS - 0x8000 + t + 1] - vs_first : vs_total - vs_first;
        if (vs_count > 32) vs_count = 32;
        for (int a = 0; a < vs_count; a++) {
            int n = vs_area_data(t, a, vs_raw, sizeof vs_raw);
            int m = to_allstars_format(vs_raw, n, vs_conv, sizeof vs_conv);
            int na = object_keys(vs_conv, m, ka, 512);
            /* All-Stars draws an area's sky and scenery by its number, so the area must have VS.'s
               background (the header's night and snow settings) and style (mushrooms, cannons) first,
               then as many of its objects as possible */
            int vs_look = n >= 2 ? header_look(vs_raw) : 0;
            int best = first[t] + (a < first[t + 1] - first[t] ? a : 0), best_score = -1, best_look = -1;
            for (int db = first[t]; db < first[t + 1]; db++) {
                int ns = smas_area_data(db, sm, sizeof sm);
                int nb = object_keys(sm, ns, kb, 512);
                int score = na + nb ? 2000 * lcs(ka, na, kb, nb) / (na + nb) : 0;
                int look = ns >= 2 ? (header_look(sm) & 0xFF00) == (vs_look & 0xFF00) ? 2 + ((header_look(sm) & 0xFF) == (vs_look & 0xFF)) : (header_look(sm) & 0xFF) == (vs_look & 0xFF) : 0;
                /* a level of VS.'s own (little in common with any All-Stars level) is matched by its
                   number in the type, the nearest look there is */
                if (score < 500) score = db - first[t] == a ? 499 : 0;
                if (look > best_look || (look == best_look && score > best_score)) { best_look = look; best_score = score; best = db; }
            }
            area_map[t][a] = (uint8_t)best;
        }
    }
}

/* ---- metatiles ---- */
static uint32_t map16_addr(uint8_t mt) {
    uint8_t g = mt >> 6;
    return 0x030000u | rom8(MAP16_LO + g) | ((uint32_t)rom8(MAP16_HI + g) << 8);
}
static void map16(uint8_t mt, uint16_t w[4]) {   /* UL, LL, UR, LR tilemap words */
    uint32_t a = map16_addr(mt) + (uint32_t)(mt & 63) * 8;
    for (int k = 0; k < 4; k++) w[k] = rom16(a + (uint32_t)k * 2);
}

/* VS. metatile -> All-Stars metatile, for blocks the game changes during play (they never come from
   the parser): by the four tiles where All-Stars kept the NES numbers, else by the place in the
   group; the parser's own pairs (seen as it renders) take precedence */
static uint8_t mt_static[256], mt_seen[256], mt_seen_ok[256];

static void map_metatiles(void) {
    int vs_count[4], sm_count[4];
    uint16_t vs_ptr[4];
    for (int g = 0; g < 4; g++)
        vs_ptr[g] = (uint16_t)(cart.prg[VS_METATILE_LO - 0x8000 + g] | (cart.prg[VS_METATILE_LO + 4 - 0x8000 + g] << 8));
    for (int g = 0; g < 4; g++) {
        vs_count[g] = g < 3 ? (vs_ptr[g + 1] - vs_ptr[g]) / 4 : 6;
        /* group 3's table is the last one: its 64 entries (All-Stars' ? blocks, coins, used block and axe
           are at $E7-$FD, after its own additions) */
        sm_count[g] = g < 3 ? (int)((map16_addr((uint8_t)((g + 1) << 6)) - map16_addr((uint8_t)(g << 6))) / 8) : 64;
    }
    for (int m = 0; m < 256; m++) mt_static[m] = (uint8_t)m;
    for (int g = 0; g < 4; g++) {
        /* a monotonic alignment of the two groups by tile matches: score[i][j] = matching quarters */
        static int dp[70][70];
        static uint8_t from[70][70];
        int nv = vs_count[g] < 64 ? vs_count[g] : 64, ns = sm_count[g] < 64 ? sm_count[g] : 64;
        memset(dp, 0, sizeof dp);
        for (int i = 1; i <= nv; i++)
            for (int j = 1; j <= ns; j++) {
                const uint8_t *t = cart.prg + vs_ptr[g] - 0x8000 + (i - 1) * 4;
                uint16_t w[4];
                map16((uint8_t)((g << 6) | (j - 1)), w);
                int sc = 0;
                for (int k = 0; k < 4; k++) sc += (w[k] & 0x3FF) == t[k];
                int best = dp[i - 1][j], f = 1;
                if (dp[i][j - 1] > best) { best = dp[i][j - 1]; f = 2; }
                if (dp[i - 1][j - 1] + sc + 1 > best) { best = dp[i - 1][j - 1] + sc + 1; f = 3; }
                dp[i][j] = best; from[i][j] = (uint8_t)f;
            }
        for (int i = nv, j = ns; i > 0 && j > 0;) {
            if (from[i][j] == 3) { mt_static[(g << 6) | (i - 1)] = (uint8_t)((g << 6) | (j - 1)); i--; j--; }
            else if (from[i][j] == 1) i--;
            else j--;
        }
    }
    /* the ones VS. writes during play as All-Stars' own code writes them: PlayerHeadCollision's place of
       a bouncing block (VS. $23; LDA #$27 / STA ($06),y) and the used block (VS. $C4; LDY #$FC), the
       vine's climbing cells (VS. $26; LDA #$2A / STA ($06),y) */
    static const struct { uint8_t vs, opcode; uint32_t at; } play[] = {
        { 0x23, 0xA9, AS_BUMP_PLACE }, { 0xC4, 0xA0, AS_USED_BLOCK }, { 0x26, 0xA9, AS_VINE_CELL },
    };
    for (size_t i = 0; i < sizeof play / sizeof play[0]; i++)
        if (rom8(play[i].at) == play[i].opcode) mt_static[play[i].vs] = rom8(play[i].at + 1);
}

/* ---- the decorations of All-Stars' own levels ----
   All-Stars' own Super Mario Bros. levels carry objects the NES format has no form for (row 15, three bytes, its own
   kinds, dispatched from $04:8E43): its castles' first staircase drawn as steps ($32), the ends of a floor at a gap
   ($36, $38), the steps between floors of two heights ($3A, $3C), caps on the wall's brick halves where the wall
   stops ($34), coral on the water levels' terrain ($3E), a tree crown that a block left standing erased ($12); and
   over the floors 5-4 and 7-4 leave hanging in mid-air it lays ordinary rows and columns of solid blocks. VS.'s
   data has none of them; but All-Stars put them wherever the shape of a level asks for them (in its six castles every
   floor end at a gap has its end tiles but one), so the port finds those places in the columns All-Stars' parser
   makes of VS.'s data and writes there what those objects would. Each rule reads the plain columns around a column
   (up to three either side) and was checked against All-Stars' own levels: on VS.'s levels that are SMB's they give
   All-Stars' own tiles but in a few cells, its own slips (the last step of 8-4, an end a column off over lava). Its
   hand-placed wall in 8-4's water ($28) is a collision change, not a decoration, and is left out. A cell never changes
   between solid and empty. */

/* the metatiles those objects write (their handlers' LDA #, and the CMP # they test), read from the handlers */
enum { D_STEP_END, D_STEP_END2, D_STEP_TOP, D_STEP_FILL, D_CAP, D_HALF_A, D_HALF_B,
       D_REND_TOP, D_REND_STOP, D_REND, D_BASE, D_LEND_TOP, D_LEND_STOP, D_LEND,
       D_DOWN_LO, D_DOWN_LO2, D_FLOOR_TOP, D_DOWN_HI_TOP, D_DOWN_HI, D_DOWN_HI2, D_FLOOR,
       D_UP_HI_TOP, D_UP_HI, D_UP_HI2, D_UP_LO, D_UP_LO2, D_CORAL, N_DECO };
static const struct { uint32_t at; uint8_t op; } deco_at[N_DECO] = {
    { 0x04904D, 0xA9 }, { 0x049058, 0xA9 }, { 0x049065, 0xA9 }, { 0x04906F, 0xA9 },   /* $32 a step's end, top, body */
    { 0x049084, 0xA9 }, { 0x04908E, 0xC9 }, { 0x049092, 0xC9 },                       /* $34 the cap, the two halves */
    { 0x0490AA, 0xA9 }, { 0x0490B3, 0xC9 }, { 0x0490B7, 0xA9 },                       /* $36 a floor's right end */
    { 0x0490CF, 0xC9 }, { 0x0490D3, 0xA9 }, { 0x0490DC, 0xC9 }, { 0x0490E0, 0xA9 },   /* $38 its left end ($FC: a firebar's base) */
    { 0x0490FA, 0xA9 }, { 0x0490FF, 0xA9 }, { 0x049109, 0xC9 }, { 0x04910D, 0xA9 },   /* $3A a step down; the floor's top */
    { 0x049111, 0xA9 }, { 0x049116, 0xA9 }, { 0x04911B, 0xA9 },                       /* ... and its body */
    { 0x049137, 0xA9 }, { 0x04913B, 0xA9 }, { 0x049140, 0xA9 }, { 0x049151, 0xA9 },   /* $3C a step up */
    { 0x049156, 0xA9 },
    { 0x049166, 0xA9 },                                                               /* $3E coral */
};
#define AS_BB_BOUNDS_CMP 0x03A5B2   /* AreaParserCore: CMP BlockBuffLowBounds,y (what is solid, by group) */
static uint8_t deco[N_DECO], deco_bound[4];
static bool deco_ok;

/* the others the rules look at: a castle's solid block (the NES's rows and columns of them) and its bridge; in the
   water levels the terrain's boulders over the seabed; a ground tree's crowns ($11 a tall tree's over $13, $12 a
   short one's) and trunk */
#define MT_BLOCK     0x6A
#define MT_BRIDGE    0x8D
#define MT_TERRAIN   0x72
#define MT_SEABED    0x73
#define MT_CROWN_TOP 0x11
#define MT_CROWN     0x12
#define MT_CROWN_LOW 0x13
#define MT_TRUNK     0x4E

static void map_decorations(void) {
    deco_ok = false;
    for (int i = 0; i < N_DECO; i++) {
        if (rom8(deco_at[i].at - 1) != deco_at[i].op) return;
        deco[i] = rom8(deco_at[i].at);
    }
    if (rom8(AS_BB_BOUNDS_CMP) != 0xD9) return;
    uint16_t t = rom16(AS_BB_BOUNDS_CMP + 1);
    for (int g = 0; g < 4; g++) deco_bound[g] = rom8(0x030000u | (uint32_t)(uint16_t)(t + g));
    deco_ok = true;
}
static bool as_solid(uint8_t m) { return m >= deco_bound[m >> 6]; }
static bool is_half(uint8_t m) { return m == deco[D_HALF_A] || m == deco[D_HALF_B]; }

/* w[k] (k = -3..3): the plain column k from this one, NULL past the area or not parsed yet; world: this column's
   number in the area */

/* $32, the staircase at a castle's start (VS.: rows of solid blocks on the floor): a column's symbol at the floor
   row f is the number of blocks standing there with room above, a pit, or something else */
#define SYM_PIT   0x40
#define SYM_OTHER 0x41
static int stair_symbol(const uint8_t *col, int f, int t) {
    if (!col) return SYM_OTHER;
    if (col[f] == deco[D_FLOOR_TOP]) {
        int k = 0;
        while (f - k - 1 >= 0 && col[f - k - 1] == MT_BLOCK) k++;
        return f - k - 1 >= 0 && col[f - k - 1] == 0x00 ? k : SYM_OTHER;
    }
    for (int r = t; r <= f; r++) if (col[r]) return SYM_OTHER;
    return SYM_PIT;
}
static void deco_stairs(uint8_t *out, const uint8_t *const *w, int world) {
    const uint8_t *col = w[0];
    int t = -1, f = -1;
    for (int r = 1; r < 13;) {   /* the blocks standing on the floor, free above */
        if (col[r] == MT_BLOCK && col[r - 1] == 0x00) {
            int s = r;
            while (r < 13 && col[r] == MT_BLOCK) r++;
            if (r < 13 && col[r] == deco[D_FLOOR_TOP]) { t = s; f = r; break; }
        } else r++;
    }
    if (t < 0) return;
    /* one step lower per column to the right, from a top step two or more high that reaches back two columns (or to
       the area's start), down to the bare floor or a pit */
    int h = f - t, sym[5];
    for (int k = -2; k <= 2; k++) {
        const uint8_t *x = k < 0 && world + k < 0 ? w[-world] : w[k];   /* before the area's start: its first column */
        sym[k + 2] = k ? stair_symbol(x, f, t) : h;
    }
    int l2 = sym[0], l1 = sym[1], r1 = sym[3], r2 = sym[4];
    bool top = l1 == h && l2 == h && h >= 2, lower = l1 == h + 1 && (l2 == h + 1 || l2 == h + 2);
    if (!(top && ((r1 == h && (r2 == h || r2 == h - 1)) || (r1 == h - 1 && (r2 == h - 2 || r2 == SYM_PIT)))) &&
        !(lower && (r1 == SYM_PIT || (r1 == h - 1 && (h == 1 || r2 == h - 2 || r2 == SYM_PIT)))))
        return;
    if (r1 == h) {   /* the step goes on to the right */
        out[t] = deco[D_STEP_TOP];
        for (int r = t + 1; r < f; r++) out[r] = deco[D_STEP_FILL];
    } else {         /* its right end */
        out[t] = deco[D_STEP_END];
        if (t + 1 < f) out[t + 1] = deco[D_STEP_END2];
        for (int r = t + 2; r < f; r++) out[r] = deco[D_STEP_FILL];
    }
}

/* $34, the caps: a castle's wall is brick halves ($65 left, $66 right); a half whose partner cell is no half gets a
   cap, and so do the blocks that carry a firebar (hanging from the wall over its base $FC, or a one-block pedestal
   on the floor under it or beside the one under it). 8-4's water area has that wall too (in world 8 All-Stars'
   parser draws the water levels' terrain as its halves), and there also a floor's last column against it */
static void deco_caps(uint8_t *out, const uint8_t *const *w, int type) {
    const uint8_t *col = w[0];
    uint8_t base = deco[D_BASE], ftop = deco[D_FLOOR_TOP];
    for (int r = 0; r < 13; r++) {
        uint8_t m = col[r];
        bool cap = false;
        if (m == deco[D_HALF_A]) cap = w[1] && !is_half(w[1][r]);
        else if (m == deco[D_HALF_B]) cap = w[-1] && !is_half(w[-1][r]);
        else if (type == 3 && m == MT_BLOCK && r >= 1 && r <= 11) {
            bool beside = (w[-1] && is_half(w[-1][r])) || (w[1] && is_half(w[1][r]));
            if (is_half(col[r - 1]) && col[r + 1] == base && !beside) cap = true;
            else if (col[r + 1] == ftop && (col[r - 1] == 0x00 || col[r - 1] == base)) {
                cap = col[r - 1] == base;
                for (int d = -1; d <= 1 && !cap; d += 2)
                    cap = w[d] && w[d][r] == MT_BLOCK && w[d][r + 1] == ftop && w[d][r - 1] == base;
            }
        } else if (type == 0 && (m == ftop || m == deco[D_FLOOR])) {
            for (int d = -1; d <= 1 && !cap; d += 2)
                cap = w[d] && w[-d] && is_half(w[d][r]) && w[-d][r] != ftop && w[-d][r] != deco[D_FLOOR];
        }
        if (cap) out[r] = deco[D_CAP];
    }
}

/* $3E, coral: the water levels' terrain above the sea floor (row 11). All-Stars leaves the inside of a level's end
   wall plain, where its picture never reaches; the widescreen bands do, so the whole wall is coral */
static void deco_coral(uint8_t *out, const uint8_t *col) {
    if (col[12] != MT_SEABED || col[11] != MT_TERRAIN) return;
    for (int r = 0; r < 11; r++) if (col[r] == MT_SEABED) return;
    for (int r = 0; r < 11; r++) if (col[r] == MT_TERRAIN) out[r] = deco[D_CORAL];
}

/* the floors VS. leaves in mid-air (its castles' terrain bands in 5-4 and 7-4): All-Stars' own data lays rows and
   columns of solid blocks over them, so they are the castle's blocks */
static void deco_masses(uint8_t *out, const uint8_t *col) {
    for (int r = 0; r < 13;) {
        if (col[r] != deco[D_FLOOR_TOP]) { r++; continue; }
        int s = r++;
        while (r < 13 && col[r] == deco[D_FLOOR]) r++;
        if (r < 13 && col[r] == 0x00) for (int k = s; k < r; k++) out[k] = MT_BLOCK;
    }
}

/* $12: a tree crown no longer on its trunk (a block took the rest of the tree's cells) is erased, from below */
static void deco_trees(uint8_t *out) {
    for (int r = 11; r >= 0; r--) {
        uint8_t m = out[r];
        bool on;
        if (m == MT_CROWN || m == MT_CROWN_LOW) on = out[r + 1] == MT_TRUNK;
        else if (m == MT_CROWN_TOP) on = out[r + 1] == MT_CROWN_LOW;
        else continue;
        if (!on) out[r] = 0x00;
    }
}

/* $36 $38 $3A $3C, a castle floor's ends and steps. Its top: the run of body, blocks and bases from the bottom up,
   ended by the top tile; a pillar of blocks no neighbour's floor goes on from counts as a gap. In a column's data
   order: the steps (the higher column of a step two rows or more also gets its end), then the plain ends; nothing
   paints over a firebar's base, and no end goes under a block or a step standing on the top (All-Stars has none:
   the rounded corner would not meet the square one above) */
static int floor_top(const uint8_t *c) {
    int r = 13;
    while (r > 0) {
        uint8_t m = c[r - 1];
        if (m == deco[D_FLOOR_TOP]) { r--; break; }
        if (m == deco[D_FLOOR] || m == MT_BLOCK || m == deco[D_BASE]) { r--; continue; }
        break;
    }
    return r <= 12 ? r : -1;
}
static int eff_top(const uint8_t *const *w, int k) {
    const uint8_t *c = w[k];
    int t = c ? floor_top(c) : -1;
    if (t < 0) return -1;
    for (int r = t; r < 13; r++) if (c[r] == deco[D_FLOOR_TOP] || c[r] == deco[D_FLOOR]) return t;
    return (w[k - 1] && floor_top(w[k - 1]) == t) || (w[k + 1] && floor_top(w[k + 1]) == t) ? t : -1;
}
static bool wall_tile(uint8_t m) { return is_half(m) || m == deco[D_CAP] || m == MT_TERRAIN || m == MT_SEABED; }
static void put(uint8_t *out, int r, uint8_t m) { if (out[r] != deco[D_BASE]) out[r] = m; }
static bool covered(const uint8_t *out, int a) {
    uint8_t m = a ? out[a - 1] : 0x00;
    return m == MT_BLOCK || m == deco[D_STEP_END] || m == deco[D_STEP_END2] || m == deco[D_STEP_TOP] ||
           m == deco[D_STEP_FILL];
}
static void floor_right_end(uint8_t *out, int a) {
    if (covered(out, a)) return;
    put(out, a, deco[D_REND_TOP]);
    for (int r = a + 1; r < 13 && out[r] != deco[D_REND_STOP]; r++) put(out, r, deco[D_REND]);
}
static void floor_left_end(uint8_t *out, int a) {
    if (covered(out, a)) return;
    put(out, a, deco[D_LEND_TOP]);
    for (int r = a + 1; r < 13 && out[r] != deco[D_LEND_STOP]; r++) put(out, r, deco[D_LEND]);
}
static void deco_floor(uint8_t *out, const uint8_t *const *w) {
    int a = eff_top(w, 0);
    if (a < 0) return;
    int L = eff_top(w, -1), R = eff_top(w, 1);
    bool down_in = L >= 0 && L < a && !as_solid(w[0][L]);      /* the lower column of a step down from the left */
    bool up_in = L > a && !as_solid(w[-1][a]);                 /* the higher column of a step up from the left */
    bool down_out = R > a && !as_solid(w[1][a]);               /* the higher column of a step down to the right */
    bool up_out = R >= 0 && R < a && !as_solid(w[0][R]);       /* the lower column of a step up to the right */
    /* a lone column between gaps keeps its look, but for its end at the bridge (where All-Stars ends every castle's
       last floor) */
    bool some = L >= 0 || R >= 0;
    bool lend = w[-1] && L < 0 && !wall_tile(w[-1][a]) && some && !down_out;
    bool rend = w[1] && R < 0 && !wall_tile(w[1][a]) && (some || w[1][a] == MT_BRIDGE) && !up_in;
    if (down_in) {
        if (!out[a - 1]) put(out, a - 1, deco[D_DOWN_LO]);
        put(out, a, deco[D_DOWN_LO2]);
    }
    if (up_in) {
        put(out, L - 1, out[L - 1] == deco[D_FLOOR_TOP] ? deco[D_UP_HI_TOP] : deco[D_UP_HI]);
        put(out, L, deco[D_UP_HI2]);
        for (int r = L + 1; r < 13; r++) put(out, r, deco[D_FLOOR]);
        if (L - a >= 2) floor_left_end(out, a);
    }
    if (down_out) {
        put(out, R - 1, out[R - 1] == deco[D_FLOOR_TOP] ? deco[D_DOWN_HI_TOP] : deco[D_DOWN_HI]);
        put(out, R, deco[D_DOWN_HI2]);
        put(out, 12, deco[D_FLOOR]);
        if (R - a >= 2) floor_right_end(out, a);
    }
    if (up_out) {
        if (!out[a - 1]) put(out, a - 1, deco[D_UP_LO]);
        put(out, a, deco[D_UP_LO2]);
    }
    if (lend) floor_left_end(out, a);
    if (rend) floor_right_end(out, a);
}

/* a column as All-Stars would draw it in its own level: win = the plain columns from three left of it to three
   right, type = the area's (0 water, 1 ground, 3 castle) */
static void decorate(uint8_t out[13], const uint8_t *const win[7], int type, int world) {
    const uint8_t *const *w = win + 3;
    memcpy(out, w[0], 13);
    if (!deco_ok) return;
    if (type == 3) deco_stairs(out, w, world);
    if (type == 3) deco_masses(out, w[0]);
    if (type == 3 || type == 0) deco_caps(out, w, type);
    if (type == 0) deco_coral(out, w[0]);
    if (type == 1) deco_trees(out);
    if (type == 3 || type == 0) deco_floor(out, w);   /* last: the others look at the floor's plain top */
    for (int r = 0; r < 13; r++) if (as_solid(out[r]) != as_solid(w[0][r])) out[r] = w[0][r];
}

/* ---- the area being shown ---- */
static struct {
    bool     pending;          /* InitializeArea ran in VS.: build at its first parsed column */
    bool     ready;            /* the sandbox holds this area */
    uint8_t  vs_ptr, db;
    uint8_t  id[5];            /* world, level, area number, type, data offset when it was built */
    uint8_t  smas[32][13], vs[32][13];   /* per column of the 512-pixel window: the parsers' metatiles */
    uint8_t  plain[32][13];    /* All-Stars' before the decorations of its own levels (see decorate) */
    uint8_t  have[32];         /* plain holds a column of this area */
    uint8_t  parsed[32];       /* the column holds this area's metatiles */
    int      world_col[32];    /* which column of the area (page * 16 + column) each holds */
    int      last_col;         /* the column VS. was rendering at the previous parser call */
    bool     resync;           /* VS.'s machine jumped (a state load, rewind, reset): parse again */
    uint16_t bg2_scroll, bg3_scroll;
    int      bg2_pages;        /* how many pages of layer 2 the area setup filled: they repeat */
    int      bg2_seam;         /* after a castle loop, layer 2's x left of which it shows what it did before (-1: none) */
} area;

static const uint16_t area_id_addr[5] = { RAM_WORLD, RAM_LEVEL, RAM_AREA_NUM, RAM_AREA_TYPE, 0x074F };

#define SNAP_COLS 64
static int snap_col[SNAP_COLS];
static int last_world = -1;   /* the area's column All-Stars' parser did last */

#define AREA_DATA_COPY 0x7FE000u   /* where VS.'s converted area data goes in the sandbox */

/* The area header's settings as the area starts. The parser changes them as it meets objects (the
   terrain, the scenery), so a rebuild later in the area cannot take them from the RAM of that moment;
   they come from the header itself: VS.'s GetAreaDataAddrs ($AC0E) copies the area's data to PRG RAM
   (at $6200), where it stays, and decodes its two header bytes as below. SecondaryHardMode ($06CC)
   is set by world and level. */
static const uint16_t header_vars[] = { 0x0744, 0x0742, 0x0733, 0x0743, 0x0727, 0x0710, 0x0715, 0x0741, 0x06CC };
#define N_HEADER_VARS (sizeof header_vars / sizeof header_vars[0])

/* The page VS.'s parser began the area it is in on (InitializeArea's start: the halfway page after a
   death, the entrance page after a pipe), seen at its first column; the halfway page is cleared later,
   so a rebuild cannot find it in the RAM of that moment. Forgotten after a state load (the start is then
   InitializeArea's from that RAM). */
static struct { bool ok; uint8_t id[5], page; } area_start;

static int start_page(const uint8_t *r) {
    if (!area_start.ok) return -1;
    for (int i = 0; i < 5; i++) if (area_start.id[i] != r[area_id_addr[i]]) return -1;
    return area_start.page;
}

static void area_header(const uint8_t *r, uint8_t v[N_HEADER_VARS]) {
    uint16_t at = (uint16_t)((r[0xE7] | (r[0xE8] << 8)) - 2);   /* the data pointer is past the header */
    uint8_t h0 = 0, h1 = 0;
    if (at >= 0x6000 && at < 0x67FF) { h0 = cart.wram[at - 0x6000]; h1 = cart.wram[at + 1 - 0x6000]; }
    uint8_t fg = h0 & 7, style = h1 >> 6;
    v[0] = fg >= 4 ? fg : 0;          /* BackgroundColorCtrl */
    v[1] = (h1 >> 4) & 3;             /* BackgroundScenery */
    v[2] = style == 3 ? 0 : style;    /* AreaStyle */
    v[3] = style == 3 ? 3 : 0;        /* CloudTypeOverride */
    v[4] = h1 & 15;                   /* TerrainControl */
    v[5] = (h0 >> 3) & 7;             /* PlayerEntranceCtrl */
    v[6] = h0 >> 6;                   /* GameTimerSetting */
    v[7] = fg >= 4 ? 0 : fg;          /* ForegroundScenery */
    v[8] = r[0x06CC];
}

/* the inputs of InitializeArea, as VS. had them, for the All-Stars area pointer ptr */
static void area_inputs(const uint8_t *r, uint8_t ptr) {
    uint8_t *w = sb->wram;
    w[RAM_AREA_PTR] = ptr;
    w[RAM_WORLD] = r[RAM_WORLD]; w[RAM_LEVEL] = r[RAM_LEVEL]; w[RAM_AREA_NUM] = r[RAM_AREA_NUM];
    w[0x0753] = r[0x0753]; w[0x0756] = r[0x0756];
    w[0x075B] = r[0x075B]; w[0x0752] = r[0x0752]; w[0x0751] = r[0x0751];
    int sp = start_page(r);
    if (sp >= 0) { w[0x075B] = (uint8_t)sp; w[0x0752] = 0; }   /* HalfwayPage, not AltEntranceControl */
    w[0x076A] = r[0x076A];
    w[0x0770] = 1; w[0x0772] = 0;
}

/* All-Stars draws its 8-4 water area in a castle's style ($BA 3, its type water): the setup loads the water's
   tiles and leaves the rest of VRAM as the castle its pipe comes from had it. So the sandbox sets up the level's
   first area (LoadAreaPointer's, by world and area number) before such an area and keeps that VRAM */
static void castle_vram_first(const uint8_t *r) {
    uint8_t *w = sb->wram, ptr = w[RAM_AREA_PTR];
    if (w[RAM_AS_STYLE] != 3 || w[RAM_AS_TYPE] == 3) return;
    sb_call(sb, LEVEL_AREA_PTR, 0, 100000);
    uint8_t lvl = (uint8_t)sb->cpu.a;
    if (lvl == ptr) return;
    area_inputs(r, lvl);
    sb_call(sb, INIT_AREA, 1, 400000);
    sb_call(sb, AREA_GRAPHICS, 0, 4000000);
    w[0x0154] = 0;
    sb_call(sb, SMB1_NMI, 0, 400000);
    static uint8_t vram[sizeof sb->ppu.vram];
    memcpy(vram, sb->ppu.vram, sizeof vram);
    memcpy(sb, sb_base, sizeof *sb);
    memcpy(sb->ppu.vram, vram, sizeof vram);
    area_inputs(r, ptr);
    sb_call(sb, INIT_AREA, 1, 400000);
}

static void build_area(const uint8_t *r) {
    area.ready = false;
    if (!sb_base) return;
    memcpy(sb, sb_base, sizeof *sb);
    uint8_t *w = sb->wram;
    int type = r[RAM_AREA_TYPE] & 3, num = r[0x074F] & 31;
    uint8_t db = area_map[type][num];
    uint8_t first = rom8(AREA_TYPE_OFS + (uint32_t)type);
    area.db = db;
    area.vs_ptr = r[RAM_AREA_PTR];
    for (int i = 0; i < 5; i++) area.id[i] = r[area_id_addr[i]];
    area_inputs(r, (uint8_t)((type << 5) | (db - first)));
    sb_call(sb, INIT_AREA, 1, 400000);
    castle_vram_first(r);
    /* the area's header and objects are VS.'s */
    uint8_t hv[N_HEADER_VARS];
    area_header(r, hv);
    for (size_t i = 0; i < N_HEADER_VARS; i++) w[header_vars[i]] = hv[i];
    static uint8_t raw[1024], conv[1536];
    uint16_t src = (uint16_t)(r[0xE7] | (r[0xE8] << 8));   /* VS.'s area data pointer (past the header) */
    int n = 0;
    raw[n++] = 0; raw[n++] = 0;
    while (n < (int)sizeof raw) {
        uint8_t b = src >= 0x6000 && src < 0x6800 ? cart.wram[(src - 0x6000) & 0x7FF] : 0xFD;
        raw[n++] = b; src++;
        if (b == 0xFD) break;
    }
    int m = to_allstars_format(raw, n, conv, sizeof conv);
    for (int i = 0; i < m; i++) w[AREA_DATA_COPY - 0x7E0000 + (uint32_t)i] = conv[i];
    w[0xFA] = (uint8_t)((AREA_DATA_COPY + 2) & 0xFF);
    w[0xFB] = (uint8_t)(((AREA_DATA_COPY + 2) >> 8) & 0xFF);
    w[0xFC] = (uint8_t)((AREA_DATA_COPY + 2) >> 16);
    sb_call(sb, AREA_GRAPHICS, 0, 4000000);
    sb_call(sb, AREA_COLORS, 0, 100000);
    /* layer 2's pattern: the pages the setup filled ($7E2000 + page * $800; 8 for the hills), repeated
       along the level and before its start (the bands see layer 2 left of its origin at half speed) */
    area.bg2_pages = 0;
    while (area.bg2_pages < 16) {
        const uint8_t *pg = &w[0x2000 + area.bg2_pages * 0x800];
        int any = 0;
        for (int i = 0; i < 0x800 && !any; i++) any = pg[i] != 0;
        if (!any) break;
        area.bg2_pages++;
    }
    if (!area.bg2_pages) area.bg2_pages = 1;
    area.bg2_seam = -1;
    w[0x0154] = 0;
    sb_call(sb, SMB1_NMI, 0, 400000);
    memset(area.parsed, 0, sizeof area.parsed);
    memset(area.have, 0, sizeof area.have);
    area.ready = true;
    area.pending = false;
}

static bool same_area(void) {
    for (int i = 0; i < 5; i++) if (ram[area_id_addr[i]] != area.id[i]) return false;
    return true;
}

/* VS.'s metatile buffer holds the column it parsed last: keep it with All-Stars' one for that column */
static void keep_vs_column(int world) {
    int slot = world & 31;
    if (world < 0 || area.world_col[slot] != world) return;
    memcpy(area.vs[slot], &ram[RAM_MT_BUFFER], 13);
    for (int r = 0; r < 13; r++) {
        uint8_t v = area.vs[slot][r], s = area.plain[slot][r];
        if (!mt_seen_ok[v]) { mt_seen[v] = s; mt_seen_ok[v] = 1; }
    }
    area.parsed[slot] = 1;
}

/* All-Stars' parser state (its low 8 KiB of RAM) before each of the last columns it parsed: run-ahead and
   other short returns to the past make VS. parse a column again, and All-Stars' parser must then start from
   where it was before that column too */
static uint8_t snap[SNAP_COLS][0x2000];


/* the column in slot s from the plain columns three either side of it (in the nametables' order: across a castle's
   loop the column after the last one before it is the first after it) */
static void finish_column(int s) {
    const uint8_t *win[7];
    for (int k = -3; k <= 3; k++) {
        int n = (s + k) & 31;
        win[k + 3] = area.have[n] && ((area.world_col[n] - area.world_col[s] - k) & 63) == 0 ? area.plain[n] : NULL;
    }
    decorate(area.smas[s], win, ram[RAM_AREA_TYPE] & 3, area.world_col[s]);
}

/* a column parsed (All-Stars' metatiles in w's metatile buffer): kept as it is, and it and the three before it
   decorated again with what is now known of their right */
static void column_parsed(int slot, const uint8_t *w) {
    memcpy(area.plain[slot], &w[RAM_MT_BUFFER], 13);
    area.have[slot] = 1;
    for (int k = 3; k >= 0; k--) if (area.have[(slot - k) & 31]) finish_column((slot - k) & 31);
}

/* A castle's loop (VS.'s ExecGameLoopback, $BFA5) sends the parser four pages back, to the loop's place in
   the area data; the columns parsed before it stay in the nametables (four pages are two nametables) and
   the level goes on from them. All-Stars' parser takes the same step, as its own loop code ($03:C3F5)
   does: the object page four back, the page select cleared, and the data offset VS.'s parser now has
   (r: its RAM), in All-Stars' copy of the data, where row-15 objects take three bytes */
static void loop_back(uint8_t *w, const uint8_t *r) {
    uint16_t src = (uint16_t)(r[0xE7] | (r[0xE8] << 8));
    int ofs = r[RAM_DATA_OFS], extra = 0;
    for (int i = 0; i + 1 < ofs; i += 2) {
        uint8_t b = cart.wram[(src + i - 0x6000) & 0x7FF];
        if (b == 0xFD) break;
        if ((b & 0x0F) == 0x0F) extra++;
    }
    w[RAM_OBJ_PAGE] = (uint8_t)(w[RAM_OBJ_PAGE] - 4);
    w[RAM_OBJ_PAGE_SEL] = 0;
    w[RAM_DATA_OFS] = (uint8_t)(ofs + extra);
}

static void parser_column(void) {
    if (area.resync) return;   /* rebuilt from scratch at the next picture */
    if (area.pending || !area.ready || !same_area()) {
        /* a new area, or not the one built: replayed from its start at the next picture (VS. does not
           always parse an area from its first column: its demo goes on from the title's screen) */
        if (area.pending) {
            area_start.ok = true;
            for (int i = 0; i < 5; i++) area_start.id[i] = ram[area_id_addr[i]];
            area_start.page = ram[RAM_PAGE];
        }
        area.pending = false;
        area.resync = true;
        return;
    }
    int world = ram[RAM_PAGE] * 16 + (ram[RAM_COLUMN] & 15);
    bool loop = last_world >= 0 && world == last_world + 1 - 64;
    keep_vs_column(loop ? last_world : world - 1);
    uint8_t *w = sb->wram;
    if (loop) {
        loop_back(w, ram);
        /* the camera went four pages back too, layer 2's scroll two of the layer's pages. All-Stars draws
           the layer in strips just ahead of its scroll ($03:A2BA, $04:9B35: 64 pixels wide and ahead in
           castles, 32 elsewhere), so what it drew before the loop stays until new strips replace it */
        int step = w[0xBA] == 3 ? 64 : 32, scroll = (((ram[0x071A] << 8) | ram[0x071C]) >> 1) & ~(step - 1);
        area.bg2_seam = (scroll + 256 + 2 * step) & 0x7FFF;
    } else if (last_world >= 0 && world != last_world + 1) {
        /* this column again, or (run-ahead back across a loop) one before the loop: from All-Stars' state
           before it, if it is still kept */
        if (snap_col[world % SNAP_COLS] != world) { area.resync = true; return; }
        memcpy(w, snap[world % SNAP_COLS], 0x2000);
    }
    memcpy(snap[world % SNAP_COLS], w, 0x2000);
    snap_col[world % SNAP_COLS] = world;
    last_world = world;
    int slot = world & 31;
    area.world_col[slot] = world;
    area.parsed[slot] = 0;
    w[RAM_PAGE] = ram[RAM_PAGE]; w[RAM_COLUMN] = ram[RAM_COLUMN];
    w[RAM_BACKLOAD] = ram[RAM_BACKLOAD]; w[RAM_BB_COLUMN] = ram[RAM_BB_COLUMN];
    if (sb_call(sb, PARSER_CORE, 1, 200000) < 0) return;
    column_parsed(slot, w);
}

/* ---- hooks ---- */
#define VS_AREA_INIT   0x965C
#define VS_PARSER_CORE 0xA2D7
#define VS_RETURN      0x0000   /* a call in VS.'s sandbox returns here */

/* a JSR to pc in VS.'s CPU sandbox: returns 0 when it came back */
static int vs_call(cpu_t *r, uint8_t *sram, uint8_t *swram, uint16_t pc, int max) {
    uint16_t ret = (uint16_t)(VS_RETURN - 1);
    sram[0x100 | r->sp] = (uint8_t)(ret >> 8); r->sp--;
    sram[0x100 | r->sp] = (uint8_t)ret; r->sp--;
    r->pc = pc;
    return cpu_sandbox_run(r, VS_RETURN, sram, swram, max) < 0 ? -1 : 0;
}

/* the replay's columns [col, end): VS.'s parser in its CPU sandbox (r, sram, swram), All-Stars' here, with
   the state before each of the last ones kept as parser_column keeps it; where it got to, -1 if a parser
   did not come back */
static int replay(const cpu_t *r, uint8_t *sram, uint8_t *swram, int col, int end) {
    uint8_t *w = sb->wram;
    for (int n = 0; col < end && n < 512; n++, col++) {
        sram[RAM_PAGE] = (uint8_t)(col >> 4); sram[RAM_COLUMN] = (uint8_t)(col & 15);
        cpu_t c = *r;
        if (vs_call(&c, sram, swram, VS_PARSER_CORE, 100000) != 0) return -1;
        sram[RAM_BB_COLUMN] = (uint8_t)((sram[RAM_BB_COLUMN] + 1) & 31);
        int slot = col & 31;
        memcpy(area.vs[slot], &sram[RAM_MT_BUFFER], 13);
        if (col >= end - SNAP_COLS) { memcpy(snap[col % SNAP_COLS], w, 0x2000); snap_col[col % SNAP_COLS] = col; }
        w[RAM_PAGE] = (uint8_t)(col >> 4); w[RAM_COLUMN] = (uint8_t)(col & 15);
        w[RAM_BB_COLUMN] = (uint8_t)((sram[RAM_BB_COLUMN] - 1) & 31);
        if (sb_call(sb, PARSER_CORE, 1, 200000) < 0) return -1;
        area.world_col[slot] = col;
        column_parsed(slot, w);
        area.parsed[slot] = 1;
    }
    return col;
}

/* the same with the castle loop y on the way (at its page, VS.'s ExecGameLoopback in both parsers) */
static int replay_loop(const cpu_t *r, uint8_t *sram, uint8_t *swram, int col, int end, int y) {
    int at = cart.prg[VS_LOOP_PAGE - 0x8000 + y] * 16;
    if (replay(r, sram, swram, col, at) != at) return -1;
    sram[RAM_OBJ_PAGE] = (uint8_t)(sram[RAM_OBJ_PAGE] - 4);
    sram[RAM_OBJ_PAGE_SEL] = 0;
    sram[RAM_DATA_OFS] = swram[VS_LOOP_OFS - 0x6000 + y];
    loop_back(sb->wram, sram);
    return replay(r, sram, swram, at - 64, end);
}

/* the replayed columns' cells that VS.'s block buffer has otherwise (columns not replayed count as empty) */
static int replay_misses(void) {
    int bad = 0;
    for (int col = 0; col < 32; col++)
        for (int row = 0; row < 13; row++) {
            uint8_t v = area.parsed[col] ? area.vs[col][row] : 0;
            uint8_t expect = v >= cart.prg[VS_BB_BOUNDS - 0x8000 + (v >> 6)] ? v : 0;
            bad += ram[((col & 16) ? 0x05D0 : 0x0500) + row * 16 + (col & 15)] != expect;
        }
    return bad;
}

/* After a jump of VS.'s machine the columns no longer match its nametables, and All-Stars' parser
   carries state from column to column: both parsers run again, VS.'s in its CPU sandbox and All-Stars'
   here, from the area's start up to the column VS.'s parser renders next, as the game itself did. */
static void resync(void) {
    static uint8_t sram[2048], swram[2048], sram0[2048], swram0[2048];
    static sbx_t *sb0;
    area.resync = false;
    area.ready = false;
    memset(area.parsed, 0, sizeof area.parsed); memset(area.have, 0, sizeof area.have);
    area.last_col = -1;
    int target = ram[RAM_PAGE] * 16 + (ram[RAM_COLUMN] & 15);
    memcpy(sram, ram, sizeof sram);
    memcpy(swram, cart.wram, sizeof swram);
    int sp = start_page(ram);
    if (sp >= 0) { sram[0x075B] = (uint8_t)sp; sram[0x0752] = 0; }   /* where the area began */
    cpu_t r = cpu;
    r.sp = 0xF0;
    mem_sandbox_begin();
    if (vs_call(&r, sram, swram, VS_AREA_INIT, 200000) != 0) return;
    /* InitializeArea finds the area from AreaPointer, which the title screen leaves stale (it shows 1-1
       with another area's pointer, so the sandbox would load that area): the area itself comes from VS.'s
       RAM, as for All-Stars' side: its data in PRG RAM, the data pointers, the area type, and the
       header's settings */
    memcpy(swram, cart.wram, sizeof swram);
    for (uint16_t a = 0xE7; a <= 0xEA; a++) sram[a] = ram[a];
    sram[RAM_AREA_TYPE] = ram[RAM_AREA_TYPE];
    sram[RAM_AREA_PTR] = ram[RAM_AREA_PTR];
    uint8_t hv[N_HEADER_VARS];
    area_header(ram, hv);
    for (size_t i = 0; i < N_HEADER_VARS; i++) sram[header_vars[i]] = hv[i];
    build_area(ram);   /* from VS.'s RAM: the area ids, its data pointer and header */
    if (!area.ready) return;
    int col = sram[RAM_PAGE] * 16 + (sram[RAM_COLUMN] & 15);
    /* less than two pages past a castle loop's new page, the nametables still hold columns from before
       the loop: the replay goes through that loop if its columns then match VS.'s block buffer better */
    int best = -1, misses = 0;
    bool tried = false;
    for (int y = 0; y < N_LOOPS; y++) {
        int at = cart.prg[VS_LOOP_PAGE - 0x8000 + y] * 16;
        if (cart.prg[VS_LOOP_WORLD - 0x8000 + y] != ram[RAM_WORLD] || at <= col || target < at - 64 || target >= at - 32) continue;
        if (!sb0 && !(sb0 = malloc(sizeof *sb0))) break;
        if (!tried) {   /* first the replay without a loop */
            memcpy(sram0, sram, sizeof sram); memcpy(swram0, swram, sizeof swram); memcpy(sb0, sb, sizeof *sb);
            misses = replay(&r, sram, swram, col, target) == target ? replay_misses() : INT_MAX;
            tried = true;
        }
        memcpy(sram, sram0, sizeof sram); memcpy(swram, swram0, sizeof swram); memcpy(sb, sb0, sizeof *sb);
        memset(area.parsed, 0, sizeof area.parsed); memset(area.have, 0, sizeof area.have);
        int m = replay_loop(&r, sram, swram, col, target, y) == target ? replay_misses() : INT_MAX;
        if (m < misses) { misses = m; best = y; }
    }
    if (tried) {
        memcpy(sram, sram0, sizeof sram); memcpy(swram, swram0, sizeof swram); memcpy(sb, sb0, sizeof *sb);
        memset(area.parsed, 0, sizeof area.parsed); memset(area.have, 0, sizeof area.have);
    }
    for (int i = 0; i < SNAP_COLS; i++) snap_col[i] = -1;
    int end = best >= 0 ? replay_loop(&r, sram, swram, col, target, best) : replay(&r, sram, swram, col, target);
    if (end < 0) return;
    last_world = end - 1;
}

void allstars_hook(uint16_t pc) {
    if (!as.rom || !sb_base) return;
    if (pc == VS_AREA_INIT) { area.pending = true; area.last_col = -1; }
    else if (pc == VS_PARSER_CORE) parser_column();
}

void allstars_reset(void) {
    area.resync = true;
    area_start.ok = false;
}

static void map_sprite_palettes(void);

void allstars_attach(void) {
    area.ready = false; area.pending = false; area.last_col = -1; area.resync = true;   /* built at the next picture */
    area_start.ok = false;
    memset(mt_seen_ok, 0, sizeof mt_seen_ok);
    if (as.rom && cart.prg && cart.chr) { map_areas(); map_metatiles(); map_decorations(); map_sprite_palettes(); }
}

/* ---- loading ---- */
static void music_free(void);
static void music_build(void);

int allstars_load(const char *path, char *err, size_t errlen) {
    FILE *f = nes_fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open the file"); return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0x100000 || len > 0x400000 + 512) { fclose(f); snprintf(err, errlen, "not a Super Mario All-Stars ROM (size)"); return -1; }
    uint8_t *data = malloc((size_t)len);
    if (!data || fread(data, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(data); snprintf(err, errlen, "cannot read the file"); return -1; }
    fclose(f);
    size_t skip = (len % 0x8000) == 512 ? 512 : 0;   /* a copier header */
    size_t size = (size_t)len - skip;
    uint8_t sha[20], md[16];
    rom_sha1(data + skip, size, sha);
    rom_md5(data + skip, size, md);
    char hex[41], mdhex[33];
    for (int i = 0; i < 20; i++) sprintf(hex + i * 2, "%02x", sha[i]);
    for (int i = 0; i < 16; i++) sprintf(mdhex + i * 2, "%02x", md[i]);
    int k;
    for (k = 0; known[k].sha1; k++) if (!strcmp(known[k].sha1, hex)) break;
    if (!known[k].sha1) {
        free(data);
        snprintf(err, errlen, "unknown version (SHA-1 %.12s...): the USA Super Mario All-Stars is needed", hex);
        return -1;
    }
    allstars_unload();
    as.rom = malloc(size);
    if (!as.rom) { free(data); snprintf(err, errlen, "out of memory"); return -1; }
    memcpy(as.rom, data + skip, size);
    free(data);
    as.size = size;
    snprintf(as.path, sizeof as.path, "%s", path);
    snprintf(as.name, sizeof as.name, "%s", known[k].name);
    memcpy(as.sha1_hex, hex, sizeof hex);
    memcpy(as.md5_hex, mdhex, sizeof mdhex);
    as.crc32 = rom_crc32(as.rom, as.size);
    if (startup() != 0) { allstars_unload(); snprintf(err, errlen, "the ROM's game did not start in the sandbox"); return -1; }
    allstars_attach();
    music_build();
    return 0;
}

void allstars_unload(void) {
    music_free();
    free(as.rom);
    memset(&as, 0, sizeof as);
    area.ready = false;
}

int allstars_loaded(void) { return as.rom != NULL; }

/* ---- the music ---- */

/* All-Stars' music, played by its own sound program on the SNES sound unit (spc700.c). All-Stars
   writes the menu's sound engine and samples ($07:FC00, then $3B:8000) and Super Mario Bros.' songs
   ($1F:8000) into the sound unit's RAM, each a stream of blocks ($00:8B17), and while it plays it
   hands the program a song number, an effect or a command on four ports each frame ($04:8163).
   Here VS.'s requests (AreaMusicQueue $FB, EventMusicQueue $FC, still there at the end of a frame:
   the next NMI's SoundEngine takes them) become the songs All-Stars asks for the same music
   (GetAreaMusic's table at $05:C8F5 and the calls noted at event_song), and Bowser's battle song
   starts when he appears ($03:D6B6), as in All-Stars. VS.'s sound effects stay: while All-Stars'
   music plays, the APU's squares and noise are heard only while an effect uses them (Square1/2
   SoundBuffer $F1/$F2, NoiseSoundBuffer $F3), its triangle not at all. */
#define SND_MENU_A       0x07FC00   /* the menu's stream: $400 bytes here, then the rest at SND_MENU_B */
#define SND_MENU_B       0x3B8000
#define SND_SAMPLES      0x0B8000   /* the instruments: their directory and samples ($00:8AA3, at the title) */
#define SND_SMB1         0x1F8000   /* Super Mario Bros.' songs ($00:8B17) */
#define SND_DRIVER       0x0500     /* where each stream starts the program */
#define MUS_RING         8192
#define MUS_GAIN         1.16       /* All-Stars' mix, brought to the APU's loudness (both RMS about 1,390 in 1-1) */

bool allstars_music_enabled;
static spc_t *spc;
static uint8_t *spc_ram0;          /* the sound unit's RAM after the streams, to start again from */
static int16_t spc_frame[2 * 640];
static struct {
    uint8_t q[4];                  /* this frame's requests: All-Stars' queues $1600-$1603 */
    uint8_t last[4], wait[4], song, hold, toggle;   /* its port routine's state ($1604-$160F) */
    double owed;                   /* sound unit cycles to run */
    int16_t ring[MUS_RING];        /* mono, at SPC_RATE */
    unsigned rd, wr;
    double phase;
    int on;                        /* All-Stars' music is the one heard (else VS. plays music it has no song for) */
    int started;                   /* in step with VS.'s music since the option was turned on */
    int asked;                     /* the song last asked for, 0 none */
    int bowser_quiet;              /* frames without Bowser; his song again after a while */
    uint8_t frame;                 /* VS.'s frame counter last time: any other step than 1 is a state loaded */
} mus;

typedef uint8_t (*snd_byte)(uint32_t i);
static uint8_t snd_menu(uint32_t i) { return i < 0x400 ? rom8(SND_MENU_A + i) : rom8(SND_MENU_B + (i - 0x400)); }
static uint8_t snd_samples(uint32_t i) { return rom8(SND_SAMPLES + i); }
static uint8_t snd_smb1(uint32_t i) { return rom8(SND_SMB1 + i); }

/* writes a stream's blocks into the sound unit's RAM; its start address, or -1 if it is not one */
static int snd_stream(snd_byte b, uint32_t max) {
    uint32_t i = 0;
    for (int blocks = 0; blocks < 64; blocks++) {
        unsigned len = b(i) | (b(i + 1) << 8), addr = b(i + 2) | (b(i + 3) << 8);
        i += 4;
        if (!len) return (int)addr;
        if (i + len > max || addr + len > 0x10000) return -1;
        for (unsigned k = 0; k < len; k++) spc->ram[addr + k] = b(i + k);
        i += len;
    }
    return -1;
}

static uint8_t music_mask(void) {
    if (!mus.on) return 0x1F;
    return (uint8_t)((ram[0xF1] ? 1 : 0) | (ram[0xF2] ? 2 : 0) | (ram[0xF3] ? 8 : 0) | 16);
}

static void music_free(void) {
    if (apu_mask_hook == music_mask) apu_mask_hook = NULL;
    free(spc); spc = NULL;
    free(spc_ram0); spc_ram0 = NULL;
}

static void music_restart(void) {
    spc_reset(spc);
    memcpy(spc->ram, spc_ram0, 0x10000);
    spc->outbuf = spc_frame; spc->outcap = (int)(sizeof spc_frame / sizeof spc_frame[0]);
    spc_start(spc, SND_DRIVER);
    memset(&mus, 0, sizeof mus);
    mus.bowser_quiet = 120;
    spc_run(spc, SPC_CLOCK / 10);   /* its start-up, before the first request */
    spc->outn = 0;
}

static void music_build(void) {
    music_free();
    spc = calloc(1, sizeof *spc);
    spc_ram0 = malloc(0x10000);
    if (!spc || !spc_ram0) { music_free(); return; }
    spc_reset(spc);
    /* as All-Stars loads them before Super Mario Bros. starts: the engine with the menu's songs and
       samples, then the instruments the games' songs use (over the menu's), then the game's songs */
    if (snd_stream(snd_menu, 0x8400) != SND_DRIVER || snd_stream(snd_samples, 0x8000) != SND_DRIVER ||
        snd_stream(snd_smb1, 0x8000) != SND_DRIVER) { music_free(); return; }
    memcpy(spc_ram0, spc->ram, 0x10000);
    music_restart();
    apu_mask_hook = music_mask;
}

int allstars_music_available(void) { return spc != NULL; }

/* All-Stars' songs for VS.'s area music (GetAreaMusic, $05:C939) and its event music; -1: none */
static int area_song(uint8_t q) {
    switch (q) {
    case 0x01: return 0x02;                  /* above ground */
    case 0x02: return 0x03;                  /* water */
    case 0x04: return 0x06;                  /* underground */
    case 0x08: return 0x08;                  /* castle */
    case 0x10: return 0x01;                  /* the coin heavens */
    case 0x20: return 0x13;                  /* into the pipe */
    case 0x40: return 0x0D;                  /* the star ($03:DE17) */
    case 0x80: return 0xF0;                  /* silence: All-Stars' stop */
    }
    return -1;
}
static int event_song(uint8_t q) {
    switch (q) {
    case 0x01: return 0x09;                  /* death ($03:B1B4, $03:DF8A) */
    case 0x02: case 0x10: return 0x0A;       /* game over ($03:A0BC) */
    case 0x04: return 0x0E;                  /* the princess ($03:896B) */
    case 0x08: return 0x0B;                  /* the castle's end ($03:D4EF) */
    case 0x20: return 0x0C;                  /* the goal ($03:B952) */
    case 0x40: return 0x1FF;                 /* time running out: effect $FF on port 0 ($03:B811) */
    case 0x80: return 0xF0;
    }
    return -1;
}

static void music_ask(int song) {
    if (song == 0x1FF) { mus.q[0] = 0xFF; return; }
    mus.q[2] = (uint8_t)song;
    mus.asked = song;
}

/* All-Stars' port routine ($04:8163): a request is held on its port until the program echoes it
   (or 3 frames; a song 4 frames after it is echoed), the queues emptied */
static void music_ports(void) {
    for (int p = 0; p < 2; p++) {
        uint8_t echo = p ? (uint8_t)(spc->out[1] & 15) : spc->out[0];
        if (mus.q[p]) { spc->in[p] = mus.q[p]; mus.last[p] = p ? (uint8_t)(mus.q[p] & 15) : mus.q[p]; mus.wait[p] = 0; }
        else if (echo == mus.last[p] || ++mus.wait[p] >= 3) { spc->in[p] = 0; mus.last[p] = 0; mus.wait[p] = 0; }
    }
    if (mus.q[2]) { spc->in[2] = mus.q[2]; mus.hold = 4; if (mus.q[2] < 0xF0) mus.song = mus.q[2]; }
    else if (spc->out[2] == mus.song && mus.hold && --mus.hold == 0) { mus.hold = 1; spc->in[2] = 0; }
    if (mus.q[3]) {
        mus.last[3] = mus.q[3];
        uint8_t v = mus.q[3];
        if (v == 1 || v == 5 || v == 6 || v == 0x4A) { v |= mus.toggle; mus.toggle ^= 0x80; }
        spc->in[3] = v; mus.wait[3] = 0;
    } else if ((spc->out[3] & 0x7F) == mus.last[3] || ++mus.wait[3] >= 3) { spc->in[3] = 0; mus.last[3] = 0; mus.wait[3] = 0; }
    memset(mus.q, 0, sizeof mus.q);
}

void allstars_music_frame(void) {
    if (!spc) return;
    int want = allstars_music_enabled && as.rom;
    if (!want) {
        if (mus.started) { mus.started = mus.on = 0; music_ask(0xF0); music_ports(); }
        mus.rd = mus.wr;
        return;
    }
    if (!mus.started || (uint8_t)(ram[0x09] - mus.frame) != 1) {
        /* turned on, or a state loaded (a rewind step, a slot): what VS. plays now (its buffers) */
        uint8_t event = ram[0x07B1];
        int song = event && event != 0x40 ? event_song(event) : ram[0xF4] ? area_song(ram[0xF4]) : 0xF0;
        mus.on = song >= 0;
        if (song >= 0 && song != mus.asked) music_ask(song);
        mus.started = 1;
    }
    mus.frame = ram[0x09];
    /* this frame's requests (an event over the area's, as VS.'s MusicHandler takes them) */
    uint8_t event = ram[0xFC], area = ram[0xFB];
    int song = event ? event_song(event) : area ? area_song(area) : -2;
    if (song >= 0) { mus.on = 1; music_ask(song); }
    else if (song == -1) { mus.on = 0; music_ask(0xF0); }   /* music All-Stars has no song for: the arcade's */
    /* Bowser's battle song when he appears, the real one's in world 8 */
    int bowser = 0;
    for (int e = 0; e < 6; e++) bowser |= ram[0x0F + e] && (ram[0x16 + e] & 0x3F) == 0x2D;
    if (bowser) {
        if (mus.bowser_quiet >= 120 && song < 0 && mus.on) music_ask(ram[0x075F] == 7 ? 0x05 : 0x04);
        mus.bowser_quiet = 0;
    } else if (mus.bowser_quiet < 120) mus.bowser_quiet++;
    if (ram[0x0770] == 0 && mus.asked != 0xF0 && mus.asked) music_ask(0xF0);   /* the title is silent */
    music_ports();
    /* a frame of sound (NTSC: 60.0988 frames a second) */
    mus.owed += SPC_CLOCK / 60.0988;
    int c = (int)mus.owed;
    mus.owed -= c;
    spc->outn = 0;
    spc_run(spc, c);
    for (int i = 0; i + 1 < spc->outn; i += 2) {
        if (((mus.wr + 1) & (MUS_RING - 1)) == mus.rd) mus.rd = (mus.rd + 1) & (MUS_RING - 1);
        mus.ring[mus.wr] = (int16_t)((spc->outbuf[i] + spc->outbuf[i + 1]) >> 1);
        mus.wr = (mus.wr + 1) & (MUS_RING - 1);
    }
    /* at most 3 frames behind: the sound follows the game */
    while (((mus.wr - mus.rd) & (MUS_RING - 1)) > 1600) mus.rd = (mus.rd + 1) & (MUS_RING - 1);
}

void allstars_music_mix(int16_t *buf, uint32_t n, uint32_t rate) {
    if (!spc || !mus.on || !rate) return;
    double step = (double)SPC_RATE / rate;
    for (uint32_t i = 0; i < n; i++) {
        if (((mus.wr - mus.rd) & (MUS_RING - 1)) < 2) break;
        int a = mus.ring[mus.rd], b = mus.ring[(mus.rd + 1) & (MUS_RING - 1)];
        int v = buf[i] + (int)((a + (b - a) * mus.phase) * MUS_GAIN);
        buf[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        mus.phase += step;
        while (mus.phase >= 1.0) { mus.phase -= 1.0; mus.rd = (mus.rd + 1) & (MUS_RING - 1); }
    }
}

/* the sandbox as files (prefix.wram, .vram, .cgram, .oam, .txt), for looking at it with other tools */
static void dump_sandbox(const sbx_t *s, const char *prefix, bool columns) {
    char path[1100];
    FILE *f;
    snprintf(path, sizeof path, "%s.columns.txt", prefix);
    if (columns && (f = fopen(path, "w"))) {
        for (int c = 0; c < 32; c++) {
            fprintf(f, "col %2d parsed %d  vs:", c, area.parsed[c]);
            for (int r = 0; r < 13; r++) fprintf(f, " %02X", area.vs[c][r]);
            fprintf(f, "  allstars:");
            for (int r = 0; r < 13; r++) fprintf(f, " %02X", area.smas[c][r]);
            fprintf(f, "\n");
        }
        fclose(f);
    }
    snprintf(path, sizeof path, "%s.wram", prefix);
    if ((f = fopen(path, "wb"))) { fwrite(s->wram, 1, sizeof s->wram, f); fclose(f); }
    snprintf(path, sizeof path, "%s.vram", prefix);
    if ((f = fopen(path, "wb"))) { fwrite(s->ppu.vram, 1, sizeof s->ppu.vram, f); fclose(f); }
    snprintf(path, sizeof path, "%s.cgram", prefix);
    if ((f = fopen(path, "wb"))) { fwrite(s->ppu.cgram, 2, 256, f); fclose(f); }
    snprintf(path, sizeof path, "%s.oam", prefix);
    if ((f = fopen(path, "wb"))) { fwrite(s->oam, 1, sizeof s->oam, f); fclose(f); }
    snprintf(path, sizeof path, "%s.nesnt", prefix);   /* VS.'s two nametables */
    if ((f = fopen(path, "wb"))) { fwrite(ppu.vram + ppu.nt_offset[0], 1, 0x400, f); fwrite(ppu.vram + ppu.nt_offset[1], 1, 0x400, f); fwrite(ppu.palette, 1, 32, f); fwrite(ppu.line_v, 2, 240, f); fwrite(ppu.line_emph, 1, 240, f); fclose(f); }
    snprintf(path, sizeof path, "%s.txt", prefix);
    if ((f = fopen(path, "w"))) {
        const snesppu_t *p = &s->ppu;
        fprintf(f, "area ready %d db %02X vs ptr %02X; mode %02X tm %02X ts %02X bgsc %02X %02X %02X nba %02X %02X obsel %02X cgwsel %02X cgadsub %02X fixed %d %d %d inidisp %02X hdma %02X\n",
                area.ready, area.db, area.vs_ptr, p->bgmode, p->tm, p->ts, p->bgsc[0], p->bgsc[1], p->bgsc[2], p->bgnba[0], p->bgnba[1],
                p->obsel, p->cgwsel, p->cgadsub, p->fixed_r, p->fixed_g, p->fixed_b, p->inidisp, s->hdma_en);
        for (int b = 0; b < 4; b++) fprintf(f, "bg%d hofs %d vofs %d\n", b + 1, p->hofs[b], p->vofs[b]);
        for (int ch = 0; ch < 8; ch++) {
            fprintf(f, "dma%d:", ch);
            for (int i = 0; i < 11; i++) fprintf(f, " %02X", s->dma[ch][i]);
            fprintf(f, "\n");
        }
        fclose(f);
    }
}

/* the area's sandbox as prefix.*, the screens' (intermission, text screens, title) as prefix-screen.* */
void allstars_debug_dump(const char *prefix) {
    char p2[1100];
    if (sb) dump_sandbox(sb, prefix, true);
    snprintf(p2, sizeof p2, "%s-screen", prefix);
    if (sb_screen) dump_sandbox(sb_screen, p2, false);
}
const char *allstars_name(void) { return as.name; }
const char *allstars_sha1(void) { return as.sha1_hex; }
const char *allstars_md5(void) { return as.md5_hex; }
uint32_t allstars_crc32(void) { return as.crc32; }
const char *allstars_path(void) { return as.path; }

/* ---- drawing ---- */

/* the camera: where the playfield's lines start in the 512-pixel nametable window (and its page) */
static int camera_x(void) {
    /* the screen's left edge as the engine had it (page, pixel) */
    return (ram[0x071A] << 8) | ram[0x071C];
}

/* layer 1: the level's metatiles in the sandbox's BG1 tilemap, from the parsers' columns and the
   blocks VS. changed since (its block buffer) */
/* whether VS.'s nametable still shows metatile v at a block-buffer cell: the game writes a changed block
   to the nametable through its VRAM buffer at the next NMI (a hit block's blank, a used block, a coin
   taken), so the arcade's picture shows the change a frame after the block buffer has it, when the
   bouncing block's sprite appears too */
static bool nt_shows(int col, int row, uint8_t v) {
    int g = v >> 6;
    uint16_t tab = (uint16_t)(cart.prg[VS_METATILE_LO - 0x8000 + g] | (cart.prg[VS_METATILE_LO + 4 - 0x8000 + g] << 8));
    const uint8_t *t = cart.prg + tab - 0x8000 + (v & 63) * 4;   /* UL, LL, UR, LR */
    const uint8_t *nt = ppu.vram + ppu.nt_offset[(col >> 4) & 1];
    int tx = (col & 15) * 2, ty = row * 2 + 4;
    return nt[ty * 32 + tx] == t[0] && nt[(ty + 1) * 32 + tx] == t[1] && nt[ty * 32 + tx + 1] == t[2] && nt[(ty + 1) * 32 + tx + 1] == t[3];
}

static uint8_t cell_metatile(int col, int row) {
    if (!area.parsed[col]) return 0;
    uint8_t v = area.vs[col][row], s = area.smas[col][row];
    /* the block buffer holds what is solid; scenery (below each group's bound, VS.'s BlockBuffLowBounds)
       is stored as 0 */
    uint8_t expect = v >= cart.prg[VS_BB_BOUNDS - 0x8000 + (v >> 6)] ? v : 0;
    uint16_t bb = (uint16_t)(((col & 16) ? 0x05D0 : 0x0500) + row * 16 + (col & 15));
    uint8_t now = ram[bb];
    if (now == expect || nt_shows(col, row, v)) return s;   /* unchanged, or the change not shown yet */
    if (now == 0) return 0;
    return mt_seen_ok[now] ? mt_seen[now] : mt_static[now];
}

static void write_bg1(void) {
    uint8_t *v = sb->ppu.vram;
    for (int col = 0; col < 32; col++)
        for (int row = -2; row < 14; row++) {
            uint16_t w[4] = { 0x0024, 0x0024, 0x0024, 0x0024 };   /* tile $24 is blank in All-Stars too */
            if (row >= 0 && row < 13) map16(cell_metatile(col, row), w);
            int tx = col * 2, ty = (row + 2) * 2;
            uint32_t base = (uint32_t)((tx >> 5) * 0x400 + ty * 32 + (tx & 31));
            uint32_t at[4] = { base, base + 32, base + 1, base + 33 };
            for (int k = 0; k < 4; k++) {
                uint32_t a = (at[k] & 0x7FFF) * 2;
                v[a] = (uint8_t)w[k]; v[a + 1] = (uint8_t)(w[k] >> 8);
            }
        }
}

/* layer 2: its pages ($7E2000 + page * $800, 32x32 tiles each) into the 64-column tilemap at $0800
   around the scroll, so the bands beside the picture see the hills too; left of a castle loop's seam
   (area.bg2_seam, for this scroll), what All-Stars drew there before the loop: the layer 512 pixels on */
static void write_bg2_rows(int scroll, int x0, int w, int r0, int r1, int seam) {
    uint8_t *v = sb->ppu.vram;
    const uint8_t *ram_map = &sb->wram[0x2000];
    int first = (scroll + x0) >> 3, last = (scroll + x0 + w + 7) >> 3;
    for (int tx = first; tx <= last && tx < first + 64; tx++) {
        int sx = seam >= 0 && ((tx * 8 - seam) & 0x7FFF) >= 0x8000 - 1024 ? tx + 64 : tx;
        int page = ((sx >> 5) % area.bg2_pages + area.bg2_pages) % area.bg2_pages, c = sx & 31;
        int dst_col = tx & 63;
        for (int r = r0; r < r1; r++) {
            const uint8_t *src = ram_map + page * 0x800 + r * 0x40 + c * 2;
            uint32_t a = ((uint32_t)(0x0800 + (dst_col >> 5) * 0x400 + r * 32 + (dst_col & 31)) & 0x7FFF) * 2;
            v[a] = src[0]; v[a + 1] = src[1];
        }
    }
}
static void write_bg2(int scroll, int x0, int w) { write_bg2_rows(scroll, x0, w, 0, 32, area.bg2_seam); }

/* layer 3's clouds: All-Stars keeps them as 16x16 tiles at $7F3000 (pages of 16 x 14, $E0 bytes, 12
   pages) and the tilemap words of each at BG3_MAP16; rows 0-3 of the tilemap are the status bar. Only
   areas above ground have them ($04:8B63), and not at night (there layer 2 is split instead); under
   water the layer has the surface's waves in its row 4, written with the level's columns ($04:8C28),
   elsewhere nothing */
static void write_bg3_plain(int water) {
    uint8_t *v = sb->ppu.vram;
    static const uint8_t wave[4] = { 0x2E, 0x2F, 0x2C, 0x2D };
    for (int ty = 4; ty < 32; ty++)
        for (int tx = 0; tx < 64; tx++) {
            uint16_t word = water && ty == 4 ? (uint16_t)(0x2C00 | wave[tx & 3]) : rom16(BG3_MAP16 + (uint32_t)(((tx & 1) << 1) | (ty & 1)) * 2);
            uint32_t a = ((uint32_t)(0x5800 + (tx >> 5) * 0x400 + ty * 32 + (tx & 31)) & 0x7FFF) * 2;
            v[a] = (uint8_t)word; v[a + 1] = (uint8_t)(word >> 8);
        }
}

static void write_bg3_clouds(int scroll, int x0, int w) {
    uint8_t *v = sb->ppu.vram;
    const uint8_t *m16 = &sb->wram[0x13000];
    int first = (scroll + x0) >> 4, last = (scroll + x0 + w + 15) >> 4;
    for (int mx = first; mx <= last && mx < first + 32; mx++) {
        int page = ((mx >> 4) % 12 + 12) % 12, c = mx & 15;   /* the 12 pages wrap, left of the start too */
        for (int r = 0; r < 14; r++) {
            uint8_t t = m16[page * 0xE0 + r * 16 + c];
            for (int k = 0; k < 4; k++) {
                uint16_t word = rom16(BG3_MAP16 + (uint32_t)t * 8 + (uint32_t)k * 2);
                int tx = (mx * 2 + (k >> 1)) & 63, ty = (r + 2) * 2 + (k & 1);
                uint32_t a = ((uint32_t)(0x5800 + (tx >> 5) * 0x400 + ty * 32 + (tx & 31)) & 0x7FFF) * 2;
                v[a] = (uint8_t)word; v[a + 1] = (uint8_t)(word >> 8);
            }
        }
    }
}

/* the tiles All-Stars animates (the routine at $05:E634, every 8 frames: bushes, ? blocks, coins,
   water and lava in BG tiles $C0-$EF, the status bar's coin in BG3), at VS.'s frame counter */
static void animate_tiles_in(sbx_t *s, uint8_t frame) {
    uint8_t *v = s->ppu.vram;
    uint32_t src = ANIM_TILES + (uint32_t)((frame >> 3) & 7) * 0x800;
    for (uint32_t i = 0; i < 0x600; i++) v[0x1C00 * 2 + i] = rom8(src + i);
    src = ANIM_HUD_TILES + (uint32_t)(((uint8_t)(frame + 4)) & 0x38) * 8;
    for (uint32_t i = 0; i < 0x40; i++) v[0x5160 * 2 + i] = rom8(src + i);
}
static void animate_tiles(uint8_t frame) { animate_tiles_in(sb, frame); }

/* ---- sprites ---- */

/* NES palette of each enemy type and All-Stars' palette for it: the two games' attribute tables */
static uint8_t enemy_pal[0x40][4];
static uint8_t pal_default[4] = { 7, 5, 6, 1 };

/* the tiles All-Stars moved: for each NES tile, at each place in its frame (6 places for enemies, 4 for
   power-ups), the tile All-Stars' table has there; 0 = the same number */
static uint8_t enemy_tile[256][6], powerup_tile[256][4];

static void map_sprite_tiles(void) {
    memset(enemy_tile, 0, sizeof enemy_tile);
    memset(powerup_tile, 0, sizeof powerup_tile);
    for (int k = 0; k < ENEMY_GFX_LEN; k++) {
        uint8_t n = cart.prg[VS_ENEMY_GFX - 0x8000 + k], s = rom8(ENEMY_GFX + (uint32_t)k);
        if (n != s) enemy_tile[n][k % 6] = s;
    }
    for (int k = 0; k < 16; k++) {
        uint8_t n = cart.prg[VS_POWERUP_GFX - 0x8000 + k], s = rom8(POWERUP_GFX + (uint32_t)k);
        if (n != s) powerup_tile[n][k % 4] = s;
    }
}

static void map_sprite_palettes(void) {
    map_sprite_tiles();
    for (int id = 0; id < 0x40; id++)
        for (int p = 0; p < 4; p++) enemy_pal[id][p] = pal_default[p];
    for (int id = 0; id < 27; id++) {
        uint8_t nes = cart.prg[VS_ENEMY_ATTR - 0x8000 + id], snes = rom8(ENEMY_ATTR + (uint32_t)id);
        if (nes == 0xFF || snes == 0xFF) continue;
        for (int p = 0; p < 4; p++) enemy_pal[id][p] = (uint8_t)((snes >> 1) & 7);
    }
}

/* what each of VS.'s 64 sprites belongs to, from the RAM its frame was built with */
enum { OWN_NONE, OWN_PLAYER, OWN_ENEMY, OWN_OTHER, OWN_BLOCK };
static uint8_t owner[64], owner_slot[64];

/* The sprite data offsets ($06E4: the player, the enemies, ...) the frame was built with, from a copy
   of RAM a frame ahead: the engine's SpriteShuffler ($8215) has added SprShuffleAmt[SprShuffleAmtOffset]
   to each offset of $28 and more since, carrying round with $28 added; undone here. */
static void unshuffle(uint8_t *ofs, const uint8_t *r) {
    uint8_t amt = r[0x06E1 + (r[0x06E0] + 2) % 3];
    for (int x = 0; x < 15; x++) {
        uint8_t v = r[0x06E4 + x];
        if (v < 0x28) ofs[x] = v;
        else if (v >= amt + 0x28) ofs[x] = (uint8_t)(v - amt);
        else ofs[x] = (uint8_t)(v + 0xD8 - amt);
    }
}

static void sprite_owners(const uint8_t *r, int ahead) {
    uint8_t ofs[15];
    memset(owner, OWN_OTHER, sizeof owner);
    memset(owner_slot, 0, sizeof owner_slot);
    owner[0] = OWN_NONE;                     /* sprite 0: the status bar split's */
    if (!r) return;
    if (ahead) unshuffle(ofs, r);
    else memcpy(ofs, &r[0x06E4], sizeof ofs);
    int p = ofs[0] >> 2;
    for (int k = 0; k < 8 && p + k < 64; k++) { owner[p + k] = OWN_PLAYER; owner_slot[p + k] = (uint8_t)k; }
    for (int e = 0; e < 6; e++) {
        if (!r[0x0F + e]) continue;          /* Enemy_Flag: the slot is in use */
        int o = ofs[1 + e] >> 2;
        for (int k = 0; k < 6 && o + k < 64; k++) {
            if (owner[o + k] == OWN_PLAYER) continue;
            owner[o + k] = OWN_ENEMY; owner_slot[o + k] = (uint8_t)(e | (k << 4));
        }
    }
    /* the two blocks that bounce when hit (Block_SprDataOffset: 4 sprites each, also the brick chunks) */
    for (int b = 0; b < 2; b++) {
        int o = ofs[8 + b] >> 2;
        for (int k = 0; k < 4 && o + k < 64; k++)
            if (owner[o + k] == OWN_OTHER) { owner[o + k] = OWN_BLOCK; owner_slot[o + k] = (uint8_t)(b | (k << 4)); }
    }
}

/* the player's pose: All-Stars keeps the 8 tiles of each pose (PlayerGfxOffset / 8) in bank $0A, Luigi's
   $4000 further, and shows them as OBJ tiles $00-$07 in the order of the NES pose table */
static void player_tiles(snesppu_t *p, const uint8_t *r, int screen_pose) {
    uint8_t pose = screen_pose >= 0 ? (uint8_t)screen_pose : r ? r[0x06D5] : ram[0x06D5];
    uint8_t luigi = r ? r[0x0753] : ram[0x0753];
    uint32_t src = PLAYER_FRAMES + (uint32_t)(pose >> 3) * 0x100 + (luigi ? 0x4000u : 0);
    for (uint32_t i = 0; i < 0x100; i++) p->vram[0x6000 * 2 + i] = rom8(src + i);
}

/* On the screens VS. draws the player itself (the continue screen's Mario, from sprite 0 on): its 8
   sprites are found by their tiles, a row of the pose table */
static int screen_owners(void) {
    memset(owner, OWN_OTHER, sizeof owner);
    memset(owner_slot, 0, sizeof owner_slot);
    for (int i = 0; i + 8 <= 64; i++)
        for (int pose = 0; pose < 26; pose++) {
            int k;
            for (k = 0; k < 8; k++) if (ppu.oam[(i + k) * 4 + 1] != cart.prg[VS_PLAYER_GFX - 0x8000 + pose * 8 + k]) break;
            if (k < 8) continue;
            for (k = 0; k < 8; k++) { owner[i + k] = OWN_PLAYER; owner_slot[i + k] = (uint8_t)k; }
            return pose * 8;
        }
    return -1;
}

/* the player's colours (OBJ palette 7) as All-Stars sets them: its GetPlayerColors ($04:9AF9) for
   Mario, Luigi and their fiery forms, and during the star its cycle ($03:B2C4: a set every 4 frames,
   every 16 when the star is running out) */
static void player_colors(snesppu_t *p, const uint8_t *r) {
    uint32_t src;
    int luigi = r[0x0753] != 0;
    if (r[0x079F]) {
        uint8_t a = r[0x09];
        if (r[0x079F] < 8) a >>= 2;
        src = STAR_COLORS + (uint32_t)(((a << 3) & 0x60) + (luigi ? 0x20 : 0));
    } else src = PLAYER_COLORS + (luigi ? 0x20u : 0) + (r[0x0756] == 2 ? 0x40u : 0);
    for (int i = 0; i < 16; i++) p->cgram[240 + i] = rom16(src + (uint32_t)i * 2);
}

/* Objects All-Stars draws with other tiles than the NES (its handlers' constants): fireballs and fire
   bars ($03:F443, tiles $BE/$BF), their explosion ($03:F4E6), the jumping coin ($05:DCA0: one 16x16
   sprite of the two NES ones, 4 pixels to the left), the lifts ($03:EC66, tile $87), brick chunks
   ($03:F3AC), hammers and air bubbles. Returns 0 for a sprite All-Stars has no counterpart for (the
   coin's lower half, a hammer's second half), -1 for one that is not among these. */
static int misc_sprite(snes_sprite_t *s, uint8_t tile, uint8_t nattr, int *sp) {
    switch (tile) {
    case 0x64: case 0x65: s->tile = (uint16_t)(0xBE + (tile - 0x64)); *sp = 4; return 1;
    case 0x66: s->tile = 0xCA; *sp = 4; return 1;
    case 0x67: s->tile = 0xCB; *sp = 4; return 1;
    case 0x68: s->tile = 0xCC; *sp = 4; return 1;
    case 0x60: case 0x61: case 0x62: case 0x63:
        if (nattr & 0x80) return 0;
        s->tile = (uint16_t)(0x28 + (tile - 0x60) * 2); s->large = 1; s->x -= 4; *sp = 4; return 1;
    case 0x80: case 0x81: case 0x82: case 0x83: {
        /* a hammer ($05:DD07): the NES draws a pose as two sprites, All-Stars as one 16x16 sprite at the
           first one's place, 8 pixels to the left; the first is told by its tile and mirroring */
        if ((nattr & 3) != 3) return -1;
        int flipped = (nattr & 0xC0) != 0, pose;
        static const int8_t first[2][4] = { { 0, -1, 1, -1 }, { -1, 2, -1, 3 } };   /* by tile - $80 */
        pose = first[flipped][tile - 0x80];
        if (pose < 0) return 0;
        s->tile = (pose & 1) ? 0x46 : 0x44; s->large = 1; s->x -= 8; *sp = 5; return 1;
    }
    case 0x74: s->tile = 0x9F; *sp = 5; return 1;    /* an air bubble ($05:E2AA) */
    case 0x5B: s->tile = 0x87; *sp = 6; return 1;
    case 0x84: s->tile = 0x84; *sp = 5; return 1;
    case 0x75: s->tile = 0x75; *sp = 6; return 1;
    }
    return -1;
}

/* a power-up's palette: All-Stars' table ($03:ECFF: mushroom 6, flower and star 4, 1-up 5), the flower
   and the star cycling through 4, 5, 6 and 3 every 2 frames ($03:ED4C) */
static int powerup_pal(const uint8_t *r) {
    static const uint8_t pals[4] = { 6, 4, 4, 5 };
    uint8_t type = r[0x0039] & 3;
    if (type == 1 || type == 2) {
        uint8_t a = r[0x09] & 6;
        if (a == 6) a = 0x0E;
        return ((a ^ 0x28) >> 1) & 7;
    }
    return pals[type];
}

/* the sprite list from an OAM: the PPU's (dx 0, real NULL), or one the game built beside the picture
   (widescreen_band_oam), whose entries equal to the PPU's own are the picture's and left out */
static void add_sprite(snesppu_t *p, int x, int y, uint16_t tile, int large, int vf, int hf, int prio, int pal) {
    if (p->nspr >= (int)(sizeof p->spr / sizeof p->spr[0])) return;
    snes_sprite_t *s = &p->spr[p->nspr++];
    s->x = (int16_t)x; s->y = (int16_t)y; s->tile = tile; s->large = (uint8_t)large;
    s->attr = (uint8_t)((vf << 7) | (hf << 6) | (prio << 4) | (pal << 1));
}

/* the arcade's sprite at OAM entry i of oam, if it is drawn: one of the picture's, or (with real, the
   PPU's own OAM) one the game built beside the picture that the picture does not have */
static int sprite_shown(const uint8_t *oam, int i, int dx, const uint8_t *real) {
    const uint8_t *o = &oam[i * 4];
    if (owner[i] == OWN_NONE || o[0] >= 0xEF) return 0;
    if (real) {
        const uint8_t *q = &real[i * 4];
        if (!memcmp(o, q, 4)) return 0;
        /* the player, wrapped round the moved screen's 8-bit X (widescreen.c's draw_sprites) */
        if (q[0] < 0xEF && o[0] == q[0] && o[1] == q[1] && o[2] == q[2] && o[3] + dx != q[3]) return 0;
    }
    return 1;
}

/* Objects All-Stars draws with routines of its own instead of its enemy table, each from one corner of
   the object, which any of its arcade sprites gives (they sit at fixed offsets from it):
   - the goal flag: one waving 16x16 sprite 16 pixels right of the NES's three ($05:EAA0);
   - the star flag over the castle: one 16x16 sprite behind the background ($03:D84C);
   - Bowser: one 32-pixel figure of four 16x16 and two 8x8 sprites for both halves ($05:D055), facing
     and upside down as the head is mirrored, the open mouth (tiles $C9/$CA) as its fire frames, the
     stepping legs ($CB/$CC) as its walking ones;
   - Bowser's flame: two overlapping 16x16 sprites, 24x16, and a spark it leaves falling ($05:CD01);
   - Bullet Bills: one 16x16 sprite, the cannon's behind the background as it leaves ($05:CB42);
   - Podoboos: one 16x16 sprite, upside down when falling ($05:EA38);
   - shells, kicked or still: tiles of their own, turning when kicked ($05:E879);
   - the vine: one column of 16x16 sprites as long as it has grown ($05:DA72);
   - the coin heavens' cloud lift: a 64-pixel cloud (48 in the second quest) with a face ($05:CDEB);
   - fireworks: a 32-pixel ring of sparks, four mirrored 16x16 quarters changing colour every frame
     ($03:F548);
   - Toad and the princess: see draw_retainer. */
enum { SP_NONE, SP_FLAG, SP_STARFLAG, SP_BOWSER, SP_FLAME, SP_BULLET, SP_PODOBOO, SP_SHELL, SP_VINE, SP_CLOUD, SP_FIREWORK, SP_RETAINER };
struct special { int kind, seen, x, y, pal, hf, vf, behind, mouth, legs, done; };

/* the kind of special object enemy slot e is, and the place of its arcade sprite k in it */
static int special_kind(const uint8_t *r, int e, int k, uint8_t tile, int *ox, int *oy) {
    uint8_t id = r[0x16 + e] & 0x3F, state = r[0x1E + e];
    *ox = (k & 1) * 8; *oy = (k >> 1) * 8;                              /* EnemyGfxHandler's 2x3 block */
    switch (id) {
    case 0x30: if (k >= 3) return SP_NONE; *ox = k ? 8 : 0; *oy = k == 2 ? 8 : 0; return SP_FLAG;    /* $7E $7F / . $7E */
    case 0x31: if (k >= 4) return SP_NONE; *ox = (k & 1) ? 0 : 8; *oy = k < 2 ? 8 : 0; return SP_STARFLAG;
    case 0x2D: return SP_BOWSER;
    case 0x15: if (k >= 3) return SP_NONE; *ox = k * 8; *oy = 0; return SP_FLAME;
    case 0x08: case 0x33: return SP_BULLET;
    case 0x0C: return SP_PODOBOO;
    case 0x35: return SP_RETAINER;
    case 0x16: if (k >= 4) return SP_NONE; *ox = (k >> 1) * 8 - 4; *oy = (k & 1) * 8 - 4; return SP_FIREWORK;   /* round its centre */
    case 0x2F:   /* a piece of vine (VineObjOffset): 6 sprites down from Enemy_Rel_YPos + $00 or $30, every other one 6 right */
        for (int y = 0; y < 2 && y < r[0x0398]; y++)
            if (r[0x039A + y] == e) { *ox = (k & 1) * 6; *oy = y * 0x30 + k * 8; return SP_VINE; }
        return SP_NONE;
    }
    if (id < 4 && ((state & 0x80) || state == 2 || state == 3 || state == 4)) return SP_SHELL;
    if (id >= 0x24 && id <= 0x2A && tile == 0x75 && r[0x0743]) { *ox = k * 8; *oy = 0; return SP_CLOUD; }   /* CloudTypeOverride */
    return SP_NONE;
}

static void special_objects(const uint8_t *r, const uint8_t *oam, int dx, const uint8_t *real, struct special sp[6]) {
    memset(sp, 0, sizeof(struct special) * 6);
    if (!r) return;
    for (int i = 0; i < 64; i++) {
        if (owner[i] != OWN_ENEMY || !sprite_shown(oam, i, dx, real)) continue;
        const uint8_t *o = &oam[i * 4];
        int e = owner_slot[i] & 15, ox, oy, kind = special_kind(r, e, owner_slot[i] >> 4, o[1], &ox, &oy);
        if (kind == SP_NONE) continue;
        struct special *s = &sp[e];
        if (!s->seen) {
            s->seen = 1; s->kind = kind; s->x = o[3] + dx - ox; s->y = o[0] - oy;
            s->pal = o[2] & 3; s->behind = (o[2] >> 5) & 1;
        }
        s->hf |= (o[2] >> 6) & 1;
        s->vf |= (o[2] >> 7) & 1;
        s->mouth |= o[1] == 0xC9 || o[1] == 0xCA;
        s->legs |= o[1] == 0xCB || o[1] == 0xCC;
    }
}

static void draw_bowser(snesppu_t *p, const uint8_t *r, struct special sp[6]) {
    int front = r[0x0368] < 6 && (r[0x16 + r[0x0368]] & 0x3F) == 0x2D ? r[0x0368] : -1, rear = -1;
    for (int e = 0; e < 6; e++)
        if ((r[0x16 + e] & 0x3F) == 0x2D && e != front) { if (front < 0) front = e; else if (rear < 0) rear = e; }
    if (front < 0) return;
    struct special *f = &sp[front], *b = rear >= 0 ? &sp[rear] : NULL;
    if (!f->seen && !(b && b->seen)) return;
    int left = f->seen ? f->hf : b->hf;   /* facing left: the head at the left, the NES mirrors it */
    int x0, y0;
    if (f->seen) { x0 = left ? f->x : f->x - 16; y0 = f->y; }
    else { x0 = left ? b->x - 16 : b->x; y0 = b->y - 8; }
    int upside = f->seen ? f->vf : b->vf;
    uint8_t fc = r[0x09];
    int frame = f->mouth ? 5 + ((fc >> 2) & 1) : (b && b->legs) ? ((fc & 0x20) ? 2 : 1) : 0;
    static const int8_t px[6] = { 0, 0, 8, 16, 16, 16 }, py[6] = { 0, 16, -8, 0, 16, -8 }, py_up[6] = { 8, -8, 24, 8, -8, 24 };
    for (int j = 0; j < 6; j++) {
        uint16_t t = (uint16_t)(0x100 | rom8(BOWSER_TILES + (uint32_t)((left ? 42 : 0) + frame * 6 + j)));
        add_sprite(p, x0 + px[j], y0 + (upside ? py_up[j] : py[j]) + 1, t, j != 2 && j != 5, upside, left, 2, 0);
    }
}

/* The castle's end. All-Stars loads two sets of OBJ tiles there, as $180-$1FF ($04:ECCA, $04:ED07);
   in worlds 1-7 Toad is tied up in a sack that wobbles ($04:ED7A), and once the player has stood
   before him for $70 frames ($03:8A55) each world has a scene of its own: the sack opening, Toad
   sitting up, jumping for joy... VS. goes from the walk straight to the messages, so the wait starts
   with them. That code runs here in a copy of the area's sandbox, once for each frame since the last
   picture, given VS.'s Toad, camera, frame counter and random bits; the sprites it writes are drawn,
   the uploads it asks its NMI for kept. All-Stars' player stops 31 pixels before him ($03:89EA; 16 more
   in world 3), VS.'s 17, so his scene is moved right by the difference. In world 8 the princess stands
   ($04:DCBB). */
static sbx_t *sb_end;
static struct {
    int live, n, level;
    uint64_t frame;
    uint8_t fc;
    snes_sprite_t spr[48];
    uint8_t vram[0x1000];   /* OBJ tiles $180-$1FF */
} cend;

static void retainer_step(const uint8_t *r, int e) {
    uint8_t *w = sb_end->wram;
    int cam = (r[0x071A] << 8) | r[0x071C];
    int x = ((r[0x6E + e] << 8) | r[0x87 + e]) + 14 + (r[0x075F] == 2 ? 16 : 0);
    int steps = (uint8_t)(r[0x09] - cend.fc);
    cend.fc = r[0x09];
    for (int k = steps - 1; k >= 0; k--) {
        w[0x09] = (uint8_t)(r[0x09] - k);
        w[0x07B9] = r[0x07A9]; w[0x07BA] = r[0x07AA];           /* PseudoRandomBitReg, $10 higher */
        w[0x42] = (uint8_t)cam; w[0x43] = (uint8_t)(cam >> 8);
        w[0x021A] = (uint8_t)x; w[0x79] = (uint8_t)(x >> 8);    /* its object 0 */
        w[0x03AE] = (uint8_t)(x - cam);
        w[0x075F] = r[0x075F]; w[0x0753] = r[0x0753];
        w[0x9E] = 0;
        if (!w[0x0F7E] && r[0x0772] >= 3 && ++w[0x0F7F] == 0x70) { w[0x0F7E] = 1; w[0x0F7F] = 0; }
        for (int i = 0; i < 128; i++) { w[0x0801 + i * 4] = 0xF0; w[0x0C00 + i * 4] = 0; }
        if (sb_call(sb_end, RETAINER_DRAW, 0, 400000) < 0) { cend.n = 0; return; }
        if (w[0x0B76]) {   /* an upload for its NMI ($03:9B42) */
            uint32_t src = ((uint32_t)w[0x0287] << 16) | (uint32_t)(w[0x0285] | (w[0x0286] << 8));
            unsigned dst = (unsigned)(w[0x028A] | (w[0x028B] << 8)) & 0x7FFF, size = (unsigned)(w[0x0288] | (w[0x0289] << 8));
            for (unsigned i = 0; i < size; i++) {
                unsigned b = dst * 2 + i;
                if (b >= 0xF000 && b < 0x10000) cend.vram[b - 0xF000] = rom8(src + i);
            }
            w[0x0B76] = 0;
        }
    }
    cend.n = 0;
    int rel = x - cam;
    for (int i = 0; i < 128 && cend.n < (int)(sizeof cend.spr / sizeof cend.spr[0]); i++) {
        const uint8_t *o = &w[0x0800 + i * 4];
        if (o[1] == 0xF0) continue;
        uint8_t sz = w[0x0C00 + i * 4];
        int sx = o[0] | ((sz & 1) << 8);
        while (sx < rel - 256) sx += 512;   /* the 9-bit X, beside the picture too */
        while (sx > rel + 256) sx -= 512;
        snes_sprite_t *s = &cend.spr[cend.n++];
        s->x = (int16_t)sx; s->y = (int16_t)(o[1] + 1);
        s->tile = (uint16_t)(o[2] | ((o[3] & 1) << 8)); s->attr = o[3]; s->large = (uint8_t)((sz >> 1) & 1);
    }
}

static void draw_retainer(snesppu_t *p, const uint8_t *r, int e, int x, int y) {
    int w8 = r[0x075F] == 7;
    int level = (r[0x075F] << 8) | r[0x075C];
    if (!cend.live || cend.level != level || ppu.frame - cend.frame > 600) {   /* the scene starts */
        if (!sb_end && !(sb_end = malloc(sizeof *sb_end))) return;
        memcpy(sb_end, sb, sizeof *sb);
        uint8_t *w = sb_end->wram;
        w[0x0F7D] = w[0x0F7E] = w[0x0F7F] = w[0x0B9C] = w[0x0B76] = 0;
        w[0x10] = 1; w[0x1C] = 0x35; w[0x29] = 0; w[0xBC] = 1;   /* its object 0: Toad */
        w[0x0238] = r[0xCF + e];                                  /* his height, All-Stars' own from now on */
        uint32_t a = RETAINER_GFX + (w8 ? 0x1000u : 0), b = 0x1E0000u | rom16(RETAINER_SETS + r[0x075F] * 2u);
        for (uint32_t i = 0; i < 0x800; i++) { cend.vram[i] = rom8(a + i); cend.vram[0x800 + i] = rom8(b + i); }
        cend.live = 1;
        cend.level = level;
        cend.fc = (uint8_t)(r[0x09] - 1);
    }
    cend.frame = ppu.frame;
    memcpy(&p->vram[0xF000], cend.vram, sizeof cend.vram);
    if (w8) {
        uint16_t t = (uint16_t)(0x100 | rom8(PRINCESS_TILES));
        add_sprite(p, x + 7, y, t, 1, 0, 0, 2, 2);
        add_sprite(p, x + 7, y + 16, (uint16_t)(t + 0x20), 1, 0, 0, 2, 2);
        return;
    }
    if (cend.fc != r[0x09]) retainer_step(r, e);
    for (int n = 0; n < cend.n && p->nspr < (int)(sizeof p->spr / sizeof p->spr[0]); n++) p->spr[p->nspr++] = cend.spr[n];
}

static void draw_special(snesppu_t *p, const uint8_t *r, int e, const struct special *s) {
    uint8_t fc = r[0x09], state = r[0x1E + e], id = r[0x16 + e] & 0x3F;
    int x = s->x, y = s->y + 1;                                         /* the NES shows OAM Y + 1 */
    switch (s->kind) {
    case SP_FLAG: {
        static const uint8_t wave[4] = { 0, 2, 4, 2 };
        add_sprite(p, x + 16, y, (uint16_t)(0x120 + wave[(fc & 0x18) >> 3]), 1, 0, 0, 2, 5);
        break;
    }
    case SP_STARFLAG:
        add_sprite(p, x, y, (uint16_t)(0x104 + ((fc & 0x18) >> 2)), 1, 0, 0, 0, 5);
        break;
    case SP_FLAME: {
        /* 3 frames of 8; at each turn it drops a spark where it is, which then falls a pixel every 2
           frames while the flame moves on left (1 1/4 pixels a frame, 1 3/8 in the second quest) */
        int t = fc % 24, f = t >> 3, back = t * (r[0x06CC] ? 11 : 10) / 8;
        uint16_t tile = (uint16_t)(0x100 | rom8(FLAME_TILES + (uint32_t)f));
        add_sprite(p, x, y, tile, 1, 0, 0, 2, 0);
        add_sprite(p, x + 8, y, (uint16_t)(tile + 1), 1, 0, 0, 2, 0);
        add_sprite(p, x + 16 + back, y + 4 + (t >> 1), rom8(FLAME_TILES + 3 + (uint32_t)f), 0, 0, 0, 2, 4);
        break;
    }
    case SP_BULLET: {
        int f = (state & 0x20) ? 0 : (fc % 37) >> 2;                   /* still once stomped */
        if (s->y + 8 < 0xE0)
            add_sprite(p, x, y + 8, rom8(BULLET_TILES + (uint32_t)f), 1, 0, r[0x46 + e] == 2, s->behind ? 0 : 2, 5);
        break;
    }
    case SP_PODOBOO:   /* 3 frames of 8 (its counter, $0E68, wraps at 24) */
        if (s->y < 0xE8) add_sprite(p, x, y, (uint16_t)(0x162 + (fc % 24 >> 3) * 2), 1, s->vf, 0, 2, 4);
        break;
    case SP_SHELL: {
        int still = state == 2 || state == 3 || state == 4, upside = (state & 0x0F) == 4;
        int turn = still ? 0 : fc & 0x0C, t0 = turn + (id == 2 ? 0x10 : 0) + (upside ? 0x20 : 0);
        if (s->y + 8 >= 0xF0) break;
        for (int j = 0; j < 4; j++)
            add_sprite(p, x + (j & 1) * 8, y + 8 + (j >> 1) * 8 + (upside ? 2 : 0), rom8(SHELL_TILES + (uint32_t)(t0 + j)), 0,
                       upside, (rom8(SHELL_FLIPS + (uint32_t)(turn + j)) >> 6) & 1, s->behind ? 0 : 2, enemy_pal[id][s->pal]);
        break;
    }
    case SP_VINE: {
        int n = (r[0x0399] >> 4) + 1;                                   /* VineHeight */
        for (int j = 0; j < n && j < 9; j++) add_sprite(p, x, y - 1 + 16 * j, j ? 0x12C : 0x12A, 1, 0, 0, 1, 5);
        break;
    }
    case SP_CLOUD: {
        static const uint8_t wide[4] = { 0, 1, 2, 3 }, narrow[3] = { 0, 1, 4 };
        int hard = r[0x06CC] != 0, ph = fc & 0x18;                      /* SecondaryHardMode */
        add_sprite(p, x + (hard ? 12 : 20), y + ((fc & 0x10) >> 4), 0xE7, 0, 0, 0, 2, 6);
        for (int j = 0; j < (hard ? 3 : 4); j++) {
            int i = hard ? narrow[j] : wide[j];
            uint8_t a = rom8(CLOUD_ATTRS + (uint32_t)(ph + i));
            add_sprite(p, x - 8 + rom8(CLOUD_X + (uint32_t)i), y - 4, rom8(CLOUD_TILES + (uint32_t)(ph + i)), 1, 0, (a >> 6) & 1, 2, (a >> 1) & 7);
        }
        break;
    }
    case SP_FIREWORK: {
        /* ExplosionGfxCounter's ring, and the colour by ExplosionTimerCounter (8 to 1) */
        int ring = r[0x58 + e] & 3, t = r[0xA0 + e] ? r[0xA0 + e] - 1 : 0;
        uint16_t tile = rom8(FIREWORK_TILES + (uint32_t)ring);
        int pal = (rom8(FIREWORK_PALS + (uint32_t)(t > 8 ? 8 : t)) >> 1) & 7;
        for (int j = 0; j < 4; j++)
            add_sprite(p, x - 8 + (j >> 1) * 16, y - 16 + (j & 1) * 16, tile, 1, j & 1, j >> 1, 2, pal);
        break;
    }
    case SP_RETAINER:
        draw_retainer(p, r, e, x, y);
        break;
    }
}

static void build_sprites(snesppu_t *p, const uint8_t *r, int ahead, int screen, const uint8_t *oam, int dx, const uint8_t *real) {
    p->nspr = 0;
    int screen_pose = -1;
    if (screen) screen_pose = screen_owners();
    else sprite_owners(r, ahead);
    player_tiles(p, r, screen_pose);
    player_colors(p, r ? r : ram);
    struct special spc[6];
    special_objects(screen ? NULL : r, oam, dx, real, spc);
    /* the player's mirroring, from its first sprite: the NES's ChkForPlayerAttrib mirrors only the second
       of the third and fourth rows' tiles in its standing, crouching, growing and dying poses, which
       All-Stars draws whole */
    int player_flip = -1;
    for (int i = 0; i < 64; i++)
        if (owner[i] == OWN_PLAYER && owner_slot[i] == 0) { player_flip = oam[i * 4 + 2] >> 6; break; }
    /* the castle's end clears no sprites: those its objects drew at other offsets in earlier frames stay,
       unseen on the NES under the same ones drawn again */
    uint8_t stale[64] = { 0 };
    for (int i = 0; i < 64; i++)
        if (owner[i] == OWN_OTHER)
            for (int j = 0; j < 64; j++)
                if ((owner[j] == OWN_ENEMY || owner[j] == OWN_PLAYER) && !memcmp(&oam[i * 4], &oam[j * 4], 4)) { stale[i] = 1; break; }
    /* the points that float up (FloateyNumbersRoutine, $86E1): two sprites side by side with the tiles of
       FloateyNumTileData ($867D) for a slot's FloateyNum_Control ($0110), in the scored object's sprites or
       a block object's. All-Stars draws them from its own table ($03:8B0C) in palette 1 at priority 3, the
       "000" of 1000-8000 one 16x16 sprite of its second table ($03:8BD5) */
    int8_t floatey[64];
    memset(floatey, -1, sizeof floatey);
    for (int f = 0; f < 7 && r && !screen; f++) {
        uint8_t c = r[0x0110 + f];
        if (!c || c > 0x0B) continue;
        uint8_t lt = cart.prg[VS_FLOATEY - 0x8000 + c * 2], rt = cart.prg[VS_FLOATEY + 1 - 0x8000 + c * 2];
        for (int i = 0; i + 1 < 64; i++) {
            const uint8_t *a = &oam[i * 4], *b = &oam[(i + 1) * 4];
            if (a[1] == lt && b[1] == rt && a[0] == b[0] && (uint8_t)(a[3] + 8) == b[3] && floatey[i] < 0) {
                floatey[i] = (int8_t)(c * 2); floatey[i + 1] = (int8_t)(c * 2 + 1);
            }
        }
    }
    int bowser_done = 0, vine_done = 0;
    for (int i = 0; i < 64; i++) {
        const uint8_t *o = &oam[i * 4];
        if (stale[i] || !sprite_shown(oam, i, dx, real)) continue;
        if (p->nspr + 12 > (int)(sizeof p->spr / sizeof p->spr[0])) break;
        uint8_t nattr = o[2], tile = o[1];
        if (floatey[i] >= 0) {
            int k = floatey[i], c = k >> 1;
            uint16_t t = rom8(FLOATEY_TILES + (uint32_t)k);
            if ((k & 1) && t == 0x0E) t = 0x10E;
            add_sprite(p, o[3] + dx, o[0] + 1, t, (k & 1) && c >= 6 && c != 0x0B, 0, 0, 3, 1);
            continue;
        }
        if (owner[i] == OWN_ENEMY && r && !screen) {
            int e = owner_slot[i] & 15, ox, oy, kind = special_kind(r, e, owner_slot[i] >> 4, tile, &ox, &oy);
            if (kind == SP_BOWSER) {
                if (!bowser_done) { bowser_done = 1; draw_bowser(p, r, spc); }
                continue;
            }
            if (kind == SP_VINE) {   /* both pieces are one vine */
                if (!vine_done) { vine_done = 1; draw_special(p, r, e, &spc[e]); }
                continue;
            }
            if (kind != SP_NONE) {
                if (!spc[e].done) { spc[e].done = 1; draw_special(p, r, e, &spc[e]); }
                continue;
            }
            if ((r[0x16 + e] & 0x3F) == 0x30) {   /* the flagpole's points: All-Stars' "000" of 5000 and 2000 is in its second table */
                add_sprite(p, o[3] + dx, o[0] + 1, tile == 0x50 ? 0x10E : tile, 0, 0, 0, 2, 1);
                continue;
            }
        }
        int pal = nattr & 3, hf = (nattr >> 6) & 1, vf = (nattr >> 7) & 1, behind = (nattr >> 5) & 1;
        snes_sprite_t *s = &p->spr[p->nspr];
        s->x = (int16_t)(o[3] + dx);
        s->y = (int16_t)(o[0] + 1);
        s->large = 0;
        int sp, misc;
        if (owner[i] == OWN_PLAYER) {
            int k = owner_slot[i];
            if (player_flip >= 0) { hf = player_flip & 1; vf = player_flip >> 1; }
            s->tile = (uint16_t)((k & 6) | ((k & 1) ^ hf));
            sp = 7;
        } else if (owner[i] == OWN_ENEMY && r) {
            int e = owner_slot[i] & 15, place = owner_slot[i] >> 4;
            uint8_t id = r[0x16 + e] & 0x3F;
            /* rows of two: the NES draws a mirrored row's tiles the other way round, and upside down
               (VerticalFlipFlag: a defeated enemy, $E9C9) it swaps the third row's tiles with the first
               row's for Hammer Bros, Lakitu and objects $15 on, with the second's for the rest */
            int row = place >> 1;
            if (vf && row) {
                int wide = id == 0x05 || id == 0x11 || id >= 0x15;
                row = row == 2 ? (wide ? 0 : 1) : (wide ? 1 : 2);
            } else if (vf && (id == 0x05 || id == 0x11 || id >= 0x15)) row = 2;
            int at = (row << 1) | ((place & 1) ^ hf);
            uint8_t moved = id == 0x2E ? powerup_tile[tile][at & 3] : enemy_tile[tile][at];
            if (!moved && (misc = misc_sprite(s, tile, nattr, &sp)) >= 0) {   /* a fire bar, a lift */
                if (!misc) continue;
            } else {
                s->tile = moved ? moved : tile;
                sp = id == 0x2E ? powerup_pal(r) : enemy_pal[id][pal];
            }
        } else if (owner[i] == OWN_BLOCK && tile >= 0x85 && tile <= 0x87) {
            /* a bouncing block ($03:F358): one 16x16 sprite at the first one's place, the used block or
               the brick (the NES's four show $87 for a used one) */
            if (owner_slot[i] >> 4) continue;
            int used = 0;
            for (int k = 0; k < 4 && i + k < 64; k++) used |= oam[(i + k) * 4 + 1] == 0x87;
            s->tile = used ? 0x100 : 0x102; s->large = 1; sp = 5; hf = vf = 0;
        } else if ((misc = misc_sprite(s, tile, nattr, &sp)) >= 0) {
            if (!misc) continue;
        } else {
            s->tile = tile;
            sp = pal_default[pal];
        }
        s->attr = (uint8_t)((vf << 7) | (hf << 6) | ((behind ? 0 : 2) << 4) | (sp << 1));
        p->nspr++;
    }
}

/* ---- the status bar ---- */

/* VS.'s status bar (rows 0-3 of nametable 0) in All-Stars' font on layer 3: the letters and digits
   have the NES numbers there too; blank, '-', 'x', '!' and the coin moved */
static uint16_t hud_word(uint8_t t) {
    switch (t) {
    case 0x24: return 0x2028;
    case 0x28: return 0x2024;
    case 0x29: return 0x2025;
    case 0x2B: return 0x2026;
    case 0x2E: return 0x2427;
    case 0xAF: return 0x2029;
    }
    if (t <= 0x23) return (uint16_t)(0x2000 | t);
    return 0x2028;
}

static void write_hud_to(sbx_t *s) {
    uint8_t *v = s->ppu.vram;
    const uint8_t *nt = ppu.vram + ppu.nt_offset[0];
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 64; col++) {
            uint16_t word = col < 32 ? hud_word(nt[row * 32 + col]) : 0x2028;
            uint32_t a = ((uint32_t)(0x5800 + (col >> 5) * 0x400 + row * 32 + (col & 31)) & 0x7FFF) * 2;
            v[a] = (uint8_t)word; v[a + 1] = (uint8_t)(word >> 8);
        }
}
static void write_hud(void) { write_hud_to(sb); }

/* ---- the screens that are not a level ---- */

/* the nametable's row holds this text (NES tile numbers: digits $00-$09, letters $0A-$23) */
static int nt_has(int row, const char *text) {
    const uint8_t *nt = ppu.vram + ppu.nt_offset[0] + row * 32;
    int n = (int)strlen(text);
    for (int c = 0; c + n <= 32; c++) {
        int k;
        for (k = 0; k < n; k++) {
            char ch = text[k];
            uint8_t t = ch >= '0' && ch <= '9' ? (uint8_t)(ch - '0') : (uint8_t)(ch - 'A' + 10);
            if (nt[c + k] != t) break;
        }
        if (k == n) return 1;
    }
    return 0;
}

/* The intermission ("WORLD 1-1", the lives) as All-Stars shows it: its DisplayIntermediate's three
   parts run in the screens' sandbox (the player's head and the world picture's objects in OAM, the
   picture on layers 1 and 2, the text on layer 3), then its NMI uploads them. Built when the world,
   level, lives or player change. */
static struct { int key; bool ok; } inter = { -1, false };

static void build_intermission(void) {
    int key = (ram[RAM_WORLD] << 16) | (ram[RAM_LEVEL] << 12) | (ram[0x075A] << 4) | (ram[0x0753] & 1);
    if (inter.key == key) return;
    inter.key = key;
    inter.ok = false;
    sbx_t *s = sb_screen;
    memcpy(s, sb_base, sizeof *s);
    uint8_t *w = s->wram;
    w[RAM_WORLD] = ram[RAM_WORLD]; w[RAM_LEVEL] = ram[RAM_LEVEL]; w[RAM_AREA_NUM] = ram[RAM_AREA_NUM];
    w[0x075A] = ram[0x075A]; w[0x0753] = ram[0x0753]; w[0x0756] = ram[0x0756];
    w[0x0770] = 1;
    w[0x0E21] = (uint8_t)(((ram[RAM_WORLD] & 7) << 2) + (ram[RAM_LEVEL] & 3) + 1);   /* the world picture's number */
    int type = ram[RAM_AREA_TYPE] & 3, num = ram[0x074F] & 31;
    w[0xDB] = area_map[type][num];
    w[0xBA] = (uint8_t)(w[0xDB] >= 0x1C ? 3 : w[0xDB] >= 0x19 ? 2 : w[0xDB] >= 0x03 ? 1 : 0);
    w[0x5C] = (uint8_t)type;
    for (int i = 0; i <= 0x1C; i++) w[0x0B45 + i] = rom8(SPR_OFFSETS + (uint32_t)i);   /* each of the picture's objects its own sprites */
    sb_call(s, CLEAR_SCREEN, 0, 200000);
    if (sb_call(s, INTER_PLAYER, 1, 400000) < 0) return;
    if (sb_call(s, INTER_PICTURE, 0, 1000000) < 0) return;
    s->cpu.a = 1;
    sb_call(s, WRITE_TEXT, 1, 200000);
    sb_call(s, INTER_LIVES, 1, 200000);
    sb_call(s, OAM_SIZES, 0, 100000);   /* the picture's 16x16 sprites (Bowser) */
    w[0x0154] = 0;
    sb_call(s, SMB1_NMI, 0, 400000);
    inter.ok = true;
}

static void write_hud_to(sbx_t *s);

/* a line of a screen that is not a level: the picture's 256 pixels, black bars beside it in the
   widescreen canvas (as with the arcade graphics) */
static void screen_line(const snesppu_t *p, int line, int x, int w, uint32_t *row) {
    snesppu_line(p, line, x, w, row);
    for (int px = x; px < x + w; px++) if (px < 0 || px >= 256) row[px - x] = 0xFF000000u;
}

static int render_intermission(uint32_t *out, int stride, int x, int y, int w, int h) {
    build_intermission();
    if (!inter.ok) return 0;
    write_hud_to(sb_screen);
    animate_tiles_in(sb_screen, ram[0x09]);
    static snesppu_t p;
    p = sb_screen->ppu;
    p.inidisp = 0x0F;
    snesppu_load_oam(&p, &sb_screen->wram[OAM_BUFFER]);
    for (int line = y; line < y + h && line < 240; line++)
        screen_line(&p, line, x, w, out + (size_t)(line - y) * (size_t)stride);
    return 1;
}

/* ---- what VS. shows that All-Stars has no art for ---- */

/* VS.'s nametables: a cell's palette (its attribute bits) and a row of a background tile's pixels */
static const uint8_t *nt_cells(int nt) { return ppu.vram + ppu.nt_offset[nt & 3]; }
static int nt_pal(int nt, int row, int col) {
    uint8_t at = nt_cells(nt)[0x3C0 + (row >> 2) * 8 + (col >> 2)];
    return (at >> (((row & 2) << 1) | (col & 2))) & 3;
}
static void nes_bg_row(uint8_t tile, int y, uint8_t out[8]) {
    uint16_t a = (uint16_t)(((ppu.frame_ctrl & 0x10) ? 0x1000 : 0) + tile * 16 + y);
    const uint8_t *chr = mapper_chr_bank_ptr(ppu.frame_chr_bank[a >> 10]);
    uint8_t p0 = chr[a & 0x3FF], p1 = chr[(a + 8) & 0x3FF];
    for (int x = 0; x < 8; x++) out[x] = (uint8_t)(((p0 >> (7 - x)) & 1) | (((p1 >> (7 - x)) & 1) << 1));
}
/* the arcade hides a line by giving it the backdrop's colours (PUSH BUTTON blinks so) */
static int nt_shown(int nt, int row, int col) {
    const uint8_t *pal = &ppu.palette[nt_pal(nt, row, col) * 4];
    uint8_t bd = ppu.palette[0] & 0x3F;
    return (pal[1] & 0x3F) != bd || (pal[2] & 0x3F) != bd || (pal[3] & 0x3F) != bd;
}
/* VS.'s font: digits $00-$09, letters $0A-$23, '-', 'x', '!', the coin, '.' and the copyright sign;
   no level metatile uses these tiles, so wherever they are in a nametable they are text */
static int vs_text(uint8_t t) { return t <= 0x23 || t == 0x28 || t == 0x29 || t == 0x2B || t == 0x2E || t == 0xAF || t == 0xCF; }

static uint32_t nes_color(uint8_t c) {
    static uint32_t rgb[512];
    static bool built;
    if (!built) { nes_palette_build_rgb(rgb, cart.vs_ppu); built = true; }
    return rgb[c & 63];
}
static uint16_t to_bgr15(uint32_t rgb) {
    return (uint16_t)(((rgb >> 19) & 31) | (((rgb >> 11) & 31) << 5) | (((rgb >> 3) & 31) << 10));
}

/* the 4bpp pixel of a tilemap word on layer 1 */
static int bg1_px(const snesppu_t *p, uint16_t word, int x, int y) {
    if (word & 0x4000) x = 7 - x;
    if (word & 0x8000) y = 7 - y;
    uint32_t a = ((((uint32_t)(p->bgnba[0] & 0x0F) << 12) + (word & 0x3FFu) * 16 + (uint32_t)y) & 0x7FFF) * 2;
    int s = 7 - x;
    return ((p->vram[a] >> s) & 1) | (((p->vram[a + 1] >> s) & 1) << 1) | (((p->vram[a + 16] >> s) & 1) << 2) | (((p->vram[a + 17] >> s) & 1) << 3);
}
static void put_tile4(snesppu_t *p, uint16_t tile, const uint8_t px[8][8]) {
    uint32_t a = ((((uint32_t)(p->bgnba[0] & 0x0F) << 12) + tile * 16u) & 0x7FFF) * 2;
    for (int y = 0; y < 8; y++) {
        uint8_t b[4] = { 0, 0, 0, 0 };
        for (int x = 0; x < 8; x++)
            for (int k = 0; k < 4; k++) b[k] |= (uint8_t)(((px[y][x] >> k) & 1) << (7 - x));
        p->vram[a + y * 2] = b[0]; p->vram[a + y * 2 + 1] = b[1];
        p->vram[a + 16 + y * 2] = b[2]; p->vram[a + 17 + y * 2] = b[3];
    }
}
static void put_bg1(snesppu_t *p, int row, int col, uint16_t word) {
    uint32_t a = ((((uint32_t)(p->bgsc[0] & 0xFC) << 8) + (uint32_t)((col >> 5) & 1) * 0x400 + (uint32_t)row * 32 + (uint32_t)(col & 31)) & 0x7FFF) * 2;
    p->vram[a] = (uint8_t)word; p->vram[a + 1] = (uint8_t)(word >> 8);
}

/* layer 1 tiles that are blank and that nothing on layers 1 and 2 uses: room for the new pictures,
   in the copy of the PPU a frame is drawn from */
static int free_tiles(const snesppu_t *p, int n, uint16_t *out) {
    static uint8_t used[1024];
    memset(used, 0, sizeof used);
    for (int layer = 0; layer < 2; layer++) {
        uint32_t base = (uint32_t)(p->bgsc[layer] & 0xFC) << 8;
        uint32_t size = 0x400u << ((p->bgsc[layer] & 1) + ((p->bgsc[layer] >> 1) & 1));
        for (uint32_t i = 0; i < size; i++) {
            uint32_t a = ((base + i) & 0x7FFF) * 2;
            used[(p->vram[a] | (p->vram[a + 1] << 8)) & 0x3FF] = 1;
        }
    }
    uint32_t chr = (uint32_t)(p->bgnba[0] & 0x0F) << 12;
    int k = 0;
    for (int t = 0x3FF; t >= 0x100 && k < n; t--) {
        if (used[t]) continue;
        const uint8_t *d = &p->vram[((chr + (uint32_t)t * 16) & 0x7FFF) * 2];
        int blank = 1;
        for (int i = 0; i < 32 && blank; i++) if (d[i]) blank = 0;
        if (blank) out[k++] = (uint16_t)t;
    }
    return k == n;
}

/* The arcade's text over a level (the title's lines, the castles' messages) in All-Stars' layer 1
   font, which has the NES numbers for the letters and digits; All-Stars' title shows its own lines so.
   '.' is only in the layer 3 font: it is copied to a free tile when a line needs it. */
static void level_text(snesppu_t *p, int title) {
    uint16_t dot = 0;
    int lv = ppu.line_v[120];
    int sx = ((((lv >> 10) & 1) << 8) | ((lv & 0x1F) << 3)) & 0x1FF;
    int c0 = sx >> 3;
    for (int k = 0; k <= 32; k++) {
        int wc = (c0 + k) & 63, nt = wc >> 5, col = wc & 31;
        const uint8_t *cells = nt_cells(nt);
        for (int row = 4; row < 30; row++) {
            uint8_t t = cells[row * 32 + col];
            if (!vs_text(t) || t == 0x2E || !nt_shown(nt, row, col)) continue;
            uint16_t pal = (uint16_t)(((title && row == 15) ? 1 : 2) << 10);   /* the copyright line, the rest */
            uint16_t word;
            if (t == 0xCF) word = pal | 0x22F;
            else if (t == 0xAF) {
                if (!dot) {
                    uint16_t slot;
                    if (!free_tiles(p, 1, &slot)) continue;
                    uint8_t px[8][8];
                    uint32_t a = ((((uint32_t)(p->bgnba[1] & 0x0F) << 12) + 0x29u * 8) & 0x7FFF) * 2;
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++)
                            px[y][x] = (uint8_t)(((p->vram[a + y * 2] >> (7 - x)) & 1) | (((p->vram[a + y * 2 + 1] >> (7 - x)) & 1) << 1));
                    put_tile4(p, slot, px);
                    dot = slot;
                }
                word = pal | dot;
            } else word = pal | t;
            put_bg1(p, row, wc, word);
        }
    }
}

/* ---- the title ---- */

/* All-Stars' title on layer 1, from its own DrawTitleScreen ($03:8E65: its stripes into the $1700
   buffer) and NMI, run once in the screens' sandbox */
#define DRAW_TITLE       0x038E65
static uint16_t as_title[32 * 32];
static int as_title_state;   /* 0 = not built, 1 = built, -1 = failed */

/* VS.'s banner (rows 4-14, columns 5-26) in All-Stars' art: All-Stars' banner one row higher, its
   SUPER six columns right, and before it "VS.": All-Stars' S and the dot of its BROS., and a V. */
static uint16_t vs_banner[11][22];
#define V_NEW 0xFFF0         /* the V's lower half: new tiles, placed in free slots when drawn */
static uint8_t v_lower[4][8][8];
static int v_state;          /* 0 = not made, 1 = made, -1 = the arcade's banner was not readable */

static void build_as_title(void) {
    if (as_title_state) return;
    as_title_state = -1;
    sbx_t *s = sb_screen;
    memcpy(s, sb_base, sizeof *s);
    inter.key = -1;                                  /* the intermission is rebuilt when shown next */
    s->wram[0x0770] = 0;
    s->wram[0x1700] = 0; s->wram[0x1701] = 0;
    if (sb_call(s, DRAW_TITLE, 1, 400000) < 0) return;
    s->wram[0x0154] = 0;
    if (sb_call(s, SMB1_NMI, 0, 400000) < 0) return;
    for (int i = 0; i < 32 * 32; i++) as_title[i] = (uint16_t)(s->ppu.vram[i * 2] | (s->ppu.vram[i * 2 + 1] << 8));
#define TW(r, c) as_title[(r) * 32 + (c)]
    for (int r = 0; r < 11; r++)
        for (int c = 0; c < 22; c++) vs_banner[r][c] = TW(r + 5, c + 5);
    for (int r = 0; r < 4; r++) {
        for (int c = 1; c <= 6; c++) vs_banner[1 + r][c] = TW(6, 16);           /* the box's fill */
        for (int c = 0; c < 10; c++) vs_banner[1 + r][7 + c] = TW(6 + r, 6 + c); /* SUPER */
        vs_banner[1 + r][3] = TW(6 + r, 6);                                       /* S */
        vs_banner[1 + r][4] = TW(6 + r, 7);
    }
    vs_banner[1][1] = TW(6, 8); vs_banner[1][2] = TW(6, 9);                       /* V: the U's top half */
    vs_banner[2][1] = TW(7, 8); vs_banner[2][2] = TW(7, 9);
    vs_banner[3][1] = V_NEW; vs_banner[3][2] = V_NEW + 1;
    vs_banner[4][1] = V_NEW + 2; vs_banner[4][2] = V_NEW + 3;
    vs_banner[3][5] = TW(14, 25);                                                 /* the dot */
    vs_banner[4][5] = TW(9, 10);                                                  /* and its shadow: a stroke's */
    as_title_state = 1;
}

/* the banner is still in this column of VS.'s first nametable (the demo scrolls it away) */
static int banner_column(int col) {
    const uint8_t *n = nt_cells(0);
    uint8_t top = n[4 * 32 + col], bottom = n[14 * 32 + col];
    return (top == 0x44 || top == 0x48 || top == 0x49) &&
           (bottom == 0x5F || bottom == 0x7A || bottom == 0x78 || (bottom >= 0x95 && bottom <= 0x98));
}

/* All-Stars has no V. The arcade made its V from its U, the U's top half and a narrowed lower half;
   this does the same to All-Stars' U: each pixel of the arcade V's lower half is painted as All-Stars
   paints a straight stroke at that height (its P's left stroke: outline, fill, the red band, the
   shadow), by how far it is from the letter's edge. The same rule turns the arcade's U into
   All-Stars' own U but for 6 of its 256 lower pixels. */
static void make_v(const snesppu_t *p) {
    if (v_state) return;
    v_state = -1;
    if (!banner_column(6) || !banner_column(7) || !banner_column(14) || !banner_column(15)) return;
    const uint8_t *n = nt_cells(0);
    uint8_t nu[32][16], nv[32][16];
    for (int tr = 0; tr < 4; tr++)
        for (int y = 0; y < 8; y++)
            for (int tc = 0; tc < 2; tc++) {
                nes_bg_row(n[(5 + tr) * 32 + 14 + tc], y, &nu[tr * 8 + y][tc * 8]);
                nes_bg_row(n[(5 + tr) * 32 + 6 + tc], y, &nv[tr * 8 + y][tc * 8]);
            }
    uint8_t box = nu[0][0], fill = nu[0][1], shadow = nu[4][7];
    if (box == fill || fill == shadow || box == shadow) return;
    uint8_t ap[32][8];
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 8; x++) ap[y][x] = (uint8_t)bg1_px(p, TW(6 + y / 8, 10), x, y & 7);
    for (int y = 16; y < 32; y++)
        for (int x = 0; x < 16; x++) {
            uint8_t m = nv[y][x], c;
            if (m == shadow) c = ap[8][7];
            else if (m != fill) c = ap[y][0];
            else {
                int d2 = 99;
                for (int yy = y - 3; yy <= y + 3; yy++)
                    for (int xx = x - 3; xx <= x + 3; xx++) {
                        int inside = yy >= 0 && yy < 32 && xx >= 0 && xx < 16 && nv[yy][xx] == fill;
                        int d = (xx - x) * (xx - x) + (yy - y) * (yy - y);
                        if (!inside && d < d2) d2 = d;
                    }
                c = d2 <= 2 ? ap[y][1] : d2 <= 4 ? ap[y][2] : ap[y][3];
            }
            v_lower[((y - 16) >> 3) * 2 + (x >> 3)][y & 7][x & 7] = c;
        }
    v_state = 1;
}

static void title_banner(snesppu_t *p) {
    build_as_title();
    if (as_title_state != 1) return;
    make_v(p);
    uint16_t slot[4];
    int have_v = v_state == 1 && free_tiles(p, 4, slot);
    if (have_v)
        for (int k = 0; k < 4; k++) put_tile4(p, slot[k], v_lower[k]);
    for (int c = 0; c < 22; c++) {
        if (!banner_column(5 + c)) continue;
        for (int r = 0; r < 11; r++) {
            uint16_t word = vs_banner[r][c];
            if (word >= V_NEW) word = have_v ? (uint16_t)((TW(6, 8) & 0xFC00) | slot[word - V_NEW]) : TW(8 + (word - V_NEW) / 2, 8 + (word - V_NEW) % 2);
            put_bg1(p, 4 + r, 5 + c, word);
        }
    }
#undef TW
}

/* ---- the text screens (PUSH BUTTON, GAME OVER, the continue timer...) ---- */

/* VS.'s own pictures there (the cabinet's buttons, the timer's box), from the arcade's pixels and
   colours drawn the way All-Stars draws its sprites: lit from the top left, shaded at the bottom
   right, outlined in a dark shade of their colour */
static uint32_t shade(uint32_t rgb, int num, int den, uint32_t to) {
    uint32_t out = 0;
    for (int k = 0; k < 24; k += 8) {
        int a = (int)((rgb >> k) & 255), b = (int)((to >> k) & 255);
        out |= (uint32_t)(a + (b - a) * num / den) << k;
    }
    return out;
}

static void screen_icons(snesppu_t *p, int nt) {
    static uint8_t val[240][256];
    static uint32_t col[240][256];
    static uint32_t rgb[240][256];
    int cells[64][2], ncell = 0;
    const uint8_t *n = nt_cells(nt);
    memset(val, 0, sizeof val);
    for (int row = 0; row < 30; row++)
        for (int c = 0; c < 32; c++) {
            uint8_t t = n[row * 32 + c];
            if (t == 0x24 || vs_text(t) || !nt_shown(nt, row, c) || ncell >= 64) continue;
            uint8_t px[8];
            int any = 0;
            for (int y = 0; y < 8; y++) {
                nes_bg_row(t, y, px);
                for (int x = 0; x < 8; x++) {
                    val[row * 8 + y][c * 8 + x] = px[x];
                    col[row * 8 + y][c * 8 + x] = nes_color(ppu.palette[nt_pal(nt, row, c) * 4 + px[x]]);
                    any |= px[x];
                }
            }
            if (any) { cells[ncell][0] = row; cells[ncell][1] = c; ncell++; }
        }
    if (!ncell) return;
    uint16_t slot[64];
    if (!free_tiles(p, ncell, slot)) return;
    /* the pixels' colours, then at most 15 of them for palette 7 */
    uint32_t pal[15];
    int npal = 0;
    for (int i = 0; i < ncell; i++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int py = cells[i][0] * 8 + y, px_ = cells[i][1] * 8 + x;
                uint32_t c = 0x1000000;   /* none */
                if (val[py][px_]) {
                    int lit = (py == 0 || !val[py - 1][px_]) || (px_ == 0 || !val[py][px_ - 1]);
                    int dark = (py == 239 || !val[py + 1][px_]) || (px_ == 255 || !val[py][px_ + 1]);
                    c = col[py][px_];
                    if (lit && !dark) c = shade(c, 2, 5, 0xFFFFFF);
                    else if (dark && !lit) c = shade(c, 2, 5, 0);
                } else {
                    /* the outline: a dark shade of the darkest colour beside it */
                    int lum = 1 << 30;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int yy = py + dy, xx = px_ + dx;
                            if (yy < 0 || yy >= 240 || xx < 0 || xx >= 256 || !val[yy][xx]) continue;
                            uint32_t n = col[yy][xx];
                            int l = (int)(((n >> 16) & 255) * 3 + ((n >> 8) & 255) * 6 + (n & 255));
                            if (l < lum) { lum = l; c = shade(n, 7, 10, 0); }
                        }
                }
                rgb[py][px_] = c;
                if (c > 0xFFFFFF) continue;
                int k;
                for (k = 0; k < npal && pal[k] != c; k++) {}
                if (k == npal && npal < 15) pal[npal++] = c;
            }
    for (int k = 0; k < npal; k++) p->cgram[7 * 16 + 1 + k] = to_bgr15(pal[k]);
    for (int i = 0; i < ncell; i++) {
        uint8_t px[8][8];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                uint32_t c = rgb[cells[i][0] * 8 + y][cells[i][1] * 8 + x];
                int best = 0;
                if (c <= 0xFFFFFF) {
                    long bd = -1;
                    for (int k = 0; k < npal; k++) {
                        long d = 0;
                        for (int s = 0; s < 24; s += 8) { long e = (long)((c >> s) & 255) - (long)((pal[k] >> s) & 255); d += e * e; }
                        if (bd < 0 || d < bd) { bd = d; best = k + 1; }
                    }
                }
                px[y][x] = (uint8_t)best;
            }
        put_tile4(p, slot[i], px);
        put_bg1(p, cells[i][0], cells[i][1], (uint16_t)(0x1C00 | slot[i]));
    }
}

/* a screen with no level: All-Stars' intermission backdrop and font (the screens' sandbox), VS.'s
   text on layer 3, its pictures on layer 1, the sprites as in a level */
static int render_text_screen(uint32_t *out, int stride, int x, int y, int w, int h) {
    build_intermission();
    if (!inter.ok) return 0;
    static snesppu_t p;
    p = sb_screen->ppu;
    p.inidisp = 0x0F;
    int nt = (ppu.line_v[120] >> 10) & 3;
    const uint8_t *n = nt_cells(nt);
    /* All-Stars' dark grey there is layer 1 filled with a solid tile: here it is the backdrop, so that
       the pictures on layer 1 have it around them; no colour math */
    uint16_t fill = (uint16_t)(p.vram[(32 + 1) * 2] | (p.vram[(32 + 1) * 2 + 1] << 8));
    p.cgram[0] = p.cgram[((fill >> 10) & 7) * 16 + bg1_px(&p, fill, 0, 0)];
    p.cgadsub = 0;
    for (int layer = 0; layer < 2; layer++) {
        uint32_t base = (uint32_t)(p.bgsc[layer] & 0xFC) << 8;
        for (uint32_t i = 0; i < 0x800; i++) {
            uint32_t a = ((base + i) & 0x7FFF) * 2;
            p.vram[a] = 0x24; p.vram[a + 1] = 0;
        }
    }
    for (int row = 0; row < 32; row++)
        for (int c = 0; c < 64; c++) {
            uint16_t word = 0x2028;
            if (row < 30 && c < 32 && vs_text(n[row * 32 + c]) && nt_shown(nt, row, c)) word = hud_word(n[row * 32 + c]);
            uint32_t a = ((uint32_t)(0x5800 + (c >> 5) * 0x400 + row * 32 + (c & 31)) & 0x7FFF) * 2;
            p.vram[a] = (uint8_t)word; p.vram[a + 1] = (uint8_t)(word >> 8);
        }
    screen_icons(&p, nt);
    for (int b = 0; b < 4; b++) { p.hofs[b] = 0; p.vofs[b] = 0; }
    build_sprites(&p, ram, 0, 1, ppu.oam, 0, NULL);
    for (int line = y; line < y + h && line < 240; line++)
        screen_line(&p, line, x, w, out + (size_t)(line - y) * (size_t)stride);
    return 1;
}

int allstars_render(uint32_t *out, int stride, int x, int y, int w, int h) {
    if (!allstars_enabled || !as.rom) return 0;
    if (!(ppu.line_emph[120] & 0x18)) return 0;   /* the screen is off (for the picture's lines, not just at its end) */
    if (ram[0x0770] == 2 && !widescreen_level_window(&(int){0}, &(int){0}) && (nt_has(9, "WORLD") || nt_has(10, "WORLD") || nt_has(11, "WORLD") || nt_has(12, "WORLD")))
        return render_intermission(out, stride, x, y, w, h);
    int lo, hi;
    if (!widescreen_level_window(&lo, &hi)) return render_text_screen(out, stride, x, y, w, h);
    if (area.resync || (!area.ready && !area.pending)) resync();
    if (!area.ready || !same_area()) return 0;
    keep_vs_column(last_world);
    animate_tiles(ram[0x09]);
    write_hud();
    int cam = camera_x();
    area.bg2_scroll = (uint16_t)(cam >> 1);
    int type = ram[RAM_AREA_TYPE] & 3;
    area.bg3_scroll = (uint16_t)(type == 1 ? cam >> 2 : cam);   /* the clouds far behind; the waves with the level */
    /* All-Stars' IRQ ($03:850D) at its line $0EF2: by day it scrolls layer 3 below the status bar; at
       night ($0ED1, $04:95CE: line $AF) layer 2 is split there instead: its NMI gives the layer the
       clouds' scroll ($0EEE, $03:84AD), the IRQ the hills' below ($0EFD), and layer 3 has no clouds */
    uint8_t irq_line = sb->wram[0x0EF2];
    bool irq = sb->wram[0x0EDE] != 0, starry = sb->wram[0x0ED1] != 0;
    write_bg1();
    if (starry) {
        int split = (irq_line + 1 + sb->ppu.vofs[1]) >> 3;   /* the first tile row below the split */
        if (split < 0) split = 0;
        if (split > 32) split = 32;
        write_bg2_rows(area.bg3_scroll, x, w, 0, split, -1);
        write_bg2_rows(area.bg2_scroll, x, w, split, 32, area.bg2_seam);
        write_bg3_plain(0);
    } else {
        write_bg2(area.bg2_scroll, x, w);
        if (type == 1) write_bg3_clouds(area.bg3_scroll, x, w);
        else write_bg3_plain(type == 0);
    }
    static snesppu_t p;
    p = sb->ppu;
    p.inidisp = 0x0F;   /* All-Stars fades its screens in and out; VS. cuts, so the picture is always at full brightness */
    uint8_t latch[2] = { sb->ofs_latch, sb->hofs_latch };
    int ahead = 0;
    const uint8_t *shown = widescreen_shown_ram(&ahead);
    /* the castle's end (VictoryMode) runs no GameEngine, so there is no copy of it; the NMI's
       SpriteShuffler still runs, so RAM is a frame ahead of the picture's sprites like a copy of this
       frame's GameEngine */
    if (!shown) { shown = ram; ahead = 1; }
    level_text(&p, ram[0x0770] == 0);
    if (ram[0x0770] == 0) title_banner(&p);
    /* the sprites of the picture, and beside it those the game builds there (widescreen) */
    static snes_sprite_t lists[3][128 + 64];
    int counts[3] = { 0, 0, 0 };
    build_sprites(&p, shown, ahead, 0, ppu.oam, 0, NULL);
    memcpy(lists[1], p.spr, sizeof(snes_sprite_t) * (size_t)p.nspr); counts[1] = p.nspr;
    for (int side = 0; side < 2; side++) {
        if (side ? x + w <= 256 : x >= 0) continue;
        uint8_t band[256];
        int dx;
        if (!widescreen_band_oam(side, band, &dx)) continue;
        build_sprites(&p, shown, ahead, 0, band, dx, ppu.oam);
        memcpy(lists[side * 2], p.spr, sizeof(snes_sprite_t) * (size_t)p.nspr); counts[side * 2] = p.nspr;
    }
    /* each part replays the frame's registers from its first line */
    static uint8_t regs[offsetof(snesppu_t, spr) - offsetof(snesppu_t, inidisp)];
    static uint16_t cgram[256];
    memcpy(regs, &p.inidisp, sizeof regs);
    memcpy(cgram, p.cgram, sizeof cgram);
    const int part[3][2] = { { x, x + w < 0 ? x + w : 0 }, { x > 0 ? x : 0, x + w < 256 ? x + w : 256 }, { x > 256 ? x : 256, x + w } };
    for (int k = 0; k < 3; k++) {
        int x0 = part[k][0], x1 = part[k][1];
        if (x0 >= x1) continue;
        memcpy(&p.inidisp, regs, sizeof regs);
        memcpy(p.cgram, cgram, sizeof cgram);
        memcpy(p.spr, lists[k], sizeof(snes_sprite_t) * (size_t)counts[k]); p.nspr = counts[k];
        uint8_t lt[2] = { latch[0], latch[1] };
        hdma_t hk;
        hdma_start(sb, &hk);
        hdma_line(sb, &hk, &p, lt);   /* the SNES shows its line 1 first: the NES line 0 comes after two HDMA lines */
        for (int line = 0; line < y + h && line < 240; line++) {
            if (line < 223) hdma_line(sb, &hk, &p, lt);
            /* the playfield's own scroll, from the PPU's record of each line */
            uint16_t lv = ppu.line_v[line];
            int fine = ppu.line_fx[line], nt = (lv >> 10) & 1;
            int sx = ((nt << 8) | ((lv & 0x1F) << 3) | fine) & 0x1FF;
            p.hofs[0] = (uint16_t)sx;
            p.vofs[0] = 0;
            p.hofs[1] = starry && !(irq && line > irq_line) ? area.bg3_scroll : area.bg2_scroll;
            p.hofs[2] = (irq && line > irq_line && !starry) ? area.bg3_scroll : 0;
            if (line < y) continue;
            uint32_t *row = out + (size_t)(line - y) * (size_t)stride;
            snesppu_line(&p, line, x0, x1 - x0, row + (x0 - x));
            /* beyond the level the game holds (before the area's start, past its rendering): black, as
               the arcade's widescreen canvas */
            for (int px = x0; px < x1; px++) if (px < lo || px >= hi) row[px - x] = 0xFF000000u;
        }
    }
    return 1;
}
