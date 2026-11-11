#ifndef LUGALOS_DRIVERS_UXBENCH_H
#define LUGALOS_DRIVERS_UXBENCH_H

#include <stdint.h>

/* `uxbench [doc [dir]]`: the ribbon's interactions timed from input to glass
 * (drivers/uxbench.c). doc is the reader document, "shell.md" when NULL or
 * empty; dir the directory the Files face is measured in, "/sd0" when not
 * given. Runs in the calling task. */
void uxbench_cmd(const char *arg);

/* A view's drawing pass, for `uxbench`'s stages: t_wait..t_draw waiting for
 * the compositor to let go of the buffer, t_draw..t_copy the view's own
 * drawing (ugui_begin() to ugui_end()), t_copy..t_end its request that the
 * screen take the picture (`'W'`). Times from time_get_us(). On the views
 * task only; does nothing unless `uxbench` is running. */
void uxbench_view_note(uint64_t t_wait, uint64_t t_draw, uint64_t t_copy, uint64_t t_end);

#endif /* LUGALOS_DRIVERS_UXBENCH_H */
