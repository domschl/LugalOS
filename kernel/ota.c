/* Updating a node's own image over its namespace: the files, the checks and
 * the trial (kernel/include/kernel/ota.h). The board's backend does the flash
 * and the booting.
 *
 * One update at a time, in one place: the ctl file's commands, the image
 * file's writes and the trial task all take g_lock. The image arrives in
 * order -- a 9P client writes it front to back -- and a write at any other
 * offset ends the update with an error rather than leaving a hole. */

#include "kernel/ota.h"
#include "kernel/ota_sig.h"
#include "kernel/identity.h"
#include "kernel/device.h"
#include "kernel/lock.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/sha256.h"
#include "kernel/time.h"
#include <string.h>

typedef enum { ST_IDLE, ST_RECEIVING, ST_RECEIVED, ST_COMMITTED, ST_ERROR } ota_state_t;

static const char *const g_state_names[] = { "idle", "receiving", "received", "committed", "error" };

/* guarded by: g_lock (below), which every entry point takes. */
static struct {
    ota_state_t        st;
    int                slot;
    uint32_t           size, received;
    ota_image_header_t *hdr;        /* a heap page while an update is under way, else NULL */
    char               err[80];
    bool               confirmed;       /* this boot's trial confirmed */
    /* The release key, read at boot by ota_start() (the 9P server task that
     * checks an image cannot read the identity store) and refreshed by
     * ota_release_key_changed(). */
    bool               have_key;
    uint8_t            key[NODE_OTA_PUBKEY_LEN];
    uint64_t           unsigned_until_ms;   /* console override, one image, until then */
    const char        *sig_status;          /* the current image's, once its header is in */
} g;

/* Zero is a free ylock: no first-use init, which two tasks could both run
 * and so reset a lock the other already held. */
static ylock_t g_lock;

static void lock(void) { ylock_acquire(&g_lock); }
static void unlock(void) { ylock_release(&g_lock); }

static int fail(const char *why) {
    g.st = ST_ERROR;
    ksnprintf(g.err, sizeof g.err, "%s", why);
    printk("[OTA] %s\n", why);
    return -1;
}

/* The header's page goes back once the update is committed or given up. */
static void release_hdr(void) {
    if (g.hdr) { palloc_free(g.hdr, 1); g.hdr = NULL; }
}

static char slot_letter(int s) { return s < 0 ? '-' : (char)('a' + s); }

/* NUL-terminated within `max`, and equal to `want`. */
static bool field_is(const char *field, uint32_t max, const char *want) {
    uint32_t n = 0;
    while (n < max && field[n]) n++;
    return n < max && strlen(want) == n && memcmp(field, want, n) == 0;
}

#ifndef CONFIG_OTA_REQUIRE_SIGNED
#define CONFIG_OTA_REQUIRE_SIGNED 0
#endif

/* A node with a release key takes only images signed with it; so does a
 * build that sets CONFIG_OTA_REQUIRE_SIGNED, key or not. Otherwise an
 * unsigned image is taken. A signature that is there and wrong is refused
 * everywhere. */
static bool signature_required(void) { return g.have_key || CONFIG_OTA_REQUIRE_SIGNED; }

static int check_signature(void) {
    ota_sig_result_t r = ota_sig_check(g.hdr, g.have_key ? g.key : NULL);
    g.sig_status = ota_sig_result_str(r);
    if (r == OTA_SIG_RESULT_OK) return 0;
    if (r == OTA_SIG_RESULT_UNSIGNED || r == OTA_SIG_RESULT_NO_KEY) {
        if (!signature_required()) return 0;
        if (g.unsigned_until_ms && time_get_ms() < g.unsigned_until_ms) {
            g.unsigned_until_ms = 0;
            g.sig_status = "unsigned, allowed once from the console";
            printk("[OTA] taking an unsigned image: allowed once from the console\n");
            return 0;
        }
        return fail(r == OTA_SIG_RESULT_NO_KEY
                    ? "the image is signed, but this node has no release key to check it with"
                    : "the image is unsigned, and this node takes signed images only");
    }
    char why[80];
    ksnprintf(why, sizeof why, "refused: %s", ota_sig_result_str(r));
    return fail(why);
}

/* The header just completed: is it an image this node may take? */
static int check_header(const ota_board_info_t *info) {
    const ota_image_header_t *h = g.hdr;
    if (!ota_header_valid(h)) return fail("not an update image (header magic, version or CRC)");
    if (!field_is(h->board, sizeof h->board, LUGALOS_BOARD_ID)) return fail("the image is for another board");
    if (!field_is(h->chip, sizeof h->chip, info->chip)) return fail("the image is for another chip");
    if (info->chip_rev && (info->chip_rev < h->chip_rev_min || info->chip_rev > h->chip_rev_max))
        return fail("the image is for another chip revision");
    if (h->part_len[0] > info->part_max[0] || h->part_len[1] > info->part_max[1])
        return fail("a part of the image does not fit a slot");
    if ((uint64_t)OTA_HEADER_LEN + h->part_len[0] + h->part_len[1] != g.size)
        return fail("the header's parts do not add up to the size given to `begin`");
    return check_signature();
}

/* The header passed: only now is anything in flash touched. A committed image
 * not yet booted may live in the slot about to be overwritten -- the next boot
 * must not try it -- and the slot's own header goes first, so a slot cut off
 * half-written is an empty one. */
static int prepare_slot(void) {
    ota_board_record_t rec;
    if (ota_board_record(&rec) && rec.pending == g.slot && ota_board_clear_pending() != 0)
        return fail("could not withdraw the pending image");
    if (ota_board_begin(g.slot) != 0) return fail("could not prepare the slot");
    printk("[OTA] receiving %.32s into slot %c\n", g.hdr->build_id, slot_letter(g.slot));
    return 0;
}

int ota_image_write(const void *buf, uint32_t len, uint64_t off) {
    lock();
    int rc = (int)len;
    ota_board_info_t info;
    if (g.st != ST_RECEIVING || !ota_board_info(&info)) {
        rc = -1;
    } else if (off != g.received || len > g.size - g.received) {
        rc = fail("the image must be written in order, and no more than `begin` said");
    } else {
        const uint8_t *p = buf;
        uint32_t left = len;
        if (g.received < OTA_HEADER_LEN) {
            uint32_t n = OTA_HEADER_LEN - g.received;
            if (n > left) n = left;
            memcpy((uint8_t *)g.hdr + g.received, p, n);
            g.received += n; p += n; left -= n;
            if (g.received == OTA_HEADER_LEN && (check_header(&info) != 0 || prepare_slot() != 0)) rc = -1;
        }
        if (rc >= 0 && left) {
            if (ota_board_write(g.slot, g.hdr, g.received - OTA_HEADER_LEN, p, left) != 0)
                rc = fail("writing to flash failed");
            else
                g.received += left;
        }
        if (rc >= 0 && g.received == g.size) {
            if (ota_board_finish(g.slot, g.hdr) != 0) rc = fail("writing to flash failed");
            else g.st = ST_RECEIVED;
        }
    }
    unlock();
    return rc;
}

int ota_image_read(void *buf, uint32_t len, uint64_t off) {
    if (off > 0xffffffffu) return 0;
    lock();
    ota_board_info_t info;
    int rc = -1;
    if (g.st == ST_IDLE && ota_board_info(&info) && info.running >= 0) {
        rc = ota_board_raw_read(info.running ^ 1, (uint32_t)off, buf, len);
    }
    unlock();
    return rc;
}

/* What is in the slot's flash, hashed, against the header. */
#define VERIFY_CHUNK 4096u

static int verify(void) {
    uint8_t *chunk = palloc_pages(1);
    if (!chunk) return fail("no memory to check the slot with");
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    uint32_t total = g.hdr->part_len[0] + g.hdr->part_len[1];
    for (uint32_t off = 0; off < total; off += VERIFY_CHUNK) {
        uint32_t n = total - off < VERIFY_CHUNK ? total - off : VERIFY_CHUNK;
        if (ota_board_read(g.slot, g.hdr, off, chunk, n) != 0) {
            palloc_free(chunk, 1);
            return fail("reading the slot back failed");
        }
        sha256_update(&ctx, chunk, n);
        sched_yield();
    }
    palloc_free(chunk, 1);
    uint8_t digest[SHA256_DIGEST_LEN];
    sha256_final(&ctx, digest);
    if (memcmp(digest, g.hdr->sha256, SHA256_DIGEST_LEN) != 0)
        return fail("the slot's SHA-256 does not match the header: not committed");
    return 0;
}

static bool parse_u32(const char *s, uint32_t *out) {
    uint64_t v = 0;
    if (*s < '0' || *s > '9') return false;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint64_t)(*s++ - '0');
        if (v > 0xffffffffu) return false;
    }
    *out = (uint32_t)v;
    return *s == 0;
}

int ota_ctl_write(const char *buf, uint32_t len) {
    char line[64];
    uint32_t n = len < sizeof line - 1 ? len : (uint32_t)sizeof line - 1;
    memcpy(line, buf, n);
    line[n] = 0;
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;

    ota_board_info_t info;
    if (!ota_board_info(&info)) return -1;
    lock();
    int rc = 0;
    if (strncmp(line, "begin ", 6) == 0) {
        uint32_t size;
        if (info.running < 0) {
            rc = fail("this boot cannot update: it was not started by the loader");
        } else if (info.trial && !g.confirmed) {
            rc = fail("the running image is on trial: confirm it first");
        } else if (!parse_u32(line + 6, &size) || size <= OTA_HEADER_LEN ||
                   size - OTA_HEADER_LEN > info.part_max[0] + info.part_max[1]) {
            rc = fail("`begin SIZE`: SIZE is the image's length in bytes, and it must fit a slot");
        } else {
            /* Nothing is erased until the header has arrived and is this board's. */
            if (!g.hdr) g.hdr = palloc_pages(1);
            if (!g.hdr) {
                rc = fail("no memory for the image header");
            } else {
                memset(g.hdr, 0, sizeof *g.hdr);
                g.sig_status = NULL;
                g.slot = info.running ^ 1;
                g.size = size;
                g.received = 0;
                g.err[0] = 0;
                g.st = ST_RECEIVING;
            }
        }
    } else if (strcmp(line, "commit") == 0) {
        if (g.st != ST_RECEIVED) {
            rc = -1;
        } else if (verify() == 0) {
            if (ota_board_seal(g.slot, g.hdr) != 0 || ota_board_set_pending(g.slot) != 0) {
                rc = fail("could not mark the slot to be tried");
            } else {
                g.st = ST_COMMITTED;
                printk("[OTA] slot %c holds %.32s, verified; the next boot tries it once\n",
                       slot_letter(g.slot), g.hdr->build_id);
                release_hdr();
            }
        } else {
            rc = -1;
        }
    } else if (strcmp(line, "switch") == 0) {
        if (info.running < 0) {
            rc = fail("this boot cannot switch slots");
        } else if (info.trial && !g.confirmed) {
            rc = fail("the running image is on trial: confirm it first");
        } else if (ota_board_switch() != 0) {
            rc = fail("could not switch to the other slot");
        } else {
            printk("[OTA] switched active slot to %c\n", slot_letter(info.running ^ 1));
        }
    } else if (strcmp(line, "reboot") == 0) {
        rc = ota_board_restart() ? 0 : -1;
    } else if (strcmp(line, "confirm") == 0) {
        if (!info.trial || g.confirmed) rc = 0;
        else if (ota_board_confirm() == 0) { g.confirmed = true; printk("[OTA] slot %c confirmed\n", slot_letter(info.running)); }
        else rc = -1;
    } else if (strcmp(line, "abort") == 0) {
        release_hdr();
        g.st = ST_IDLE;
        g.err[0] = 0;
    } else {
        rc = -1;
    }
    unlock();
    return rc == 0 ? (int)len : -1;
}

uint32_t ota_ctl_read(char *out, uint32_t cap) {
    ota_board_info_t info;
    if (!ota_board_info(&info)) return (uint32_t)ksnprintf(out, cap, "supported=no\n");
    lock();
    uint32_t n = 0;
#define PUT(...) do { if (n < cap) n += (uint32_t)ksnprintf(out + n, cap - n, __VA_ARGS__); } while (0)
    PUT("board=%s\nchip=%s\nlayout=%s\nrunning=%c\n", LUGALOS_BOARD_ID, info.chip,
        info.layout ? info.layout : "loader", slot_letter(info.running));
    const char *ver = ota_board_version();
    if (ver) PUT("version=%s\n", ver);
    PUT("trial=%s\nfell_back=%s\n", info.trial && !g.confirmed ? "yes" : "no", info.fell_back ? "yes" : "no");
    for (int s = 0; s < (int)info.nslots; s++) {
        ota_image_header_t h;
        if (ota_board_header(s, &h) == 0) PUT("slot_%c=%.32s\n", slot_letter(s), h.build_id);
        else PUT("slot_%c=empty\n", slot_letter(s));
    }
    if (n < cap) n += ota_board_map(out + n, cap - n);
    ota_board_record_t rec;
    if (ota_board_record(&rec))
        PUT("active=%c\npending=%c\ntried=%s\nfailed=%s\n", slot_letter(rec.active), slot_letter(rec.pending),
            rec.tried ? "yes" : "no", rec.failed ? "yes" : "no");
    if (g.have_key) {
        uint8_t id[OTA_KEY_ID_LEN];
        ota_sig_key_id(g.key, id);
        PUT("release_key=");
        for (unsigned i = 0; i < OTA_KEY_ID_LEN; i++) PUT("%02x", id[i]);
        PUT("\n");
    } else {
        PUT("release_key=none\n");
    }
    PUT("signatures=%s\n", signature_required() ? "required" : "optional");
    if (g.sig_status) PUT("image_signature=%s\n", g.sig_status);
    PUT("state=%s\n", g_state_names[g.st]);
    if (g.st == ST_RECEIVING || g.st == ST_RECEIVED) PUT("received=%u/%u\n", (unsigned)g.received, (unsigned)g.size);
    if (g.st == ST_ERROR) PUT("error=%s\n", g.err);
#undef PUT
    unlock();
    return n < cap ? n : cap;
}

/* --- the trial ------------------------------------------------------------------
 * Confirms once the board says it is healthy, after a short settling time; gives
 * up shortly before the watchdog would, so the restart says why. */

#define TRIAL_SETTLE_S 10u

static void trial_task(void *arg) {
    (void)arg;
    ota_board_info_t info;
    if (!ota_board_info(&info)) return;
    uint32_t give_up_s = info.trial_s > TRIAL_SETTLE_S + 10u ? info.trial_s - 10u : info.trial_s;
    for (;;) {
        task_sleep_ms(1000);
        ota_board_trial_tick();
        uint32_t up_s = (uint32_t)(time_get_ms() / 1000u);
        if (up_s >= TRIAL_SETTLE_S && ota_board_healthy()) {
            lock();
            bool ok = g.confirmed || ota_board_confirm() == 0;
            if (ok) g.confirmed = true;
            unlock();
            if (ok) {
                printk("[OTA] slot %c confirmed after %u s\n", slot_letter(info.running), (unsigned)up_s);
                return;
            }
        }
        if (up_s >= give_up_s) {
            printk("[OTA] slot %c is not healthy after %u s: restarting into the previous image\n",
                   slot_letter(info.running), (unsigned)up_s);
            (void)ota_board_restart();
            return;
        }
    }
}

void ota_release_key_changed(void) {
    uint8_t key[NODE_OTA_PUBKEY_LEN];
    bool have = node_ota_pubkey(key);
    lock();
    g.have_key = have;
    if (have) memcpy(g.key, key, sizeof key);
    else memset(g.key, 0, sizeof g.key);
    unlock();
}

void ota_allow_unsigned_once(uint32_t window_s) {
    lock();
    g.unsigned_until_ms = time_get_ms() + (uint64_t)window_s * 1000u;
    unlock();
}

void ota_start(void) {
    ota_release_key_changed();
    ota_board_boot();
    ota_board_info_t info;
    if (!ota_board_info(&info)) return;
    if (info.fell_back)
        printk("[OTA] the last trial did not confirm: running slot %c\n", slot_letter(info.running));
    if (!info.trial) return;
    printk("[OTA] slot %c is on trial: confirming once healthy (deadline %u s)\n",
           slot_letter(info.running), (unsigned)info.trial_s);
    if (task_create("otatrial", trial_task, NULL) < 0) printk("[OTA] could not start the trial task\n");
}
