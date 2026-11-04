/* input.h - keyboard + every connected game controller, merged into one NES pad */
#ifndef INPUT_H
#define INPUT_H
#include <SDL.h>
#include <stdint.h>

#define NES_BUTTONS 8   /* A B Select Start Up Down Left Right = bits 0..7 */
/* the analog triggers, bindable like buttons (pulled past half way = down) */
#define INPUT_PAD_LT 100
#define INPUT_PAD_RT 101

void    input_init(void);
void    input_shutdown(void);
void    input_handle_event(const SDL_Event *e);     /* controller / joystick add and remove */
/* key[i]: SDL_Keycode; pad[i]: SDL_GameControllerButton, INPUT_PAD_LT / RT, or -1. The left stick always acts as the d-pad. */
uint8_t input_read(const int key[NES_BUTTONS], const int pad[NES_BUTTONS]);
void    input_default_pads(int pad[NES_BUTTONS]);
int     input_held(int key, int pad);               /* a hotkey: the key, or the pad button on any controller, is down */
int     input_controller_count(void);
const char *input_controller_summary(void);         /* names of the open devices, for the menu */
const char *input_pad_name(int button);             /* "A", "X", "Back", "D-pad Up", ... or "none" */
int     input_pad_from_name(const char *name);      /* inverse of input_pad_name / SDL string names */

#endif
