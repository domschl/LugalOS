#ifndef NET_NTP_SERVER_H
#define NET_NTP_SERVER_H

/*
 * Serving time to the segment (P6,
 * plan/phase24_dcf77_precision_and_ntp_server.md).
 *
 * **The design principle is §4's, and it is the whole reason the protocol half
 * is not merely mechanical: a server that advertises a confidence it cannot
 * justify is worse than one that admits it is unsynchronised**, because
 * clients believe stratum numbers. A board that says stratum 1 while coasting
 * on a day-old radio frame does not degrade gracefully -- it hands every
 * client on the segment a lie with authority attached.
 *
 * So everything this replies with is derived from what the discipline loop
 * actually knows: stratum 1 only while genuinely tracking, root dispersion
 * that grows during holdover, and LI 3 / stratum 16 once holdover has gone on
 * long enough that the number would be dishonest.
 *
 * Deliberately absent in this version: no kiss-o'-death rate limiting (this
 * serves a home segment, and saying so is more honest than implementing a
 * mechanism nothing will trigger), no authentication, no IPv6.
 */

#include <stdbool.h>
#include <stdint.h>

/* Binds UDP 123. Returns 0, or negative if the port is taken. Safe to call on
 * a persona with no time source: it will simply always answer unsynchronised,
 * which is why the caller gates it rather than this. */
int ntp_server_start(void);

typedef struct {
    bool     running;
    uint32_t requests;      /* well-formed client packets answered */
    uint32_t malformed;     /* wrong length, version or mode */
    uint32_t unsync_replies;/* answered, but admitting we do not know */
    uint8_t  last_stratum;
    uint32_t last_disp_us;
} ntp_server_stats_t;

void ntp_server_status(ntp_server_stats_t *out);

#endif /* NET_NTP_SERVER_H */
