#ifndef LUGALOS_DRIVERS_TOUCH_H
#define LUGALOS_DRIVERS_TOUCH_H

/* A GT911 touch controller, polled, and three gestures for the ribbon --
 * 47.12, . Portable: it talks to the
 * controller through drivers/i2c_bus.h, and to the screen through the
 * console's hotkeys (kernel/console.h), so the LCD-7B and the RP2350
 * touch terminal run the same code.
 *
 *   a tap on a window          focuses it          (console_touch_tap())
 *   a tap on the menu bar      focuses the window its minimap block stands for
 *   a horizontal swipe         the ribbon moves one window, as Cmd+Left/Right
 *
 * Nothing else: no pointer, no selection, nothing that needs touch. */

#include <stdbool.h>
#include <stdint.h>

#include "lugalos_config.h"

#if defined(CONFIG_TOUCH_GT911)
#define TOUCH_PRESENT 1
#endif

/* Touch event callback: return true to capture event and suppress desktop ribbon gestures */
typedef bool (*touch_hook_fn)(int x, int y, int kind);

/* After sched_init() and the I2C task: finds the controller and starts the
 * polling task. -1 (and one log line) if there is no controller. */
int touch_start(void);

/* Direct touch hook for interactive full-screen applications */
void touch_set_hook(touch_hook_fn hook);

/* `ts`: the controller, its resolution, the counters. `ts log [s]`:
 * every press, move and release with its mapped point, for s seconds (20). */
void touch_command(const char *args);

/* Returns total number of touch press events detected since boot */
uint32_t touch_press_count(void);

#endif /* LUGALOS_DRIVERS_TOUCH_H */
