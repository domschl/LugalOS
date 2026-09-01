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

/* --- I3, plan/phase21_identity_and_authentication.md: the on-device
 * toolset's data layer. kernel/shell.c's `identity` command and
 * user/lisp/lisp.c's Lisp equivalents are thin wrappers over these -- the
 * argument parsing lives in each presentation layer, not here. */

/* Matches fs/9p.h's P9_AUTH_KEY_MAX: the two need not be the same constant,
 * but a device key is exactly the same kind of pre-shared secret phase 18's
 * key store already bounds, so there is no reason for a second number. */
#define NODE_DEVKEY_MAX 64

typedef enum {
    NODE_ID_OK = 0,
    NODE_ID_ERR_NO_BACKEND,    /* identity_store_device() returned NULL on this target */
    NODE_ID_ERR_POPULATED,     /* provision refused: already provisioned, no --force */
    NODE_ID_ERR_BAD_INPUT,     /* empty/oversized name, or a key that fails §5.1's checks */
    NODE_ID_ERR_NO_ENTROPY,    /* --generate refused: random_is_hardware() is false (§5.1) */
    NODE_ID_ERR_WRITE_FAILED,  /* the device write itself failed */
} node_id_result_t;

/* One error-to-string mapping shared by kernel/shell.c's `identity` command
 * and user/lisp/lisp.c's Lisp equivalents, so the two surfaces never drift
 * into describing the same failure two different ways. */
const char *node_id_result_str(node_id_result_t rc);

/* The record's device key, if one is provisioned -- absent on any node I4
 * has not yet reached, and always for report/fingerprint purposes only;
 * §6's toolset never hands this to a caller that prints it. `cap` bounds
 * how much of a longer-than-expected field gets copied; the real length is
 * returned via `len_out` regardless. Returns false (and leaves `out`/
 * `len_out` untouched) when there is no backend or no key field. */
bool node_devkey(uint8_t *out, uint32_t cap, uint32_t *len_out);

/* `identity provision [--force]` (§6). Mints a UID (random_bytes(), a
 * public identifier -- no entropy gate) and sets the name field to whatever
 * node_name() currently resolves to. Refuses when the store already parses
 * as IDSTORE_VALID unless `force` is true; an UNPROVISIONED or CORRUPT store
 * proceeds either way, since there is nothing valid to protect. `force`
 * overwrites even an existing UID -- §2 calls the UID "write-once ... it IS
 * the device," but also says a key that can never be rotated turns a mistake
 * into a hardware replacement; `--force` is the same deliberate-gesture
 * argument applied to the whole record, not an oversight of the write-once
 * table. A device key already on record, if any, is always carried forward
 * unchanged -- provisioning identity is not `identity key`'s job. */
node_id_result_t node_identity_provision(bool force);

/* `identity name <name>` (§6): persists a rename into the record (unlike
 * node_set_name(), which is runtime-only) and updates the live name/source
 * immediately, so a reboot is not required to see it take -- but does NOT
 * touch node_mac(): a persisted rename must keep the same non-obviousness
 * guarantee node_set_name() already gives (identity.h's own comment on
 * why). Same character/length validation as node_set_name(). */
node_id_result_t node_identity_rename_persistent(const char *name);

/* `identity key <hex>|--generate` (§6). §5.1's "validated at rest": refuses
 * an empty key, one longer than NODE_DEVKEY_MAX, or one that is all one
 * byte value or a straight +1/-1 run (the two shapes a key typed by hand
 * while distracted actually produces) -- not full pattern detection, just
 * the cheap, concrete cases the plan names. Any existing UID/name in the
 * record is carried forward unchanged. */
node_id_result_t node_identity_set_key(const uint8_t *key, uint32_t key_len);

/* `identity key --generate`'s own half: 32 random bytes, refused outright
 * when random_is_hardware() is false (§5.1 -- "a provisioner must refuse to
 * mint a key when random_is_hardware() is false"), which is every QEMU
 * target today. Installs the key the same way node_identity_set_key() does
 * on success. */
node_id_result_t node_identity_generate_key(void);

/* --- I6, §5.3: WLAN credentials. Lands with phase 19's R5 (the CYW43
 * driver) and is unused before it -- this is storage and the toolset, not
 * a radio, same shape as the device key was between I3 and I4. */

#define NODE_WLAN_SSID_MAX 32  /* 802.11's own limit */
#define NODE_WLAN_PSK_LEN  32  /* WPA2's PSK is always 256 bits -- never a variable length */

/* The provisioned SSID, null-terminated into `out` (at most `cap` bytes
 * including the terminator). Returns false (and leaves `out` untouched)
 * when there is no backend or no SSID field. */
bool node_wlan_ssid(char *out, uint32_t cap);

/* The provisioned WPA2 PSK -- the *derived* 256-bit key
 * (tools/provision.py's derive_wpa2_psk()), never the passphrase, which
 * this node never sees. Returns false (and leaves `out` untouched) when
 * there is no backend or no PSK field. */
bool node_wlan_psk(uint8_t out[NODE_WLAN_PSK_LEN]);

/* `wlan <ssid> <psk-hex>` (§6). Refuses an empty/oversized SSID or a PSK
 * that is not exactly NODE_WLAN_PSK_LEN bytes -- WPA2's PSK has no other
 * valid length, so this is a shape check, not a policy one; there is no
 * "trivially patterned" rule here because §5.1's version of that concern
 * is why the PSK is derived on the host in the first place (a passphrase
 * weak enough to guess produces a PSK indistinguishable from a strong
 * one's). Any existing UID/name/device key on record is carried forward
 * unchanged. */
node_id_result_t node_identity_set_wlan(const char *ssid, uint32_t ssid_len,
                                        const uint8_t *psk, uint32_t psk_len);

/* --- Network autoconfig: the address, stored beside the credentials that
 * reach it. Read at net_task_start(), so a board that has one comes up on
 * the network with no boot script involved. --- */

#define NODE_IPV4_LEN 4

/* The stored IPv4 configuration, or false if there is none (no store, no
 * valid record, or no such field). Any of the three outputs may be NULL.
 * A gateway of 0.0.0.0 is a real answer -- "no route off this segment" --
 * not an absent one, which is why the three arrive together or not at all. */
bool node_ipv4(uint8_t ip[NODE_IPV4_LEN], uint8_t mask[NODE_IPV4_LEN],
               uint8_t gw[NODE_IPV4_LEN]);

/* `netcfg <ip> <mask> [gw]`. Refuses an all-zero address or mask -- both are
 * configurations that cannot work, and storing one would produce a board
 * that comes up looking configured while answering nothing. Pass gw as
 * 0.0.0.0 for a segment with no router. Any existing UID/name/key/WLAN
 * fields are carried forward unchanged. */
node_id_result_t node_identity_set_ipv4(const uint8_t ip[NODE_IPV4_LEN],
                                        const uint8_t mask[NODE_IPV4_LEN],
                                        const uint8_t gw[NODE_IPV4_LEN]);

/* `netcfg clear`. Removes the field, so the board goes back to coming up
 * unconfigured. Exists because a *stale* address is the failure this feature
 * can cause: a board that silently comes up on the wrong subnet looks alive
 * and answers nothing. Overwriting is not always what is wanted either --
 * moving a board to DHCP later means removing this, not replacing it. */
node_id_result_t node_identity_clear_ipv4(void);

#endif /* LUGALOS_KERNEL_IDENTITY_H */
