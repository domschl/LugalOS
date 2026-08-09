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
#define ROM_FUNC_RESET_USB_BOOT ROM_TABLE_CODE('U', 'B')

typedef void *(*rom_table_lookup_fn)(uint32_t code, uint32_t mask);
typedef void (*rom_reset_usb_boot_fn)(uint32_t usb_activity_gpio_pin_mask,
                                      uint32_t disable_interface_mask);

/* Converting the ROM's data pointer to a function pointer is exactly what
 * this interface requires, and ISO C has no conforming way to express it --
 * the Pico SDK suppresses the same diagnostic at the same place. Scoped to
 * this file so it cannot mask the warning anywhere it would be meaningful. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

static void *rom_func_lookup(uint32_t code) {
    rom_table_lookup_fn lookup =
        (rom_table_lookup_fn)(uintptr_t)*(volatile uint16_t *)BOOTROM_TABLE_LOOKUP_ENTRY_OFFSET;
    if (!lookup) return 0;
    return lookup(code, RT_FLAG_FUNC_RISCV);
}

bool rp2350_reboot_to_bootsel(void) {
    rom_reset_usb_boot_fn reset_usb_boot =
        (rom_reset_usb_boot_fn)rom_func_lookup(ROM_FUNC_RESET_USB_BOOT);
    if (!reset_usb_boot) return false;

    /* (0, 0): no USB activity LED, and no interfaces disabled -- expose both
     * mass storage and PICOBOOT, which is what a plain BOOTSEL button press
     * gives and what host tooling expects. Does not return. */
    reset_usb_boot(0, 0);
    return true; /* unreachable in practice */
}

#pragma GCC diagnostic pop

#else

bool rp2350_reboot_to_bootsel(void) {
    return false; /* no bootrom to call on QEMU targets */
}

#endif
