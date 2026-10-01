#ifndef LUGALOS_KERNEL_LINE_EDITOR_H
#define LUGALOS_KERNEL_LINE_EDITOR_H

#include <stdbool.h>

void line_editor_init(void);
int readline_interactive(const char *prompt, char *out_buf, int max_len);

/* 37.5a, plan/phase37_screen_layouts_and_apps.md: the same editor for every
 * other text input -- `ed`'s lines, the `lisp` REPL, `e`'s prompts -- so that
 * all of them have the same keys, UTF-8, selection and the clipboard.
 *
 *   no_history     not added to, and Up/Down/Ctrl-X do not reach, the
 *                  shell's history and multi-line editor
 *   cancel_on_esc  Esc or Ctrl-G with nothing selected returns -1
 *   no_newline     Enter does not move to a new line (a status-line prompt)
 *
 * NULL is the shell's: history, no cancel, a newline. Returns the line's
 * length, or -1 if cancelled. */
typedef struct {
    bool no_history;
    bool cancel_on_esc;
    bool no_newline;
} readline_opts_t;

int readline_ex(const char *prompt, char *out_buf, int max_len, const readline_opts_t *opts);

/* 37.5a: called about ten times a second while a line waits for its next key
 * -- the shell sets Lisp's canvas redraw check (lisp_canvas_poll()), so a
 * canvas lost to a split hotkey is redrawn at once. NULL for none. */
void readline_set_idle(void (*fn)(void));

/* 37.5b: one turn of that hook, for the editor's own key wait. */
void readline_idle(void);

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
/* The shell's Ctrl-X Ctrl-E box: kernel/editor.c without an evaluator, so
 * Ctrl-X Ctrl-E hands the text back (its length; 0 if the user left). */
int edit_multiline_box(const char *initial_filename, char *out_buf, int max_len);

#endif /* LUGALOS_KERNEL_LINE_EDITOR_H */

