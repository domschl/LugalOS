#ifndef NET_INTERNAL_H
#define NET_INTERNAL_H

#include "net/ip.h"

/* Shared between the stack's own translation units and nothing else. The
 * counters are writable here and read-only through net_state(): a caller
 * outside net/ that wants to change what the stack has seen is asking the
 * wrong question. */

net_state_t *net_mutable_state(void);
const uint8_t *net_broadcast_mac(void);

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806
#define ETH_HDR_LEN    14

#endif // NET_INTERNAL_H
