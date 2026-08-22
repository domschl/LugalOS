#ifndef LUGALOS_KERNEL_LINE_EDITOR_H
#define LUGALOS_KERNEL_LINE_EDITOR_H

#include <stdbool.h>

void line_editor_init(void);
int readline_interactive(const char *prompt, char *out_buf, int max_len);

/* Non-blocking line reader, sharing readline_interactive()'s editor exactly
 * (same history, cursor movement, Home/End/Delete, Ctrl-X multiline escape --
 * see kernel/line_editor.c, where the two are one body with two drivers).
 *
 * Consumes only input that is already available. Returns the completed line's
 * length, or a negative value if the line is still being typed -- in which
 * case call again later; the partial text stays in `out_buf`.
 *
 * One caller at a time, always the same buffer: the editing state is file
 * scope. That suits its purpose (an event loop that must also poll other
 * input sources) rather than limiting it. */
int readline_poll(const char *prompt, char *out_buf, int max_len);

/* Abandons a half-typed line, so the next readline_poll() starts fresh and
 * redraws its prompt. */
void readline_poll_reset(void);

/* Whether a partially typed line is currently outstanding. Lets a caller
 * avoid stepping on the user's in-progress input when it wants to redraw
 * something else. */
bool readline_poll_active(void);
int edit_multiline_box(const char *initial_filename, char *out_buf, int max_len);

#endif /* LUGALOS_KERNEL_LINE_EDITOR_H */

