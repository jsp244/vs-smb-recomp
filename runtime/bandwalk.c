/*
 * bandwalk.c - with a widescreen view, the player may walk beyond the screen's left edge into the
 * band of level drawn beside the picture, as far as the band shows the level (widescreen.h).
 *
 * VS. Super Mario Bros. never scrolls back, and it stops the player at the left edge of its
 * 256-pixel screen; everything that places him on that screen works in 8-bit screen coordinates,
 * which wrap round to the right edge once he is left of it. A few patches (sys.patch, run just
 * before the instructions recomp/symbols.txt marks "patch") move the wall and keep the rest
 * consistent:
 *  - the wall: ScrollHandler puts him back at the edge when his offscreen bits say he is beyond it.
 *    He is let through as far as widescreen_walk pixels (the band the player sees), never past what
 *    the band shows (widescreen_level_left), and stopped there as the game stops him at the edge;
 *    not while he stands on a platform, which would carry him off it beside the screen.
 *  - the scroll: ScrollHandler scrolls when his position on the screen (last frame's, 8 bits) is
 *    past 80 pixels; beside the screen that position wraps to a large number, so it is replaced by
 *    the real one there and just after.
 *  - his sprites: their columns beside the screen would wrap round to its right edge. They are
 *    hidden in the real frame; the band's sprites (the engine run again with the camera moved, in
 *    the CPU sandbox, where these patches apply too) show him beside the picture.
 *  - collisions: his bounding box is clipped at the screen's left edge, as the game clips every
 *    other object's. An enemy or power-up the game skips because it is not wholly on the screen is
 *    tested when it or the player reaches beyond that edge, with both boxes moved for the test into
 *    an 8-bit frame where neither wraps, so the game's own rules decide (stomp, injury, power-up).
 *    Hammers and platforms do not touch him while he is wholly beside the screen.
 *  - scores: one for something beside the screen is shown at the screen's left edge.
 *  - the page before a part-way start (a halfway point, a pipe's entrance page): the game renders
 *    the area from its start page on, after clearing the whole block buffer, so the page before it,
 *    which the band draws, would have no ground. As the area starts, the band's rendering of that
 *    page gives the game its collision for the 128 pixels he can reach (widescreen_fill_collision).
 * With widescreen_walk 0 the patches change nothing. They read only the machine and that setting
 * (and the band's page before a part-way start, which follows the machine), so the interpreter and
 * the recompiled code agree, and run-ahead and save states stay deterministic.
 */
#include "widescreen.h"
#include "nes.h"
#include <string.h>

int widescreen_walk = 0;

/* RAM of the engine (as in the NES Super Mario Bros.) */
#define OBJ_X              0x0086   /* SprObject_X_Position: the player 0, the enemies 1-6 */
#define OBJ_PAGE           0x006D   /* SprObject_PageLoc */
#define OBJ_Y              0x00CE   /* SprObject_Y_Position */
#define OBJ_BOX_CTRL       0x0499   /* SprObj_BoundBoxCtrl: the box type */
#define BOX                0x04AC   /* BoundingBox_UL_Corner: x0 y0 x1 y1 per object */
#define OBJECT_OFFSET      0x0008   /* the enemy being processed */
#define LEFT_RIGHT_BUTTONS 0x000C   /* 1 right, 2 left */
#define PLAYER_X_SPEED     0x0057
#define PLAYER_SPR_OFS     0x06E4   /* where the player's 8 sprites start in the sprite page */
#define ENEMY_FLAG         0x000F
#define ENEMY_ID           0x0016
#define PLATFORM_FLAG      0x03A2   /* a platform's: the player stands on it */
#define SPRITES            0x0200
#define SCREEN_LEFT_PAGE   0x071A
#define SCREEN_LEFT_X      0x071C
#define BOX_TABLE          0xE153   /* BoundBoxCtrlData (ROM): x0 y0 x1 y1 offsets per box type */

/* the patched instructions */
#define P_WALL         0xAEB2   /* ChkPOffscr: STA $00 with the player's horizontal offscreen bits in A */
#define P_SCROLL       0xAE52   /* ScrollHandler: CMP #$50 with Player_Pos_ForScroll in A */
#define P_SPRITES      0xADC6   /* GameEngine: the player's sprites are built (JSR BlockObjMT_Updater) */
#define P_PLAYER_BOX   0xB007   /* PlayerSubs: his bounding box is built (JSR PlayerBGCollision) */
#define P_ENEMY_GATE   0xD7B7   /* PlayerEnemyCollision: BNE past an enemy not wholly on the screen */
#define P_ENEMY_TEST   0xD7C8   /* ... JSR PlayerCollisionCore, Y = the enemy's box */
#define P_ENEMY_DONE   0xD7CB   /* ... after it */
#define P_ENEMY_GATE2  0xD7F4   /* HandlePECollisions: BNE past one touched before, or not wholly on the screen */
#define P_SCORE_ENEMY  0xD978   /* SetupFloateyNumber: STA FloateyNum_X_Pos with the enemy's screen X */
#define P_SCORE_PLAYER 0xD4E8   /* the same with the player's */
#define P_HAMMER       0xD734   /* BCC after a hammer's test against the player */
#define P_LARGE_PLAT   0xDACA   /* BCC after a large platform's */
#define P_SMALL_PLAT   0xDAF6   /* BCS after a small platform's */
#define P_AREA_START   0x9687   /* InitializeArea: STA BackloadingFlag with the start page in A */
#define COLLISION_BITS 0x0491   /* Enemy_CollisionBits: d0 = touching the player already */
static const uint16_t patched[] = { P_WALL, P_SCROLL, P_SPRITES, P_PLAYER_BOX, P_ENEMY_GATE, P_ENEMY_TEST,
                                    P_ENEMY_DONE, P_ENEMY_GATE2, P_SCORE_ENEMY, P_SCORE_PLAYER, P_HAMMER,
                                    P_LARGE_PLAT, P_SMALL_PLAT, P_AREA_START };

/* Positions are page:pixel, 16 bits, measured from the screen's left edge as the game measures them,
   by a 16-bit difference: page $FF, a step left of an area's start, is left of a screen at page 0 */
static int screen_left(void) { return ram[SCREEN_LEFT_PAGE] * 256 + ram[SCREEN_LEFT_X]; }
static int rel_x(int s) {
    return (int16_t)(uint16_t)(ram[OBJ_PAGE + s] * 256 + ram[OBJ_X + s] - screen_left());
}

/* the box BoundingBoxCore gives object s, with x from the screen's left edge without the 8-bit wrap
   (y as the game has it) */
typedef struct { int x0, y0, x1, y1; } box_t;
static box_t screen_box(int s) {
    uint16_t t = (uint16_t)(BOX_TABLE + ram[OBJ_BOX_CTRL + s] * 4);
    int x = rel_x(s);
    box_t b;
    b.x0 = x + (int8_t)mem_read(t);
    b.x1 = x + (int8_t)mem_read((uint16_t)(t + 2));
    b.y0 = (uint8_t)(ram[OBJ_Y + s] + mem_read((uint16_t)(t + 1)));
    b.y1 = (uint8_t)(ram[OBJ_Y + s] + mem_read((uint16_t)(t + 3)));
    return b;
}

static int touch(const box_t *a, const box_t *b) {
    return a->x0 <= b->x1 && b->x0 <= a->x1 && a->y0 <= b->y1 && b->y0 <= a->y1;
}

/* the player stands on a platform (the flag its collision code leaves) */
static int on_platform(void) {
    for (int k = 0; k < 6; k++) {
        if (!ram[ENEMY_FLAG + k]) continue;
        int id = ram[ENEMY_ID + k];
        if (id >= 0x24 && id <= 0x2A && ram[PLATFORM_FLAG + k] != 0xFF) return 1;   /* large: $FF = not on it */
        if ((id == 0x2B || id == 0x2C) && ram[PLATFORM_FLAG + k] != 0) return 1;     /* small: 0 = not on it */
    }
    return 0;
}

/* how far beyond the screen's left edge the wall is now; at most MAX_WALK, as the band's sprites come
   from the engine run with the camera moved 112 pixels, and he would not be drawn further out */
#define MAX_WALK 104
static int room(void) {
    int r = widescreen_level_left();
    if (r > widescreen_walk) r = widescreen_walk;
    return r > MAX_WALK ? MAX_WALK : r;
}

static void patch_wall(void) {
    int x = rel_x(0);
    if (x > 0) return;                              /* on the screen: the game's own edges (at the edge
                                                       itself the game stops him already) */
    int r = on_platform() ? 0 : room();
    if (r <= 0) return;                             /* the game puts him back at the edge */
    if (x <= -r) {                                  /* at or past the band's edge: stopped there as at the screen's */
        int wall = screen_left() - r;               /* never before the area's start: room() sees to that */
        ram[OBJ_X] = (uint8_t)wall;
        ram[OBJ_PAGE] = (uint8_t)(wall >> 8);
        if (ram[LEFT_RIGHT_BUTTONS] != 0x01) ram[PLAYER_X_SPEED] = 0;
    }
    cpu.a = 0;                                      /* on the screen, as far as this routine goes */
}

static void patch_scroll(void) {
    int rel = rel_x(0);
    /* beside the screen, or back on it with last frame's wrapped position (which can be past 128
       only for a player who was far right, not a few pixels from the left edge) */
    if (rel < 0 || (cpu.a >= 0x80 && rel < 0x40)) cpu.a = (uint8_t)(rel < 0 ? 0 : rel);
}

static void patch_sprites(void) {
    if (rel_x(0) >= 0) return;
    /* beside the screen his columns' X wrapped round past 128; a column still on it is at 0-7 */
    for (int k = 0; k < 8; k++) {
        uint8_t *spr = ram + SPRITES + ((ram[PLAYER_SPR_OFS] + k * 4) & 0xFF);
        if (spr[3] >= 0x80) spr[0] = 0xF8;
    }
}

static void patch_player_box(void) {
    box_t b = screen_box(0);
    if (b.x0 >= 0) return;                          /* wholly on the screen: as the game made it */
    ram[BOX + 0] = 0;                               /* the part on the screen (none: 0, 0) */
    ram[BOX + 2] = (uint8_t)(b.x1 < 0 ? 0 : b.x1);
}

/* an enemy (slot) beside the screen that the game would skip: tested when it touches the player
   where the band shows it. The handler of a contact skips it once more for the same reason
   (HandlePECollisions); let_through says which enemy that check is to let pass. */
static int let_through;   /* slot + 1, 0 = none */

static void patch_enemy_gate(void) {
    let_through = 0;
    if (cpu.a == 0) return;                         /* wholly on the screen: the game tests it */
    box_t p = screen_box(0), e = screen_box(cpu.x + 1);
    if (p.x0 >= 0 && e.x0 >= 0) return;            /* off the screen elsewhere (the right, below) */
    if (e.x1 < -room() || !touch(&p, &e)) return;
    cpu.a = 0; cpu.z = 1; cpu.n = 0;
    let_through = cpu.x + 1;
}

static void patch_enemy_gate2(void) {   /* A = touched before | the offscreen bits */
    if (let_through == cpu.x + 1) {
        cpu.a = ram[COLLISION_BITS + cpu.x] & 1;
        cpu.z = cpu.a == 0; cpu.n = 0;
    }
    let_through = 0;
}

/* the test itself, in an 8-bit frame where neither box wraps; the boxes are put back after it */
static struct { int on, ofs; uint8_t player[4], other[4]; } moved;

static void patch_enemy_test(void) {
    box_t p = screen_box(0), e = screen_box(ram[OBJECT_OFFSET] + 1);
    moved.on = 0;
    if (p.x0 >= 0 && e.x0 >= 0) return;            /* both on the screen: the game's own boxes */
    int origin = (p.x0 < e.x0 ? p.x0 : e.x0) - 8;
    int far = p.x1 > e.x1 ? p.x1 : e.x1;
    if (far - origin > 255) return;                 /* too far apart to touch; the game's boxes say so too */
    moved.on = 1;
    moved.ofs = cpu.y;
    memcpy(moved.player, ram + BOX, 4);
    memcpy(moved.other, ram + BOX + cpu.y, 4);
    ram[BOX + 0] = (uint8_t)(p.x0 - origin);
    ram[BOX + 2] = (uint8_t)(p.x1 - origin);
    uint8_t *o = ram + BOX + cpu.y;
    o[0] = (uint8_t)(e.x0 - origin); o[1] = (uint8_t)e.y0;
    o[2] = (uint8_t)(e.x1 - origin); o[3] = (uint8_t)e.y1;
}

static void patch_enemy_done(void) {
    if (!moved.on) return;
    memcpy(ram + BOX, moved.player, 4);
    memcpy(ram + BOX + moved.ofs, moved.other, 4);
    moved.on = 0;
}

static void patch_score_enemy(void) {
    if (rel_x(ram[OBJECT_OFFSET] + 1) < 0) cpu.a = 0;
}

static void patch_score_player(void) {
    if (rel_x(0) < 0) cpu.a = 0;
}

/* a hammer or platform test against the player: no contact while he is wholly beside the screen (his
   box is then the game's empty one at the edge, which anything clipped there would seem to touch) */
static void patch_wholly_beside(void) {
    if (screen_box(0).x1 < 0) cpu.c = 0;
}

/* an area starting part-way: the page before it gets its collision (not in the band's own runs of
   InitializeArea, which render that page) */
static void patch_area_start(void) {
    if (!sys.sandbox && cpu.a > 0) widescreen_fill_collision(cpu.a);
}

static void bandwalk_patch(uint16_t pc) {
    if (widescreen_walk <= 0) return;
    switch (pc) {
    case P_WALL: patch_wall(); break;
    case P_SCROLL: patch_scroll(); break;
    case P_SPRITES: patch_sprites(); break;
    case P_PLAYER_BOX: patch_player_box(); break;
    case P_ENEMY_GATE: patch_enemy_gate(); break;
    case P_ENEMY_TEST: patch_enemy_test(); break;
    case P_ENEMY_DONE: patch_enemy_done(); break;
    case P_ENEMY_GATE2: patch_enemy_gate2(); break;
    case P_SCORE_ENEMY: patch_score_enemy(); break;
    case P_SCORE_PLAYER: patch_score_player(); break;
    case P_HAMMER: case P_LARGE_PLAT: case P_SMALL_PLAT: patch_wholly_beside(); break;
    case P_AREA_START: patch_area_start(); break;
    default: break;
    }
}

void bandwalk_attach(int on) {
    for (size_t k = 0; k < sizeof patched / sizeof patched[0]; k++) {
        uint16_t a = patched[k];
        if (on) sys_patch_map[a >> 3] |= (uint8_t)(1 << (a & 7));
        else sys_patch_map[a >> 3] &= (uint8_t)~(1 << (a & 7));
    }
    if (on) sys.patch = bandwalk_patch;
    else if (sys.patch == bandwalk_patch) sys.patch = NULL;
}
