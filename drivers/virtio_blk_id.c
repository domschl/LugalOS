#include "drivers/virtio_blk_id.h"
#include "kernel/identity.h"
#include "kernel/sched.h"
#include "kernel/printk.h"
#include <string.h>
#include <stdint.h>

/* A second, independent virtio-blk MMIO instance for the identity store --
 * see the header for why this is a separate small file rather than a second
 * mode of drivers/virtio_blk.c. The protocol constants, structs and
 * bring-up sequence below are the same virtio-blk MMIO v1/v2 handshake that
 * file implements; duplicated deliberately rather than shared, the same way
 * drivers/spisd_rp2350.c's "sdblk" task duplicates rather than shares
 * drivers/virtio_blk.c's "blk" task -- two small, independent devices, not
 * one device wearing two hats. */

#define VIRTIO_MMIO_MAGIC          0x74726976  // "virt"
#define VIRTIO_DEV_BLOCK           2

#define REG_MAGIC_VALUE            0x00
#define REG_VERSION                0x04
#define REG_DEVICE_ID              0x08
#define REG_DEVICE_FEATURES_SEL    0x14
#define REG_DRIVER_FEATURES        0x20
#define REG_DRIVER_FEATURES_SEL    0x24
#define REG_GUEST_PAGE_SIZE        0x28
#define REG_QUEUE_SEL              0x30
#define REG_QUEUE_NUM_MAX          0x34
#define REG_QUEUE_NUM              0x38
#define REG_QUEUE_ALIGN            0x3c
#define REG_QUEUE_PFN              0x40
#define REG_QUEUE_READY            0x44
#define REG_QUEUE_NOTIFY           0x50
#define REG_STATUS                 0x70
#define REG_QUEUE_DESC_LOW         0x80
#define REG_QUEUE_DESC_HIGH        0x84
#define REG_QUEUE_DRIVER_LOW       0x88
#define REG_QUEUE_DRIVER_HIGH      0x8c
#define REG_QUEUE_DEVICE_LOW       0x90
#define REG_QUEUE_DEVICE_HIGH      0x94
#define REG_CONFIG                 0x100

#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4

#define VRING_DESC_F_NEXT          1
#define VRING_DESC_F_WRITE         2

#define VIRTIO_BLK_T_IN            0  // Read block
#define VIRTIO_BLK_T_OUT           1  // Write block

#define ID_QUEUE_SIZE 4  /* the identity store is 8 blocks; a request never spans more than that */

struct id_virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
struct id_virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[ID_QUEUE_SIZE];
};
struct id_virtq_used_elem {
    uint32_t id;
    uint32_t len;
};
struct id_virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct id_virtq_used_elem ring[ID_QUEUE_SIZE];
};
struct id_virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};
struct id_virtqueue_mem {
    struct id_virtq_desc   desc[ID_QUEUE_SIZE];
    struct id_virtq_avail  avail;
    uint8_t pad[4096 - sizeof(struct id_virtq_desc) * ID_QUEUE_SIZE - sizeof(struct id_virtq_avail)];
    struct id_virtq_used   used;
} __attribute__((aligned(4096)));

static struct id_virtqueue_mem       g_id_vq_mem __attribute__((aligned(4096)));
static struct id_virtio_blk_req_hdr  g_id_req_hdr __attribute__((aligned(16)));
static uint8_t                       g_id_req_status __attribute__((aligned(16)));

static volatile uint32_t *g_id_mmio_base = NULL;
static uint32_t g_id_mmio_version = 0;
static uint32_t g_id_num_sectors = 8;
static block_dev_t g_id_dev;
static int g_id_initialized = 0;
static int g_id_probe_logged = 0;
static uint16_t g_id_last_used_idx = 0;

static int virtio_blk_id_transfer(uint32_t type, void *buf, uint32_t lba, uint32_t count) {
    if (!g_id_mmio_base || count == 0) return -1;

    g_id_req_hdr.type = type;
    g_id_req_hdr.reserved = 0;
    g_id_req_hdr.sector = lba;
    g_id_req_status = 0xFF;

    g_id_vq_mem.desc[0].addr = (uintptr_t)&g_id_req_hdr;
    g_id_vq_mem.desc[0].len = sizeof(struct id_virtio_blk_req_hdr);
    g_id_vq_mem.desc[0].flags = VRING_DESC_F_NEXT;
    g_id_vq_mem.desc[0].next = 1;

    g_id_vq_mem.desc[1].addr = (uintptr_t)buf;
    g_id_vq_mem.desc[1].len = count * 512;
    g_id_vq_mem.desc[1].flags = VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    g_id_vq_mem.desc[1].next = 2;

    g_id_vq_mem.desc[2].addr = (uintptr_t)&g_id_req_status;
    g_id_vq_mem.desc[2].len = 1;
    g_id_vq_mem.desc[2].flags = VRING_DESC_F_WRITE;
    g_id_vq_mem.desc[2].next = 0;

    uint16_t avail_idx = g_id_vq_mem.avail.idx;
    g_id_vq_mem.avail.ring[avail_idx % ID_QUEUE_SIZE] = 0;

    __asm__ __volatile__("" ::: "memory");
    g_id_vq_mem.avail.idx = avail_idx + 1;
    __asm__ __volatile__("" ::: "memory");

    g_id_mmio_base[REG_QUEUE_NOTIFY / 4] = 0;

    while (g_id_vq_mem.used.idx == g_id_last_used_idx) {
        sched_yield();
    }
    g_id_last_used_idx = g_id_vq_mem.used.idx;

    return (g_id_req_status == 0) ? 0 : -1;
}

static int virtio_blk_id_read(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (!buf || count == 0 || lba + count > g_id_num_sectors) return -1;
    return virtio_blk_id_transfer(VIRTIO_BLK_T_IN, buf, lba, count);
}

static int virtio_blk_id_write(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (!buf || count == 0 || lba + count > g_id_num_sectors) return -1;
    return virtio_blk_id_transfer(VIRTIO_BLK_T_OUT, (void *)buf, lba, count);
}

int virtio_blk_id_init(void) {
    /* Success latches; failure does not.
     *
     * This used to set g_id_initialized before scanning, so the *first* call
     * decided the answer for the rest of the boot -- and that call is easy to
     * make too early by accident, because it is reached indirectly through
     * identity_store_device() from anything that touches the record. One such
     * call from vfs_init() (before any bus had been scanned) left this
     * returning -1 forever: no device key, so a node could not prove itself
     * to a peer, and grants silently fell back to the SD card. The far end of
     * a two-node auth test saw a connection carrying zero bytes, which is a
     * long way from the cause.
     *
     * Re-scanning while the device is genuinely absent costs a handful of
     * register reads on a cold path. Latching a failure costs identity,
     * everywhere, undetectably. */
    if (g_id_initialized && g_id_mmio_base != NULL) return 0;

    /* The primary driver (drivers/virtio_blk.c) takes the FIRST virtio-blk
     * device it finds scanning this same range in this same order; this
     * takes the SECOND. Neither reads the other's state -- QEMU assigns MMIO
     * slots in the order `-device` arguments appear on its command line, so
     * two independent scans in address order agree on "first" and "second"
     * without needing to coordinate. */
    uint32_t seen = 0;
    for (uintptr_t addr = 0x10001000; addr <= 0x10008000; addr += 0x1000) {
        volatile uint32_t *mmio = (volatile uint32_t *)addr;
        if (mmio[REG_MAGIC_VALUE / 4] == VIRTIO_MMIO_MAGIC &&
            mmio[REG_DEVICE_ID / 4] == VIRTIO_DEV_BLOCK) {
            seen++;
            if (seen == 2) { g_id_mmio_base = mmio; break; }
        }
    }

    if (!g_id_mmio_base) {
        /* Logged once, not on every retry: a board without an identity disk
         * is a normal configuration, and this is now reached again on each
         * lookup rather than exactly once. */
        if (!g_id_probe_logged) {
            g_id_probe_logged = 1;
            printk("[VirtIO-Blk-Id] No second VirtIO MMIO block device detected; running unprovisioned.\n");
        }
        return -1;
    }
    g_id_initialized = 1;

    g_id_mmio_version = g_id_mmio_base[REG_VERSION / 4];

    g_id_mmio_base[REG_STATUS / 4] = 0;
    g_id_mmio_base[REG_STATUS / 4] |= VIRTIO_STATUS_ACKNOWLEDGE;
    g_id_mmio_base[REG_STATUS / 4] |= VIRTIO_STATUS_DRIVER;

    memset(&g_id_vq_mem, 0, sizeof(g_id_vq_mem));
    g_id_last_used_idx = 0;

    if (g_id_mmio_version == 1) {
        g_id_mmio_base[REG_GUEST_PAGE_SIZE / 4] = 4096;
        g_id_mmio_base[REG_QUEUE_SEL / 4] = 0;

        uint32_t max_queue = g_id_mmio_base[REG_QUEUE_NUM_MAX / 4];
        if (max_queue == 0) {
            printk("[VirtIO-Blk-Id] Queue 0 un-selectable.\n");
            g_id_mmio_base = NULL;
            return -1;
        }

        g_id_mmio_base[REG_QUEUE_NUM / 4] = ID_QUEUE_SIZE;
        g_id_mmio_base[REG_QUEUE_ALIGN / 4] = 4096;
        g_id_mmio_base[REG_QUEUE_PFN / 4] = (uint32_t)((uintptr_t)&g_id_vq_mem >> 12);
    } else {
        g_id_mmio_base[REG_QUEUE_SEL / 4] = 0;
        g_id_mmio_base[REG_QUEUE_NUM / 4] = ID_QUEUE_SIZE;

        uintptr_t desc_addr  = (uintptr_t)&g_id_vq_mem.desc[0];
        uintptr_t avail_addr = (uintptr_t)&g_id_vq_mem.avail;
        uintptr_t used_addr  = (uintptr_t)&g_id_vq_mem.used;

        g_id_mmio_base[REG_QUEUE_DESC_LOW / 4]  = (uint32_t)desc_addr;
        g_id_mmio_base[REG_QUEUE_DESC_HIGH / 4] = (uint32_t)(((uint64_t)desc_addr) >> 32);
        g_id_mmio_base[REG_QUEUE_DRIVER_LOW / 4]  = (uint32_t)avail_addr;
        g_id_mmio_base[REG_QUEUE_DRIVER_HIGH / 4] = (uint32_t)(((uint64_t)avail_addr) >> 32);
        g_id_mmio_base[REG_QUEUE_DEVICE_LOW / 4]  = (uint32_t)used_addr;
        g_id_mmio_base[REG_QUEUE_DEVICE_HIGH / 4] = (uint32_t)(((uint64_t)used_addr) >> 32);
        g_id_mmio_base[REG_QUEUE_READY / 4] = 1;
    }

    g_id_mmio_base[REG_STATUS / 4] |= VIRTIO_STATUS_DRIVER_OK;

    uint32_t cap_lo = g_id_mmio_base[REG_CONFIG / 4];
    uint32_t cap_hi = g_id_mmio_base[(REG_CONFIG + 4) / 4];
    uint64_t cap_sectors = ((uint64_t)cap_hi << 32) | cap_lo;
    if (cap_sectors > 0) {
        g_id_num_sectors = (uint32_t)cap_sectors;
    }

    g_id_dev.name = "virtio_blk_id0";
    g_id_dev.block_size = 512;
    g_id_dev.num_blocks = g_id_num_sectors;
    g_id_dev.read_blocks = virtio_blk_id_read;
    g_id_dev.write_blocks = virtio_blk_id_write;

    printk("[VirtIO-Blk-Id] Identity store device found: 'virtio_blk_id0', %u blocks (%u bytes)\n",
           g_id_num_sectors, g_id_num_sectors * 512);

    return 0;
}

block_dev_t *virtio_blk_id_get_device(void) {
    /* Retried until it works, rather than latched on the first attempt.
     *
     * g_id_initialized used to be set by virtio_blk_id_init() whether or not
     * it found anything, so a single call made *before* the bus was scanned
     * -- which is easy to do by accident, since this is reached indirectly
     * through identity_store_device() from anything that reads the record --
     * left this returning NULL for the rest of the boot. Every identity
     * operation then failed silently: no device key, so a node could not
     * prove itself to a peer, and grants quietly fell back to the SD card.
     *
     * Cost of retrying: one extra probe per call while the device is genuinely
     * absent, which is a handful of register reads on a path that is not hot.
     * Cost of latching: a boot-order mistake anywhere disables identity
     * everywhere, undetectably. Found on 2026-09-02, when caching the grants
     * at p9_init() -- which runs from vfs_init(), before dev_probe_all() --
     * made a two-node auth test fail with the far end receiving zero bytes. */
    if (!g_id_initialized || g_id_mmio_base == NULL) {
        virtio_blk_id_init();
    }
    if (g_id_mmio_base != NULL) {
        return &g_id_dev;
    }
    return NULL;
}

/* Overrides kernel/identity.c's weak default -- this file is only compiled
 * for the QEMU targets (CMakeLists.txt), so on any other target the default
 * (no backend) is still what's linked. */
block_dev_t *identity_store_device(void) { return virtio_blk_id_get_device(); }
