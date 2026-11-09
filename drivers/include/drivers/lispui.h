#ifndef LUGALOS_DRIVERS_LISPUI_H
#define LUGALOS_DRIVERS_LISPUI_H

/* drivers/lispui.h -- `ugui` for Lisp programs (76.2): one ribbon window, a
 * view (drivers/view.h) whose picture a Lisp program describes.
 *
 * Immediate mode, as `ugui` itself: the program owns its loop. Between
 * lispui_begin() and lispui_end() each widget call appends one record to a
 * display list; lispui_end() hands the list to the views task, which draws it
 * with `ugui` and keeps it. Everything that changes the picture without the
 * program -- a finger pressing a button, the mouse hovering, the keyboard's
 * focus, a text field's caret, a list's scroll -- the views task draws again
 * from the kept list. What the program must act on comes as events, read
 * with lispui_next_event(); nothing calls into the program.
 *
 * Callers: the Lisp task (lisp.c's `ui-` primitives), under g_lisp_lock. */

#include <stdbool.h>
#include <stdint.h>
#include "lugalos_config.h"

/* Where there are views, which is where there is a screen. */
#if !(defined(CONFIG_BOARD_ESP32C6) || (defined(CONFIG_BOARD_RP2350) && !defined(CONFIG_LCD_PCLK_GPIO)))
#define LISPUI_PRESENT 1
#endif

/* Display-list operations: the numbers a record carries, then its strings. */
typedef enum {
    LISPUI_OP_FILL = 1,     /* x y w h gray                         */
    LISPUI_OP_LABEL,        /* x y bold muted; text                 */
    LISPUI_OP_BUTTON,       /* id x y w h active; label             */
    LISPUI_OP_CARD,         /* id x y w h active; title subtitle    */
    LISPUI_OP_CHECKBOX,     /* id x y w h checked; label            */
    LISPUI_OP_TOGGLE,       /* id x y w h on; label                 */
    LISPUI_OP_SLIDER,       /* id x y w h value min max             */
    LISPUI_OP_PROGRESS,     /* x y w h permille                     */
    LISPUI_OP_STEPPER,      /* dec-id inc-id x y w h; label value   */
    LISPUI_OP_SEGMENTED,    /* base-id x y w h selected; labels...  */
    LISPUI_OP_TABS,         /* base-id x y w h selected; labels...  */
    LISPUI_OP_LIST,         /* id x y w h selected; items...        */
    LISPUI_OP_TEXTFIELD,    /* id x y w h                           */
    LISPUI_OP_SECTION,      /* x y w; title                         */
    LISPUI_OP_DISABLE,      /* parts: the next widget's dimmed parts */
    LISPUI_OP_DIALOG,       /* base-id; title message buttons...    */
} lispui_op_t;

#define LISPUI_MAX_NUMS   8
#define LISPUI_MAX_STRS   64      /* a list's items */
#define LISPUI_TEXT_MAX   128     /* a text field's bytes, with the NUL */

/* What a program hears. */
typedef enum {
    LISPUI_EV_PRESS = 1,    /* a widget tapped, clicked, or activated with Enter: id, index */
    LISPUI_EV_CHANGE,       /* a slider moved (value), a text field edited */
    LISPUI_EV_SUBMIT,       /* Enter in a text field */
    LISPUI_EV_CANCEL,       /* Esc in a text field */
    LISPUI_EV_CONTEXT,      /* a long press or a right click: id, index, x, y */
    LISPUI_EV_TAP,          /* a tap on no widget: x, y */
    LISPUI_EV_KEY,          /* a key no widget took: value the key (kernel/keyseq.h), index the mods */
    LISPUI_EV_RESIZE,       /* the window's content is x by y now: draw again */
    LISPUI_EV_CLOSE,        /* the window was closed */
} lispui_ev_type_t;

typedef struct {
    uint8_t  type;          /* lispui_ev_type_t */
    uint16_t id;
    int16_t  index;
    int16_t  x, y;
    int32_t  value;
} lispui_ev_t;

/* A window titled `title` (focused if there is one already); false where
 * there is no screen, or no room for a window. Forgets the last picture. */
bool lispui_open(const char *title);
/* Closes it (a CLOSE event follows). */
void lispui_close(void);
bool lispui_is_open(void);
/* Its content's size now; false when it is not open. */
bool lispui_size(int *w, int *h);

/* The display list: lispui_add() is false when the list is full (the record
 * is dropped, and lispui_end() says so by returning false too). */
void lispui_begin(void);
bool lispui_add(lispui_op_t op, const int32_t *nums, int nnums, const char *const *strs, int nstrs);
bool lispui_end(void);

/* The next event, if there is one. */
bool lispui_next_event(lispui_ev_t *ev);

/* A text field's text (into out, cap bytes with the NUL; "" for none), and
 * setting it (the caret goes to its end). */
void lispui_text_get(uint16_t id, char *out, uint32_t cap);
bool lispui_text_set(uint16_t id, const char *text);

/* For `ui` in the shell and the tests: what the window is and holds. */
void lispui_report(void);

#endif /* LUGALOS_DRIVERS_LISPUI_H */
