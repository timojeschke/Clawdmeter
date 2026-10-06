#pragma once
#include <Arduino.h>

// Obergrenzen, keine Zusage: Der Daemon kuerzt selbst auf das, was in eine
// Funknachricht passt.
//
// 24 Byte waren zu wenig — "Heimatschutzverein - Dokumente" hat 30 Zeichen und
// wurde hier abgeschnitten, nachdem der Daemon ihn extra ungekuerzt geschickt
// hatte. Ein Umlaut kostet in UTF-8 zwei Byte, deshalb grosszuegig: 48 Byte
// fassen auch einen 40 Zeichen langen Namen mit Umlauten.
#define SESSIONS_MAX_NAMES 8
#define SESSIONS_NAME_LEN  48

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
    // Per-model weekly quota, e.g. "Fable". Separate from weekly_pct, which is
    // the pooled limit across all models. Absent unless the account has one.
    bool scoped_valid;
    char scoped_name[12];
    int  scoped_pct;

    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse

    // ---- Claude Code sessions on a remote machine (optional second source) ----
    // Absent unless the daemon is configured for it; sessions_valid stays false
    // then and the sessions screen says so instead of showing three zeros,
    // which would read as "nothing is running" rather than "nothing is known".
    bool sessions_valid;
    int  sessions_waiting;   // finished a turn, waiting for input
    int  sessions_working;   // busy right now
    int  sessions_background; // reply done, a background task still runs (Claude Code status "shell")
    int  sessions_parked;    // idle for a long while
    // Anzeige: Total = waiting + working + parked + background, Running =
    // working + background, Idle = der Rest (gerechnet in ui.cpp).
    // Names of the running sessions (working first, then background), as many
    // as fit the 512-byte BLE payload.
    // sessions_hidden counts those left out — the screen shows "+N more"
    // rather than silently presenting a partial list as complete.
    char sessions_names[SESSIONS_MAX_NAMES][SESSIONS_NAME_LEN];
    int  sessions_name_count;
    int  sessions_hidden;
    // Wann zuletzt Sessionfelder ankamen (millis). Fehlen sie in einer
    // einzelnen Nachricht, heisst das "diesmal nichts dabei", nicht "es gibt
    // nichts" — siehe parse_json().
    unsigned long sessions_last_ms;
};
