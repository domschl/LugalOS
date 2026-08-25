#include "net/ip.h"
#include "net/net_internal.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/palloc.h"
#include <string.h>

/* The stack's state, its single transmit buffer, and the Ethernet-level
 * dispatch that feeds ARP and IPv4 (R2,
 * plan/phase19_ip_stack_and_ethernet.md). */

static net_state_t g_net;

/* One transmit frame and one receive frame, reused. Everything above runs
 * from net_poll() on one call stack, so a second frame can never be in flight
 * -- and a stack that cannot buffer a second frame also cannot leak one,
 * queue one behind a lost ARP, or need a packet allocator. Losing that
 * property is a design change, not an optimisation.
 *
 * **On the heap, not in .bss**, and taken only at attach time. Together they
 * are 3028 bytes, which fits in one page with room to spare -- but as statics
 * they were 3424 bytes of .bss on *every* persona, and on the RP2350 .bss and
 * the heap are the same budget (plan/phase15_memory_reclamation.md). That
 * cost the chess persona a whole heap page for a stack it has no interface to
 * run. Phase 15's rule applies exactly: memory taken only while it is used. A
 * board with no netif never attaches, so it never pays. */
static uint8_t *g_frames;
static uint8_t *g_tx;
static uint8_t *g_rx;

static const uint8_t BROADCAST_MAC[NETIF_MAC_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static uint8_t g_unclaimed[NET_UNCLAIMED_HEAD];
static uint32_t g_unclaimed_len;      /* the full frame length, 0 = empty */
static uint32_t g_unclaimed_total;    /* how many have ever arrived */

uint32_t net_unclaimed_count(void) { return g_unclaimed_total; }

uint32_t net_take_unclaimed(uint8_t *out) {
    if (!out || g_unclaimed_len == 0) return 0;
    memcpy(out, g_unclaimed, NET_UNCLAIMED_HEAD);
    uint32_t len = g_unclaimed_len;
    g_unclaimed_len = 0;
    return len;
}

int net_stack_attach(netif_t *nif) {
    if (!nif) return -1;
    if (!g_frames) {
        g_frames = (uint8_t *)palloc_pages(1);
        if (!g_frames) {
            printk("[Net] No page for the stack's frame buffers; interface stays down.\n");
            return -1;
        }
        g_tx = g_frames;
        g_rx = g_frames + NETIF_FRAME_MAX;
    }
    memset(&g_net, 0, sizeof(g_net));
    g_net.nif = nif;
    return 0;
}

bool net_configured(void) { return g_net.configured && g_net.nif != NULL; }
const net_state_t *net_state(void) { return &g_net; }

int net_set_address(const uint8_t ip[IPV4_LEN], const uint8_t mask[IPV4_LEN],
                    const uint8_t gw[IPV4_LEN]) {
    if (!g_net.nif) {
        /* Late binding: (net-config) may run from a boot script before
         * anything has attached an interface, and on a board that has none it
         * never will. Take the default now if there is one. */
        netif_t *nif = netif_default();
        if (!nif) return -1;
        net_stack_attach(nif);
    }
    if (!ip || !mask) return -1;
    memcpy(g_net.ip, ip, IPV4_LEN);
    memcpy(g_net.mask, mask, IPV4_LEN);
    if (gw) memcpy(g_net.gw, gw, IPV4_LEN);
    else memset(g_net.gw, 0, IPV4_LEN);
    g_net.configured = true;

    /* Tell the segment who we are without being asked. It costs one frame and
     * it is what stops the first exchange after a reconfiguration from
     * waiting on somebody else's stale cache. */
    arp_announce();
    return 0;
}

uint8_t *net_tx_payload(void) { return g_tx + ETH_HDR_LEN; }
uint32_t net_tx_payload_max(void) { return NETIF_FRAME_MAX - ETH_HDR_LEN; }

int net_tx_send(const uint8_t dst_mac[NETIF_MAC_LEN], uint16_t ethertype, uint32_t payload_len) {
    if (!g_net.nif || !dst_mac) return -1;
    if (payload_len > net_tx_payload_max()) return -1;

    memcpy(g_tx, dst_mac, NETIF_MAC_LEN);
    memcpy(g_tx + NETIF_MAC_LEN, g_net.nif->mac, NETIF_MAC_LEN);
    g_tx[12] = (uint8_t)(ethertype >> 8);
    g_tx[13] = (uint8_t)(ethertype & 0xff);

    uint32_t len = ETH_HDR_LEN + payload_len;
    /* Ethernet's 60-byte minimum (the FCS makes 64 on the wire, and is not
     * ours to count -- see net/netif.h). virtio-net does not care; the
     * ENC28J60 in R4 will, and padding here means it never becomes a second
     * driver's problem. The pad is zeroed rather than left as the previous
     * frame's tail, which would leak whatever the stack last sent. */
    if (len < 60u) {
        memset(g_tx + len, 0, 60u - len);
        len = 60u;
    }
    return netif_send(g_net.nif, g_tx, len);
}

/* Internal: shared with arp.c for the broadcast destination. */
const uint8_t *net_broadcast_mac(void) { return BROADCAST_MAC; }

uint32_t net_poll(uint32_t budget) {
    if (!g_net.nif) return 0;
    uint32_t handled = 0;

    while (handled < budget) {
        if (netif_poll(g_net.nif) != 1) break;
        int n = netif_recv(g_net.nif, g_rx, NETIF_FRAME_MAX);
        if (n < ETH_HDR_LEN) {
            if (n >= 0) g_net.drop_short++;
            /* n < 0 was already counted by the driver or the netif wrapper --
             * counting it a second time here would make the totals lie. */
            handled++;
            continue;
        }

        /* Address filter. The device may or may not do this for us (virtio
         * does not, the ENC28J60 can be told to), so the stack does it
         * unconditionally: a frame for somebody else is a counted event, not
         * something to parse. */
        bool for_us = (memcmp(g_rx, g_net.nif->mac, NETIF_MAC_LEN) == 0) ||
                      (g_rx[0] & 0x01);   /* broadcast and multicast both have it */
        if (!for_us) {
            g_net.drop_not_for_us++;
            handled++;
            continue;
        }

        uint16_t ethertype = (uint16_t)((uint16_t)g_rx[12] << 8 | g_rx[13]);
        if (ethertype == ETHERTYPE_ARP) {
            g_net.rx_arp++;
            arp_input(g_rx, (uint32_t)n);
        } else if (ethertype == ETHERTYPE_IPV4) {
            g_net.rx_ip++;
            ip_input(g_rx, (uint32_t)n);
        } else {
            /* 0x88b5 test frames land here, as does anything else on the
             * segment we do not speak. Not an error, and not counted as a
             * drop either: "a protocol we do not implement" is the normal
             * state of a LAN, not a fault in this stack. Latched rather than
             * discarded, so a bring-up can ask what went past. The newest
             * wins: on a chatty segment the last frame is the one an
             * operator just caused. */
            uint32_t head = (uint32_t)n < NET_UNCLAIMED_HEAD ? (uint32_t)n : NET_UNCLAIMED_HEAD;
            memset(g_unclaimed, 0, NET_UNCLAIMED_HEAD);
            memcpy(g_unclaimed, g_rx, head);
            g_unclaimed_len = (uint32_t)n;
            g_unclaimed_total++;
        }
        handled++;
    }
    return handled;
}

/* --- The pump ---
 *
 * A scheduled kernel task, exactly like fs/p9_link.c's `p9srv`: poll a
 * budget of frames, yield, repeat. Four per turn rather than one, because a
 * ping flood and a 9P transfer both arrive in bursts and a strict
 * one-per-scheduling-quantum is a throughput ceiling for no benefit.
 *
 * **Not yet a U-mode task in its own memory domain, and the reason is
 * sequencing rather than reluctance.** The plan's §3 asks for one, on the
 * grounds that a stack parsing attacker-controlled bytes is the last place to
 * make an exception to this kernel's isolation rules -- which is right. But
 * the isolation machinery (PMP domains for driver tasks, phase 12's M5) lives
 * on the RP2350 personas, and *those have no network at all until R4*. The
 * only target with a netif today is QEMU, where no driver is domained:
 * drivers/virtio_blk.c and drivers/virtio_console.c are both plain kernel
 * code. Domaining the stack here would mean inventing the QEMU-side driver
 * isolation pattern for a device that exists only in an emulator, and then
 * redoing it against real silicon anyway.
 *
 * R4 is where both halves exist at once: an ENC28J60 driver written as a
 * U-mode driver task in the M5 style, with the stack above it in its own
 * domain. Recorded here rather than in a plan file alone, because the next
 * person to read this function will ask. */
static void net_task_body(void *arg) {
    (void)arg;
    for (;;) {
        net_poll(4);
        sched_yield();
    }
}

int net_task_start(void) {
    if (!g_net.nif) return -1;
    int pid = task_create_sized("netsrv", net_task_body, NULL, 2);
    if (pid < 0) printk("[Net] Could not start the stack task.\n");
    return pid;
}

/* One report, two callers -- `net` from the shell and `(net-status)` from
 * Lisp. It lives here rather than in either of them because its subject is
 * this file's state; `cat /proc/net` prints the same facts in a fuller,
 * machine-readable form. */
void net_print_status(void) {
    if (!g_net.nif) {
        cprintf("net: no interfaces.\n");
        cprintf("  This board has no network hardware fitted, or its driver found none.\n");
        cprintf("  On QEMU, add: -netdev ... -device virtio-net-device,netdev=...\n");
        return;
    }
    char mac[18];
    netif_mac_str(g_net.nif->mac, mac);
    cprintf("%s: mac %s, link %s\n", g_net.nif->name, mac,
            netif_link_up(g_net.nif) ? "up" : "down");
    if (g_net.configured) {
        cprintf("  addr %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u\n",
                g_net.ip[0], g_net.ip[1], g_net.ip[2], g_net.ip[3],
                g_net.mask[0], g_net.mask[1], g_net.mask[2], g_net.mask[3],
                g_net.gw[0], g_net.gw[1], g_net.gw[2], g_net.gw[3]);
    } else {
        cprintf("  addr unconfigured -- (net-config \"ip\" \"mask\" [\"gw\"])\n");
    }
    cprintf("  rx %lu frames, %lu bytes, %lu dropped\n",
            (unsigned long)g_net.nif->rx_frames, (unsigned long)g_net.nif->rx_bytes,
            (unsigned long)g_net.nif->rx_dropped);
    cprintf("  tx %lu frames, %lu bytes, %lu errors\n",
            (unsigned long)g_net.nif->tx_frames, (unsigned long)g_net.nif->tx_bytes,
            (unsigned long)g_net.nif->tx_errors);
    cprintf("  arp %lu/%lu, ip %lu/%lu, icmp %lu/%lu, udp %lu/%lu (rx/tx)\n",
            (unsigned long)g_net.rx_arp, (unsigned long)g_net.tx_arp,
            (unsigned long)g_net.rx_ip, (unsigned long)g_net.tx_ip,
            (unsigned long)g_net.rx_icmp, (unsigned long)g_net.tx_icmp,
            (unsigned long)g_net.rx_udp, (unsigned long)g_net.tx_udp);
    cprintf("  arp cache %lu entries; see /proc/net for every drop counter\n",
            (unsigned long)arp_entries());
}

/* --- accessors the protocol files share, kept here with the state --- */
net_state_t *net_mutable_state(void) { return &g_net; }
