/* The RP2350's half of updating over the namespace (kernel/include/kernel/ota.h):
 * the bootrom's A/B partitions (datasheet 5.1.7) with try-before-you-buy
 * (5.1.17), and a second stage of our own on top.
 *
 * ## The two stages
 *
 * A pushed image goes to the slot this boot is not running from, flagged
 * try-before-you-buy (tools/lota.py sets the bit before it hashes the image).
 * `reboot` is a "flash update" boot of that slot (rp2350_reboot_flash_update);
 * a TBYB image is never chosen by any other boot. The bootrom then gives the
 * kernel 16.7 s to call explicit_buy, which this does as soon as the flash
 * driver is usable (ota_board_boot()). That is stage one, and it is all the
 * bootrom offers: an image that dies before it buys is dropped by the next
 * reset.
 *
 * A Wi-Fi join and a DHCP lease do not fit in 16.7 s, so what "healthy" means
 * is judged in stage two, by the record (two sectors, the format of
 * kernel/include/kernel/ota_image.h) and a deadman. A bought image is the
 * bootrom's preferred one whatever happens next, so the record is what
 * remembers that it is still on trial: it is marked `tried` at its first boot,
 * and a boot that finds it already tried is a trial that did not confirm. That
 * boot rolls back by erasing the failed slot's first sector -- the bootrom's
 * own device for a downgrade -- and resetting, so the old image is the only
 * one the bootrom can find. Until the trial confirms, the deadman reset is
 * re-armed every second (ota_board_trial_tick()), so a hang ends the same way.
 *
 * ## Reading and writing
 *
 * The bootrom maps only the booted partition at 0x10000000, so the other slot,
 * the headers and the records are read through the uncached untranslated XIP
 * alias, which has no translation to set up and is never stale after a write.
 * Writes are a sector at a time from an SRAM page (drivers/flash_rp2350.c); a
 * slot's update header lives in the slot's last sector, because the first
 * bytes are the image the bootrom looks for. */

#include "kernel/ota.h"
#include "kernel/device.h"
#include "kernel/identity.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "drivers/flash_rp2350.h"
#include "arch/rp2350_bootrom.h"
#include "fs/vfs.h"
#include "net/netif.h"
#include <string.h>

#define SECTOR          4096u
#define UNTRANSLATED    0x1c000000u         /* XIP, uncached, no address translation */
#define TRIAL_S         60u
#define DEADMAN_MS      15000u              /* the longest the watchdog holds */
#define PICOBIN_MARKER  0xffffded3u
#define IMAGE_DEF_TBYB  0x8000u             /* in the IMAGE_DEF item's flags (datasheet 5.9.3.1) */

static const uint32_t g_phys[2] = { LUGALOS_SLOT_A_PHYS, LUGALOS_SLOT_B_PHYS };

/* accepted: set by ota_board_boot() before the 9P and trial tasks exist, and
 * only ever read afterwards; -2: not asked yet, -1: not from the A/B pair. */
static int  g_running = -2;
/* accepted: written by ota_board_boot() before any task runs, then cleared by
 * the confirming path under kernel/ota.c's lock; a stale read costs one more
 * deadman re-arm. */
static bool g_trial;
/* accepted: set once at boot, then only read. */
static bool g_fell_back;
/* guarded by: kernel/ota.c's lock, which every write path holds; the boot
 * path uses them before any task exists. */
static uint8_t *g_sec;                      /* the sector being gathered */
static uint32_t g_sec_phys;                 /* its storage address; 0: none */

static void flash_rd(uint32_t phys, void *buf, uint32_t len) {
    memcpy(buf, (const void *)(uintptr_t)(UNTRANSLATED + phys), len);
}

static int running(void) {
    if (g_running == -2) g_running = rp2350_boot_slot();
    return g_running;
}

/* Only the slot this boot is not running from, the records, and -- for a
 * rollback -- the failed slot's first sector. Everything else in flash is the
 * kernel's own image or /flash0. */
static bool writable(uint32_t phys, bool rollback) {
    int run = running();
    if (run < 0) return false;
    if (phys >= LUGALOS_BOOTREC_PHYS && phys < LUGALOS_BOOTREC_PHYS + 2u * SECTOR) return true;
    uint32_t other = g_phys[run ^ 1];
    if (phys >= other && phys < other + LUGALOS_SLOT_SPAN) return true;
    return rollback && phys == g_phys[run];
}

static int write_sector(uint32_t phys, const void *data, bool rollback) {
    if (!writable(phys, rollback)) return -1;
    return flash_rp2350_write_sector(phys, data);
}

/* A zeroed-to-ones page for a short write. NULL if there is no memory. */
static uint8_t *page(void) {
    if (!g_sec) g_sec = palloc_pages(1);
    return g_sec;
}

static int erase_sector(uint32_t phys, bool rollback) {
    uint8_t *pg = page();
    if (!pg) return -1;
    memset(pg, 0xff, SECTOR);
    return write_sector(phys, pg, rollback);
}

bool ota_board_info(ota_board_info_t *out) {
    int run = running();
    out->nslots = 2;
    out->chip = "rp2350";
    out->chip_rev = 0;
    out->trial_s = TRIAL_S;
    out->running = run;
    out->trial = g_trial;
    out->fell_back = g_fell_back;
    out->part_max[0] = LUGALOS_SLOT_SIZE;
    out->part_max[1] = 0;
    out->layout = run >= 0 ? "partition" : "none";
    return true;
}

int ota_board_begin(int slot) {
    if (slot < 0 || slot > 1 || slot == running() || !page()) return -1;
    g_sec_phys = 0;
    /* The header first, then the image's first sector: the slot is empty now,
     * and a slot cut off half-way has no block loop for the bootrom to find. */
    if (erase_sector(g_phys[slot] + LUGALOS_SLOT_HDR_OFF, false) != 0) return -1;
    return erase_sector(g_phys[slot], false);
}

static int flush(void) {
    if (!g_sec_phys) return 0;
    int rc = write_sector(g_sec_phys, g_sec, false);
    g_sec_phys = 0;
    sched_yield();
    return rc;
}

int ota_board_write(int slot, const ota_image_header_t *h, uint32_t off, const void *buf, uint32_t len) {
    (void)h;
    const uint8_t *p = buf;
    if (off > LUGALOS_SLOT_SIZE || len > LUGALOS_SLOT_SIZE - off) return -1;
    while (len) {
        uint32_t phys = g_phys[slot] + off;
        uint32_t sector = phys & ~(SECTOR - 1u);
        uint32_t n = SECTOR - (phys - sector);
        if (n > len) n = len;
        if (sector != g_sec_phys) {
            if (flush() != 0) return -1;
            memset(g_sec, 0xff, SECTOR);
            g_sec_phys = sector;
        }
        memcpy(g_sec + (phys - sector), p, n);
        p += n; off += n; len -= n;
    }
    return 0;
}

int ota_board_finish(int slot, const ota_image_header_t *h) {
    (void)slot; (void)h;
    int rc = flush();
    if (g_sec) { palloc_free(g_sec, 1); g_sec = NULL; }
    return rc;
}

int ota_board_read(int slot, const ota_image_header_t *h, uint32_t off, void *buf, uint32_t len) {
    (void)h;
    if (off > LUGALOS_SLOT_SIZE || len > LUGALOS_SLOT_SIZE - off) return -1;
    flash_rd(g_phys[slot] + off, buf, len);
    return 0;
}

int ota_board_raw_read(int slot, uint32_t off, void *buf, uint32_t len) {
    if (slot < 0 || slot > 1) return -1;
    if (off >= LUGALOS_SLOT_SPAN) return 0;
    if (len > LUGALOS_SLOT_SPAN - off) len = LUGALOS_SLOT_SPAN - off;
    flash_rd(g_phys[slot] + off, buf, len);
    return (int)len;
}

/* The IMAGE_DEF at the start of the slot is flagged try-before-you-buy:
 * otherwise nothing keeps a half-trusted image from being the bootrom's
 * choice on an ordinary boot. Walks the first block's items. */
static bool image_is_tbyb(int slot) {
    uint32_t w[1024];
    flash_rd(g_phys[slot], w, sizeof w);
    for (unsigned i = 0; i < 1024; i++) {
        if (w[i] != PICOBIN_MARKER) continue;
        for (unsigned j = i + 1; j < 1024; ) {
            uint32_t item = w[j];
            if ((item & 0x7fu) == 0x42u) return ((item >> 16) & IMAGE_DEF_TBYB) != 0;
            unsigned size = (item & 0x80u) ? (item >> 8) & 0xffffu : (item >> 8) & 0xffu;
            if ((item & 0xffu) == 0xffu || size == 0) break;
            j += size;
        }
    }
    return false;
}

int ota_board_seal(int slot, const ota_image_header_t *h) {
    if (!image_is_tbyb(slot)) {
        printk("[OTA] slot %c's image is not flagged try-before-you-buy: not sealing it\n", 'a' + slot);
        return -1;
    }
    uint8_t *pg = page();
    if (!pg) return -1;
    memset(pg, 0xff, SECTOR);
    memcpy(pg, h, sizeof *h);
    return write_sector(g_phys[slot] + LUGALOS_SLOT_HDR_OFF, pg, false);
}

int ota_board_header(int slot, ota_image_header_t *h) {
    if (slot < 0 || slot > 1) return -1;
    flash_rd(g_phys[slot] + LUGALOS_SLOT_HDR_OFF, h, sizeof *h);
    return ota_header_valid(h) ? 0 : -1;
}

/* --- the records ----------------------------------------------------------------- */

static int read_records(ota_bootrec_t r[2], const ota_bootrec_t *rec[2]) {
    for (int i = 0; i < 2; i++) {
        flash_rd(LUGALOS_BOOTREC_PHYS + (uint32_t)i * SECTOR, &r[i], sizeof r[i]);
        rec[i] = &r[i];
    }
    return ota_bootrec_current(rec, 2);
}

/* A new record in the sector the current one is not in. `tried` and `failed`
 * are written as given: all ones, or zero once set (their CRC does not cover them). */
static int write_record(int active, int pending, bool tried, bool failed, bool rollback) {
    ota_bootrec_t r[2], nr;
    const ota_bootrec_t *rec[2];
    int cur = read_records(r, rec);
    int target = cur == 0 ? 1 : 0;
    uint32_t phys = LUGALOS_BOOTREC_PHYS + (uint32_t)target * SECTOR;
    ota_bootrec_make(&nr, cur >= 0 ? r[cur].seq + 1u : 1u, (uint8_t)active, (uint8_t)pending);
    if (tried) nr.tried = 0;
    if (failed) nr.failed = 0;
    uint8_t *pg = page();
    if (!pg) return -1;
    memset(pg, 0xff, SECTOR);
    memcpy(pg, &nr, sizeof nr);
    ota_bootrec_t back;
    if (write_sector(phys, pg, rollback) != 0) return -1;
    flash_rd(phys, &back, sizeof back);
    return memcmp(&back, &nr, sizeof nr) == 0 ? 0 : -1;
}

bool ota_board_record(ota_board_record_t *out) {
    ota_bootrec_t r[2];
    const ota_bootrec_t *rec[2];
    int cur = read_records(r, rec);
    if (cur < 0) {
        out->active = -1;
        out->pending = -1;
        out->tried = out->failed = false;
        return true;
    }
    out->active = r[cur].active;
    out->pending = r[cur].pending == OTA_SLOT_NONE ? -1 : r[cur].pending;
    out->tried = r[cur].tried != 0xffffffffu;
    out->failed = r[cur].failed != 0xffffffffu;
    return true;
}

int ota_board_set_pending(int slot) {
    if (running() < 0) return -1;
    return write_record(running(), slot, false, false, false);
}

int ota_board_clear_pending(void) {
    if (running() < 0) return -1;
    return write_record(running(), OTA_SLOT_NONE, false, false, false);
}

int ota_board_switch(void) {
    int run = running();
    ota_image_header_t h;
    if (run < 0 || ota_board_header(run ^ 1, &h) != 0) return -1;
    return write_record(run ^ 1, OTA_SLOT_NONE, false, false, false);
}

int ota_board_confirm(void) {
    int run = running();
    if (run < 0 || write_record(run, OTA_SLOT_NONE, false, false, false) != 0) return -1;
    g_trial = false;
    rp2350_reboot_cancel();
    return 0;
}

/* --- boot and restart ------------------------------------------------------------- */

/* An unconfirmed trial is over: this slot's first sector goes (the bootrom can
 * no longer find it), the record names the old slot, and the next boot is the
 * bootrom's own choice. Returns only if there is nothing to go back to. */
static void roll_back(void) {
    int run = running();
    ota_image_header_t h;
    uint32_t first[2];
    flash_rd(g_phys[run ^ 1], first, sizeof first);
    if (ota_board_header(run ^ 1, &h) != 0 || first[0] == 0xffffffffu) {
        printk("[OTA] the trial did not confirm, and slot %c holds nothing to go back to\n", 'a' + (run ^ 1));
        (void)write_record(run, OTA_SLOT_NONE, true, true, false);
        g_trial = false;
        return;
    }
    printk("[OTA] the trial of slot %c did not confirm: going back to slot %c\n", 'a' + run, 'a' + (run ^ 1));
    if (write_record(run ^ 1, OTA_SLOT_NONE, true, true, false) != 0 ||
        erase_sector(g_phys[run], true) != 0) {
        printk("[OTA] could not roll back\n");
        return;
    }
    printk("[OTA] slot %c erased, restarting\n", 'a' + run);
    task_sleep_ms(500);                 /* the USB console is pumped by a task: let that line out */
    (void)rp2350_reboot();
}

void ota_board_boot(void) {
    int run = running();
    if (run < 0) return;
    if (rp2350_boot_buy_pending()) {
        uint8_t *pg = page();
        int rc = pg ? flash_rp2350_explicit_buy(pg) : -1;
        printk("[OTA] explicit buy: %s (%d)\n", rc == 0 ? "done" : "refused", rc);
    }
    ota_bootrec_t r[2];
    const ota_bootrec_t *rec[2];
    int cur = read_records(r, rec);
    if (cur < 0) return;
    if (r[cur].pending == OTA_SLOT_NONE) {
        if (r[cur].active != run) (void)write_record(run, OTA_SLOT_NONE, false, false, false);
        g_fell_back = r[cur].failed != 0xffffffffu && r[cur].active == run;
        return;
    }
    if (r[cur].pending == run) {
        if (r[cur].tried == 0xffffffffu) {
            if (write_record(r[cur].active, run, true, false, false) == 0) {
                g_trial = true;
                (void)rp2350_reboot_after_ms(DEADMAN_MS);
            }
        } else {
            roll_back();
            g_fell_back = true;
        }
        return;
    }
    /* The other slot was pending and this one booted: it never got as far as
     * buying, or the bootrom did not take it. */
    g_fell_back = true;
    (void)write_record(run, OTA_SLOT_NONE, true, true, false);
}

void ota_board_trial_tick(void) {
    if (g_trial) (void)rp2350_reboot_after_ms(DEADMAN_MS);
}

static void restart_task(void *arg) {
    (void)arg;
    task_sleep_ms(300);
    int run = running();
    ota_board_record_t rec;
    if (run >= 0 && ota_board_record(&rec)) {
        if (g_trial) {
            roll_back();
        } else if (rec.pending >= 0 && rec.pending != run) {
            (void)rp2350_reboot_flash_update(g_phys[rec.pending]);
        } else if (rec.active >= 0 && rec.active != run) {
            (void)rp2350_reboot_flash_update(g_phys[rec.active]);
        }
    }
    (void)rp2350_reboot();
}

/* The version in the running image's own IMAGE_DEF block, which is what the
 * bootrom orders by: an update patched by tools/lota.py differs from the one
 * this build was compiled with. Read from the XIP window, where the booted
 * partition starts at 0x10000000. */
/* guarded by: kernel/ota.c's lock, which ota_ctl_read() holds. */
static char g_version[16];

const char *ota_board_version(void) {
    if (g_version[0]) return g_version;
    const volatile uint32_t *w = (const volatile uint32_t *)0x10000000u;
    for (unsigned i = 0; i < 1023; i++) {
        if (w[i] != PICOBIN_MARKER) continue;
        for (unsigned j = i + 1; j < 1022; ) {
            uint32_t item = w[j];
            if ((item & 0x7fu) == 0x48u) {
                uint32_t v = w[j + 1];
                ksnprintf(g_version, sizeof g_version, "%u.%u", (unsigned)(v >> 16), (unsigned)(v & 0xffffu));
                return g_version;
            }
            unsigned size = (item & 0x80u) ? (item >> 8) & 0xffffu : (item >> 8) & 0xffu;
            if ((item & 0xffu) == 0xffu || size == 0) break;
            j += size;
        }
    }
    return NULL;
}

bool ota_board_restart(void) {
    return task_create("otarestart", restart_task, NULL) >= 0;
}

/* A trial is healthy when a volume is mounted and, if a Wi-Fi network is
 * configured, wlan0 has joined it. */
bool ota_board_healthy(void) {
    if (!vfs_volume_mounted("flash0") && !vfs_volume_mounted("sd0")) return false;
    char ssid[NODE_WLAN_SSID_MAX + 1];
    if (!node_wlan_ssid(ssid, sizeof ssid)) return true;
    netif_t *nif = netif_find("wlan0");
    return nif && netif_link_up(nif);
}
