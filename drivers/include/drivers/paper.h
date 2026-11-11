#ifndef LUGALOS_DRIVERS_PAPER_H
#define LUGALOS_DRIVERS_PAPER_H

#include <stdbool.h>
#include <stdint.h>

/* The screen's two colours (drivers/paper.c): every panel draws 1-bpp and
 * maps it to an ink and a paper colour at output, so "warmth" is that pair.
 *
 * display.warmth 0..PAPER_WARMTH_MAX moves the paper from white to a light
 * warm paper (#F4E8D0) at 5, then on to a deep, aged sepia (#E3CCA0) at 10;
 * with display.warm_ink on, the ink goes from black to dark brown (#3B2A1A)
 * by 5 and stays there. Contrast at 10: 8.8:1 with the brown ink, 13.4:1
 * with black (WCAG AAA asks 7:1). Warmth 0 is exactly black on white,
 * 0x0000 / 0xffff. */

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
