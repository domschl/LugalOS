#ifndef DRIVERS_AUDIOTEST_ESP32P4_H
#define DRIVERS_AUDIOTEST_ESP32P4_H

/* Audio Hardware Diagnostics & Verification for ESP32-P4
 *
 * Implements the `audiotest` diagnostic shell command for ESP32-P4 boards
 * (M5Stack Tab5 with ES8388 and Waveshare LCD-7B / NANO with ES8311).
 * Tests I2C codec identification and registers, power amplifier control,
 * and I2S0 clock/reset peripherals.
 */

void audiotest_cmd(const char *arg);

#endif /* DRIVERS_AUDIOTEST_ESP32P4_H */
