#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
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

void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
