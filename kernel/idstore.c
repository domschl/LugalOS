#include "kernel/idstore.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/palloc.h"
#include <string.h>

#define IDSTORE_MAGIC0 'L'
#define IDSTORE_MAGIC1 'G'
#define IDSTORE_MAGIC2 'I'
#define IDSTORE_MAGIC3 'D'
#define IDSTORE_VERSION 1u

static uint32_t rd_u16le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr_u16le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* --- CRC-32, bitwise (no table: this runs on at most 4 KB, occasionally,
 * and a 1 KB rodata table buys nothing worth the space on RP2350). */

static uint32_t crc32_update(uint32_t crc, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

uint32_t crc32_compute(const void *data, uint32_t len) {
    return ~crc32_update(0xFFFFFFFFu, data, len);
}

/* The record's CRC covers the header up to (but not including) the CRC
 * field itself, then the field data -- never the CRC field, which would be
 * self-referential, and never the unused tail of `buf` beyond fields_len,
 * which is not part of the record. */
static uint32_t record_crc(const uint8_t *buf, uint32_t fields_len) {
    uint32_t crc = crc32_update(0xFFFFFFFFu, buf, IDSTORE_HEADER_LEN - 4u);
    crc = crc32_update(crc, &buf[IDSTORE_HEADER_LEN], fields_len);
    return ~crc;
}

static bool field_type_known(uint8_t type) {
    return type == IDSTORE_FIELD_UID || type == IDSTORE_FIELD_NAME || type == IDSTORE_FIELD_DEVKEY ||
           type == IDSTORE_FIELD_WLAN_SSID || type == IDSTORE_FIELD_WLAN_PSK ||
           type == IDSTORE_FIELD_IPV4;
}

/* I3, kernel/include/kernel/idstore.h's own comment on this declaration
 * explains why this matches nothing today and is enforced anyway. */
bool idstore_path_is_secret(const char *path) {
    /* Named after /dev's "vblk" -- the existing entry this would collide
     * with in shape if the raw store were ever registered the same way. */
    static const char *const RESERVED[] = {
        "/dev/identity", "/dev/identity0",
    };
    if (!path) return false;
    for (unsigned i = 0; i < sizeof(RESERVED) / sizeof(RESERVED[0]); i++) {
        if (strcmp(path, RESERVED[i]) == 0) return true;
    }
    return false;
}

idstore_state_t idstore_read(block_dev_t *dev, idstore_t *out) {
    uint8_t buf[IDSTORE_SIZE_BYTES];

    if (!dev || !dev->read_blocks || !out) return IDSTORE_CORRUPT;
    if (dev->read_blocks(dev, buf, 0, IDSTORE_BLOCKS) != 0) return IDSTORE_CORRUPT;

    if (buf[0] != IDSTORE_MAGIC0 || buf[1] != IDSTORE_MAGIC1 ||
        buf[2] != IDSTORE_MAGIC2 || buf[3] != IDSTORE_MAGIC3) {
        return IDSTORE_UNPROVISIONED;
    }

    uint8_t  version = buf[4];
    uint16_t length  = (uint16_t)rd_u16le(&buf[6]);
    uint32_t crc_stored = rd_u32le(&buf[8]);

    if (version != IDSTORE_VERSION) return IDSTORE_CORRUPT;
    if (length < IDSTORE_HEADER_LEN || length > IDSTORE_SIZE_BYTES) return IDSTORE_CORRUPT;

    uint32_t fields_len = (uint32_t)length - IDSTORE_HEADER_LEN;
    if (record_crc(buf, fields_len) != crc_stored) return IDSTORE_CORRUPT;

    memcpy(out->buf, buf, IDSTORE_SIZE_BYTES);
    out->fields_len = fields_len;

    /* Walk the TLV stream once, at read time, so a caller doing repeated
     * idstore_get_field() lookups never has to. Any field type this build
     * does not know is legitimate -- it is skipped by length and counted,
     * never treated as corruption -- since that is the whole reason the
     * record is typed fields and not a fixed struct (§4). */
    uint32_t unknown = 0;
    uint32_t off = IDSTORE_HEADER_LEN;
    uint32_t end = IDSTORE_HEADER_LEN + fields_len;
    while (off + 3u <= end) {
        uint8_t  type = buf[off];
        uint16_t flen = (uint16_t)rd_u16le(&buf[off + 1]);
        uint32_t voff = off + 3u;
        if (voff + flen > end) break;  /* malformed TLV inside a CRC-valid record: stop, keep what parsed */
        if (!field_type_known(type)) unknown++;
        off = voff + flen;
    }
    out->unknown_fields_skipped = unknown;

    return IDSTORE_VALID;
}

int idstore_get_field(const idstore_t *rec, uint8_t type, void *val, uint32_t cap) {
    if (!rec) return -1;
    uint32_t off = IDSTORE_HEADER_LEN;
    uint32_t end = IDSTORE_HEADER_LEN + rec->fields_len;
    while (off + 3u <= end) {
        uint8_t  t    = rec->buf[off];
        uint16_t flen = (uint16_t)rd_u16le(&rec->buf[off + 1]);
        uint32_t voff = off + 3u;
        if (voff + flen > end) break;
        if (t == type) {
            uint32_t n = flen < cap ? flen : cap;
            if (val && n) memcpy(val, &rec->buf[voff], n);
            return (int)flen;
        }
        off = voff + flen;
    }
    return -1;
}

/* See kernel/idstore.h. Shares idstore_get_field()'s walk deliberately
 * rather than duplicating it: two copies of a TLV walk is two places to get
 * the bounds wrong, and this one exists only so a large field can be carried
 * forward without a second buffer to hold it. */
const uint8_t *idstore_field_ptr(const idstore_t *rec, uint8_t type, uint32_t *len_out) {
    if (!rec) return NULL;
    uint32_t off = IDSTORE_HEADER_LEN;
    uint32_t end = IDSTORE_HEADER_LEN + rec->fields_len;
    while (off + 3u <= end) {
        uint8_t  t    = rec->buf[off];
        uint16_t flen = (uint16_t)rd_u16le(&rec->buf[off + 1]);
        uint32_t voff = off + 3u;
        if (voff + flen > end) break;
        if (t == type) {
            if (len_out) *len_out = flen;
            return &rec->buf[voff];
        }
        off = voff + flen;
    }
    return NULL;
}

void idstore_writer_init(idstore_writer_t *w) {
    memset(w->buf, 0, sizeof(w->buf));
    w->fields_len = 0;
}

int idstore_writer_add_field(idstore_writer_t *w, uint8_t type, const void *val, uint16_t len) {
    if (IDSTORE_HEADER_LEN + w->fields_len + 3u + len > IDSTORE_SIZE_BYTES) return -1;
    uint8_t *p = &w->buf[IDSTORE_HEADER_LEN + w->fields_len];
    p[0] = type;
    wr_u16le(&p[1], len);
    if (len) memcpy(&p[3], val, len);
    w->fields_len += 3u + len;
    return 0;
}

int idstore_writer_commit(idstore_writer_t *w, block_dev_t *dev) {
    if (!dev || !dev->write_blocks) return -1;

    w->buf[0] = IDSTORE_MAGIC0; w->buf[1] = IDSTORE_MAGIC1;
    w->buf[2] = IDSTORE_MAGIC2; w->buf[3] = IDSTORE_MAGIC3;
    w->buf[4] = IDSTORE_VERSION;
    w->buf[5] = 0;
    wr_u16le(&w->buf[6], (uint16_t)(IDSTORE_HEADER_LEN + w->fields_len));
    wr_u32le(&w->buf[8], record_crc(w->buf, w->fields_len));

    return dev->write_blocks(dev, w->buf, 0, IDSTORE_BLOCKS);
}

/* --- Self-test (I1) ----------------------------------------------------- */

/* NULL outside idstore_selftest() itself: the buffer is palloc'd for the
 * duration of the test and freed at the end, rather than a permanent 4 KB
 * static -- the same C5 argument drivers/ramdisk.c already makes ("the RAM
 * disk's storage, taken from the page allocator rather than reserved in
 * .bss") applies just as directly to a buffer that exists only to be read
 * and thrown away by one shell command. RP2350's heap is the one this
 * actually matters for; QEMU's is large enough not to care either way. */
static uint8_t *g_fake_disk;

static int fake_read(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (!g_fake_disk || lba + count > IDSTORE_BLOCKS) return -1;
    memcpy(buf, &g_fake_disk[lba * IDSTORE_BLOCK_SIZE], count * IDSTORE_BLOCK_SIZE);
    return 0;
}
static int fake_write(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (!g_fake_disk || lba + count > IDSTORE_BLOCKS) return -1;
    memcpy(&g_fake_disk[lba * IDSTORE_BLOCK_SIZE], buf, count * IDSTORE_BLOCK_SIZE);
    return 0;
}
static block_dev_t g_fake_dev = {
    .name = "idstore-selftest-fake",
    .block_size = IDSTORE_BLOCK_SIZE,
    .num_blocks = IDSTORE_BLOCKS,
    .read_blocks = fake_read,
    .write_blocks = fake_write,
};

int idstore_selftest(void) {
    int failures = 0;
    cprintf("Identity store selftest:\n");

    _Static_assert(IDSTORE_SIZE_BYTES == PAGE_SIZE, "the fake disk below assumes one page holds one record");
    g_fake_disk = (uint8_t *)palloc_pages(1);
    if (!g_fake_disk) {
        cprintf("  [FAIL] no memory for the selftest's fake disk\n");
        cprintf("IDSTORE_SELFTEST_FAIL (1 failed)\n");
        return 1;
    }

    /* Erased flash (all 0xFF) reads as unprovisioned. */
    {
        memset(g_fake_disk, 0xFF, IDSTORE_SIZE_BYTES);
        idstore_t rec;
        bool ok = idstore_read(&g_fake_dev, &rec) == IDSTORE_UNPROVISIONED;
        cprintf("  [%s] erased flash (all 0xFF) reads as unprovisioned\n", ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }

    /* An untouched QEMU disk image (all 0x00) reads as unprovisioned too --
     * the same state, reached the other way, since neither has the magic. */
    {
        memset(g_fake_disk, 0x00, IDSTORE_SIZE_BYTES);
        idstore_t rec;
        bool ok = idstore_read(&g_fake_dev, &rec) == IDSTORE_UNPROVISIONED;
        cprintf("  [%s] a zero-filled disk reads as unprovisioned\n", ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }

    /* A record written by this build reads back byte-identical, and its
     * fields round-trip. */
    uint8_t written_disk[IDSTORE_SIZE_BYTES];
    {
        idstore_writer_t w;
        idstore_writer_init(&w);
        const uint8_t uid[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        const char name[] = "test-node-3f2a";
        bool ok = idstore_writer_add_field(&w, IDSTORE_FIELD_UID, uid, sizeof(uid)) == 0;
        ok = ok && idstore_writer_add_field(&w, IDSTORE_FIELD_NAME, name, (uint16_t)strlen(name)) == 0;
        ok = ok && idstore_writer_commit(&w, &g_fake_dev) == 0;
        memcpy(written_disk, g_fake_disk, sizeof(written_disk));

        idstore_t rec;
        idstore_state_t st = idstore_read(&g_fake_dev, &rec);
        ok = ok && st == IDSTORE_VALID;
        ok = ok && rec.unknown_fields_skipped == 0;
        ok = ok && memcmp(rec.buf, written_disk, sizeof(written_disk)) == 0;

        uint8_t got_uid[8];
        ok = ok && idstore_get_field(&rec, IDSTORE_FIELD_UID, got_uid, sizeof(got_uid)) == (int)sizeof(uid);
        ok = ok && memcmp(got_uid, uid, sizeof(uid)) == 0;

        char got_name[32] = {0};
        int nlen = idstore_get_field(&rec, IDSTORE_FIELD_NAME, got_name, sizeof(got_name) - 1);
        ok = ok && nlen == (int)strlen(name) && strcmp(got_name, name) == 0;

        cprintf("  [%s] a written record reads back byte-identical and its fields round-trip\n",
                ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }

    /* Flipping one byte inside the CRC-covered region turns VALID into
     * CORRUPT -- refused, never half-interpreted. */
    {
        memcpy(g_fake_disk, written_disk, IDSTORE_SIZE_BYTES);
        g_fake_disk[IDSTORE_HEADER_LEN] ^= 0x01;  /* first byte of field data */
        idstore_t rec;
        bool ok = idstore_read(&g_fake_dev, &rec) == IDSTORE_CORRUPT;
        cprintf("  [%s] a flipped bit is refused as corrupt, not half-read\n", ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }

    /* An unrecognised field type is skipped and counted, not fatal -- this
     * is what lets I6 add a field type without invalidating what I2 wrote. */
    {
        idstore_writer_t w;
        idstore_writer_init(&w);
        const uint8_t uid[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
        const uint8_t future[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
        bool ok = idstore_writer_add_field(&w, IDSTORE_FIELD_UID, uid, sizeof(uid)) == 0;
        ok = ok && idstore_writer_add_field(&w, 200 /* not a type this build knows */, future, sizeof(future)) == 0;
        const char name[] = "after-unknown";
        ok = ok && idstore_writer_add_field(&w, IDSTORE_FIELD_NAME, name, (uint16_t)strlen(name)) == 0;
        ok = ok && idstore_writer_commit(&w, &g_fake_dev) == 0;

        idstore_t rec;
        idstore_state_t st = idstore_read(&g_fake_dev, &rec);
        ok = ok && st == IDSTORE_VALID;
        ok = ok && rec.unknown_fields_skipped == 1;

        char got_name[32] = {0};
        int nlen = idstore_get_field(&rec, IDSTORE_FIELD_NAME, got_name, sizeof(got_name) - 1);
        ok = ok && nlen == (int)strlen(name) && strcmp(got_name, name) == 0;

        cprintf("  [%s] an unknown field type is skipped and counted, and fields after it still parse\n",
                ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }

    /* I3: a devkey field round-trips like any other -- it is a secret only
     * in the sense that nothing ever prints it (kernel/shell.c's toolset),
     * never in the sense that idstore itself treats it specially. */
    {
        idstore_writer_t w;
        idstore_writer_init(&w);
        const uint8_t key[32] = {
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
            0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
        };
        bool ok = idstore_writer_add_field(&w, IDSTORE_FIELD_DEVKEY, key, sizeof(key)) == 0;
        ok = ok && idstore_writer_commit(&w, &g_fake_dev) == 0;

        idstore_t rec;
        idstore_state_t st = idstore_read(&g_fake_dev, &rec);
        ok = ok && st == IDSTORE_VALID;
        ok = ok && rec.unknown_fields_skipped == 0;

        uint8_t got_key[32];
        ok = ok && idstore_get_field(&rec, IDSTORE_FIELD_DEVKEY, got_key, sizeof(got_key)) == (int)sizeof(key);
        ok = ok && memcmp(got_key, key, sizeof(key)) == 0;

        cprintf("  [%s] a device key field round-trips\n", ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }

    /* I6, §5.3: an SSID and a derived PSK round-trip as two fields, same
     * as any other -- idstore has no notion of "this one is a WPA2
     * credential"; that meaning lives entirely in kernel/identity.c and
     * the toolset above it. */
    {
        idstore_writer_t w;
        idstore_writer_init(&w);
        const char ssid[] = "test-network";
        const uint8_t psk[32] = {
            0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
            0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
        };
        bool ok = idstore_writer_add_field(&w, IDSTORE_FIELD_WLAN_SSID, ssid, (uint16_t)strlen(ssid)) == 0;
        ok = ok && idstore_writer_add_field(&w, IDSTORE_FIELD_WLAN_PSK, psk, sizeof(psk)) == 0;
        ok = ok && idstore_writer_commit(&w, &g_fake_dev) == 0;

        idstore_t rec;
        idstore_state_t st = idstore_read(&g_fake_dev, &rec);
        ok = ok && st == IDSTORE_VALID;
        ok = ok && rec.unknown_fields_skipped == 0;

        char got_ssid[32] = {0};
        int slen = idstore_get_field(&rec, IDSTORE_FIELD_WLAN_SSID, got_ssid, sizeof(got_ssid) - 1);
        ok = ok && slen == (int)strlen(ssid) && strcmp(got_ssid, ssid) == 0;

        uint8_t got_psk[32];
        ok = ok && idstore_get_field(&rec, IDSTORE_FIELD_WLAN_PSK, got_psk, sizeof(got_psk)) == (int)sizeof(psk);
        ok = ok && memcmp(got_psk, psk, sizeof(psk)) == 0;

        cprintf("  [%s] a WLAN ssid+psk pair round-trips\n", ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }

    /* I3, §4: the store's own reserved paths are refused, matching
     * fs/9p.c's p9_auth_path_is_secret() for the key directory. Pure string
     * logic -- no device, no 9P server -- which is the whole point of
     * putting the test here rather than with the server. */
    {
        static const struct { const char *path; bool secret; } CASES[] = {
            { "/dev/identity",       true  },
            { "/dev/identity0",      true  },
            { "/proc/node",          false },  /* the safe, fingerprint-only report (I3 extends this) */
            { "/sd0/system/etc",     false },
        };
        bool ok = true;
        for (unsigned i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
            if (idstore_path_is_secret(CASES[i].path) != CASES[i].secret) {
                cprintf("        %s: expected %s\n", CASES[i].path,
                        CASES[i].secret ? "refused" : "servable");
                ok = false;
            }
        }
        cprintf("  [%s] the store's reserved paths are refused, and only those\n", ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }

    palloc_free(g_fake_disk, 1);
    g_fake_disk = NULL;

    if (failures == 0) cprintf("IDSTORE_SELFTEST_OK (8/8)\n");
    else               cprintf("IDSTORE_SELFTEST_FAIL (%d failed)\n", failures);
    return failures;
}
