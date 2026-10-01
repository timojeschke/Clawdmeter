#pragma once
// In-memory stand-in for ESP32 NVS Preferences (used by brightness.cpp).
// State lasts for the process lifetime only — the sim boots with defaults.
#include <stdint.h>
#include <stddef.h>

class Preferences {
public:
    bool begin(const char* name, bool read_only = false) {
        (void)name; (void)read_only;
        return true;
    }
    void end(void) {}
    uint8_t getUChar(const char* key, uint8_t def = 0);
    size_t  putUChar(const char* key, uint8_t value);
    // Fuer die gespeicherte Verbrauchsrate der Batterie (battery_runtime).
    uint32_t getULong(const char* key, uint32_t def = 0);
    size_t   putULong(const char* key, uint32_t value);
    bool     remove(const char* key);
    bool     isKey(const char* key);
};
