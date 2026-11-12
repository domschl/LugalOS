/*
 * LugalOS driver: identity store on NVMe SSD (StarFive VisionFive 2).
 *
 * Backing storage:
 * 4096 bytes (8 blocks of 512 bytes) on the M.2 NVMe SSD.
 *
 * Location Strategy:
 * 1. If MBR Partition 2 is defined (type 0x4C or non-zero start_lba),
 *    use the start LBA of Partition 2.
 * 2. Fallback: If Partition 1 starts at LBA >= 2048 (standard 1MB offset),
 *    use LBA (part1_start - IDSTORE_BLOCKS) (sectors 2040..2047 in the MBR gap).
 * 3. Fallback: Use sectors 2040..2047.
 *
 * Strong implementation of kernel/identity.c's weak identity_store_device().
 */

#include "drivers/block.h"
#include "drivers/nvme.h"
#include "kernel/identity.h"
#include "kernel/idstore.h"
#include "kernel/lock.h"
#include "kernel/printk.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

_Static_assert(IDSTORE_SIZE_BYTES == 4096, "identity store size is 4096 bytes");

static ylock_t g_id_lock;

/* guarded by: g_id_lock */
static uint64_t g_id_lba = 0;
/* guarded by: g_id_lock */
static bool g_id_lba_resolved = false;
/* guarded by: g_id_lock */
static bool g_id_available = false;

/* guarded by: g_id_lock */
static uint8_t s_check_buf[IDSTORE_SIZE_BYTES];

static void idstore_nvme_resolve_lba(void) {
    if (g_id_lba_resolved) return;
    block_dev_t *dev = nvme_get_device();
    if (!dev || !dev->read_blocks) {
        return;
    }

    uint8_t sector[512];
    if (dev->read_blocks(dev, sector, 0, 1) != 0) {
        printk("[IdStore] NVMe: failed to read MBR sector 0\n");
        return;
    }

    /* Check MBR signature (0x55, 0xAA) */
    if (sector[510] == 0x55 && sector[511] == 0xAA) {
        /* Check Partition 2 (offset 0x1CE) */
        const uint8_t *part2 = &sector[0x1CE];
        uint8_t p2_type = part2[4];
        uint32_t p2_start = (uint32_t)part2[8] | ((uint32_t)part2[9] << 8) |
                            ((uint32_t)part2[10] << 16) | ((uint32_t)part2[11] << 24);

        if (p2_type == 0x4C || (p2_type != 0 && p2_start > 0 && p2_start < 0xFFFFFFF0u)) {
            g_id_lba = p2_start;
            g_id_available = true;
            g_id_lba_resolved = true;
            printk("[IdStore] NVMe Identity Store located at Partition 2 LBA %llu (Type 0x%02X)\n",
                   (unsigned long long)g_id_lba, p2_type);
            return;
        }

        /* Check Partition 1 (offset 0x1BE) */
        const uint8_t *part1 = &sector[0x1BE];
        uint32_t p1_start = (uint32_t)part1[8] | ((uint32_t)part1[9] << 8) |
                            ((uint32_t)part1[10] << 16) | ((uint32_t)part1[11] << 24);

        if (p1_start >= 2048) {
            /* Standard 1MB partition alignment gap: sectors 2040..2047 */
            g_id_lba = (uint64_t)(p1_start - IDSTORE_BLOCKS);
            g_id_available = true;
            g_id_lba_resolved = true;
            printk("[IdStore] NVMe Identity Store using pre-partition reserved LBA %llu..%llu\n",
                   (unsigned long long)g_id_lba, (unsigned long long)(g_id_lba + IDSTORE_BLOCKS - 1));
            return;
        }
    }

    /* Fallback: use sectors 2040..2047 */
    g_id_lba = 2040;
    g_id_available = true;
    g_id_lba_resolved = true;
    printk("[IdStore] NVMe Identity Store defaulted to reserved LBA %llu..%llu\n",
           (unsigned long long)g_id_lba, (unsigned long long)(g_id_lba + IDSTORE_BLOCKS - 1));
}

static int idnvme_read(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if ((uint64_t)lba + count > IDSTORE_BLOCKS) return -1;
    block_dev_t *n_dev = nvme_get_device();
    if (!n_dev || !n_dev->read_blocks) return -1;

    ylock_acquire(&g_id_lock);
    idstore_nvme_resolve_lba();
    if (!g_id_available) {
        ylock_release(&g_id_lock);
        return -1;
    }
    int rc = n_dev->read_blocks(n_dev, buf, (uint32_t)(g_id_lba + lba), count);
    ylock_release(&g_id_lock);
    return rc;
}

static int idnvme_write(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (lba != 0 || count != IDSTORE_BLOCKS) {
        printk("[IdStore] NVMe partial write refused (lba %u, %u blocks): only whole %u-block record\n",
               (unsigned)lba, (unsigned)count, (unsigned)IDSTORE_BLOCKS);
        return -1;
    }
    block_dev_t *n_dev = nvme_get_device();
    if (!n_dev || !n_dev->write_blocks) return -1;

    ylock_acquire(&g_id_lock);
    idstore_nvme_resolve_lba();
    if (!g_id_available) {
        ylock_release(&g_id_lock);
        return -1;
    }

    printk("[IdStore] writing identity record to NVMe LBA %llu\n", (unsigned long long)g_id_lba);
    int rc = n_dev->write_blocks(n_dev, buf, (uint32_t)g_id_lba, count);

    /* Verify write */
    if (rc == 0) {
        if (n_dev->read_blocks(n_dev, s_check_buf, (uint32_t)g_id_lba, count) != 0 ||
            memcmp(s_check_buf, buf, IDSTORE_SIZE_BYTES) != 0) {
            printk("[IdStore] NVMe write verification failed!\n");
            rc = -1;
        }
        memset(s_check_buf, 0, sizeof(s_check_buf));
    }

    ylock_release(&g_id_lock);
    return rc;
}

/* guarded by: g_id_lock */
static block_dev_t g_idstore_nvme_dev = {
    .name = "idnvme0",
    .block_size = IDSTORE_BLOCK_SIZE,
    .num_blocks = IDSTORE_BLOCKS,
    .read_blocks = idnvme_read,
    .write_blocks = idnvme_write,
};

/* single task: driver initialization and identity backend hook */
block_dev_t *identity_store_device(void) {
    /* single task: initialization on first call */
    static bool inited = false;
    if (!inited) {
        ylock_init(&g_id_lock);
        inited = true;
    }
    return &g_idstore_nvme_dev;
}
