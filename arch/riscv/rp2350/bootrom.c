#include "arch/rp2350_bootrom.h"

#if defined(CONFIG_BOARD_RP2350)

/* RP2350 bootrom access: rebooting into BOOTSEL (USB mass-storage) mode so a
 * host can flash a new UF2 without anyone physically holding the BOOTSEL
 * button.
 *
 * Constants and the lookup sequence are taken from the Pico SDK
 * (src/rp2_common/boot_bootrom_headers/include/boot/bootrom_constants.h and
 * src/rp2_common/pico_bootrom/include/pico/bootrom.h), not guessed. The
 * RISC-V path differs from the Arm one in a way that matters: on RISC-V the
 * table entry *is* executable code (a jump), so the lookup function pointer
 * comes from BOOTROM_TABLE_LOOKUP_ENTRY_OFFSET and the lookup is called with
 * RT_FLAG_FUNC_RISCV. Using the Arm offset here would call into the wrong
 * thing entirely.
 *
 * The SDK also applies a +32K offset when the ROM is 64K, but that is guarded
 * by RASPBERRYPI_AMETHYST_FPGA -- it is an FPGA-only case, and real RP2350
 * silicon uses no adjustment.
 */

#define BOOTROM_ENTRY_OFFSET              0x7dfc
#define BOOTROM_WELL_KNOWN_PTR_SIZE       2
#define BOOTROM_TABLE_LOOKUP_ENTRY_OFFSET (BOOTROM_ENTRY_OFFSET - BOOTROM_WELL_KNOWN_PTR_SIZE)

#define RT_FLAG_FUNC_RISCV      0x0001
#define ROM_TABLE_CODE(c1, c2)  ((c1) | ((c2) << 8))

/* ROM_FUNC_RESET_USB_BOOT ('U','B') is **RP2040 only** -- it sits inside
 * `#if PICO_RP2040` in the SDK's bootrom_constants.h, and looking it up on
 * RP2350 returns NULL. Verified the hard way on real silicon: the first
 * version of this file used it and the board reported
 * "bootrom reset_usb_boot not found" over the debug UART rather than
 * rebooting. RP2350 exposes the unified reboot() API instead, which is what
 * the SDK's own rom_reset_usb_boot() falls back to when the RP2040 symbol is
 * absent (pico_bootrom/bootrom.c). */
#define ROM_FUNC_REBOOT         ROM_TABLE_CODE('R', 'B')

#define REBOOT2_FLAG_REBOOT_TYPE_NORMAL    0x0000
#define REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL   0x0002
#define REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE 0x0004
#define REBOOT2_FLAG_NO_RETURN_ON_SUCCESS  0x0100

typedef void *(*rom_table_lookup_fn)(uint32_t code, uint32_t mask);
typedef int (*rom_reboot_fn)(uint32_t flags, uint32_t delay_ms,
                             uint32_t p0, uint32_t p1);

/* Converting the ROM's data pointer to a function pointer is exactly what
 * this interface requires, and ISO C has no conforming way to express it --
 * the Pico SDK suppresses the same diagnostic at the same place. Scoped to
 * this file so it cannot mask the warning anywhere it would be meaningful. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

void *rp2350_rom_func_lookup(uint32_t code) {
    rom_table_lookup_fn lookup =
        (rom_table_lookup_fn)(uintptr_t)*(volatile uint16_t *)BOOTROM_TABLE_LOOKUP_ENTRY_OFFSET;
    if (!lookup) return 0;
    return lookup(code, RT_FLAG_FUNC_RISCV);
}

bool rp2350_reboot_to_bootsel(void) {
    rom_reboot_fn reboot = (rom_reboot_fn)rp2350_rom_func_lookup(ROM_FUNC_REBOOT);
    if (!reboot) return false;

    /* Arguments mirror the SDK's own rom_reset_usb_boot() fallback:
     *   flags   = BOOTSEL reboot type, and do not return if it succeeds
     *   delay   = 10 ms, giving the USB host a moment before the detach
     *   p0      = BOOTSEL flags; 0 disables nothing, so both mass storage and
     *             PICOBOOT are exposed, exactly as a physical BOOTSEL press
     *   p1      = activity GPIO; unused, no LED is requested
     * Does not return on success. */
    reboot(REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS,
           10, 0, 0);
    return false; /* reached only if the bootrom refused the request */
}

/* Arms a reboot `ms` from now and RETURNS -- the same bootrom entry point as
 * rp2350_reboot() below, minus NO_RETURN_ON_SUCCESS.
 *
 * A deadman for experiments that can hang the machine (X7's core-1
 * bring-up). Three attempts at that milestone each left a board whose USB
 * console never came back, and each one cost a physical BOOTSEL press and
 * took the evidence with it. A scheduled reboot converts that into a board
 * that reappears on its own -- and, because a bootrom reboot is not a
 * power-on reset, SRAM survives it, so linker/rp2350.ld's .smpmark still
 * holds the last step the wedged core reached. The failure reports itself
 * after the fact instead of being silent.
 *
 * rp2350_reboot_cancel() below withdraws it if the experiment survives. */
bool rp2350_reboot_after_ms(uint32_t ms) {
    rom_reboot_fn reboot = (rom_reboot_fn)rp2350_rom_func_lookup(ROM_FUNC_REBOOT);
    if (!reboot) return false;
    return reboot(REBOOT2_FLAG_REBOOT_TYPE_NORMAL, ms, 0, 0) >= 0;
}

/* Withdraws a pending rp2350_reboot_after_ms() by disabling the watchdog the
 * bootrom armed it with. WATCHDOG_CTRL bit 30 is ENABLE (RP2350 datasheet
 * §12.9.2); the write is a plain clear, and the 0xd0000000-style password
 * registers elsewhere in the chip do not apply to this one. */
#define WATCHDOG_CTRL   (*(volatile uint32_t *)(0x400d8000UL + 0x00))
#define WATCHDOG_ENABLE (1u << 30)

void rp2350_reboot_cancel(void) {
    WATCHDOG_CTRL &= ~WATCHDOG_ENABLE;
}

/* The QMI's address translation (datasheet 12.14.4): ATRANSn covers runtime
 * 0x10000000 + n * 4 MB; BASE (bits 11:0) is the storage address of its first
 * byte and SIZE (26:16) its length, both in 4 KB units. The bootrom has set
 * ATRANS0 for the booted partition and left the rest empty. */
#define QMI_ATRANS(n)  (*(volatile uint32_t *)(uintptr_t)(0x400d0000UL + 0x34UL + 4UL * (n)))

static uint32_t atrans(uint32_t phys, uint32_t size) {
    return ((size >> 12) << 16) | (phys >> 12);
}

void rp2350_map_data_windows(void) {
    QMI_ATRANS(1) = atrans(LUGALOS_FLASHFS_PHYS, LUGALOS_FLASHFS_SIZE);
    QMI_ATRANS(2) = atrans(LUGALOS_IDENTITY_PHYS, LUGALOS_IDENTITY_SIZE);
}

#define ROM_FUNC_GET_SYS_INFO   ROM_TABLE_CODE('G', 'S')
#define SYS_INFO_BOOT_INFO      0x0040
#define BOOT_TBYB_BUY_PENDING   0x1u

typedef int (*rom_get_sys_info_fn)(uint32_t *out, uint32_t words, uint32_t flags);

/* result[0] echoes the flag; then boot_word (diagnostic partition, boot type,
 * partition, TBYB/update flags -- one byte each, low to high), a diagnostic
 * word and the two reboot parameters. */
static bool boot_info(uint32_t *boot_word) {
    rom_get_sys_info_fn get = (rom_get_sys_info_fn)rp2350_rom_func_lookup(ROM_FUNC_GET_SYS_INFO);
    uint32_t r[5];
    if (!get || get(r, 5, SYS_INFO_BOOT_INFO) != 5 || r[0] != SYS_INFO_BOOT_INFO) return false;
    *boot_word = r[1];
    return true;
}

int rp2350_boot_slot(void) {
    uint32_t w;
    if (!boot_info(&w)) return -1;
    int8_t partition = (int8_t)((w >> 16) & 0xffu);
    return partition == 0 || partition == 1 ? partition : -1;
}

bool rp2350_boot_buy_pending(void) {
    uint32_t w;
    return boot_info(&w) && ((w >> 24) & BOOT_TBYB_BUY_PENDING);
}

bool rp2350_reboot_flash_update(uint32_t phys) {
    rom_reboot_fn reboot = (rom_reboot_fn)rp2350_rom_func_lookup(ROM_FUNC_REBOOT);
    if (!reboot) return false;
    reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS,
           10, 0x10000000u + phys, 0);
    return false;
}

bool rp2350_reboot(void) {
    rom_reboot_fn reboot = (rom_reboot_fn)rp2350_rom_func_lookup(ROM_FUNC_REBOOT);
    if (!reboot) return false;

    /* The same bootrom entry point as the BOOTSEL path above, with the normal
     * reboot type: come back up running the flashed image rather than as a
     * mass-storage device. Reusing the proven lookup is the point -- the only
     * thing that differs between "reflash me" and "start me again" is this
     * flag.
     *
     * The 10 ms delay is not cosmetic: it gives the USB host a moment to see
     * the detach coming, which is what makes the port re-enumerate cleanly
     * rather than the host deciding the device fell off the bus. */
    reboot(REBOOT2_FLAG_REBOOT_TYPE_NORMAL | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS,
           10, 0, 0);
    return false; /* reached only if the bootrom refused the request */
}

#pragma GCC diagnostic pop

#else

bool rp2350_reboot_to_bootsel(void) {
    return false; /* no bootrom to call on QEMU targets */
}

bool rp2350_reboot_after_ms(uint32_t ms) { (void)ms; return false; }
void rp2350_reboot_cancel(void) { }

bool rp2350_reboot(void) {
    return false; /* likewise: nothing to ask for a reset here */
}

void rp2350_map_data_windows(void) { }
int rp2350_boot_slot(void) { return -1; }
bool rp2350_boot_buy_pending(void) { return false; }
bool rp2350_reboot_flash_update(uint32_t phys) { (void)phys; return false; }

void *rp2350_rom_func_lookup(uint32_t code) {
    (void)code;
    return 0; /* no bootrom on the QEMU targets */
}

#endif
