/*
 * LugalOS Driver: RP2350 internal flash writes -- I7b,
 * plan/phase21_identity_and_authentication.md §3.2.
 *
 * One operation, deliberately: erase a 4 KB sector and program 4 KB into it.
 * That is the whole shape of what the identity store needs
 * (IDSTORE_SIZE_BYTES is exactly one sector, and idstore_writer_commit()
 * writes all of it in a single call), and a general-purpose flash API would
 * be more surface than anything here uses.
 *
 * ## Why any of this is delicate
 *
 * The image executes from flash over XIP. Writing flash means turning XIP
 * off, which means that for the duration of the operation *every* instruction
 * fetch from 0x10000000-and-up faults or hangs. So:
 *
 *   - the routine doing it must already be in RAM (`.ramfunc`, copied at boot
 *     by _reset_handler's table walk -- see linker/rp2350.ld),
 *   - it must not call anything that is not (no printk, no memcpy from libc,
 *     no bootrom lookups -- those are resolved by the caller, in flash, and
 *     passed in),
 *   - and interrupts must be off, because this kernel preempts: the 100 Hz
 *     ticker alone would vector to a handler in flash.
 *
 * §10 of the plan names the failure mode for getting this wrong, and it is
 * worth repeating here: the board hangs, it does not report an error. There
 * is nothing left running to report with.
 *
 * ## What is taken from the SDK rather than from prose
 *
 * The call sequence, the ROM table codes and the erase parameters all come
 * from pico-sdk's src/rp2_common/hardware_flash/flash.c and
 * boot_bootrom_headers/include/boot/bootrom_constants.h. In particular the
 * RP2350 (non-RP2040) path re-enables XIP with ROM_FUNC_FLASH_ENTER_CMD_XIP
 * rather than by re-running a boot2 copy, and flash_flush_cache is required
 * for more than the cache -- it also releases the CSn IO force.
 */

#include "drivers/flash_rp2350.h"
#include "arch/rp2350_bootrom.h"
#include "kernel/printk.h"

#if defined(CONFIG_BOARD_RP2350)

#define ROM_TABLE_CODE(c1, c2)  ((uint32_t)((c1) | ((c2) << 8)))

#define ROM_FUNC_CONNECT_INTERNAL_FLASH ROM_TABLE_CODE('I', 'F')
#define ROM_FUNC_FLASH_EXIT_XIP         ROM_TABLE_CODE('E', 'X')
#define ROM_FUNC_FLASH_RANGE_ERASE      ROM_TABLE_CODE('R', 'E')
#define ROM_FUNC_FLASH_RANGE_PROGRAM    ROM_TABLE_CODE('R', 'P')
#define ROM_FUNC_FLASH_FLUSH_CACHE      ROM_TABLE_CODE('F', 'C')
#define ROM_FUNC_FLASH_ENTER_CMD_XIP    ROM_TABLE_CODE('C', 'X')

/* pico-sdk hardware_flash/flash.h and flash.c. The block size and block erase
 * command are what the ROM may use to erase large ranges more efficiently;
 * for a single 4 KB range it falls back to 0x20 sector erases regardless, but
 * passing the SDK's own values keeps this call identical to the one the SDK
 * makes rather than a variant nobody has run. */
#define FLASH_SECTOR_SIZE     4096u
#define FLASH_BLOCK_SIZE      65536u
#define FLASH_BLOCK_ERASE_CMD 0xd8u

/* Converting the ROM's data pointer to a function pointer is what this
 * interface requires and ISO C has no conforming way to express it -- the
 * same suppression, for the same reason, as arch/riscv/rp2350/bootrom.c.
 * Scoped to this file so it cannot mask the warning where it would mean
 * something. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

typedef void (*rom_void_fn)(void);
typedef void (*rom_flash_range_erase_fn)(uint32_t addr, uint32_t count,
                                         uint32_t block_size, uint8_t block_cmd);
typedef void (*rom_flash_range_program_fn)(uint32_t addr, const uint8_t *data,
                                           uint32_t count);

/* Resolved in flash-resident code, before XIP goes away, and handed to the
 * RAM-resident routine as data. */
typedef struct {
    rom_void_fn                connect_internal_flash;
    rom_void_fn                flash_exit_xip;
    rom_flash_range_erase_fn   flash_range_erase;
    rom_flash_range_program_fn flash_range_program;
    rom_void_fn                flash_flush_cache;
    rom_void_fn                flash_enter_cmd_xip;
} flash_rom_fns_t;

/* The critical section itself. Everything it touches is either in RAM (its
 * own code, the caller's buffer, the resolved pointers) or in the bootrom;
 * nothing is in flash. `noinline` so it cannot be inlined back into a
 * flash-resident caller, which would silently undo the whole point of the
 * section attribute. */
__attribute__((section(".ramfunc"), noinline))
static void flash_write_sector_ram(const flash_rom_fns_t *f,
                                   uint32_t flash_offs,
                                   const uint8_t *data) {
    f->connect_internal_flash();
    f->flash_exit_xip();
    f->flash_range_erase(flash_offs, FLASH_SECTOR_SIZE,
                         FLASH_BLOCK_SIZE, FLASH_BLOCK_ERASE_CMD);
    f->flash_range_program(flash_offs, data, FLASH_SECTOR_SIZE);
    /* Not only a cache flush: this is also what releases the CSn IO force
     * that flash_exit_xip() applied. Skipping it leaves the bus held. */
    f->flash_flush_cache();
    f->flash_enter_cmd_xip();
}

int flash_rp2350_write_sector(uint32_t flash_offs, const void *data) {
    if (flash_offs % FLASH_SECTOR_SIZE != 0) {
        printk("[Flash] refusing unaligned sector write at offset 0x%x\n", flash_offs);
        return -1;
    }
    if (!data) return -1;

    flash_rom_fns_t f;
    f.connect_internal_flash = (rom_void_fn)rp2350_rom_func_lookup(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    f.flash_exit_xip         = (rom_void_fn)rp2350_rom_func_lookup(ROM_FUNC_FLASH_EXIT_XIP);
    f.flash_range_erase      = (rom_flash_range_erase_fn)rp2350_rom_func_lookup(ROM_FUNC_FLASH_RANGE_ERASE);
    f.flash_range_program    = (rom_flash_range_program_fn)rp2350_rom_func_lookup(ROM_FUNC_FLASH_RANGE_PROGRAM);
    f.flash_flush_cache      = (rom_void_fn)rp2350_rom_func_lookup(ROM_FUNC_FLASH_FLUSH_CACHE);
    f.flash_enter_cmd_xip    = (rom_void_fn)rp2350_rom_func_lookup(ROM_FUNC_FLASH_ENTER_CMD_XIP);

    /* Checked as a group and *before* anything is disturbed. A missing ROM
     * function discovered halfway through would leave XIP off with no way to
     * fetch the instruction that would report it. */
    if (!f.connect_internal_flash || !f.flash_exit_xip || !f.flash_range_erase ||
        !f.flash_range_program || !f.flash_flush_cache || !f.flash_enter_cmd_xip) {
        printk("[Flash] bootrom flash functions not found -- not writing\n");
        return -1;
    }

    /* Interrupts off across the whole operation, restored exactly as found.
     * csrrci returns the previous mstatus, so a caller that already had them
     * disabled stays that way. */
    uintptr_t saved;
    __asm__ __volatile__("csrrci %0, mstatus, 0x8" : "=r"(saved));

    flash_write_sector_ram(&f, flash_offs, (const uint8_t *)data);

    if (saved & 0x8) {
        __asm__ __volatile__("csrsi mstatus, 0x8");
    }
    return 0;
}

#pragma GCC diagnostic pop

#else

int flash_rp2350_write_sector(uint32_t flash_offs, const void *data) {
    (void)flash_offs; (void)data;
    return -1; /* no internal flash to write on the QEMU targets */
}

#endif
