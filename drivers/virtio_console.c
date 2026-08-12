#include "drivers/virtio_console.h"
#include "kernel/printk.h"
#include <string.h>
#include <stdint.h>

#define VIRTIO_MMIO_MAGIC          0x74726976  // "virt"
#define VIRTIO_DEV_CONSOLE         3

#define REG_MAGIC_VALUE            0x00
#define REG_VERSION                0x04
#define REG_DEVICE_ID              0x08
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
#define REG_GUEST_PAGE_SIZE        0x28

#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4

#define VRING_DESC_F_NEXT          1
#define VRING_DESC_F_WRITE         2

/* virtio-console, non-multiport: queue 0 is the receiveq (host -> guest),
 * queue 1 is the transmitq (guest -> host). No control queue exists unless
 * VIRTIO_CONSOLE_F_MULTIPORT is negotiated, which this driver (like
 * drivers/virtio_blk.c) deliberately doesn't -- it accepts host defaults,
 * matching QEMU's plain `-device virtconsole` (not `virtserialport`). */
#define QUEUE_SIZE   4
#define RX_BUF_SIZE  2048
#define TX_BUF_SIZE  2048
#define RX_RING_CAP  8192  // must exceed P9_MAX_MSIZE (4096) with headroom for an in-flight partial frame

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

struct virtqueue_mem {
    struct virtq_desc desc[QUEUE_SIZE];
    struct virtq_avail avail;
    uint8_t pad[4096 - sizeof(struct virtq_desc) * QUEUE_SIZE - sizeof(struct virtq_avail)];
    struct virtq_used used;
} __attribute__((aligned(4096)));

static struct virtqueue_mem g_rxq __attribute__((aligned(4096)));
static struct virtqueue_mem g_txq __attribute__((aligned(4096)));
static uint8_t g_rx_buf[RX_BUF_SIZE] __attribute__((aligned(16)));
static uint8_t g_tx_buf[TX_BUF_SIZE] __attribute__((aligned(16)));

static volatile uint32_t *g_mmio_base = NULL;
static uint32_t g_mmio_version = 0;
static int g_probed = 0;
static uint16_t g_rx_last_used = 0;
static uint16_t g_tx_last_used = 0;

/* --- Software byte ring: buffers bytes drained from completed RX
 * virtqueue transfers until a caller has pulled a full 9P frame out via
 * recv_frame(). --- */
typedef struct {
    uint8_t buf[RX_RING_CAP];
    uint32_t head, tail, count;
} byte_ring_t;

static byte_ring_t g_rx_ring;

static void ring_push(byte_ring_t *r, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len && r->count < RX_RING_CAP; i++) {
        r->buf[r->tail] = data[i];
        r->tail = (r->tail + 1) % RX_RING_CAP;
        r->count++;
    }
}

static uint32_t ring_peek(byte_ring_t *r, uint8_t *out, uint32_t max) {
    uint32_t n = max < r->count ? max : r->count;
    uint32_t idx = r->head;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = r->buf[idx];
        idx = (idx + 1) % RX_RING_CAP;
    }
    return n;
}

static uint32_t ring_pop(byte_ring_t *r, uint8_t *out, uint32_t max) {
    uint32_t n = ring_peek(r, out, max);
    r->head = (r->head + n) % RX_RING_CAP;
    r->count -= n;
    return n;
}

/* Posts g_rx_buf (desc 0 of the RX queue) onto the avail ring so the
 * device has somewhere to write the next chunk of incoming bytes. Called
 * once at init and again after every completed receive. */
static void virtio_console_repost_rx(void) {
    g_rxq.desc[0].addr = (uintptr_t)g_rx_buf;
    g_rxq.desc[0].len = RX_BUF_SIZE;
    g_rxq.desc[0].flags = VRING_DESC_F_WRITE;
    g_rxq.desc[0].next = 0;

    uint16_t avail_idx = g_rxq.avail.idx;
    g_rxq.avail.ring[avail_idx % QUEUE_SIZE] = 0;
    __asm__ __volatile__("" ::: "memory");
    g_rxq.avail.idx = avail_idx + 1;
    __asm__ __volatile__("" ::: "memory");

    g_mmio_base[REG_QUEUE_NOTIFY / 4] = 0; // queue 0
}

/* Non-blocking: drains any RX virtqueue completions into g_rx_ring and
 * re-posts the buffer. Never waits for the device. */
static void virtio_console_drain_rx(void) {
    if (!g_mmio_base) return;
    while (g_rxq.used.idx != g_rx_last_used) {
        struct virtq_used_elem *e = &g_rxq.used.ring[g_rx_last_used % QUEUE_SIZE];
        uint32_t n = e->len;
        if (n > RX_BUF_SIZE) n = RX_BUF_SIZE;
        ring_push(&g_rx_ring, g_rx_buf, n);
        g_rx_last_used++;
        virtio_console_repost_rx();
    }
}

/* Blocking (like virtio_blk_transfer()): sends exactly `len` bytes over the
 * TX queue and waits for the device to consume them. `len` must fit in
 * TX_BUF_SIZE -- true for every 9P frame this server ever produces
 * (P9_MAX_MSIZE = 4096 > TX_BUF_SIZE would be a bug; guarded below). */
static int virtio_console_send(const uint8_t *data, uint32_t len) {
    if (!g_mmio_base || !data || len == 0 || len > TX_BUF_SIZE) return -1;

    memcpy(g_tx_buf, data, len);

    g_txq.desc[0].addr = (uintptr_t)g_tx_buf;
    g_txq.desc[0].len = len;
    g_txq.desc[0].flags = 0; // guest -> device, no NEXT/WRITE
    g_txq.desc[0].next = 0;

    uint16_t avail_idx = g_txq.avail.idx;
    g_txq.avail.ring[avail_idx % QUEUE_SIZE] = 0;
    __asm__ __volatile__("" ::: "memory");
    g_txq.avail.idx = avail_idx + 1;
    __asm__ __volatile__("" ::: "memory");

    g_mmio_base[REG_QUEUE_NOTIFY / 4] = 1; // queue 1

    while (g_txq.used.idx == g_tx_last_used) {
        __asm__ __volatile__("" ::: "memory");
    }
    g_tx_last_used = g_txq.used.idx;
    return (int)len;
}

/* J5 (plan/phase10_chess_completion.md): raw byte read/write for
 * chess_uci_run() (chess_ui.c) on QEMU, which has no USB CDC hardware at
 * all -- unlike RP2350's dedicated ACM2/EP6 (drivers/usb_cdc.c), this is
 * the same single virtio-console link 9P already uses in the background
 * (DEV_F_BACKGROUND_9P, kernel/board.c's dev_vconsole), so a QEMU UCI test
 * session must suspend that background service first (p9_link_
 * unregister_background()) to avoid both consumers racing over g_rx_ring --
 * chess_uci_run() itself does this. Real hardware deployments don't hit
 * this at all: ACM2 is a dedicated wire, never shared with usbnet's P9. */
int virtio_console_write_raw(const uint8_t *data, uint32_t len) {
    return virtio_console_send(data, len);
}

uint32_t virtio_console_read_raw(uint8_t *buf, uint32_t max) {
    if (!buf || max == 0) return 0;
    virtio_console_drain_rx();
    return ring_pop(&g_rx_ring, buf, max);
}

/* --- p9_link_t glue --- */

static int link_poll(p9_link_t *link) {
    (void)link;
    virtio_console_drain_rx();
    if (g_rx_ring.count < 4) return 0;

    uint8_t hdr[4];
    ring_peek(&g_rx_ring, hdr, 4);
    uint32_t declared = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (declared < 7 || declared > RX_RING_CAP) return -1; // corrupt stream

    return (g_rx_ring.count >= declared) ? 1 : 0;
}

static int link_recv_frame(p9_link_t *link, uint8_t *buf, uint32_t max_len) {
    (void)link;
    if (g_rx_ring.count < 4) return -1;
    uint8_t hdr[4];
    ring_peek(&g_rx_ring, hdr, 4);
    uint32_t declared = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (declared < 7 || declared > max_len || g_rx_ring.count < declared) return -1;
    return (int)ring_pop(&g_rx_ring, buf, declared);
}

static int link_send_frame(p9_link_t *link, const uint8_t *buf, uint32_t len) {
    (void)link;
    return virtio_console_send(buf, len);
}

static p9_link_t g_link = {
    .name = "virtio-console",
    .poll = link_poll,
    .send_frame = link_send_frame,
    .recv_frame = link_recv_frame,
    .ctx = NULL,
};

p9_link_t *virtio_console_get_link(void) {
    return g_mmio_base ? &g_link : NULL;
}

static int virtio_console_init_queue(struct virtqueue_mem *vq, uint32_t queue_idx) {
    memset(vq, 0, sizeof(*vq));
    g_mmio_base[REG_QUEUE_SEL / 4] = queue_idx;

    if (g_mmio_version == 1) {
        uint32_t max_queue = g_mmio_base[REG_QUEUE_NUM_MAX / 4];
        if (max_queue == 0) return -1;
        g_mmio_base[REG_QUEUE_NUM / 4] = QUEUE_SIZE;
        g_mmio_base[REG_QUEUE_ALIGN / 4] = 4096;
        g_mmio_base[REG_QUEUE_PFN / 4] = (uint32_t)((uintptr_t)vq >> 12);
    } else {
        g_mmio_base[REG_QUEUE_NUM / 4] = QUEUE_SIZE;
        uintptr_t desc_addr = (uintptr_t)&vq->desc[0];
        uintptr_t avail_addr = (uintptr_t)&vq->avail;
        uintptr_t used_addr = (uintptr_t)&vq->used;
        g_mmio_base[REG_QUEUE_DESC_LOW / 4] = (uint32_t)desc_addr;
        g_mmio_base[REG_QUEUE_DESC_HIGH / 4] = (uint32_t)(((uint64_t)desc_addr) >> 32);
        g_mmio_base[REG_QUEUE_DRIVER_LOW / 4] = (uint32_t)avail_addr;
        g_mmio_base[REG_QUEUE_DRIVER_HIGH / 4] = (uint32_t)(((uint64_t)avail_addr) >> 32);
        g_mmio_base[REG_QUEUE_DEVICE_LOW / 4] = (uint32_t)used_addr;
        g_mmio_base[REG_QUEUE_DEVICE_HIGH / 4] = (uint32_t)(((uint64_t)used_addr) >> 32);
        g_mmio_base[REG_QUEUE_READY / 4] = 1;
    }
    return 0;
}

int virtio_console_init(void) {
    if (g_probed) return (g_mmio_base != NULL) ? 0 : -1;
    g_probed = 1;

    for (uintptr_t addr = 0x10001000; addr <= 0x10008000; addr += 0x1000) {
        volatile uint32_t *mmio = (volatile uint32_t *)addr;
        if (mmio[REG_MAGIC_VALUE / 4] == VIRTIO_MMIO_MAGIC &&
            mmio[REG_DEVICE_ID / 4] == VIRTIO_DEV_CONSOLE) {
            g_mmio_base = mmio;
            break;
        }
    }

    if (!g_mmio_base) {
        printk("[VirtIO-Console] No VirtIO MMIO console device detected (no -device virtconsole?).\n");
        return -1;
    }

    g_mmio_version = g_mmio_base[REG_VERSION / 4];

    g_mmio_base[REG_STATUS / 4] = 0; // reset
    g_mmio_base[REG_STATUS / 4] |= VIRTIO_STATUS_ACKNOWLEDGE;
    g_mmio_base[REG_STATUS / 4] |= VIRTIO_STATUS_DRIVER;

    if (g_mmio_version == 1) {
        g_mmio_base[REG_GUEST_PAGE_SIZE / 4] = 4096;
    }

    if (virtio_console_init_queue(&g_rxq, 0) != 0 || virtio_console_init_queue(&g_txq, 1) != 0) {
        printk("[VirtIO-Console] Queue setup failed.\n");
        g_mmio_base = NULL;
        return -1;
    }

    g_mmio_base[REG_STATUS / 4] |= VIRTIO_STATUS_DRIVER_OK;

    g_rx_last_used = 0;
    g_tx_last_used = 0;
    memset(&g_rx_ring, 0, sizeof(g_rx_ring));
    virtio_console_repost_rx();

    printk("[VirtIO-Console] Dedicated 9P transport channel online.\n");
    return 0;
}
