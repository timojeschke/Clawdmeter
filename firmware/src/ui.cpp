#include "ui.h"
#include "splash.h"
#include "brightness.h"
#include "idle.h"
#include <Preferences.h>
#include "battery_runtime.h"
#include <lvgl.h>
#include <time.h>
#include "logo.h"
#include "clawd_still.h"
#include "icons.h"
#include "hal/board_caps.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_24_fett);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_mono_18);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    int16_t bar_h;
    int16_t panel_pad_x, panel_pad_y;
    int16_t pill_pad_x, pill_pad_y;
    const lv_font_t* title_font;     // screen title / clock
    const lv_font_t* pct_font;       // big percentage number
    const lv_font_t* ent_pct_font;   // enterprise spending number
    const lv_font_t* pill_font;      // "Current" / "Weekly" pill
    const lv_font_t* reset_font;     // "Resets in ..." line
    const lv_font_t* pace_font;      // enterprise "Under/On/Over pace" line
    const lv_font_t* anim_font;      // animated status line
    int16_t anim_y;                  // status line offset from bottom
    bool    small_icons;             // 40px logo + 24px battery (vs 80/48) on small screens
    int16_t title_nudge;             // Verschiebung der Ueberschrift, damit sie
                                     // zwischen Logo und Batterie mittig sitzt
                                     // statt mittig im Bildschirm — links steht
                                     // das Logo, rechts die Batterie, und beide
                                     // sind verschieden breit. Berechnet in
                                     // compute_layout(), nicht geraten.
    int16_t logo_y;                  // logo top edge
    int16_t batt_y;                  // battery icon top edge
    int16_t batt_w;                  // battery icon width, for position math
    const lv_font_t* sess_count_font; // session counts — serif, like the title
    const lv_font_t* sess_name_font;    // die Sessionnamen selbst
    const lv_font_t* sess_caption_font; // "Total" / "Running" / "Running now" —
                                        // read from across the desk, so a step
                                        // above the pace line they used to share
    const lv_font_t* batt_font;      // battery percentage
    int16_t batt_lbl_gap;            // gap when the percentage sits beside it
    int16_t batt_h;                  // battery body height
    int16_t batt_nub_w, batt_nub_h;  // the little contact stub on the right
    bool    batt_inside;             // percentage inside the body, or beside it

    // Pairing hint / idle screen
    int16_t pair_y1, pair_y2, pair_y3;
    int16_t idle_px;                 // sleeping-creature size on the idle screen

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
// Abstand zwischen Batteriekoerper und Kontaktstueck.
#define BATT_NUB_GAP   1

// Mitte des freien Felds zwischen Logo und Batterie, als Abweichung von der
// Bildschirmmitte. Setzt voraus, dass L.margin, L.scr_w, L.batt_w und
// L.batt_nub_w bereits gesetzt sind.
static int16_t titel_versatz(int16_t logo_w) {
    const int16_t logo_rechts = L.margin + logo_w;
    const int16_t batt_links  = L.scr_w - L.margin
                              - (L.batt_w + BATT_NUB_GAP + L.batt_nub_w);
    return (int16_t)(((logo_rechts + batt_links) / 2) - (L.scr_w / 2));
}

static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    // Values shared by the two original breakpoints; the small branch below
    // overrides them wholesale.
    L.bar_h = 24;
    L.panel_pad_x = 16;
    L.panel_pad_y = 12;
    L.pill_pad_x = 18;
    L.pill_pad_y = 6;
    L.title_font   = &font_tiempos_56;
    L.pct_font     = &font_styrene_48;
    L.ent_pct_font = &font_tiempos_56;
    L.pill_font    = &font_styrene_28;
    L.reset_font   = &font_styrene_28;
    L.pace_font    = &font_styrene_16;
    L.anim_font    = &font_mono_32;
    L.anim_y = -15;
    L.small_icons = false;
    L.logo_y = L.title_y - 10;
    // Centred where the 48 px icon's centre used to be, so the header keeps
    // its balance against the logo on the left.
    // Tiempos for the counts, not Styrene: the serif is Claude's display face
    // and it ties the three numbers to the "Sessions" title above them.
    L.sess_count_font = &font_tiempos_56;
    L.sess_name_font = &font_styrene_28;
    L.sess_caption_font = &font_styrene_20;
    L.batt_y = L.title_y + 6;
    // Deliberately larger than the 48 px icon it replaced: at arm's length on
    // a desk the number has to be readable at a glance, and the header has the
    // room. Only one weight of Styrene ships, so "heavier" means a larger size.
    L.batt_w = 66;
    L.batt_h = 34;
    L.batt_nub_w = 6;
    L.batt_nub_h = 16;
    L.batt_inside = true;
    L.batt_font = &font_styrene_24_fett;
    L.batt_lbl_gap = 6;
    L.pair_y1 = 40;
    L.pair_y2 = 120;
    L.pair_y3 = 160;
    L.idle_px = 160;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else if (c.height >= 300) {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    } else {
        // Small layout — tuned for 240x240 (LCD-1.54 and similar square TFTs).
        // Everything shrinks: fonts two steps down, panels ~half height, and
        // the corner logo/battery switch to the 40px/24px small assets.
        L.margin = 8;
        L.title_y = 4;
        L.content_y = 44;
        L.usage_panel_h = 74;
        L.usage_panel_gap = 6;
        L.usage_bar_y = 30;
        L.usage_reset_y = 46;
        L.bar_h = 12;
        L.panel_pad_x = 10;
        L.panel_pad_y = 6;
        L.pill_pad_x = 8;
        L.pill_pad_y = 2;
        L.title_font   = &font_tiempos_34;
        L.pct_font     = &font_styrene_24;
        L.ent_pct_font = &font_tiempos_34;
        L.pill_font    = &font_styrene_14;
        L.reset_font   = &font_styrene_14;
        L.pace_font    = &font_styrene_12;
        L.anim_font    = &font_mono_18;
        // Center the status line in the strip below the weekly panel; flush
        // against the bottom edge it reads as unevenly spaced.
        L.anim_y = -10;
        L.small_icons = true;
        L.logo_y = 2;
        L.sess_count_font = &font_tiempos_34;
        L.sess_name_font = &font_styrene_14;
        L.sess_caption_font = &font_styrene_14;
        L.batt_y = 16;   // same centre the 24 px icon had
        // At this size the interior is ~7 px tall — no font is legible in
        // there, so the number stays beside the battery on small screens.
        L.batt_w = 20;
        L.batt_h = 11;
        L.batt_nub_w = 2;
        L.batt_nub_h = 5;
        L.batt_inside = false;
        L.batt_font = &font_styrene_12;
        L.batt_lbl_gap = 3;
        L.pair_y1 = 12;
        L.pair_y2 = 56;
        L.pair_y3 = 80;
        L.idle_px = 96;
        L.bt_info_panel_h = 90;
        L.bt_reset_zone_h = 60;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_20;
        L.bt_device_font   = &font_styrene_14;
        L.bt_credit_1_font = &font_styrene_12;
        L.bt_credit_2_font = &font_styrene_12;
    }

    L.content_w = L.scr_w - 2 * L.margin;
    // ZULETZT: Die Ueberschrift soll mittig zwischen Logo und Batterie stehen,
    // nicht mittig im Bildschirm — links das Logo, rechts die Batterie, beide
    // verschieden breit. Die Rechnung braucht L.margin, L.batt_w und
    // L.batt_nub_w, und die stehen erst hier fest.
    //
    // Genau daran ist die erste Fassung gescheitert: Sie rechnete oben im
    // Block, als L.batt_w noch 0 war, und schob den Titel dadurch 35 Pixel zu
    // weit nach rechts. Eine Funktion mit Voraussetzungen gehoert dorthin, wo
    // die Voraussetzungen erfuellt sind.
    L.title_nudge = titel_versatz(L.small_icons ? CLAWD_STILL_SMALL_W
                                                : CLAWD_STILL_W);

}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t* usage_container;

// ---- Sessions screen ----
static lv_obj_t* sessions_container;
static lv_obj_t* sess_count_lbl[3];     // waiting / working / parked
static lv_obj_t* sess_name_lbl[SESSIONS_MAX_NAMES];
static lv_obj_t* sess_counts_panel;     // Total / Running / Idle
static lv_obj_t* sess_list_panel;       // card holding the waiting sessions
static lv_obj_t* sess_list_caption;     // "Waiting for input" above the names
static lv_obj_t* sess_more_lbl;         // "+N more" when the list was trimmed
static int16_t   sess_row_h;            // row pitch, for resizing the card
static int16_t   sess_first_row_y;      // y of the first name inside the card
static int16_t   sess_list_max_h;       // card height when it runs to the bottom
#define SESS_ZEILEN_WUNSCH 4
static int16_t   sess_max_zeilen;       // wie viele Namen wirklich in die Karte passen
static lv_obj_t* sess_hint_lbl;         // shown when no session data has arrived
static lv_obj_t* lbl_title;
static lv_obj_t* sess_title_lbl;   // Ueberschrift der Sessions-Seite
// Clock fed by the daemon: base epoch (local wall-clock seconds) + the lv_tick at
// which it landed, so the title ticks forward locally between 60s payloads.
static long     clock_base_epoch = 0;
static uint32_t clock_base_ms = 0;
static int      clock_fmt = 24;   // 12 or 24, set from the daemon payload
static int      clock_last_min = -1;   // last rendered minute; avoids redrawing the title every tick
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* panel_session = nullptr;
static lv_obj_t* panel_weekly = nullptr;
// Enterprise-only widgets inside panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" in smaller font
static lv_obj_t* lbl_spending_desc = nullptr;     // "of your monthly budget"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under pace" / "On pace" / "Over pace"
static lv_obj_t* lbl_anim;
static lv_obj_t* lbl_scoped;
static lv_obj_t* scoped_ring;    // kleiner Fortschrittsring links der Quote    // per-model weekly quota, replaces the idle line      // status line: connection state + whimsical idle

// ---- Battery indicator (shared, on top) ----
// ---- Battery indicator: drawn, not an icon ----
// The Lucide battery glyph fills its interior with level bars, so a number
// placed inside would sit on top of them. Drawing the battery ourselves frees
// the interior for the percentage — the way phones show it — and lets the fill
// take its colour from theme.h instead of being baked into an image.
#define BATT_BORDER_W  2
#define BATT_LOW_PCT  10   // below this the fill turns red
// Solid terracotta, the same accent the usage bars use — the header then reads
// as part of the same design instead of a grey box borrowed from elsewhere.
// Durchmesser des Rings neben der Modellquote, und die Dicke seines Bogens.
#define SCOPED_RING_PX    34
#define SCOPED_RING_Y_KORR 2
#define SCOPED_RING_DICKE  5

// Senkrechter Ausgleich der Ziffern im Batteriekoerper, ausgemessen.
#define BATT_ZAHL_Y_KORR -2

// Wie lange der letzte Sessionstand ohne Nachschub weitergilt. Der Daemon
// schickt alle drei Sekunden; zwei Minuten decken einen Serverneustart und
// einen Verbindungsabbruch ab, ohne veraltete Namen ewig stehen zu lassen.
#define SESSIONS_STALE_MS (2u * 60u * 1000u)

#define BATT_FILL_OPA LV_OPA_COVER

static lv_obj_t* battery_body;
static lv_obj_t* battery_fill;
static lv_obj_t* battery_nub;
static lv_obj_t* battery_lbl;
// Faux-Fettung: zwei versetzte Kopien hinter der Zahl. Styrene liegt hier nur
// als Regular vor (assets/StyreneB-Regular.otf); ein echter Fettschnitt hiesse
// eine zweite Schriftdatei fuer drei Ziffern. Zwei Versaetze statt einem, weil
// einer allein gegen die gefuellte Flaeche noch zu duenn wirkte.
// Styrene liegt hier nur als Regular vor (assets/StyreneB-Regular.otf). Statt
// eine zweite Schriftdatei fuer drei Ziffern einzubinden, wird die Zahl acht
// Mal ringsum versetzt gezeichnet — eine Umrandung von einem Pixel in jede
// Richtung. Das verdickt den Strich symmetrisch; ein Versatz nur nach unten
// rechts, wie vorher, sieht aus wie ein Schatten und nicht wie Fettdruck.
static lv_obj_t* battery_sub_lbl;   // charge symbol while charging, else time left
static lv_obj_t* logo_img;

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static bool      data_ok = true;        // last payload's ok flag; a {"ok":false} beat = "no fresh data"
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 usage
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;

// Die schlafende Kreatur der Usage-Seite, damit die Sessionseite dieselbe
// zeigen kann. splash.cpp haelt genau EINE (siehe splash.h) — ein zweites
// Exemplar waere ein zweiter Puffer von rund 32 KB, und der C6 hat kein
// PSRAM. Sie wird deshalb umgehaengt statt verdoppelt; sichtbar ist ohnehin
// immer nur eine Seite.
static lv_obj_t* mini_kreatur = NULL;

// Der zuletzt empfangene Stand. Die Sessionseite muss sich auch dann
// aktualisieren, wenn gerade NICHTS ankommt — genau das war der Fehler: Ihre
// Aktualisierung haengt an ui_update(), und die ruft main.cpp nur beim
// Eintreffen einer Nutzlast. Ohne Funk blieb die Seite deshalb im Zustand vom
// Aufbau stehen: schmale Karte, linksbuendiger Hinweis, Ueberschrift ueber
// einer Fehlmeldung.
static UsageData letzte_daten;
static bool sessionseite_leer = false;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_right(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_top(panel, L.panel_pad_y, 0);
    lv_obj_set_style_pad_bottom(panel, L.panel_pad_y, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, L.pill_font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_right(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_top(lbl, L.pill_pad_y, 0);
    lv_obj_set_style_pad_bottom(lbl, L.pill_pad_y, 0);
    return lbl;
}

// Builds the battery out of primitives: body, fill, contact stub, number.
// Right-aligned as a unit so the stub lands where the old icon's edge was.
static void rate_laden(void);

static void battery_create(lv_obj_t* parent) {
    // Boards without battery telemetry never show the indicator (per the HAL
    // contract; previously every board drew the empty-battery glyph).
    if (!board_caps().has_battery) return;

    const int16_t total_w = L.batt_w + BATT_NUB_GAP + L.batt_nub_w;
    const int16_t body_x  = L.scr_w - L.margin - total_w;

    battery_body = lv_obj_create(parent);
    lv_obj_remove_style_all(battery_body);
    lv_obj_clear_flag(battery_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(battery_body, L.batt_w, L.batt_h);
    lv_obj_set_pos(battery_body, body_x, L.batt_y);
    lv_obj_set_style_radius(battery_body, L.batt_h / 3, 0);
    lv_obj_set_style_border_width(battery_body, BATT_BORDER_W, 0);
    // Quiet outline, loud fill: the charge level should carry the colour, not
    // the housing.
    lv_obj_set_style_border_color(battery_body, THEME_DIM, 0);

    // Width is set per update; height and position are fixed.
    battery_fill = lv_obj_create(battery_body);
    lv_obj_remove_style_all(battery_fill);
    lv_obj_clear_flag(battery_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(battery_fill, L.batt_h - 2 * BATT_BORDER_W);
    lv_obj_align(battery_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(battery_fill, (L.batt_h / 3) - BATT_BORDER_W, 0);
    lv_obj_set_style_bg_opa(battery_fill, BATT_FILL_OPA, 0);

    battery_nub = lv_obj_create(parent);
    lv_obj_remove_style_all(battery_nub);
    lv_obj_clear_flag(battery_nub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(battery_nub, L.batt_nub_w, L.batt_nub_h);
    lv_obj_set_pos(battery_nub, body_x + L.batt_w + BATT_NUB_GAP,
                   L.batt_y + (L.batt_h - L.batt_nub_h) / 2);
    lv_obj_set_style_radius(battery_nub, 1, 0);
    lv_obj_set_style_bg_opa(battery_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(battery_nub, THEME_DIM, 0);

    // Inside the body on large screens, beside it on small ones where the
    // interior is too short for any legible font. ui_update_battery() places
    // the outside variant, because its width changes with the digit count.
    // Inside the body the number is set in a faux bold: the same glyphs drawn
    // twice, one pixel apart. Styrene ships here in a single weight
    // (assets/StyreneB-Regular.otf), so a real bold cut would mean generating
    // and embedding a second font for three digits. Doubling thickens the
    // strokes enough to read against the fill, which is the whole point, and
    // lets the size come down a step so the number stops crowding the outline.
    battery_lbl = lv_label_create(L.batt_inside ? battery_body : parent);
    lv_obj_set_style_text_font(battery_lbl, L.batt_font, 0);
    lv_obj_set_style_text_color(battery_lbl, L.batt_inside ? THEME_TEXT : THEME_DIM, 0);
    lv_label_set_text(battery_lbl, "");
    if (L.batt_inside) {
        // Nicht lv_obj_center(): Das zentriert den Textkasten, nicht die
        // Ziffern. Der Kasten ist 31 px hoch, die Ziffern belegen davon nur
        // 17 px, und sie sitzen darin nicht mittig.
        //
        // Der Wert ist am gerenderten Bild ausgemessen, nicht aus den
        // Schriftmassen hergeleitet: Innenraum 30 px, Tinte 17 px hoch mit
        // 11 px Luft oben und 2 px unten. Mein erster Versuch rechnete mit
        // base_line und verschob in die FALSCHE Richtung — Timo hat es
        // gesehen, bevor ich es nachgemessen hatte.
        lv_obj_align(battery_lbl, LV_ALIGN_CENTER, 0, BATT_ZAHL_Y_KORR);
    }

    // One line under the battery: the charge symbol while on the cable, an
    // estimated time left otherwise. Empty while neither applies — see
    // battery_runtime.h on why an unknown estimate stays blank.
    battery_sub_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(battery_sub_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(battery_sub_lbl, THEME_DIM, 0);
    // Mittig unter der Batterie, nicht rechtsbuendig: Timo, 2026-09-25, "die
    // restlaufzeit soll direkt unter der batterie stehen". Rechtsbuendig sah
    // sie nach links verrutscht aus, weil das Etikett breiter ist als der
    // Koerper — breiter muss es sein, sonst bricht "ca. 200 min" um.
    lv_obj_set_style_text_align(battery_sub_lbl, LV_TEXT_ALIGN_CENTER, 0);
    // Senkrecht in die Mitte zwischen Batterieunterkante und Oberkante des
    // ersten Blocks. Timo, 2026-09-25: "genau mittig von der Hoehe zwischen
    // der Batterie und dem naechsten Block." Ein fester Abstand von drei
    // Pixeln klebte die Zeile an die Batterie und liess darunter ein Loch.
    const int16_t batt_unten = L.batt_y + L.batt_h;
    const int16_t luecke     = L.content_y - batt_unten;
    const int16_t sub_h      = lv_font_get_line_height(&lv_font_montserrat_14);
    // Die Breite ergibt sich aus dem Platz rechts der Mitte: Ein mittiges
    // Etikett kann hoechstens doppelt so breit sein wie der Abstand seiner
    // Mitte zum Bildrand — sonst laeuft es hinaus und wird abgeschnitten.
    // Gemessen (480x480): Mitte bei 423, also 114 Pixel; "ca. 200 min" und
    // die Ladezeile passen darin.
    const int16_t mitte_x = body_x + total_w / 2;
    const int16_t sub_w   = 2 * (L.scr_w - mitte_x);
    lv_obj_set_width(battery_sub_lbl, sub_w);
    lv_obj_set_pos(battery_sub_lbl, mitte_x - sub_w / 2,
                   batt_unten + (luecke - sub_h) / 2);
    lv_label_set_long_mode(battery_sub_lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(battery_sub_lbl, "");
}


// ======== Usage Screen ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y,
                        L.content_w - 2 * L.panel_pad_x, L.bar_h);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.reset_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);

    return panel;
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, L.pair_y1);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, L.pair_y2);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, L.pair_y3);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

// Idle "Zzz" screen — shown when the host is connected but no usage update has
// landed recently (token expired, daemon down, host asleep…). Full-screen, like
// the pairing hint, so we never render hours-old numbers as if they were live.
static void build_idle_group(lv_obj_t* parent) {
    idle_group = lv_obj_create(parent);
    lv_obj_set_size(idle_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(idle_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_group, 0, 0);
    lv_obj_set_style_pad_all(idle_group, 0, 0);
    lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // A shrunk-down resting creature (the official cloud-ride animation)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "cloud", L.idle_px);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);
    mini_kreatur = creature;

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

// Haengt die eine Mini-Kreatur dorthin, wo sie gerade gebraucht wird.
// Das Umhaengen selbst passiert nur bei echtem Wechsel; das Ausrichten ist
// billig genug, um es jedes Mal zu tun.
static void mini_kreatur_zeigen(lv_obj_t* ziel, int16_t y_versatz) {
    if (!mini_kreatur || !ziel) return;
    if (lv_obj_get_parent(mini_kreatur) != ziel) {
        lv_obj_set_parent(mini_kreatur, ziel);
    }
    lv_obj_align(mini_kreatur, LV_ALIGN_CENTER, 0, y_versatz);
    lv_obj_clear_flag(mini_kreatur, LV_OBJ_FLAG_HIDDEN);
}

// One column of the counts panel: a big number over a quiet caption.
static lv_obj_t* make_session_count(lv_obj_t* parent, int16_t x, int16_t w,
                                    const char* caption, lv_color_t colour) {
    lv_obj_t* zahl = lv_label_create(parent);
    lv_obj_set_style_text_font(zahl, L.sess_count_font, 0);
    lv_obj_set_style_text_color(zahl, colour, 0);
    lv_obj_set_style_text_align(zahl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(zahl, w);
    lv_obj_set_pos(zahl, x, 0);
    lv_label_set_text(zahl, "-");

    lv_obj_t* text = lv_label_create(parent);
    lv_obj_set_style_text_font(text, L.sess_caption_font, 0);
    lv_obj_set_style_text_color(text, COL_DIM, 0);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(text, w);
    lv_label_set_text(text, caption);
    lv_obj_align_to(text, zahl, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    return zahl;
}

static void init_sessions_screen(lv_obj_t* scr) {
    sessions_container = lv_obj_create(scr);
    lv_obj_set_size(sessions_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(sessions_container, 0, 0);
    lv_obj_set_style_bg_opa(sessions_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sessions_container, 0, 0);
    lv_obj_set_style_pad_all(sessions_container, 0, 0);
    lv_obj_clear_flag(sessions_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(sessions_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* titel = lv_label_create(sessions_container);
    sess_title_lbl = titel;
    lv_label_set_text(titel, "Sessions");
    lv_obj_set_style_text_font(titel, L.title_font, 0);
    lv_obj_set_style_text_color(titel, COL_TEXT, 0);
    lv_obj_align(titel, LV_ALIGN_TOP_MID, L.title_nudge, L.title_y);

    // Counts panel. Running carries the accent, because that is the number the
    // page is about — the names below it are exactly those sessions.
    //
    // Timo, 2026-09-27: "möchte anstatt die waiting zahl was anderes haben. Die
    // running zahl soll orange werden. Total möchte ich, das gut."
    //
    // Why "Waiting" had to go: it was never a sharp state. Claude Code calls
    // EVERY idle session that, so with 29 sessions open it counted almost all
    // of them — measured 2026-09-25, see the vault note. "Total" and "Idle" are
    // both exact, and together with Running the row adds up again: a glance
    // tells him how many sessions are open and how many of them are working.
    // Height from the content, not from the usage screen's panel height —
    // the number plus its caption is barely half of that, and the leftover
    // read as a broken panel.
    const int16_t zahlen_h = L.sess_count_font->line_height + 2
                           + L.pace_font->line_height + 2 * L.panel_pad_y;
    lv_obj_t* zahlen = make_panel(sessions_container, L.margin, L.content_y,
                                  L.content_w, zahlen_h);
    sess_counts_panel = zahlen;
    const int16_t spalte = (L.content_w - 2 * L.panel_pad_x) / 3;
    const char* beschriftung[3] = { "Total", "Running", "Idle" };
    const lv_color_t farbe[3]   = { COL_TEXT, COL_ACCENT, COL_DIM };
    for (int i = 0; i < 3; i++) {
        sess_count_lbl[i] = make_session_count(zahlen, spalte * i, spalte,
                                               beschriftung[i], farbe[i]);
    }

    // The waiting sessions, in a card of their own — the same rounded panel
    // the usage screen uses, so the page reads as one design rather than a
    // stat block with loose text under it. Only the waiting ones are listed:
    // running and parked sessions ask nothing of anyone.
    const int16_t liste_y = L.content_y + zahlen_h + L.usage_panel_gap;
    const int16_t erste_zeile = L.sess_caption_font->line_height + 6;

    // Die Zeilenhoehe ergibt sich aus der Karte, nicht aus der Schrift.
    //
    // Timo, 2026-09-25: "mach die so gross, dass 4 Stueck genau hinpassen,
    // plus der Hinweis +xx more." Also nicht so viele Zeilen wie moeglich,
    // sondern genau vier, die den Platz ausfuellen. Eine Zeile bleibt fuer
    // "+N more" reserviert — die will er ausdruecklich sehen.
    //
    // Das muss VOR dem Erzeugen der Etiketten stehen: Sie bekommen ihre
    // Position einmal beim Aufbau, eine spaeter geaenderte Hoehe erreicht sie
    // nicht mehr.
    sess_list_max_h = L.scr_h - liste_y - L.margin;
    sess_max_zeilen = SESS_ZEILEN_WUNSCH;
    // Timo, 2026-09-25: "bisschen mehr abstand zum +xx more, die abstaende
    // zwischen den sessions einfach verringern." Die Zeilen stehen deshalb
    // dicht beieinander, statt den Platz gleichmaessig zu fuellen — was
    // uebrig bleibt, wird zur Luft vor der Fussnote. Die sitzt ohnehin am
    // unteren Rand der Karte.
    const int16_t zeile_h = L.sess_name_font->line_height + 1;

    sess_list_panel = make_panel(sessions_container, L.margin, liste_y,
                                 L.content_w, zeile_h * 2);

    sess_list_caption = lv_label_create(sess_list_panel);
    lv_obj_set_style_text_font(sess_list_caption, L.sess_caption_font, 0);
    lv_obj_set_style_text_color(sess_list_caption, COL_DIM, 0);
    lv_label_set_text(sess_list_caption, "Running now");
    lv_obj_set_pos(sess_list_caption, 0, 0);

    for (int i = 0; i < SESSIONS_MAX_NAMES; i++) {
        sess_name_lbl[i] = lv_label_create(sess_list_panel);
        lv_obj_set_style_text_font(sess_name_lbl[i], L.sess_name_font, 0);
        lv_obj_set_style_text_color(sess_name_lbl[i], COL_TEXT, 0);
        lv_label_set_long_mode(sess_name_lbl[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(sess_name_lbl[i], L.content_w - 2 * L.panel_pad_x);
        // Feste Hoehe von genau einer Zeile: Ohne sie waechst das Etikett in
        // die Hoehe, statt zu kuerzen — lange Namen brachen um und liefen in
        // die naechste Zeile hinein. Mit fester Hoehe greift LONG_DOT.
        lv_obj_set_height(sess_name_lbl[i], L.sess_name_font->line_height);
        lv_obj_set_pos(sess_name_lbl[i], 0, erste_zeile + i * zeile_h);
        lv_label_set_text(sess_name_lbl[i], "");
        lv_obj_add_flag(sess_name_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }

    sess_more_lbl = lv_label_create(sess_list_panel);
    lv_obj_set_style_text_font(sess_more_lbl, L.sess_caption_font, 0);
    lv_obj_set_style_text_color(sess_more_lbl, COL_DIM, 0);
    lv_obj_set_pos(sess_more_lbl, 0, erste_zeile);
    lv_label_set_text(sess_more_lbl, "");
    lv_obj_add_flag(sess_more_lbl, LV_OBJ_FLAG_HIDDEN);

    // Row geometry the update pass needs to resize the card to its content.
    sess_row_h = zeile_h;
    sess_first_row_y = erste_zeile;

    // Shown instead of three zeros when nothing has arrived: "0 waiting" and
    // "nothing known" look identical otherwise, and mean opposite things.
    // Lives inside the same card, at the same inset as the names — a bare
    // sentence floating where a panel belongs looks like a rendering fault.
    sess_hint_lbl = lv_label_create(sess_list_panel);
    lv_obj_set_style_text_font(sess_hint_lbl, L.reset_font, 0);
    lv_obj_set_style_text_color(sess_hint_lbl, COL_DIM, 0);
    lv_obj_set_pos(sess_hint_lbl, 0, erste_zeile);
    lv_label_set_text(sess_hint_lbl, "No connection");

    lv_obj_add_flag(sessions_container, LV_OBJ_FLAG_HIDDEN);
}

// Der letzte bekannte Stand gilt eine Weile weiter. Erst wenn laenger nichts
// kam, ist "No data" die Wahrheit statt eines Schreckens bei jedem
// Serverhaenger. Eigene Funktion, weil der Takt diese Frage beantworten muss,
// ohne dafuer die halbe Seite neu zu schreiben.
static bool sessionsdaten_frisch(const UsageData* d) {
    return d->sessions_valid
        && (lv_tick_get() - (uint32_t)d->sessions_last_ms) < SESSIONS_STALE_MS;
}

static void update_sessions_screen(const UsageData* d) {
    if (!sessions_container) return;

    const bool sessions_bekannt = sessionsdaten_frisch(d);

    sessionseite_leer = !sessions_bekannt;

    if (!sessions_bekannt) {
        // Genau der Aufbau der Usage-Seite im selben Fall: keine Karten,
        // sondern die schlafende Kreatur mittig auf dem Seitenhintergrund.
        // Timo, 2026-09-27: "da soll dieselbe animation wie beim usage screen
        // sein."
        //
        // Die Karten MUESSEN dafuer weichen, und das ist kein Geschmack: Die
        // Kreatur haengt in einer Leinwand mit echtem Schwarz als Hintergrund.
        // Auf dem Seitenhintergrund faellt das nicht auf, in der helleren
        // Karte stand ein sichtbarer schwarzer Kasten um sie herum.
        lv_obj_add_flag(sess_counts_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sess_list_panel, LV_OBJ_FLAG_HIDDEN);

        // Mitte des Inhaltsbereichs, nicht des Schirms: oben steht die
        // Kopfzeile mit Uhr und Batterie, und die bleibt sichtbar.
        const int16_t mitte = L.content_y / 2;
        mini_kreatur_zeigen(sessions_container, mitte - 18);

        // Genau sagen, was fehlt: ohne Funk ist es die Verbindung zum Rechner,
        // mit Funk der Server dahinter. Beides "No connection" zu nennen,
        // schickt bei der Fehlersuche in die falsche Richtung.
        //
        // "No data" statt "No session data": dasselbe Wort wie die Usage-Seite
        // im selben Fall. Timo, 2026-09-28: "wenn keine Daten vom Server
        // kommen, dann auch No Data anzeigen."
        if (lv_obj_get_parent(sess_hint_lbl) != sessions_container) {
            lv_obj_set_parent(sess_hint_lbl, sessions_container);
        }
        lv_label_set_text(sess_hint_lbl,
                          s_ble_connected ? "No data" : "No connection");
        lv_obj_set_style_text_align(sess_hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(sess_hint_lbl, LV_ALIGN_CENTER, 0, mitte + L.idle_px / 2);
        lv_obj_clear_flag(sess_hint_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(sess_counts_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(sess_list_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sess_hint_lbl, LV_OBJ_FLAG_HIDDEN);
    // Die Kreatur gehoert zurueck auf die Usage-Seite, sonst steht sie ueber
    // der Liste, die gleich wieder Namen zeigt.
    if (mini_kreatur && lv_obj_get_parent(mini_kreatur) == sessions_container) {
        lv_obj_set_parent(mini_kreatur, idle_group);
        lv_obj_align(mini_kreatur, LV_ALIGN_CENTER, 0, -20);
        lv_obj_add_flag(mini_kreatur, LV_OBJ_FLAG_HIDDEN);
    }
    // Der Hinweis ebenso zurueck in die Karte. Der Leerzustand haengt ihn auf
    // die ganze Seite um; blieb er dort, zentrierte sich "Nothing running"
    // danach auf den BILDSCHIRM statt auf die Karte — und die Bildschirmmitte
    // liegt genau an deren Oberkante. Traf jeden Start, weil das Geraet
    // anfangs immer kurz ohne Sessiondaten ist. Timo, 2026-09-28: "das
    // Nothing Running ist nicht zentriert im Block, das ist einfach ganz oben."
    if (lv_obj_get_parent(sess_hint_lbl) != sess_list_panel) {
        lv_obj_set_parent(sess_hint_lbl, sess_list_panel);
    }
    lv_obj_clear_flag(sess_list_caption, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(sess_list_panel, LV_OBJ_FLAG_HIDDEN);
    // Total und Idle werden hier gerechnet, nicht gefunkt: Die Summe der VIER
    // gefunkten Zahlen IST die Gesamtzahl offener Sessions, und "untaetig" ist
    // genau das, was weder arbeitet noch im Hintergrund laeuft. Zusaetzliche
    // Felder auf der Leitung waeren dieselbe Information zum Preis von Namen
    // in der Liste — der Schreibvorgang ist auf 244 Byte begrenzt.
    //
    // "Running" zaehlt Arbeitende UND Hintergrund-Sessions zusammen. Timo,
    // 2026-09-30: Sessions mit Hintergrundaufgaben sollen als laufend
    // zaehlen, aber in der Namensliste sichtbar abgesetzt bleiben (gedimmt) —
    // siehe die Farbwahl unten.
    const int gesamt = d->sessions_waiting + d->sessions_working
                     + d->sessions_parked + d->sessions_background;
    const int laufend = d->sessions_working + d->sessions_background;
    lv_label_set_text_fmt(sess_count_lbl[0], "%d", gesamt);
    lv_label_set_text_fmt(sess_count_lbl[1], "%d", laufend);
    lv_label_set_text_fmt(sess_count_lbl[2], "%d", gesamt - laufend);

    // Was nicht auf den Schirm passt, wird gezaehlt statt abgeschnitten —
    // sonst behauptet die Liste Vollstaendigkeit, die sie nicht hat.
    const int gezeigt = (d->sessions_name_count < sess_max_zeilen)
                        ? d->sessions_name_count : sess_max_zeilen;
    const int zusaetzlich_verborgen = d->sessions_name_count - gezeigt;

    // Die Namen kommen bereits geordnet an — erst die arbeitenden, dann die im
    // Hintergrund (sessions_source.py::_laufende_namen). Eintrag i gehoert
    // also genau dann zum Hintergrund, wenn i >= sessions_working. Die Farbe
    // wird bei JEDEM Update neu gesetzt, nicht nur beim Aufbau: Die Etiketten
    // werden wiederverwendet, und ohne das wuerde ein Name, der von
    // Hintergrund zu arbeitend wechselt, gedimmt stehen bleiben.
    for (int i = 0; i < SESSIONS_MAX_NAMES; i++) {
        if (i < gezeigt) {
            lv_label_set_text(sess_name_lbl[i], d->sessions_names[i]);
            lv_obj_set_style_text_color(sess_name_lbl[i],
                i >= d->sessions_working ? COL_DIM : COL_TEXT, 0);
            lv_obj_clear_flag(sess_name_lbl[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(sess_name_lbl[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Verbunden, aber nichts laeuft: Das ist eine Aussage und kein Fehler.
    // Eine leere Karte sieht dagegen kaputt aus.
    if (gezeigt == 0 && d->sessions_hidden == 0) {
        lv_obj_add_flag(sess_more_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sess_list_caption, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(sess_list_panel, sess_list_max_h);
        lv_label_set_text(sess_hint_lbl, "Nothing running");
        lv_obj_align(sess_hint_lbl, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(sess_hint_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const int verborgen = d->sessions_hidden + zusaetzlich_verborgen;
    int zeilen = gezeigt;
    if (verborgen > 0) {
        // Unten in der Karte, nicht direkt unter der letzten Zeile: Die Karte
        // reicht ohnehin bis zum unteren Rand, und "+N more" ist eine Fussnote
        // zur ganzen Liste, kein weiterer Eintrag. Timo, 2026-09-25: "das +xx
        // more soll ganz unten auf dem screen sein".
        lv_label_set_text_fmt(sess_more_lbl, "+%d more", verborgen);
        lv_obj_align(sess_more_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_clear_flag(sess_more_lbl, LV_OBJ_FLAG_HIDDEN);
        zeilen++;
    } else {
        lv_obj_add_flag(sess_more_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    // With names present the card runs to the bottom margin instead of hugging
    // its content: three lines in a card that stops after three lines leaves a
    // ragged gap under it, and the page has nothing else to put there.
    if (zeilen == 0) {
        lv_obj_add_flag(sess_list_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_height(sess_list_panel, sess_list_max_h);
    }
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, L.title_font, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    // The nudge balances the corner logo on the left; smaller on small
    // screens where the logo is 40px and the battery icon sits closer.
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, L.title_nudge, L.title_y);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_session = make_usage_panel(usage_group, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);

    // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "of your monthly budget");
    lv_obj_set_style_text_font(lbl_spending_desc, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
    lv_obj_set_pos(lbl_spending_desc, 0, L.usage_reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_status, "");
    lv_obj_set_style_text_font(lbl_spending_status, L.pace_font, 0);
    lv_obj_set_pos(lbl_spending_status, 0, L.usage_reset_y + 20);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(usage_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    // Recolor enabled so enterprise period box can color pace and reset separately
    lv_label_set_recolor(lbl_weekly_reset, true);

    build_pair_group(usage_container);
    build_idle_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, L.anim_font, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, L.anim_y);

    // Same slot as the animated status line — one or the other is visible,
    // never both. Quieter than the accent line it replaces: this is a figure
    // to glance at, not something asking for attention.
    lbl_scoped = lv_label_create(usage_container);
    lv_label_set_text(lbl_scoped, "");
    // Ein kleiner Ring links der Quote, dessen gefuellter Bogen den Prozentwert
    // zeigt. Ein Balken waere hier die dritte gleiche Form auf der Seite; der
    // Ring unterscheidet die Modellquote vom gemeinsamen Topf darueber, ohne
    // eine eigene Zeile zu brauchen.
    scoped_ring = lv_arc_create(usage_container);
    lv_obj_remove_style_all(scoped_ring);
    lv_obj_set_size(scoped_ring, SCOPED_RING_PX, SCOPED_RING_PX);
    lv_arc_set_rotation(scoped_ring, 270);          // Start oben, im Uhrzeigersinn
    lv_arc_set_bg_angles(scoped_ring, 0, 360);
    lv_arc_set_range(scoped_ring, 0, 100);
    lv_obj_remove_flag(scoped_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(scoped_ring, SCOPED_RING_DICKE, LV_PART_MAIN);
    lv_obj_set_style_arc_width(scoped_ring, SCOPED_RING_DICKE, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(scoped_ring, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(scoped_ring, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scoped_ring, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_add_flag(scoped_ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(lbl_scoped, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_scoped, COL_DIM, 0);
    lv_obj_align(lbl_scoped, LV_ALIGN_BOTTOM_MID, 0, L.anim_y);
    lv_obj_add_flag(lbl_scoped, LV_OBJ_FLAG_HIDDEN);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

#ifndef BOARD_HAS_PSRAM
    // Static corner mascot (see clawd_still.h) — the animated one needs PSRAM.
    if (L.small_icons) init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_SMALL_W, CLAWD_STILL_SMALL_H, clawd_still_small_data);
    else               init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_W, CLAWD_STILL_H, clawd_still_data);
#endif

    init_usage_screen(scr);
    init_sessions_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Corner mascot in the old logo slot. The still Clawd is shorter than the
    // 80/40 px slot the spark logo used; center it vertically in that slot.
    {
        const int slot  = L.small_icons ? LOGO_SMALL_HEIGHT : LOGO_HEIGHT;
        const int art_h = L.small_icons ? CLAWD_STILL_SMALL_H : CLAWD_STILL_H;
        const int top   = L.logo_y + (slot - art_h) / 2;
#ifdef BOARD_HAS_PSRAM
        // Animated: idles, does acts, and takes walk-off/lurk trips.
        splash_mascot_create(scr, L.margin, top + art_h, L.small_icons ? 2 : 3);
#else
        logo_img = lv_image_create(scr);
        lv_image_set_src(logo_img, &logo_dsc);
        lv_obj_set_pos(logo_img, L.margin, top);
#endif
    }

    battery_create(scr);
    rate_laden();
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    // A real number beats a decorative status line: when the account has a
    // per-model weekly quota, it takes that slot instead.
    if (lbl_scoped) {
        if (data->scoped_valid) {
            lv_label_set_text_fmt(lbl_scoped, "%s  %d%%", data->scoped_name, data->scoped_pct);
            lv_obj_clear_flag(lbl_scoped, LV_OBJ_FLAG_HIDDEN);
            if (scoped_ring) {
                // Gleiche Schwellen wie die Balken: der Ring sagt auf einen
                // Blick, wie es um die Quote steht, nicht nur dass es sie gibt.
                lv_arc_set_value(scoped_ring, data->scoped_pct);
                lv_obj_set_style_arc_color(scoped_ring,
                                           pct_color((float)data->scoped_pct),
                                           LV_PART_INDICATOR);
                lv_obj_clear_flag(scoped_ring, LV_OBJ_FLAG_HIDDEN);
                // Erst das Etikett ausmessen lassen, sonst richtet sich der
                // Ring nach einer Breite von null aus und sitzt daneben.
                // Die Feinkorrektur nach unten gleicht aus, dass die Ziffern
                // ihre Grundlinie oberhalb der Zeilenmitte haben — ohne sie
                // schwebt der Ring sichtbar zu hoch.
                lv_obj_update_layout(lbl_scoped);
                lv_obj_align_to(scoped_ring, lbl_scoped,
                                LV_ALIGN_OUT_LEFT_MID, -12, SCOPED_RING_Y_KORR);
            }
            if (lbl_anim) lv_obj_add_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_scoped, LV_OBJ_FLAG_HIDDEN);
            if (scoped_ring) lv_obj_add_flag(scoped_ring, LV_OBJ_FLAG_HIDDEN);
            if (lbl_anim) lv_obj_clear_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
        }
    }
    letzte_daten = *data;
    update_sessions_screen(data);
    data_ok = data->ok;
    if (!data->ok) return;          // a {"ok":false} "no data" beat → fall through to idle, keep last numbers
    last_data_ms = lv_tick_get();   // a real usage update just landed
    data_received = true;

    if (data->clock_epoch > 0) {    // daemon supplied wall-clock time → drive the title clock
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = data->clock_fmt;
    } else if (clock_base_epoch != 0) {   // clock turned off daemon-side → revert title to "Usage"
        clock_base_epoch = 0;
        clock_last_min = -1;
        lv_label_set_text(lbl_title, "Usage");
    }

    int s_pct = (int)(data->session_pct + 0.5f);

    if (data->enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc + pace
        lv_obj_set_style_text_font(lbl_session_pct, L.ent_pct_font, 0);
        lv_label_set_text(lbl_session_label, "Spending");
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_font(lbl_session_pct, L.pct_font, 0);
        lv_label_set_text(lbl_session_label, "Current");
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[48];

    // Pace vars used in both enterprise blocks below
    const char* pace_text = "Under pace";
    lv_color_t  pace_color = COL_GREEN;
    const char* pace_hex   = "788c5d";   // matches THEME_GREEN
    if (data->session_pct > (float)data->time_pct + 15.0f) {
        pace_text = "Over pace";  pace_color = COL_RED;   pace_hex = "c0392b";
    } else if (data->session_pct > (float)data->time_pct - 15.0f) {
        pace_text = "On pace";    pace_color = COL_AMBER; pace_hex = "d97757";
    }

    if (data->enterprise) {
        lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
        lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                        LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    } else {
        lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
        format_reset_time(data->session_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_session_reset, buf);
    }

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    if (data->enterprise) {
        // Period box: time % + dynamic pace color + "Resets <date>" label
        lv_label_set_text(lbl_weekly_label, "Period");
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", data->time_pct);
        lv_bar_set_value(bar_weekly, data->time_pct, LV_ANIM_ON);
        lv_color_t bar_pace = (data->session_pct <= (float)data->time_pct) ? COL_GREEN :
                              (data->session_pct <= (float)data->time_pct + 15.0f) ? COL_AMBER :
                              COL_RED;
        lv_obj_set_style_bg_color(bar_weekly, bar_pace, LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "#%s %s# - #faf9f5 Resets %s#",
                 pace_hex, pace_text, data->reset_date);
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        int w_pct = (int)(data->weekly_pct + 0.5f);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);
        format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_weekly_reset, buf);
    }
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group) return;
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && data_ok && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // idle / Zzz
    }
    if (v == view_state) return;
    view_state = v;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group : usage_group,
                      LV_OBJ_FLAG_HIDDEN);
}

// Setzt beide Ueberschriften. Timo, 2026-09-25: "anstatt die ueberschriften
// bitte einfach die Uhrzeit." Beide, nicht nur die der Usage-Seite — sonst
// stuende auf der einen Seite die Zeit und auf der anderen ein Wort.
static void titel_setzen(const char* text) {
    if (lbl_title)      lv_label_set_text(lbl_title, text);
    if (sess_title_lbl) lv_label_set_text(sess_title_lbl, text);
}

// Die Uhr laeuft zwischen zwei Nutzlasten lokal weiter, damit die Minute
// umspringt, ohne auf den Daemon zu warten.
static void uhr_tick(void) {
    if (clock_base_epoch <= 0) return;
    const uint32_t now = lv_tick_get();
    time_t cur = (time_t)(clock_base_epoch + (now - clock_base_ms) / 1000);
    struct tm tmv;
    gmtime_r(&cur, &tmv);   // epoch ist bereits Ortszeit
    if (tmv.tm_min == clock_last_min) return;
    clock_last_min = tmv.tm_min;
    char tbuf[12];
    if (clock_fmt == 12) {
        int h12 = tmv.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", h12, tmv.tm_min,
                 tmv.tm_hour < 12 ? "AM" : "PM");
    } else {
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    }
    titel_setzen(tbuf);
    // Auch auf dem Clawd-Bildschirm, dort als Pixel-Art in der Ecke.
    splash_set_clock(tbuf);
}

void ui_tick_anim(void) {
    uhr_tick();

    // Die Sessionseite wird vom Takt gefuehrt, nicht von der Nutzlast. Sonst
    // veraltet sie stumm: main.cpp ruft ui_update() nur, wenn etwas ankommt —
    // und wenn nichts ankommt, ist genau das die Nachricht, die auf den Schirm
    // gehoert. letzte_daten ist beim Start genullt, sessions_valid also false,
    // und damit stimmt die Anzeige schon vor der ersten Nutzlast.
    if (current_screen == SCREEN_SESSIONS) {
        // Nur beim Umschlagen neu aufbauen, nicht bei jedem Bild: LVGL
        // verwirft bei jedem lv_label_set_text den Bereich, und ein Vollbild
        // kostet auf dem C6 rund 120 ms (gemessen 2026-09-25). Die Frage
        // selbst ist billig, das Neuzeichnen nicht.
        if (sessionsdaten_frisch(&letzte_daten) == sessionseite_leer) {
            update_sessions_screen(&letzte_daten);
        }
        if (sessionseite_leer) splash_mini_tick();
        return;
    }

    if (current_screen != SCREEN_USAGE) return;
    update_view_state();
    if (view_state == 1) {
        mini_kreatur_zeigen(idle_group, -20);  // zurueck, falls sie bei den Sessions war
        splash_mini_tick();                    // animate the sleeping creature on the idle screen
    }

    uint32_t now = lv_tick_get();


    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
    } else if (view_state == 1) {      // idle — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
// Die gemessenen Verbrauchsraten ueberleben Neustart und Kabel, indem sie im
// NVS liegen — derselbe Bereich wie die Helligkeit. Je Helligkeitsstufe ein
// Schluessel, weil der Bildschirm der groesste Verbraucher ist. Geschrieben wird
// nur bei einer neuen Messung: Flash hat endlich viele Schreibzyklen, und eine
// Rate aendert sich hoechstens alle paar Minuten.
//
// Der fruehere Einzelschluessel "battrate" wird nicht mehr gelesen: Er wurde
// auch am Kabel bei vollem Akku (PMU meldet dort "laedt nicht") gemessen und
// ist verfaelscht: Der NVS-Auszug vom 2026-09-30 zeigte 5846863 ms/%, rund 54-mal
// die Annahme. Das remove laeuft bei jedem Start und ist ohne den Schluessel
// wirkungslos.
static const char* const RATE_SCHLUESSEL[BATTERY_RUNTIME_STAGES] = {
    "battrate0", "battrate1", "battrate2", "battrate3"
};
#define RATE_SCHLUESSEL_ALT "battrate"

static uint32_t rate_zuletzt_gesichert[BATTERY_RUNTIME_STAGES] = {0, 0, 0, 0};

static void rate_laden(void) {
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    for (int i = 0; i < BATTERY_RUNTIME_STAGES; i++) {
        const uint32_t rate = prefs.getULong(RATE_SCHLUESSEL[i], 0);
        battery_runtime_set_rate(i, rate);
        rate_zuletzt_gesichert[i] = rate;
    }
    prefs.remove(RATE_SCHLUESSEL_ALT);   // fehlt der Schluessel, ist das folgenlos
    prefs.end();
}

// Meldet der Schaetzer eine frische Messung, wird sie protokolliert und die
// betroffene Stufe gesichert. Serial ist die einzige Stelle, an der man von
// aussen sieht, was das Geraet gemessen hat.
static void rate_sichern_wenn_geaendert(void) {
    int      stufe;
    uint32_t rate, spanne_ms;
    int      abfall;
    if (!battery_runtime_neue_messung(&stufe, &rate, &spanne_ms, &abfall)) return;

    Serial.printf("Akku-Rate gemessen: Stufe %d, %lu ms/%%, Spanne %lu ms, Abfall %d %%\n",
                  stufe, (unsigned long)rate, (unsigned long)spanne_ms, abfall);

    if (rate == rate_zuletzt_gesichert[stufe]) return;
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putULong(RATE_SCHLUESSEL[stufe], rate);
    prefs.end();
    rate_zuletzt_gesichert[stufe] = rate;
}

static void apply_battery_visibility(void) {
    if (!battery_body) return;
    // On the splash the whole indicator gets out of the way of the artwork.
    const bool hide = (current_screen == SCREEN_SPLASH);
    lv_obj_t* teile[] = { battery_body, battery_nub, battery_lbl,
                          battery_sub_lbl };
    for (lv_obj_t* teil : teile) {
        if (!teil) continue;
        if (hide) lv_obj_add_flag(teil, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_clear_flag(teil, LV_OBJ_FLAG_HIDDEN);
    }
    // An unknown charge (-1) leaves the number blank; the empty body still
    // shows, so the indicator does not vanish without explanation.
    if (battery_lbl && lv_label_get_text(battery_lbl)[0] == '\0') {
        lv_obj_add_flag(battery_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

// Tapping the panel used to flip between splash and usage. The side buttons
// own page turns now, so the tap took over the two actions the PWR short-press
// used to carry — kept rather than dropped, just moved to where they fit the
// page you are looking at.
static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_SPLASH) {
        splash_next();
        // LVGL processes this click event and may repaint the container over
        // the directly-drawn picture; ask for a full redraw afterwards.
        splash_force_repaint();
    } else {
        brightness_cycle();
    }
}

// Die neue Seite blendet auf, statt hereinzufahren.
//
// Der Schub ueber die volle Breite war auf dem Geraet sichtbar hakelig, und
// das ist kein Einstellungsfehler: Der C6 hat kein PSRAM, LVGL zeichnet in
// schmalen Streifen, und ein Inhalt, der sich ueber 480 Pixel bewegt, faellt
// bei jedem Bild komplett neu an. Timo, 2026-09-25: "das ist ja mega kacke
// hakelig. wenns nicht anders geht, die animation von ganz am anfang einfach
// reinmachen."
//
// Das Aufblenden aendert nur einen Wert je Bild statt die Geometrie und
// bleibt deshalb ruhig. Wer es ganz ohne will, setzt ui_set_seitenanimation
// auf false — dann schaltet das Geraet hart um, was ehrlicher ist als eine
// stockende Bewegung.
#define SEITENWECHSEL_MS 200

// AUS, und zwar gemessen statt vermutet.
//
// Auf dem Geraet (C6, 480x480, kein PSRAM) am 2026-09-25 ueber drei Wechsel:
//   224 ms, 2 Bilder, groesste Luecke 134 ms
//   244 ms, 2 Bilder, groesste Luecke 125 ms
//   261 ms, 2 Bilder, groesste Luecke 132 ms
// Zum Vergleich der Simulator auf dem Rechner: 33 Bilder in 200 ms, groesste
// Luecke 10 ms.
//
// Zwei Bilder sind keine Animation. Ein vollflaechiges Neuzeichnen kostet hier
// gut 120 ms, und das Aufblenden macht den Wechsel damit nur um diese Zeit
// langsamer, ohne dass etwas fliesst. Der harte Wechsel ist schneller UND
// sieht besser aus.
//
// Die Mechanik bleibt stehen, weil sie auf einem Board mit PSRAM und
// schnellerem Panel tragen koennte — aber sie wird eingeschaltet, wenn sie
// dort gemessen wurde, nicht vorher.
static bool seitenanimation_an = false;

void ui_set_seitenanimation(bool an) { seitenanimation_an = an; }

// Messung des Seitenwechsels. Ob eine Animation "ruckelt", ist eine Frage an
// das Auge — aber die Zahlen dahinter sind messbar, und die PC-Session sieht
// das Display nicht, nur die serielle Konsole. Deshalb misst die Firmware
// selbst: Wie viele Bilder wurden waehrend des Uebergangs gezeichnet, und wie
// gross war die groesste Luecke zwischen zwei Bildern.
//
// Fluessig heisst rund 30 Bilder je Sekunde, also Luecken unter 33 ms. Eine
// groesste Luecke von 100 ms und mehr sieht man als Stocken.
static uint32_t wechsel_start_ms;
static uint32_t wechsel_letztes_bild_ms;
static uint16_t wechsel_bilder;
static uint16_t wechsel_groesste_luecke_ms;
static bool     wechsel_laeuft;

static void wechsel_messung_starten(void) {
    wechsel_start_ms = lv_tick_get();
    wechsel_letztes_bild_ms = wechsel_start_ms;
    wechsel_bilder = 0;
    wechsel_groesste_luecke_ms = 0;
    wechsel_laeuft = true;
}

// Aus der Hauptschleife, einmal je gezeichnetem Bild.
void ui_wechsel_messung_tick(void) {
    if (!wechsel_laeuft) return;
    const uint32_t jetzt = lv_tick_get();
    const uint32_t luecke = jetzt - wechsel_letztes_bild_ms;
    wechsel_letztes_bild_ms = jetzt;
    if (luecke > wechsel_groesste_luecke_ms) {
        wechsel_groesste_luecke_ms = (uint16_t)luecke;
    }
    wechsel_bilder++;

    if (jetzt - wechsel_start_ms < SEITENWECHSEL_MS) return;
    wechsel_laeuft = false;
    Serial.printf("Seitenwechsel: %u ms, %u Bilder, groesste Luecke %u ms\n",
                  (unsigned)(jetzt - wechsel_start_ms),
                  (unsigned)wechsel_bilder,
                  (unsigned)wechsel_groesste_luecke_ms);
}

static void seite_aufblenden(lv_obj_t* container) {
    if (!container) return;
    if (!seitenanimation_an) {
        lv_obj_set_style_opa(container, LV_OPA_COVER, 0);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, container);
    lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
    lv_anim_set_time(&a, SEITENWECHSEL_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
        lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
    });
    lv_anim_start(&a);
    wechsel_messung_starten();
}

void ui_show_screen(screen_t screen) {
    const bool wechsel = (screen != current_screen);

    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    if (sessions_container) lv_obj_add_flag(sessions_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:  splash_show(); break;
    case SCREEN_USAGE:
        lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
        if (wechsel) seite_aufblenden(usage_container);
        break;
    case SCREEN_SESSIONS:
        if (sessions_container) {
            lv_obj_clear_flag(sessions_container, LV_OBJ_FLAG_HIDDEN);
            if (wechsel) seite_aufblenden(sessions_container);
        }
        break;
    default: break;
    }

    splash_mascot_set_visible(screen != SCREEN_SPLASH);
    if (logo_img) {
        if (screen == SCREEN_SPLASH) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_next_screen(void) {
    ui_show_screen((screen_t)((current_screen + 1) % SCREEN_COUNT));
}

void ui_prev_screen(void) {
    ui_show_screen((screen_t)((current_screen + SCREEN_COUNT - 1) % SCREEN_COUNT));
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

void ui_update_battery(int percent, bool charging, bool vbus_in) {
    if (!battery_body) return;

    const int16_t innen = L.batt_w - 2 * BATT_BORDER_W;
    const int pct = percent < 0 ? 0 : (percent > 100 ? 100 : percent);

    // Round up so that 1% still draws a visible sliver rather than nothing.
    int16_t fuellung = (int16_t)((innen * pct + 99) / 100);
    if (pct > 0 && fuellung < 1) fuellung = 1;
    lv_obj_set_width(battery_fill, fuellung);

    lv_color_t farbe = THEME_ACCENT;
    // Timo, 2026-09-25: beim Laden die Akzentfarbe, nicht Gruen. Gruen gehoert
    // hier zu den Nutzungsbalken und hiesse dort "viel Luft" — auf der
    // Batterie sagte es faelschlich dasselbe.
    if (charging)                 farbe = THEME_ACCENT;
    else if (pct <= BATT_LOW_PCT) farbe = THEME_RED;
    lv_obj_set_style_bg_color(battery_fill, farbe, 0);

    // Bildschirm und Helligkeitsstufe liest der Schaetzer hier selbst ein: Sie
    // aendern den Verbrauch genauso wie das Kabel, und main.cpp ruft diese
    // Funktion bei jedem ihrer Wechsel auf.
    battery_runtime_sample(percent, charging, vbus_in, idle_is_asleep(),
                           brightness_get_stage(), lv_tick_get());
    rate_sichern_wenn_geaendert();
    if (battery_sub_lbl) {
        const int rest = battery_runtime_minutes();
        if (charging) {
            lv_label_set_text(battery_sub_lbl, LV_SYMBOL_CHARGE " Charging");
        } else if (vbus_in) {
            // Kabel steckt, Akku ist voll: keine Zahl, denn es wird nichts
            // verbraucht, was man hochrechnen koennte. "On USB" statt
            // "Plugged in": das ragte auf 480 px ueber den 20-px-Rand.
            lv_label_set_text(battery_sub_lbl, LV_SYMBOL_CHARGE " On USB");
        } else if (rest >= 0) {
            // Rounded to the coarseness the estimate deserves: a drain slope
            // from a whole-percent reading cannot justify single minutes.
            // Nur die Zahl und die Einheit. Das "ca." stand vorher davor,
            // aber es ist dort ohnehin klar, dass eine Restlaufzeit geschaetzt
            // ist, und auf einem Tischdisplay zaehlt jedes Zeichen. Timo,
            // 2026-09-25: "lass da ca. weg bei der Schaetzung."
            lv_label_set_text_fmt(battery_sub_lbl, "%d min", rest);
        } else {
            lv_label_set_text(battery_sub_lbl, "");
        }
    }

    if (battery_lbl) {
        if (percent < 0) {
            lv_label_set_text(battery_lbl, "");
        } else if (L.batt_inside) {
            // No percent sign inside — the battery outline already says what
            // the number means, and the glyph would cost a third of the room.
            lv_label_set_text_fmt(battery_lbl, "%d", percent);
        } else {
            lv_label_set_text_fmt(battery_lbl, "%d%%", percent);
            // Re-align on every update: the label width changes with the
            // digit count and lv_obj_align_to() is a one-shot placement.
            lv_obj_align_to(battery_lbl, battery_body,
                            LV_ALIGN_OUT_LEFT_MID, -L.batt_lbl_gap, 0);
        }
    }
    apply_battery_visibility();
}
