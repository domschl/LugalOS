/*
 * LugalOS Driver: the RP2350 identity store backend -- I7b,
 * plan/phase21_identity_and_authentication.md §3.2/§3.3.
 *
 * This is the one hook the rest of phase 21 was written around. Everything
 * above it -- the record format, the toolset, the auth wiring, grants, WLAN
 * credentials -- already worked against QEMU's virtio backend
 * (drivers/virtio_blk_id.c) and does not change for this file to exist.
 *
 * Two pieces of silicon, for two different questions:
 *
 *   - **What is this board?** OTP's CHIPID rows, a factory-programmed 64-bit
 *     identifier. Read, never written, so the UID needs no provisioning at
 *     all on this target (§3.1) -- which is a better answer than the flash
 *     chip's id phase 19 sketched, and the reason board_unique_id() finally
 *     returns true on hardware.
 *
 *   - **What has this board been told?** The reserved 4 KB sector at
 *     LUGALOS_IDENTITY_BASE, read straight from XIP and written through
 *     drivers/flash_rp2350.c. It is its own flash segment (I7a), so no UF2
 *     this build emits covers it and an OS reflash leaves it alone --
 *     measured, not assumed.
 */

#include "drivers/block.h"
#include "drivers/flash_rp2350.h"
#include "kernel/identity.h"
#include "kernel/idstore.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include <string.h>

#define FLASH_XIP_BASE 0x10000000u

/* OTP's ECC-guarded read window presents each row as a 16-bit value, indexed
 * by row number -- `(uint16_t *)OTP_DATA_BASE + row`. Taken from the SDK's own
 * usage (src/rp2_common/hardware_powman/powman.c reads LPOSC_CALIB that way),
 * not inferred from the datasheet's register tables, because the stride is
 * exactly the sort of thing that is easy to get wrong and produces plausible
 * garbage rather than a fault.
 *
 * CHIPID0..3 are rows 0x00..0x03 (otp_data.h). The SDK's own comment on them:
 * "The CHIPID0..3 rows contain a 64-bit random identifier for this chip [...]
 * for practical purposes CHIPID may be treated as unique." */
#define OTP_DATA_BASE        0x40130000u
#define OTP_DATA_CHIPID0_ROW 0x00u

bool board_unique_id(uint8_t out[8]) {
    const volatile uint16_t *otp = (const volatile uint16_t *)(uintptr_t)OTP_DATA_BASE;

    uint16_t w[4];
    uint16_t all_zero = 0, all_ones = 0xffff;
    for (unsigned i = 0; i < 4; i++) {
        w[i] = otp[OTP_DATA_CHIPID0_ROW + i];
        all_zero |= w[i];
        all_ones &= w[i];
    }

    /* All-zero or all-ones across every row is not a chip id, it is a read
     * that did not work -- an unprogrammed part, or the window not responding.
     * Returning false leaves kernel/identity.c on its derived-from-build-seed
     * floor, which is exactly the honest fallback that ladder exists for,
     * rather than every board of a batch claiming the same "unique" id. */
    if (all_zero == 0 || all_ones == 0xffff) return false;

    for (unsigned i = 0; i < 4; i++) {
        out[i * 2]     = (uint8_t)(w[i] & 0xff);
        out[i * 2 + 1] = (uint8_t)(w[i] >> 8);
    }
    return true;
}

/* --- the store, as a block device ------------------------------------- */

static int idstore_flash_read(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if ((uint64_t)lba + count > IDSTORE_BLOCKS) return -1;
    const uint8_t *src = (const uint8_t *)(uintptr_t)LUGALOS_IDENTITY_BASE;
    memcpy(buf, src + (size_t)lba * IDSTORE_BLOCK_SIZE,
           (size_t)count * IDSTORE_BLOCK_SIZE);
    return 0;
}

static int idstore_flash_write(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    (void)dev;

    /* Whole-sector writes only, and this is a real limit rather than an
     * oversight worth hiding. Flash erases 4 KB at a time, so a partial write
     * means read-modify-write, and the 4 KB staging buffer that needs would be
     * permanent .bss on a board where .bss comes straight out of the page
     * allocator's heap (phase 15's rule). Nothing asks for it:
     * idstore_writer_commit() is the only writer and always writes all
     * IDSTORE_BLOCKS at lba 0. Refused loudly rather than silently
     * mis-serviced, so if a future caller does want it, it says so here
     * instead of corrupting a record. */
    if (lba != 0 || count != IDSTORE_BLOCKS) {
        printk("[IdStore] partial write refused (lba %u, %u blocks) -- "
               "this backend erases a whole 4 KB sector, so only a full "
               "%u-block write at lba 0 is supported\n",
               lba, count, IDSTORE_BLOCKS);
        return -1;
    }

    /* The short delay gets this onto the wire before the write stops
     * scheduling for a tenth of a second: USB CDC output is pumped by a
     * task. */
    printk("[IdStore] writing the identity sector\n");
    time_delay_us(50000);

    uint32_t offs = (uint32_t)LUGALOS_IDENTITY_BASE - FLASH_XIP_BASE;
    if (flash_rp2350_write_sector(offs, buf) != 0) return -1;

    /* Read back what actually landed. The write path turns off interrupts and
     * XIP and cannot report anything while it runs, so this is the first
     * moment a failure can be noticed at all -- and a store that reports
     * success without having stored anything is precisely the failure mode
     * §7 cares about for a key. */
    if (memcmp((const void *)(uintptr_t)LUGALOS_IDENTITY_BASE, buf, IDSTORE_SIZE_BYTES) != 0) {
        printk("[IdStore] write did not verify -- the sector does not read back "
               "as written\n");
        return -1;
    }

    /* No reboot after the write, since 2026-10-04 (phase 40 item 12).
     *
     * There used to be one, for two reasons. flash_rp2350_write_sector()
     * left XIP in the bootrom's generic 03h read mode until a reset -- fixed
     * by 38.1 (plan/phase38_psram.md), which puts the QSPI interface back as
     * it found it. And the USB console did not survive the ~100 ms with
     * interrupts off: the first `identity provision` on hardware (2026-09-01)
     * wrote its record and went silent until a physical BOOTSEL.
     *
     * The second was measured again on the RP2350-LCD-7 at 7844345 with the
     * reboot taken out: two `identity name` writes (114 ms each), the
     * console answering straight after each, the record read back as
     * stored, and the 9P link on the second CDC port, the QSPI windows and
     * PSRAM's pattern tests all passing after them. Whatever starved the
     * controller in September -- the CDC task and the USB stack have both
     * been reworked since -- does not any more. */
    return 0;
}

static block_dev_t g_idstore_dev = {
    .name = "idstore0",
    .block_size = IDSTORE_BLOCK_SIZE,
    .num_blocks = IDSTORE_BLOCKS,
    .read_blocks = idstore_flash_read,
    .write_blocks = idstore_flash_write,
};

/* Strong definition of kernel/identity.c's weak hook. No probe and no
 * registration step: unlike a virtio device there is nothing to discover --
 * the sector is at a fixed address that the linker has already ASSERTed lies
 * inside flash and clear of every other segment. */
block_dev_t *identity_store_device(void) {
    return &g_idstore_dev;
}
