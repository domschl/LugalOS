#ifndef NET_DNS_H
#define NET_DNS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "net/ip.h"

/* Resolves `name` to an IPv4 address.
 * 1. If `name` is already an IPv4 dotted quad string (e.g. "192.168.1.1"), parses it directly.
 * 2. Checks static hosts tables: /sd0/etc/hosts and /flash0/etc/hosts.
 * 3. Sends a standard RFC 1035 UDP A-record query on port 53 to the configured DNS server
 *    (or gateway IP fallback).
 * Returns true if resolved, false otherwise. */
bool dns_resolve(const char *name, uint8_t ip_out[IPV4_LEN]);

/* Sets explicit DNS server address, overriding DHCP / gateway default. */
void dns_set_server(const uint8_t ip[IPV4_LEN]);

/* Gets currently active DNS server address. Returns false if none configured. */
bool dns_get_server(uint8_t ip_out[IPV4_LEN]);

#endif /* NET_DNS_H */
