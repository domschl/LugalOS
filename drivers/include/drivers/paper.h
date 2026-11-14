#ifndef LUGALOS_DRIVERS_PAPER_H
#define LUGALOS_DRIVERS_PAPER_H

#include <stdbool.h>
#include <stdint.h>

/* The screen's two colours (drivers/paper.c): every panel draws 1-bpp and
 * maps it to an ink and a paper colour at output, so "warmth" is that pair.
 *
 * display.warmth 0..PAPER_WARMTH_MAX moves the paper from white to a light
 * warm paper (#F4E8D0) at 5, then on to a rich sepia (#E8D7B4) at 10;
 * with display.warm_ink on, the ink goes from black to sepia brown (#5B4636)
 * by 5 and stays there. Warmth 0 is exactly black on white, 0x0000 / 0xffff. */

#define PAPER_WARMTH_MAX 10

/* The pair for warmth w (clamped) as RGB565. Pure integer arithmetic. */
void paper_pair(int warmth, bool warm_ink, uint16_t *ink, uint16_t *paper);

/* The pair the settings ask for, given to the panel this board has (none:
 * nothing). From the settings store, which applies it at boot and on each
 * change. */
void paper_apply(void);

/* The pair last applied (black on white before any). */
void paper_current(uint16_t *ink, uint16_t *paper);

/* `paper check`-style self-test of paper_pair(): the ends and monotony. */
bool paper_selftest(void);

#endif /* LUGALOS_DRIVERS_PAPER_H */
