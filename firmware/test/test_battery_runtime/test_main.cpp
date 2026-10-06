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
//
// Semantik seit 2026-10-01: Nach jedem Reset (Start, Kabel ab, Stufenwechsel,
// Aufwachen) merkt sich das erste Sample nur den Ladestand. Der Messanker
// entsteht erst bei dem Sample, bei dem der Wert UNTER den gemerkten faellt —
// an der ersten Prozentkante. Die Spanne einer Messung laeuft also von Kante
// zu Sample, nie vom ersten Sample an.

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

// Annahme-Rate des Schaetzers: 900 s je Prozent = 15 min je Prozent. Mit ihr
// gilt in allen Fallen unten ohne eigene Messung: Minuten = 15 mal Ladestand.
static const uint32_t ANNAHME_MS = 900u * 1000u;

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
    // 900 s je Prozent, 100 % uebrig: 15 min * 100 = 1500 min.
    entlaedt(100, 0);
    CHECK(battery_runtime_minutes() == 1500);

    // --- Too little drop, even after a long time ---------------------------
    // 1 % ist Rauschen auf einer ganzzahligen Anzeige: keine eigene Messung,
    // also weiter die Annahme. 99 @ 30 min ist die erste Kante und damit nur
    // der Anker (Abfall 0): 99 % mal 15 min = 1485 min.
    entlaedt(99, 30 * MINUTE);
    CHECK(battery_runtime_minutes() == 1485);
    CHECK(battery_runtime_rate(2) == 0);

    // --- Enough drop but too short a window --------------------------------
    // 100 @ 0 merken, 99 @ 1 min ist der Anker, 97 @ 3 min fiele um 2 %
    // (MIN_ABFALL_PCT erreicht), aber das Fenster ab dem Anker ist nur 2 min
    // lang (< 3 min): keine Messung, Annahme 97 * 15 = 1455 min.
    frisch();
    entlaedt(100, 0);
    entlaedt(99, 1 * MINUTE);
    entlaedt(97, 3 * MINUTE);
    CHECK(battery_runtime_minutes() == 1455);
    CHECK(battery_runtime_rate(2) == 0);

    // --- A real slope ------------------------------------------------------
    // 100 @ 0 merken, 90 @ 10 min ist der Anker, 80 @ 40 min: 10 % in 30 min =
    // 3 min je Prozent; 80 % uebrig => 3 * 80 = 240 min.
    frisch();
    entlaedt(100, 0);
    entlaedt(90, 10 * MINUTE);
    entlaedt(80, 40 * MINUTE);
    CHECK(battery_runtime_minutes() == 240);
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
    probe(81, true, true, false, 2, 41 * MINUTE);
    CHECK(battery_runtime_minutes() == -1);

    // --- Nach dem Abstecken traegt die zuletzt gemessene Rate ---------------
    // Die Rate von vorhin ist eine echte Messung an diesem Geraet — 3 min je
    // Prozent, 95 % uebrig, also 285 min. 95 @ 50 min ist nur das Merken.
    entlaedt(95, 50 * MINUTE);
    CHECK(battery_runtime_minutes() == 285);
    // Eine frische Messung ersetzt die gespeicherte sofort: 94 @ 60 min ist die
    // Kante (Anker), 84 @ 80 min fiele um 10 % in 20 min = 2 min je Prozent;
    // 84 % uebrig => 2 * 84 = 168 min (nicht mehr 3 * 84 = 252).
    entlaedt(94, 60 * MINUTE);
    entlaedt(84, 80 * MINUTE);
    CHECK(battery_runtime_rate(2) == 120000u);
    CHECK(battery_runtime_minutes() == 168);

    // --- A reading above the remembered level only raises it ---------------
    // Vor der ersten Kante gibt es keinen Anker, der neu zu setzen waere: 50 @ 0
    // wird gemerkt, 70 @ 10 min hebt den gemerkten Wert auf 70 (Annahme:
    // 15 * 70 = 1050 min). 60 @ 40 min ist die Kante, 50 @ 70 min fiel um 10 %
    // in 30 min = 3 min je Prozent; 50 uebrig => 150 min.
    frisch();
    entlaedt(50, 0);
    entlaedt(70, 10 * MINUTE);   // pack recovered / jitter
    CHECK(battery_runtime_anker_pct() == -1);
    CHECK(battery_runtime_minutes() == 1050);
    entlaedt(60, 40 * MINUTE);
    CHECK(battery_runtime_anker_pct() == 60);
    entlaedt(50, 70 * MINUTE);
    CHECK(battery_runtime_minutes() == 150);

    // --- A missing reading is ignored, not treated as empty ----------------
    frisch();
    entlaedt(-1, 0);
    CHECK(battery_runtime_minutes() == -1);

    // --- Small jitter inside the tolerance keeps the anchor ----------------
    // 80 @ 0 merken, 79 @ 2 min ist der Anker, 80 @ 5 min (+1, geduldet:
    // 80 > 79 + 1 ist falsch) laesst ihn stehen, 69 @ 32 min: 10 % in 30 min =
    // 3 min je Prozent; 69 uebrig => 207 min. Waere der Anker verworfen worden,
    // gaebe es hier keine Messung.
    frisch();
    entlaedt(80, 0);
    entlaedt(79, 2 * MINUTE);
    entlaedt(80, 5 * MINUTE);
    CHECK(battery_runtime_anker_pct() == 79);
    entlaedt(69, 32 * MINUTE);
    CHECK(battery_runtime_rate(2) == 180000u);
    CHECK(battery_runtime_minutes() == 207);

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
    // Erst nach dem Abstecken beginnt eine neue Messung. 90 @ 31 min wird nur
    // gemerkt: Annahme 15 * 90 = 1350 min, nichts gemessen.
    entlaedt(90, 31 * MINUTE);
    CHECK(battery_runtime_minutes() == 1350);
    // 89 @ 40 min ist die Kante, 79 @ 70 min: 10 % in 30 min.
    entlaedt(89, 40 * MINUTE);
    entlaedt(79, 70 * MINUTE);
    CHECK(battery_runtime_rate(2) == 180000u);
    CHECK(battery_runtime_minutes() == 237);         // 3 min * 79
    // Wieder anstecken, ohne zu laden: keine Zahl, die gespeicherte Rate bleibt.
    probe(79, false, true, false, 2, 71 * MINUTE);
    probe(60, false, true, false, 2, 500 * MINUTE);
    CHECK(battery_runtime_minutes() == -1);
    CHECK(battery_runtime_rate(2) == 180000u);

    // --- Stufenwechsel: neuer Anker, Rate nur fuer die eigene Stufe ---------
    frisch();
    entlaedt(100, 0, 0);
    entlaedt(90, 10 * MINUTE, 0);                    // Kante: Anker
    entlaedt(80, 40 * MINUTE, 0);                    // Stufe 0: 10 % in 30 min = 180000 ms/%
    CHECK(battery_runtime_rate(0) == 180000u);
    // Wechsel auf Stufe 3 ohne Prozentwechsel: das Fenster beginnt neu, 80 wird
    // nur gemerkt.
    entlaedt(80, 41 * MINUTE, 3);
    CHECK(battery_runtime_rate(3) == 0);
    // Stufe 3 kennt sich noch nicht: Rueckfall auf die einzige bekannte Rate,
    // 3 min * 80 = 240 min.
    CHECK(battery_runtime_minutes() == 240);
    entlaedt(79, 45 * MINUTE, 3);                    // Kante: Anker
    entlaedt(77, 54 * MINUTE, 3);                    // 2 % in 9 min: 270000 ms/%
    CHECK(battery_runtime_rate(3) == 270000u);
    CHECK(battery_runtime_rate(0) == 180000u);       // Stufe 0 unangetastet
    CHECK(battery_runtime_minutes() == 346);         // 270000 * 77 / 60000 = 346,5
    {
        int stufe = -1; uint32_t rate = 0, spanne = 0; int abfall = 0;
        CHECK(battery_runtime_neue_messung(&stufe, &rate, &spanne, &abfall));
        CHECK(stufe == 3 && rate == 270000u && spanne == 9 * MINUTE && abfall == 2);
    }

    // --- Ein Fenster ueber den Stufenwechsel hinweg wird nicht gemessen -----
    // Stufe 0: Anker 90 @ 10 min. Stufe 3 ab 30 min: 85 wird gemerkt, 84 @ 35 min
    // ist die Kante, 74 @ 50 min fiele um 10 % in 15 min = 90000. Ohne
    // Zuruecksetzen ergaebe sich aus dem alten Anker 16 % in 40 min = 150000.
    frisch();
    entlaedt(100, 0, 0);
    entlaedt(90, 10 * MINUTE, 0);
    entlaedt(85, 30 * MINUTE, 3);                    // Wechsel
    entlaedt(84, 35 * MINUTE, 3);
    entlaedt(74, 50 * MINUTE, 3);
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
    CHECK(battery_runtime_minutes() == 750);         // 15 min mal 50 %

    // --- Bildschirm aus: keine Messung ------------------------------------
    frisch();
    entlaedt(100, 0, 1);
    probe(95, false, false, true, 1, 10 * MINUTE);   // Bildschirm geht aus
    probe(80, false, false, true, 1, 100 * MINUTE);  // dunkel: wird nicht gemessen
    CHECK(battery_runtime_rate(1) == 0);
    probe(80, false, false, false, 1, 101 * MINUTE); // Bildschirm an: nur Merken
    probe(79, false, false, false, 1, 111 * MINUTE); // Kante: neuer Anker
    probe(69, false, false, false, 1, 141 * MINUTE); // 10 % in 30 min
    CHECK(battery_runtime_rate(1) == 180000u);
    CHECK(battery_runtime_minutes() == 207);         // 3 min * 69

    // --- Zustandswechsel ohne Prozentwechsel setzt den Anker zurueck --------
    // Der Schaetzer hoert nur bei Aenderungen zu; aendert sich allein der
    // Bildschirm, muss der Anker trotzdem neu beginnen.
    frisch();
    entlaedt(100, 0);
    entlaedt(90, 2 * MINUTE);                        // Kante: Anker bei 2 min
    probe(90, false, false, true, 2, 10 * MINUTE);   // aus, Prozent gleich
    probe(90, false, false, false, 2, 20 * MINUTE);  // an, Prozent gleich: nur Merken
    entlaedt(89, 25 * MINUTE);                       // Kante: neuer Anker bei 25 min
    entlaedt(79, 55 * MINUTE);                       // 10 % in 30 min
    CHECK(battery_runtime_rate(2) == 180000u);       // nicht aus dem alten Anker
                                                     // (sonst 11 % in 53 min = 289090)
    CHECK(battery_runtime_minutes() == 237);         // 3 min * 79

    // --- Plateau bei 100 % nach dem Abstecken -------------------------------
    // Der Akku steht nach dem Abstecken lange auf 100 %, der PMU meldet nichts
    // Neues (also kein Sample). 100 @ 0 wird nur gemerkt, 99 @ 40 min ist die
    // Kante, 97 @ 70 min: 2 % in 30 min = 15 min je Prozent. Vom ersten Sample
    // aus gerechnet waeren es 3 % in 70 min = 23,3 min je Prozent gewesen.
    frisch();
    entlaedt(100, 0);
    entlaedt(99, 40 * MINUTE);
    entlaedt(97, 70 * MINUTE);
    CHECK(battery_runtime_rate(2) == ANNAHME_MS);    // 30 min / 2 = 900000 ms/%
    CHECK(battery_runtime_minutes() == 1455);        // 15 min * 97
    {
        int stufe = -1; uint32_t rate = 0, spanne = 0; int abfall = 0;
        CHECK(battery_runtime_neue_messung(&stufe, &rate, &spanne, &abfall));
        CHECK(spanne == 30 * MINUTE && abfall == 2);
    }

    // --- Vor der ersten Kante gibt es keinen Anker, aber eine Zahl ----------
    // Aus der Annahme: 15 min * 80 = 1200 min. Weitere Samples mit gleichem
    // Wert aendern daran nichts.
    frisch();
    entlaedt(80, 0);
    CHECK(battery_runtime_anker_pct() == -1);
    CHECK(battery_runtime_anker_alter_ms(5 * MINUTE) == 0);
    CHECK(battery_runtime_minutes() == 1200);
    entlaedt(80, 60 * MINUTE);
    CHECK(battery_runtime_anker_pct() == -1);
    // Aus der gespeicherten Rate: 2 min je Prozent * 80 = 160 min.
    frisch();
    battery_runtime_set_rate(2, 120000);
    entlaedt(80, 0);
    CHECK(battery_runtime_anker_pct() == -1);
    CHECK(battery_runtime_minutes() == 160);

    // --- Ein Anstieg nach gesetztem Anker fuehrt zurueck in "warte auf Kante"
    // 90 @ 0 merken, 89 @ 10 min ist der Anker, 79 @ 40 min: 10 % in 30 min =
    // 3 min je Prozent, 3 * 79 = 237 min. Der Anker bleibt bei 89; erst ein Wert
    // ueber 89 + Toleranz 1, hier 92 @ 50 min, ist ein Anstieg: Anker und frische
    // Messung fallen, minutes() rechnet aus der gespeicherten Rate: 3 * 92 = 276.
    frisch();
    entlaedt(90, 0);
    entlaedt(89, 10 * MINUTE);
    entlaedt(79, 40 * MINUTE);
    CHECK(battery_runtime_minutes() == 237);
    entlaedt(92, 50 * MINUTE);
    CHECK(battery_runtime_anker_pct() == -1);
    CHECK(battery_runtime_minutes() == 276);
    // Die naechste Messung beginnt an der naechsten Kante: 91 @ 60 min ist der
    // Anker, 89 @ 75 min: 2 % in 15 min = 7,5 min je Prozent = 450000 ms;
    // 450000 * 89 / 60000 = 667,5 -> 667 min. Vom alten Anker (89 @ 10 min)
    // aus ergaeben sich keine 2 %, vom Anstieg (92 @ 50 min) aus nur 3 % in 25 min.
    entlaedt(91, 60 * MINUTE);
    CHECK(battery_runtime_anker_pct() == 91);
    CHECK(battery_runtime_anker_alter_ms(60 * MINUTE) == 0);
    entlaedt(89, 75 * MINUTE);
    CHECK(battery_runtime_rate(2) == 450000u);
    CHECK(battery_runtime_minutes() == 667);

    // --- Anker fuer die serielle Diagnose ----------------------------------
    // 77 @ 5 min wird nur gemerkt (-1), 76 @ 8 min ist die Kante.
    frisch();
    entlaedt(77, 5 * MINUTE);
    CHECK(battery_runtime_anker_pct() == -1);
    entlaedt(76, 8 * MINUTE);
    CHECK(battery_runtime_anker_pct() == 76);
    CHECK(battery_runtime_anker_alter_ms(11 * MINUTE) == 3 * MINUTE);
    probe(76, false, true, false, 2, 12 * MINUTE);   // Kabel: Anker verworfen
    CHECK(battery_runtime_anker_pct() == -1);

    // --- Plausibilitaetsgrenzen --------------------------------------------
    // Gespeicherte Rate zu klein (59999 < 60000 ms/%) oder zu gross (3000001):
    // wird nicht uebernommen, die Stufe bleibt "unbekannt".
    frisch();
    battery_runtime_set_rate(1, 59999u);
    CHECK(battery_runtime_rate(1) == 0u);
    battery_runtime_set_rate(1, 3000001u);
    CHECK(battery_runtime_rate(1) == 0u);
    battery_runtime_set_rate(1, 60000u);       // die Grenzen selbst sind erlaubt
    CHECK(battery_runtime_rate(1) == 60000u);
    battery_runtime_set_rate(1, 3000000u);
    CHECK(battery_runtime_rate(1) == 3000000u);
    battery_runtime_set_rate(1, 3000001u);     // ausserhalb: alter Wert bleibt
    CHECK(battery_runtime_rate(1) == 3000000u);

    // Neue Messung zu klein: 100 @ 0 wird gemerkt, 99 @ 1 min ist die Kante,
    // 97 @ 2 min = 2 % in 1 min = 30000 ms/% -> verworfen: keine Rate, keine
    // Flanke, Restlaufzeit weiter aus der Annahme (15 min * 97 = 1455).
    frisch();
    entlaedt(100, 0);
    entlaedt(99, 1 * MINUTE);
    entlaedt(97, 2 * MINUTE);
    CHECK(battery_runtime_rate(2) == 0u);
    { int s; uint32_t r, sp; int a; CHECK(!battery_runtime_neue_messung(&s, &r, &sp, &a)); }
    CHECK(battery_runtime_minutes() == 1455);

    // Neue Messung zu gross: 98 @ 0 wird gemerkt, 97 @ 10 h ist die Kante,
    // 95 @ 80 h = 2 % in 70 h = 126 Mio ms/% -> verworfen (15 min * 95 = 1425).
    frisch();
    entlaedt(98, 0);
    entlaedt(97, 10 * 60 * MINUTE);
    entlaedt(95, 80 * 60 * MINUTE);
    CHECK(battery_runtime_rate(2) == 0u);
    { int s; uint32_t r, sp; int a; CHECK(!battery_runtime_neue_messung(&s, &r, &sp, &a)); }
    CHECK(battery_runtime_minutes() == 1425);

    if (failures == 0) printf("battery_runtime: alle Faelle bestanden\n");
    else               printf("battery_runtime: %d Fehler\n", failures);
    return failures != 0;
}
