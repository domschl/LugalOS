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
#include "drivers/touch.h"

#define OSK_KEY_NONE        0
#define OSK_KEY_ESC         0x1B
#define OSK_KEY_TAB         '\t'
#define OSK_KEY_ENTER       '\r'
#define OSK_KEY_BKSP        0x7F
#define OSK_KEY_SHIFT       0x101
#define OSK_KEY_CTRL        0x102
#define OSK_KEY_SYM         0x103
#define OSK_KEY_HIDE        0x104
#define OSK_KEY_UP          0x105
#define OSK_KEY_DOWN        0x106
#define OSK_KEY_LEFT        0x107
#define OSK_KEY_RIGHT       0x108
#define OSK_KEY_CTRL_C      0x109
#define OSK_KEY_CMD         0x10A

#define OSK_MOD_SHIFT       (1u << 0)
#define OSK_MOD_CTRL        (1u << 1)
#define OSK_MOD_SYM         (1u << 2)
#define OSK_MOD_CMD         (1u << 3)

/* Get total height of OSK in screen pixels (scale-aware) */
int osk_get_height(void);

/* Number of text rows occupied by OSK given font cell height */
int osk_get_text_rows(int cell_h);

/* Render the OSK into canvas1 within rectangle (x0, y0, w, h) */
void osk_draw(const canvas1_t *cv, int x0, int y0, int w, int h, uint8_t modifiers, int pressed);

/* Render a small Classic Mac keyboard toggle button in terminal corner when OSK is hidden */
void osk_draw_toggle_button(const canvas1_t *cv, int btn_x, int btn_y, int btn_w, int btn_h);

/* Hit test a tap at (client_x, client_y) within OSK rect (w, h).
 * Returns key code (ASCII char or OSK_KEY_*), 0xFFFF for padding, or 0 for miss. */
uint16_t osk_hit_test(int client_x, int client_y, int w, int h, uint8_t modifiers);

/* The key under (client_x, client_y) of an OSK w x h, as osk_draw()'s
 * `pressed` (row * 32 + key), or -1 for none. A pressed key draws inverted
 * and, for a character key below the top row, with a preview of its label
 * in large type above it, inside the keyboard. */
int osk_key_at(int client_x, int client_y, int w, int h);

#endif /* LUGALOS_DRIVERS_OSK_H */
