#ifndef NET_TCP_H
#define NET_TCP_H

#include <stdint.h>
#include <stdbool.h>
#include "net/ip.h"
#include "fs/p9_link.h"

/* Server-side TCP (R3, plan/phase19_ip_stack_and_ethernet.md).
 *
 * The largest correctness surface in this project's history: a state machine
 * with ten states, parsing attacker-controlled bytes, whose bugs present as
 * "the transfer hangs sometimes". Every structural mitigation §8 named is
 * here, and they are the reason it is this small:
 *
 *   * **Passive open only.** No connect(). A server does not need one, and
 *     active open is R3b.
 *   * **No out-of-order reassembly.** A segment that does not start at
 *     rcv_nxt is dropped -- with a duplicate ACK, so the peer retransmits at
 *     once rather than after its own RTO. That costs throughput on a lossy
 *     link and saves the single largest RAM line item and bug surface in a
 *     small TCP. §2 records the trade.
 *   * **Two connections, one listener.**
 *   * **No options but MSS**, no window scaling, no SACK, no Nagle, no
 *     delayed ACK, no timestamps.
 *
 * **Everything runs in one task.** `netsrv` pumps the stack, runs the
 * retransmission timers, services 9P on established connections and drains
 * their send buffers -- in that order, on one call stack. Nothing here takes
 * a lock because nothing here is ever entered twice, and `send_frame()` never
 * blocks waiting for an acknowledgement it would have to be running to
 * receive. See tcp_service().
 */

/* Listens on `port`, replacing any previous listener. Allocates the
 * connections' buffers on first call (from palloc, like the stack's own
 * frames -- a board that never listens never pays). Returns 0 on success. */
int tcp_listen(uint16_t port);

/* Stops listening. Established connections are left alone: a server that
 * drops live sessions when its listener closes is surprising, and closing
 * them is what a reset is for. */
void tcp_unlisten(void);

/* The one call the pump makes: timers, then 9P service, then transmit. */
void tcp_service(void);

/* --- R3b: the active open ---
 *
 * Half the state machine was unreachable without it: SYN_SENT was never
 * entered, and FIN_WAIT_1/FIN_WAIT_2/CLOSING/TIME_WAIT only ever reachable
 * from the side that closes first, which nothing was. It is also what lets
 * one node mount another's namespace over IP instead of over a cable.
 *
 * tcp_connect() returns immediately with a link that is not yet usable --
 * blocking here would block whichever task also runs the pump that completes
 * the handshake. Poll tcp_link_ready() until it answers 1. */
p9_link_t *tcp_connect(const uint8_t ip[IPV4_LEN], uint16_t port);
int tcp_link_ready(p9_link_t *link);
void tcp_close(p9_link_t *link);

/* --- Q0: the same connection, as a stream of bytes ---
 *
 * Everything above hands out a `p9_link_t`, because when R3 was written the
 * only thing that spoke over a connection was 9P and its four-byte length
 * prefix was the framing. That assumption is in link_poll() and
 * link_recv_frame(), not in the state machine, so a protocol with framing of
 * its own (MQTT, Q1) needs a second *view* of a connection rather than a
 * second transport: same slot table, same buffers, same pump, no framing
 * applied.
 *
 * **No new memory.** A stream takes one of the TCP_MAX_CONNS slots and the
 * buffers ensure_bufs() already allocates, so a board that dials a broker
 * pays nothing it did not already pay to serve 9P. The flip side is that the
 * two-slot limit is now shared between the two: a board serving 9P to two
 * peers cannot also dial one. plan/phase26 §2 records that.
 *
 * **A stream connection has no p9_link_t face at all** -- conn_init_link()
 * is never called for one. Relying on `is_client` to keep the 9P server away
 * would conflate "we dialled" with "this is not 9P", and R3b's (net-mount)
 * dials 9P deliberately.
 *
 * Non-blocking throughout, for the reason tcp_connect() already is: the task
 * that would wait is often the one whose pump would have to run for the wait
 * to end. Callers loop with sched_yield(), and consult
 * console_interrupt_requested() while they do.
 */
/* **The caller owns the handle**, the way a driver owns its netif_t: a
 * static, as everywhere else in this kernel, and no allocation on this path.
 *
 * That is not only convention here, it is the aliasing fix. A pool of handles
 * indexed by connection slot would hand the *same* struct back on the next
 * open of that slot, so a caller still holding the old handle would read the
 * new connection's epoch out of it and its staleness check would pass -- the
 * exact hazard link_conn()'s epoch check exists to prevent, reintroduced one
 * level up. A handle in the caller's own memory keeps the epoch it was opened
 * with, so a slot that has since been reused is detected rather than
 * silently inherited.
 *
 * Treat the fields as private: they are here so the struct has a size. */
typedef struct tcp_stream {
    uint32_t index;      /* which connection slot */
    uint32_t epoch;      /* that slot's epoch when this handle was opened */
    bool     open;
} tcp_stream_t;

/* Dials `ip`:`port` into a caller-supplied handle. The stream is not usable
 * until tcp_stream_ready() says so. Returns 0, or -1 when there is no address
 * configured, no free slot, or no memory. */
int tcp_stream_open(tcp_stream_t *s, const uint8_t ip[IPV4_LEN], uint16_t port);

/* 1 established, 0 still handshaking, -1 refused, reset, or gone. A handle
 * whose slot has since been reused answers -1 rather than attaching itself to
 * a stranger's connection -- the epoch check link_conn() already makes. */
int tcp_stream_ready(tcp_stream_t *s);

/* Copies out what has arrived, in order. Returns the byte count (0 when
 * nothing is buffered yet -- not an error), or -1 when the connection is
 * gone. A peer that has closed still yields its buffered bytes first: read
 * until 0, then ask tcp_stream_peer_closed(). */
int tcp_stream_read(tcp_stream_t *s, uint8_t *buf, uint32_t max);

/* Appends to the send buffer whatever fits, and returns how many bytes that
 * was -- which may be fewer than `len`, or 0 when the buffer is full and the
 * peer has not acknowledged. That is not an error and callers must loop: the
 * task that drains the buffer is `netsrv`, so a caller that spins without
 * yielding will wait forever for a drain only it is preventing.
 * -1 when the connection is gone. */
int tcp_stream_write(tcp_stream_t *s, const uint8_t *buf, uint32_t len);

/* How many bytes tcp_stream_write() would take right now. */
uint32_t tcp_stream_writable(tcp_stream_t *s);

/* True once the peer's FIN has been received. Buffered bytes may still be
 * waiting -- this says the peer will send no more, not that there is no more
 * to read. */
bool tcp_stream_peer_closed(tcp_stream_t *s);

/* Graceful: a FIN, not a reset, and the handle is released either way. */
void tcp_stream_close(tcp_stream_t *s);

void tcp_input(const uint8_t src[IPV4_LEN], const uint8_t *ip_hdr,
               const uint8_t *seg, uint32_t len);

/* For /proc/net and `net`. */
bool tcp_listening(uint16_t *port_out);
uint32_t tcp_conn_count(void);
void tcp_conn_str(uint32_t index, char *out, uint32_t max);
uint32_t tcp_accepted_total(void);
uint32_t tcp_reset_total(void);

#endif // NET_TCP_H
