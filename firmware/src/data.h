#pragma once
#include <Arduino.h>

// Six names of 24 bytes is what the daemon's 480-byte budget realistically
// delivers alongside the usage numbers; it also fills the screen without
// scrolling. The daemon trims to fit — these are the ceiling, not a promise.
#define SESSIONS_MAX_NAMES 6
#define SESSIONS_NAME_LEN  24

struct UsageData {
    float session_pct;       // utilization 0-100 (5h window Pro/Max; spending % Enterprise)
    int session_reset_mins;  // minutes until reset
    float weekly_pct;        // 7-day utilization (Pro/Max only; 0 for Enterprise)
    int weekly_reset_mins;   // minutes until weekly reset (Pro/Max only)
    char status[16];         // "allowed", "limited", etc.
    bool chime;              // play the session-reset chime; false unless daemon opts in
    bool enterprise;         // true = Enterprise spending-limit account
    int time_pct;            // 0-100: fraction of billing period elapsed (Enterprise)
    int period_days;         // total billing period length in days (Enterprise)
    char reset_date[12];     // formatted reset date e.g. "Jul 1" (Enterprise)
    long clock_epoch;        // local wall-clock epoch (s) from daemon; 0 = not provided
    int  clock_fmt;          // 12 or 24 (hour format from daemon); defaults to 24
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse

    // ---- Claude Code sessions on a remote machine (optional second source) ----
    // Absent unless the daemon is configured for it; sessions_valid stays false
    // then and the sessions screen says so instead of showing three zeros,
    // which would read as "nothing is waiting" rather than "nothing is known".
    bool sessions_valid;
    int  sessions_waiting;   // finished a turn, waiting for input
    int  sessions_working;   // busy right now
    int  sessions_parked;    // idle for a long while
    // Names of waiting sessions, as many as fit the 512-byte BLE payload.
    // sessions_hidden counts those left out — the screen shows "+N more"
    // rather than silently presenting a partial list as complete.
    char sessions_names[SESSIONS_MAX_NAMES][SESSIONS_NAME_LEN];
    int  sessions_name_count;
    int  sessions_hidden;
};
