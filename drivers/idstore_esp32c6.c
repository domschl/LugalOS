/*
 * LugalOS driver: the identity store on the ESP32-C6 -- 45.8,
 * ; the record itself is 
 * authentication.md's, unchanged.
 *
 * One 4 KB flash sector at LUGALOS_C6_IDENTITY_BASE (cmake/flash_layout_
 * esp32c6.cmake), offered to kernel/identity.c as the block device it keeps
 * the record on -- the same seam drivers/idstore_rp2350.c fills. The sector
 * is written by the device and never by a flashing tool: it is not in
 * flash.manifest.
 *
 * Every transfer goes through drivers/flash_esp32c6.c, which owns the ROM's
 * SPI routines and the guard around them (the kernel executes from the flash
 * it writes), and which refuses to write anywhere but the regions the kernel
 * owns.
 */

#include "drivers/block.h"
#include "drivers/flash_esp32c6.h"
#include "kernel/identity.h"
#include "kernel/idstore.h"
#include "kernel/printk.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if !defined(LUGALOS_C6_IDENTITY_BASE) || !defined(LUGALOS_C6_FLASH_SIZE)
#error "the C6 flash map (cmake/flash_layout_esp32c6.cmake) defines where the identity sector lives"
#endif

#define SECTOR_BYTES 4096u
#define CHUNK_BYTES  256u

/* --- the store, as a block device ------------------------------------- */

static int idflash_read(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if ((uint64_t)lba + count > IDSTORE_BLOCKS) return -1;
    return c6flash_read((uint32_t)LUGALOS_C6_IDENTITY_BASE + lba * IDSTORE_BLOCK_SIZE, buf, count * IDSTORE_BLOCK_SIZE);
}

/* Whole-sector writes only, as on the RP2350 and for its reason: flash erases
 * 4 KB at a time, a partial write would need a 4 KB read-modify-write buffer,
 * and the only writer (idstore_writer_commit) always writes the whole record. */
static int idflash_write(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (lba != 0 || count != IDSTORE_BLOCKS) {
        printk("[IdStore] partial write refused (lba %u, %u blocks): only the whole %u-block sector\n",
               (unsigned)lba, (unsigned)count, (unsigned)IDSTORE_BLOCKS);
        return -1;
    }
    const uint32_t base = (uint32_t)LUGALOS_C6_IDENTITY_BASE;
    printk("[IdStore] writing the identity sector\n");
    if (c6flash_erase_sector(base) != 0 || c6flash_write(base, buf, SECTOR_BYTES) != 0) return -1;
    /* Read back what landed: a store that reports success without having stored
     * anything is the failure that matters most for a key. */
    const uint8_t *in = buf;
    for (uint32_t off = 0; off < SECTOR_BYTES; off += CHUNK_BYTES) {
        uint8_t tmp[CHUNK_BYTES];
        if (c6flash_read(base + off, tmp, CHUNK_BYTES) != 0 || memcmp(tmp, in + off, CHUNK_BYTES) != 0) {
            printk("[IdStore] write did not verify at +0x%x\n", (unsigned)off);
            return -1;
        }
    }
    return 0;
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
    return &g_idstore_dev;
}
