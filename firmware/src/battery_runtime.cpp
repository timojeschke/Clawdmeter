#include "battery_runtime.h"

// The anchor is the oldest observation still worth measuring against: the
// moment the device last started discharging. Slope is computed from that
// anchor to now, which smooths the PMU's coarse steps far better than
// comparing consecutive samples would.

// Zuletzt gemessene Verbrauchsrate je Helligkeitsstufe, als Millisekunden je
// Prozentpunkt (0 = fuer diese Stufe nie gemessen). Ueberlebt Neustart und
// Kabel, weil der Aufrufer sie in den NVS legt.
//
// Ohne sie stuende nach jedem Start und nach jedem Abstecken eine Annahme da,
// obwohl das Geraet seinen eigenen Verbrauch laengst kennt. Die Rate von
// gestern ist eine echte Messung an diesem Geraet — sie wird sofort verworfen,
// sobald fuer dieselbe Stufe eine frische vorliegt.
static uint32_t raten_ms[BATTERY_RUNTIME_STAGES] = {0, 0, 0, 0};

// Startwert fuer den allerersten Akkubetrieb, solange nichts gemessen und
// nichts gespeichert ist: 108 Sekunden je Prozentpunkt, also rund drei Stunden
// fuer eine volle Ladung.
//
// Das ist eine ANNAHME, keine Messung — die einzige Zahl in diesem Modul, die
// nicht vom Geraet stammt. Sie steht hier, weil "misst..." laut Timo schlechter
// ist als eine Zahl, die sich selbst korrigiert: Nach zwei Prozent Abfall,
// also wenigen Minuten, ersetzt die erste echte Messung sie und wird
// gespeichert. Danach wird dieser Wert fuer diese Stufe nie wieder benutzt.
#define ANNAHME_RATE_MS (108u * 1000u)

static bool     gesperrt   = false;   // Kabel steckt: keine Zahl, keine Messung
static uint32_t anker_ms   = 0;
static int      anker_pct  = -1;
static int      letzte_pct = -1;
static int      minuten    = -1;
static int      aktuelle_stufe = 0;

// Zustand des letzten Aufrufs, um Wechsel zu erkennen — unabhaengig davon, ob
// sich der Ladestand mitbewegt hat.
static bool kontext_bekannt   = false;
static bool voriges_gesperrt  = false;
static bool voriges_asleep    = false;
static int  vorige_stufe      = 0;

// Flanke "frische Rate ermittelt", siehe battery_runtime_neue_messung().
static bool     messung_neu     = false;
static int      messung_stufe   = 0;
static uint32_t messung_rate    = 0;
static uint32_t messung_spanne  = 0;
static int      messung_abfall  = 0;

// Below this the slope is noise: the AXP reports whole percent, so a single
// step over a short window would imply wildly different runtimes.
//
// Lowered from 3 %/5 min on 2026-09-25: at those thresholds the line stayed
// blank through a whole evening of ordinary use, which reads as a broken
// feature rather than as caution. Two points over three minutes is still two
// independent readings and still refuses to answer from a single sample.
#define MIN_ABFALL_PCT   2
#define MIN_FENSTER_MS   (3u * 60u * 1000u)

// A reading above the anchor means the pack recovered (cable, load dropped,
// or simply PMU jitter). Re-anchor instead of computing a negative slope.
#define ANSTIEG_TOLERANZ 1

static int stufe_begrenzen(int stage) {
    if (stage < 0) return 0;
    if (stage >= BATTERY_RUNTIME_STAGES) return BATTERY_RUNTIME_STAGES - 1;
    return stage;
}

void battery_runtime_reset(void) {
    // Die gespeicherten Raten bleiben absichtlich stehen: Sie sind das Einzige,
    // was ein Neustart oder das Kabel nicht entwertet.
    anker_ms   = 0;
    anker_pct  = -1;
    letzte_pct = -1;
    minuten    = -1;
}

void battery_runtime_sample(int percent, bool charging, bool vbus_in,
                            bool asleep, int stage, uint32_t now_ms) {
    if (percent < 0) return;
    stage = stufe_begrenzen(stage);

    // Steckt das Kabel, wird nicht verbraucht, was man messen koennte — auch
    // dann nicht, wenn der Akku voll ist und der PMU "laedt nicht" meldet.
    const bool ist_gesperrt = charging || vbus_in;

    // Kabel, Bildschirm und Helligkeitsstufe aendern den Verbrauch schlagartig.
    // Ein Fenster, das darueber hinweg gemessen wird, mischt zwei Zustaende und
    // ergibt eine Rate, die zu keinem von beiden passt.
    if (kontext_bekannt && (ist_gesperrt != voriges_gesperrt ||
                            asleep != voriges_asleep ||
                            stage != vorige_stufe)) {
        battery_runtime_reset();
    }
    kontext_bekannt  = true;
    voriges_gesperrt = ist_gesperrt;
    voriges_asleep   = asleep;
    vorige_stufe     = stage;
    aktuelle_stufe   = stage;
    gesperrt         = ist_gesperrt;

    if (ist_gesperrt || asleep) return;   // hier gibt es nichts zu messen

    if (anker_pct < 0) {
        anker_ms   = now_ms;
        anker_pct  = percent;
        letzte_pct = percent;
        return;
    }

    if (percent > anker_pct + ANSTIEG_TOLERANZ) {
        battery_runtime_reset();
        anker_ms   = now_ms;
        anker_pct  = percent;
        letzte_pct = percent;
        return;
    }
    letzte_pct = percent;

    const uint32_t spanne_ms = now_ms - anker_ms;
    const int      abfall    = anker_pct - percent;
    if (abfall < MIN_ABFALL_PCT || spanne_ms < MIN_FENSTER_MS) return;

    // Minutes per percentage point, then scaled by what is left. Integer
    // arithmetic throughout — this runs on a microcontroller and the result
    // is rounded to a coarse display anyway.
    const uint32_t ms_pro_prozent = spanne_ms / (uint32_t)abfall;
    raten_ms[stage] = ms_pro_prozent;        // fuer diese Stufe merken
    messung_neu    = true;
    messung_stufe  = stage;
    messung_rate   = ms_pro_prozent;
    messung_spanne = spanne_ms;
    messung_abfall = abfall;
    const uint64_t rest_ms = (uint64_t)ms_pro_prozent * (uint64_t)percent;
    minuten = (int)(rest_ms / 60000u);
}

// Rate der Stufe, sonst die der naechstgelegenen Stufe mit bekannter Rate
// (bei gleichem Abstand die dunklere), sonst 0.
static uint32_t rate_mit_rueckfall(int stage) {
    if (raten_ms[stage] != 0) return raten_ms[stage];
    for (int abstand = 1; abstand < BATTERY_RUNTIME_STAGES; abstand++) {
        const int unten = stage - abstand;
        const int oben  = stage + abstand;
        if (unten >= 0 && raten_ms[unten] != 0) return raten_ms[unten];
        if (oben < BATTERY_RUNTIME_STAGES && raten_ms[oben] != 0) return raten_ms[oben];
    }
    return 0;
}

int battery_runtime_minutes(void) {
    if (gesperrt) return -1;                 // am Kabel gibt es keine Restlaufzeit
    if (minuten >= 0) return minuten;        // frisch gemessen schlaegt alles
    if (letzte_pct < 0) return -1;           // noch kein Ladestand gesehen
    uint32_t rate = rate_mit_rueckfall(aktuelle_stufe);
    if (rate == 0) rate = ANNAHME_RATE_MS;
    // Aus der gespeicherten Rate hochgerechnet. Dieselbe Arithmetik wie bei
    // der frischen Messung, nur mit der aelteren Steigung.
    const uint64_t rest_ms = (uint64_t)rate * (uint64_t)letzte_pct;
    return (int)(rest_ms / 60000u);
}

void battery_runtime_set_rate(int stage, uint32_t ms_pro_prozent) {
    if (stage < 0 || stage >= BATTERY_RUNTIME_STAGES) return;
    raten_ms[stage] = ms_pro_prozent;
}

uint32_t battery_runtime_rate(int stage) {
    if (stage < 0 || stage >= BATTERY_RUNTIME_STAGES) return 0;
    return raten_ms[stage];
}

bool battery_runtime_neue_messung(int* stage, uint32_t* ms_pro_prozent,
                                  uint32_t* spanne_ms, int* abfall) {
    if (!messung_neu) return false;
    messung_neu = false;
    if (stage)          *stage          = messung_stufe;
    if (ms_pro_prozent) *ms_pro_prozent = messung_rate;
    if (spanne_ms)      *spanne_ms      = messung_spanne;
    if (abfall)         *abfall         = messung_abfall;
    return true;
}

int battery_runtime_anker_pct(void) {
    return anker_pct;
}

uint32_t battery_runtime_anker_alter_ms(uint32_t now_ms) {
    return anker_pct < 0 ? 0 : now_ms - anker_ms;
}
