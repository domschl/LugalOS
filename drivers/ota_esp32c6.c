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
#include "kernel/device.h"
#include "kernel/lock.h"
#include "drivers/ota_esp32c6.h"
#include "drivers/flash_esp32c6.h"
#include "kernel/llp_node.h"
#include "kernel/radio_c6.h"
#include "kernel/identity.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "net/netif.h"
#include "lugalos_build_id.h"
#include <string.h>

extern c6_boot_info_t g_c6_boot;                /* arch/riscv/common/xip_esp32c6.c */

/* Standard ESP-IDF application descriptor at offset 0x20 of the first flash-mapped
 * segment (.radio_text at 0x42000020 in linker/esp32c6.ld), verified by esp-hosted-mcu's
 * esp_ota_begin()/esp_ota_write() and the ESP-IDF 2nd-stage bootloader. */
__attribute__((section(".flash.appdesc"), used))
const esp_app_desc_t g_esp_app_desc = {
    .magic_word     = ESP_APP_DESC_MAGIC,
    .secure_version = 0,
    .version        = LUGALOS_BUILD_ID,
    .project_name   = "LugalOS",
    .time           = "00:00:00",
    .date           = "2026-10-17",
    .idf_ver        = "v5.4-lugalos",
};

#define SECTOR 4096u
#define ESP_PARTITION_TABLE_OFFSET 0x8000u
#define ESP_PARTITION_TABLE_MAX_LEN 0xc00u

static const uint32_t g_base[2] = { LUGALOS_C6_SLOT_A_BASE, LUGALOS_C6_SLOT_B_BASE };

/* guarded by: g_idf_lock (idf_ensure_layout()); constant once checked. */
static ota_idf_layout_t g_idf;
static bool g_idf_checked;
static bool g_idf_valid;

static bool booted_by_loader(void) { return g_c6_boot.magic == OTA_C6_BOOT_MAGIC && g_c6_boot.slot < 2; }
static bool booted_by_idf(void)    { return g_c6_boot.magic == OTA_C6_IDF_BOOT_MAGIC && g_c6_boot.slot < 2; }
static bool ota_capable(void)      { return booted_by_loader() || booted_by_idf(); }

/* The partition table, read once. With no usable table there is no guessing:
 * a wrong guess would put the inactive slot over the running one. Updates,
 * switching and confirming are then refused for this boot. */
static bool idf_ensure_layout_locked(void);

/* Once, for the first caller; the others wait for its answer instead of
 * reading "not valid" while it is still being worked out. */
static ylock_t g_idf_lock;

static bool idf_ensure_layout(void) {
    if (!booted_by_idf()) return false;
    ylock_acquire(&g_idf_lock);
    bool ok = idf_ensure_layout_locked();
    ylock_release(&g_idf_lock);
    return ok;
}

static bool idf_ensure_layout_locked(void) {
    if (g_idf_checked) return g_idf_valid;
    g_idf_checked = true;
    uint8_t *buf = palloc_pages(1);
    if (buf) {
        if (c6flash_read(ESP_PARTITION_TABLE_OFFSET, buf, ESP_PARTITION_TABLE_MAX_LEN) == 0)
            g_idf_valid = ota_idf_parse_partitions(buf, ESP_PARTITION_TABLE_MAX_LEN, &g_idf);
        palloc_free(buf, 1);
    }
    if (!g_idf_valid) {
        printk("[OTA] no usable ESP-IDF partition table at 0x%x: updates disabled\n",
               (unsigned)ESP_PARTITION_TABLE_OFFSET);
        return false;
    }
    if (g_idf.otadata_base != OTA_C6_IDF_OTADATA_BASE || g_idf.slot_base[0] != OTA_C6_IDF_OTA0_BASE) {
        printk("[OTA] partition table has otadata 0x%x, ota_0 0x%x; boot handling expects 0x%x, 0x%x: "
               "updates disabled\n", (unsigned)g_idf.otadata_base, (unsigned)g_idf.slot_base[0],
               (unsigned)OTA_C6_IDF_OTADATA_BASE, (unsigned)OTA_C6_IDF_OTA0_BASE);
        g_idf_valid = false;
        return false;
    }
    if (g_c6_boot.xip_base == g_idf.slot_base[0]) {
        g_c6_boot.slot = 0;
    } else if (g_c6_boot.xip_base == g_idf.slot_base[1]) {
        g_c6_boot.slot = 1;
    } else {
        printk("[OTA] running from 0x%x, which is neither ota_0 nor ota_1: updates disabled\n",
               (unsigned)g_c6_boot.xip_base);
        g_idf_valid = false;
        return false;
    }
    return true;
}

static bool inside(uint32_t addr, uint32_t len, uint32_t base, uint32_t size) {
    return addr >= base && len <= size && addr - base <= size - len;
}

bool ota_c6_idf_writable(uint32_t addr, uint32_t len) {
    if (!idf_ensure_layout()) return false;
    if (inside(addr, len, g_idf.otadata_base, g_idf.otadata_size)) return true;
    uint32_t other = g_c6_boot.slot ^ 1u;
    return inside(addr, len, g_idf.slot_base[other], g_idf.slot_size[other]);
}

bool ota_board_info(ota_board_info_t *out) {
    out->nslots = 2;
    out->chip = "esp32c6";
    out->chip_rev = 0;
    out->trial_s = OTA_C6_TRIAL_S;
    if (booted_by_idf() && idf_ensure_layout()) {
        uint32_t other = g_c6_boot.slot ^ 1u;
        out->running = (int)g_c6_boot.slot;
        out->trial = (g_c6_boot.flags & OTA_C6_FLAG_TRIAL) != 0;
        out->fell_back = (g_c6_boot.flags & OTA_C6_FLAG_FELL_BACK) != 0;
        out->part_max[0] = g_idf.slot_size[other];
        out->part_max[1] = 0;
        out->layout = "idf";
        return true;
    }
    out->running = booted_by_loader() ? (int)g_c6_boot.slot : -1;
    out->trial = booted_by_loader() && (g_c6_boot.flags & OTA_C6_FLAG_TRIAL);
    out->fell_back = booted_by_loader() && (g_c6_boot.flags & OTA_C6_FLAG_FELL_BACK);
    out->part_max[0] = OTA_C6_PART0_MAX;
    out->part_max[1] = (uint32_t)LUGALOS_C6_SLOT_SIZE - OTA_C6_PART1_OFF;
    out->layout = booted_by_loader() ? "loader" : booted_by_idf() ? "idf" : "none";
    return true;
}

/* Where byte `off` of the parts lands in flash. */
static uint32_t map(int slot, const ota_image_header_t *h, uint32_t off) {
    if (booted_by_idf()) return g_idf.slot_base[slot] + off;
    return off < h->part_len[0] ? g_base[slot] + OTA_C6_PART0_OFF + off
                                : g_base[slot] + OTA_C6_PART1_OFF + (off - h->part_len[0]);
}

/* The sector being gathered: its flash address (0: none) and its bytes. */
/* guarded by: kernel/ota.c's lock, which every write path holds. */
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
    if (booted_by_idf() && !idf_ensure_layout()) return -1;
    if (!g_sec && !(g_sec = palloc_pages(1))) return -1;
    g_sec_addr = 0;
    uint32_t base = booted_by_idf() ? g_idf.slot_base[slot] : g_base[slot];
    return c6flash_erase_sector(base);                  /* the header goes first: the slot is empty now */
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

int ota_board_raw_read(int slot, uint32_t off, void *buf, uint32_t len) {
    if (slot < 0 || slot > 1) return -1;
    if (booted_by_idf() && !idf_ensure_layout()) return -1;
    uint32_t base = booted_by_idf() ? g_idf.slot_base[slot] : g_base[slot];
    uint32_t size = booted_by_idf() ? g_idf.slot_size[slot] : (uint32_t)LUGALOS_C6_SLOT_SIZE;
    if (off >= size) return 0;
    if (len > size - off) len = size - off;
    return c6flash_read(base + off, buf, len) == 0 ? (int)len : -1;
}

static int idf_slot_app_desc(int slot, esp_app_desc_t *desc_out) {
    if (!idf_ensure_layout() || slot < 0 || slot > 1) return -1;
    uint32_t hdr0 = 0;
    uint32_t base = g_idf.slot_base[slot];
    if (c6flash_read(base, &hdr0, 4) != 0 || (hdr0 & 0xffu) != 0xE9u) return -1;
    if (c6flash_read(base + 0x20u, desc_out, sizeof *desc_out) != 0 ||
        desc_out->magic_word != ESP_APP_DESC_MAGIC)
        return -1;
    return 0;
}

int ota_board_seal(int slot, const ota_image_header_t *h) {
    if (booted_by_idf()) {
        esp_app_desc_t desc;
        return idf_slot_app_desc(slot, &desc);
    }
    return c6flash_write(g_base[slot], h, sizeof *h);
}

int ota_board_header(int slot, ota_image_header_t *h) {
    if (slot < 0 || slot > 1) return -1;
    if (booted_by_idf()) {
        esp_app_desc_t desc;
        if (idf_slot_app_desc(slot, &desc) != 0) return -1;
        memset(h, 0, sizeof *h);
        if (strncmp(desc.project_name, "LugalOS", 8) == 0)
            ksnprintf(h->build_id, sizeof h->build_id, "%.31s", desc.version);
        else
            ksnprintf(h->build_id, sizeof h->build_id, "%.15s:%.15s", desc.project_name, desc.version);
        return 0;
    }
    if (c6flash_read(g_base[slot], h, sizeof *h) != 0) return -1;
    return ota_header_valid(h) ? 0 : -1;
}

/* --- the boot records (loader & ESP-IDF otadata) --------------------------------- */

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

static int read_idf_otadata(esp_ota_select_entry_t s[2]) {
    memset(s, 0xff, 2 * sizeof(esp_ota_select_entry_t));
    if (!idf_ensure_layout()) return -1;
    for (int i = 0; i < 2; i++)
        (void)c6flash_read(g_idf.otadata_base + (uint32_t)i * SECTOR, &s[i], sizeof s[i]);
    return ota_idf_active_sector(s);
}

static int write_idf_otadata(int target_sec, uint32_t seq, uint32_t state, bool trial_label) {
    if (!idf_ensure_layout() || target_sec < 0 || target_sec > 1) return -1;
    esp_ota_select_entry_t nr, back;
    ota_idf_select_make(&nr, seq, state, trial_label);
    uint32_t addr = g_idf.otadata_base + (uint32_t)target_sec * SECTOR;
    if (c6flash_erase_sector(addr) != 0 || c6flash_write(addr, &nr, sizeof nr) != 0 ||
        c6flash_read(addr, &back, sizeof back) != 0 || memcmp(&back, &nr, sizeof nr) != 0)
        return -1;
    return 0;
}

/* The running trial's entry has been rewritten VALID by ota_board_confirm(). */
static bool idf_running_confirmed(const esp_ota_select_entry_t s[2]) {
    for (int i = 0; i < 2; i++)
        if (ota_idf_select_valid(&s[i]) && s[i].ota_state == ESP_OTA_IMG_VALID &&
            ((s[i].ota_seq - 1u) & 1u) == g_c6_boot.slot)
            return true;
    return false;
}

bool ota_board_record(ota_board_record_t *out) {
    if (booted_by_idf()) {
        if (!idf_ensure_layout()) return false;
        esp_ota_select_entry_t s[2];
        int cur = read_idf_otadata(s);
        if (cur < 0) {
            out->active = 0;
            out->pending = -1;
            out->tried = out->failed = false;
            return true;
        }
        int cur_slot = (int)((s[cur].ota_seq - 1u) & 1u);
        if (ota_idf_select_is_trial(&s[cur]) && s[cur].ota_state == ESP_OTA_IMG_UNDEFINED) {
            out->active = cur_slot ^ 1;
            out->pending = cur_slot;
            out->tried = false;
            out->failed = false;
        } else if ((g_c6_boot.flags & OTA_C6_FLAG_TRIAL) && !idf_running_confirmed(s)) {
            out->active = (int)(g_c6_boot.slot ^ 1u);
            out->pending = (int)g_c6_boot.slot;
            out->tried = true;
            out->failed = false;
        } else {
            out->active = cur_slot;
            out->pending = -1;
            out->tried = (g_c6_boot.flags & OTA_C6_FLAG_FELL_BACK) != 0;
            out->failed = (g_c6_boot.flags & OTA_C6_FLAG_FELL_BACK) != 0;
        }
        return true;
    }
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
    if (booted_by_idf()) {
        esp_app_desc_t desc;
        if (idf_slot_app_desc(slot, &desc) != 0) return -1;
        esp_ota_select_entry_t s[2];
        int cur = read_idf_otadata(s);
        uint32_t cur_seq = cur >= 0 ? s[cur].ota_seq : 0u;
        int target = cur == 0 ? 1 : 0;
        uint32_t next_seq = ota_idf_next_seq(cur_seq, slot);
        bool is_lugalos = strncmp(desc.project_name, "LugalOS", 8) == 0;
        uint32_t state = is_lugalos ? ESP_OTA_IMG_UNDEFINED : ESP_OTA_IMG_VALID;
        return write_idf_otadata(target, next_seq, state, is_lugalos);
    }
    if (!booted_by_loader()) return -1;
    return write_record((int)g_c6_boot.slot, slot);
}

int ota_board_clear_pending(void) {
    if (booted_by_idf()) {
        esp_ota_select_entry_t s[2];
        int cur = read_idf_otadata(s);
        if (cur >= 0 && (uint32_t)((s[cur].ota_seq - 1u) & 1u) != g_c6_boot.slot)
            return c6flash_erase_sector(g_idf.otadata_base + (uint32_t)cur * SECTOR);
        return 0;
    }
    if (!booted_by_loader()) return -1;
    return write_record((int)g_c6_boot.slot, OTA_SLOT_NONE);
}

int ota_board_switch(void) {
    if (!ota_capable()) return -1;
    int other = (int)g_c6_boot.slot ^ 1;
    ota_image_header_t h;
    if (ota_board_header(other, &h) != 0) return -1;
    if (booted_by_idf()) {
        esp_ota_select_entry_t s[2];
        int cur = read_idf_otadata(s);
        uint32_t cur_seq = cur >= 0 ? s[cur].ota_seq : 0u;
        int target = cur == 0 ? 1 : 0;
        uint32_t next_seq = ota_idf_next_seq(cur_seq, other);
        return write_idf_otadata(target, next_seq, ESP_OTA_IMG_VALID, false);
    }
    return write_record(other, OTA_SLOT_NONE);
}

/* The RTC watchdog the loader (or esp32c6_xip_map) armed for the trial. */
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
    if (booted_by_idf()) {
        esp_ota_select_entry_t s[2];
        if (!idf_ensure_layout()) return -1;
        (void)read_idf_otadata(s);
        /* The newest entry naming this slot. Its state is ABORTED already
         * (esp32c6_xip_map() marks a trial so before it starts), so the CRC is
         * the test, not ota_idf_select_valid(). */
        int target = -1;
        for (int i = 0; i < 2; i++) {
            if (s[i].ota_seq == 0xffffffffu || s[i].ota_seq == 0u ||
                s[i].crc != ota_idf_crc32(s[i].ota_seq) ||
                ((s[i].ota_seq - 1u) & 1u) != g_c6_boot.slot)
                continue;
            if (target < 0 || s[i].ota_seq > s[target].ota_seq) target = i;
        }
        if (target < 0) return -1;
        if (write_idf_otadata(target, s[target].ota_seq, ESP_OTA_IMG_VALID, false) != 0) return -1;
        trial_watchdog_off();
        return 0;
    }
    if (!booted_by_loader() || write_record((int)g_c6_boot.slot, OTA_SLOT_NONE) != 0) return -1;
    trial_watchdog_off();
    return 0;
}

/* A boot that is not a trial must not inherit one's watchdog: its settings
 * survive the system reset a failed trial ends in (the loader clears it too;
 * this covers a loader that does not). */
void ota_board_boot(void) {
    if (booted_by_idf()) {
        if (!idf_ensure_layout()) {
            if (g_c6_boot.flags & OTA_C6_FLAG_TRIAL)
                printk("[OTA] this trial cannot be confirmed: the watchdog restarts into the previous image\n");
            else
                trial_watchdog_off();
            return;
        }
        if (g_c6_boot.flags & OTA_C6_FLAG_TRIAL) return;
        trial_watchdog_off();
        esp_ota_select_entry_t s[2];
        int cur = read_idf_otadata(s);
        if (cur < 0) return;
        uint32_t cur_slot = (s[cur].ota_seq - 1u) & 1u;
        if (cur_slot != g_c6_boot.slot) {
            /* The newest entry names the other slot, yet the bootloader started
             * this one: it rejected that image (hash, chip revision, segments
             * over its own RAM). Record the running slot as the newest entry so
             * the record says what boots, and report the fall-back. */
            printk("[OTA] the bootloader rejected slot %c: running slot %c\n",
                   (char)('a' + cur_slot), (char)('a' + g_c6_boot.slot));
            g_c6_boot.flags |= OTA_C6_FLAG_FELL_BACK;
            (void)write_idf_otadata(cur ^ 1, ota_idf_next_seq(s[cur].ota_seq, (int)g_c6_boot.slot),
                                    ESP_OTA_IMG_VALID, false);
        } else if (s[cur].ota_state == ESP_OTA_IMG_NEW || s[cur].ota_state == ESP_OTA_IMG_PENDING_VERIFY ||
                   s[cur].ota_state == ESP_OTA_IMG_UNDEFINED) {
            (void)write_idf_otadata(cur, s[cur].ota_seq, ESP_OTA_IMG_VALID, false);
        }
        return;
    }
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

bool ota_board_restart(void) { return board_reset_later(false, 300); }
void ota_board_trial_tick(void) { }
const char *ota_board_version(void) { return NULL; }
