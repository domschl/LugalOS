#ifndef LUGALOS_DRIVERS_UXBENCH_H
#define LUGALOS_DRIVERS_UXBENCH_H

/* `uxbench [doc]`: the ribbon's interactions timed from input to glass
 * (drivers/uxbench.c). doc is the reader document, "shell.md" when NULL or
 * empty. Runs in the calling task. */
void uxbench_cmd(const char *arg);

#endif /* LUGALOS_DRIVERS_UXBENCH_H */
