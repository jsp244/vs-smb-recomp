/*
 * allstars.h - the remastered graphics: VS. Super Mario Bros. drawn the way Super Mario All-Stars
 * draws Super Mario Bros., from the player's own All-Stars ROM (SNES; Super Mario Collection on
 * the Super Famicom). Nothing of either game is in this program: the All-Stars ROM is read when the
 * player chooses it, and the art, palettes and scenery come from it as the game runs.
 *
 * How: All-Stars' Super Mario Bros. is the NES game's engine rewritten for the SNES, so its own code
 * can build what it would show for VS.'s state. A small SNES (cpu816.c, a bus with DMA, the PPU's
 * memories) runs that code in a sandbox: its start-up, its per-area graphics setup (tilesets,
 * palettes, the parallax hills and clouds, the sky's gradient) and its area parser, fed with VS.'s
 * own level data, column for column as VS. draws them. What moves (objects, the status bar, the
 * blocks the player hits) is taken from VS.'s frame and drawn with All-Stars' tiles and palettes.
 * snesppu.c draws the result. allstars.c has the details.
 */
#ifndef ALLSTARS_H
#define ALLSTARS_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Loads and checks an All-Stars ROM (.sfc/.smc, with or without a copier header). Returns 0 when it
   is a version this program knows, else -1 with the reason in err. */
int  allstars_load(const char *path, char *err, size_t errlen);
void allstars_unload(void);
int  allstars_loaded(void);
const char *allstars_name(void);       /* "Super Mario All-Stars (USA)" etc. */
const char *allstars_sha1(void);
const char *allstars_md5(void);
uint32_t allstars_crc32(void);
const char *allstars_path(void);

/* option, not machine state: show the remastered picture when an All-Stars ROM is loaded */
extern bool allstars_enabled;

/* All-Stars' music instead of the arcade's (an option, not machine state; needs the ROM too). Its
   sound program runs on the SNES sound unit (spc700.c) with the songs All-Stars asks for VS.'s
   music; VS.'s sound effects stay. */
extern bool allstars_music_enabled;
int  allstars_music_available(void);   /* the ROM's sound program is loaded */
/* once after each real frame (not a run-ahead one): VS.'s music requests to the sound program, and
   that frame of its sound */
void allstars_music_frame(void);
/* adds n mono samples of that sound, at `rate` samples a second, to buf */
void allstars_music_mix(int16_t *buf, uint32_t n, uint32_t rate);

/* Connects to the running VS. Super Mario Bros. (hooks in the game's area start, area parser and
   frame engine; read-only) and forgets what was built for the previous ROM or state. Called after a
   ROM or state load and after allstars_load. */
void allstars_attach(void);
void allstars_reset(void);   /* a state was loaded or the machine reset: rebuild the area */
/* the instructions the hooks watch (for the frame engine's dispatcher in widescreen.c) */
void allstars_hook(uint16_t pc);

/* Draws [x, x + w) x [y, y + h) of the picture (x relative to the NES picture's left edge, so the
   widescreen bands are negative or beyond 255) as 0xAARRGGBB into out (stride pixels). Returns 1 when
   the frame was drawn, 0 when it cannot be (no All-Stars ROM, or a screen it does not know): the
   caller then shows the original picture. */
int  allstars_render(uint32_t *out, int stride, int x, int y, int w, int h);
void allstars_debug_dump(const char *prefix);   /* the sandbox SNES as files (for tools) */

#endif
