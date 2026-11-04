/* drivers/include/drivers/osk.h — Classic Mac On-Screen Touch Keyboard
 *
 * Provides a virtual keyboard for terminal windows on touch-enabled displays
 * when no physical USB keyboard is attached:
 * - 5-row laptop/terminal layout (Esc, Numbers, QWERTY, Shift, Space, Arrows, ^C, Enter)
 * - Classic Mac beveled button aesthetics using 1-bpp canvas and Gray8 anti-aliasing
 * - Direct coordinate geometry hit testing (zero dynamic allocation, no hitbox limits)
 * - Sticky Shift, sticky Ctrl, and Symbols mode toggle
 * - Hide/show toggle button and auto-docking
 */

#ifndef LUGALOS_DRIVERS_OSK_H
#define LUGALOS_DRIVERS_OSK_H

#include <stdbool.h>
#include <stdint.h>
#include "drivers/canvas1.h"

/* Returns whether the On-Screen Keyboard is currently visible/expanded */
bool osk_is_visible(void);

/* Set visibility of the OSK */
void osk_set_visible(bool visible);

/* Toggle visibility of the OSK */
void osk_toggle(void);

/* Get total height of OSK in screen pixels (scale-aware) */
int osk_get_height(void);

/* Number of text rows occupied by OSK given font cell height */
int osk_get_text_rows(int cell_h);

/* Render the OSK into canvas1 within rectangle (x0, y0, w, h) */
void osk_draw(const canvas1_t *cv, int x0, int y0, int w, int h);

/* Render a small Classic Mac keyboard toggle button in terminal corner when OSK is hidden */
void osk_draw_toggle_button(const canvas1_t *cv, int btn_x, int btn_y, int btn_w, int btn_h);

/* Handle tap at (client_x, client_y) within OSK area of width w and height h.
 * Returns true if the tap was consumed. If a key was typed, feeds it to active vterm. */
bool osk_handle_tap(int client_x, int client_y, int w, int h);

/* Check and handle tap on the collapsed toggle button */
bool osk_handle_toggle_tap(int client_x, int client_y, int btn_x, int btn_y, int btn_w, int btn_h);

#endif /* LUGALOS_DRIVERS_OSK_H */
