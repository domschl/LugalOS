#ifndef LUGALOS_DRIVERS_VIEW_H
#define LUGALOS_DRIVERS_VIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/event.h"

/* Views: the kernel's own windows on the ribbon -- the launcher and the
 * reader today -- seen by the console through one interface (drivers/view.c).
 *
 * Everything the console has for the focused view comes as one event, in
 * the view's content coordinates (kernel/event.h):
 *   POINTER_DOWN + WIN_EVF_PRESS   a finger went down: show what it is on pressed
 *   POINTER_UP   + WIN_EVF_PRESS   it lifted or moved off: let go
 *   POINTER_UP   + WIN_EVF_TAP     a tap at (x, y): act
 *   POINTER_MOVE                   a captured finger's point (captures() said so)
 *   WIN_EV_DRAG / DRAG_END         a finger dragging, and its end (dy, key = ms)
 *   WIN_EV_SCROLL                  a vertical swipe (dy, key = ms)
 *   WIN_EV_KEY_DOWN                a key (key, mods), decoded; the system's Cmd
 *                                  shortcuts have been taken out by the console
 * `inst` is the view's instance: the reader's id, 0 for the launcher. */

typedef struct {
    const char *name;
    bool (*active)(void);                                    /* open on the ribbon */
    void (*event)(uint8_t inst, const win_event_t *ev);
    bool (*captures)(uint8_t inst, int x, int y);             /* NULL: never */
} view_ops_t;

/* The view behind a ribbon window type (RIBBON_WIN_READER, ..._LAUNCHER), or
 * NULL for terminals and canvases. */
const view_ops_t *view_for_window(uint8_t type);

#endif /* LUGALOS_DRIVERS_VIEW_H */
