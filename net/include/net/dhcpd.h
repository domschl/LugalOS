#ifndef NET_DHCPD_H
#define NET_DHCPD_H

/* A DHCP server and a catch-all DNS responder for the setup access point (plan/phase52, 52.12).
 *
 * A phone that joins the setup network needs an address and a way to find the setup page without
 * typing one: dhcpd leases it one of four addresses from a /24 and names this node as router, DNS
 * server and captive portal (RFC 8910, option 114); dnsd answers every A query with this node's
 * address, which is what makes a phone's connectivity check land on the page.
 *
 * The packet handling is two pure functions (a request in, a reply out, no sockets), so it is
 * tested on the host without a radio; dhcpd_start()/dnsd_start() bind them to UDP 67 and 53. */

#include <stdint.h>
#include <stdbool.h>

#define DHCPD_LEASES      4u
#define DHCPD_LEASE_S     3600u
#define DHCPD_MAX_REPLY   320u

typedef struct {
    uint8_t  server_ip[4];          /* this node: router, DNS, DHCP server */
    uint8_t  mask[4];
    uint8_t  first_ip[4];           /* DHCPD_LEASES consecutive addresses start here */
    const char *portal_uri;         /* option 114, or NULL */
} dhcpd_config_t;

typedef struct {
    uint8_t  mac[6];
    uint8_t  ip[4];
    uint32_t until_s;               /* the lease ends at this time; 0: the slot is free */
    bool     offered_only;          /* offered, not yet requested */
} dhcpd_lease_t;

typedef struct {
    dhcpd_config_t cfg;
    dhcpd_lease_t  lease[DHCPD_LEASES];
} dhcpd_t;

void dhcpd_init(dhcpd_t *d, const dhcpd_config_t *cfg);

/* Handles one request. Returns the reply's length in `reply` (at most DHCPD_MAX_REPLY), or 0 for
 * a message that gets none (not a request, not ours, a lease that is not there to release). `now_s`
 * is a monotonic clock in seconds. Replies go to the broadcast address: this stack cannot address a
 * client that has no address yet, and a client accepts a broadcast reply. */
uint32_t dhcpd_handle(dhcpd_t *d, const uint8_t *req, uint32_t len, uint32_t now_s,
                      uint8_t reply[DHCPD_MAX_REPLY]);

/* DNS: answers the first question of a query. A: this node's address (TTL 60). AAAA and every other
 * type: no error and no answer (so a client does not wait for IPv6 or retry). Anything that is not a
 * standard query gets no answer. Returns the reply's length, or 0. */
uint32_t dnsd_handle(const uint8_t ip[4], const uint8_t *req, uint32_t len, uint8_t *reply, uint32_t cap);

/* Binds both to UDP 67 and 53 on the current interface, serving `cfg`. */
int  setup_net_services_start(const dhcpd_config_t *cfg);
void setup_net_services_stop(void);
uint32_t dhcpd_active_leases(uint32_t now_s);

#endif
