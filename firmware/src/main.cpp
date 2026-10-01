#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "data.h"
#include "ui.h"
#include "ble.h"
#include "splash.h"
#include "usage_rate.h"
#include "idle.h"
#include "idle_cfg.h"
#include "brightness.h"
#include "battery_runtime.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"
#include "hal/sound_hal.h"

static UsageData usage = {};

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight and LVGL
//           can't quietly toggle splash<->usage on a black panel.
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    const bool raw_pressed = pressed;

    if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Parse a JSON line into UsageData.
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->chime = doc["c"] | false;   // absent (old daemon / chime off) → stay silent
    const char* acct = doc["acct"] | "pro";
    out->enterprise = (strcmp(acct, "ent") == 0);
    out->time_pct = doc["tp"] | 0;
    out->period_days = doc["pd"] | 30;
    strlcpy(out->reset_date, doc["rd"] | "", sizeof(out->reset_date));
    out->clock_epoch = doc["t"] | 0L;
    out->clock_fmt = doc["tf"] | 24;
    out->ok = doc["ok"] | false;

    // Per-model weekly quota ("Fable 12%"). Optional: only accounts that have
    // such a limit get it, and only a daemon that reads /api/oauth/usage sends
    // it. Absent means unknown, and the UI hides the line rather than showing
    // a zero that would read as "nothing used this week".
    out->scoped_valid = doc["fn"].is<const char*>() && doc["fp"].is<int>();
    if (out->scoped_valid) {
        strlcpy(out->scoped_name, doc["fn"] | "", sizeof(out->scoped_name));
        out->scoped_pct = doc["fp"] | 0;
    }

    // Session fields are optional — the daemon only sends them when it has been
    // pointed at a sessions server. "sw" absent means unknown, which the UI
    // must not render as three zeros.
    //
    // Fehlen sie, bleibt der bisherige Stand stehen, statt ihn zu verwerfen.
    // Grund: Der Daemon laesst die Felder auch dann weg, wenn der Sessions-
    // Server einmal nicht antwortet oder ein Schreibvorgang auf die Fassung
    // ohne Sessions zurueckfaellt. Beides dauert Sekunden — vorher sprang die
    // Anzeige dabei jedes Mal auf "No session data" und behauptete damit
    // etwas Falsches. Wie lange der alte Stand gilt, entscheidet die UI
    // anhand von sessions_last_ms.
    if (doc["sw"].is<int>()) {
        out->sessions_valid = true;
        out->sessions_last_ms = millis();
        out->sessions_waiting = doc["sw"] | 0;
        out->sessions_working = doc["sa"] | 0;
        // "sb" fehlt bei einem aelteren Daemon ohne Hintergrund-Zustand — dann
        // einfach 0, wie jedes andere optionale Feld hier.
        out->sessions_background = doc["sb"] | 0;
        out->sessions_parked  = doc["sg"] | 0;
        out->sessions_hidden  = doc["sx"] | 0;
        out->sessions_name_count = 0;
        for (JsonVariant name : doc["sn"].as<JsonArray>()) {
            if (out->sessions_name_count >= SESSIONS_MAX_NAMES) {
                // More names than the screen holds: count the rest as hidden
                // so the total still adds up to sessions_waiting.
                out->sessions_hidden++;
                continue;
            }
            strlcpy(out->sessions_names[out->sessions_name_count],
                    name.as<const char*>() ? name.as<const char*>() : "",
                    SESSIONS_NAME_LEN);
            out->sessions_name_count++;
        }
    }

    out->valid = true;
    return true;
}

// ---- Serial command buffer ----
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

// Was das Geraet ueber seinen Akku denkt, in einer Zeile — damit sich ohne
// Blick aufs Display auslesen laesst, warum eine Restlaufzeit so aussieht.
static void print_akku_status() {
    const uint32_t jetzt = lv_tick_get();
    Serial.printf("akku: pct=%d charging=%d vbus=%d stufe=%d asleep=%d "
                  "anker_pct=%d anker_alter_s=%lu "
                  "raten_ms=[%lu,%lu,%lu,%lu] angezeigt_min=%d\n",
                  power_hal_battery_pct(), power_hal_is_charging(),
                  power_hal_is_vbus_in(), brightness_get_stage(), idle_is_asleep(),
                  battery_runtime_anker_pct(),
                  (unsigned long)(battery_runtime_anker_alter_ms(jetzt) / 1000u),
                  (unsigned long)battery_runtime_rate(0),
                  (unsigned long)battery_runtime_rate(1),
                  (unsigned long)battery_runtime_rate(2),
                  (unsigned long)battery_runtime_rate(3),
                  battery_runtime_minutes());
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            else if (strcmp(cmd_buf, "buzz") == 0)  sound_hal_play_reset();
            else if (strcmp(cmd_buf, "akku") == 0)  print_akku_status();
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

// ---- Side buttons: tap turns a page, hold sends the HID key ----
// Putting two actions on one button costs BTN_HOLD_MS of latency before Space
// starts. Push-to-talk is a hold gesture anyway, so the delay lands where it
// is least noticed; a page turn fires on release and feels instant.
#define BTN_HOLD_MS 250

struct SideButton {
    InputButton  id;
    uint8_t      hid_key;
    uint8_t      hid_mod;
    void       (*on_tap)(void);
    bool         was;
    bool         hid_active;
    bool         wake_swallowed;
    uint32_t     down_ms;
};

// on_tap for the primary button is assigned in setup(): boards with only one
// side button step forward, so a single button still reaches every page.
static SideButton btn_primary   = { INPUT_BTN_PRIMARY,   0x2C, 0x00, nullptr,        false, false, false, 0 };
static SideButton btn_secondary = { INPUT_BTN_SECONDARY, 0x2B, 0x02, ui_next_screen, false, false, false, 0 };

// ble_keyboard_release() clears the whole HID report, so releasing one button
// while the other is still held has to re-assert that one instead of going
// silent. Only one key is carried at a time — holding both is not a gesture
// this device offers.
static void hid_refresh(void) {
    if (btn_secondary.hid_active)    ble_keyboard_press(btn_secondary.hid_key, btn_secondary.hid_mod);
    else if (btn_primary.hid_active) ble_keyboard_press(btn_primary.hid_key, btn_primary.hid_mod);
    else                             ble_keyboard_release();
}

static void side_button_tick(SideButton& b) {
    const bool     now_held = input_hal_is_held(b.id);
    const uint32_t now      = millis();

    if (now_held && !b.was) {
        // Press edge. A press that wakes the panel is swallowed whole — no
        // page turn, no keystroke — so waking never surprises the user.
        b.down_ms = now;
        b.wake_swallowed = idle_consume_wake_press();
    } else if (!now_held && b.was) {
        if (b.hid_active) {
            b.hid_active = false;
            hid_refresh();
        } else if (!b.wake_swallowed && b.on_tap) {
            b.on_tap();
        }
        b.wake_swallowed = false;
    } else if (now_held && !b.hid_active && !b.wake_swallowed &&
               now - b.down_ms >= BTN_HOLD_MS) {
        b.hid_active = true;
        ble_keyboard_press(b.hid_key, b.hid_mod);
    }
    b.was = now_held;
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    board_init();

    display_hal_init();
    display_hal_begin();
    idle_init();        // takes over panel brightness and starts the idle timer
    brightness_init();  // load the user's saved brightness level and apply via idle

    // Startdiagnose für eine ungeklärte I2C-Meldung
    // (i2cWrite ESP_ERR_INVALID_STATE, ~1007 ms, direkt nach der Helligkeit).
    // Sie tritt sporadisch auf, bleibt folgenlos — Touch und IMU melden danach
    // beide OK — und liess sich aus der Ferne nicht zuordnen. Diese vier Zeilen
    // sagen im Startprotokoll, WELCHER der drei I2C-Teilnehmer sie auslöst.
    // Dürfen wieder raus, sobald das geklärt ist.
    Serial.println("init: power");
    power_hal_init();
    // Settle before talking to the QMI8658. Located by bracketing the init
    // steps on real hardware: the failing write sits between "init: imu" and
    // the sensor's own OK line, so it is the IMU's first transaction — not the
    // PMU, where an earlier guess of mine put it.
    //
    // Correlates with display brightness (step 3: three of three boots),
    // although brightness never touches I2C — it is a QSPI command. A brighter
    // AMOLED simply draws more from the shared supply, and the sensor's first
    // write is what happens to land in that moment. The library ignores the
    // failure and the chip reports OK afterwards, so this only silences a
    // harmless log line; it is worth doing because an unexplained error in the
    // boot log trains people to ignore the boot log.
    delay(30);
    Serial.println("init: imu");
    imu_hal_init();
    Serial.println("init: sound");
    sound_hal_init();
    Serial.println("init: touch");
    touch_hal_init();
    Serial.println("init: fertig");

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    input_hal_init();
    // One side button means no "back" — stepping forward still reaches every
    // page, it just always wraps the same way.
    btn_primary.on_tap = (board_caps().button_count >= 2) ? ui_prev_screen : ui_next_screen;

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging(),
                      power_hal_is_vbus_in());
    ui_show_screen(SCREEN_SESSIONS);

    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data on BLE...\n",
        board_caps().name, W, H);
}

// Mindestabstand zwischen zwei Samples fuer usage_rate.cpp (siehe loop()).
// 60 s und nicht 30 s: Der Ring hat 6 Plaetze, also 5 Abstaende, und die
// Messspanne muss MIN_WINDOW_MS (240 s) erreichen. 5 x 30 s = 150 s haette die
// Gruppe dauerhaft auf "Idle" gehalten, 5 x 60 s = 300 s reicht.
static const uint32_t USAGE_RATE_SAMPLE_MS = 60000;
static uint32_t usage_rate_last_ms = 0;
static bool     usage_rate_sampled = false;

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Hold-to-pair gesture: hold the PWR button ~3s, then RELEASE → clear all BLE
// bonds and re-advertise. Clearing on *release* (not while held) is deliberate:
// holding to power the device OFF (AXP hardware shutdown at 8s) must not wipe
// the bond — a power-off hold never releases before shutdown. To stop a
// "chicken-out" release just before 8s from pairing, the gesture disarms at 6s.
//
//   ~1.5s long-press edge → PENDING
//   3.0s (+1500)          → ARMED   (release from here clears bonds)
//   6.0s (+4500)          → DISARMED (no clear; AXP powers off at 8s)
#define PAIR_ARM_AFTER_LONG_MS    1500   // 3.0s total
#define PAIR_DISARM_AFTER_LONG_MS 4500   // 6.0s total
enum pair_state_t { PAIR_IDLE, PAIR_PENDING, PAIR_ARMED };
static pair_state_t pair_state        = PAIR_IDLE;
static uint32_t     pair_long_seen_ms = 0;

static void pair_tick(void) {
    if (pair_state == PAIR_IDLE && power_hal_pwr_long_pressed()) {
        pair_state = PAIR_PENDING;
        pair_long_seen_ms = millis();
        (void)power_hal_pwr_released();  // drain any stale release edge
        Serial.println("PWR long-press: hold to ~3s then release to pair");
        return;
    }
    if (pair_state == PAIR_IDLE) return;

    if (power_hal_pwr_released()) {
        if (pair_state == PAIR_ARMED) {
            Serial.println("Pair: released in window — clearing bonds, advertising");
            ble_clear_bonds();
        } else {
            Serial.println("Pair: released too early — cancelled");
        }
        pair_state = PAIR_IDLE;
        return;
    }

    uint32_t held = millis() - pair_long_seen_ms;
    if (pair_state == PAIR_PENDING && held >= PAIR_ARM_AFTER_LONG_MS) {
        pair_state = PAIR_ARMED;
        Serial.println("Pair: armed — release to pair");
    } else if (pair_state == PAIR_ARMED && held >= PAIR_DISARM_AFTER_LONG_MS) {
        pair_state = PAIR_IDLE;  // power-off territory; don't pair
        Serial.println("Pair: disarmed (holding toward power-off)");
    }
}

void loop() {
    idle_tick();
    lv_timer_handler();
    ui_tick_anim();
    ui_wechsel_messung_tick();
    ble_tick();
    power_hal_tick();
    imu_hal_tick();
    sound_hal_tick();
    splash_tick();
    splash_mascot_tick();
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // A rotation blanks and re-ramps the panel. On boards where the splash
    // paints straight to the display, that wipes the picture and the
    // incremental draw never brings it back — it stays half erased. Ask for a
    // full repaint whenever the orientation actually changed.
    {
        static int last_quadrant = -1;
        const int quadrant = imu_hal_rotation_quadrant();
        if (quadrant != last_quadrant) {
            last_quadrant = quadrant;
            if (splash_is_active()) splash_force_repaint();
        }
    }

    // ---- Physical buttons ----
    //   PRIMARY   → tap: page back (page forward on one-button boards)
    //               hold: HID Space (Claude Code voice-mode PTT)
    //   SECONDARY → tap: page forward · hold: HID Shift+Tab (mode toggle)
    //   PWR       → tap: sleep now · hold ~3s + release: pairing mode
    //               (the PMU still powers the device off at 8s)
    //
    // Cycling animations and brightness moved to a tap on the panel, where
    // the action follows the page you are looking at — see global_click_cb()
    // in ui.cpp. Nothing was dropped, only relocated.
    {
        side_button_tick(btn_primary);
        if (board_caps().button_count >= 2) side_button_tick(btn_secondary);

        if (power_hal_pwr_pressed()) {
            if (!idle_consume_wake_press()) idle_sleep_now();
        }

        pair_tick();
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    // Der Schaetzer muss von jedem Wechsel erfahren, der den Verbrauch aendert —
    // nicht nur von Prozentwechseln: Kabel (auch ohne Laden, wenn der Akku voll
    // ist), dunkler Bildschirm und Helligkeitsstufe setzen sein Messfenster zurueck.
    static int  last_pct      = -2;
    static bool last_charging = false;
    static bool last_vbus     = false;
    static bool last_asleep   = false;
    static int  last_stage    = -1;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    bool vbus     = power_hal_is_vbus_in();
    bool asleep   = idle_is_asleep();
    int  stage    = brightness_get_stage();
    if (pct != last_pct || charging != last_charging || vbus != last_vbus ||
        asleep != last_asleep || stage != last_stage) {
        if (pct != last_pct) ble_set_battery_level(pct);
        last_pct = pct;
        last_charging = charging;
        last_vbus = vbus;
        last_asleep = asleep;
        last_stage = stage;
        ui_update_battery(pct, charging, vbus);
    }

    check_serial_cmd();

    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            int g_before = usage_rate_group();
            // Nur echte Nutzungsmessungen sampeln, und hoechstens alle 60 s:
            // Die Session-Updates alle paar Sekunden tragen dieselbe Nutzung und
            // fuellten den Ring mit 6 Plaetzen so schnell, dass er weniger als
            // MIN_WINDOW_MS abdeckte (Gruppe blieb "Idle"); ein {"ok":false}
            // sampelte 0.0 und erzeugte danach einen scheinbaren Sprung.
            // Der Reset-Ton haengt am Rueckgabewert; ein Abfall wird beim
            // naechsten Sample genau einmal erkannt, danach ist der Ring neu.
            bool session_reset = false;
            if (usage.ok && (!usage_rate_sampled ||
                             millis() - usage_rate_last_ms >= USAGE_RATE_SAMPLE_MS)) {
                session_reset = usage_rate_sample(usage.session_pct);
                usage_rate_last_ms = millis();
                usage_rate_sampled = true;
            }
            int g_after = usage_rate_group();
            // 5-hour session limit refilled → chime so the user knows they can
            // use Claude again (no-op on boards without a buzzer). Gated on the
            // daemon's opt-in `chime` config; the `buzz` serial cmd ignores it.
            if (session_reset && usage.chime) {
                Serial.println("session reset detected — chime");
                sound_hal_play_reset();
            }
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
