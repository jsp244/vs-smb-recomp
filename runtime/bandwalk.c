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
 *    an 8-bit frame where neither wraps, so the game's own rules decide (stomp, injury, power-up);
 *    so are his fireballs against the enemies there, and the enemies there against each other
 *    (bumping, a kicked shell). Hammers and platforms do not touch him while he is wholly beside
 *    the screen.
 *  - other objects beside the screen: his fireballs and a vine, which the game erases as soon as
 *    they leave the screen, stay while they are in the band (a fireball still goes below the
 *    screen, and both go once they have left the band); the vine stays climbable, and grabbing it
 *    there gives him its page. Their sprites, a coin or score knocked out of a block and a brick's
 *    chunks have their 8-bit X wrapped round to the right edge there; they are hidden in the real
 *    frame, and the band draws them.
 *  - firebars: one whose centre is in the band is processed as on the screen, its parts placed and
 *    tested against him by their true places (the game's test of 8-bit X let one 256 pixels to his
 *    right hurt him while he stood in the band).
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
#define P_FBALL_ERASE  0xB593   /* FireballObjCore: BNE to its erasure, its offscreen bits & $CC in A */
#define P_FBALL_DRAWN  0xB51B   /* ProcFireball_Bubble: both fireballs moved and drawn (LDA AreaType) */
#define P_FBALL_GATE   0xD66A   /* FireballEnemyCollision: BNE past an enemy not wholly on the screen */
#define P_FBALL_TEST   0xD673   /* ... JSR SprObjectCollisionCore, Y = the fireball's box, X = the enemy's */
#define P_FBALL_DONE   0xD676   /* ... after it */
#define P_EE_GATE1     0xD9A5   /* EnemiesCollision: BNE out when this enemy is not wholly on the screen */
#define P_EE_GATE2     0xD9C6   /* ... BNE past another enemy not wholly on the screen */
#define P_EE_TEST      0xD9CF   /* ... JSR SprObjectCollisionCore, Y = this enemy's box, X = the other's */
#define P_EE_DONE      0xD9D2   /* ... after it */
#define P_VINE         0xB83C   /* VineObjectHandler: BEQ past the vine's erasure, its offscreen bits & $0C in A */
#define P_VINE_LEFT    0xB878   /* VS.'s own vine check: BMI to its erasure when it is left of the screen */
#define P_VINE_NEAR    0xB87C   /* ... BCS past it when it is 9 pixels or more from the screen's left edge */
#define P_VINE_PAGE    0xDE13   /* PutPlayerOnVine: STA Player_PageLoc with ScreenRight_PageLoc + ClimbPLocAdder */
#define P_MISC_DRAWN   0xADD9   /* GameEngine: the block and misc objects (chunks, jumping coins, points) drawn */
#define P_FBAR_SKIP    0xCC87   /* ProcFirebar: BNE past it all, Enemy_OffscreenBits & 8 in A (its centre off the screen) */
#define P_FBAR_CENTRE  0xCCC8   /* ... its centre's sprite written, Y = its offset (LDA #$01) */
#define P_FBAR_WRAP    0xCD29   /* DrawFirebar_Collision: CMP #$59 with a part's distance from the centre in 8 bits */
#define P_FBAR_PART    0xCD49   /* ... STA $07 just after the part's sprite Y (STA $0200,Y) */
#define P_FBAR_EDGE    0xCD87   /* FirebarCollision: BCS past a part whose X ($06) is $F0 or more */
#define P_FBAR_X       0xCD94   /* ... BPL with the player's sprite 1 X + 4 less the part's X in A */
#define ENEMY_REL_X    0x03AE   /* Enemy_Rel_XPos */
#define PART_X         0x0006   /* the firebar part's X while it is drawn and tested */
#define MISC_STATE     0x002A   /* Misc_State: 0 none, d7 a hammer, else a jumping coin and then its points */
#define MISC_SPR_OFS   0x06F3   /* Misc_SprDataOffset: their two sprites */
#define MISC_OBJ       13       /* the misc objects' SprObject index (13-21) */
#define BLOCK_STATE    0x0026   /* Block_State: 0 none, else a block bouncing or a brick's chunks */
#define BLOCK_OBJ      9        /* the block objects' SprObject index (9, 10); their sprites at ALT_SPR_OFS */
#define SCREEN_RIGHT_PAGE 0x071B
#define COLLISION_BITS 0x0491   /* Enemy_CollisionBits: d0 = touching the player already */
#define FBALL_STATE    0x0024   /* Fireball_State: 0 none, d7 exploding */
#define FBALL_SPR_OFS  0x06F1   /* FBall_SprDataOffset: a flying fireball's sprite */
#define ALT_SPR_OFS    0x06EC   /* Alt_SprDataOffset: its explosion's four */
#define ENEMY_SPR_OFS  0x06E5   /* Enemy_SprDataOffset */
#define VINE_PIECES    0x0398   /* VineFlagOffset: how many vine objects */
#define VINE_SLOTS     0x039A   /* VineObjOffset: their enemy slots */
#define FBALL_OBJ      7        /* the fireballs' SprObject index (7, 8) */
static const uint16_t patched[] = { P_WALL, P_SCROLL, P_SPRITES, P_PLAYER_BOX, P_ENEMY_GATE, P_ENEMY_TEST,
                                    P_ENEMY_DONE, P_ENEMY_GATE2, P_SCORE_ENEMY, P_SCORE_PLAYER, P_HAMMER,
                                    P_LARGE_PLAT, P_SMALL_PLAT, P_AREA_START, P_FBALL_ERASE, P_FBALL_DRAWN,
                                    P_FBALL_GATE, P_FBALL_TEST, P_FBALL_DONE, P_VINE, P_VINE_LEFT, P_VINE_NEAR,
                                    P_VINE_PAGE, P_MISC_DRAWN, P_EE_GATE1, P_EE_GATE2, P_EE_TEST, P_EE_DONE,
                                    P_FBAR_SKIP, P_FBAR_CENTRE, P_FBAR_WRAP, P_FBAR_PART, P_FBAR_EDGE, P_FBAR_X };

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

/* n sprites from offset ofs of an object beside the screen's left edge: those whose 8-bit X wrapped round
   past 128 are hidden in the real frame (a column still on the screen is at 0-7); the band's run of the
   engine, with the camera moved, draws them beside the picture */
static void hide_wrapped(uint8_t ofs, int n) {
    for (int k = 0; k < n; k++) {
        uint8_t *spr = ram + SPRITES + ((ofs + k * 4) & 0xFF);
        if (spr[3] >= 0x80) spr[0] = 0xF8;
    }
}

/* object s is at the screen's left side, so that its horizontal offscreen bits are the left edge's (the
   game counts its first columns as off the screen too), and not wholly beyond the band */
static int left_in_band(int s) {
    return rel_x(s) < 128 && screen_box(s).x1 >= -room();
}

static void patch_sprites(void) {
    if (rel_x(0) >= 0) return;
    hide_wrapped(ram[PLAYER_SPR_OFS], 8);
}

static void patch_player_box(void) {
    box_t b = screen_box(0);
    if (b.x0 >= 0) return;                          /* wholly on the screen: as the game made it */
    ram[BOX + 0] = 0;                               /* the part on the screen (none: 0, 0) */
    ram[BOX + 2] = (uint8_t)(b.x1 < 0 ? 0 : b.x1);
}

/* an enemy (slot) beside the screen, where the band shows it, that the game would skip: tested as the
   game tests one on the screen, every other frame, with both boxes moved (patch_enemy_test), so that
   its own code decides a contact and, when there is none, clears the enemy's touching bit
   (Enemy_CollisionBits d0, set at a contact so that it counts once). Testing it only while it touched
   the player left that bit set once they parted, and HandlePECollisions then let nothing more happen
   between them: a shell stomped there could not be kicked. The handler of a contact skips it once
   more for being off the screen; let_through says which enemy that check is to let pass. */
static int let_through;   /* slot + 1, 0 = none */

static void patch_enemy_gate(void) {
    let_through = 0;
    if (cpu.a == 0) return;                         /* wholly on the screen: the game tests it */
    box_t p = screen_box(0), e = screen_box(cpu.x + 1);
    if (p.x0 >= 0 && e.x0 >= 0) return;            /* off the screen elsewhere (the right, below) */
    if (e.x1 < -room()) return;                     /* beyond the band */
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

/* The player's fireballs. FireballObjCore erases one as soon as its offscreen bits say it is beside the
   screen, so one thrown in the band, or flying into it, went at once. Beside the screen's left edge
   within the band only its vertical bits count (it still goes below the screen, or once it has left the
   band as at the screen's edge). DrawFireball writes its 8-bit X with no such test, so there it wraps
   round to the right edge and is hidden once both are drawn. FireballEnemyCollision skips an enemy not
   wholly on the screen: one in the band is tested, both boxes moved as for the player's test. */
static void patch_fball_erase(void) {
    if (!(cpu.a & 0x0C) || !left_in_band(FBALL_OBJ + ram[OBJECT_OFFSET])) return;
    cpu.a &= 0xC0;
    cpu.z = cpu.a == 0; cpu.n = (cpu.a & 0x80) != 0;
}

static void patch_fball_drawn(void) {
    for (int k = 0; k < 2; k++) {
        if (!ram[FBALL_STATE + k] || rel_x(FBALL_OBJ + k) >= 0) continue;
        if (ram[FBALL_STATE + k] & 0x80) hide_wrapped(ram[ALT_SPR_OFS + k], 4);   /* its explosion */
        else hide_wrapped(ram[FBALL_SPR_OFS + k], 1);
    }
}

/* an enemy not wholly on the screen that the game skips, let through while it is in the band (the gates of
   FireballEnemyCollision and EnemiesCollision, A = its masked offscreen bits, X = its slot); one partly
   above or below the screen (a vertical bit, d4-d7) is skipped there as anywhere */
static void patch_object_gate(void) {
    if (cpu.a == 0 || (cpu.a & 0xF0) || !left_in_band(cpu.x + 1)) return;
    cpu.a = 0; cpu.z = 1; cpu.n = 0;
}

/* the test of two objects' boxes (SprObjectCollisionCore with Y and X their box offsets), moved for it into an
   8-bit frame where neither wraps when either is beside the screen, and put back after it */
static struct { int on, ofs[2]; uint8_t saved[2][4]; } pmoved;

static void move_pair(int obj_y, int obj_x) {
    box_t b[2] = { screen_box(obj_y), screen_box(obj_x) };
    int ofs[2] = { cpu.y, cpu.x };
    pmoved.on = 0;
    if (b[0].x0 >= 0 && b[1].x0 >= 0) return;      /* both on the screen: the game's own boxes */
    int origin = (b[0].x0 < b[1].x0 ? b[0].x0 : b[1].x0) - 8;
    int far = b[0].x1 > b[1].x1 ? b[0].x1 : b[1].x1;
    if (far - origin > 255) return;
    pmoved.on = 1;
    for (int k = 0; k < 2; k++) {
        uint8_t *o = ram + BOX + ofs[k];
        pmoved.ofs[k] = ofs[k];
        memcpy(pmoved.saved[k], o, 4);
        o[0] = (uint8_t)(b[k].x0 - origin); o[1] = (uint8_t)b[k].y0;
        o[2] = (uint8_t)(b[k].x1 - origin); o[3] = (uint8_t)b[k].y1;
    }
}

static void put_back_pair(void) {
    if (!pmoved.on) return;
    for (int k = 0; k < 2; k++) memcpy(ram + BOX + pmoved.ofs[k], pmoved.saved[k], 4);
    pmoved.on = 0;
}

/* Y = the fireball's box, X = the enemy's; $01 = the enemy's slot */
static void patch_fball_test(void) { move_pair(FBALL_OBJ + ram[OBJECT_OFFSET], ram[0x01] + 1); }

/* EnemiesCollision: the enemies bump or a kicked shell knocks one over only on the screen, so a shell sliding
   in the band went through what it met there. Y = the enemy's box (ObjectOffset), X = the other's ($01) */
static void patch_enemies_test(void) { move_pair(ram[OBJECT_OFFSET] + 1, ram[0x01] + 1); }

/* A vine (VineObjectHandler, enemy slot 5 with up to three objects) is erased as soon as its offscreen
   bits say it is beside the screen, and VS. adds a check of its own after it ($B86C): within 9 pixels
   of the screen's left edge or left of it, the vine goes and its climbing metatiles with it. So one
   the screen scrolled past left the band at once. Within the band it stays (and keeps writing its
   climbing metatile to the block buffer); its sprites, drawn just before with their 8-bit X wrapped
   round to the right edge, are hidden in the real frame. */
static int vine_in_band(void) {
    int x = rel_x(ram[OBJECT_OFFSET] + 1);         /* the vine's own slot (5) */
    return x < 128 && x + 14 >= -room();            /* its sprites are 14 pixels wide */
}

static void patch_vine(void) {
    if (cpu.a == 0 || !vine_in_band()) return;
    for (int k = 0; k < ram[VINE_PIECES] && k < 3; k++) hide_wrapped(ram[ENEMY_SPR_OFS + ram[VINE_SLOTS + k]], 6);
    cpu.a = 0; cpu.z = 1; cpu.n = 0;
}

static void patch_vine_left(void) {
    if (vine_in_band()) cpu.n = 0;
}

static void patch_vine_near(void) {
    if (vine_in_band()) cpu.c = 1;
}

/* the player grabs a vine at the first column of an even page (its block buffer address' low byte 0):
   PutPlayerOnVine takes his page from the screen's right edge, which is the vine's page only while the
   vine is on the screen; beside its left edge it is the vine's own page, else he would land 256 pixels on */
static void patch_vine_page(void) {
    if (ram[ENEMY_FLAG + 5] && ram[ENEMY_ID + 5] == 0x2F && rel_x(6) < 0)   /* the vine (slot 5) beside the screen */
        cpu.a = (uint8_t)(cpu.a - ram[SCREEN_RIGHT_PAGE] + ram[OBJ_PAGE + 6]);
}

/* a block hit beside the screen: the coin knocked out of it (and the points it turns into), which
   JCoinGfxHandler draws with no offscreen test, and a brick's chunks, which DrawBrickChunks hides only
   as for a block at the screen's edges, have their 8-bit X wrapped round to the right edge; hidden in
   the real frame, the band's run draws them in place (a block bouncing hides itself, and hammers erase
   themselves off the screen) */
static void patch_objects_drawn(void) {
    for (int k = 0; k < 2; k++)
        if (ram[BLOCK_STATE + k] && rel_x(BLOCK_OBJ + k) < 0) hide_wrapped(ram[ALT_SPR_OFS + k], 4);
    for (int k = 0; k < 9; k++) {
        uint8_t st = ram[MISC_STATE + k];
        if (st && !(st & 0x80) && rel_x(MISC_OBJ + k) < 0) hide_wrapped(ram[MISC_SPR_OFS + k], 2);
    }
}

/* Firebars. ProcFirebar skips one whose centre is off the screen, and DrawFirebar_Collision hides a part
   whose 8-bit X is 89 pixels or more from the centre's (one that wrapped round the screen), so one in the
   band was drawn there by the band's run of the engine but touched nothing, and the parts of one on the
   screen that reach into the band were hidden. FirebarCollision then tests the player's sprite 1 X (last
   frame's, 8 bits) against the part's: beside the screen it wraps, and a part 256 pixels to his right hurt
   him. Within the band the firebar is processed, a part is placed by its true place (beyond the band it is
   hidden, past the right edge the game's own test hides it), its sprite and the centre's hidden in the real
   frame where they wrapped (the band draws them), and the test made by their true places. */
static int fbar_part_x(void) {   /* the part's X ($06) from the screen's left edge, by its centre's */
    int c = rel_x(ram[OBJECT_OFFSET] + 1);
    return c + (int8_t)(uint8_t)(ram[PART_X] - (uint8_t)c);
}

static void patch_fbar_skip(void) {
    int c = rel_x(cpu.x + 1);
    if (cpu.a == 0 || c >= 128 || c < -room() - 88) return;   /* parts reach 88 pixels from the centre */
    cpu.a = 0; cpu.z = 1; cpu.n = 0;
}

static void patch_fbar_centre(void) {
    if (rel_x(ram[OBJECT_OFFSET] + 1) < 0) ram[SPRITES + cpu.y] = 0xF8;
}

static void patch_fbar_wrap(void) {
    int f = fbar_part_x();
    if (rel_x(ram[OBJECT_OFFSET] + 1) >= 0 && f >= 0) return;   /* the game's own test */
    cpu.a = f >= -room() - 8 && f < 256 ? 0x00 : 0xFF;
}

static void patch_fbar_part(void) {
    if (fbar_part_x() < 0) ram[SPRITES + cpu.y] = 0xF8;
}

static void patch_fbar_edge(void) {
    if (fbar_part_x() < 0) cpu.c = 0;
}

static void patch_fbar_x(void) {
    int f = fbar_part_x(), p = rel_x(0);
    if (p >= 0 && f >= 0) return;                   /* both on the screen: the 8-bit difference is the true one */
    int s = p + (int8_t)(uint8_t)(ram[SPRITES + 7] - (uint8_t)p);   /* sprite 1 X, by his place */
    int d = s + 4 - f;
    if (d > -8 && d < 8) return;                    /* touching: the game's test says so too */
    cpu.a = 0x80; cpu.n = 1; cpu.z = 0;             /* far apart: as a difference of 128 */
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
    case P_FBALL_ERASE: patch_fball_erase(); break;
    case P_FBALL_DRAWN: patch_fball_drawn(); break;
    case P_FBALL_GATE: case P_EE_GATE1: case P_EE_GATE2: patch_object_gate(); break;
    case P_FBALL_TEST: patch_fball_test(); break;
    case P_EE_TEST: patch_enemies_test(); break;
    case P_FBALL_DONE: case P_EE_DONE: put_back_pair(); break;
    case P_VINE: patch_vine(); break;
    case P_VINE_LEFT: patch_vine_left(); break;
    case P_VINE_NEAR: patch_vine_near(); break;
    case P_VINE_PAGE: patch_vine_page(); break;
    case P_MISC_DRAWN: patch_objects_drawn(); break;
    case P_FBAR_SKIP: patch_fbar_skip(); break;
    case P_FBAR_CENTRE: patch_fbar_centre(); break;
    case P_FBAR_WRAP: patch_fbar_wrap(); break;
    case P_FBAR_PART: patch_fbar_part(); break;
    case P_FBAR_EDGE: patch_fbar_edge(); break;
    case P_FBAR_X: patch_fbar_x(); break;
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
