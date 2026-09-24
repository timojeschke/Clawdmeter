#pragma once
#include <stdbool.h>
#include <stdint.h>

// Estimated time left on battery, derived from how fast the charge is actually
// dropping on this device. The PMU reports a percentage and a charging flag —
// it does not report remaining time, so this is arithmetic on observed drain,
// not a reading.
//
// Deliberately conservative about admitting it does not know: right after boot,
// while charging, and after any charge increase there is no usable slope, and
// the estimate stays unavailable rather than inventing a number. A wrong
// "20 minutes left" is worse than an empty line.

void battery_runtime_reset(void);

// Feed one observation. Safe to call every poll; it keeps only what it needs.
void battery_runtime_sample(int percent, bool charging, uint32_t now_ms);

// Minutes left, or -1 when there is not yet enough evidence.
int  battery_runtime_minutes(void);
