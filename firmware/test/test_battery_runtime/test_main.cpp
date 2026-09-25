// Host unit test for the battery runtime estimate — pure arithmetic on
// observed drain, no Arduino/LVGL/hardware deps:
//
//   g++ -std=c++17 -I ../../src test_main.cpp ../../src/battery_runtime.cpp -o t && ./t
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

int main() {
    // --- Nothing known yet -------------------------------------------------
    battery_runtime_reset();
    battery_runtime_set_rate(0);   // kein gespeicherter Wert aus einem Vorlauf
    CHECK(battery_runtime_minutes() == -1);

    battery_runtime_sample(100, false, 0);
    CHECK(battery_runtime_minutes() == -1);   // one point is not a slope

    // --- Too little drop, even after a long time ---------------------------
    battery_runtime_sample(99, false, 30 * MINUTE);
    CHECK(battery_runtime_minutes() == -1);   // 1 % is noise on a whole-percent reading

    // --- Enough drop but too short a window --------------------------------
    battery_runtime_reset();
    battery_runtime_sample(100, false, 0);
    battery_runtime_sample(90, false, 2 * MINUTE);
    CHECK(battery_runtime_minutes() == -1);

    // --- A real slope ------------------------------------------------------
    // 10 % in 30 min = 3 min per point; 90 % left => 270 min.
    battery_runtime_reset();
    battery_runtime_sample(100, false, 0);
    battery_runtime_sample(90, false, 30 * MINUTE);
    CHECK(battery_runtime_minutes() == 270);

    // --- Am Kabel gibt es keine Restlaufzeit --------------------------------
    // Dort wird nichts verbraucht, also waere jede Zahl erfunden.
    battery_runtime_sample(91, true, 31 * MINUTE);
    CHECK(battery_runtime_minutes() == -1);

    // --- Nach dem Abstecken traegt die zuletzt gemessene Rate ---------------
    // Neu seit 2026-09-25: Frueher stand hier minutenlang nichts, bis eine
    // frische Steigung vorlag. Die Rate von vorhin ist aber eine echte Messung
    // an diesem Geraet — 3 min je Prozent, 95 % uebrig, also 285 min.
    battery_runtime_sample(95, false, 40 * MINUTE);
    CHECK(battery_runtime_minutes() == 285);
    // Eine frische Messung ersetzt die gespeicherte sofort.
    battery_runtime_sample(85, false, 70 * MINUTE);
    CHECK(battery_runtime_minutes() == 255);   // 10 % in 30 min, 85 left

    // --- A reading above the anchor re-anchors instead of going negative ---
    battery_runtime_reset();
    battery_runtime_set_rate(0);                      // ohne Altwert pruefen
    battery_runtime_sample(50, false, 0);
    battery_runtime_sample(70, false, 10 * MINUTE);   // pack recovered / jitter
    CHECK(battery_runtime_minutes() == -1);
    battery_runtime_sample(60, false, 40 * MINUTE);   // 10 % in 30 min, 60 left
    CHECK(battery_runtime_minutes() == 180);

    // --- A missing reading is ignored, not treated as empty ----------------
    battery_runtime_reset();
    battery_runtime_sample(-1, false, 0);
    CHECK(battery_runtime_minutes() == -1);

    // --- Small jitter inside the tolerance keeps the anchor ----------------
    battery_runtime_reset();
    battery_runtime_sample(80, false, 0);
    battery_runtime_sample(81, false, 5 * MINUTE);    // +1 is tolerated
    battery_runtime_sample(70, false, 30 * MINUTE);   // 10 % in 30 min, 70 left
    CHECK(battery_runtime_minutes() == 210);

    if (failures == 0) printf("battery_runtime: alle Faelle bestanden\n");
    else               printf("battery_runtime: %d Fehler\n", failures);
    return failures != 0;
}
