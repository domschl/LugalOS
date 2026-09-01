#ifndef NET_NTP_H
#define NET_NTP_H

#include <stdint.h>
#include <stdbool.h>
#include "net/ip.h"

/* SNTP client (R6, plan/phase19_ip_stack_and_ethernet.md).
 *
 * The first thing that makes this network useful to a *persona* rather than
 * to a host: the phase 17 clock can set its own time from the segment instead
 * of being told it. UDP has been waiting for exactly this since R2 --
 * net/udp.c's own comment names an NTP client as one of the four bindings it
 * reserved.
 *
 * Client only, and unicast only. A server (this board answering, disciplined
 * by DCF-77) is phase 24, not this: it needs a continuously disciplined clock
 * rather than a protocol, and the protocol is the small half.
 *
 * **What this deliberately is not.** No polling loop, no clock filter, no
 * frequency discipline, no server selection among several, no broadcast or
 * manycast mode, no NTS or symmetric-key authentication. This asks one
 * server once, applies the answer as a step, and says what it did. That is
 * what RFC 4330 calls an SNTP client, and stating the limit here is the
 * point -- an unstated limit gets credited as a feature (§7 of the phase 19
 * plan).
 *
 * **Resolution is milliseconds, not microseconds**, because kernel/time.c's
 * wall clock is (`g_base_epoch_ms`). Over WiFi the round trip is tens of
 * milliseconds anyway, so the clock's representation is not the limit today
 * -- but it *is* the first thing phase 24 has to change, and the arithmetic
 * below is written in signed 64-bit milliseconds so that widening it is a
 * change of unit rather than a change of shape.
 */

typedef struct {
    uint8_t  server[IPV4_LEN];
    uint8_t  stratum;      /* 1 = a reference clock is directly attached */
    uint8_t  leap;         /* 0 none, 1 +1s, 2 -1s at the end of the month */
    char     refid[5];     /* stratum 1: four ASCII chars ("GPS", "DCF", "PPS") */
    bool     refid_is_ip;  /* stratum >= 2: refid is the upstream's address */
    uint8_t  refip[IPV4_LEN];
    int64_t  offset_ms;    /* add this to our clock to agree with the server */
    int64_t  delay_ms;     /* round trip, excluding the server's own think time */
} ntp_result_t;

/* Ask one server, do not touch any clock. Returns 0 on a usable answer, or a
 * negative NTP_ERR_* below. Runs on the caller's task and yields while it
 * waits, because `netsrv` is what will deliver the reply -- the same shape as
 * (net-mount)'s handshake wait. */
int ntp_query(const uint8_t server[IPV4_LEN], uint32_t timeout_ms, ntp_result_t *out);

/* ntp_query(), and on success step the kernel clock and the DS3231 by the
 * offset it found. Nothing is written when the query fails, matching
 * drivers/dcf77_service.c: a clock holding a slightly wrong time is
 * categorically better than one holding a garbage time. */
int ntp_sync(const uint8_t server[IPV4_LEN], uint32_t timeout_ms, ntp_result_t *out);

#define NTP_ERR_NO_NET      -1  /* no interface, or no address configured */
#define NTP_ERR_BUSY        -2  /* could not bind a source port */
#define NTP_ERR_SEND        -3  /* the datagram did not leave (usually no ARP route) */
#define NTP_ERR_TIMEOUT     -4  /* nothing came back */
#define NTP_ERR_MALFORMED   -5  /* came back, was not an answer to our question */
#define NTP_ERR_UNSYNCED    -6  /* the server says its own clock is not set */
#define NTP_ERR_KISS        -7  /* kiss-o'-death: stratum 0, refid says why */
#define NTP_ERR_INTERRUPTED -8  /* Ctrl-C */

/* Human-readable form of the codes above, for a caller that has to explain
 * itself to a person. */
const char *ntp_err_str(int rc);

/* The report behind `ntp` and `(ntp-sync)`: server, stratum, refid, offset
 * and round trip, on one line each. */
void ntp_print_result(const ntp_result_t *r, bool applied);

#endif /* NET_NTP_H */
