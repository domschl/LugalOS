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
    bool (*active)(uint8_t inst);                            /* that instance open */
    void (*event)(uint8_t inst, const win_event_t *ev);
    bool (*captures)(uint8_t inst, int x, int y);             /* NULL: never */
    /* Its window's content is w x h now (a layout change): draws again if
     * that changed. */
    void (*resize)(uint8_t inst, int w, int h);
    /* Its window was closed (close box, Cmd-W, `q`): let go of the instance. */
    void (*close)(uint8_t inst);
    /* For the P4's compositor: the GRAY8 surface it draws (NULL: none) and
     * the number that moves at each drawing. */
    uint8_t *(*surface)(uint8_t inst, uint32_t *stride, int *w, int *h);
    uint32_t (*seq)(uint8_t inst);
} view_ops_t;

/* The view behind a ribbon window type -- every type from RIBBON_WIN_READER
 * up is one (ribbon_is_view()) -- or NULL for terminals and canvases. A new
 * view is a type number, its ops here, and its own file: the screen and the
 * compositor handle its window as any view's. */
const view_ops_t *view_for_window(uint8_t type);

#endif /* LUGALOS_DRIVERS_VIEW_H */
