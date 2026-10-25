#ifndef LUGALOS_KERNEL_PRIVILEGE_H
#define LUGALOS_KERNEL_PRIVILEGE_H

#include <stdbool.h>

/* Work done on behalf of something other than the person at the console.
 *
 * `ea` (kernel/ea.c) runs the shell commands and Lisp an LLM's reply names,
 * and that reply arrives over the network. While such a tool call runs, the
 * task that runs it is *untrusted*, and the operations that are for a person
 * at the console refuse:
 *
 *   - shell commands that change keys, identity or network configuration,
 *     write memory or install firmware (kernel/shell.c's list)
 *   - the Lisp primitives that do the same (user/lisp/lisp.c)
 *   - writing under any `system/etc/` directory: the 9P keys, the release
 *     key's companions, and the boot scripts, where a write would persist
 *     past the session (fs/vfs_server.c)
 *
 * Per task: a second terminal's shell is unaffected while an agent runs.
 * Nests, so a tool call that runs a shell command that evaluates Lisp stays
 * untrusted throughout. */

void untrusted_enter(void);
void untrusted_leave(void);

/* True while the current task is inside untrusted_enter()/untrusted_leave(). */
bool untrusted_active(void);

/* When untrusted: prints "<what>: refused -- ..." and returns true. The
 * caller then does nothing. */
bool privileged_refused(const char *what);

/* For a write (open for writing or creating, remove) of `path`: true, with
 * the refusal printed, when untrusted and the path lies under a `system/etc`
 * directory (any volume; case-insensitive; "." and ".." resolved). */
bool privileged_path_refused(const char *path);

#endif
