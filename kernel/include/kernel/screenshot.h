#ifndef LUGALOS_KERNEL_SCREENSHOT_H
#define LUGALOS_KERNEL_SCREENSHOT_H

#include <stdint.h>

/* The screen to a file -- 37.5a, plan/phase37_screen_layouts_and_apps.md,
 * the owner's request "for documentation purposes".
 *
 * A binary PBM (P4): a two-line text header and the 1-bpp frame, rows packed
 * eight pixels a byte with the leftmost pixel in the most significant bit,
 * 1 = black. Exactly the panel's own format apart from the bit order, so the
 * file is the frame, pixel for pixel; GIMP, ImageMagick and most viewers open
 * it, and `convert shot.pbm shot.png` makes a PNG.
 *
 * `path` NULL picks the next free /sd0/screenshots/shot-NNN.pbm (the
 * directory is made if missing). The path written goes to `out` (cap bytes).
 * Returns 0, or -1 with nothing written: no screen, no card, or a full one.
 * Taken by `screenshot`, `(screenshot)` and Super+Shift+3. */
int screenshot_save(const char *path, char *out, uint32_t cap);

#endif /* LUGALOS_KERNEL_SCREENSHOT_H */
