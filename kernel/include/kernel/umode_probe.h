#ifndef LUGALOS_KERNEL_UMODE_PROBE_H
#define LUGALOS_KERNEL_UMODE_PROBE_H

/* Runs one U-mode task under a three-region memory domain, has it prove it
 * can reach what it was granted, and then has it touch something it was not.
 * The second half is the point: a driver running happily in U-mode shows only
 * that its allowed accesses are allowed.
 *
 * Reports PASS only if the task entered U-mode, used its own stack and its
 * granted block, attempted the forbidden access, did *not* survive it, and
 * was killed by the fault handler. See kernel/umode_probe.c. */
void umode_probe_run(void);

#include <stdint.h>

/* One 32-bit load from addr in a U-mode task whose domain grants only its stack, code and a block.
 * Returns 1 and the word read if the load completed, 0 if it faulted (the task was killed), -1 if it
 * could not run (no enforced domains, another probe running). */
int umode_probe_read(uintptr_t addr, uint32_t *value);

/* /proc/pmp's second half on the ESP32-C6: what such a task reaches (kernel/umode_probe.c). */
uint32_t umode_reach_render(char *buf, uint32_t cap);

#endif /* LUGALOS_KERNEL_UMODE_PROBE_H */
