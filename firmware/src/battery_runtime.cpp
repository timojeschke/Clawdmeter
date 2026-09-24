#include "battery_runtime.h"

// The anchor is the oldest observation still worth measuring against: the
// moment the device last started discharging. Slope is computed from that
// anchor to now, which smooths the PMU's coarse steps far better than
// comparing consecutive samples would.
static uint32_t anker_ms   = 0;
static int      anker_pct  = -1;
static int      letzte_pct = -1;
static int      minuten    = -1;

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

void battery_runtime_reset(void) {
    anker_ms   = 0;
    anker_pct  = -1;
    letzte_pct = -1;
    minuten    = -1;
}

void battery_runtime_sample(int percent, bool charging, uint32_t now_ms) {
    if (percent < 0) return;

    // On the cable there is no drain to measure, and the figure from before
    // the cable went in is stale the moment it is plugged. Start over.
    if (charging) {
        battery_runtime_reset();
        return;
    }

    if (anker_pct < 0) {
        anker_ms  = now_ms;
        anker_pct = percent;
        letzte_pct = percent;
        return;
    }

    if (percent > anker_pct + ANSTIEG_TOLERANZ) {
        battery_runtime_reset();
        anker_ms  = now_ms;
        anker_pct = percent;
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
    const uint64_t rest_ms = (uint64_t)ms_pro_prozent * (uint64_t)percent;
    minuten = (int)(rest_ms / 60000u);
}

int battery_runtime_minutes(void) {
    return minuten;
}
