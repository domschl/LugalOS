#ifndef NET_TCP_H
#define NET_TCP_H

#include <stdint.h>
#include <stdbool.h>
#include "net/ip.h"

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

void tcp_input(const uint8_t src[IPV4_LEN], const uint8_t *ip_hdr,
               const uint8_t *seg, uint32_t len);

/* For /proc/net and `net`. */
bool tcp_listening(uint16_t *port_out);
uint32_t tcp_conn_count(void);
void tcp_conn_str(uint32_t index, char *out, uint32_t max);
uint32_t tcp_accepted_total(void);
uint32_t tcp_reset_total(void);

#endif // NET_TCP_H
