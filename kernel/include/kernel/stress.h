#ifndef LUGALOS_KERNEL_STRESS_H
#define LUGALOS_KERNEL_STRESS_H

#include <stdbool.h>
#include <stdint.h>

/* `stress [seconds]` (kernel/stress.c): worker tasks on the shared paths for
 * `seconds` (0: 60), then a report and STRESS_OK / STRESS_FAIL. 0 or -1. */
int stress_run(uint32_t seconds);
/* The Tab5's panel rotation task in the next run (on by default). */
void stress_set_rotation(bool on);
/* `only=fmt,ram,sd,p9,chan,log,shell`: the paths the next run picks from
 * (NULL or no `only=`: all). */
void stress_set_paths(const char *list);

/* `stress bg [seconds]`: the same run as a task of its own, so the console
 * stays free (a host harness types into it meanwhile); the report is printed
 * when it ends. */
void stress_start_bg(uint32_t seconds);

#endif /* LUGALOS_KERNEL_STRESS_H */
