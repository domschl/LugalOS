#ifndef LUGALOS_KERNEL_KLOG_H
#define LUGALOS_KERNEL_KLOG_H

#include <stdint.h>
#include <stdbool.h>

/* Kernel log ring + output sink registry (B0, plan/phase5_distributed_design.md §5.4).
 *
 * Before this existed, printk() called uart_putc() directly, so kernel log
 * output had exactly one destination decided at compile time and no history:
 * the moment a UART was repurposed as a 9P transport (`p9serve`) or handed to
 * a login shell, the log was simply gone. That is the first of the three
 * concrete blockers §5.2 measures, and it needs no scheduler to fix.
 *
 * Two independent things live here:
 *
 *  1. The ring -- every byte printk() emits is retained in memory regardless
 *     of whether any sink is attached. This is what makes detaching a sink
 *     non-destructive, and it is what /proc/kmsg serves (so kernel logs are
 *     readable over 9P from another node, which they never were before).
 *
 *  2. The sink registry -- zero or more live output destinations, each
 *     attachable/detachable at runtime. Boot registers the console sink, so
 *     default behavior is byte-identical to the pre-B0 direct-to-UART path.
 *
 * No locking: this kernel is still single-call-stack (kernel/sched.c is a
 * bookkeeping shim, not a scheduler). B2 introduces tasks and must revisit
 * every function here -- klog_write() in particular becomes a critical
 * section the moment two tasks can printk() concurrently.
 */

#define KLOG_RING_SIZE 4096
#define KLOG_MAX_SINKS 4

typedef void (*klog_putc_fn)(char);

/* Appends to the ring and fans out to every attached sink. */
void klog_write(const char *s, uint32_t len);
void klog_putc(char c);

/* --- Sink registry --- */

/* Attaches `putc` under `name`. Re-registering an existing name replaces its
 * function (and re-attaches it if detached). Returns 0 on success, -1 if the
 * table is full. */
int klog_sink_register(const char *name, klog_putc_fn putc);

/* Detaches by name -- the sink stops receiving output, but the ring keeps
 * accumulating, so nothing is lost and it can be re-attached later. This is
 * the operation that makes handing a UART to 9P or to a login shell safe.
 * Returns 0 on success, -1 if no such sink. */
int klog_sink_detach(const char *name);
int klog_sink_attach(const char *name);

/* Enumeration, for `klog` / (klog-sinks) and /proc. `index` is 0-based;
 * returns false once exhausted. */
bool klog_sink_info(uint32_t index, const char **name_out, bool *attached_out);

/* --- Ring readback --- */

/* Total bytes ever written, monotonic. Bytes below
 * (klog_total() - KLOG_RING_SIZE) have been overwritten and are gone. */
uint64_t klog_total(void);

/* Oldest absolute position still retained in the ring. */
uint64_t klog_oldest(void);

/* Copies up to `count` bytes starting at absolute position `abs_offset` (in
 * the same coordinate space as klog_total()/klog_oldest()) into `buf`.
 * Returns the number of bytes copied, or 0 once `abs_offset` reaches the end
 * of what the caller asked for or the ring no longer holds it.
 *
 * Absolute positions rather than a 0-based cursor are deliberate: /proc/kmsg
 * snapshots [oldest, total) at open time and serves only that window, so log
 * output produced *by the act of reading the log* (cat's own printk() calls)
 * cannot feed back into the read and never terminate. */
uint32_t klog_read(uint64_t abs_offset, char *buf, uint32_t count);

#endif /* LUGALOS_KERNEL_KLOG_H */
