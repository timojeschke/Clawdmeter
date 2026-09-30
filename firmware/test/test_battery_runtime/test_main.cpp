// Host unit test for the battery runtime estimate — pure arithmetic on
// observed drain, no Arduino/LVGL/hardware deps.
//
// Kein pio-Test (`pio test` scheitert am Linken), sondern von Hand gebaut:
//
//   cd firmware/test/test_battery_runtime
//   g++ -std=c++17 -I ../../src test_main.cpp ../../src/battery_runtime.cpp -o /tmp/bt && /tmp/bt
//
// What is worth testing here is the refusal to answer: the estimate must stay
// unavailable until there is real evidence, because a confident-looking
// "20 minutes left" that is wrong is worse than a blank line.

#include "battery_runtime.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static const uint32_t MINUTE = 60u * 1000u;

// Kurzform: Entladen am Akku, Bildschirm an.
static void entlaedt(int pct, uint32_t t, int stufe = 2) {
    battery_runtime_sample(pct, false, false, false, stufe, t);
}
static void probe(int pct, bool laedt, bool vbus, bool asleep, int stufe, uint32_t t) {
    battery_runtime_sample(pct, laedt, vbus, asleep, stufe, t);
}
// Schaetzer und alle Raten auf "nie etwas gemessen" stellen.
static void frisch(void) {
    battery_runtime_reset();
    for (int i = 0; i < BATTERY_RUNTIME_STAGES; i++) battery_runtime_set_rate(i, 0);
    int s; uint32_t r, sp; int a;
    while (battery_runtime_neue_messung(&s, &r, &sp, &a)) {}   // Flanke leeren
}

int main() {
    // --- Nothing known yet -------------------------------------------------
    frisch();
    CHECK(battery_runtime_minutes() == -1);   // noch kein Ladestand gesehen

    // Ein einzelner Messpunkt ist keine Steigung — aber seit 2026-09-25 wird
    // daraus mit der Annahme-Rate hochgerechnet, statt nichts zu zeigen.
    // 108 s je Prozent, 100 % uebrig: 180 min.
    entlaedt(100, 0);
    CHECK(battery_runtime_minutes() == 180);

    // --- Too little drop, even after a long time ---------------------------
    // 1 % ist Rauschen auf einer ganzzahligen Anzeige: keine eigene Messung,
    // also weiter die Annahme — 99 % mal 108 s sind 178 min.
    entlaedt(99, 30 * MINUTE);
    CHECK(battery_runtime_minutes() == 178);

    // --- Enough drop but too short a window --------------------------------
    frisch();
    entlaedt(100, 0);
    entlaedt(90, 2 * MINUTE);
    CHECK(battery_runtime_minutes() == 162);   // Annahme: 90 % mal 108 s

    // --- A real slope ------------------------------------------------------
    // 10 % in 30 min = 3 min per point; 90 % left => 270 min.
    frisch();
    entlaedt(100, 0);
    entlaedt(90, 30 * MINUTE);
    CHECK(battery_runtime_minutes() == 270);
    {
        // Die frische Rate wird genau einmal gemeldet: Stufe, Spanne, Abfall.
        int stufe = -1; uint32_t rate = 0, spanne = 0; int abfall = 0;
        CHECK(battery_runtime_neue_messung(&stufe, &rate, &spanne, &abfall));
        CHECK(stufe == 2 && rate == 180000u && spanne == 30 * MINUTE && abfall == 10);
        CHECK(!battery_runtime_neue_messung(&stufe, &rate, &spanne, &abfall));
        CHECK(battery_runtime_rate(2) == 180000u);
    }

    // --- Am Kabel gibt es keine Restlaufzeit --------------------------------
    // Dort wird nichts verbraucht, also waere jede Zahl erfunden.
    probe(91, true, true, false, 2, 31 * MINUTE);
    CHECK(battery_runtime_minutes() == -1);

    // --- Nach dem Abstecken traegt die zuletzt gemessene Rate ---------------
    // Die Rate von vorhin ist eine echte Messung an diesem Geraet — 3 min je
    // Prozent, 95 % uebrig, also 285 min.
    entlaedt(95, 40 * MINUTE);
    CHECK(battery_runtime_minutes() == 285);
    // Eine frische Messung ersetzt die gespeicherte sofort.
    entlaedt(85, 70 * MINUTE);
    CHECK(battery_runtime_minutes() == 255);   // 10 % in 30 min, 85 left

    // --- A reading above the anchor re-anchors instead of going negative ---
    frisch();
    entlaedt(50, 0);
    entlaedt(70, 10 * MINUTE);   // pack recovered / jitter
    CHECK(battery_runtime_minutes() == 126);   // Annahme: 70 % mal 108 s
    entlaedt(60, 40 * MINUTE);   // 10 % in 30 min, 60 left
    CHECK(battery_runtime_minutes() == 180);

    // --- A missing reading is ignored, not treated as empty ----------------
    frisch();
    entlaedt(-1, 0);
    CHECK(battery_runtime_minutes() == -1);

    // --- Small jitter inside the tolerance keeps the anchor ----------------
    frisch();
    entlaedt(80, 0);
    entlaedt(81, 5 * MINUTE);    // +1 is tolerated
    entlaedt(70, 30 * MINUTE);   // 10 % in 30 min, 70 left
    CHECK(battery_runtime_minutes() == 210);

    // --- Kabel steckt, der PMU meldet aber "laedt nicht" (Akku voll) --------
    // Das war die Hauptursache fuer absurde Zahlen: Das Geraet lief am Kabel,
    // und der Schaetzer hielt es fuer Akkubetrieb.
    frisch();
    probe(100, false, true, false, 2, 0);
    probe(90, false, true, false, 2, 30 * MINUTE);   // waere eine gueltige Messung
    CHECK(battery_runtime_minutes() == -1);          // keine Zahl am Kabel
    CHECK(battery_runtime_rate(2) == 0);             // und keine Rate
    {
        int s; uint32_t r, sp; int a;
        CHECK(!battery_runtime_neue_messung(&s, &r, &sp, &a));
    }
    // Erst nach dem Abstecken beginnt eine neue Messung, mit frischem Anker.
    entlaedt(90, 31 * MINUTE);
    CHECK(battery_runtime_minutes() == 162);         // Annahme, nichts gemessen
    entlaedt(80, 61 * MINUTE);                       // 10 % in 30 min
    CHECK(battery_runtime_rate(2) == 180000u);
    CHECK(battery_runtime_minutes() == 240);
    // Wieder anstecken, ohne zu laden: keine Zahl, die gespeicherte Rate bleibt.
    probe(80, false, true, false, 2, 62 * MINUTE);
    probe(60, false, true, false, 2, 500 * MINUTE);
    CHECK(battery_runtime_minutes() == -1);
    CHECK(battery_runtime_rate(2) == 180000u);

    // --- Stufenwechsel: neuer Anker, Rate nur fuer die eigene Stufe ---------
    frisch();
    entlaedt(100, 0, 0);
    entlaedt(90, 30 * MINUTE, 0);                    // Stufe 0: 180000 ms/%
    CHECK(battery_runtime_rate(0) == 180000u);
    // Wechsel auf Stufe 3 ohne Prozentwechsel: das Fenster beginnt neu.
    entlaedt(90, 31 * MINUTE, 3);
    CHECK(battery_runtime_rate(3) == 0);
    // Stufe 3 kennt sich noch nicht: Rueckfall auf die einzige bekannte Rate.
    CHECK(battery_runtime_minutes() == 270);
    entlaedt(88, 40 * MINUTE, 3);                    // 2 % in 9 min: 270000 ms/%
    CHECK(battery_runtime_rate(3) == 270000u);
    CHECK(battery_runtime_rate(0) == 180000u);       // Stufe 0 unangetastet
    CHECK(battery_runtime_minutes() == 396);
    {
        int stufe = -1; uint32_t rate = 0, spanne = 0; int abfall = 0;
        CHECK(battery_runtime_neue_messung(&stufe, &rate, &spanne, &abfall));
        CHECK(stufe == 3 && rate == 270000u && spanne == 9 * MINUTE && abfall == 2);
    }

    // --- Ein Fenster ueber den Stufenwechsel hinweg wird nicht gemessen -----
    // Ohne Zuruecksetzen ergaebe sich aus 20 % in 45 min die Mischrate 135000
    // — gemessen wird nur, was auf Stufe 3 lief: 10 % in 15 min.
    frisch();
    entlaedt(100, 0, 0);
    entlaedt(90, 30 * MINUTE, 3);                    // Wechsel
    entlaedt(80, 45 * MINUTE, 3);
    CHECK(battery_runtime_rate(3) == 90000u);
    CHECK(battery_runtime_rate(0) == 0);

    // --- Rueckfall auf die naechstgelegene Stufe mit bekannter Rate ---------
    frisch();
    battery_runtime_set_rate(1, 120000);
    battery_runtime_set_rate(3, 60000);
    entlaedt(50, 0, 2);                              // 1 und 3 gleich weit: die hellere
    CHECK(battery_runtime_minutes() == 50);          // 60000 ms/% mal 50 %
    entlaedt(50, 0, 3);                              // eigene Rate
    CHECK(battery_runtime_minutes() == 50);          // 60000 ms/% mal 50 %
    entlaedt(50, 0, 0);                              // naechste bekannte: Stufe 1
    CHECK(battery_runtime_minutes() == 100);
    frisch();
    battery_runtime_set_rate(3, 60000);
    entlaedt(50, 0, 0);                              // nur Stufe 3 bekannt, drei weit weg
    CHECK(battery_runtime_minutes() == 50);
    frisch();
    entlaedt(50, 0, 1);                              // gar keine bekannt: Annahme
    CHECK(battery_runtime_minutes() == 90);          // 108 s mal 50 %

    // --- Bildschirm aus: keine Messung ------------------------------------
    frisch();
    entlaedt(100, 0, 1);
    probe(95, false, false, true, 1, 10 * MINUTE);   // Bildschirm geht aus
    probe(80, false, false, true, 1, 100 * MINUTE);  // dunkel: wird nicht gemessen
    CHECK(battery_runtime_rate(1) == 0);
    probe(80, false, false, false, 1, 101 * MINUTE); // Bildschirm an: neuer Anker
    probe(70, false, false, false, 1, 131 * MINUTE); // 10 % in 30 min
    CHECK(battery_runtime_rate(1) == 180000u);
    CHECK(battery_runtime_minutes() == 210);

    // --- Zustandswechsel ohne Prozentwechsel setzt den Anker zurueck --------
    // Der Schaetzer hoert nur bei Aenderungen zu; aendert sich allein der
    // Bildschirm, muss der Anker trotzdem neu beginnen.
    frisch();
    entlaedt(100, 0);
    entlaedt(90, 2 * MINUTE);
    probe(90, false, false, true, 2, 10 * MINUTE);   // aus, Prozent gleich
    probe(90, false, false, false, 2, 20 * MINUTE);  // an, Prozent gleich
    entlaedt(80, 50 * MINUTE);                       // Fenster ab 20 min: 10 % in 30 min
    CHECK(battery_runtime_rate(2) == 180000u);       // nicht aus dem alten Anker
    CHECK(battery_runtime_minutes() == 240);

    // --- Anker fuer die serielle Diagnose ----------------------------------
    frisch();
    entlaedt(77, 5 * MINUTE);
    CHECK(battery_runtime_anker_pct() == 77);
    CHECK(battery_runtime_anker_alter_ms(8 * MINUTE) == 3 * MINUTE);
    probe(77, false, true, false, 2, 6 * MINUTE);    // Kabel: Anker verworfen
    CHECK(battery_runtime_anker_pct() == -1);

    if (failures == 0) printf("battery_runtime: alle Faelle bestanden\n");
    else               printf("battery_runtime: %d Fehler\n", failures);
    return failures != 0;
}
