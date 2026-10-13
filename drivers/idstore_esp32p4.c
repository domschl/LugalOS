/*
 * LugalOS driver: the identity store on the ESP32-P4. The record itself is
 * kernel/idstore.c's, unchanged; this is only where it lives.
 *
 * One 4 KB flash sector at LUGALOS_P4_IDENTITY_BASE (cmake/flash_layout_
 * esp32p4.cmake), offered to kernel/identity.c as the block device it keeps
 * the record on -- the seam drivers/idstore_rp2350.c and idstore_esp32c6.c
 * fill on their chips. The sector is written by the device and never by a
 * flashing tool.
 *
 * Every transfer goes through drivers/flash_esp32p4.c, which owns the ROM's
 * SPI flash routines, the XIP cache around them, and the write floor: this
 * file adds only that it writes its own sector whole, and reads it back.
 */

#include "drivers/block.h"
#include "drivers/flash_esp32p4.h"
#include "kernel/identity.h"
#include "kernel/idstore.h"
#include "kernel/lock.h"
#include "kernel/printk.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if !defined(LUGALOS_P4_IDENTITY_BASE)
#error "the P4 flash map (cmake/flash_layout_esp32p4.cmake) defines where the identity sector lives"
#endif

_Static_assert(IDSTORE_SIZE_BYTES == FLASH_P4_SECTOR_SIZE, "the record is one flash sector");

static ylock_t g_lock;

static int idflash_read(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if ((uint64_t)lba + count > IDSTORE_BLOCKS) return -1;
    if (!flash_p4_init()) return -1;
    ylock_acquire(&g_lock);
    int rc = flash_p4_read((uint32_t)LUGALOS_P4_IDENTITY_BASE + lba * IDSTORE_BLOCK_SIZE,
                           buf, count * IDSTORE_BLOCK_SIZE);
    ylock_release(&g_lock);
    return rc;
}

/* Whole-sector writes only: an erase takes the whole sector, so a partial
 * write would have to read, merge and rewrite it -- the caller (identity.c)
 * always writes the whole record anyway. */
static int idflash_write(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (lba != 0 || count != IDSTORE_BLOCKS) {
        printk("[IdStore] partial write refused (lba %u, %u blocks): only the whole %u-block sector\n",
               (unsigned)lba, (unsigned)count, (unsigned)IDSTORE_BLOCKS);
        return -1;
    }
    if (!flash_p4_init()) return -1;
    const uint32_t base = (uint32_t)LUGALOS_P4_IDENTITY_BASE;
    printk("[IdStore] writing the identity sector\n");
    ylock_acquire(&g_lock);
    int rc = flash_p4_erase_sector(base);
    if (rc == 0) rc = flash_p4_write(base, buf, IDSTORE_SIZE_BYTES);
    /* Read back what landed: a store that reports success without having
     * stored anything is the failure that matters most for a key. */
    static uint8_t check[IDSTORE_SIZE_BYTES];
    if (rc == 0 && (flash_p4_read(base, check, sizeof check) != 0 || memcmp(check, buf, sizeof check) != 0)) {
        printk("[IdStore] write did not verify\n");
        rc = -1;
    }
    memset(check, 0, sizeof check);
    ylock_release(&g_lock);
    return rc;
}

static block_dev_t g_idstore_dev = {
    .name = "idstore0",
    .block_size = IDSTORE_BLOCK_SIZE,
    .num_blocks = IDSTORE_BLOCKS,
    .read_blocks = idflash_read,
    .write_blocks = idflash_write,
};

/* Strong definition of kernel/identity.c's weak hook. */
block_dev_t *identity_store_device(void) {
    static bool inited;
    if (!inited) { ylock_init(&g_lock); inited = true; }
    return &g_idstore_dev;
}
