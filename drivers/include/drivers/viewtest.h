#ifndef LUGALOS_DRIVERS_VIEWTEST_H
#define LUGALOS_DRIVERS_VIEWTEST_H

#include "drivers/view.h"

/* `viewtest [open]` (dev builds): a view of its own type with one button,
 * and what it was told -- taps, the button, keys, resizes, focus. */
void viewtest_cmd(const char *arg);
extern const view_ops_t g_viewtest_view;

#endif /* LUGALOS_DRIVERS_VIEWTEST_H */
