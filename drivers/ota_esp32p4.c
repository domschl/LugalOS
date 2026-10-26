/* The ESP32-P4's half of updating over the namespace (kernel/include/kernel/ota.h):
 * two slots, the boot records, and the trial watchdog the loader arms
 * (arch/riscv/esp32p4/loader.c). Where an image's parts sit in a slot is
 * drivers/include/drivers/ota_esp32p4.h.
 *
 * A slot is written a sector at a time: the bytes for one 4 KB sector are
 * gathered in a page, then the sector is erased and programmed, and the
 * writer yields. An erase masks interrupts and parks the other core for tens
 * of milliseconds (drivers/flash_esp32p4.c); one per sector keeps the
 * network and the channel to the host served in between. Only the slot the
 * XIP window does not map is ever written. */

#include "kernel/ota.h"
#include "kernel/device.h"
#include "kernel/lock.h"
#include "drivers/ota_esp32p4.h"
#include "drivers/flash_esp32p4.h"
#include "fs/vfs.h"
#include "net/netif.h"
#include "kernel/identity.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include <string.h>

extern p4_boot_info_t g_p4_boot;                /* arch/riscv/common/xip_esp32p4.c */
extern unsigned esp32p4_chip_rev(void);         /* drivers/efuse_esp32p4.c */

#define SECTOR FLASH_P4_SECTOR_SIZE

static const uint32_t g_base[2] = { LUGALOS_P4_SLOT_A_BASE, LUGALOS_P4_SLOT_B_BASE };

static bool booted_by_loader(void) { return g_p4_boot.magic == OTA_P4_BOOT_MAGIC && g_p4_boot.slot < 2; }

bool ota_p4_slot_writable(uint32_t addr, uint32_t len) {
    if (!booted_by_loader() || len == 0) return false;
    uint32_t other = g_base[g_p4_boot.slot ^ 1u];
    return addr >= other && len <= LUGALOS_P4_SLOT_SIZE && addr - other <= LUGALOS_P4_SLOT_SIZE - len;
}

bool ota_board_info(ota_board_info_t *out) {
    out->nslots = 2;
    out->chip = "esp32p4";
    out->chip_rev = (uint16_t)esp32p4_chip_rev();
    out->trial_s = OTA_P4_TRIAL_S;
    out->running = booted_by_loader() ? (int)g_p4_boot.slot : -1;
    out->trial = booted_by_loader() && (g_p4_boot.flags & OTA_P4_FLAG_TRIAL);
    out->fell_back = booted_by_loader() && (g_p4_boot.flags & OTA_P4_FLAG_FELL_BACK);
    out->part_max[0] = OTA_P4_PART0_MAX;
    out->part_max[1] = (uint32_t)LUGALOS_P4_SLOT_SIZE - OTA_P4_PART1_OFF;
    out->layout = booted_by_loader() ? "loader" : "none";
    return true;
}

/* Where byte `off` of the parts lands in flash. */
static uint32_t map(int slot, const ota_image_header_t *h, uint32_t off) {
    return off < h->part_len[0] ? g_base[slot] + OTA_P4_PART0_OFF + off
                                : g_base[slot] + OTA_P4_PART1_OFF + (off - h->part_len[0]);
}

/* The sector being gathered: its flash address (0: none) and its bytes. */
/* guarded by: kernel/ota.c's lock, which every write path holds. */
static uint8_t *g_sec;
static uint32_t g_sec_addr;

static int flush(void) {
    if (!g_sec_addr) return 0;
    int rc = (flash_p4_erase_sector(g_sec_addr) == 0 && flash_p4_write(g_sec_addr, g_sec, SECTOR) == 0) ? 0 : -1;
    g_sec_addr = 0;
    sched_yield();
    return rc;
}

int ota_board_begin(int slot) {
    if (slot < 0 || slot > 1 || !flash_p4_init()) return -1;
    if (!g_sec && !(g_sec = palloc_pages(1))) return -1;
    g_sec_addr = 0;
    return flash_p4_erase_sector(g_base[slot]);         /* the header goes first: the slot is empty now */
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
        if (flash_p4_read(map(slot, h, off), p, n) != 0) return -1;
        p += n; off += n; len -= n;
    }
    return 0;
}

int ota_board_raw_read(int slot, uint32_t off, void *buf, uint32_t len) {
    if (slot < 0 || slot > 1 || !flash_p4_init()) return -1;
    if (off >= LUGALOS_P4_SLOT_SIZE) return 0;
    if (len > LUGALOS_P4_SLOT_SIZE - off) len = LUGALOS_P4_SLOT_SIZE - off;
    return flash_p4_read(g_base[slot] + off, buf, len) == 0 ? (int)len : -1;
}

/* Writes the header: from now on the slot holds a complete image. */
int ota_board_seal(int slot, const ota_image_header_t *h) {
    return flash_p4_write(g_base[slot], h, sizeof *h);
}

int ota_board_header(int slot, ota_image_header_t *h) {
    if (slot < 0 || slot > 1 || !flash_p4_init()) return -1;
    if (flash_p4_read(g_base[slot], h, sizeof *h) != 0) return -1;
    return ota_header_valid(h) ? 0 : -1;
}

/* --- the boot records ------------------------------------------------------------ */

static int read_records(ota_bootrec_t r[2], const ota_bootrec_t *rec[2]) {
    for (int i = 0; i < 2; i++)
        rec[i] = flash_p4_read(LUGALOS_P4_BOOTREC_BASE + (uint32_t)i * SECTOR, &r[i], sizeof r[i]) == 0 ? &r[i] : NULL;
    return ota_bootrec_current(rec, 2);
}

/* A new record in the sector the current one is not in (kernel/include/kernel/ota_image.h). */
static int write_record(int active, int pending) {
    ota_bootrec_t r[2], nr, back;
    const ota_bootrec_t *rec[2];
    if (!flash_p4_init()) return -1;
    int cur = read_records(r, rec);
    int target = cur == 0 ? 1 : 0;
    uint32_t addr = LUGALOS_P4_BOOTREC_BASE + (uint32_t)target * SECTOR;
    ota_bootrec_make(&nr, cur >= 0 ? r[cur].seq + 1u : 1u, (uint8_t)active, (uint8_t)pending);
    if (flash_p4_erase_sector(addr) != 0 || flash_p4_write(addr, &nr, sizeof nr) != 0 ||
        flash_p4_read(addr, &back, sizeof back) != 0 || memcmp(&back, &nr, sizeof nr) != 0)
        return -1;
    return 0;
}

bool ota_board_record(ota_board_record_t *out) {
    ota_bootrec_t r[2];
    const ota_bootrec_t *rec[2];
    if (!flash_p4_init()) return false;
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
    return write_record((int)g_p4_boot.slot, slot);
}

int ota_board_clear_pending(void) {
    if (!booted_by_loader()) return -1;
    return write_record((int)g_p4_boot.slot, OTA_SLOT_NONE);
}

int ota_board_switch(void) {
    if (!booted_by_loader()) return -1;
    int other = (int)g_p4_boot.slot ^ 1;
    ota_image_header_t h;
    if (ota_board_header(other, &h) != 0) return -1;
    return write_record(other, OTA_SLOT_NONE);
}

/* The RTC watchdog the loader armed for the trial. */
#define LP_WDT_BASE     0x50116000u
#define LP_WDT_CONFIG0  (LP_WDT_BASE + 0x00u)
#define LP_WDT_WPROTECT (LP_WDT_BASE + 0x18u)
#define WDT_WKEY        0x50D83AA1u
#define LP_WDT_EN       (1u << 31)

static void trial_watchdog_off(void) {
    *(volatile uint32_t *)LP_WDT_WPROTECT = WDT_WKEY;
    *(volatile uint32_t *)LP_WDT_CONFIG0 &= ~LP_WDT_EN;
    *(volatile uint32_t *)LP_WDT_WPROTECT = 0;
}

int ota_board_confirm(void) {
    if (!booted_by_loader() || write_record((int)g_p4_boot.slot, OTA_SLOT_NONE) != 0) return -1;
    trial_watchdog_off();
    return 0;
}

/* A boot that is not a trial must not inherit one's watchdog: its settings
 * survive the system reset a failed trial ends in (the loader clears it too;
 * this covers a kernel started without it). */
void ota_board_boot(void) {
    if (!(booted_by_loader() && (g_p4_boot.flags & OTA_P4_FLAG_TRIAL))) trial_watchdog_off();
}

/* A trial is healthy when the board is up the way it is used: a volume is
 * mounted, and, if a Wi-Fi network is configured, the C6's wlan0 has joined
 * it. A board with no network to join (a cable, or the console, is all it
 * has) is asked nothing about one. */
bool ota_board_healthy(void) {
    if (!vfs_volume_mounted("flash0") && !vfs_volume_mounted("sd0")) return false;
    char ssid[NODE_WLAN_SSID_MAX + 1];
    if (!node_wlan_ssid(ssid, sizeof ssid)) return true;
    netif_t *nif = netif_find("wlan0");
    return nif && netif_link_up(nif);
}

uint32_t ota_board_map(char *out, uint32_t cap) {
    int n = ksnprintf(out, cap,
        "at_loader=0x%x+0x%x\nat_flashfs=0x%x+0x%x\nat_a=0x%x+0x%x\nat_b=0x%x+0x%x\n"
        "at_records=0x%x+0x%x\nat_identity=0x%x+0x%x\n",
        (unsigned)LUGALOS_P4_BOOTLOADER_BASE, 0xE000u,
        (unsigned)LUGALOS_P4_FLASHFS_BASE, (unsigned)LUGALOS_P4_FLASHFS_SIZE,
        (unsigned)LUGALOS_P4_SLOT_A_BASE, (unsigned)LUGALOS_P4_SLOT_SIZE,
        (unsigned)LUGALOS_P4_SLOT_B_BASE, (unsigned)LUGALOS_P4_SLOT_SIZE,
        (unsigned)LUGALOS_P4_BOOTREC_BASE, 2u * SECTOR,
        (unsigned)LUGALOS_P4_IDENTITY_BASE, SECTOR);
    return n < 0 ? 0 : (uint32_t)n < cap ? (uint32_t)n : cap;
}

bool ota_board_restart(void) { return board_reset_later(false, 300); }
void ota_board_trial_tick(void) { }
const char *ota_board_version(void) { return NULL; }
