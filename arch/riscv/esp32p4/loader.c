/* The ESP32-P4 loader: the image the boot ROM loads from flash offset 0x2000,
 * and the one piece of the P4's flash an update never rewrites. It picks a
 * slot from the boot records (kernel/include/kernel/ota_image.h), loads that
 * slot's RAM half into L2MEM and jumps to it, telling the kernel which slot's
 * XIP half to map (drivers/include/drivers/ota_esp32p4.h).
 *
 * It runs from L2MEM in a region the kernel's RAM half never loads into
 * (linker/esp32p4_loader.ld), with interrupts off as the ROM left them. Flash
 * is read through the ROM's SPI routines, as drivers/flash_esp32p4.c does; the
 * only flash write is clearing one word of a boot record, which needs no
 * erase. The ROM bounds-checks addresses against its own record of the
 * chip's size, so the record is set from the flash map first.
 *
 * A trial boot gets the RTC watchdog armed for OTA_P4_TRIAL_S with a system
 * reset. The kernel's early boot clears only the ROM's flash-boot enable of
 * that watchdog, not this one; confirming the image disarms it. A trial that
 * hangs, crashes or never confirms is reset, and this loader then boots the
 * old slot. ROM addresses are from IDF's esp32p4.rom.ld. */

#include "kernel/ota_image.h"
#include "drivers/ota_esp32p4.h"
#include <stdbool.h>
#include <stdint.h>

#if !defined(LUGALOS_P4_BOOTREC_BASE) || !defined(LUGALOS_P4_SLOT_A_BASE) || !defined(LUGALOS_BOARD_ID)
#error "the P4 flash map (cmake/flash_layout_esp32p4.cmake) and the board id are compile definitions"
#endif

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

typedef int (*rom_printf_fn)(const char *fmt, ...);
typedef int (*rom_read_fn)(uint32_t src, uint32_t *dest, int32_t len);
typedef int (*rom_write_fn)(uint32_t dest, const uint32_t *src, int32_t len);
typedef int (*rom_unlock_fn)(void);
typedef int (*rom_config_param_fn)(uint32_t device_id, uint32_t chip_size, uint32_t block_size,
                                   uint32_t sector_size, uint32_t page_size, uint32_t status_mask);
typedef void (*rom_cache_map_fn)(uint32_t map);

#define rom_printf       ((rom_printf_fn)0x4fc00024u)       /* ets_printf */
#define rom_write        ((rom_write_fn)0x4fc00154u)        /* esp_rom_spiflash_write */
#define rom_read         ((rom_read_fn)0x4fc00158u)         /* esp_rom_spiflash_read */
#define rom_unlock       ((rom_unlock_fn)0x4fc0015cu)       /* esp_rom_spiflash_unlock */
#define rom_config_param ((rom_config_param_fn)0x4fc00168u) /* esp_rom_spiflash_config_param */
#define rom_cache_writeback_all ((rom_cache_map_fn)0x4fc00414u)  /* Cache_WriteBack_All */
#define rom_cache_invalidate_all ((rom_cache_map_fn)0x4fc00404u)  /* Cache_Invalidate_All */
#define CACHE_MAP_L1_ICACHE_0   (1u << 0)                   /* esp32p4/rom/cache.h */
#define CACHE_MAP_L1_ICACHE_1   (1u << 1)
#define CACHE_MAP_L1_DCACHE     (1u << 4)

/* The chip drivers/flash_esp32p4.c is configured for: GigaDevice c8 4018. */
#define FLASH_DEVICE_ID     0x001840c8u

/* The RTC watchdog (LP_WDT, IDF soc/esp32p4/register/.../lp_wdt_reg.h). */
#define LP_WDT_BASE         0x50116000u
#define LP_WDT_CONFIG0      (LP_WDT_BASE + 0x00u)
#define LP_WDT_CONFIG1      (LP_WDT_BASE + 0x04u)        /* stage 0 hold, in RTC slow clock ticks */
#define LP_WDT_FEED         (LP_WDT_BASE + 0x14u)
#define LP_WDT_WPROTECT     (LP_WDT_BASE + 0x18u)
#define WDT_WKEY            0x50D83AA1u
#define LP_WDT_EN           (1u << 31)
#define LP_WDT_FLASHBOOT_EN (1u << 12)
#define LP_WDT_STG0_RESET_SYSTEM (3u << 28)
#define LP_WDT_SYS_RESET_LENGTH  (7u << 13)
#define LP_WDT_CPU_RESET_LENGTH  (7u << 16)
/* Measured on the NANO (v1.3): a 60 s hold reset a hung trial after 112 s at
 * 136000 (IDF's SOC_CLK_RC_SLOW_FREQ_APPROX), so the clock runs near 73 kHz. */
#define RC_SLOW_HZ          73000u

#define L2MEM_BASE          0x4ff00000u
#define L2MEM_END           0x4ffc0000u
extern char _loader_start[], _loader_end[];

#define ESP_IMAGE_MAGIC     0xE9u
#define ESP_CHECKSUM_SEED   0xEFu

static const uint32_t g_slot_base[2] = { LUGALOS_P4_SLOT_A_BASE, LUGALOS_P4_SLOT_B_BASE };

/* The loader links no library, and GCC emits these for struct copies. */
void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
void *memset(void *dst, int c, unsigned long n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

static bool flash_read(uint32_t addr, void *dest, uint32_t len) {
    return rom_read(addr, (uint32_t *)dest, (int32_t)len) == 0;
}

static void flash_clear_word(uint32_t addr) {
    uint32_t zero = 0;
    if (rom_unlock() == 0) (void)rom_write(addr, &zero, 4);
}

static bool streq(const char *a, const char *b, uint32_t max) {
    for (uint32_t i = 0; i < max; i++) {
        if (a[i] != b[i]) return false;
        if (!a[i]) return true;
    }
    return true;
}

/* A slot holds an image for this board whose parts fit where they go. */
static bool slot_ok(uint32_t base, ota_image_header_t *h) {
    if (!flash_read(base, h, sizeof *h) || !ota_header_valid(h)) return false;
    if (!streq(h->board, LUGALOS_BOARD_ID, sizeof h->board)) return false;
    return h->part_len[0] > 24u && h->part_len[0] <= OTA_P4_PART0_MAX &&
           h->part_len[1] > 0u && h->part_len[1] <= (uint32_t)LUGALOS_P4_SLOT_SIZE - OTA_P4_PART1_OFF;
}

/* The RAM half: the boot ROM's image format (esptool elf2image), every segment
 * into L2MEM outside this loader, its checksum checked. Returns the entry
 * point, or 0. */
static uint32_t load_ram_half(uint32_t addr, uint32_t len) {
    uint32_t hdr[6];                            /* the ROM reads into words */
    if (!flash_read(addr, hdr, sizeof hdr) || (hdr[0] & 0xffu) != ESP_IMAGE_MAGIC) return 0;
    uint32_t nseg = (hdr[0] >> 8) & 0xffu;
    uint32_t entry = hdr[1];
    uint32_t off = sizeof hdr;
    uint8_t sum = ESP_CHECKSUM_SEED;
    uint32_t lo = (uint32_t)(uintptr_t)_loader_start, hi = (uint32_t)(uintptr_t)_loader_end;
    for (uint32_t s = 0; s < nseg; s++) {
        uint32_t seg[2];
        if (off + 8u > len || !flash_read(addr + off, seg, 8)) return 0;
        off += 8;
        uint32_t dest = seg[0], n = seg[1];
        if (dest < L2MEM_BASE || (dest & 3u) || (n & 3u) || n > len - off || dest + n > L2MEM_END ||
            (dest < hi && dest + n > lo)) {
            rom_printf("[loader] segment %u (0x%08x, %u bytes) does not fit\n",
                       (unsigned)s, (unsigned)dest, (unsigned)n);
            return 0;
        }
        for (uint32_t done = 0; done < n; done += 4096u) {
            uint32_t k = n - done < 4096u ? n - done : 4096u;
            if (!flash_read(addr + off + done, (void *)(uintptr_t)(dest + done), k)) return 0;
        }
        const uint8_t *p = (const uint8_t *)(uintptr_t)dest;
        for (uint32_t i = 0; i < n; i++) sum ^= p[i];
        off += n;
    }
    /* The checksum is the last byte of the 16-byte block the segments end in. */
    uint32_t at = (off | 15u);
    uint32_t blk;
    if (at >= len || !flash_read(addr + (at & ~3u), &blk, 4) || (uint8_t)(blk >> (8u * (at & 3u))) != sum) {
        rom_printf("[loader] the RAM half's checksum does not match\n");
        return 0;
    }
    return entry;
}

/* Every other boot: the watchdog off, and the ROM's flash-boot enable with it.
 * Its settings live in the low-power domain, which a system reset leaves
 * alone -- a trial the watchdog reset would otherwise leave it running in the
 * image booted next, and reset that every OTA_P4_TRIAL_S. */
static void disarm_trial_watchdog(void) {
    REG(LP_WDT_WPROTECT) = WDT_WKEY;
    REG(LP_WDT_CONFIG0) &= ~(LP_WDT_EN | LP_WDT_FLASHBOOT_EN);
    REG(LP_WDT_WPROTECT) = 0;
}

static void arm_trial_watchdog(void) {
    REG(LP_WDT_WPROTECT) = WDT_WKEY;
    REG(LP_WDT_CONFIG1) = OTA_P4_TRIAL_S * RC_SLOW_HZ;
    REG(LP_WDT_CONFIG0) = LP_WDT_EN | LP_WDT_STG0_RESET_SYSTEM | LP_WDT_SYS_RESET_LENGTH | LP_WDT_CPU_RESET_LENGTH;
    REG(LP_WDT_FEED) = 1u << 31;
    REG(LP_WDT_WPROTECT) = 0;
}

void loader_main(void) {
    (void)rom_config_param(FLASH_DEVICE_ID, LUGALOS_P4_FLASH_SIZE, 65536u, 4096u, 256u, 0xffffu);
    static ota_bootrec_t r[2];
    const ota_bootrec_t *rec[2];
    for (int i = 0; i < 2; i++)
        rec[i] = flash_read(LUGALOS_P4_BOOTREC_BASE + (uint32_t)i * 4096u, &r[i], sizeof r[i]) ? &r[i] : 0;
    ota_boot_choice_t c = ota_boot_choose(rec, 2);
    if (c.mark_sector >= 0)
        flash_clear_word(LUGALOS_P4_BOOTREC_BASE + (uint32_t)c.mark_sector * 4096u + c.mark_off);

    uint32_t flags = (c.trial ? OTA_P4_FLAG_TRIAL : 0u) | (c.fell_back ? OTA_P4_FLAG_FELL_BACK : 0u);
    static ota_image_header_t h;
    int slot = c.slot;
    if (!slot_ok(g_slot_base[slot], &h)) {
        rom_printf("[loader] slot %c holds no image for %s\n", 'A' + slot, LUGALOS_BOARD_ID);
        if (c.trial && c.current >= 0) {
            /* The trial cannot even start: count it as failed now. */
            flash_clear_word(LUGALOS_P4_BOOTREC_BASE + (uint32_t)c.current * 4096u + OTA_BOOTREC_FAILED_OFF);
            flags = OTA_P4_FLAG_FELL_BACK;
        }
        /* The record's `active` slot, or -- if that is the one that failed --
         * the other. */
        slot = c.current >= 0 ? r[c.current].active : 0;
        if (slot == c.slot) slot ^= 1;
        if (!slot_ok(g_slot_base[slot], &h)) {
            rom_printf("[loader] slot %c holds none either: nothing to boot\n", 'A' + slot);
            for (;;) { }
        }
        flags &= ~OTA_P4_FLAG_TRIAL;
    }
    uint32_t base = g_slot_base[slot];
    uint32_t entry = load_ram_half(base + OTA_P4_PART0_OFF, h.part_len[0]);
    if (!entry) {
        rom_printf("[loader] slot %c: the RAM half did not load\n", 'A' + slot);
        for (;;) { }
    }
    if (flags & OTA_P4_FLAG_TRIAL) arm_trial_watchdog();
    else disarm_trial_watchdog();
    rom_printf("[loader] slot %c, %s%s\n", 'A' + slot, h.build_id,
               (flags & OTA_P4_FLAG_TRIAL) ? " (trial)" : (flags & OTA_P4_FLAG_FELL_BACK) ? " (the trial did not confirm)" : "");
    /* What was just loaded went through the L1 data cache, and the kernel's
     * first instructions are fetched through the L1 instruction cache, which
     * `fence.i` does not clear on this core: measured, the entry point's old
     * contents executed (an illegal instruction at the first fetch) while a
     * data read of the same address returned the new ones. Write back, then
     * invalidate the instruction cache -- an invalidate only costs a refetch,
     * so it is safe under the loader's own code. */
    rom_cache_writeback_all(CACHE_MAP_L1_DCACHE);
    rom_cache_invalidate_all(CACHE_MAP_L1_ICACHE_0 | CACHE_MAP_L1_ICACHE_1);
    __asm__ __volatile__("fence.i" ::: "memory");
    ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t))(uintptr_t)entry)(
        OTA_P4_BOOT_MAGIC, base + OTA_P4_PART1_OFF, flags, (uint32_t)slot);
    for (;;) { }
}
