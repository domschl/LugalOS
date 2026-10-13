#ifndef KERNEL_LLP_NODE_H
#define KERNEL_LLP_NODE_H

#include <stdbool.h>
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
 * The channel today is QEMU's virtio-console (two guests, one socket); the
 * C6's SDIO and a UART follow in 49.3/49.6 behind the same two calls. */

typedef enum { LLP_NODE_OFF = 0, LLP_NODE_HOST = 1, LLP_NODE_RADIO = 2 } llp_node_role_t;

/* Starts the node in `role` on the board's LLP channel. 0, or -1 with the
 * reason printed (already running, no channel, no interface to bridge). */
int llp_node_start(llp_node_role_t role);

/* The 9P link over LLP, or NULL when the node is not running. */
p9_link_t *llp_node_p9_link(void);

/* `llp` in the shell: role, link state, peer, counters. */
void llp_node_print_status(void);

#endif
