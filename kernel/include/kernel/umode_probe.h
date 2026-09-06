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

#endif /* LUGALOS_KERNEL_UMODE_PROBE_H */
