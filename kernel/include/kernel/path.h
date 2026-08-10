#ifndef LUGALOS_KERNEL_PATH_H
#define LUGALOS_KERNEL_PATH_H

#include <stdint.h>

/* The search path (C1, plan/phase6_memory_and_processes.md).
 *
 * A volume may carry two well-known directories:
 *
 *     /<vol>/system/bin/    ELF executables, reachable by bare name
 *     /<vol>/system/etc/    Lisp scripts (init.lisp, stdlib.lisp)
 *
 * Typing `uhello` at the shell runs the first `/<vol>/system/bin/uhello.elf`
 * found by walking the path in order. A path with a '/' in it is always taken
 * literally, so a specific build can still be named exactly.
 *
 * ## Order, and the trust decision in it
 *
 * The default is ram0, sd0, flash0 -- most volatile first, so a utility
 * dropped on the RAM disk shadows the one shipped in flash. That is the point
 * (`cc` writing a new `cc.elf` should be usable immediately), but it is worth
 * saying plainly rather than discovering later: **anything able to write
 * /ram0/system/bin can shadow every system utility on the machine.** For a
 * single-user board that is the desired override semantics. It would not be
 * on a machine with a trust boundary, and the order is settable precisely so
 * that decision can be made per board rather than compiled in.
 *
 * ## Why it is a variable
 *
 * Set from init.lisp via (path-set ...) and readable at /proc/path, rather
 * than being a constant here. Which volumes exist, and which should win, is a
 * property of a board's assembly -- the same reason B0 moved the device list
 * into a per-board table and B4 made console ownership a binding. A constant
 * in this file would be one more thing to fork per target.
 */

#define PATH_MAX_VOLUMES 4
#define PATH_VOLUME_MAX  16

/* Installs the default order (ram0, sd0, flash0). Call once at boot, before
 * anything can resolve a name. */
void path_init(void);

/* Replaces the path from a space-separated list of volume names, with or
 * without surrounding slashes ("ram0 sd0" and "/ram0/ /sd0/" are the same).
 * Returns the number of volumes accepted, or -1 if the spec was unusable --
 * in which case the previous path is left intact rather than the machine
 * being left with no path at all. */
int path_set(const char *spec);

int path_count(void);
const char *path_volume(int index);

/* Renders the path as a space-separated list, for /proc/path. Returns the
 * number of bytes written. */
int path_format(char *buf, uint32_t cap);

/* Resolves `name` to a full path by trying /<vol>/system/<subdir>/<name><suffix>
 * for each volume in order, and returns 0 having filled `out` with the first
 * one that exists. Returns -1 if no volume has it.
 *
 * `suffix` is appended verbatim -- ".elf" for programs, "" for scripts -- so
 * the shell can accept `uhello` for `uhello.elf` without the caller having to
 * paste extensions together itself. */
int path_resolve(const char *subdir, const char *name, const char *suffix,
                 char *out, uint32_t out_len);

#endif /* LUGALOS_KERNEL_PATH_H */
