#include "drivers/virtio_rng.h"
#include "kernel/lock.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include <string.h>

/* virtio-mmio registers: the subset drivers/virtio_net.c uses. */
#define VIRTIO_MMIO_MAGIC          0x74726976  /* "virt" */
#define VIRTIO_DEV_RNG             4

#define REG_MAGIC_VALUE            0x00
#define REG_VERSION                0x04
#define REG_DEVICE_ID              0x08
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

#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8

#define VRING_DESC_F_WRITE         2

#define QUEUE_SIZE   2
#define BUF_SIZE     64u
#define READ_TIMEOUT_US 200000u

struct virtq_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[QUEUE_SIZE]; };
struct virtq_used_elem { uint32_t id; uint32_t len; };
struct virtq_used { uint16_t flags; uint16_t idx; struct virtq_used_elem ring[QUEUE_SIZE]; };

/* The legacy (version 1) layout: the used ring on the page after the descriptors and the avail ring. */
struct virtqueue_mem {
    struct virtq_desc desc[QUEUE_SIZE];
    struct virtq_avail avail;
    uint8_t pad[4096 - sizeof(struct virtq_desc) * QUEUE_SIZE - sizeof(struct virtq_avail)];
    struct virtq_used used;
} __attribute__((aligned(4096)));

/* guarded by: g_lock (the queue, the buffer and the used index are one request at a time); the
 * probe fills them once, at boot, before any task runs. */
static ylock_t g_lock;
static struct virtqueue_mem g_q __attribute__((aligned(4096)));
static uint8_t g_buf[BUF_SIZE] __attribute__((aligned(16)));
static uint16_t g_last_used;
/* single task: the boot task (virtio_rng_init()); read-only afterwards. */
static volatile uint32_t *g_mmio;
static uint32_t g_version;

bool virtio_rng_present(void) { return g_mmio != NULL; }

static int init_queue(void) {
    memset(&g_q, 0, sizeof g_q);
    g_mmio[REG_QUEUE_SEL / 4] = 0;
    if (g_mmio[REG_QUEUE_NUM_MAX / 4] < QUEUE_SIZE) return -1;
    g_mmio[REG_QUEUE_NUM / 4] = QUEUE_SIZE;
    if (g_version == 1) {
        g_mmio[REG_QUEUE_ALIGN / 4] = 4096;
        g_mmio[REG_QUEUE_PFN / 4] = (uint32_t)((uintptr_t)&g_q >> 12);
    } else {
        uintptr_t d = (uintptr_t)&g_q.desc[0], a = (uintptr_t)&g_q.avail, u = (uintptr_t)&g_q.used;
        g_mmio[REG_QUEUE_DESC_LOW / 4] = (uint32_t)d;
        g_mmio[REG_QUEUE_DESC_HIGH / 4] = (uint32_t)((uint64_t)d >> 32);
        g_mmio[REG_QUEUE_DRIVER_LOW / 4] = (uint32_t)a;
        g_mmio[REG_QUEUE_DRIVER_HIGH / 4] = (uint32_t)((uint64_t)a >> 32);
        g_mmio[REG_QUEUE_DEVICE_LOW / 4] = (uint32_t)u;
        g_mmio[REG_QUEUE_DEVICE_HIGH / 4] = (uint32_t)((uint64_t)u >> 32);
        g_mmio[REG_QUEUE_READY / 4] = 1;
    }
    return 0;
}

int virtio_rng_init(void) {
    if (g_mmio) return 0;
    volatile uint32_t *found = NULL;
    for (uintptr_t addr = 0x10001000; addr <= 0x10008000; addr += 0x1000) {
        volatile uint32_t *m = (volatile uint32_t *)addr;
        if (m[REG_MAGIC_VALUE / 4] == VIRTIO_MMIO_MAGIC && m[REG_DEVICE_ID / 4] == VIRTIO_DEV_RNG) { found = m; break; }
    }
    if (!found) return -1;                       /* no -device virtio-rng-device: not an error */
    g_mmio = found;
    g_version = g_mmio[REG_VERSION / 4];

    g_mmio[REG_STATUS / 4] = 0;
    g_mmio[REG_STATUS / 4] |= VIRTIO_STATUS_ACKNOWLEDGE;
    g_mmio[REG_STATUS / 4] |= VIRTIO_STATUS_DRIVER;
    /* The device has no features of its own; nothing is accepted. */
    if (g_version == 1) {
        g_mmio[REG_DRIVER_FEATURES / 4] = 0;
        g_mmio[REG_GUEST_PAGE_SIZE / 4] = 4096;
    } else {
        for (uint32_t sel = 0; sel < 2; sel++) {
            g_mmio[REG_DRIVER_FEATURES_SEL / 4] = sel;
            g_mmio[REG_DRIVER_FEATURES / 4] = 0;
        }
        g_mmio[REG_STATUS / 4] |= VIRTIO_STATUS_FEATURES_OK;
        if (!(g_mmio[REG_STATUS / 4] & VIRTIO_STATUS_FEATURES_OK)) { g_mmio = NULL; return -1; }
    }
    if (init_queue() != 0) { g_mmio = NULL; return -1; }
    g_mmio[REG_STATUS / 4] |= VIRTIO_STATUS_DRIVER_OK;
    g_last_used = 0;
    printk("[VirtIO-RNG] entropy device up (virtio-mmio v%u)\n", (unsigned)g_version);
    return 0;
}

/* One request: the whole buffer offered to the device, then a poll until it hands it back. The
 * device may return fewer bytes than offered; the caller asks again. Called with g_lock held. */
static uint32_t fill_once(void) {
    g_q.desc[0].addr = (uintptr_t)g_buf;
    g_q.desc[0].len = BUF_SIZE;
    g_q.desc[0].flags = VRING_DESC_F_WRITE;
    g_q.desc[0].next = 0;
    uint16_t ai = g_q.avail.idx;
    g_q.avail.ring[ai % QUEUE_SIZE] = 0;
    __asm__ __volatile__("fence rw, rw" ::: "memory");
    g_q.avail.idx = (uint16_t)(ai + 1);
    __asm__ __volatile__("fence rw, rw" ::: "memory");
    g_mmio[REG_QUEUE_NOTIFY / 4] = 0;

    uint64_t start = time_get_us();
    for (;;) {
        __asm__ __volatile__("fence rw, rw" ::: "memory");
        if (*(volatile uint16_t *)&g_q.used.idx != g_last_used) break;
        if (time_get_us() - start > READ_TIMEOUT_US) return 0;
    }
    uint32_t n = g_q.used.ring[g_last_used % QUEUE_SIZE].len;
    g_last_used++;
    return n > BUF_SIZE ? BUF_SIZE : n;
}

bool virtio_rng_read(void *buf, uint32_t len) {
    if (!g_mmio) return false;
    uint8_t *p = (uint8_t *)buf;
    bool ok = true;
    ylock_acquire(&g_lock);
    while (len > 0) {
        uint32_t n = fill_once();
        if (n == 0) { ok = false; break; }
        if (n > len) n = len;
        memcpy(p, g_buf, n);
        p += n;
        len -= n;
    }
    memset(g_buf, 0, sizeof g_buf);
    ylock_release(&g_lock);
    return ok;
}
