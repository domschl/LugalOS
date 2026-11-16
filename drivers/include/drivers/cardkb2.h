#ifndef DRIVERS_CARDKB2_H
#define DRIVERS_CARDKB2_H

#include <stdint.h>
#include <stdbool.h>

/* M5Stack CardKB2 (SKU U215) in I2C mode: a 42-key QWERTY keyboard at 0x5F.
 *
 * A read returns one byte: 0 for no key, else the ASCII of a key press (the unit
 * auto-repeats after 300 ms at 50 ms steps). Register 0xF1 holds the firmware
 * version. The Sym key is a toggle on the unit itself (its blue LED shows it), and
 * Fn+Sym+1 / Fn+Sym+2 select I2C / UART mode: a unit left in UART mode does not answer.
 * No arrow keys exist in I2C mode.
 *
 * cardkb2_start() probes the unit and, when one answers, registers it as a console
 * input source and starts the task that polls it. A unit plugged in later is found by
 * `cardkb init`, or by the task's retry once it has been started.
 */

#define CARDKB2_ADDR  0x5Fu

bool     cardkb2_start(void);           /* probe, then task + console source; false: no unit */
bool     cardkb2_is_detected(void);
uint8_t  cardkb2_firmware(void);

/* Maps the unit's byte to the console's: Backspace (0x08) becomes DEL (0x7F) as the USB
 * keyboard sends it; 0 means "drop" (no key, or a value no key produces). */
uint8_t  cardkb2_translate(uint8_t raw);

/* The escape sequence for a cursor-key byte, else NULL. */
const char *cardkb2_arrow(uint8_t raw);

uint32_t cardkb2_selftest(bool report);
void     cardkb2_print_status(void);

#endif /* DRIVERS_CARDKB2_H */
