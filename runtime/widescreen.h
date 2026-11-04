/*
 * widescreen.h - the level beyond the edges of the picture for the widescreen
 * modes, from the columns the game keeps in its nametables.
 */
#ifndef WIDESCREEN_H
#define WIDESCREEN_H
#include <stdint.h>

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
   every screen that is not a level, the canvas is black.
   Returns 1 when the level was drawn beside the picture, 0 otherwise. */
int widescreen_render(uint8_t *out, uint8_t *emph, int x, int y, int w, int h);

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
