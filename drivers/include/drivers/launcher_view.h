/* drivers/include/drivers/launcher_view.h -- the launcher (RIBBON_WIN_LAUNCHER),
 * the home view: its Shortcuts face (drivers/shortcuts.h) and its Files
 * face (the mounted volumes and their directories). drivers/launcher_view.c
 * says how each works; doc/user/launcher.md is the user's side. */

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
/* Press feedback: the control under a finger that went down shows pressed
 * until launcher_release(). */
void     launcher_press(int client_x, int client_y);
void     launcher_release(void);
void     launcher_scroll(int delta_y);
/* A finger dragging over the launcher: the face's list or grid follows it, and
 * glides on when it lifts still moving (dy over its last ms). */
void     launcher_drag(int dy);
void     launcher_drag_end(int dy, uint32_t ms);
/* The control gallery in place of the launcher's content (`launcher
 * gallery`; its Done button ends it): every ugui control, by touch and keys. */
void     launcher_gallery(bool on);
/* A dialog over the launcher (title, message lines split at '\n', OK) --
 * for what it could not do. */
void     launcher_alert(const char *title, const char *msg);
/* An alert over the launcher (opened and focused for it): an icon by name
 * (drivers/icons.h, NULL none), its title and message, and buttons from
 * the right, the last the default (Enter), `cancel` Esc's; done(choice,
 * arg) once it is answered. One at a time: a new one replaces it. The
 * buttons must outlive it. On the views task. */
typedef void (*launcher_ask_fn)(int choice, void *arg);
void     launcher_ask(const char *icon, const char *title, const char *msg, const char *const *buttons, int count,
                      int cancel, launcher_ask_fn done, void *arg);
/* That alert taken away unanswered (what it asked about settled itself). */
void     launcher_ask_dismiss(launcher_ask_fn done, void *arg);
/* Whether a finger down at the point is on a control that follows it (a
 * slider), and that finger's moves while it is captured. */
bool     launcher_drag_target(int client_x, int client_y);
void     launcher_pointer_move(int client_x, int client_y);
bool     launcher_poll_exec(char *out_cmd, size_t max_len);
uint8_t *launcher_get_aa_backing(uint32_t *out_stride, int *out_w, int *out_h);
uint32_t launcher_get_seq(void);
void     launcher_report(void);

#endif /* LUGALOS_DRIVERS_LAUNCHER_VIEW_H */
