/*
 * touchpad.c - the on-screen NES controller of the phone edition.
 *
 * Translucent controls drawn over the picture and played with the fingers: the d-pad at the
 * bottom left, B and A at the bottom right, COIN (the cabinet's coin slot, on the pad's Select
 * line) and START at the bottom centre and MENU at the top. Every finger is followed from the moment it lands. One that lands on the d-pad
 * steers it until it is lifted, by its angle from the centre (eight ways, the diagonals
 * narrower than the four directions). One that lands on B or A presses whichever it is over as
 * it slides, and both between them. The buttons reach the game in the same pad byte as a
 * controller's, read at the start of each frame, so touch adds no delay of its own. The
 * controls hide while a controller is in use and come back with the next touch.
 *
 * A small faint button at the top right (beside MENU's row, kept clear of a corner camera)
 * switches the controls off for good, for playing with a controller or a phone grip: then only
 * it shows, crossed out, and a tap brings them back. A controller in use hides it as well; the
 * touch that shows it again does not also press it.
 */
#include "touchpad.h"
#include "font.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { CTL_NONE, CTL_DPAD, CTL_AB, CTL_SELECT, CTL_START, CTL_MENU, CTL_TOGGLE };
enum { NES_A = 0x01, NES_B = 0x02, NES_SELECT = 0x04, NES_START = 0x08,
       NES_UP = 0x10, NES_DOWN = 0x20, NES_LEFT = 0x40, NES_RIGHT = 0x80 };
#define MAX_FINGERS 10
#define DPAD_HIT 1.35f      /* the area a finger may land in, times the drawn radius */
#define AB_HIT   1.4f
#define TOGGLE_HIT 1.5f     /* the corner button's: a little beyond the drawn disc, nothing more */
#define PI_F     3.14159265f

typedef struct { float x, y, r; } circle_t;
typedef struct { float x, y, w, h; } box_t;     /* centre and size */
typedef struct { SDL_FingerID id; int ctl, used; float x, y; } finger_t;

static int enabled, hidden, controls_on = 1;
static finger_t fingers[MAX_FINGERS];
static struct { int w, h; float u; circle_t dpad, a, b, toggle; box_t select, start, menu; } lay;

/* the controls on a w x h output; u is a hundredth of the shorter side */
static void layout(int w, int h) {
    if (w == lay.w && h == lay.h) return;
    float u = (w < h ? w : h) / 100.0f;
    lay.w = w; lay.h = h; lay.u = u;
    lay.dpad = (circle_t){ 21 * u, h - 27 * u, 16 * u };
    lay.b = (circle_t){ w - 31 * u, h - 22 * u, 8 * u };
    lay.a = (circle_t){ w - 13 * u, h - 32 * u, 8 * u };
    lay.select = (box_t){ w / 2.0f - 11 * u, h - 7 * u, 16 * u, 6 * u };
    lay.start = (box_t){ w / 2.0f + 11 * u, h - 7 * u, 16 * u, 6 * u };
    lay.menu = (box_t){ w / 2.0f, 7 * u, 16 * u, 6 * u };
    /* at the top right, in MENU's row; 14 from the edge, so a camera hole in the corner (drawn beside
       on a short edge) does not cover it */
    lay.toggle = (circle_t){ w - 14 * u, 7 * u, 3 * u };
}

static int in_circle(const circle_t *c, float x, float y, float scale) {
    float dx = x - c->x, dy = y - c->y, r = c->r * scale;
    return dx * dx + dy * dy <= r * r;
}

static int in_box(const box_t *b, float x, float y) {
    float pad = 2 * lay.u;
    return fabsf(x - b->x) <= b->w / 2 + pad && fabsf(y - b->y) <= b->h / 2 + pad;
}

static int control_at(float x, float y) {
    if (in_circle(&lay.toggle, x, y, TOGGLE_HIT)) return CTL_TOGGLE;
    if (!controls_on) return CTL_NONE;
    if (in_box(&lay.menu, x, y)) return CTL_MENU;
    if (in_box(&lay.select, x, y)) return CTL_SELECT;
    if (in_box(&lay.start, x, y)) return CTL_START;
    if (in_circle(&lay.a, x, y, AB_HIT) || in_circle(&lay.b, x, y, AB_HIT)) return CTL_AB;
    if (in_circle(&lay.dpad, x, y, DPAD_HIT)) return CTL_DPAD;
    return CTL_NONE;
}

/* the d-pad from a finger's offset to its centre: nothing in the middle fifth, the four directions
   60 degrees wide and the diagonals 30 */
static uint8_t dpad_bits(float dx, float dy, float r) {
    if (dx * dx + dy * dy < 0.04f * r * r) return 0;
    float a = atan2f(dy, dx) * (180.0f / PI_F);   /* 0 = right, 90 = down */
    if (a < 0) a += 360;
    uint8_t b = 0;
    if (a < 60 || a >= 300) b |= NES_RIGHT;
    if (a >= 30 && a < 150) b |= NES_DOWN;
    if (a >= 120 && a < 240) b |= NES_LEFT;
    if (a >= 210 && a < 330) b |= NES_UP;
    return b;
}

static uint8_t finger_bits(const finger_t *f) {
    switch (f->ctl) {
    case CTL_DPAD: return dpad_bits(f->x - lay.dpad.x, f->y - lay.dpad.y, lay.dpad.r);
    case CTL_AB: return (uint8_t)((in_circle(&lay.a, f->x, f->y, AB_HIT) ? NES_A : 0) | (in_circle(&lay.b, f->x, f->y, AB_HIT) ? NES_B : 0));
    case CTL_SELECT: return in_box(&lay.select, f->x, f->y) ? NES_SELECT : 0;
    case CTL_START: return in_box(&lay.start, f->x, f->y) ? NES_START : 0;
    default: return 0;
    }
}

static finger_t *find(SDL_FingerID id) {
    for (int i = 0; i < MAX_FINGERS; i++) if (fingers[i].used && fingers[i].id == id) return &fingers[i];
    return NULL;
}

void touchpad_enable(int on) { enabled = on; hidden = 0; touchpad_release_all(); }
int touchpad_enabled(void) { return enabled; }
void touchpad_set_controls(int on) { controls_on = on != 0; touchpad_release_all(); }
int touchpad_controls(void) { return controls_on; }
int touchpad_visible(void) { return enabled && !hidden && controls_on; }
void touchpad_release_all(void) { memset(fingers, 0, sizeof fingers); }

uint8_t touchpad_buttons(void) {
    uint8_t b = 0;
    if (!enabled || hidden || !controls_on) return 0;
    for (int i = 0; i < MAX_FINGERS; i++) if (fingers[i].used) b |= finger_bits(&fingers[i]);
    return b;
}

int touchpad_handle_event(const SDL_Event *e, int w, int h, int in_game) {
    if (!enabled) return TOUCHPAD_NONE;
    switch (e->type) {
    case SDL_CONTROLLERBUTTONDOWN: case SDL_JOYBUTTONDOWN: case SDL_JOYHATMOTION:
        hidden = 1;   /* a controller in use: the controls and the corner button step aside */
        touchpad_release_all();
        return TOUCHPAD_NONE;
    case SDL_FINGERDOWN: case SDL_FINGERMOTION: case SDL_FINGERUP: break;
    default: return TOUCHPAD_NONE;
    }
    SDL_TouchDeviceType type = SDL_GetTouchDeviceType(e->tfinger.touchId);
    if (type == SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE || type == SDL_TOUCH_DEVICE_INDIRECT_RELATIVE) return TOUCHPAD_NONE;   /* a trackpad */
    layout(w, h);
    float x = e->tfinger.x * w, y = e->tfinger.y * h;
    finger_t *f = find(e->tfinger.fingerId);
    if (e->type == SDL_FINGERDOWN) {
        int was_hidden = hidden;
        hidden = 0;
        if (!in_game) return TOUCHPAD_NONE;
        int ctl = control_at(x, y);
        if (ctl == CTL_NONE) return TOUCHPAD_NONE;
        if (ctl == CTL_TOGGLE && was_hidden) return TOUCHPAD_NONE;   /* it was not showing: this touch only shows it */
        if (!f) for (int i = 0; i < MAX_FINGERS && !f; i++) if (!fingers[i].used) f = &fingers[i];
        if (!f) return TOUCHPAD_NONE;
        *f = (finger_t){ e->tfinger.fingerId, ctl, 1, x, y };
        return TOUCHPAD_NONE;
    }
    if (!f) return TOUCHPAD_NONE;   /* a finger that landed elsewhere, or in the menu */
    f->x = x; f->y = y;
    if (e->type == SDL_FINGERUP) {
        int ctl = f->ctl;
        f->used = 0;
        if (ctl == CTL_MENU && in_box(&lay.menu, x, y)) return TOUCHPAD_MENU;
        if (ctl == CTL_TOGGLE && in_circle(&lay.toggle, x, y, TOGGLE_HIT)) {
            touchpad_set_controls(!controls_on);   /* and every other finger lets go */
            return TOUCHPAD_TOGGLED;
        }
    }
    return TOUCHPAD_NONE;
}

/* ---- drawing ---- */
static SDL_Vertex vtx(float x, float y, SDL_Color c) {
    SDL_Vertex v;
    v.position.x = x; v.position.y = y; v.color = c; v.tex_coord.x = v.tex_coord.y = 0;
    return v;
}

/* a convex outline filled as a fan from its centre (at most 64 points) */
static void fill_fan(SDL_Renderer *r, float cx, float cy, const float *px, const float *py, int n, SDL_Color c) {
    SDL_Vertex v[65];
    int idx[64 * 3];
    v[0] = vtx(cx, cy, c);
    for (int i = 0; i < n; i++) {
        v[i + 1] = vtx(px[i], py[i], c);
        idx[i * 3] = 0; idx[i * 3 + 1] = i + 1; idx[i * 3 + 2] = (i + 1) % n + 1;
    }
    SDL_RenderGeometry(r, NULL, v, n + 1, idx, n * 3);
}

static void fill_circle(SDL_Renderer *r, float cx, float cy, float rad, SDL_Color c) {
    float px[48], py[48];
    for (int i = 0; i < 48; i++) {
        float a = i * (2 * PI_F / 48);
        px[i] = cx + rad * cosf(a); py[i] = cy + rad * sinf(a);
    }
    fill_fan(r, cx, cy, px, py, 48, c);
}

static void fill_round_box(SDL_Renderer *r, float cx, float cy, float w, float h, float rad, SDL_Color c) {
    static const float sx[4] = { 1, -1, -1, 1 }, sy[4] = { 1, 1, -1, -1 };
    float px[40], py[40], hx = w / 2 - rad, hy = h / 2 - rad;
    int n = 0;
    for (int k = 0; k < 4; k++)        /* the corners clockwise from the bottom right, 9 points each */
        for (int i = 0; i <= 8; i++) {
            float a = (k * 90 + i * 11.25f) * (PI_F / 180);
            px[n] = cx + sx[k] * hx + rad * cosf(a); py[n] = cy + sy[k] * hy + rad * sinf(a); n++;
        }
    fill_fan(r, cx, cy, px, py, n, c);
}

/* the d-pad's cross, arms t wide and reaching e from the centre */
static void fill_cross(SDL_Renderer *r, float cx, float cy, float e, float t, SDL_Color c) {
    float h = t / 2;
    const float px[12] = { cx + h, cx + e, cx + e, cx + h, cx + h, cx - h, cx - h, cx - e, cx - e, cx - h, cx - h, cx + h };
    const float py[12] = { cy - h, cy - h, cy + h, cy + h, cy + e, cy + e, cy + h, cy + h, cy - h, cy - h, cy - e, cy - e };
    fill_fan(r, cx, cy, px, py, 12, c);
}

/* an arrow in an arm of the d-pad, pointing away from the centre (dx, dy = the direction) */
static void fill_arrow(SDL_Renderer *r, float cx, float cy, float dx, float dy, float size, SDL_Color c) {
    float tx = cx + dx * size, ty = cy + dy * size;     /* tip */
    float bx = cx - dx * size * 0.2f, by = cy - dy * size * 0.2f;
    float wx = -dy * size * 0.75f, wy = dx * size * 0.75f;
    SDL_Vertex v[3] = { vtx(tx, ty, c), vtx(bx + wx, by + wy, c), vtx(bx - wx, by - wy, c) };
    SDL_RenderGeometry(r, NULL, v, 3, NULL, 0);
}

static void label(SDL_Renderer *r, float cx, float cy, float height, const char *s, Uint8 alpha) {
    int scale = (int)(height / 7);
    if (scale < 1) scale = 1;
    SDL_Color c = { 255, 255, 255, alpha };
    font_draw(r, (int)(cx - font_width(s, scale) / 2.0f + 0.5f), (int)(cy - 3.5f * scale + 0.5f), scale, c, 1, s);
}

static void draw_box(SDL_Renderer *r, const box_t *b, const char *s, int pressed) {
    const SDL_Color edge = { 0, 0, 0, 70 }, fill = { 255, 255, 255, (Uint8)(pressed ? 120 : 50) };
    float rad = b->h / 2, o = lay.u * 0.5f;
    fill_round_box(r, b->x, b->y, b->w + 2 * o, b->h + 2 * o, rad + o, edge);
    fill_round_box(r, b->x, b->y, b->w, b->h, rad, fill);
    label(r, b->x, b->y, b->h * 0.5f, s, pressed ? 230 : 160);
}

static void draw_button(SDL_Renderer *r, const circle_t *c, const char *s, int pressed) {
    const SDL_Color edge = { 0, 0, 0, 70 }, fill = { 255, 255, 255, (Uint8)(pressed ? 120 : 50) };
    fill_circle(r, c->x, c->y, c->r * 1.07f, edge);
    fill_circle(r, c->x, c->y, c->r, fill);
    label(r, c->x, c->y, c->r * 0.75f, s, pressed ? 230 : 160);
}

/* a bar from (x0, y0) to (x1, y1), t thick */
static void fill_bar(SDL_Renderer *r, float x0, float y0, float x1, float y1, float t, SDL_Color c) {
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len <= 0) return;
    float nx = -dy / len * t / 2, ny = dx / len * t / 2;
    SDL_Vertex v[4] = { vtx(x0 + nx, y0 + ny, c), vtx(x1 + nx, y1 + ny, c), vtx(x1 - nx, y1 - ny, c), vtx(x0 - nx, y0 - ny, c) };
    const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(r, NULL, v, 4, idx, 6);
}

/* the corner button: a small faint disc with a controller in it, crossed out while the controls are off */
static void draw_toggle(SDL_Renderer *r, int pressed) {
    const circle_t *t = &lay.toggle;
    float R = t->r;
    Uint8 ink = (Uint8)(pressed ? 220 : controls_on ? 120 : 150);
    const SDL_Color edge = { 0, 0, 0, 55 }, fill = { 255, 255, 255, (Uint8)(pressed ? 90 : 30) };
    const SDL_Color body = { 255, 255, 255, ink }, hole = { 0, 0, 0, (Uint8)(ink * 2 / 3) };
    fill_circle(r, t->x, t->y, R * 1.08f, edge);
    fill_circle(r, t->x, t->y, R, fill);
    fill_round_box(r, t->x, t->y + R * 0.04f, R * 1.3f, R * 0.74f, R * 0.3f, body);   /* the controller */
    fill_cross(r, t->x - R * 0.33f, t->y + R * 0.04f, R * 0.21f, R * 0.09f, hole);   /* its d-pad */
    fill_circle(r, t->x + R * 0.25f, t->y + R * 0.12f, R * 0.08f, hole);             /* and buttons */
    fill_circle(r, t->x + R * 0.41f, t->y - R * 0.04f, R * 0.08f, hole);
    if (!controls_on) {
        float k = R * 0.62f;
        fill_bar(r, t->x - k, t->y - k, t->x + k, t->y + k, R * 0.26f, edge);
        fill_bar(r, t->x - k, t->y - k, t->x + k, t->y + k, R * 0.13f, body);
    }
}

void touchpad_draw(SDL_Renderer *r, int w, int h) {
    if (!enabled || hidden) return;
    layout(w, h);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    int toggle_down = 0;
    for (int i = 0; i < MAX_FINGERS; i++) if (fingers[i].used && fingers[i].ctl == CTL_TOGGLE) toggle_down = 1;
    draw_toggle(r, toggle_down);
    if (!controls_on) return;
    uint8_t held = touchpad_buttons();

    const circle_t *d = &lay.dpad;
    const SDL_Color edge = { 0, 0, 0, 70 }, fill = { 255, 255, 255, 50 };
    float arm = d->r * 0.66f;
    fill_cross(r, d->x, d->y, d->r * 1.06f, arm + d->r * 0.12f, edge);
    fill_cross(r, d->x, d->y, d->r, arm, fill);
    static const struct { uint8_t bit; float dx, dy; } dirs[4] = {
        { NES_UP, 0, -1 }, { NES_DOWN, 0, 1 }, { NES_LEFT, -1, 0 }, { NES_RIGHT, 1, 0 } };
    for (int i = 0; i < 4; i++) {
        int on = (held & dirs[i].bit) != 0;
        SDL_Color c = { 255, 255, 255, (Uint8)(on ? 230 : 110) };
        if (on) {   /* the pressed arm lights up */
            const SDL_Color lit = { 255, 255, 255, 80 };
            float mx = d->x + dirs[i].dx * d->r * 0.55f, my = d->y + dirs[i].dy * d->r * 0.55f;
            fill_round_box(r, mx, my, dirs[i].dx ? d->r * 0.9f : arm, dirs[i].dy ? d->r * 0.9f : arm, arm * 0.1f, lit);
        }
        fill_arrow(r, d->x + dirs[i].dx * d->r * 0.62f, d->y + dirs[i].dy * d->r * 0.62f, dirs[i].dx, dirs[i].dy, arm * 0.3f, c);
    }

    draw_button(r, &lay.b, "B", (held & NES_B) != 0);
    draw_button(r, &lay.a, "A", (held & NES_A) != 0);
    draw_box(r, &lay.select, "COIN", (held & NES_SELECT) != 0);
    draw_box(r, &lay.start, "START", (held & NES_START) != 0);
    int menu_down = 0;
    for (int i = 0; i < MAX_FINGERS; i++) if (fingers[i].used && fingers[i].ctl == CTL_MENU) menu_down = 1;
    draw_box(r, &lay.menu, "MENU", menu_down);
}

/* ---- self-test ---- */
static int send(int type, int id, float x, float y, int in_game) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = (Uint32)type;
    e.tfinger.touchId = 77;   /* no such device: treated as a touchscreen */
    e.tfinger.fingerId = id;
    e.tfinger.x = x / lay.w; e.tfinger.y = y / lay.h;
    return touchpad_handle_event(&e, lay.w, lay.h, in_game);
}

int touchpad_selftest(void) {
    int failures = 0, saved = enabled, saved_controls = controls_on;
#define TCHECK(cond, what) do { if (!(cond)) { failures++; printf("touch-test: FAILED %s\n", what); } } while (0)
    touchpad_enable(1);
    touchpad_set_controls(1);
    layout(2400, 1080);
    const circle_t d = lay.dpad, a = lay.a, b = lay.b;
    float r = d.r;

    send(SDL_FINGERDOWN, 1, d.x + 0.8f * r, d.y, 1);
    TCHECK(touchpad_buttons() == NES_RIGHT, "d-pad right");
    send(SDL_FINGERMOTION, 1, d.x, d.y + 0.8f * r, 1);
    TCHECK(touchpad_buttons() == NES_DOWN, "slide to down");
    send(SDL_FINGERMOTION, 1, d.x + 0.6f * r, d.y + 0.6f * r, 1);
    TCHECK(touchpad_buttons() == (NES_DOWN | NES_RIGHT), "diagonal down-right");
    send(SDL_FINGERMOTION, 1, d.x - 0.8f * r, d.y - 0.25f * r, 1);
    TCHECK(touchpad_buttons() == NES_LEFT, "near left is left (narrow diagonals)");
    send(SDL_FINGERMOTION, 1, d.x + 0.05f * r, d.y, 1);
    TCHECK(touchpad_buttons() == 0, "the centre is no direction");
    send(SDL_FINGERMOTION, 1, d.x, d.y - 2.5f * r, 1);
    TCHECK(touchpad_buttons() == NES_UP, "the d-pad keeps its finger beyond its edge");
    send(SDL_FINGERUP, 1, d.x, d.y - 2.5f * r, 1);
    TCHECK(touchpad_buttons() == 0, "lifted");

    send(SDL_FINGERDOWN, 2, a.x, a.y, 1);
    TCHECK(touchpad_buttons() == NES_A, "A");
    send(SDL_FINGERMOTION, 2, (a.x + b.x) / 2, (a.y + b.y) / 2, 1);
    TCHECK(touchpad_buttons() == (NES_A | NES_B), "between A and B: both");
    send(SDL_FINGERMOTION, 2, b.x, b.y, 1);
    TCHECK(touchpad_buttons() == NES_B, "slid onto B");
    send(SDL_FINGERDOWN, 3, d.x, d.y - 0.8f * r, 1);
    TCHECK(touchpad_buttons() == (NES_B | NES_UP), "two fingers: B and up");
    send(SDL_FINGERUP, 2, b.x, b.y, 1);
    send(SDL_FINGERUP, 3, d.x, d.y, 1);
    TCHECK(touchpad_buttons() == 0, "both lifted");

    send(SDL_FINGERDOWN, 4, lay.start.x, lay.start.y, 1);
    TCHECK(touchpad_buttons() == NES_START, "Start");
    send(SDL_FINGERMOTION, 4, lay.start.x, lay.start.y - 20 * lay.u, 1);
    TCHECK(touchpad_buttons() == 0, "slid off Start");
    send(SDL_FINGERUP, 4, lay.start.x, lay.start.y - 20 * lay.u, 1);
    send(SDL_FINGERDOWN, 5, lay.select.x, lay.select.y, 1);
    send(SDL_FINGERDOWN, 6, lay.start.x, lay.start.y, 1);
    TCHECK(touchpad_buttons() == (NES_SELECT | NES_START), "Select + Start together");
    send(SDL_FINGERUP, 5, lay.select.x, lay.select.y, 1);
    send(SDL_FINGERUP, 6, lay.start.x, lay.start.y, 1);

    send(SDL_FINGERDOWN, 7, lay.menu.x, lay.menu.y, 1);
    TCHECK(send(SDL_FINGERUP, 7, lay.menu.x, lay.menu.y, 1) == 1, "MENU tap");
    send(SDL_FINGERDOWN, 8, lay.menu.x, lay.menu.y, 1);
    TCHECK(send(SDL_FINGERUP, 8, lay.w / 2.0f, lay.h / 2.0f, 1) == 0, "MENU let go elsewhere is no tap");
    send(SDL_FINGERDOWN, 9, lay.w / 2.0f, lay.h / 2.0f, 1);
    TCHECK(touchpad_buttons() == 0, "the picture itself is no control");
    send(SDL_FINGERUP, 9, lay.w / 2.0f, lay.h / 2.0f, 1);
    send(SDL_FINGERDOWN, 10, a.x, a.y, 0);
    TCHECK(touchpad_buttons() == 0, "fingers in the menu are the menu's");
    send(SDL_FINGERUP, 10, a.x, a.y, 1);
    TCHECK(touchpad_buttons() == 0, "and stay so after it closes");

    send(SDL_FINGERDOWN, 11, a.x, a.y, 1);
    SDL_Event pad;
    memset(&pad, 0, sizeof pad);
    pad.type = SDL_CONTROLLERBUTTONDOWN;
    touchpad_handle_event(&pad, lay.w, lay.h, 1);
    TCHECK(!touchpad_visible() && touchpad_buttons() == 0, "a controller press hides the controls");
    send(SDL_FINGERUP, 11, a.x, a.y, 1);
    send(SDL_FINGERDOWN, 12, b.x, b.y, 1);
    TCHECK(touchpad_visible() && touchpad_buttons() == NES_B, "the next touch shows them and plays");
    send(SDL_FINGERUP, 12, b.x, b.y, 1);

    /* the corner button */
    const circle_t t = lay.toggle;
    send(SDL_FINGERDOWN, 13, t.x, t.y, 1);
    TCHECK(touchpad_buttons() == 0, "the corner button is no game button");
    TCHECK(send(SDL_FINGERUP, 13, t.x, t.y, 1) == TOUCHPAD_TOGGLED && !controls_on, "the corner button switches the controls off");
    TCHECK(!touchpad_visible(), "off: the controls do not show");
    send(SDL_FINGERDOWN, 14, a.x, a.y, 1);
    TCHECK(touchpad_buttons() == 0, "off: A does nothing");
    send(SDL_FINGERUP, 14, a.x, a.y, 1);
    send(SDL_FINGERDOWN, 15, lay.menu.x, lay.menu.y, 1);
    TCHECK(send(SDL_FINGERUP, 15, lay.menu.x, lay.menu.y, 1) == TOUCHPAD_NONE, "off: no MENU either");
    touchpad_handle_event(&pad, lay.w, lay.h, 1);
    TCHECK(hidden, "a controller press hides the corner button too");
    send(SDL_FINGERDOWN, 16, t.x, t.y, 1);
    TCHECK(!hidden, "the next touch shows it again");
    TCHECK(send(SDL_FINGERUP, 16, t.x, t.y, 1) == TOUCHPAD_NONE && !controls_on, "that touch does not press it (it was not showing)");
    send(SDL_FINGERDOWN, 17, t.x, t.y, 1);
    TCHECK(send(SDL_FINGERUP, 17, t.x + 8 * t.r, t.y + 8 * t.r, 1) == TOUCHPAD_NONE && !controls_on, "let go elsewhere: no switch");
    send(SDL_FINGERDOWN, 18, t.x, t.y, 0);
    TCHECK(send(SDL_FINGERUP, 18, t.x, t.y, 0) == TOUCHPAD_NONE && !controls_on, "in the menu the finger is the menu's");
    send(SDL_FINGERDOWN, 19, t.x, t.y, 1);
    TCHECK(send(SDL_FINGERUP, 19, t.x, t.y, 1) == TOUCHPAD_TOGGLED && controls_on && touchpad_visible(), "a tap brings the controls back");
    send(SDL_FINGERDOWN, 20, a.x, a.y, 1);
    TCHECK(touchpad_buttons() == NES_A, "and they play");
    send(SDL_FINGERUP, 20, a.x, a.y, 1);
    send(SDL_FINGERDOWN, 21, t.x + 2 * t.r, t.y, 1);
    TCHECK(send(SDL_FINGERUP, 21, t.x + 2 * t.r, t.y, 1) == TOUCHPAD_NONE && controls_on, "a touch beside the small button misses it");
#undef TCHECK
    touchpad_set_controls(saved_controls);
    touchpad_enable(saved);
    lay.w = lay.h = 0;
    printf("touch-test: %s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
