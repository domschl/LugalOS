#include "drivers/block.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include <string.h>

/* The RAM disk's storage, taken from the page allocator rather than reserved
 * in .bss (C5, plan/phase6_memory_and_processes.md).
 *
 * It used to be a fixed array -- 64 KB on RP2350, 512 KB elsewhere -- which
 * made it the single largest static object in the image and, on a board with
 * 512 KB of SRAM, an eighth of the machine spent whether or not anything ever
 * mounted /ram0.
 *
 * Be precise about what this saves, because it is easy to overstate: while the
 * disk *is* mounted the memory is still gone, just from the heap instead of
 * from .bss. Two things are genuinely bought. The size becomes a decision made
 * in init.lisp rather than a constant compiled into a driver, so a board that
 * wants 16 KB of scratch space says so. And `_kernel_end` drops by the whole
 * array, which raises the largest NAPOT region the heap can place -- the
 * constraint C4 runs into, where a k-byte region needs a heap of at least k
 * bytes (see §2.1). A 64 KB array in .bss cannot be borrowed for that; 64 KB
 * of heap can.
 */

#define RAMDISK_BLOCK_SIZE 512

/* An upper bound on what a mount may ask for, not a reservation. Generous
 * relative to the heap on each target, so the allocator's refusal is what
 * limits an oversized request -- a cap that binds first would report the wrong
 * reason. */
#if defined(CONFIG_BOARD_RP2350)
#define RAMDISK_MAX_BLOCKS 128   /* 64 KB */
#else
#define RAMDISK_MAX_BLOCKS 1024  /* 512 KB */
#endif

static uint8_t *g_storage;      /* NULL until ramdisk_init() succeeds */
static uint32_t g_pages;        /* what to hand back on release */

static block_dev_t ramdisk_device;

static int ramdisk_read(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (!g_storage || !buf) return -1;
    if (lba + count > ramdisk_device.num_blocks) return -1;
    memcpy(buf, &g_storage[lba * RAMDISK_BLOCK_SIZE], count * RAMDISK_BLOCK_SIZE);
    return 0;
}

static int ramdisk_write(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (!g_storage || !buf) return -1;
    if (lba + count > ramdisk_device.num_blocks) return -1;
    memcpy(&g_storage[lba * RAMDISK_BLOCK_SIZE], buf, count * RAMDISK_BLOCK_SIZE);
    return 0;
}

static block_dev_t ramdisk_device = {
    .name = "ramdisk0",
    .block_size = RAMDISK_BLOCK_SIZE,
    .num_blocks = 0,            /* nothing until it is allocated */
    .read_blocks = ramdisk_read,
    .write_blocks = ramdisk_write
};

int ramdisk_init(uint32_t blocks) {
    if (blocks == 0 || blocks > RAMDISK_MAX_BLOCKS) return -1;

    uint32_t bytes = blocks * RAMDISK_BLOCK_SIZE;
    uint32_t pages = (bytes + (uint32_t)PAGE_SIZE - 1) / (uint32_t)PAGE_SIZE;

    /* Already the right size: keep the contents. Remounting the same disk
     * should not silently discard what is on it. */
    if (g_storage && pages == g_pages) {
        ramdisk_device.num_blocks = blocks;
        return 0;
    }

    /* Allocated before the old block is released, so a failed resize leaves
     * the existing disk intact rather than destroying it and then reporting
     * an error. */
    uint8_t *fresh = (uint8_t *)palloc_pages(pages);
    if (!fresh) {
        printk("[RAMDisk] No memory for a %u KB RAM disk (%u pages)\n",
               bytes / 1024, pages);
        return -1;
    }

    if (g_storage) palloc_free(g_storage, g_pages);
    g_storage = fresh;
    g_pages = pages;
    ramdisk_device.num_blocks = blocks;
    return 0;
}

void ramdisk_release(void) {
    if (!g_storage) return;
    palloc_free(g_storage, g_pages);
    g_storage = NULL;
    g_pages = 0;
    ramdisk_device.num_blocks = 0;
}

block_dev_t *ramdisk_get_device(void) {
    return &ramdisk_device;
}

uint32_t ramdisk_max_blocks(void) {
    return RAMDISK_MAX_BLOCKS;
}
