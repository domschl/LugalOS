#ifndef LUGALOS_KERNEL_UACCESS_H
#define LUGALOS_KERNEL_UACCESS_H

#include <stdint.h>

/* The syscall boundary (B3, plan/phase5_distributed_design.md §5.4).
 *
 * Before this, trap.c passed `frame->a1` and `frame->a2` straight into
 * printk(), vfs_read() and vfs_write(). With a task in U-mode that is a
 * confused-deputy hole: the kernel runs at a privilege level where PMP does
 * not restrict it, so a task that cannot read kernel memory itself could ask
 * the kernel to read it *on the task's behalf* and hand back the contents.
 * The restriction would be intact and completely bypassed.
 *
 * These functions close it in the only way that composes: validate the range
 * against the calling task's own memory domain, then COPY. The kernel never
 * dereferences a caller-supplied address.
 *
 * ## This is Rule 1 (§5.1) becoming checkable
 *
 * B1 imposed copy-always IPC and noted honestly that it could not be tested:
 * on a single address space, deleting the copies and passing pointers through
 * would have passed every test. That is no longer true. With U-mode and PMP
 * live, a pointer that escapes validation now names memory the caller cannot
 * reach, and the difference is observable.
 *
 * The copies happen even for unrestricted kernel callers, where they are
 * provably redundant -- same discipline, same reason as kernel/chan.h.
 */

/* Copy `len` bytes from user address `usrc` into `dst`. Returns 0, or -1 if
 * the caller's domain does not grant read over the whole range. */
int copy_from_user(void *dst, uintptr_t usrc, uint32_t len);

/* Copy `len` bytes from `src` out to user address `udst`. Returns 0, or -1 if
 * the caller's domain does not grant write over the whole range. */
int copy_to_user(uintptr_t udst, const void *src, uint32_t len);

/* NUL-terminated string variant. Copies at most `max` bytes including the
 * terminator and always NUL-terminates on success. Returns the length copied
 * (excluding the NUL), or -1 if the string is unreadable or unterminated
 * within `max`.
 *
 * The length is not known in advance, so the range cannot be validated in one
 * go: each byte is permission-checked before it is read. Slower, and the only
 * way to avoid either trusting a length the caller supplied or reading past
 * what it was allowed to. */
int strncpy_from_user(char *dst, uintptr_t usrc, uint32_t max);

#endif /* LUGALOS_KERNEL_UACCESS_H */
