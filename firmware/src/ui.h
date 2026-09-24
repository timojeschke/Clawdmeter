#pragma once
#include "data.h"
#include "ble.h"

// Carousel order — the side buttons step through this list and wrap.
// Sessions sits first because it is the screen that asks something of the
// user; the splash sits last so a page turn never lands on it by accident.
enum screen_t {
    SCREEN_SESSIONS,
    SCREEN_USAGE,
    SCREEN_SPLASH,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);

// Page carousel driven by the side buttons: step one screen forward or back
// in enum order, wrapping at both ends. Boards with a single side button only
// ever call ui_next_screen() and still reach every page.
void ui_next_screen(void);
void ui_prev_screen(void);

// Blaettert selbsttaetig zwischen Usage und Sessions, solange niemand einen
// Knopf drueckt. Aus der Hauptschleife aufzurufen; der Splash bleibt aussen
// vor, er ist der Bildschirmschoner und wird bewusst aufgerufen.
void ui_auto_rotate_tick(void);
void ui_auto_rotate_set(bool an);

void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
