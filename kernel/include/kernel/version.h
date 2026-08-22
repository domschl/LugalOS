#ifndef LUGALOS_KERNEL_VERSION_H
#define LUGALOS_KERNEL_VERSION_H

/* Single source of truth for the LugalOS version string, printed by both
 * the boot banner (kernel/main.c) and /proc/version (fs/vfs_server.c).
 * Bump here; nowhere else in C source should hardcode a version number. */
#define LUGALOS_VERSION_MAJOR 0
#define LUGALOS_VERSION_MINOR 12
#define LUGALOS_VERSION_PATCH 0
#define LUGALOS_VERSION "0.12.0"

/* Git-derived build identifier: "<commit-count>.<short-hash>", with a '+'
 * suffix when the tree had uncommitted changes. Generated per build by
 * cmake/gen_build_id.cmake.
 *
 * Reported in the boot banner and /proc/version so a running board can be
 * matched against a local build. That is not cosmetic: tests/hw/ talks to
 * real hardware, and "is this board running the firmware I just built?" was
 * previously only answerable by noticing that some feature was missing --
 * which cost a full flash-and-measure cycle to discover. */
#include "lugalos_build_id.h"

#define LUGALOS_VERSION_FULL LUGALOS_VERSION " (build " LUGALOS_BUILD_ID ")"

#endif /* LUGALOS_KERNEL_VERSION_H */
