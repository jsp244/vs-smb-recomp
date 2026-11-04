/* font.h - tiny 5x7 bitmap font for the launcher UI (original data, no game assets) */
#ifndef FONT_H
#define FONT_H
#include <SDL.h>

#define FONT_CELL_W 6   /* 5 pixel glyph + 1 pixel gap */
#define FONT_CELL_H 8

void font_init(SDL_Renderer *r);
void font_quit(void);
int  font_width(const char *s, int scale);
/* draws s at (x, y); shadow = 1 adds a dark offset copy underneath */
void font_draw(SDL_Renderer *r, int x, int y, int scale, SDL_Color c, int shadow, const char *s);

#endif
