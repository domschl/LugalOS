#include "drivers/virtio_blk.h"
#include "kernel/sched.h"
#include "kernel/printk.h"
#include <string.h>
#include <stdint.h>

#define VIRTIO_MMIO_MAGIC          0x74726976  // "virt"
#define VIRTIO_DEV_BLOCK           2

#define REG_MAGIC_VALUE            0x00
#define REG_VERSION                0x04
#define REG_DEVICE_ID              0x08
#define REG_VENDOR_ID              0x0c
#define REG_DEVICE_FEATURES        0x10
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
#define REG_INTERRUPT_STATUS       0x60
#define REG_INTERRUPT_ACK          0x64
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
#define VIRTIO_STATUS_FEATURES_OK  8

#define VRING_DESC_F_NEXT          1
#define VRING_DESC_F_WRITE         2

#define VIRTIO_BLK_T_IN            0  // Read block
#define VIRTIO_BLK_T_OUT           1  // Write block

#define QUEUE_SIZE 8

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QUEUE_SIZE];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[QUEUE_SIZE];
};

struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

struct virtqueue_mem {
    struct virtq_desc desc[QUEUE_SIZE];
    struct virtq_avail avail;
    uint8_t pad[4096 - sizeof(struct virtq_desc) * QUEUE_SIZE - sizeof(struct virtq_avail)];
    struct virtq_used used;
} __attribute__((aligned(4096)));

static struct virtqueue_mem g_vq_mem __attribute__((aligned(4096)));
static struct virtio_blk_req_hdr g_req_hdr __attribute__((aligned(16)));
static uint8_t g_req_status __attribute__((aligned(16)));

static volatile uint32_t *g_mmio_base = NULL;
static uint32_t g_mmio_version = 0;
static uint32_t g_num_sectors = 1024;
static block_dev_t g_virtio_blk_dev;
static int g_initialized = 0;
static uint16_t g_last_used_idx = 0;

static int virtio_blk_transfer(uint32_t type, void *buf, uint32_t lba, uint32_t count) {
    if (!g_mmio_base || count == 0) return -1;

    g_req_hdr.type = type;
    g_req_hdr.reserved = 0;
    g_req_hdr.sector = lba;
    g_req_status = 0xFF;

    /* Setup Descriptor 0: Request Header */
    g_vq_mem.desc[0].addr = (uintptr_t)&g_req_hdr;
    g_vq_mem.desc[0].len = sizeof(struct virtio_blk_req_hdr);
    g_vq_mem.desc[0].flags = VRING_DESC_F_NEXT;
    g_vq_mem.desc[0].next = 1;

    /* Setup Descriptor 1: Data Buffer */
    g_vq_mem.desc[1].addr = (uintptr_t)buf;
    g_vq_mem.desc[1].len = count * 512;
    g_vq_mem.desc[1].flags = VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    g_vq_mem.desc[1].next = 2;

    /* Setup Descriptor 2: Status Byte */
    g_vq_mem.desc[2].addr = (uintptr_t)&g_req_status;
    g_vq_mem.desc[2].len = 1;
    g_vq_mem.desc[2].flags = VRING_DESC_F_WRITE;
    g_vq_mem.desc[2].next = 0;

    /* Put Head Descriptor (0) onto Avail Ring */
    uint16_t avail_idx = g_vq_mem.avail.idx;
    g_vq_mem.avail.ring[avail_idx % QUEUE_SIZE] = 0;

    __asm__ __volatile__("" ::: "memory");
    g_vq_mem.avail.idx = avail_idx + 1;
    __asm__ __volatile__("" ::: "memory");

    /* Notify Queue 0 */
    g_mmio_base[REG_QUEUE_NOTIFY / 4] = 0;

    /* Poll for completion */
    while (g_vq_mem.used.idx == g_last_used_idx) {
        /* B2: yield instead of spinning. A block transfer is slow enough
         * that holding the CPU through it would stall every other task. */
        sched_yield();
    }
    g_last_used_idx = g_vq_mem.used.idx;

    return (g_req_status == 0) ? 0 : -1;
}

static int virtio_blk_read(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (!buf || lba + count > g_num_sectors) return -1;
    return virtio_blk_transfer(VIRTIO_BLK_T_IN, buf, lba, count);
}

static int virtio_blk_write(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (!buf || lba + count > g_num_sectors) return -1;
    return virtio_blk_transfer(VIRTIO_BLK_T_OUT, (void *)buf, lba, count);
}

int virtio_blk_init(void) {
    if (g_initialized) return (g_mmio_base != NULL) ? 0 : -1;
    g_initialized = 1;

    /* Probe QEMU VirtIO MMIO slots (0x10001000..0x10008000) */
    for (uintptr_t addr = 0x10001000; addr <= 0x10008000; addr += 0x1000) {
        volatile uint32_t *mmio = (volatile uint32_t *)addr;
        if (mmio[REG_MAGIC_VALUE / 4] == VIRTIO_MMIO_MAGIC &&
            mmio[REG_DEVICE_ID / 4] == VIRTIO_DEV_BLOCK) {
            g_mmio_base = mmio;
            break;
        }
    }

    if (!g_mmio_base) {
        printk("[VirtIO-Blk] No VirtIO MMIO block device detected.\n");
        return -1;
    }

    g_mmio_version = g_mmio_base[REG_VERSION / 4];

    /* Reset device */
    g_mmio_base[REG_STATUS / 4] = 0;

    /* Acknowledge & Driver Status */
    g_mmio_base[REG_STATUS / 4] |= VIRTIO_STATUS_ACKNOWLEDGE;
    g_mmio_base[REG_STATUS / 4] |= VIRTIO_STATUS_DRIVER;

    memset(&g_vq_mem, 0, sizeof(g_vq_mem));
    g_last_used_idx = 0;

    if (g_mmio_version == 1) {
        /* Legacy VirtIO MMIO (v1) */
        g_mmio_base[REG_GUEST_PAGE_SIZE / 4] = 4096;
        g_mmio_base[REG_QUEUE_SEL / 4] = 0;

        uint32_t max_queue = g_mmio_base[REG_QUEUE_NUM_MAX / 4];
        if (max_queue == 0) {
            printk("[VirtIO-Blk] Queue 0 un-selectable.\n");
            g_mmio_base = NULL;
            return -1;
        }

        g_mmio_base[REG_QUEUE_NUM / 4] = QUEUE_SIZE;
        g_mmio_base[REG_QUEUE_ALIGN / 4] = 4096;
        g_mmio_base[REG_QUEUE_PFN / 4] = (uint32_t)((uintptr_t)&g_vq_mem >> 12);
    } else {
        /* Modern VirtIO MMIO (v2) */
        g_mmio_base[REG_QUEUE_SEL / 4] = 0;
        g_mmio_base[REG_QUEUE_NUM / 4] = QUEUE_SIZE;

        uintptr_t desc_addr = (uintptr_t)&g_vq_mem.desc[0];
        uintptr_t avail_addr = (uintptr_t)&g_vq_mem.avail;
        uintptr_t used_addr = (uintptr_t)&g_vq_mem.used;

        g_mmio_base[REG_QUEUE_DESC_LOW / 4] = (uint32_t)desc_addr;
        g_mmio_base[REG_QUEUE_DESC_HIGH / 4] = (uint32_t)(((uint64_t)desc_addr) >> 32);
        g_mmio_base[REG_QUEUE_DRIVER_LOW / 4] = (uint32_t)avail_addr;
        g_mmio_base[REG_QUEUE_DRIVER_HIGH / 4] = (uint32_t)(((uint64_t)avail_addr) >> 32);
        g_mmio_base[REG_QUEUE_DEVICE_LOW / 4] = (uint32_t)used_addr;
        g_mmio_base[REG_QUEUE_DEVICE_HIGH / 4] = (uint32_t)(((uint64_t)used_addr) >> 32);
        g_mmio_base[REG_QUEUE_READY / 4] = 1;
    }

    /* Driver OK */
    g_mmio_base[REG_STATUS / 4] |= VIRTIO_STATUS_DRIVER_OK;

    /* Read capacity in sectors from config space */
    uint32_t cap_lo = g_mmio_base[REG_CONFIG / 4];
    uint32_t cap_hi = g_mmio_base[(REG_CONFIG + 4) / 4];
    uint64_t cap_sectors = ((uint64_t)cap_hi << 32) | cap_lo;
    if (cap_sectors > 0) {
        g_num_sectors = (uint32_t)cap_sectors;
    }

    g_virtio_blk_dev.name = "virtio_blk0";
    g_virtio_blk_dev.block_size = 512;
    g_virtio_blk_dev.num_blocks = g_num_sectors;
    g_virtio_blk_dev.read_blocks = virtio_blk_read;
    g_virtio_blk_dev.write_blocks = virtio_blk_write;

    printk("[VirtIO-Blk] Disk Image Mounted! Device: 'virtio_blk0', Capacity: %u blocks (%u KB)\n",
           g_num_sectors, (g_num_sectors * 512) / 1024);

    return 0;
}

block_dev_t *virtio_blk_get_device(void) {
    if (!g_initialized) {
        virtio_blk_init();
    }
    if (g_mmio_base != NULL) {
        return &g_virtio_blk_dev;
    }
    return NULL;
}
