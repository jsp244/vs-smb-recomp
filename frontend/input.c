/*
 * input.c - opens every game controller SDL can see (XInput, HIDAPI, mapped
 * DirectInput) and merges their state, so a pad that shows up twice on Windows
 * (for example an Xbox controller over Bluetooth) still works whichever
 * instance reports input. Pads without a mapping fall back to a generic
 * joystick layout.
 */
#include "input.h"
#include <stdio.h>
#include <string.h>

#define MAX_DEVICES 8

static SDL_GameController *pads[MAX_DEVICES];
static int npads;
static SDL_Joystick *sticks[MAX_DEVICES];
static int nsticks;
static char summary[240] = "none detected";

static void rebuild_summary(void) {
    if (!npads && !nsticks) { snprintf(summary, sizeof summary, "none detected"); return; }
    summary[0] = 0;
    int shown = 0;
    for (int i = 0; i < npads && shown < 2; i++, shown++) {
        const char *n = SDL_GameControllerName(pads[i]);
        snprintf(summary + strlen(summary), sizeof summary - strlen(summary), "%s%s", shown ? ", " : "", n ? n : "controller");
    }
    for (int i = 0; i < nsticks && shown < 2; i++, shown++) {
        const char *n = SDL_JoystickName(sticks[i]);
        snprintf(summary + strlen(summary), sizeof summary - strlen(summary), "%s%s (generic)", shown ? ", " : "", n ? n : "joystick");
    }
    if (npads + nsticks > shown)
        snprintf(summary + strlen(summary), sizeof summary - strlen(summary), " +%d more", npads + nsticks - shown);
}

static int is_open(SDL_JoystickID id) {
    for (int i = 0; i < npads; i++) if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) return 1;
    for (int i = 0; i < nsticks; i++) if (SDL_JoystickInstanceID(sticks[i]) == id) return 1;
    return 0;
}

/* Steam Input (Big Picture, the Steam Deck) lists the controllers it has taken over in
   SDL_GAMECONTROLLER_IGNORE_DEVICES (VID/PID pairs) and gives the game a virtual pad for them.
   SDL then treats them as plain joysticks; opening them here as generic joysticks as well
   would read every press twice, once with the wrong layout. */
static int listed(const char *list, Uint16 vendor, Uint16 product) {
    if (!list) return 0;
    char id[16];
    snprintf(id, sizeof id, "0x%04x/0x%04x", vendor, product);
    size_t len = strlen(id);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',') p++;
        size_t n = strcspn(p, ",");
        if (n == len && SDL_strncasecmp(p, id, len) == 0) return 1;
        p += n;
    }
    return 0;
}

static int taken_over(int index) {
    Uint16 v = SDL_JoystickGetDeviceVendor(index), p = SDL_JoystickGetDeviceProduct(index);
    const char *except = SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT);
    if (except && *except) return !listed(except, v, p);
    return listed(SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES), v, p);
}

static void open_device(int index) {
    if (index < 0 || index >= SDL_NumJoysticks()) return;
    if (is_open(SDL_JoystickGetDeviceInstanceID(index))) return;
    if (SDL_IsGameController(index)) {
        if (npads >= MAX_DEVICES) return;
        SDL_GameController *c = SDL_GameControllerOpen(index);
        if (c) {
            pads[npads++] = c;
            char *map = SDL_GameControllerMapping(c);
            printf("controller: %s\n  mapping: %s\n", SDL_GameControllerName(c), map ? map : "(none)");
            if (map) SDL_free(map);
        }
    } else {
        if (taken_over(index)) {
            printf("joystick left to Steam Input: %s\n", SDL_JoystickNameForIndex(index));
            return;
        }
        if (nsticks >= MAX_DEVICES) return;
        SDL_Joystick *j = SDL_JoystickOpen(index);
        if (j) { sticks[nsticks++] = j; printf("joystick (generic layout): %s\n", SDL_JoystickName(j)); }
    }
    rebuild_summary();
}

static void close_instance(SDL_JoystickID id) {
    for (int i = 0; i < npads; i++) {
        if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) {
            SDL_GameControllerClose(pads[i]);
            pads[i] = pads[--npads];
            break;
        }
    }
    for (int i = 0; i < nsticks; i++) {
        if (SDL_JoystickInstanceID(sticks[i]) == id) {
            SDL_JoystickClose(sticks[i]);
            sticks[i] = sticks[--nsticks];
            break;
        }
    }
    rebuild_summary();
}

void input_init(void) {
    for (int i = 0; i < SDL_NumJoysticks(); i++) open_device(i);
    rebuild_summary();
}

void input_shutdown(void) {
    for (int i = 0; i < npads; i++) SDL_GameControllerClose(pads[i]);
    for (int i = 0; i < nsticks; i++) SDL_JoystickClose(sticks[i]);
    npads = nsticks = 0;
    rebuild_summary();
}

void input_handle_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_CONTROLLERDEVICEADDED: open_device(e->cdevice.which); break;       /* which = device index */
    case SDL_JOYDEVICEADDED: open_device(e->jdevice.which); break;
    case SDL_CONTROLLERDEVICEREMOVED: close_instance(e->cdevice.which); break;  /* which = instance id */
    case SDL_JOYDEVICEREMOVED: close_instance(e->jdevice.which); break;
    default: break;
    }
}

#define TRIGGER_DOWN 16000   /* of 32767 */

static int pad_button_down(SDL_GameController *c, int b) {
    if (b == INPUT_PAD_LT || b == INPUT_PAD_RT) {
        SDL_GameControllerAxis ax = b == INPUT_PAD_LT ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
#ifdef __ANDROID__
        /* L2 / R2 that are plain buttons: SDL's Android driver reports them as joystick buttons
           15 / 16, which no controller mapping uses */
        if (!SDL_GameControllerHasAxis(c, ax))
            return SDL_JoystickGetButton(SDL_GameControllerGetJoystick(c), b == INPUT_PAD_LT ? 15 : 16);
#endif
        return SDL_GameControllerGetAxis(c, ax) > TRIGGER_DOWN;
    }
    return b >= 0 && b < SDL_CONTROLLER_BUTTON_MAX && SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b);
}

uint8_t input_read(const int key[NES_BUTTONS], const int pad[NES_BUTTONS]) {
    const int dead = 10000;
    uint8_t b = 0;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    for (int i = 0; i < NES_BUTTONS; i++) {
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)key[i]);
        if (sc != SDL_SCANCODE_UNKNOWN && ks[sc]) b |= (uint8_t)(1 << i);
    }
    for (int p = 0; p < npads; p++) {
        SDL_GameController *c = pads[p];
        for (int i = 0; i < NES_BUTTONS; i++)
            if (pad_button_down(c, pad[i])) b |= (uint8_t)(1 << i);
        int ax = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        int ay = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        if (ax < -dead) b |= 0x40; else if (ax > dead) b |= 0x80;
        if (ay < -dead) b |= 0x10; else if (ay > dead) b |= 0x20;
    }
    for (int s = 0; s < nsticks; s++) {
        /* generic layout: buttons 0/1/2 = A/B/B, 6 = select, 7 = start, hat 0 = d-pad, axes 0/1 = stick */
        SDL_Joystick *j = sticks[s];
        int nb = SDL_JoystickNumButtons(j);
        if (nb > 0 && SDL_JoystickGetButton(j, 0)) b |= 0x01;
        if (nb > 1 && SDL_JoystickGetButton(j, 1)) b |= 0x02;
        if (nb > 2 && SDL_JoystickGetButton(j, 2)) b |= 0x02;
        if (nb > 6 && SDL_JoystickGetButton(j, 6)) b |= 0x04;
        if (nb > 7 && SDL_JoystickGetButton(j, 7)) b |= 0x08;
        if (SDL_JoystickNumHats(j) > 0) {
            Uint8 h = SDL_JoystickGetHat(j, 0);
            if (h & SDL_HAT_UP) b |= 0x10;
            if (h & SDL_HAT_DOWN) b |= 0x20;
            if (h & SDL_HAT_LEFT) b |= 0x40;
            if (h & SDL_HAT_RIGHT) b |= 0x80;
        }
        if (SDL_JoystickNumAxes(j) > 1) {
            int ax = SDL_JoystickGetAxis(j, 0), ay = SDL_JoystickGetAxis(j, 1);
            if (ax < -dead) b |= 0x40; else if (ax > dead) b |= 0x80;
            if (ay < -dead) b |= 0x10; else if (ay > dead) b |= 0x20;
        }
    }
    return b;
}

int input_held(int key, int pad) {
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)key);
    if (sc != SDL_SCANCODE_UNKNOWN && ks[sc]) return 1;
    for (int p = 0; p < npads; p++) if (pad_button_down(pads[p], pad)) return 1;
    return 0;
}

/* NES A (jump) on A and NES B (attack) on X, the button left of it: on an Xbox (XInput) pad the
   A label matches, and B sits to the left of A as on the NES controller */
void input_default_pads(int pad[NES_BUTTONS]) {
    pad[0] = SDL_CONTROLLER_BUTTON_A;
    pad[1] = SDL_CONTROLLER_BUTTON_X;
    pad[2] = SDL_CONTROLLER_BUTTON_BACK;
    pad[3] = SDL_CONTROLLER_BUTTON_START;
    pad[4] = SDL_CONTROLLER_BUTTON_DPAD_UP;
    pad[5] = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    pad[6] = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    pad[7] = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
}

int input_controller_count(void) { return npads + nsticks; }
const char *input_controller_summary(void) { return summary; }

static const struct { int button; const char *name; } pad_names[] = {
    { SDL_CONTROLLER_BUTTON_A, "A" }, { SDL_CONTROLLER_BUTTON_B, "B" },
    { SDL_CONTROLLER_BUTTON_X, "X" }, { SDL_CONTROLLER_BUTTON_Y, "Y" },
    { SDL_CONTROLLER_BUTTON_BACK, "Back" }, { SDL_CONTROLLER_BUTTON_GUIDE, "Guide" },
    { SDL_CONTROLLER_BUTTON_START, "Start" },
    { SDL_CONTROLLER_BUTTON_LEFTSTICK, "LS" }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, "RS" },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "LB" }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "RB" },
    { INPUT_PAD_LT, "LT" }, { INPUT_PAD_RT, "RT" },
    { SDL_CONTROLLER_BUTTON_DPAD_UP, "D-pad Up" }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, "D-pad Down" },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, "D-pad Left" }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "D-pad Right" },
    { SDL_CONTROLLER_BUTTON_MISC1, "Misc" },
    { SDL_CONTROLLER_BUTTON_PADDLE1, "Paddle 1" }, { SDL_CONTROLLER_BUTTON_PADDLE2, "Paddle 2" },
    { SDL_CONTROLLER_BUTTON_PADDLE3, "Paddle 3" }, { SDL_CONTROLLER_BUTTON_PADDLE4, "Paddle 4" },
    { SDL_CONTROLLER_BUTTON_TOUCHPAD, "Touchpad" },
};

const char *input_pad_name(int button) {
    for (size_t i = 0; i < sizeof pad_names / sizeof pad_names[0]; i++)
        if (pad_names[i].button == button) return pad_names[i].name;
    return "none";
}

int input_pad_from_name(const char *name) {
    for (size_t i = 0; i < sizeof pad_names / sizeof pad_names[0]; i++)
        if (SDL_strcasecmp(pad_names[i].name, name) == 0) return pad_names[i].button;
    int b = SDL_GameControllerGetButtonFromString(name);   /* SDL's own names, e.g. "dpup" */
    return b == SDL_CONTROLLER_BUTTON_INVALID ? -1 : b;
}
