#ifndef DRIVERS_LCD2004_H
#define DRIVERS_LCD2004_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* HD44780 character LCD (20x4) behind a PCF8574 I2C backpack.
 *
 * Both uses share one frame buffer; nothing reaches the glass until lcd2004_flush(),
 * which sends only the characters that differ from what the display shows. A character
 * costs six I2C bytes (about 0.6 ms at 100 kHz), so a whole-screen redraw is ~50 ms
 * and a changed digit is a single short transfer.
 *
 * Terminal mode: lcd2004_term_write() -- cursor, \n \r \b \f, wrap, scroll.
 * Dashboard mode: lcd2004_put_at()   -- text at x,y, clipped, never scrolls, cursor untouched.
 * The modes may be mixed; a dashboard is a terminal screen that is simply never scrolled.
 *
 * PCF8574 pin map: P0 RS, P1 RW (held low), P2 E, P3 backlight, P4..P7 D4..D7.
 */

#define LCD2004_COLS   20u
#define LCD2004_ROWS   4u
#define LCD2004_ADDR   0x27u   /* PCF8574; the PCF8574A variant is at 0x3F */

/* Probes 0x27 then 0x3F (or `addr` when nonzero), runs the HD44780 4-bit init, clears. */
bool        lcd2004_init(uint8_t addr);
bool        lcd2004_is_ready(void);
uint8_t     lcd2004_address(void);

/* Frame buffer operations. Each initializes the display on first use. */
void lcd2004_clear(void);
void lcd2004_put_at(uint32_t x, uint32_t y, const char *s);
void lcd2004_term_write(const char *s, size_t len);
void lcd2004_set_cursor(uint32_t x, uint32_t y);
void lcd2004_cursor_visible(bool on);
void lcd2004_backlight(bool on);

/* Sends the changed characters. False when the bus failed; the next flush then
 * redraws everything, since what the display shows is unknown. */
bool lcd2004_flush(void);

/* The frame buffer's row `y` (LCD2004_COLS characters, no NUL), for tests and status. */
const char *lcd2004_row(uint32_t y);

typedef struct { uint8_t x, len; } lcd2004_run_t;

/* The runs of characters in `want` that differ from `shown` (one row), longest-first not
 * required: in column order. Returns the number of runs, at most `max`. */
uint32_t lcd2004_diff_runs(const char *shown, const char *want, lcd2004_run_t *runs, uint32_t max);

/* Bytes sent to the PCF8574 for one HD44780 byte (command or data), six of them. */
void lcd2004_encode(uint8_t out[6], uint8_t v, bool data, bool backlight);

uint32_t lcd2004_selftest(bool report);
void     lcd2004_print_status(void);

#endif /* DRIVERS_LCD2004_H */
