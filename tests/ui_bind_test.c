/*
 * ui_bind_test.c - feeds synthetic keyboard / game-controller events through the
 * menu code and checks that rebinding produces the expected key and pad bindings.
 * Build: see tests/run_ui_bind_test.sh (needs SDL2; no window or device is opened).
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "input.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } else printf("ok:   %s\n", msg); } while (0)

static ui_event key(ui_menu *m, SDL_Keycode k, int repeat) {
    SDL_Event e; memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN; e.key.keysym.sym = k; e.key.repeat = (Uint8)repeat;
    return ui_handle_event(m, &e);
}

static ui_event pad(ui_menu *m, int button) {
    SDL_Event e; memset(&e, 0, sizeof e);
    e.type = SDL_CONTROLLERBUTTONDOWN; e.cbutton.button = (Uint8)button;
    return ui_handle_event(m, &e);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    SDL_SetMainReady();
    int keyval = SDLK_x, padval = SDL_CONTROLLER_BUTTON_B, other_key = SDLK_z, other_pad = SDL_CONTROLLER_BUTTON_A;
    ui_item items[3];
    memset(items, 0, sizeof items);
    items[0] = (ui_item){ "A (jump)", UI_KEY, 7, &keyval, NULL, 0, 0, 0, 0, NULL, "", &padval };
    items[1] = (ui_item){ "B (attack)", UI_KEY, 8, &other_key, NULL, 0, 0, 0, 0, NULL, "", &other_pad };
    items[2] = (ui_item){ "Reset", UI_ACTION, 9, NULL, NULL, 0, 0, 0, 0, NULL, "", NULL };
    ui_tab tab = { "Controls", items, 3, 0 };
    ui_menu m; memset(&m, 0, sizeof m);
    m.tabs = &tab; m.ntabs = 1; m.focus = 1; m.hover_tab = m.hover_item = -1;

    /* 1. Enter on a row starts capture */
    ui_event e = key(&m, SDLK_RETURN, 0);
    CHECK(e.type == UI_EV_NONE && m.capture == 1, "Enter starts capturing on the A row");

    /* 2. a held Enter's auto-repeat must not be captured as the new key */
    e = key(&m, SDLK_RETURN, 1);
    CHECK(m.capture == 1 && keyval == SDLK_x, "auto-repeated Enter is ignored while capturing");

    /* 3. a controller button press is bound to the pad slot, key untouched */
    e = pad(&m, SDL_CONTROLLER_BUTTON_X);
    CHECK(e.type == UI_EV_CHANGED && e.id == 7 && padval == SDL_CONTROLLER_BUTTON_X && keyval == SDLK_x && m.capture == 0,
          "pad X press rebinds the A row's pad button and reports CHANGED");

    /* 4. keyboard capture: Enter then 'q' binds the key, pad untouched */
    key(&m, SDLK_RETURN, 0);
    e = key(&m, SDLK_q, 0);
    CHECK(e.type == UI_EV_CHANGED && keyval == SDLK_q && padval == SDL_CONTROLLER_BUTTON_X && m.capture == 0,
          "key q rebinds the A row's key");

    /* 5. Escape cancels a capture without changing anything */
    key(&m, SDLK_RETURN, 0);
    e = key(&m, SDLK_ESCAPE, 0);
    CHECK(e.type == UI_EV_NONE && m.capture == 0 && keyval == SDLK_q, "Escape cancels capture");

    /* 6. moving down with the d-pad and binding the B row with the pad's B button (even B, which normally means back) */
    e = pad(&m, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(tab.sel == 1, "d-pad down selects the B row");
    e = pad(&m, SDL_CONTROLLER_BUTTON_A);          /* A = confirm = start capture */
    CHECK(m.capture == 1, "pad A starts capture on the B row");
    e = pad(&m, SDL_CONTROLLER_BUTTON_B);          /* while capturing, B is a binding, not 'back' */
    CHECK(e.type == UI_EV_CHANGED && e.id == 8 && other_pad == SDL_CONTROLLER_BUTTON_B && m.capture == 0,
          "pad B is captured as a binding instead of acting as back");

    /* 7. names round-trip through the ini format */
    CHECK(input_pad_from_name(input_pad_name(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) == SDL_CONTROLLER_BUTTON_DPAD_LEFT, "pad name round-trip (D-pad Left)");
    CHECK(input_pad_from_name("none") == -1 && input_pad_from_name("dpup") == SDL_CONTROLLER_BUTTON_DPAD_UP, "pad names: none and SDL's own names");
    CHECK(SDL_GetKeyFromName(SDL_GetKeyName(SDLK_RSHIFT)) == SDLK_RSHIFT, "key name round-trip (Right Shift)");

    /* 8. the merged NES pad read with no devices: keyboard only (no SDL video, so no keys are down) */
    int keys[8] = { SDLK_x, SDLK_z, SDLK_RSHIFT, SDLK_RETURN, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT }, pads[8];
    input_default_pads(pads);
    CHECK(pads[0] == SDL_CONTROLLER_BUTTON_A && pads[1] == SDL_CONTROLLER_BUTTON_X && pads[2] == SDL_CONTROLLER_BUTTON_BACK &&
          pads[3] == SDL_CONTROLLER_BUTTON_START,
          "default pad layout: A = pad A (jump), B = pad X (run, left of A as on the NES pad), Back = coin, Start = Start");
    (void)keys;

    /* 9. text rows: Enter edits, typed text and Backspace apply, Enter keeps, Escape restores */
    {
        char name[16] = "old";
        ui_item titems[2];
        memset(titems, 0, sizeof titems);
        titems[0] = (ui_item){ "User name", UI_TEXT, 21, NULL, NULL, 0, 0, 0, 0, NULL, "", NULL, name, sizeof name, 0 };
        titems[1] = (ui_item){ "Log in", UI_ACTION, 22, NULL, NULL, 0, 0, 0, 0, NULL, "", NULL };
        ui_tab ttab = { "Achievements", titems, 2, 0 };
        ui_menu tm; memset(&tm, 0, sizeof tm);
        tm.tabs = &ttab; tm.ntabs = 1; tm.focus = 1; tm.hover_tab = tm.hover_item = -1;
        e = key(&tm, SDLK_RETURN, 0);
        CHECK(e.type == UI_EV_NONE && tm.capture == UI_CAPTURE_TEXT, "Enter on a text row starts editing");
        SDL_Event te; memset(&te, 0, sizeof te);
        te.type = SDL_TEXTINPUT; strcpy(te.text.text, "ab");
        ui_handle_event(&tm, &te);
        e = key(&tm, SDLK_BACKSPACE, 0);
        strcpy(te.text.text, "c"); ui_handle_event(&tm, &te);
        CHECK(strcmp(name, "oldac") == 0 && tm.capture == UI_CAPTURE_TEXT, "typed text is appended and Backspace deletes (oldac)");
        e = key(&tm, SDLK_DOWN, 0);
        CHECK(ttab.sel == 0 && tm.capture == UI_CAPTURE_TEXT, "arrow keys do not leave the row while editing");
        e = key(&tm, SDLK_RETURN, 0);
        CHECK(e.type == UI_EV_CHANGED && e.id == 21 && tm.capture == 0 && strcmp(name, "oldac") == 0, "Enter keeps the text and reports CHANGED");
        key(&tm, SDLK_RETURN, 0);
        strcpy(te.text.text, "zzz"); ui_handle_event(&tm, &te);
        e = key(&tm, SDLK_ESCAPE, 0);
        CHECK(e.type == UI_EV_NONE && tm.capture == 0 && strcmp(name, "oldac") == 0, "Escape restores the previous text");
        e = key(&tm, SDLK_DOWN, 0);
        e = key(&tm, SDLK_RETURN, 0);
        CHECK(e.type == UI_EV_ACTIVATE && e.id == 22, "the action row below still activates");
    }

    /* 9b. a trigger binds like a button and its name round-trips through the ini format */
    {
        int k2 = SDLK_TAB, p2 = -1;
        ui_item titem[1];
        memset(titem, 0, sizeof titem);
        titem[0] = (ui_item){ "Fast-forward (hold)", UI_KEY, 41, &k2, NULL, 0, 0, 0, 0, NULL, "", &p2 };
        ui_tab ttab2 = { "Controls", titem, 1, 0 };
        ui_menu tm2; memset(&tm2, 0, sizeof tm2);
        tm2.tabs = &ttab2; tm2.ntabs = 1; tm2.focus = 1; tm2.hover_tab = tm2.hover_item = -1;
        key(&tm2, SDLK_RETURN, 0);
        SDL_Event ax; memset(&ax, 0, sizeof ax);
        ax.type = SDL_CONTROLLERAXISMOTION; ax.caxis.axis = SDL_CONTROLLER_AXIS_TRIGGERRIGHT; ax.caxis.value = 3000;
        e = ui_handle_event(&tm2, &ax);
        CHECK(e.type == UI_EV_NONE && tm2.capture == 1, "a lightly touched trigger does not bind");
        ax.caxis.value = 30000;
        e = ui_handle_event(&tm2, &ax);
        CHECK(e.type == UI_EV_CHANGED && p2 == INPUT_PAD_RT && k2 == SDLK_TAB && tm2.capture == 0, "pulling RT binds the right trigger");
        CHECK(strcmp(input_pad_name(INPUT_PAD_RT), "RT") == 0 && input_pad_from_name("lt") == INPUT_PAD_LT, "trigger names: RT and LT");
    }

    /* 10. an auto-repeated Enter (held since before the menu opened) activates nothing; a fresh press does */
    {
        ui_item aitems[1];
        memset(aitems, 0, sizeof aitems);
        aitems[0] = (ui_item){ "Resume game", UI_ACTION, 31, NULL, NULL, 0, 0, 0, 0, NULL, "", NULL };
        ui_tab atab = { "Play", aitems, 1, 0 };
        ui_menu am; memset(&am, 0, sizeof am);
        am.tabs = &atab; am.ntabs = 1; am.focus = 1; am.hover_tab = am.hover_item = -1;
        e = key(&am, SDLK_RETURN, 1);
        CHECK(e.type == UI_EV_NONE, "a repeated Enter does not resume the game");
        e = key(&am, SDLK_ESCAPE, 1);
        CHECK(e.type == UI_EV_NONE && am.focus == 1, "a repeated Escape does not leave the item list");
        e = key(&am, SDLK_RETURN, 0);
        CHECK(e.type == UI_EV_ACTIVATE && e.id == 31, "a fresh Enter press still activates");
    }

    /* 11. a choice row with a single choice (Run-ahead, locked off while the All-Stars graphics are on) is a
       fixed value: Left, Right, Enter and the pad change nothing; a row with more choices still changes */
    {
        static const char *const one[] = { "Off (All-Stars graphics)" };
        static const char *const four[] = { "Off", "1 frame", "2 frames", "3 frames" };
        int locked = 0, ra = 2, moved = 0;
        ui_item citems[2];
        memset(citems, 0, sizeof citems);
        citems[0] = (ui_item){ "Run-ahead", UI_CHOICE, 41, &locked, one, 1, 0, 0, 0, NULL, "", NULL };
        citems[1] = (ui_item){ "Run-ahead", UI_CHOICE, 42, &ra, four, 4, 0, 0, 0, NULL, "", NULL };
        ui_tab ctab = { "Graphics", citems, 2, 0 };
        ui_menu cm; memset(&cm, 0, sizeof cm);
        cm.tabs = &ctab; cm.ntabs = 1; cm.focus = 1; cm.hover_tab = cm.hover_item = -1;
        moved |= key(&cm, SDLK_RIGHT, 0).type != UI_EV_NONE;
        moved |= key(&cm, SDLK_LEFT, 0).type != UI_EV_NONE;
        moved |= key(&cm, SDLK_RETURN, 0).type != UI_EV_NONE;
        moved |= pad(&cm, SDL_CONTROLLER_BUTTON_DPAD_RIGHT).type != UI_EV_NONE;
        moved |= pad(&cm, SDL_CONTROLLER_BUTTON_A).type != UI_EV_NONE;
        CHECK(!moved && locked == 0 && ctab.sel == 0, "a single-choice row (run-ahead locked off) does not change");
        key(&cm, SDLK_DOWN, 0);
        e = key(&cm, SDLK_RIGHT, 0);
        CHECK(ctab.sel == 1 && e.type == UI_EV_CHANGED && e.id == 42 && ra == 3, "a row with four choices still changes");
    }

    printf("%s (%d failure%s)\n", failures ? "UI BINDING TEST FAILED" : "UI BINDING TEST PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
