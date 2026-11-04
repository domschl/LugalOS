/* drivers/include/drivers/launcher_view.h — Classic Mac Touch Launcher Window
 *
 * Implements the touch-first Launcher ribbon window (RIBBON_WIN_LAUNCHER) for
 * terminal presets when operating without a keyboard or via touch navigation:
 * - Application cards: Reader, Moonbase, Terminal
 * - Document Library browser: scans /sd0/pub, /sd0/help, /flash0/help
 * - Quick Settings: Volume stepper, Brightness stepper, Orientation selector
 * - Freestanding C, zero heap allocations, zero FPU in M-mode.
 */

#ifndef LUGALOS_DRIVERS_LAUNCHER_VIEW_H
#define LUGALOS_DRIVERS_LAUNCHER_VIEW_H

#include "drivers/canvas1.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void     launcher_init(void);
bool     launcher_open(void);
void     launcher_close(void);
bool     launcher_is_active(void);
uint8_t  launcher_get_slot_id(void);
bool     launcher_set_target(uint8_t slot_id, int w, int h);
void     launcher_update(void);
void     launcher_draw(const canvas1_t *c, int w, int h);
bool     launcher_tap(int client_x, int client_y);
void     launcher_scroll(int delta_y);
void     launcher_feed_key(int c);
bool     launcher_poll_exec(char *out_cmd, size_t max_len);
uint8_t *launcher_get_aa_backing(uint32_t *out_stride, int *out_w, int *out_h);
uint32_t launcher_get_seq(void);
void     launcher_report(void);

#endif /* LUGALOS_DRIVERS_LAUNCHER_VIEW_H */
