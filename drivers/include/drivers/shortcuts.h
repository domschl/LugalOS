#ifndef LUGALOS_DRIVERS_SHORTCUTS_H
#define LUGALOS_DRIVERS_SHORTCUTS_H

#include <stdbool.h>
#include <stdint.h>

/* The launcher's Shortcuts face (drivers/shortcuts.c): an ordered list of
 * tiles, each an application, a document, a directory or a book.
 *
 * Kept in /sd0/system/shortcuts -- /flash0/system/shortcuts where there is
 * no card -- one per line, `type<TAB>target<TAB>label<TAB>icon`, types
 * `app`, `doc`, `dir`, `book`; the icon is a built-in icon's name (empty:
 * the target's own). With neither file, built-in defaults: the applications
 * a person starts by touch, the help, the documents folder. Every change is
 * written at once (a shortcut is changed by hand, rarely).
 *
 * single task: views (the launcher), and the shell through view_call(). */

#define SHORTCUT_MAX    32
#define SHORTCUT_TARGET 112
#define SHORTCUT_LABEL  40
#define SHORTCUT_ICON   16

typedef enum { SHORTCUT_APP, SHORTCUT_DOC, SHORTCUT_DIR, SHORTCUT_BOOK } shortcut_type_t;

typedef struct {
    uint8_t type;                       /* shortcut_type_t */
    char    target[SHORTCUT_TARGET];    /* an app id, or a path */
    char    label[SHORTCUT_LABEL];
    char    icon[SHORTCUT_ICON];        /* "" for the target's own */
} shortcut_t;

/* Read again from the file, or the defaults. */
void shortcuts_load(void);
int  shortcuts_count(void);
const shortcut_t *shortcuts_at(int i);
/* The icon to draw for s: its own, or by its type and target. */
const char *shortcuts_icon(const shortcut_t *s);
/* A built-in icon for a file by its extension ("doc-md"), or a folder's. */
const char *shortcuts_path_icon(const char *name, bool dir);

/* Each saves at once; false when it cannot (full, out of range, no room on
 * any volume). add() appends; an entry with the same type and target is
 * not added twice (it is still true: it is there). */
bool shortcuts_add(shortcut_type_t type, const char *target, const char *label, const char *icon);
bool shortcuts_remove(int i);
bool shortcuts_move(int i, int by);
bool shortcuts_rename(int i, const char *label);
/* The defaults back, the file removed. */
bool shortcuts_reset(void);

/* The type's name ("app"), and the type for a name (-1: none). */
const char *shortcuts_type_name(shortcut_type_t t);
int shortcuts_type_of(const char *name);

/* `shortcuts`: the list; `shortcuts add TYPE TARGET [LABEL]`, `remove N`,
 * `move N BY`, `rename N LABEL`, `reset`. From the shell. */
void shortcuts_cmd(const char *args);

#endif /* LUGALOS_DRIVERS_SHORTCUTS_H */
