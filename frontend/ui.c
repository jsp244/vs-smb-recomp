/*
 * ui.c - tabbed options menu drawn with SDL_Renderer primitives and the
 * bitmap font. Navigable with keyboard, game controller and mouse.
 */
#include "ui.h"
#include "font.h"
#include "input.h"
#include <stdio.h>
#include <string.h>

static const SDL_Color C_BG     = { 14, 18, 22, 255 };
static const SDL_Color C_PANEL  = { 22, 28, 34, 255 };
static const SDL_Color C_ROW    = { 31, 42, 51, 255 };
static const SDL_Color C_ROWHOV = { 26, 35, 43, 255 };
static const SDL_Color C_ACCENT = { 53, 196, 162, 255 };
static const SDL_Color C_TEXT   = { 232, 238, 242, 255 };
static const SDL_Color C_DIM    = { 138, 151, 163, 255 };
static const SDL_Color C_LINE   = { 46, 58, 68, 255 };

static void fill(SDL_Renderer *r, SDL_Rect rc, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(r, &rc);
}

static int selectable(const ui_item *it) { return it->kind != UI_INFO; }

static int item_value_text(const ui_item *it, int capture, char *buf, size_t n) {
    switch (it->kind) {
    case UI_CHOICE: {
        int v = it->value ? *it->value : 0;
        if (v < 0 || v >= it->nchoices) v = 0;
        snprintf(buf, n, "%s", it->choices[v]);
        return 1; }
    case UI_INT:
        snprintf(buf, n, "%d%s", it->value ? *it->value : 0, it->suffix ? it->suffix : "");
        return 1;
    case UI_KEY:
        if (capture) snprintf(buf, n, it->pad ? "press a key or pad button..." : "press a key...");
        else if (it->pad) snprintf(buf, n, "%s  /  pad %s", SDL_GetKeyName((SDL_Keycode)(it->value ? *it->value : 0)), input_pad_name(*it->pad));
        else snprintf(buf, n, "%s", SDL_GetKeyName((SDL_Keycode)(it->value ? *it->value : 0)));
        return 1;
    case UI_TEXT: {
        const char *t = it->text ? it->text : "";
        size_t len = strlen(t);
        if (it->masked) {
            size_t k = len < n - 2 ? len : n - 2;
            memset(buf, '*', k); buf[k] = 0;
        } else {
            /* the end of a long string is what matters while typing */
            size_t keep = n - 2;
            snprintf(buf, n, "%s", len > keep ? t + (len - keep) : t);
        }
        if (capture) strncat(buf, "_", n - strlen(buf) - 1);
        else if (!len) snprintf(buf, n, "%s", it->placeholder ? it->placeholder : "(none)");
        return 1; }
    default:
        buf[0] = 0;
        return 0;
    }
}

/* text with '\n' between paragraphs, each wrapped at words to maxw pixels; draws it when
   draw is set; returns the number of lines */
static int wrap_text(SDL_Renderer *r, int x, int y, int maxw, int scale, SDL_Color c, const char *p, int draw) {
    int lh = FONT_CELL_H * scale + scale, lines = 0;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t plen = end ? (size_t)(end - p) : strlen(p);
        size_t start = 0;
        if (plen == 0) lines++;
        while (start < plen) {
            /* the longest run of words from start that fits (at least one word) */
            size_t best = 0;
            for (size_t k = start; k <= plen; k++) {
                if (k < plen && p[k] != ' ') continue;
                size_t len = k - start;
                if (len == 0) continue;
                if (best == 0 || (int)len * FONT_CELL_W * scale <= maxw) best = len;
                if ((int)len * FONT_CELL_W * scale > maxw) break;
            }
            if (draw) {
                char line[400];
                if (best >= sizeof line) best = sizeof line - 1;
                memcpy(line, p + start, best);
                line[best] = 0;
                font_draw(r, x, y + lines * lh, scale, c, 0, line);
            }
            lines++;
            start += best;
            while (start < plen && p[start] == ' ') start++;
        }
        p += plen;
        if (*p == '\n') p++;
    }
    return lines;
}

void ui_select_first(ui_menu *m) {
    ui_tab *t = &m->tabs[m->tab];
    t->sel = 0;
    for (int i = 0; i < t->nitems; i++) if (selectable(&t->items[i])) { t->sel = i; break; }
}

void ui_draw(SDL_Renderer *r, ui_menu *m, int ww, int wh) {
    int s = wh / 240; if (s < 1) s = 1; if (s > 6) s = 6;      /* body text scale */
    int ts = s * 2;                                            /* title scale */
    int mg = 8 * s;                                            /* margin */
    int row = 12 * s;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_Color panel = C_PANEL;
    if (m->backdrop) panel.a = 165;
    else { SDL_Rect all = { 0, 0, ww, wh }; fill(r, all, C_BG); }

    /* header */
    int y = mg;
    font_draw(r, mg, y, ts, C_ACCENT, 1, m->title ? m->title : "");
    if (m->subtitle) font_draw(r, ww - mg - font_width(m->subtitle, s), y + (FONT_CELL_H * ts - FONT_CELL_H * s), s, C_DIM, 0, m->subtitle);
    y += FONT_CELL_H * ts + mg / 2;
    SDL_Rect line = { mg, y, ww - 2 * mg, s > 1 ? s / 2 + 1 : 1 };
    fill(r, line, C_LINE);
    y += mg;

    int footer_h = 2 * (FONT_CELL_H * s + mg / 2) + mg;
    int body_h = wh - y - footer_h;
    if (body_h < row) body_h = row;

    /* tab column */
    int leftw = 17 * FONT_CELL_W * s;
    if (leftw > ww / 3) leftw = ww / 3;
    SDL_Rect left = { mg, y, leftw, body_h };
    fill(r, left, panel);
    for (int i = 0; i < m->ntabs && i < UI_MAX_TABS; i++) {
        SDL_Rect rc = { left.x, left.y + mg / 2 + i * (row + s), left.w, row };
        m->tab_rects[i] = rc;
        if (i == m->tab) {
            fill(r, rc, m->focus == 0 ? C_ROW : C_ROWHOV);
            SDL_Rect bar = { rc.x, rc.y, 2 * s, rc.h };
            fill(r, bar, C_ACCENT);
        } else if (i == m->hover_tab) {
            fill(r, rc, C_ROWHOV);
        }
        font_draw(r, rc.x + mg, rc.y + (row - FONT_CELL_H * s) / 2, s, i == m->tab ? C_TEXT : C_DIM, 0, m->tabs[i].name);
    }

    /* item panel: scrolls to keep the selected row in view when the tab is taller than the panel */
    SDL_Rect right = { left.x + left.w + mg, y, ww - (left.x + left.w + mg) - mg, body_h };
    fill(r, right, panel);
    ui_tab *t = &m->tabs[m->tab];
    int small = s > 1 ? s - 1 : 1;                  /* information text scale */
    int lh = FONT_CELL_H * small + small;
    int maxw = right.w - 2 * mg;
    int heights[UI_MAX_ITEMS];
    int total = mg / 2, sel_bottom = 0, last_sel = -1;
    for (int i = 0; i < t->nitems && i < UI_MAX_ITEMS; i++) {
        ui_item *it = &t->items[i];
        heights[i] = it->kind == UI_INFO ? wrap_text(r, 0, 0, maxw, small, C_DIM, it->info, 0) * lh + 2 * s : row;
        if (selectable(it)) last_sel = i;
        if (i == t->sel) sel_bottom = total + heights[i];
        total += heights[i] + s;
    }
    int view = right.h;
    int off = 0;
    if (total > view) {
        if (t->sel == last_sel) off = total - view;               /* the last row: show what follows it too */
        else if (sel_bottom + mg / 2 > view) off = sel_bottom + mg / 2 - view;
        if (off > total - view) off = total - view;
        if (off < 0) off = 0;
    }
    SDL_RenderSetClipRect(r, &right);
    int iy = right.y + mg / 2 - off;
    for (int i = 0; i < UI_MAX_ITEMS; i++) { m->item_rects[i].w = 0; m->left_arrow[i].w = 0; m->right_arrow[i].w = 0; }
    for (int i = 0; i < t->nitems && i < UI_MAX_ITEMS; i++) {
        ui_item *it = &t->items[i];
        int h = heights[i];
        /* only a row shown whole takes mouse clicks */
        int whole = iy >= right.y && iy + h <= right.y + right.h;
        if (iy + h < right.y || iy > right.y + right.h) { iy += h + s; continue; }
        if (it->kind == UI_INFO) {
            wrap_text(r, right.x + mg, iy + s, maxw, small, C_DIM, it->info, 1);
            if (whole) m->item_rects[i] = (SDL_Rect){ right.x, iy, right.w, h };
            iy += h + s;
            continue;
        }
        SDL_Rect rc = { right.x, iy, right.w, row };
        if (whole) m->item_rects[i] = rc;
        int selected = (m->focus == 1 && t->sel == i);
        if (selected) fill(r, rc, C_ROW);
        else if (i == m->hover_item) fill(r, rc, C_ROWHOV);
        if (selected) { SDL_Rect bar = { rc.x, rc.y, 2 * s, rc.h }; fill(r, bar, C_ACCENT); }
        int ty = rc.y + (row - FONT_CELL_H * s) / 2;
        font_draw(r, rc.x + mg, ty, s, selected ? C_TEXT : C_DIM, 0, it->label);
        char vbuf[96];
        if (item_value_text(it, m->capture && selected, vbuf, sizeof vbuf)) {
            if (it->kind == UI_TEXT && !(m->capture && selected) && it->text && !it->text[0]) {
                /* an empty text row shows a dim placeholder */
                int ew = font_width(vbuf, s);
                font_draw(r, rc.x + rc.w - mg - ew, ty, s, C_DIM, 0, vbuf);
                iy += row + s;
                continue;
            }
            int aw = FONT_CELL_W * s;                      /* arrow glyph width */
            int rx = rc.x + rc.w - mg - aw;                /* '>' position */
            int vw = font_width(vbuf, s);
            int vx = rx - s * 2 - vw;
            int lx = vx - s * 2 - aw;
            int adjustable = (it->kind == UI_CHOICE || it->kind == UI_INT);
            if (adjustable) {
                font_draw(r, lx, ty, s, selected ? C_ACCENT : C_DIM, 0, "<");
                font_draw(r, rx, ty, s, selected ? C_ACCENT : C_DIM, 0, ">");
                if (whole) {
                    m->left_arrow[i] = (SDL_Rect){ lx - s, rc.y, aw + 2 * s, rc.h };
                    m->right_arrow[i] = (SDL_Rect){ rx - s, rc.y, aw + 2 * s, rc.h };
                }
            }
            font_draw(r, vx, ty, s, selected ? C_TEXT : C_DIM, 0, vbuf);
        }
        iy += row + s;
    }
    SDL_RenderSetClipRect(r, NULL);
    /* more above / below */
    if (off > 0) font_draw(r, right.x + right.w - mg - FONT_CELL_W * small, right.y + s, small, C_ACCENT, 0, "^");
    if (off < total - view) font_draw(r, right.x + right.w - mg - FONT_CELL_W * small, right.y + right.h - FONT_CELL_H * small - s, small, C_ACCENT, 0, "v");

    /* footer */
    int fy = wh - footer_h + mg / 2;
    if (m->status[0]) font_draw(r, mg, fy, s, C_TEXT, 0, m->status);
    fy += FONT_CELL_H * s + mg / 2;
    if (m->hint) font_draw(r, mg, fy, s > 1 ? s - 1 : 1, C_DIM, 0, m->hint);
}

static ui_event ev(int type, int id) { ui_event e = { type, id }; return e; }

static ui_event change(ui_item *it, int dir) {
    if (it->kind == UI_CHOICE && it->value && it->nchoices > 0) {
        *it->value = (*it->value + dir + it->nchoices) % it->nchoices;
        return ev(UI_EV_CHANGED, it->id);
    }
    if (it->kind == UI_INT && it->value) {
        int v = *it->value + dir * (it->step ? it->step : 1);
        if (v < it->min) v = it->min;
        if (v > it->max) v = it->max;
        if (v != *it->value) { *it->value = v; return ev(UI_EV_CHANGED, it->id); }
    }
    return ev(UI_EV_NONE, 0);
}

static void move_sel(ui_tab *t, int dir) {
    if (t->nitems == 0) return;
    int i = t->sel;
    for (int k = 0; k < t->nitems; k++) {
        i = (i + dir + t->nitems) % t->nitems;
        if (selectable(&t->items[i])) { t->sel = i; return; }
    }
}

static void end_capture(ui_menu *m) {
    if (m->capture == UI_CAPTURE_TEXT) SDL_StopTextInput();
    m->capture = UI_CAPTURE_NONE;
}

static ui_event activate(ui_menu *m, ui_item *it) {
    switch (it->kind) {
    case UI_ACTION: return ev(UI_EV_ACTIVATE, it->id);
    case UI_CHOICE: return change(it, +1);
    case UI_KEY: m->capture = UI_CAPTURE_KEY; return ev(UI_EV_NONE, 0);
    case UI_TEXT:
        if (!it->text || it->text_cap < 2) return ev(UI_EV_NONE, 0);
        snprintf(m->text_backup, sizeof m->text_backup, "%s", it->text);
        m->capture = UI_CAPTURE_TEXT;
        SDL_StartTextInput();
        return ev(UI_EV_NONE, 0);
    default: return ev(UI_EV_NONE, 0);
    }
}

/* append UTF-8 text to the row being edited (control characters and newlines dropped) */
static void text_append(ui_item *it, const char *s) {
    size_t len = strlen(it->text);
    for (; *s; s++) {
        if ((unsigned char)*s < 32 || *s == 127) continue;
        if (len + 1 >= (size_t)it->text_cap) break;
        it->text[len++] = *s;
    }
    it->text[len] = 0;
}

static void text_backspace(ui_item *it) {
    size_t len = strlen(it->text);
    if (!len) return;
    len--;
    while (len > 0 && ((unsigned char)it->text[len] & 0xC0) == 0x80) len--;   /* whole UTF-8 sequence */
    it->text[len] = 0;
}

static ui_event text_key(ui_menu *m, ui_item *it, SDL_Keycode k, Uint16 mod) {
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER:
        end_capture(m);
        return ev(UI_EV_CHANGED, it->id);
    case SDLK_ESCAPE:
        snprintf(it->text, (size_t)it->text_cap, "%s", m->text_backup);
        end_capture(m);
        return ev(UI_EV_NONE, 0);
    case SDLK_BACKSPACE:
        text_backspace(it);
        return ev(UI_EV_NONE, 0);
    case SDLK_v:
        if (mod & KMOD_CTRL) {
            char *clip = SDL_GetClipboardText();
            if (clip) { text_append(it, clip); SDL_free(clip); }
        }
        return ev(UI_EV_NONE, 0);
    default:
        return ev(UI_EV_NONE, 0);   /* characters arrive as SDL_TEXTINPUT */
    }
}

static ui_event key_action(ui_menu *m, SDL_Keycode k) {
    ui_tab *t = &m->tabs[m->tab];
    ui_item *it = (t->nitems > 0) ? &t->items[t->sel] : NULL;
    if (m->capture == UI_CAPTURE_TEXT) {
        if (it && it->kind == UI_TEXT) return text_key(m, it, k, 0);
        end_capture(m);
        return ev(UI_EV_NONE, 0);
    }
    if (m->capture && it && it->kind == UI_KEY) {
        m->capture = UI_CAPTURE_NONE;
        if (k == SDLK_ESCAPE) return ev(UI_EV_NONE, 0);
        if (it->value) *it->value = (int)k;
        return ev(UI_EV_CHANGED, it->id);
    }
    switch (k) {
    case SDLK_UP:
        if (m->focus == 0) { if (m->tab > 0) m->tab--; ui_select_first(m); }
        else move_sel(t, -1);
        break;
    case SDLK_DOWN:
        if (m->focus == 0) { if (m->tab < m->ntabs - 1) m->tab++; ui_select_first(m); }
        else move_sel(t, +1);
        break;
    case SDLK_LEFT:
        if (m->focus == 1 && it) {
            if (it->kind == UI_CHOICE || it->kind == UI_INT) return change(it, -1);
            m->focus = 0;
        }
        break;
    case SDLK_RIGHT:
        if (m->focus == 0) { m->focus = 1; if (it && !selectable(it)) ui_select_first(m); }
        else if (it) return change(it, +1);
        break;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
        if (m->focus == 0) { m->focus = 1; if (it && !selectable(it)) ui_select_first(m); }
        else if (it) return activate(m, it);
        break;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_AC_BACK:
        if (m->focus == 1) m->focus = 0;
        else return ev(UI_EV_BACK, 0);
        break;
    default: break;
    }
    return ev(UI_EV_NONE, 0);
}

static int in_rect(const SDL_Rect *rc, int x, int y) {
    return rc->w > 0 && x >= rc->x && x < rc->x + rc->w && y >= rc->y && y < rc->y + rc->h;
}

ui_event ui_handle_event(ui_menu *m, const SDL_Event *e) {
    switch (e->type) {
    case SDL_KEYDOWN:
        /* a held key auto-repeats; while binding, only a fresh press may be captured */
        if (m->capture == UI_CAPTURE_KEY && e->key.repeat) return ev(UI_EV_NONE, 0);
        if (m->capture == UI_CAPTURE_TEXT) {
            ui_tab *t = &m->tabs[m->tab];
            ui_item *it = (t->nitems > 0) ? &t->items[t->sel] : NULL;
            if (it && it->kind == UI_TEXT) return text_key(m, it, e->key.keysym.sym, e->key.keysym.mod);
            end_capture(m);
            return ev(UI_EV_NONE, 0);
        }
        /* an auto-repeating Enter / Space / Escape (a key held since before the menu opened, such
           as the keyboard's Start + Select) must not activate or close anything; arrows still repeat */
        if (e->key.repeat) {
            SDL_Keycode k = e->key.keysym.sym;
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_ESCAPE || k == SDLK_BACKSPACE || k == SDLK_AC_BACK)
                return ev(UI_EV_NONE, 0);
        }
        return key_action(m, e->key.keysym.sym);
    case SDL_CONTROLLERAXISMOTION:
        /* the triggers are axes: pulled past half way while a row waits for a binding, they bind like buttons */
        if (m->capture == UI_CAPTURE_KEY && e->caxis.value > 16000 &&
            (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
            ui_tab *t = &m->tabs[m->tab];
            ui_item *it = (t->nitems > 0) ? &t->items[t->sel] : NULL;
            m->capture = UI_CAPTURE_NONE;
            if (it && it->kind == UI_KEY && it->pad) {
                *it->pad = e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? INPUT_PAD_LT : INPUT_PAD_RT;
                return ev(UI_EV_CHANGED, it->id);
            }
        }
        return ev(UI_EV_NONE, 0);
    case SDL_TEXTINPUT:
        if (m->capture == UI_CAPTURE_TEXT) {
            ui_tab *t = &m->tabs[m->tab];
            ui_item *it = (t->nitems > 0) ? &t->items[t->sel] : NULL;
            if (it && it->kind == UI_TEXT) text_append(it, e->text.text);
        }
        return ev(UI_EV_NONE, 0);
    case SDL_CONTROLLERBUTTONDOWN:
        if (m->capture == UI_CAPTURE_TEXT) {   /* the pad cannot type: A keeps the text, B cancels */
            if (e->cbutton.button == SDL_CONTROLLER_BUTTON_A) return key_action(m, SDLK_RETURN);
            if (e->cbutton.button == SDL_CONTROLLER_BUTTON_B) return key_action(m, SDLK_ESCAPE);
            return ev(UI_EV_NONE, 0);
        }
        if (m->capture) {   /* binding a pad button to the selected row */
            ui_tab *t = &m->tabs[m->tab];
            ui_item *it = (t->nitems > 0) ? &t->items[t->sel] : NULL;
            m->capture = UI_CAPTURE_NONE;
            if (it && it->kind == UI_KEY && it->pad) { *it->pad = e->cbutton.button; return ev(UI_EV_CHANGED, it->id); }
            return ev(UI_EV_NONE, 0);
        }
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return key_action(m, SDLK_UP);
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return key_action(m, SDLK_DOWN);
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return key_action(m, SDLK_LEFT);
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return key_action(m, SDLK_RIGHT);
        case SDL_CONTROLLER_BUTTON_A: return key_action(m, SDLK_RETURN);
        case SDL_CONTROLLER_BUTTON_B: return key_action(m, SDLK_ESCAPE);
        case SDL_CONTROLLER_BUTTON_START: return ev(UI_EV_BACK, 0);
        default: break;
        }
        break;
    case SDL_JOYBUTTONDOWN:          /* generic joystick fallback: 0 = confirm, 1 = back, 7 = start */
#ifdef __ANDROID__
        if (m->capture == UI_CAPTURE_KEY && (e->jbutton.button == 15 || e->jbutton.button == 16)) {
            /* L2 / R2 that are plain buttons (see input.c) bind as LT / RT */
            SDL_GameController *gc = SDL_GameControllerFromInstanceID(e->jbutton.which);
            SDL_GameControllerAxis ax = e->jbutton.button == 15 ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
            if (gc && !SDL_GameControllerHasAxis(gc, ax)) {
                ui_tab *t = &m->tabs[m->tab];
                ui_item *it = (t->nitems > 0) ? &t->items[t->sel] : NULL;
                m->capture = UI_CAPTURE_NONE;
                if (it && it->kind == UI_KEY && it->pad) {
                    *it->pad = e->jbutton.button == 15 ? INPUT_PAD_LT : INPUT_PAD_RT;
                    return ev(UI_EV_CHANGED, it->id);
                }
                return ev(UI_EV_NONE, 0);
            }
        }
#endif
        if (SDL_GameControllerFromInstanceID(e->jbutton.which)) break;   /* already handled as a controller */
        if (e->jbutton.button == 0) return key_action(m, SDLK_RETURN);
        if (e->jbutton.button == 1) return key_action(m, SDLK_ESCAPE);
        if (e->jbutton.button == 7) return ev(UI_EV_BACK, 0);
        break;
    case SDL_JOYHATMOTION:
        if (SDL_GameControllerFromInstanceID(e->jhat.which)) break;
        if (e->jhat.value & SDL_HAT_UP) return key_action(m, SDLK_UP);
        if (e->jhat.value & SDL_HAT_DOWN) return key_action(m, SDLK_DOWN);
        if (e->jhat.value & SDL_HAT_LEFT) return key_action(m, SDLK_LEFT);
        if (e->jhat.value & SDL_HAT_RIGHT) return key_action(m, SDLK_RIGHT);
        break;
    case SDL_MOUSEMOTION: {
        int x = e->motion.x, y = e->motion.y;
        m->hover_tab = -1; m->hover_item = -1;
        for (int i = 0; i < m->ntabs && i < UI_MAX_TABS; i++) if (in_rect(&m->tab_rects[i], x, y)) m->hover_tab = i;
        ui_tab *t = &m->tabs[m->tab];
        for (int i = 0; i < t->nitems && i < UI_MAX_ITEMS; i++)
            if (selectable(&t->items[i]) && in_rect(&m->item_rects[i], x, y)) m->hover_item = i;
        break; }
    case SDL_MOUSEBUTTONDOWN: {
        if (e->button.button != SDL_BUTTON_LEFT) break;
        int x = e->button.x, y = e->button.y;
        if (m->capture) { end_capture(m); break; }
        for (int i = 0; i < m->ntabs && i < UI_MAX_TABS; i++)
            if (in_rect(&m->tab_rects[i], x, y)) { m->tab = i; m->focus = 0; ui_select_first(m); return ev(UI_EV_NONE, 0); }
        ui_tab *t = &m->tabs[m->tab];
        for (int i = 0; i < t->nitems && i < UI_MAX_ITEMS; i++) {
            ui_item *it = &t->items[i];
            if (!selectable(it) || !in_rect(&m->item_rects[i], x, y)) continue;
            t->sel = i; m->focus = 1;
            if (in_rect(&m->left_arrow[i], x, y)) return change(it, -1);
            if (in_rect(&m->right_arrow[i], x, y)) return change(it, +1);
            return activate(m, it);
        }
        break; }
    case SDL_MOUSEWHEEL: {
        if (m->hover_item < 0) break;
        ui_tab *t = &m->tabs[m->tab];
        ui_item *it = &t->items[m->hover_item];
        if (it->kind == UI_CHOICE || it->kind == UI_INT) { t->sel = m->hover_item; m->focus = 1; return change(it, e->wheel.y > 0 ? +1 : -1); }
        break; }
    default: break;
    }
    return ev(UI_EV_NONE, 0);
}
