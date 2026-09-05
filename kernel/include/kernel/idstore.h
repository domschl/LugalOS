#ifndef LUGALOS_KERNEL_IDSTORE_H
#define LUGALOS_KERNEL_IDSTORE_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/block.h"

/* The identity record, and the block_dev_t seam it lives on -- I1,
 * plan/phase21_identity_and_authentication.md §4.
 *
 * A fixed-size (§3.2's "last 4 KB sector") record: magic, version, length,
 * CRC32, then typed (TLV) fields. Typed fields rather than a fixed struct so
 * that a later milestone (I4's device key, I5's peer list, I6's WLAN
 * credential) can add a field type without invalidating a record an earlier
 * build wrote -- a reader that does not recognise a type skips it by length
 * and counts it, rather than refusing the whole record.
 *
 * Target-independent on purpose, the same argument kernel/sha256.c and
 * kernel/random.c already make: this is a parser and a CRC, and neither
 * should be debugged by flashing a board. I2 supplies the QEMU virtio-blk
 * device this reads from; I7 supplies the RP2350 flash sector. Until then,
 * anything implementing block_dev_t -- including a QEMU target's own
 * ramdisk, or the fake device idstore_selftest() builds in memory -- is a
 * valid backing store to develop and test this against.
 *
 * Three states, because a bare magic cannot tell the last two apart:
 *   - UNPROVISIONED: the magic is not there at all. Covers erased flash
 *     (all 0xFF) and an untouched QEMU disk image (all 0x00) alike -- both
 *     mean "no record has ever been written here".
 *   - CORRUPT: the magic is there but the version, length or CRC is not
 *     what it should be. Refused outright, never half-interpreted.
 *   - VALID: parses cleanly.
 */

#define IDSTORE_BLOCK_SIZE 512u
#define IDSTORE_BLOCKS     8u
#define IDSTORE_SIZE_BYTES (IDSTORE_BLOCK_SIZE * IDSTORE_BLOCKS)  /* 4096 */

/* magic(4) + version(1) + reserved(1) + length(2) + crc32(4) */
#define IDSTORE_HEADER_LEN 12u

typedef enum {
    IDSTORE_UNPROVISIONED = 0,
    IDSTORE_CORRUPT,
    IDSTORE_VALID,
} idstore_state_t;

/* Field type tags for the TLV stream. A reader that sees a tag not listed
 * here does not fail -- it skips `len` bytes and counts the field as
 * unknown (idstore_t.unknown_fields_skipped). Add new tags as later
 * milestones need them; never reuse a retired one. */
typedef enum {
    IDSTORE_FIELD_UID    = 1,  /* device scope, §2: 8 bytes, write-once in practice */
    IDSTORE_FIELD_NAME   = 2,  /* instance scope: up to NODE_NAME_MAX-1 bytes, freely rewritable */
    IDSTORE_FIELD_DEVKEY = 3,  /* device scope, secret (§2, I3): the node's own auth key.
                                 * Stored and fingerprinted from I3 on; not yet consulted by
                                 * the 9P auth path -- I4 is what makes p9_auth_key_for()
                                 * read this instead of (or before) the SD-card key file. */
    IDSTORE_FIELD_WLAN_SSID = 4,  /* instance scope (§5.3, I6): ASCII, 1-32 bytes (802.11's own limit) */
    IDSTORE_FIELD_WLAN_PSK  = 5,  /* instance scope, secret (§5.3, I6): the *derived* 256-bit
                                    * WPA2 PSK, never the passphrase it came from -- see
                                    * tools/provision.py's derive_wpa2_psk(). Lands with phase
                                    * 19's R5 (the CYW43 driver) and is unused before it, same
                                    * as IDSTORE_FIELD_DEVKEY was between I3 and I4. */
    IDSTORE_FIELD_IPV4      = 6,  /* instance scope: exactly 12 bytes -- ip[4], mask[4], gw[4],
                                    * in dotted-quad order, the same layout net_set_address()
                                    * takes. **One field rather than three**, because the three
                                    * numbers are not independently meaningful: an address
                                    * without its mask is not a configuration, and three fields
                                    * would make a half-written one representable. A gateway of
                                    * 0.0.0.0 is valid and means "no route off this segment".
                                    *
                                    * This is what keeps per-board addressing out of init.lisp.
                                    * Since I7a the filesystem image is byte-identical on every
                                    * board; putting an address in a boot script would make it
                                    * per-board again and cost that property. So the address
                                    * lives here, beside the SSID and PSK it is reached with,
                                    * and the boot script stays the same everywhere. */
    IDSTORE_FIELD_GRANTS    = 7,  /* instance scope, secret-adjacent: the peer grants list --
                                    * who may attach here, at what aname, read-only or not --
                                    * as the exact text `p9_grants_list()` already parses, one
                                    * "name key aname mode" line per grant.
                                    *
                                    * **The same bytes as the file, in a different home**, and
                                    * that is deliberate: the parser and serialiser in fs/9p.c
                                    * are unchanged, so this is a storage decision rather than
                                    * a format one, and a board with an SD card can still keep
                                    * its list there.
                                    *
                                    * Why it had to move: grants lived only at
                                    * /sd0/system/etc/auth/keys, and a board with no card --
                                    * the clock persona is one -- had nowhere persistent to put
                                    * one at all. `p9key` is this-boot-only and /flash0 is
                                    * read-only and byte-identical across boards, so *networked
                                    * 9P could never work on such a board*: it would refuse
                                    * every attach, forever, with no way to authorise anyone.
                                    * Phase 21 §5.2 already argued grants belong to identity;
                                    * this is where that becomes true rather than aspirational.
                                    *
                                    * Secret-adjacent rather than secret: the keys in it are
                                    * bearer tokens for *other* nodes, so it is covered by the
                                    * same never-served guard as the rest of the record. */
    IDSTORE_FIELD_MQTT      = 8,  /* instance scope, secret-bearing (Q6,
                                    * plan/phase26_mqtt_and_environment_sensors.md): where this
                                    * node publishes, and as whom.
                                    *
                                    *   ip[4]  port_be16  sample_be16  keepalive_be16
                                    *   u8 len + username
                                    *   u8 len + password
                                    *   u8 len + topic prefix
                                    *
                                    * **One field, for IDSTORE_FIELD_IPV4's reason**: a broker
                                    * address without its port and credentials is not a
                                    * configuration, and separate fields would make a
                                    * half-written one representable.
                                    *
                                    * It holds a password **in the clear**, like the WLAN PSK
                                    * beside it. That is a property of an unencrypted store on
                                    * a device someone can pick up, phase 21's "what it does
                                    * not defend" already covers it, and net/mqtt.h says the
                                    * same password crosses the LAN in the clear anyway -- so
                                    * the record is not the weak link. `mqttcfg` and
                                    * /proc/node print it as `set`, never as its value. */
} idstore_field_type_t;

typedef struct {
    uint8_t  buf[IDSTORE_SIZE_BYTES];   /* the whole record, as read from the device */
    uint32_t fields_len;                /* bytes of TLV data following the header */
    uint32_t unknown_fields_skipped;    /* fields whose type this build does not recognise */
} idstore_t;

/* Reads and validates blocks 0..IDSTORE_BLOCKS-1 of `dev`. `out` is filled
 * (and walkable with idstore_get_field()) only when the return value is
 * IDSTORE_VALID; a read failure at the device level is reported as
 * IDSTORE_CORRUPT, since a store that cannot be read cannot be trusted
 * either. */
idstore_state_t idstore_read(block_dev_t *dev, idstore_t *out);

/* Looks up `type` in a record idstore_read() returned IDSTORE_VALID for.
 * Copies min(field length, cap) bytes into `val` and returns the field's
 * actual length. Returns -1 if the type is absent. `val`/`cap` may be
 * 0/NULL to just test presence and get the length. */
int idstore_get_field(const idstore_t *rec, uint8_t type, void *val, uint32_t cap);

/* The same lookup without the copy: a pointer into `rec`'s own buffer, valid
 * for as long as `rec` is. NULL when the field is absent.
 *
 * For fields large enough that a caller carrying them forward would otherwise
 * need a second buffer of their size. The grants list is the case that
 * prompted it -- up to ~1.5 KB of text that identity_store_write() copies
 * from the old record to the new one on *every* unrelated write, and a
 * 1.5 KB stack frame or a permanent .bss buffer are both worse than not
 * copying at all on a board where .bss and the heap are the same budget. */
const uint8_t *idstore_field_ptr(const idstore_t *rec, uint8_t type, uint32_t *len_out);

/* Builds a new record in memory. idstore_writer_init(), zero or more
 * idstore_writer_add_field(), then idstore_writer_commit() to finalise the
 * header (including the CRC) and write it out. */
typedef struct {
    uint8_t  buf[IDSTORE_SIZE_BYTES];
    uint32_t fields_len;
} idstore_writer_t;

void idstore_writer_init(idstore_writer_t *w);

/* Appends one field. Returns 0, or -1 if it does not fit in what remains of
 * the record. */
int idstore_writer_add_field(idstore_writer_t *w, uint8_t type, const void *val, uint16_t len);

/* Finalises magic/version/length/CRC32 and writes all IDSTORE_BLOCKS blocks
 * to `dev`. Returns 0, or -1 if the device write failed. */
int idstore_writer_commit(idstore_writer_t *w, block_dev_t *dev);

/* CRC-32 (the IEEE 802.3 polynomial, init/final 0xFFFFFFFF -- the same
 * construction as zip and Ethernet, so any host tool can check a record
 * without a bespoke implementation). Exposed for tools/provision.py (I2) and
 * for I7's "corrupt a byte and confirm the CRC catches it" verification. */
uint32_t crc32_compute(const void *data, uint32_t len);

/* I3, §4: "the identity store must join [p9_auth_path_is_secret()'s guard],
 * and the test for that belongs with the store rather than with the server."
 * The store is never mounted into the VFS/9P namespace today -- I2/I3's
 * kernel/identity.c reads and writes it directly through block_dev_t,
 * bypassing the filesystem layer entirely -- so nothing currently matches
 * this. It exists anyway, on the same reasoning fs/9p.c already enforces its
 * own guard on the server rather than trusting every caller to remember it:
 * a guard installed only once something reaches for the wrong path is a
 * guard installed too late. fs/9p.c's p9_path_is_secret() calls this in
 * addition to its own check, so both live behind one guard. */
bool idstore_path_is_secret(const char *path);

/* Self-test: the three states, unknown-field skipping, and a byte-identical
 * round trip -- all against an in-memory fake block_dev_t, so it needs
 * neither hardware nor a mounted filesystem (I1's verify list,
 * plan/phase21_identity_and_authentication.md). */
int idstore_selftest(void);

#endif /* LUGALOS_KERNEL_IDSTORE_H */
