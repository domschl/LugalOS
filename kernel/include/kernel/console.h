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

/* What a DEV_KIND_CONSOLE device hands back from its get(). A struct rather
 * than a bare function pointer because dev_get() returns void*, and ISO C
 * has no conforming conversion between object and function pointers. */
typedef struct {
    console_putc_fn putc;
} console_dev_t;

/* Binds the console stream to an output function. Passing NULL discards
 * output rather than crashing -- a console bound to nothing is a legitimate
 * state (the device was handed to something else), not an error. */
void console_bind(console_putc_fn putc);

void console_putc(char c);
void console_puts(const char *s);

/* Formatted user-facing output. Same format engine as printk(); the
 * difference is only which stream it lands on. */
int cprintf(const char *fmt, ...);

/* --- The console as a server (B4) ---
 *
 * Two things make this a server rather than a renamed printf:
 *
 *   1. It is reachable through a channel. `/srv/console` is a chan endpoint,
 *      so any task -- or a remote node over 9P, since /srv/ is in the
 *      namespace -- can send it output using the same copy-always IPC as
 *      every other service. Nothing about writing to the console requires
 *      being the kernel, or being on this machine.
 *
 *   2. Its device is bound by NAME at runtime, from the device registry, so
 *      init.lisp decides who owns the terminal. That is §5.2's scenario:
 *      a channel carries kernel output until something else should have it.
 *
 * Local cprintf() deliberately stays a direct call rather than a channel
 * round trip. Routing every character through a rendezvous would make output
 * a scheduling event -- unusable from a fault handler, and a large cost for
 * no isolation gain while the console driver is kernel code anyway. The
 * server-ness here is about reachability and ownership, not about forcing
 * every byte through a queue. B6's preemption is what would change that
 * calculus, and it is called out in the plan rather than assumed away. */

/* Registers the "console" channel endpoint. Call once at boot, after the
 * device registry is populated. */
int console_server_init(void);

/* Binds the console stream to a named DEV_KIND_CONSOLE device from the
 * registry. Returns 0, or -1 if no such device is present. */
int console_bind_device(const char *name);

/* The name currently bound, or "(none)". */
const char *console_bound_device(void);

#endif /* LUGALOS_KERNEL_CONSOLE_H */
