/* drivers/view.c -- the views behind ribbon windows (drivers/include/drivers/view.h). */

#include "drivers/view.h"
#include "drivers/screen.h"

extern const view_ops_t g_reader_view;
extern const view_ops_t g_launcher_view;

const view_ops_t *view_for_window(uint8_t type) {
    switch (type) {
    case RIBBON_WIN_READER:   return &g_reader_view;
    case RIBBON_WIN_LAUNCHER: return &g_launcher_view;
    default:                  return 0;
    }
}
