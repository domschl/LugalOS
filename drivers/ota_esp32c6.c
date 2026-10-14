/* The ESP32-C6's half of updating over the namespace (kernel/include/kernel/ota.h):
 * two slots, the boot records, and the trial watchdog the loader arms
 * (arch/riscv/esp32c6/loader.c). Where an image's parts sit in a slot is
 * drivers/include/drivers/ota_esp32c6.h.
 *
 * A slot is written a sector at a time: the bytes for one 4 KB sector are
 * gathered in a page, then the sector is erased and programmed, and the
 * writer yields. An erase masks interrupts for tens of milliseconds
 * (drivers/flash_esp32c6.c); one per sector, never a 64 KB block at once,
 * keeps the radio and the channel to the host served in between. */

#include "kernel/ota.h"
#include "drivers/ota_esp32c6.h"
#include "drivers/flash_esp32c6.h"
#include "kernel/llp_node.h"
#include "kernel/radio_c6.h"
#include "kernel/identity.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "net/netif.h"
#include <string.h>

extern c6_boot_info_t g_c6_boot;                /* arch/riscv/common/xip_esp32c6.c */

#define SECTOR 4096u
static const uint32_t g_base[2] = { LUGALOS_C6_SLOT_A_BASE, LUGALOS_C6_SLOT_B_BASE };

static bool booted_by_loader(void) { return g_c6_boot.magic == OTA_C6_BOOT_MAGIC && g_c6_boot.slot < 2; }

bool ota_board_info(ota_board_info_t *out) {
    out->nslots = 2;
    out->running = booted_by_loader() ? (int)g_c6_boot.slot : -1;
    out->trial = booted_by_loader() && (g_c6_boot.flags & OTA_C6_FLAG_TRIAL);
    out->fell_back = booted_by_loader() && (g_c6_boot.flags & OTA_C6_FLAG_FELL_BACK);
    out->part_max[0] = OTA_C6_PART0_MAX;
    out->part_max[1] = (uint32_t)LUGALOS_C6_SLOT_SIZE - OTA_C6_PART1_OFF;
    out->chip = "esp32c6";
    out->trial_s = OTA_C6_TRIAL_S;
    return true;
}

/* Where byte `off` of the parts lands in flash. */
static uint32_t map(int slot, const ota_image_header_t *h, uint32_t off) {
    return off < h->part_len[0] ? g_base[slot] + OTA_C6_PART0_OFF + off
                                : g_base[slot] + OTA_C6_PART1_OFF + (off - h->part_len[0]);
}

/* The sector being gathered: its flash address (0: none) and its bytes. */
static uint8_t *g_sec;
static uint32_t g_sec_addr;

static int flush(void) {
    if (!g_sec_addr) return 0;
    int rc = (c6flash_erase_sector(g_sec_addr) == 0 && c6flash_write(g_sec_addr, g_sec, SECTOR) == 0) ? 0 : -1;
    g_sec_addr = 0;
    sched_yield();
    return rc;
}

int ota_board_begin(int slot) {
    if (slot < 0 || slot > 1) return -1;
    if (!g_sec && !(g_sec = palloc_pages(1))) return -1;
    g_sec_addr = 0;
    return c6flash_erase_sector(g_base[slot]);          /* the header goes first: the slot is empty now */
}

int ota_board_write(int slot, const ota_image_header_t *h, uint32_t off, const void *buf, uint32_t len) {
    const uint8_t *p = buf;
    while (len) {
        uint32_t addr = map(slot, h, off);
        uint32_t sector = addr & ~(SECTOR - 1u);
        /* Never across the end of part 0: the next byte lands elsewhere. */
        uint32_t n = SECTOR - (addr - sector);
        if (off < h->part_len[0] && n > h->part_len[0] - off) n = h->part_len[0] - off;
        if (n > len) n = len;
        if (sector != g_sec_addr) {
            if (flush() != 0) return -1;
            memset(g_sec, 0xff, SECTOR);
            g_sec_addr = sector;
        }
        memcpy(g_sec + (addr - sector), p, n);
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
    uint8_t *p = buf;
    while (len) {
        uint32_t n = len;
        if (off < h->part_len[0] && n > h->part_len[0] - off) n = h->part_len[0] - off;
        if (c6flash_read(map(slot, h, off), p, n) != 0) return -1;
        p += n; off += n; len -= n;
    }
    return 0;
}

int ota_board_seal(int slot, const ota_image_header_t *h) {
    return c6flash_write(g_base[slot], h, sizeof *h);
}

int ota_board_header(int slot, ota_image_header_t *h) {
    if (slot < 0 || slot > 1 || c6flash_read(g_base[slot], h, sizeof *h) != 0) return -1;
    return ota_header_valid(h) ? 0 : -1;
}

/* --- the boot records ------------------------------------------------------------ */

static int read_records(ota_bootrec_t r[2], const ota_bootrec_t *rec[2]) {
    for (int i = 0; i < 2; i++)
        rec[i] = c6flash_read(LUGALOS_C6_BOOTREC_BASE + (uint32_t)i * SECTOR, &r[i], sizeof r[i]) == 0 ? &r[i] : NULL;
    return ota_bootrec_current(rec, 2);
}

/* A new record in the sector the current one is not in (kernel/include/kernel/ota_image.h). */
static int write_record(int active, int pending) {
    ota_bootrec_t r[2], nr, back;
    const ota_bootrec_t *rec[2];
    int cur = read_records(r, rec);
    int target = cur == 0 ? 1 : 0;
    uint32_t addr = LUGALOS_C6_BOOTREC_BASE + (uint32_t)target * SECTOR;
    ota_bootrec_make(&nr, cur >= 0 ? r[cur].seq + 1u : 1u, (uint8_t)active, (uint8_t)pending);
    if (c6flash_erase_sector(addr) != 0 || c6flash_write(addr, &nr, sizeof nr) != 0 ||
        c6flash_read(addr, &back, sizeof back) != 0 || memcmp(&back, &nr, sizeof nr) != 0)
        return -1;
    return 0;
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
    if (!booted_by_loader()) return -1;
    return write_record((int)g_c6_boot.slot, slot);
}

int ota_board_clear_pending(void) {
    if (!booted_by_loader()) return -1;
    return write_record((int)g_c6_boot.slot, OTA_SLOT_NONE);
}

/* The RTC watchdog the loader armed for the trial (its registers: loader.c). */
#define LP_WDT_CONFIG0  0x600B1C00u
#define LP_WDT_WPROTECT 0x600B1C18u
#define WDT_WKEY        0x50D83AA1u
#define LP_WDT_EN       (1u << 31)

static void trial_watchdog_off(void) {
    *(volatile uint32_t *)LP_WDT_WPROTECT = WDT_WKEY;
    *(volatile uint32_t *)LP_WDT_CONFIG0 &= ~LP_WDT_EN;
    *(volatile uint32_t *)LP_WDT_WPROTECT = 0;
}

int ota_board_confirm(void) {
    if (!booted_by_loader() || write_record((int)g_c6_boot.slot, OTA_SLOT_NONE) != 0) return -1;
    trial_watchdog_off();
    return 0;
}

/* A boot that is not a trial must not inherit one's watchdog: its settings
 * survive the system reset a failed trial ends in (the loader clears it too;
 * this covers a loader that does not). */
void ota_board_boot(void) {
    if (!(booted_by_loader() && (g_c6_boot.flags & OTA_C6_FLAG_TRIAL))) trial_watchdog_off();
}

/* A trial is healthy when it does what this C6 is for: beside a host, the LLP
 * link to it is up; on its own with a network to join, it has joined; with
 * neither, having run this long is all there is to ask. */
bool ota_board_healthy(void) {
    if (llp_node_p9_link()) return llp_node_link_up();
    char ssid[NODE_WLAN_SSID_MAX + 1];
    if (node_wlan_ssid(ssid, sizeof ssid)) {
        struct netif *nif = radio_c6_netif();
        return nif && netif_link_up(nif);
    }
    return true;
}
