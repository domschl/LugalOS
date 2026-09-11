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

/* 8 KB since Y5a (plan/phase31_concurrency_hierarchy.md §5.3), and the size
 * is a measurement rather than a round number.
 *
 * At 4096 this ring had already wrapped *before the board finished booting*:
 * the live RP2350's /proc/kmsg began mid-word, its earliest surviving
 * timestamp was 0.461s, and its own boot banner was gone. rv32's minimal boot
 * to a shell prompt is 3916 bytes against the same 4096 -- 0.96x, which is to
 * say the history was full the moment it existed.
 *
 * That was tolerable while the ring was only history. Y5 makes it the
 * transport: a consumer drains it and the producer never blocks, so bytes
 * evicted before the consumer reaches them are bytes nobody ever sees. 8192
 * is ~2x the measured boot volume, which leaves room for the burst that is
 * guaranteed to happen on every power-up and has no consumer running for the
 * first part of it.
 *
 * One page on the RP2350, whose heap is 87 of them. Y5a's other half is the
 * terser boot output, which is worth more than any size here. */
#define KLOG_RING_SIZE 8192
#define KLOG_MAX_SINKS 4

/* The largest message a single record can carry, in payload bytes (Y5b).
 *
 * Formatting into a buffer before storing it means a fixed maximum, where
 * the old char-at-a-time path had none. 320 comes from measurement, not
 * taste: the longest kernel log line observed across the QEMU and hardware
 * suites is 185 bytes, and the longest printk format literal in the tree is
 * 230 (drivers/enc28j60_rp2350.c's SRAM loopback failure) before its
 * arguments are substituted.
 *
 * A message longer than this is truncated with a visible marker and counted
 * -- see klog_truncations(). Silently losing the tail of exactly the longest,
 * most detailed diagnostic in the tree is the one outcome worth ruling out. */
#define KLOG_REC_MAX 320

/* A record's timestamp, in milliseconds since boot, or KLOG_NO_TS.
 *
 * Stored as four binary bytes and rendered only when the log is read (Y5b,
 * plan/phase31_concurrency_hierarchy.md §5.7). The rendered form
 * "[    0.010] " is twelve bytes on every line -- 456 of rv32's 2645-byte
 * boot, 17% of the ring -- spent on a number that fits in four.
 *
 * uint32 milliseconds wraps at 49.7 days. The ring holds minutes, so a
 * wrapped timestamp misleads only for the one record either side of the
 * wrap; ordering within the ring is unaffected. Said here rather than
 * discovered on a node that has been up for seven weeks.
 *
 * KLOG_NO_TS means the message carries no timestamp -- the convention is
 * vprintk_to()'s, which prefixes one only when the format string starts with
 * a bracketed tag, so the banner and continuation lines have none. */
#define KLOG_NO_TS ((uint32_t)0xFFFFFFFFu)

typedef void (*klog_putc_fn)(char);

/* Appends one whole record and fans it out to every attached sink.
 *
 * One call, one record, one critical section: the atomicity that
 * printk_lock() used to provide by being held across a char-at-a-time
 * emission now comes from the append itself. `text` is the message without
 * its timestamp; `ms` is rendered in front of it on the way out. */
void klog_emit(uint32_t ms, const char *text, uint32_t len);

/* Ring only, no sink fan-out. For printk_critical(), which may not reach a
 * sink whose putc can block. See kernel/klog.c. */
void klog_record_text(uint32_t ms, const char *text, uint32_t len);

/* Counts one truncation. Called by the producer, because that is where the
 * loss actually happens: printk() formats into a KLOG_REC_MAX buffer, so a
 * message too long to fit is already cut by the time klog sees it, and klog
 * would otherwise record a perfectly ordinary full-length record. */
void klog_truncated(void);

/* Messages truncated at KLOG_REC_MAX since boot. `klog` reports it; a
 * nonzero value means some diagnostic lost its tail. */
uint32_t klog_truncations(void);

/* Records dropped because a single message exceeded what the ring itself
 * could hold. Distinct from eviction, which is normal and silent. */
uint32_t klog_drops(void);

/* Bytes the ring actually holds, and records in it (Y5b).
 *
 * Distinct from klog_total()/klog_oldest(), which count *rendered* bytes --
 * what a reader receives. The difference between the two is what the binary
 * timestamp buys: twelve rendered characters per stamped record stored as
 * four, minus the six-byte header. `klog` prints both, because "the ring
 * holds more than it appears to" is otherwise invisible. */
uint64_t klog_stored_bytes(void);
uint32_t klog_records(void);

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
