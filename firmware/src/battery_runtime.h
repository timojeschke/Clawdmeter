#pragma once
#include <stdbool.h>
#include <stdint.h>

// Estimated time left on battery, derived from how fast the charge is actually
// dropping on this device. The PMU reports a percentage and a charging flag —
// it does not report remaining time, so this is arithmetic on observed drain,
// not a reading.
//
// Deliberately conservative about admitting it does not know: right after boot,
// on the cable, and after any charge increase there is no usable slope, and
// the estimate stays unavailable rather than inventing a number. A wrong
// "20 minutes left" is worse than an empty line.

// Helligkeitsstufen, fuer die je eine eigene Verbrauchsrate gefuehrt wird. Der
// Bildschirm ist der groesste Verbraucher, und seine Leistung haengt an der
// Stufe — eine Rate bei Stufe 3 taugt nicht fuer Stufe 0.
#define BATTERY_RUNTIME_STAGES 4

void battery_runtime_reset(void);

// Feed one observation. Der Aufrufer meldet sich bei jedem Prozentwechsel UND
// bei jedem Wechsel von Kabel, Bildschirm oder Stufe — ein Zustandswechsel
// setzt das laufende Messfenster auch dann zurueck, wenn der Ladestand gleich
// blieb.
//   charging : PMU meldet aktives Laden
//   vbus_in  : USB-Kabel steckt (auch bei vollem Akku, wenn nichts geladen wird)
//   asleep   : Bildschirm ist dunkel
//   stage    : Index der Helligkeitsstufe (0..BATTERY_RUNTIME_STAGES-1)
// Am Kabel und bei dunklem Bildschirm wird nie gemessen.
void battery_runtime_sample(int percent, bool charging, bool vbus_in,
                            bool asleep, int stage, uint32_t now_ms);

// Minutes left, or -1 when nothing is known at all (und stets am Kabel).
//
// Solange keine frische Messung vorliegt, wird aus der Rate der aktuellen
// Stufe hochgerechnet; fehlt sie, aus der der naechstgelegenen Stufe mit
// bekannter Rate, sonst aus der Annahme. Eine frische Messung ersetzt das sofort.
int  battery_runtime_minutes(void);

// Die Verbrauchsrate je Stufe in Millisekunden je Prozentpunkt (0 = unbekannt).
// Der Aufrufer legt sie beim Start aus dem NVS hinein und schreibt sie zurueck,
// wenn eine neue Messung gemeldet wird — dieses Modul kennt keinen Speicher und
// bleibt so testbar.
//
// Plausibilitaetsgrenzen einer Rate in ms je Prozentpunkt. Darunter waere der
// Akku in unter 100 min leer, darueber haelt er laenger als rund 83 Stunden —
// beides ist ein Messfehler, kein Verbrauch (ein verfaelschter NVS-Wert lag
// einmal bei 5846863 ms/%). Raten ausserhalb werden weder uebernommen noch
// gespeichert; 0 bleibt "unbekannt" und ist erlaubt.
#define BATTERY_RUNTIME_RATE_MIN_MS    60000u     // 100 % in 100 min
#define BATTERY_RUNTIME_RATE_MAX_MS  3000000u     // 100 % in rund 83 h
bool     battery_runtime_rate_plausibel(uint32_t ms_pro_prozent);
void     battery_runtime_set_rate(int stage, uint32_t ms_pro_prozent);
uint32_t battery_runtime_rate(int stage);

// Flanke: true genau einmal, nachdem sample() eine frische Rate ermittelt hat.
// Liefert Stufe, Rate, Messspanne und Abfall dieser Messung (Zeiger duerfen
// NULL sein), damit der Aufrufer genau diese Stufe sichern und protokollieren kann.
bool battery_runtime_neue_messung(int* stage, uint32_t* ms_pro_prozent,
                                  uint32_t* spanne_ms, int* abfall);

// Fuer die serielle Diagnose: Messanker (Prozent, -1 = keiner) und dessen Alter.
// Der Anker entsteht erst an der ersten Prozentkante nach einem Reset; davor
// ist er -1, auch wenn minutes() schon aus Rate oder Annahme hochrechnet.
int      battery_runtime_anker_pct(void);
uint32_t battery_runtime_anker_alter_ms(uint32_t now_ms);
