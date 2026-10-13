#ifndef KERNEL_LLP_NODE_H
#define KERNEL_LLP_NODE_H

#include <stdbool.h>
#include <stdint.h>
#include "fs/p9_link.h"

/* An LLP node (protocol in doc/dev/protocols/llp.md, kernel/llp.c): one end of the link between a host (the node
 * that owns the IP address -- a P4, an RP2350) and a radio (a C6).
 *
 * * As the **radio** ("modem"), the node gives its network interface to the
 *   link: its own stack lets go of it (net_stack_detach()) and every frame
 *   goes over LLP, both ways. It announces the interface's MAC in HELLO and
 *   its link state in STATE.
 * * As the **host**, the node gains `wlan0`: a netif whose frames are LLP's,
 *   with the radio's MAC, under its own IP stack and DHCP client.
 * * Both serve 9P over the link (LLP's P9 frames), so each can mount the
 *   other: the `llp` 9P link, e.g. `(mount-remote "c6" "llp")`.
 *
 * One task owns the link (`llp`): it reads the channel, runs kernel/llp.c and
 * sends. Everyone else -- the stack's task, the DHCP client, 9P server and
 * clients -- only queues frames for it, P9 ahead of ETH, so the protocol state
 * needs no lock.
 *
 * The channel is the board's: QEMU's virtio-console (two guests, one
 * socket), the C6-Zero's USB port, or SDIO between a P4 and its C6. */

typedef enum { LLP_NODE_OFF = 0, LLP_NODE_HOST = 1, LLP_NODE_RADIO = 2 } llp_node_role_t;

/* Starts the node in `role` on the board's LLP channel. 0, or -1 with the
 * reason printed (already running, no channel, no interface to bridge). */
int llp_node_start(llp_node_role_t role);

/* At boot: the role a board's wiring implies, if any -- the radio on a C6
 * with an SDIO slave, the host on a P4 whose only network is its C6. */
void llp_node_autostart(void);

/* The radio's start path, with its new interface: true when an LLP host is
 * up and the bridge takes the interface (the node's own stack must not touch
 * it); false when the stack has been attached to it instead -- the bridge
 * takes it over from the stack if a host comes later. */
struct netif;
bool llp_node_offer_netif(struct netif *nif);

/* The 9P link over LLP, or NULL when the node is not running. */
p9_link_t *llp_node_p9_link(void);

/* `llp` in the shell: role, link state, peer, counters. */
void llp_node_print_status(void);

/* Radio role: Ethernet frames from the host go back to it instead of to the
 * radio (`llp loop on|off`), for llp_node_bench() on the host. */
void llp_node_set_loop(bool on);

/* Host role: `count` frames of `size` bytes to a radio in loop mode, each
 * checked when it comes back; prints the throughput (`llp bench`). */
void llp_node_bench(uint32_t count, uint32_t size);

#endif
