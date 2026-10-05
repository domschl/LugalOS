#ifndef LUGALOS_WS2812_H
#define LUGALOS_WS2812_H

#include <stdbool.h>
#include <stdint.h>

/* One WS2812 ("NeoPixel") on a single data line, driven by a peripheral's
 * pulse generator (45.5, plan/phase45_esp32c6.md).
 *
 * The protocol is a 800 kHz self-clocked bit stream: every bit is a high pulse
 * then a low one, and the *width* of the high pulse is the data (about 0.3 us
 * is a 0, 0.9 us a 1; each bit 1.25 us +/- 150 ns), 24 bits per LED, then
 * >50 us of low latches it. That tolerance is why this is not bit-banged: from
 * a core that executes in place from flash a cache miss in the middle of a bit
 * is a wrong colour, so the waveform is *generated* by hardware, from a table
 * of pulse widths, and the CPU only fills the table.
 *
 * The table is built by ws2812_encode(), which touches no hardware and so is
 * checked on the host against the datasheet's timings (tests/host/ws2812_host.c).
 */

/* One pulse-generator word: two (level, duration) halves, the same 32-bit
 * format as the ESP32's RMT items. Duration 0 ends a transmission. */
#define WS2812_ITEM(l0, d0, l1, d1) \
    ((uint32_t)(d0) | ((uint32_t)(l0) << 15) | ((uint32_t)(d1) << 16) | ((uint32_t)(l1) << 31))

/* Pulse widths in ticks of 100 ns (a 10 MHz pulse clock). */
#define WS2812_T0H 3u
#define WS2812_T0L 9u
#define WS2812_T1H 9u
#define WS2812_T1L 3u

/* 24 data words and the terminator. */
#define WS2812_WORDS 25u

/* Fills `out` (WS2812_WORDS words) for one pixel. The wire order is G, R, B on
 * the part itself; some boards (the Waveshare ESP32-C6-Zero's, per its own
 * demo) have a variant that takes R, G, B, which is `rgb_order`. */
void ws2812_encode(uint32_t *out, uint8_t r, uint8_t g, uint8_t b, bool rgb_order);

/* The hardware side. All of it is a no-op returning false on a board without
 * the LED. Safe to call from any task; a call arriving while the previous
 * frame is still on the wire waits for it (bounded). */
bool ws2812_init(void);
bool ws2812_set(uint8_t r, uint8_t g, uint8_t b);

#endif
