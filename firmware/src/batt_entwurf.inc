// ENTWURF (Wegwerf-Zweig entwurf/batterie-ecke) — wird nie zusammengefuehrt.
// Fuenf Gestaltungsvarianten der Batterie-Ecke oben rechts, nur im Simulator
// per SIM_BATT_STIL=1..5 waehlbar (0 = heutiger Stand). Eingebunden aus ui.cpp.
//
// Gemeinsame Regeln aller Varianten:
//  - Die Ecke ist ein durchsichtiger Container, rechtsbuendig auf 20 px Abstand
//    zum rechten Rand, Oberkante 20 px, Unterkante hoechstens L.content_y.
//  - Keine Montserrat-Schrift, kein LV_SYMBOL: Der Ladezustand steht als Wort.
//  - Rot nur bei Entladen und <= 10 %; beim Laden bleibt es Terrakotta.
#include <stdlib.h>
#include <stdio.h>

#define E_LOW_PCT     10
#define E_TRACK       THEME_BAR_BG
#define E_TOP         20          // Oberkante der Ecke (20 px Randabstand)

static int e_stil = 0;

static int entwurf_stil_lesen(void) {
    const char* v = getenv("SIM_BATT_STIL");
    int s = v ? atoi(v) : 0;
    return (s >= 0 && s <= 5) ? s : 0;
}

// Breite der Ecke je Variante, fuer die Titelmitte (siehe compute_layout).
static int16_t entwurf_breite(int stil) {
    switch (stil) {
        case 1: return 82;
        case 2: return 96;
        case 3: return 124;
        case 4: return 150;
        case 5: return 112;
        default: return 0;
    }
}

static lv_obj_t* e_root;
static lv_obj_t* e_sub;          // Unterzeile: Restlaufzeit oder Ladezustand
static lv_obj_t* e_pct;          // Prozentzahl
static lv_obj_t* e_fill;         // Fuellung (V1, V2, V4)
static lv_obj_t* e_track;        // Spur (V2, V4)
static lv_obj_t* e_body;         // V1: Batteriekoerper, V4: Kapsel
static lv_obj_t* e_dot;          // V4: Trenner
static lv_obj_t* e_arc;          // V3: Ring
static lv_obj_t* e_seg[5];       // V5: Segmente
static lv_obj_t* e_nub;          // V1/V5: Kontaktstueck
static int16_t   e_w = 0;
static int16_t   e_fill_max = 0; // V1: nutzbare Fuellbreite, V2: Linienlaenge

static lv_obj_t* e_box(lv_obj_t* parent, int w, int h, lv_color_t col, int radius) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, col, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

static lv_obj_t* e_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t col) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_label_set_text(l, "");
    return l;
}

// ---------------------------------------------------------------- Aufbau

// V1: duenne Elfenbein-Kontur, Terrakotta-Fuellung mit Luft zur Kontur.
static void e_build_v1(void) {
    const int bw = 76, bh = 34, pad = 3;
    e_body = lv_obj_create(e_root);
    lv_obj_remove_style_all(e_body);
    lv_obj_clear_flag(e_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(e_body, bw, bh);
    lv_obj_align(e_body, LV_ALIGN_TOP_RIGHT, -5, 12);
    lv_obj_set_style_radius(e_body, 10, 0);
    lv_obj_set_style_border_width(e_body, 1, 0);
    lv_obj_set_style_border_color(e_body, THEME_TEXT, 0);
    lv_obj_set_style_border_opa(e_body, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(e_body, pad, 0);
    e_fill_max = bw - 2 - 2 * pad;
    e_fill = e_box(e_body, e_fill_max, bh - 2 - 2 * pad, THEME_ACCENT, 7);
    lv_obj_set_pos(e_fill, 0, 0);
    e_pct = e_label(e_body, &font_styrene_20, THEME_TEXT);
    e_nub = e_box(e_root, 4, 12, THEME_TEXT, 1);
    lv_obj_align_to(e_nub, e_body, LV_ALIGN_OUT_RIGHT_MID, 1, 0);
    e_sub = e_label(e_root, &font_styrene_14, THEME_DIM);
}

// V2: rein typografisch, rechtsbuendig, dazu eine 2-px-Linie auf Spur.
static void e_build_v2(void) {
    e_pct = e_label(e_root, &font_styrene_28, THEME_TEXT);
    e_sub = e_label(e_root, &font_styrene_14, THEME_DIM);
    e_fill_max = e_w;
    e_track = e_box(e_root, e_w, 2, E_TRACK, 1);
    lv_obj_align(e_track, LV_ALIGN_TOP_RIGHT, 0, 66);
    e_fill = e_box(e_root, 1, 2, THEME_ACCENT, 1);
    lv_obj_align(e_fill, LV_ALIGN_TOP_LEFT, 0, 66);
}

// V3: Ring wie scoped_ring, Zahl und Restlaufzeit links daneben.
static void e_build_v3(void) {
    const int d = 38;
    e_arc = lv_arc_create(e_root);
    lv_obj_remove_style_all(e_arc);
    lv_obj_set_size(e_arc, d, d);
    lv_arc_set_rotation(e_arc, 270);
    lv_arc_set_bg_angles(e_arc, 0, 360);
    lv_arc_set_range(e_arc, 0, 100);
    lv_obj_remove_flag(e_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(e_arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(e_arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(e_arc, E_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(e_arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(e_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(e_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_align(e_arc, LV_ALIGN_TOP_RIGHT, 0, 17);
    e_pct = e_label(e_root, &font_styrene_24, THEME_TEXT);
    e_sub = e_label(e_root, &font_styrene_14, THEME_DIM);
}

// V4: Kapsel in Paneelgrau, eine Zeile, Terrakotta-Anteil am unteren Rand.
static void e_build_v4(void) {
    e_body = lv_obj_create(e_root);
    lv_obj_remove_style_all(e_body);
    lv_obj_clear_flag(e_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(e_body, 38);
    lv_obj_set_width(e_body, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(e_body, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(e_body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(e_body, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(e_body, true, 0);
    lv_obj_set_style_pad_hor(e_body, 16, 0);
    lv_obj_set_style_pad_ver(e_body, 0, 0);
    lv_obj_set_style_pad_column(e_body, 0, 0);
    lv_obj_set_flex_flow(e_body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(e_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(e_body, LV_ALIGN_TOP_RIGHT, 0, 19);
    e_pct = e_label(e_body, &font_styrene_16, THEME_TEXT);
    e_dot = e_label(e_body, &font_styrene_16, THEME_DIM);
    lv_label_set_text(e_dot, "\xC2\xB7");
    lv_obj_set_style_pad_hor(e_dot, 7, 0);
    e_sub = e_label(e_body, &font_styrene_16, THEME_DIM);
    e_track = e_box(e_body, 10, 2, E_TRACK, 1);
    lv_obj_add_flag(e_track, LV_OBJ_FLAG_IGNORE_LAYOUT);
    e_fill = e_box(e_body, 1, 2, THEME_ACCENT, 1);
    lv_obj_add_flag(e_fill, LV_OBJ_FLAG_IGNORE_LAYOUT);
}

// V5: Pixelbatterie aus fuenf Bloecken, wie die Figur Clawd.
static void e_build_v5(void) {
    const int sw = 8, sh = 18, gap = 3, nubw = 4, nubh = 8;
    const int grp_w = 5 * sw + 4 * gap + 3 + nubw;
    const int y = 22;
    for (int i = 0; i < 5; i++) {
        e_seg[i] = e_box(e_root, sw, sh, E_TRACK, 0);
        lv_obj_set_pos(e_seg[i], e_w - grp_w + i * (sw + gap), y);
    }
    e_nub = e_box(e_root, nubw, nubh, E_TRACK, 0);
    lv_obj_set_pos(e_nub, e_w - nubw, y + (sh - nubh) / 2);
    e_pct = e_label(e_root, &font_mono_18, THEME_TEXT);
    e_sub = e_label(e_root, &font_styrene_14, THEME_DIM);
}

static void entwurf_create(lv_obj_t* parent, int stil) {
    e_stil = stil;
    e_w = entwurf_breite(stil);
    e_root = lv_obj_create(parent);
    lv_obj_remove_style_all(e_root);
    lv_obj_clear_flag(e_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(e_root, LV_OBJ_FLAG_CLICKABLE);
    // V4 ist je nach Zeile verschieden breit; der Container nimmt das Maximum.
    const int16_t root_w = (stil == 4) ? 172 : e_w;
    lv_obj_set_size(e_root, root_w, L.content_y - E_TOP);
    lv_obj_set_pos(e_root, L.scr_w - L.margin - root_w, E_TOP);
    switch (stil) {
        case 1: e_build_v1(); break;
        case 2: e_build_v2(); break;
        case 3: e_build_v3(); break;
        case 4: e_build_v4(); break;
        case 5: e_build_v5(); break;
    }
}

// --------------------------------------------------------------- Aktualisieren

static void e_set_fill(int16_t max_w, int pct) {
    int16_t w = (int16_t)((max_w * pct + 99) / 100);
    if (pct > 0 && w < 2) w = 2;
    lv_obj_set_width(e_fill, w);
}

static void e_upd_v1(int pct, lv_color_t col, bool low) {
    lv_obj_set_style_border_color(e_body, low ? THEME_RED : THEME_TEXT, 0);
    lv_obj_set_style_bg_color(e_nub, low ? THEME_RED : THEME_TEXT, 0);
    lv_obj_set_style_bg_color(e_fill, col, 0);
    e_set_fill(e_fill_max, pct);
    lv_obj_center(e_pct);
    lv_obj_align_to(e_sub, e_body, LV_ALIGN_OUT_BOTTOM_MID, 0, 5);
}

static void e_upd_v2(int pct, lv_color_t col, bool low) {
    lv_obj_set_style_text_color(e_pct, low ? THEME_RED : THEME_TEXT, 0);
    lv_obj_set_style_bg_color(e_fill, col, 0);
    e_set_fill(e_fill_max, pct);
    lv_obj_align(e_pct, LV_ALIGN_TOP_RIGHT, 0, 6);
    lv_obj_align(e_sub, LV_ALIGN_TOP_RIGHT, 0, 43);
}

static void e_upd_v3(int pct, lv_color_t col, bool low) {
    lv_arc_set_value(e_arc, pct);
    lv_obj_set_style_arc_color(e_arc, col, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(e_pct, low ? THEME_RED : THEME_TEXT, 0);
    lv_obj_align_to(e_pct, e_arc, LV_ALIGN_OUT_LEFT_TOP, -12, -9);
    lv_obj_align_to(e_sub, e_arc, LV_ALIGN_OUT_LEFT_BOTTOM, -12, 6);
}

static void e_upd_v4(int pct, lv_color_t col, bool low) {
    lv_obj_set_style_text_color(e_pct, low ? THEME_RED : THEME_TEXT, 0);
    lv_obj_set_style_border_width(e_body, low ? 1 : 0, 0);
    lv_obj_set_style_border_color(e_body, THEME_RED, 0);
    lv_obj_set_style_bg_color(e_fill, col, 0);
    lv_obj_update_layout(e_body);
    const int16_t w = lv_obj_get_width(e_body);
    // Linie auf der flachen Unterseite, an beiden Enden eingerueckt, damit
    // die Rundung der Kapsel sie nicht anschneidet (auch 7 % bleiben sichtbar).
    const int16_t inset = 16;
    e_fill_max = w - 2 * inset;
    lv_obj_set_width(e_track, e_fill_max);
    lv_obj_set_pos(e_track, 0, 31);
    e_set_fill(e_fill_max, pct);
    lv_obj_set_pos(e_fill, 0, 31);
}

static void e_upd_v5(int pct, lv_color_t col, bool low) {
    int an = (pct + 10) / 20;   // gerundet, ab 1 % mindestens ein Block
    if (pct > 0 && an < 1) an = 1;
    for (int i = 0; i < 5; i++) {
        lv_obj_set_style_bg_color(e_seg[i], i < an ? col : E_TRACK, 0);
    }
    lv_obj_set_style_text_color(e_pct, low ? THEME_RED : THEME_TEXT, 0);
    lv_obj_align_to(e_pct, e_seg[0], LV_ALIGN_OUT_LEFT_MID, -10, 0);
    lv_obj_align(e_sub, LV_ALIGN_TOP_RIGHT, 0, 51);
}

static void entwurf_update(int percent, bool charging, bool vbus_in, int rest) {
    const int pct = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    const bool low = !charging && pct <= E_LOW_PCT;
    const lv_color_t col = low ? THEME_RED : THEME_ACCENT;

    char buf[24];
    snprintf(buf, sizeof buf, "%d%%", pct);
    if (percent < 0) buf[0] = '\0';
    lv_label_set_text(e_pct, buf);

    lv_color_t sub_col = THEME_DIM;
    if (charging)                { lv_label_set_text(e_sub, "Charging"); sub_col = THEME_ACCENT; }
    else if (vbus_in)            { lv_label_set_text(e_sub, "On USB"); }
    else if (rest >= BATT_STUNDEN_AB_MIN) lv_label_set_text_fmt(e_sub, "%d h", (rest + 30) / 60);
    else if (rest >= 0)          lv_label_set_text_fmt(e_sub, "%d min", rest);
    else                         lv_label_set_text(e_sub, "");
    lv_obj_set_style_text_color(e_sub, sub_col, 0);

    switch (e_stil) {
        case 1: e_upd_v1(pct, col, low); break;
        case 2: e_upd_v2(pct, col, low); break;
        case 3: e_upd_v3(pct, col, low); break;
        case 4:
            // V4: eine Zeile "87% · 22 h". Ohne Restlaufzeit entfaellt der Trenner.
            lv_obj_set_style_text_color(e_dot, THEME_DIM, 0);
            if (e_sub && lv_label_get_text(e_sub)[0] == '\0') lv_label_set_text(e_dot, "");
            else lv_label_set_text(e_dot, "\xC2\xB7");
            e_upd_v4(pct, col, low);
            break;
        case 5: e_upd_v5(pct, col, low); break;
    }
}
