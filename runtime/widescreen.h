/*
 * widescreen.h - the level beyond the edges of the picture for the widescreen
 * modes, from the columns the game keeps in its nametables.
 */
#ifndef WIDESCREEN_H
#define WIDESCREEN_H
#include <stdint.h>
#include <stdbool.h>

/* The widescreen canvas: the PPU's 256x240 picture with up to WS_BAND pixels of
   level on each side (the level is no taller than the screen, so no lines are
   added above or below: WS_VBAND is 0). */
#define WS_BAND  128
#define WS_VBAND 0
#define WS_W     (256 + 2 * WS_BAND)
#define WS_H     (240 + 2 * WS_VBAND)

/* Renders the part [x, x + w) x [y, y + h) of the canvas (the picture's top left
   corner is at WS_BAND, WS_VBAND) into out (w x h 6-bit palette indices, the
   format of ppu.fb) and emph (h bytes: the $2001 value each row is shown with).
   During play the columns beside the picture are the level as the game has
   rendered it into its two nametables, ahead of the screen and behind it, drawn
   with each line's own scroll (the status bar keeps its place); beyond what the
   game holds (before the start of the area, past its rendering frontier) and on
   every screen that is not a level, the canvas is black. With widescreen_sprites
   the objects the game keeps beside the screen are drawn there too (and in the
   sprite columns it hides at the screen's edges), by running the frame's engine
   code again in the CPU sandbox with the camera moved.
   Returns 1 when the level was drawn beside the picture, 0 otherwise. */
int widescreen_render(uint8_t *out, uint8_t *emph, int x, int y, int w, int h);

/* Objects beside the picture, and the page before an area's start page when the area was entered
   part-way (after a death at its halfway point, from a pipe); both on by default. Not machine state. */
extern bool widescreen_sprites, widescreen_fill;

/* Starts keeping the copy of RAM the objects are drawn from (a hook at the engine's first instruction)
   when the loaded ROM is VS. Super Mario Bros.; widescreen_render calls it itself, calling it after a
   ROM load makes the objects appear from the first frame. */
void widescreen_attach(void);

/* The RAM the engine built the sprites the PPU shows from (a copy made at GameEngine's start of the
   previous frame), NULL when there is none (not in a level). Right after a state load there is only
   this frame's copy, a frame ahead: *ahead is then 1. */
const uint8_t *widescreen_shown_ram(int *ahead);
/* The picture's columns [lo, hi) that show the level this frame (beyond 0..256 in the bands); 0 when
   the frame is not a level (a text screen, the intermission). */
int widescreen_level_window(int *lo, int *hi);

/* The sprites the game builds beside the picture, for other renderers (the remastered graphics): the
   64 OAM entries of the frame's engine run with the camera moved to the left (side 0) or right (side 1),
   and how far right of their OAM X they land on the picture (*dx). Entries equal to the PPU's own are
   the picture's objects, not new ones. Returns 0 when there is no such run (not in a level, objects
   beside the picture off). */
int widescreen_band_oam(int side, uint8_t oam[256], int *dx);

/* Self-check of the objects: runs the engine run whose sprites the PPU shows again, with the camera
   where it was, and compares the sprites it builds with the PPU's. Returns 1 when identical, 0 when
   they differ, -1 when there is no run to repeat (not in a level, or right after a state load). */
int widescreen_sprite_check(void);

/* Self-check: re-renders the centre 256x240 of the current frame from the
   nametables the way the bands are drawn and compares it with the PPU's frame
   (sprites are the only legitimate difference). Returns 0 when the frame is a
   level and was compared, -1 otherwise. */
int widescreen_check(int *bad_pixels, int *total_pixels);

/* Width of the picture to show for a display of the given height (in NES lines)
   and pixel aspect ratio (1.0 = square pixels): the part of the WS_W-wide canvas
   that makes a picture of the given ratio (16/9, 16/10), at most WS_W and at
   least the normal 256. */
int widescreen_visible_width(int lines, double pixel_aspect, double ratio);

#endif
