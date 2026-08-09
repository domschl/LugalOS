#ifndef LUGALOS_KERNEL_CONSOLE_H
#define LUGALOS_KERNEL_CONSOLE_H

#include <stdint.h>
#include <stdbool.h>

/* The console stream (B4, plan/phase5_distributed_design.md §5.4).
 *
 * B0 gave the kernel log a ring and detachable sinks, and recorded a
 * limitation it could not fix at the time: printk() carried *two* unrelated
 * things -- kernel diagnostics and user-facing output (shell results, the
 * Lisp REPL, editor screens). So `klog detach console` silenced the shell
 * along with the log, which is not what anyone wants and not the §5.2
 * scenario this track exists to deliver.
 *
 * The two streams are now separate:
 *
 *   printk()   -- kernel diagnostics. Goes to the klog ring and its sinks.
 *   cprintf()  -- user-facing output. Goes to whatever device the console is
 *                 bound to, and is never affected by log-sink changes.
 *
 * Detaching the log sink now stops `[Sched] ...` lines from reaching the
 * terminal while the shell keeps working, and the log keeps accumulating in
 * the ring for /proc/kmsg either way. That is the whole point.
 *
 * The console is *bindable* for the same reason the log sinks are: a channel
 * may carry kernel output until something else should own it. Binding it to
 * a different device is what B4's later work (a console server reachable
 * through a channel) builds on.
 */

typedef void (*console_putc_fn)(char);

/* Binds the console stream to an output function. Passing NULL discards
 * output rather than crashing -- a console bound to nothing is a legitimate
 * state (the device was handed to something else), not an error. */
void console_bind(console_putc_fn putc);

void console_putc(char c);
void console_puts(const char *s);

/* Formatted user-facing output. Same format engine as printk(); the
 * difference is only which stream it lands on. */
int cprintf(const char *fmt, ...);

#endif /* LUGALOS_KERNEL_CONSOLE_H */
