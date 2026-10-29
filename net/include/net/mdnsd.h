#ifndef NET_MDNSD_H
#define NET_MDNSD_H

/* A multicast DNS responder (RFC 6762) for exactly one name: "<node name>.local", an A record with the node's
 * address. It is how a person who has just handed the node their Wi-Fi finds it again without knowing the
 * address the router chose: phones and laptops resolve .local names themselves.
 *
 * mdnsd_handle() is a pure function (a query in, a response out, no sockets), tested on the host;
 * mdnsd_start() binds it to UDP 5353. */

#include <stdint.h>
#include <stdbool.h>

#define MDNSD_MAX_REPLY 96u

/* A valid host label: 1..63 of [a-z0-9-], not starting or ending with '-'. Upper case is folded for matching. */
bool mdnsd_label_ok(const char *label);

/* Answers `query` (a whole DNS message) if it asks for an A record of "<host>.local". Returns the length of
 * the response written to `out` (cap >= MDNSD_MAX_REPLY), or 0 for "not for us". *unicast is set when the
 * querier asked for a unicast reply (the QU bit) or did not use port 5353 (RFC 6762 section 6.7), so the caller
 * answers it directly instead of to the group. */
uint32_t mdnsd_handle(const uint8_t *query, uint32_t len, const char *host, const uint8_t ip[4],
                      uint8_t *out, uint32_t cap, bool *unicast, uint16_t src_port);

int mdnsd_start(void);

#endif
