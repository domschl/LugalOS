#ifndef LUGALOS_KERNEL_APPS_H
#define LUGALOS_KERNEL_APPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The applications and what opens what (kernel/apps.c).
 *
 * Two tables in .rodata, sorted for bsearch: the applications by id, and the
 * file associations by extension. The launcher, `open` and (later) the Files
 * face and shortcuts all resolve through them, so a file opens the same way
 * from everywhere.
 *
 * An application starts as a shell command line: `exec`, with the path it
 * opens in place of %s (dropped, with the space before it, when there is
 * none). Built-in views take this place when the view framework lands
 * (71.4). */

#define APP_TOUCH      0x01u    /* usable by touch alone */
#define APP_KBD        0x02u    /* usable by keyboard alone */
#define APP_FULLSCREEN 0x04u    /* takes the whole screen (the games) */
#define APP_SINGLETON  0x08u    /* one at a time: opening it again goes to the running one */
#define APP_WINDOW     0x10u    /* opens a window of its own (a canvas): needs room on the ribbon */

typedef struct {
    const char *id;             /* "chess", "editor", "moonbase", "reader", "term" */
    const char *title;
    const char *exec;           /* the command line; %s: the path */
    const char *icon;           /* an icon name (71.4's icons); the generic one when absent */
    const char *window;         /* a singleton's window: its canvas name or title prefix */
    uint16_t    caps;           /* APP_* */
} app_desc_t;

/* The application called id, or NULL. */
const app_desc_t *app_find(const char *id);

/* The application that opens path, by its extension (case does not matter:
 * FAT names match in any case, and 8.3 names may come in capitals), or NULL. */
const app_desc_t *app_for_path(const char *path);

/* a's command line for path (NULL: none) into out; false if it does not fit. */
bool app_command(const app_desc_t *a, const char *path, char *out, size_t cap);

/* The applications in id order, for listings: i from 0 until NULL. */
const app_desc_t *app_at(unsigned i);

/* The association i (its extension in *ext), until NULL. */
const app_desc_t *app_assoc_at(unsigned i, const char **ext);

/* `apps check`: both tables sorted (the lookups search them), every
 * association naming an application, every application found by its id.
 * Returns the faults. */
int apps_check(void);

#endif /* LUGALOS_KERNEL_APPS_H */
