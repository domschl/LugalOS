#ifndef LUGALOS_KERNEL_EDITOR_H
#define LUGALOS_KERNEL_EDITOR_H

#include <stdint.h>

/* `e` -- the editor, and the writer. 37.5b, plan/phase37_screen_layouts_and_apps.md.
 *
 * Full screen in the text tile: it fills whatever size the console reports
 * (80 x 24 on a serial line, which reports none), follows a resize -- the
 * divider moved with Super+[ / ] while it runs -- and redraws only the rows
 * that changed, scrolling with the terminal's scroll region rather than
 * repainting (drivers/vtterm.c understands it since 37.5b).
 *
 * Two modes, chosen by the file's extension and switched with Ctrl-X Ctrl-T:
 *
 *   code   line numbers, long lines scroll sideways, Enter keeps the indent
 *   text   the writer (.txt, .md): soft wrap at word boundaries, no line
 *          numbers -- a paragraph is one line on disk, so the file reads
 *          the same in any editor on a PC
 *
 * The keys are the line editor's, in both families: Emacs and Super (Cmd).
 * Movement: arrows, Ctrl-B/F/P/N, Home/End, Ctrl-A/E, PgUp/PgDn, Ctrl-V and
 * Alt-V, words with Alt-B/F or Ctrl-arrows, Alt-< and Alt-> (Ctrl-Home/End,
 * Super+Up/Down) for the ends of the buffer, Alt-G (Super+L) to a line.
 * Selection: Shift with a movement, or Ctrl-Space then movement; Ctrl-G or
 * Esc ends it. Clipboard: Ctrl-W / Super+X cut, Alt-W / Super+C copy,
 * Ctrl-Y / Super+V paste, Super+A (Ctrl-X H) all, Ctrl-K kills to the end of
 * the line (consecutive kills collect). Undo: Ctrl-_ / Super+Z / Ctrl-X U.
 * Search: Ctrl-S / Ctrl-R / Super+F incremental, Super+G and Super+Shift+G
 * again; replace: Alt-% / Super+R, each match confirmed (y n ! . q).
 * Files: Ctrl-X Ctrl-S (Super+S) save, Ctrl-X Ctrl-W (Super+Shift+S) save
 * as, Ctrl-X Ctrl-F (Super+O) open, Ctrl-X I insert. Ctrl-X Ctrl-E
 * (Super+Enter) evaluates -- the selection if there is one -- and stays;
 * Ctrl-X Ctrl-C (Super+Q) leaves.
 *
 * **Saving is safe:** the text goes to a copy first -- the file's name with
 * the extension's first letter made '~' (notes.~xt), an 8.3 name of its own
 * on the card -- and then to the file, and the copy is removed only once
 * both writes succeeded, so a complete text -- old or new -- is on the card
 * at every moment. A failed save says
 * so and keeps the buffer, and its modified mark, in memory.
 *
 * While it runs, the kernel log is not printed (it goes on into the ring,
 * /proc/kmsg); a question asked with the canvas full screen brings the text
 * back first. */

typedef struct {
    /* Evaluates `src` (NUL-terminated) and leaves a one-line account of it
     * -- the value, or the error -- in `msg`. NULL: Ctrl-X Ctrl-E ends the
     * editor and hands the text back instead (the shell's Ctrl-X box). */
    void (*eval)(const char *src, char *msg, uint32_t cap);
} editor_hooks_t;

/* Edits `filename`. With hooks, returns -1 when the user leaves. Without,
 * Ctrl-X Ctrl-E copies the text (at most out_max - 1 bytes) to `out` and
 * returns its length; leaving returns -1. Allocates the buffer, its undo
 * (8 KB, on the first edit) and its state on the heap, and frees them all. */
int editor_run(const char *filename, const editor_hooks_t *hooks, char *out, int out_max);

/* `editselftest`: wrapping, cursor movement over wrapped rows and UTF-8,
 * undo, search and the safe save, on a RAM buffer and /ram0. Prints
 * EDITOR_SELFTEST_OK/_FAIL; returns the failures. */
int editor_selftest(void);

#endif /* LUGALOS_KERNEL_EDITOR_H */
