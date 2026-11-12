/* touchpad.h - the phone edition's on-screen NES controller, played with the fingers */
#ifndef TOUCHPAD_H
#define TOUCHPAD_H
#include <SDL.h>
#include <stdint.h>

void    touchpad_enable(int on);
int     touchpad_enabled(void);
/* what an event did (touchpad_handle_event) */
enum { TOUCHPAD_NONE = 0, TOUCHPAD_MENU = 1, TOUCHPAD_TOGGLED = 2 };
/* every event, with the output size in pixels; in_game = 0 in the menu (fingers are the menu's
   there). Returns TOUCHPAD_MENU when MENU was tapped, TOUCHPAD_TOGGLED when the small button at
   the top right switched the controls off or on. */
int     touchpad_handle_event(const SDL_Event *e, int w, int h, int in_game);
uint8_t touchpad_buttons(void);         /* NES buttons held on the screen, bits as input_read */
/* the controls switched on or off (a setting: off, only the small button that brings them back
   shows, for playing with a controller) */
void    touchpad_set_controls(int on);
int     touchpad_controls(void);
int     touchpad_visible(void);         /* the controls show: enabled, switched on, no controller in use since the last touch */
void    touchpad_release_all(void);     /* the menu opened: every finger lets go */
void    touchpad_draw(SDL_Renderer *r, int w, int h);
int     touchpad_selftest(void);        /* the finger logic on synthetic events; returns the failures */

#endif
