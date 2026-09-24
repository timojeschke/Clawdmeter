#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <SensorQMI8658.hpp>

// QMI8658 on the 2.16 carrier PCB drives auto-rotation. Same poll/hysteresis
// logic as the S3 AMOLED-2.16 port; the C6 applies the resulting quadrant via
// the CO5300 MADCTL register (display.cpp) instead of a CPU rotation strip,
// so it costs no RAM.

#define IMU_POLL_MS       100    // ~10 Hz
#define STABLE_TIME_MS    300    // orientation must hold this long before rotating
#define TILT_THRESHOLD    0.5f   // ~30° from axis (sin 30° ≈ 0.5)

static SensorQMI8658 imu;
static uint8_t  current_rotation   = 0;
static uint8_t  candidate_rotation = 0;
static uint32_t candidate_since    = 0;
static uint32_t last_poll_ms       = 0;
static bool     imu_ok             = false;

// Dead zone: the dominant axis must beat the other by 25 % before we commit,
// otherwise (near 45°, e.g. a tilted stand) the current quadrant is kept.
#define AXIS_MARGIN       1.25f

static uint8_t accel_to_rotation(float ax, float ay) {
    float abs_ax = fabsf(ax);
    float abs_ay = fabsf(ay);
    if (abs_ax < TILT_THRESHOLD && abs_ay < TILT_THRESHOLD) {
        return 255;  // ambiguous (face-up/down)
    }
    if (abs_ay > abs_ax * AXIS_MARGIN) return (ay > 0) ? 3 : 1;
    if (abs_ax > abs_ay * AXIS_MARGIN) return (ax > 0) ? 0 : 2;
    return 255;      // too close to a diagonal — keep what we have
}

// KNOWN AND HARMLESS: the boot log sometimes carries one red line here —
//   [E][esp32-hal-i2c-ng.c:275] i2cWrite(): i2c_master_transmit failed:
//   [259] ESP_ERR_INVALID_STATE
// always at ~1036 ms, always between "init: imu" and "QMI8658 init OK". The
// sensor reports OK every time and auto-rotation works. Investigated over 11
// boots on hardware in 2026-09; do not restart the chase without new evidence.
//
// Established:
//   - The failing write sits INSIDE SensorQMI8658::begin(), which still
//     returns true. A retry around begin() can therefore never fire — that is
//     the obvious fix, and it is a dead end.
//   - Not the second Wire.begin() the sensor library issues (SensorCommon.tpp
//     line 83): ESP32 TwoWire::begin() returns early on i2cIsInit() and tears
//     nothing down.
//   - Not brightness/current draw. That theory fit 3 of 3 failing boots at the
//     brightest step and was then falsified by a failure at step 1.
//   - Not readable from source: ESP_ERR_INVALID_STATE is not a documented
//     return of i2c_master_transmit, and the IDF driver ships pre-compiled.
//
// The warm-up below and the settle delay in main.cpp are kept on weak but
// real evidence: 3 of 3 boots failed without them, 2 of 7 with them. Small
// numbers, gathered across firmware revisions — enough to keep two cheap
// lines, not enough to call it a fix. Neither is load-bearing; the driver
// works without them.
static void warm_up_i2c(void) {
    // Spend the first transaction on a throwaway probe. A zero-length
    // transfer takes the HAL's probe path, which logs at verbose level, so
    // if the flakiness lands here it stays silent instead of shouting.
    for (int versuch = 0; versuch < 3; ++versuch) {
        Wire.beginTransmission(QMI8658_L_SLAVE_ADDRESS);
        if (Wire.endTransmission() == 0) return;
        delay(5);
    }
}

void imu_hal_init(void) {
    warm_up_i2c();
    if (!imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        Serial.println("QMI8658 init failed");
        return;
    }
    Serial.println("QMI8658 init OK (auto-rotation via MADCTL)");
    imu.configAccelerometer(
        SensorQMI8658::ACC_RANGE_4G,
        SensorQMI8658::ACC_ODR_LOWPOWER_21Hz,
        SensorQMI8658::LPF_MODE_3);
    imu.enableAccelerometer();
    imu_ok = true;
}

void imu_hal_tick(void) {
    if (!imu_ok) return;
    uint32_t now = millis();
    if (now - last_poll_ms < IMU_POLL_MS) return;
    last_poll_ms = now;

    float ax, ay, az;
    if (!imu.getAccelerometer(ax, ay, az)) return;

    uint8_t target = accel_to_rotation(ax, ay);
    if (target == 255 || target == current_rotation) {
        candidate_rotation = current_rotation;
        return;
    }
    if (target != candidate_rotation) {
        candidate_rotation = target;
        candidate_since = now;
    } else if (now - candidate_since >= STABLE_TIME_MS) {
        current_rotation = target;
        Serial.printf("Rotation: %d (ax=%.2f ay=%.2f)\n", current_rotation, ax, ay);
    }
}

uint8_t imu_hal_rotation_quadrant(void) { return current_rotation; }
