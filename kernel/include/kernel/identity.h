#ifndef LUGALOS_KERNEL_IDENTITY_H
#define LUGALOS_KERNEL_IDENTITY_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/block.h"

/* Who this node is (plan/phase19_ip_stack_and_ethernet.md).
 *
 * A name and a MAC address, resolved once at boot. Both matter the moment a
 * second board joins a segment, and phase 18 got this wrong in a way worth
 * recording: its plan described deriving a MAC from the RP2350's unique id,
 * and what actually shipped was the constant 02:4C:47:00:00:01 in the W5500
 * driver -- so any two boards would have collided, and the design vanished
 * with the driver when the part was cancelled. Identity is not a driver's
 * business. It lives here, above every wire.
 *
 * **Resolution order, most specific first.**
 *
 * The name:
 *   1. node_set_name() at runtime -- `(net-identity "clock-01")` in
 *      /sd0/system/etc/usr_init.lisp, beside (net-config)
 *   2. CONFIG_NODE_NAME, pinned by a board file
 *   3. derived: "<persona>-<4 hex>", e.g. "rp2350-gateway-3f2a"
 *
 * The MAC:
 *   1. CONFIG_NODE_MAC, pinned by a board file (a MAC printed on a label)
 *   2. a MAC the *device* supplies -- virtio's config space, an EEPROM on a
 *      NIC. Left to the driver: netif_register() only fills a MAC the driver
 *      left as zeroes.
 *   3. derived: 02:4C:47 + 3 bytes of SHA-256 over the node's own identity
 *
 * **On 02:4C:47.** Not an invented manufacturer code -- inventing one means
 * squatting on bytes the IEEE has assigned or will. Bit 1 of the first octet
 * is the *locally administered* bit, reserved by IEEE 802 for exactly this,
 * and 0x02 is the canonical unicast spelling of it. 4C 47 is ASCII "LG", so
 * the address still reads as ours in a packet dump without pretending to be
 * registered to anyone.
 */

#define NODE_NAME_MAX 32
#define NODE_MAC_LEN  6
#define NODE_UID_LEN  8

/* Resolves both. Call once, early in kernel_main() -- before any netif
 * registers, since that is where the MAC is handed out. */
void node_identity_init(void);

const char *node_name(void);
const uint8_t *node_mac(void);

/* Where each came from, for /proc and for anyone wondering why two boards
 * answer to the same thing. */
const char *node_name_source(void);
const char *node_mac_source(void);

/* Runtime override for the name. **The MAC deliberately does not follow it**:
 * it is derived from the silicon or the build, not from the name, so renaming
 * a node does not move it to a different address -- which would invalidate
 * every ARP cache on the segment for a change meant to be cosmetic.
 * Rejects an empty name, anything longer than NODE_NAME_MAX-1, and anything
 * outside [A-Za-z0-9.-]: the same string must be safe as a 9P uname, as a
 * hostname and in a log line. Returns 0 on success. */
int node_set_name(const char *name);

/* Board hook: true, and fills `out`, when this silicon can identify itself.
 * The default implementation answers false; RP2350's flash id lands with R4,
 * where it can be checked against two real boards rather than asserted. */
bool board_unique_id(uint8_t out[8]);

/* The device-scope UID (I2/§2, plan/phase21_identity_and_authentication.md):
 * silicon (board_unique_id()) when this board has one, else the identity
 * store's provisioned UID field, else absent. Returns false and leaves `out`
 * untouched when neither source has one -- an unprovisioned QEMU node has no
 * stable device identity, and this says so rather than inventing one. */
bool node_uid(uint8_t out[NODE_UID_LEN]);
const char *node_uid_source(void);

/* Target hook: the block device the identity store (kernel/idstore.h) lives
 * on, or NULL where none exists yet. Weak default answers NULL; QEMU's
 * virtio-blk backend and the RP2350's flash-sector backend (I7) each define
 * this symbol for their own target. */
block_dev_t *identity_store_device(void);

#endif /* LUGALOS_KERNEL_IDENTITY_H */
