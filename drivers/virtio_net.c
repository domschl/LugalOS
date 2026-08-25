#include "drivers/virtio_net.h"
#include "kernel/printk.h"
#include <string.h>
#include <stdint.h>

/* virtio-mmio register map -- the same subset drivers/virtio_blk.c and
 * drivers/virtio_console.c use, plus the feature registers, which neither of
 * them touches and this driver must (see "Why this one negotiates" below). */
#define VIRTIO_MMIO_MAGIC          0x74726976  // "virt"
#define VIRTIO_DEV_NET             1

#define REG_MAGIC_VALUE            0x00
#define REG_VERSION                0x04
#define REG_DEVICE_ID              0x08
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

/* Feature bits. Only three matter here. */
#define VIRTIO_NET_F_MAC           5    /* device supplies a MAC in config space */
#define VIRTIO_NET_F_MRG_RXBUF     15   /* NOT negotiated -- see the header note */
#define VIRTIO_F_VERSION_1         32

/* Queue 0 is the receiveq, queue 1 the transmitq. There is no control queue
 * unless VIRTIO_NET_F_CTRL_VQ is negotiated, which this driver does not. */
#define RXQ 0
#define TXQ 1

#define QUEUE_SIZE   8
/* One frame plus the largest virtio-net header, rounded up. Sized for
 * NETIF_FRAME_MAX rather than for an MTU: a frame that arrives larger than we
 * can hold must be dropped as a counted event, not truncated into the stack. */
#define BUF_SIZE     1536
#define RX_BUFS      4

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
static uint8_t g_rx_buf[RX_BUFS][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t g_tx_buf[BUF_SIZE] __attribute__((aligned(16)));

static volatile uint32_t *g_mmio_base = NULL;
static uint32_t g_mmio_version = 0;
static int g_probed = 0;
static uint16_t g_rx_last_used = 0;
static uint16_t g_tx_last_used = 0;
static uint32_t g_hdr_len = 10;      /* set during feature negotiation */

/* Frames the device delivered that were too large for the caller's buffer, or
 * that arrived with an impossible length. Driver-level, so it lives here
 * rather than in netif_t's counters (net/netif.h says why). */
static uint32_t g_rx_malformed;

/* --- Why this one negotiates features, when neither sibling does ---
 *
 * drivers/virtio_blk.c and drivers/virtio_console.c skip feature negotiation
 * entirely and work, because for their devices the guest-visible layout does
 * not depend on it. For virtio-net it does, and by exactly two bytes: the
 * per-buffer header is `struct virtio_net_hdr`, which carries a trailing
 * `num_buffers` field -- and therefore is 12 bytes rather than 10 -- if and
 * only if VIRTIO_F_VERSION_1 or VIRTIO_NET_F_MRG_RXBUF was accepted.
 *
 * Guessing wrong does not fail loudly. It shifts every frame by two bytes, so
 * a destination MAC reads as the tail of the header, ARP looks like garbage,
 * and the bug presents as "the network does not work" rather than as anything
 * pointing at this function. Negotiating explicitly is four register writes
 * and removes the question. */
static uint32_t read_device_features(uint32_t sel) {
    if (g_mmio_version == 1) return sel == 0 ? g_mmio_base[REG_DEVICE_FEATURES / 4] : 0;
    g_mmio_base[REG_DEVICE_FEATURES_SEL / 4] = sel;
    __asm__ __volatile__("" ::: "memory");
    return g_mmio_base[REG_DEVICE_FEATURES / 4];
}

static void write_driver_features(uint32_t sel, uint32_t bits) {
    if (g_mmio_version == 1) {
        if (sel == 0) g_mmio_base[REG_DRIVER_FEATURES / 4] = bits;
        return;
    }
    g_mmio_base[REG_DRIVER_FEATURES_SEL / 4] = sel;
    __asm__ __volatile__("" ::: "memory");
    g_mmio_base[REG_DRIVER_FEATURES / 4] = bits;
}

/* Posts RX descriptor `i` (its whole buffer) so the device has somewhere to
 * write the next incoming frame. */
static void repost_rx(uint16_t i) {
    g_rxq.desc[i].addr = (uintptr_t)g_rx_buf[i];
    g_rxq.desc[i].len = BUF_SIZE;
    g_rxq.desc[i].flags = VRING_DESC_F_WRITE;
    g_rxq.desc[i].next = 0;

    uint16_t avail_idx = g_rxq.avail.idx;
    g_rxq.avail.ring[avail_idx % QUEUE_SIZE] = i;
    __asm__ __volatile__("" ::: "memory");
    g_rxq.avail.idx = avail_idx + 1;
    __asm__ __volatile__("" ::: "memory");

    g_mmio_base[REG_QUEUE_NOTIFY / 4] = RXQ;
}

/* --- netif_t glue --- */

static int net_poll(netif_t *nif) {
    (void)nif;
    if (!g_mmio_base) return -1;
    /* The device writes used.idx; acknowledging the interrupt is not required
     * to see it, since nothing here runs off an interrupt yet -- this driver
     * is polled, like every other transport in this tree. */
    __asm__ __volatile__("" ::: "memory");
    return (g_rxq.used.idx != g_rx_last_used) ? 1 : 0;
}

static int net_recv_frame(netif_t *nif, uint8_t *buf, uint32_t max_len) {
    if (!g_mmio_base || !buf) return -1;
    if (g_rxq.used.idx == g_rx_last_used) return -1;

    struct virtq_used_elem *e = &g_rxq.used.ring[g_rx_last_used % QUEUE_SIZE];
    uint16_t id = (uint16_t)(e->id % RX_BUFS);
    uint32_t total = e->len;

    int result;
    if (total <= g_hdr_len || total > BUF_SIZE) {
        /* Shorter than its own header, or longer than the buffer we posted.
         * Neither is something to pass upward. */
        g_rx_malformed++;
        result = -1;
    } else {
        uint32_t len = total - g_hdr_len;
        if (len > max_len || len > NETIF_FRAME_MAX) {
            nif->rx_dropped++;
            result = -1;
        } else {
            memcpy(buf, g_rx_buf[id] + g_hdr_len, len);
            result = (int)len;
        }
    }

    /* The buffer goes back to the device either way: a frame we could not use
     * is still a buffer the device is entitled to have back, and leaking one
     * per bad frame would quietly starve the receive path. */
    g_rx_last_used++;
    repost_rx(id);
    return result;
}

static int net_send_frame(netif_t *nif, const uint8_t *buf, uint32_t len) {
    (void)nif;
    if (!g_mmio_base || !buf || len == 0 || len + g_hdr_len > BUF_SIZE) return -1;

    memset(g_tx_buf, 0, g_hdr_len);   /* no checksum offload, no GSO */
    memcpy(g_tx_buf + g_hdr_len, buf, len);

    g_txq.desc[0].addr = (uintptr_t)g_tx_buf;
    g_txq.desc[0].len = g_hdr_len + len;
    g_txq.desc[0].flags = 0;          /* guest -> device */
    g_txq.desc[0].next = 0;

    uint16_t avail_idx = g_txq.avail.idx;
    g_txq.avail.ring[avail_idx % QUEUE_SIZE] = 0;
    __asm__ __volatile__("" ::: "memory");
    g_txq.avail.idx = avail_idx + 1;
    __asm__ __volatile__("" ::: "memory");

    g_mmio_base[REG_QUEUE_NOTIFY / 4] = TXQ;

    /* Blocking until the device has taken the buffer, matching
     * virtio_console_send() and drivers/virtio_blk.c: g_tx_buf is a single
     * shared staging buffer, so returning before the device is done with it
     * would let the next send overwrite a frame in flight. */
    while (g_txq.used.idx == g_tx_last_used) {
        __asm__ __volatile__("" ::: "memory");
    }
    g_tx_last_used = g_txq.used.idx;
    return (int)len;
}

static netif_t g_netif = {
    .name = "virtio-net",
    .poll = net_poll,
    .send_frame = net_send_frame,
    .recv_frame = net_recv_frame,
    .link_up = NULL,     /* a virtual wire has no carrier to lose */
    .ctx = NULL,
};

netif_t *virtio_net_get_netif(void) {
    return g_mmio_base ? &g_netif : NULL;
}

uint32_t virtio_net_malformed(void) { return g_rx_malformed; }

static int init_queue(struct virtqueue_mem *vq, uint32_t queue_idx) {
    memset(vq, 0, sizeof(*vq));
    g_mmio_base[REG_QUEUE_SEL / 4] = queue_idx;

    if (g_mmio_version == 1) {
        if (g_mmio_base[REG_QUEUE_NUM_MAX / 4] == 0) return -1;
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

int virtio_net_init(void) {
    if (g_probed) return (g_mmio_base != NULL) ? 0 : -1;
    g_probed = 1;

    for (uintptr_t addr = 0x10001000; addr <= 0x10008000; addr += 0x1000) {
        volatile uint32_t *mmio = (volatile uint32_t *)addr;
        if (mmio[REG_MAGIC_VALUE / 4] == VIRTIO_MMIO_MAGIC &&
            mmio[REG_DEVICE_ID / 4] == VIRTIO_DEV_NET) {
            g_mmio_base = mmio;
            break;
        }
    }
    if (!g_mmio_base) return -1;   /* no -netdev/-device: not an error, just no wire */

    g_mmio_version = g_mmio_base[REG_VERSION / 4];

    g_mmio_base[REG_STATUS / 4] = 0;                              /* reset */
    g_mmio_base[REG_STATUS / 4] |= VIRTIO_STATUS_ACKNOWLEDGE;
    g_mmio_base[REG_STATUS / 4] |= VIRTIO_STATUS_DRIVER;

    uint32_t dev_lo = read_device_features(0);
    uint32_t dev_hi = read_device_features(1);

    /* Accept only what is used: a MAC from config space, and VERSION_1 where
     * the device offers it. Everything else -- checksum offload, GSO, merged
     * receive buffers, the control queue -- is declined, which is what keeps
     * the header a fixed size and the receive path a single descriptor. */
    uint32_t drv_lo = dev_lo & (1u << VIRTIO_NET_F_MAC);
    uint32_t drv_hi = dev_hi & (1u << (VIRTIO_F_VERSION_1 - 32));

    write_driver_features(0, drv_lo);
    write_driver_features(1, drv_hi);

    bool modern = (drv_hi & (1u << (VIRTIO_F_VERSION_1 - 32))) != 0;
    g_hdr_len = modern ? 12u : 10u;

    if (g_mmio_version != 1) {
        g_mmio_base[REG_STATUS / 4] |= VIRTIO_STATUS_FEATURES_OK;
        __asm__ __volatile__("" ::: "memory");
        if (!(g_mmio_base[REG_STATUS / 4] & VIRTIO_STATUS_FEATURES_OK)) {
            printk("[VirtIO-Net] Device rejected our feature set.\n");
            g_mmio_base = NULL;
            return -1;
        }
    } else {
        g_mmio_base[REG_GUEST_PAGE_SIZE / 4] = 4096;
    }

    if (init_queue(&g_rxq, RXQ) != 0 || init_queue(&g_txq, TXQ) != 0) {
        printk("[VirtIO-Net] Queue setup failed.\n");
        g_mmio_base = NULL;
        return -1;
    }

    /* The MAC, from config space when the device offers one. Otherwise a
     * locally-administered address (first octet 0x02) that is at least
     * well-formed -- an interface with a broadcast or multicast source
     * address is a subtler failure than an interface with no address. */
    if (drv_lo & (1u << VIRTIO_NET_F_MAC)) {
        volatile uint8_t *cfg = (volatile uint8_t *)(g_mmio_base) + REG_CONFIG;
        for (uint32_t i = 0; i < NETIF_MAC_LEN; i++) g_netif.mac[i] = cfg[i];
    }
    /* No else. A device that offers no MAC leaves g_netif.mac zeroed, and
     * netif_register() fills it from the node's own identity -- which is
     * where a fallback belongs, so that every driver gets the same one
     * instead of each inventing a constant. This one used to invent
     * 02:00:00:00:00:01, which is to say: the same address on every board. */

    g_mmio_base[REG_STATUS / 4] |= VIRTIO_STATUS_DRIVER_OK;

    g_rx_last_used = 0;
    g_tx_last_used = 0;
    for (uint16_t i = 0; i < RX_BUFS; i++) repost_rx(i);

    if (netif_register(&g_netif) != 0) {
        g_mmio_base = NULL;
        return -1;
    }

    char mac[18];
    netif_mac_str(g_netif.mac, mac);
    printk("[VirtIO-Net] %s up, MAC %s, %s header (%u bytes), %u RX buffers.\n",
           g_netif.name, mac, modern ? "modern" : "legacy", g_hdr_len, RX_BUFS);
    return 0;
}
