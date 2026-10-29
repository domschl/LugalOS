/*
 * LugalOS driver: the identity store on the K210 -- 65.6, the record
 * itself is kernel/idstore.h's, unchanged.
 *
 * One 4 KB sector at the very top of the 16 MB NOR
 * (LUGALOS_K210_IDENTITY_BASE, cmake/flash_layout_k210.cmake), offered
 * to kernel/identity.c as the block device it keeps the record on --
 * the same seam drivers/idstore_esp32c6.c fills. The sector is written
 * by the device and never by a provisioning tool: k210flash.py's map
 * stops the moment identity begins, and a reflash of loader, slots and
 * /flash0 leaves a provisioned device provisioned across power cycles.
 *
 * Every byte travels through drivers/spi_nor_k210.c, which owns the
 * SPI3 controller, its ylock, and the writable-floor guard (identity
 * sits far above it, and the guard is what keeps anything that ever
 * grows downward from eating the record).
 *
 * The K210 has no readable silicon unique id, so this sector is the
 * whole device scope: the UID field provisioned here is what
 * node_uid() reports ("record", not "silicon" -- kernel/identity.c
 * already distinguishes and prints exactly that).
 */

#include "drivers/block.h"
#include "drivers/spi_nor_k210.h"
#include "kernel/identity.h"
#include "kernel/idstore.h"
#include "kernel/printk.h"
#include <stdint.h>
#include <string.h>

#if !defined(LUGALOS_K210_IDENTITY_BASE) || !defined(LUGALOS_K210_FLASH_SIZE)
#error "the K210 flash map (cmake/flash_layout_k210.cmake) defines where the identity sector lives"
#endif

#define SECTOR_BYTES 4096u
#define CHUNK_BYTES  256u

/* Boot order forces this: node_identity_init() runs at the top of
 * kernel_main(), long before the flashdisk task first calls
 * spi_nor_k210_init(). Calling init here (idempotent, and cheap once a
 * part has answered) is what lets the record be read on the very first
 * boot instead of reporting "unprovisioned" for a sector that holds a
 * perfectly good UID. */
static int nor_ready(void) {
    if (spi_nor_k210_init() != 0) return -1;
    return 0;
}

static int idflash_read(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (nor_ready() != 0) return -1;
    if ((uint64_t)lba + count > IDSTORE_BLOCKS) return -1;
    return spi_nor_k210_read((uint32_t)LUGALOS_K210_IDENTITY_BASE + lba * IDSTORE_BLOCK_SIZE,
                             buf, count * IDSTORE_BLOCK_SIZE);
}

/* Whole-sector writes only, as on every board: the NOR erases 4 KB at a
 * time, the only writer (idstore_writer_commit) always writes the whole
 * record, and read-modify-write here would need a sector of scratch to
 * merge into somebody else's bytes. */
static int idflash_write(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (nor_ready() != 0) return -1;
    if (lba != 0 || count != IDSTORE_BLOCKS) {
        printk("[IdStore] partial write refused (lba %u, %u blocks): only the whole %u-block sector\n",
               (unsigned)lba, (unsigned)count, (unsigned)IDSTORE_BLOCKS);
        return -1;
    }
    const uint32_t base = (uint32_t)LUGALOS_K210_IDENTITY_BASE;
    printk("[IdStore] writing the identity sector\n");
    if (spi_nor_k210_erase_sector(base) != 0) return -1;
    if (spi_nor_k210_write(base, buf, SECTOR_BYTES) != 0) return -1;
    /* Read back what landed -- a store that reports success without
     * having stored anything is the failure that matters most for a
     * key. (The NOR read path is the same one flashtest proved
     * byte-exact across 665 KB.) */
    const uint8_t *in = buf;
    for (uint32_t off = 0; off < SECTOR_BYTES; off += CHUNK_BYTES) {
        uint8_t tmp[CHUNK_BYTES];
        if (spi_nor_k210_read(base + off, tmp, CHUNK_BYTES) != 0 ||
            memcmp(tmp, in + off, CHUNK_BYTES) != 0) {
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
