/* The ESP32-C6's SDIO slave in packet mode: LLP's SDIO binding, slave side.
 * kernel/include/kernel/llp_sdio.h is the contract with the P4's host driver;
 * this file is the C6's half of it.
 *
 * The peripheral has three register blocks: HINF (the card's identity and
 * CCCR bits), SLC (the two DMA engines and their interrupts) and SLCHOST
 * (what the host sees through function 1: tokens, PKT_LEN, the shared
 * bytes). The SLC names are the host's: SLC "tx" moves host -> slave, which
 * this file calls receiving; SLC "rx" moves slave -> host, sending.
 *
 * Polled, no interrupt: the LLP pump calls in, and the host polls too.
 *
 * ## Receiving
 *
 * NRX buffers of LLP_SDIO_BUF bytes, each with a descriptor, chained in ring
 * order with the last one loaded ending the chain (next = NULL). The token
 * counter tells the host how many are loaded; the host never writes more
 * than that, so the DMA never reaches the end of the chain. A packet fills
 * consecutive buffers, the DMA clears each one's owner bit and sets eof on the
 * last. A consumed buffer is reloaded at the tail (owner set, linked, DMA
 * restarted) and counted again.
 *
 * ## Sending
 *
 * One descriptor, one packet: the DMA is reset and started on it, then PKT_LEN
 * is advanced by its length (the host reads the difference). The host reading
 * it to the end raises RX_EOF. The descriptor sequence is the one IDF's
 * sdio_slave_hal.c uses (send_new_packet()).
 *
 * ## Fenced
 *
 * The slave's DMA is a bus master the HP APM can confine, and it is: the
 * TEE puts it in REE1, a security mode nothing else on this chip uses, and APM
 * region 1 lets REE1 read and write exactly the channel's block -- the
 * descriptors, the receive ring and the two send buffers, one allocation --
 * and nothing else (region 0, the whole address space, never grants REE1; see
 * kernel/radio_c6.c). A host that asks for something odd cannot make this DMA
 * touch the kernel. Without the grant the APM's reset state answers the DMA's
 * reads with zeros and drops its writes, silently: transfers "complete" and
 * nothing moves.
 *
 * Registers: IDF components/soc/esp32c6/register/soc/sdio_slc_reg.h,
 * sdio_slc_host_reg.h, sdio_hinf_reg.h, pcr_reg.h, tee_reg.h, hp_apm_reg.h; the
 * APM master number from esp_hal_security's apm_types.h; the descriptor layout from
 * esp_hal_sd/esp32c6/include/hal/sdio_slave_ll.h. The pads are fixed:
 * IO_MUX function 0 of GPIO18..23 (sdio_slave_pins.h). */

#include "lugalos_config.h"
#include "drivers/sdio_slave_esp32c6.h"
#include "kernel/console.h"

#if defined(CONFIG_SDIO_SLAVE)

#include "kernel/llp_sdio.h"
#include "kernel/palloc.h"
#include "kernel/random.h"
#include <string.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define PCR_SDIO_SLAVE_CONF  0x600960B0u
#define  PCR_SDIO_CLK_EN         (1u << 0)
#define  PCR_SDIO_RST_EN         (1u << 1)

#define IO_MUX_GPIO(n)       (0x60090004u + 4u * (uint32_t)(n))
#define  IO_MUX_FUN_WPU          (1u << 8)
#define  IO_MUX_FUN_IE           (1u << 9)
#define  IO_MUX_FUN_DRV_S        10
#define  IO_MUX_MCU_SEL_S        12

#define HINF_BASE            0x60016000u
#define HINF_CFG_DATA1       (HINF_BASE + 0x04u)
#define  HINF_IOREADY1           (1u << 1)
#define  HINF_HIGHSPEED          (1u << 2)
#define  HINF_SDIO_VER_S         12
#define  HINF_SDIO_VER_M         (0xFFFu << HINF_SDIO_VER_S)

#define SLC_BASE             0x60017000u
#define SLC_CONF0            (SLC_BASE + 0x000u)
#define  SLC0_TX_RST             (1u << 0)    /* host -> slave DMA */
#define  SLC0_RX_RST             (1u << 1)    /* slave -> host DMA */
#define  SLC0_TX_LOOP_TEST       (1u << 4)
#define  SLC0_RX_LOOP_TEST       (1u << 5)
#define  SLC0_RX_AUTO_WRBACK     (1u << 6)
#define  SLC0_TOKEN_AUTO_CLR     (1u << 14)
#define SLC_INT_RAW          (SLC_BASE + 0x004u)
#define SLC_INT_ENA          (SLC_BASE + 0x00Cu)
#define SLC_INT_CLR          (SLC_BASE + 0x010u)
#define  SLC_INT_FRHOST_M        0xFFu        /* host -> slave bits 0..7 */
#define  SLC_INT_TX_DONE         (1u << 14)
#define  SLC_INT_RX_EOF          (1u << 17)
#define SLC_RX_LINK          (SLC_BASE + 0x03Cu)  /* send */
#define SLC_RX_LINK_ADDR     (SLC_BASE + 0x040u)
#define SLC_TX_LINK          (SLC_BASE + 0x044u)  /* receive */
#define SLC_TX_LINK_ADDR     (SLC_BASE + 0x048u)
#define  SLC_LINK_STOP           (1u << 28)
#define  SLC_LINK_START          (1u << 29)
#define  SLC_LINK_RESTART        (1u << 30)
#define SLC_TOKEN1           (SLC_BASE + 0x064u)
#define  SLC_TOKEN_WDATA_S       0
#define  SLC_TOKEN_WR            (1u << 12)
#define  SLC_TOKEN_INC_MORE      (1u << 14)
#define SLC_CONF1            (SLC_BASE + 0x070u)
#define  SLC0_LEN_AUTO_CLR       (1u << 4)
#define  SLC0_TX_STITCH_EN       (1u << 5)
#define  SLC0_RX_STITCH_EN       (1u << 6)
#define SLC_RX_DSCR_CONF     (SLC_BASE + 0x0A8u)
#define  SLC0_TOKEN_NO_REPLACE   (1u << 0)
#define SLC_LEN_CONF         (SLC_BASE + 0x0F4u)
#define  SLC_LEN_WR              (1u << 20)

#define SLCHOST_BASE         0x60018000u
#define SLCHOST_INT_CLR      (SLCHOST_BASE + LLP_SDIO_REG_INT_CLR)
#define  SLCHOST_NEW_PACKET      (1u << 23)
#define SLCHOST_PKT_LEN      (SLCHOST_BASE + LLP_SDIO_REG_PKT_LEN)
#define SLCHOST_CONF_W0      (SLCHOST_BASE + LLP_SDIO_REG_SHARED)
#define SLCHOST_CONF_W1      (SLCHOST_BASE + LLP_SDIO_REG_SHARED + 4u)
#define SLCHOST_CONF_W7      (SLCHOST_BASE + (LLP_SDIO_REG_SLV_INT & ~3u))
#define  SLCHOST_SLV_INT_S       ((LLP_SDIO_REG_SLV_INT & 3u) * 8u)
#define SLCHOST_CONF         (SLCHOST_BASE + 0x1F0u)
#define  SLCHOST_FRC_SDIO11_S    0
#define  SLCHOST_FRC_SDIO20_S    5
#define  SLCHOST_FRC_NEG_SAMP_S  10
#define  SLCHOST_FRC_POS_SAMP_S  15

#define TEE_M_MODE_CTRL(m)   (0x60098000u + 4u * (uint32_t)(m))
#define  TEE_MASTER_SDIO_SLAVE   3u
#define  TEE_MODE_REE1           2u
#define HP_APM_BASE          0x60099000u
#define HP_APM_FILTER_EN     (HP_APM_BASE + 0x00u)
#define HP_APM_REGION_START(n) (HP_APM_BASE + 0x04u + 12u * (uint32_t)(n))
#define HP_APM_REGION_END(n)   (HP_APM_BASE + 0x08u + 12u * (uint32_t)(n))
#define HP_APM_REGION_ATTR(n)  (HP_APM_BASE + 0x0Cu + 12u * (uint32_t)(n))
#define  APM_REGION_SDIO         1u
#define  APM_REE1_RW             (6u << 4)    /* R (4) | W (2), REE1's field [6:4] */

#define SDIO_FIRST_GPIO      18u
#define SDIO_LAST_GPIO       23u
#define SDIO_CLK_GPIO        19u

/* The DMA descriptor: word 0 is size[13:0], length[27:14], eof bit 30, owner
 * bit 31 (1 = the DMA's). */
typedef struct dma_desc {
    volatile uint32_t ctl;
    volatile uint32_t buf;
    volatile uint32_t next;
} dma_desc_t;

#define D_SIZE(n)    ((uint32_t)(n) & 0x3FFFu)
#define D_LEN(n)     (((uint32_t)(n) & 0x3FFFu) << 14)
#define D_GET_LEN(c) (((c) >> 14) & 0x3FFFu)
#define D_EOF        (1u << 30)
#define D_OWNER      (1u << 31)

#define NRX 12u                   /* > LLP_SDIO_PKT_MAX / LLP_SDIO_BUF: a whole packet, and some */

/* The channel's block: everything the DMA may touch, in one allocation. */
typedef struct {
    dma_desc_t rxd[NRX];
    dma_desc_t txd;
    uint8_t    rxbuf[NRX][LLP_SDIO_BUF] __attribute__((aligned(4)));
    uint8_t    txbuf[2][LLP_SDIO_PKT_MAX] __attribute__((aligned(4)));
} chan_block_t;
#define BLOCK_PAGES ((sizeof(chan_block_t) + PAGE_SIZE - 1u) / PAGE_SIZE)

static chan_block_t *g_blk;
#define g_rxd   (g_blk->rxd)
#define g_txd   (g_blk->txd)
static uint32_t   g_rx_head;      /* the oldest loaded buffer */
static uint32_t   g_rx_tail;      /* the last loaded buffer: its next is NULL */

static bool       g_tx_busy;
static uint32_t   g_tx_total;     /* bytes offered since the channel's reset, mod 2^20 */

static bool       g_started;
static uint8_t    g_gen;
static uint32_t   g_timing;
static uint8_t    g_shared[8];

static struct {
    uint32_t rx_pkts, rx_bytes, rx_too_big, tx_pkts, tx_bytes, resets;
} g_st;

static void put_shared(void) {
    REG(SLCHOST_CONF_W0) = (uint32_t)g_shared[0] | ((uint32_t)g_shared[1] << 8) |
                           ((uint32_t)g_shared[2] << 16) | ((uint32_t)g_shared[3] << 24);
    REG(SLCHOST_CONF_W1) = (uint32_t)g_shared[4] | ((uint32_t)g_shared[5] << 8) |
                           ((uint32_t)g_shared[6] << 16) | ((uint32_t)g_shared[7] << 24);
}

void sdio_slave_set_timing(uint32_t t) {
    /* The FRC_* fields force each of five lines' edge; all five together. */
    static const uint8_t sdio20[4] = { 0x1F, 0x1F, 0x00, 0x00 };   /* send on the rising edge */
    static const uint8_t possamp[4] = { 0x1F, 0x00, 0x1F, 0x00 };  /* sample on the rising edge */
    g_timing = t & 3u;
    uint32_t v = REG(SLCHOST_CONF) & ~0xFFFFFu;
    v |= ((uint32_t)(sdio20[g_timing] ^ 0x1Fu) << SLCHOST_FRC_SDIO11_S) |
         ((uint32_t)sdio20[g_timing] << SLCHOST_FRC_SDIO20_S) |
         ((uint32_t)(possamp[g_timing] ^ 0x1Fu) << SLCHOST_FRC_NEG_SAMP_S) |
         ((uint32_t)possamp[g_timing] << SLCHOST_FRC_POS_SAMP_S);
    REG(SLCHOST_CONF) = v;
}

/* --- Receive ring ------------------------------------------------------------- */

static void token_inc(void) {
    /* WDATA and INC_MORE in one write (sdio_slave_ll.h). */
    REG(SLC_TOKEN1) = (1u << SLC_TOKEN_WDATA_S) | SLC_TOKEN_INC_MORE;
}

static void rx_ring_reset(void) {
    REG(SLC_TX_LINK) = SLC_LINK_STOP;
    REG(SLC_CONF0) |= SLC0_TX_RST;
    REG(SLC_CONF0) &= ~SLC0_TX_RST;
    for (uint32_t i = 0; i < NRX; i++) {
        g_rxd[i].ctl = D_SIZE(LLP_SDIO_BUF) | D_OWNER;
        g_rxd[i].buf = (uint32_t)(uintptr_t)g_blk->rxbuf[i];
        g_rxd[i].next = (i + 1u < NRX) ? (uint32_t)(uintptr_t)&g_rxd[i + 1u] : 0u;
    }
    g_rx_head = 0;
    g_rx_tail = NRX - 1u;
    REG(SLC_TX_LINK_ADDR) = (uint32_t)(uintptr_t)&g_rxd[0];
    REG(SLC_TX_LINK) = SLC_LINK_START;
    REG(SLC_TOKEN1) = (0u << SLC_TOKEN_WDATA_S) | SLC_TOKEN_WR;
    for (uint32_t i = 0; i < NRX; i++) token_inc();
}

/* Buffer `i`, consumed, goes back to the DMA at the end of the chain. */
static void rx_reload(uint32_t i) {
    g_rxd[i].ctl = D_SIZE(LLP_SDIO_BUF) | D_OWNER;
    g_rxd[i].next = 0u;
    g_rxd[g_rx_tail].next = (uint32_t)(uintptr_t)&g_rxd[i];
    g_rx_tail = i;
    REG(SLC_TX_LINK) = SLC_LINK_RESTART;
    token_inc();
}

int sdio_slave_recv(uint8_t *buf, uint32_t cap) {
    if (!g_started) return 0;
    /* Find a whole packet: consecutive buffers the DMA has given back, up to
     * one with eof. A packet still arriving is left alone. */
    uint32_t n = 0, total = 0;
    bool whole = false;
    while (n < NRX) {
        uint32_t c = g_rxd[(g_rx_head + n) % NRX].ctl;
        if (c & D_OWNER) break;
        total += D_GET_LEN(c);
        n++;
        if (c & D_EOF) { whole = true; break; }
    }
    if (!whole) return 0;
    bool fits = total <= cap;
    uint32_t off = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = (g_rx_head + k) % NRX;
        uint32_t len = D_GET_LEN(g_rxd[i].ctl);
        if (fits) memcpy(buf + off, g_blk->rxbuf[i], len);
        off += len;
        rx_reload(i);
    }
    g_rx_head = (g_rx_head + n) % NRX;
    REG(SLC_INT_CLR) = SLC_INT_TX_DONE;
    if (!fits) { g_st.rx_too_big++; return -1; }
    g_st.rx_pkts++;
    g_st.rx_bytes += total;
    return (int)total;
}

/* --- Send --------------------------------------------------------------------- */

static void tx_reset(void) {
    REG(SLC_RX_LINK) = SLC_LINK_STOP;
    REG(SLC_CONF0) |= SLC0_RX_RST;
    REG(SLC_CONF0) &= ~SLC0_RX_RST;
    REG(SLC_INT_CLR) = SLC_INT_RX_EOF;
    g_tx_busy = false;
    g_tx_total = 0;
    REG(SLC_LEN_CONF) = 0u | SLC_LEN_WR;
    REG(SLCHOST_INT_CLR) = SLCHOST_NEW_PACKET;
}

bool sdio_slave_send_ready(void) { return g_started && !g_tx_busy; }

uint8_t *sdio_slave_tx_buf(unsigned i) { return (g_blk && i < 2u) ? g_blk->txbuf[i] : NULL; }

int sdio_slave_send(const uint8_t *buf, uint32_t len) {
    if (!g_started || g_tx_busy || len == 0 || len > LLP_SDIO_PKT_MAX) return -1;
    if (buf != g_blk->txbuf[0] && buf != g_blk->txbuf[1]) return -1;   /* outside the fence */
    g_txd.ctl = D_SIZE(LLP_SDIO_ALIGN(len)) | D_LEN(len) | D_EOF | D_OWNER;
    g_txd.buf = (uint32_t)(uintptr_t)buf;
    g_txd.next = 0u;
    REG(SLC_RX_LINK) = SLC_LINK_STOP;
    REG(SLC_CONF0) |= SLC0_RX_RST;
    REG(SLC_CONF0) &= ~SLC0_RX_RST;
    REG(SLC_INT_CLR) = SLC_INT_RX_EOF;
    REG(SLC_RX_LINK_ADDR) = (uint32_t)(uintptr_t)&g_txd;
    REG(SLC_RX_LINK) = SLC_LINK_START;
    /* Only now does the host learn there is something to read. */
    g_tx_total = (g_tx_total + len) & LLP_SDIO_LEN_MASK;
    REG(SLC_LEN_CONF) = g_tx_total | SLC_LEN_WR;
    g_tx_busy = true;
    g_st.tx_pkts++;
    g_st.tx_bytes += len;
    return 0;
}

/* --- Channel ------------------------------------------------------------------ */

static void channel_reset(void) {
    REG(SLCHOST_CONF_W7) &= ~(0xFFu << SLCHOST_SLV_INT_S);
    REG(SLC_INT_CLR) = SLC_INT_FRHOST_M;
    tx_reset();
    rx_ring_reset();
    /* The generation last: the host waits for it to change before it trusts
     * the counters. */
    do { g_gen++; } while (g_gen == 0);
    g_shared[LLP_SDIO_SH_GEN] = g_gen;
    put_shared();
}

bool sdio_slave_poll(void) {
    if (!g_started) return false;
    uint32_t raw = REG(SLC_INT_RAW);
    if (g_tx_busy && (raw & SLC_INT_RX_EOF)) {
        REG(SLC_INT_CLR) = SLC_INT_RX_EOF;
        g_tx_busy = false;
    }
    if (raw & SLC_INT_FRHOST_M) {
        /* The host's bits are a plain register byte (CONF_W7[15:8]) that
         * keeps raising the interrupt while set: clear it, then the
         * interrupt. */
        bool reset = (raw & LLP_SDIO_SLV_RESET) != 0;
        REG(SLCHOST_CONF_W7) &= ~(0xFFu << SLCHOST_SLV_INT_S);
        REG(SLC_INT_CLR) = raw & SLC_INT_FRHOST_M;
        if (reset) {
            channel_reset();
            g_st.resets++;
            return true;
        }
    }
    return false;
}

void sdio_slave_set_link(uint8_t up, int8_t rssi) {
    g_shared[LLP_SDIO_SH_LINK] = up;
    g_shared[LLP_SDIO_SH_RSSI] = (uint8_t)rssi;
    if (g_started) put_shared();
}

int sdio_slave_start(void) {
    if (g_started) return 0;
    if (!g_blk) {
        g_blk = (chan_block_t *)palloc_pages(BLOCK_PAGES);
        if (!g_blk) return -1;
        memset(g_blk, 0, sizeof *g_blk);
    }

    /* The fence (see the top of this file), before the DMA can run. */
    uintptr_t lo = (uintptr_t)g_blk, hi = lo + sizeof *g_blk - 1u;
    REG(HP_APM_REGION_START(APM_REGION_SDIO)) = (uint32_t)lo;
    REG(HP_APM_REGION_END(APM_REGION_SDIO)) = (uint32_t)hi;
    REG(HP_APM_REGION_ATTR(APM_REGION_SDIO)) = APM_REE1_RW;
    REG(HP_APM_FILTER_EN) |= 1u << APM_REGION_SDIO;
    REG(TEE_M_MODE_CTRL(TEE_MASTER_SDIO_SLAVE)) = TEE_MODE_REE1;

    /* The pads: IO_MUX function 0 is the slave's own; input enabled, the
     * strongest drive, pull-ups on all but the clock (the boards have 51 kOhm
     * externally as well). */
    for (uint32_t g = SDIO_FIRST_GPIO; g <= SDIO_LAST_GPIO; g++) {
        uint32_t v = (0u << IO_MUX_MCU_SEL_S) | IO_MUX_FUN_IE | (3u << IO_MUX_FUN_DRV_S);
        if (g != SDIO_CLK_GPIO) v |= IO_MUX_FUN_WPU;
        REG(IO_MUX_GPIO(g)) = v;
    }

    REG(PCR_SDIO_SLAVE_CONF) |= PCR_SDIO_CLK_EN;
    REG(PCR_SDIO_SLAVE_CONF) |= PCR_SDIO_RST_EN;
    REG(PCR_SDIO_SLAVE_CONF) &= ~PCR_SDIO_RST_EN;

    /* sdio_slave_ll_init(): no CPU interrupts, auto write-back of send
     * descriptors, no loop tests, no stitching, PKT_LEN kept until written. */
    REG(SLC_INT_ENA) = 0;
    uint32_t c0 = REG(SLC_CONF0);
    c0 |= SLC0_RX_AUTO_WRBACK;
    c0 &= ~(SLC0_TOKEN_AUTO_CLR | SLC0_RX_LOOP_TEST | SLC0_TX_LOOP_TEST);
    REG(SLC_CONF0) = c0;
    REG(SLC_CONF1) &= ~(SLC0_RX_STITCH_EN | SLC0_TX_STITCH_EN | SLC0_LEN_AUTO_CLR);
    REG(SLC_RX_DSCR_CONF) |= SLC0_TOKEN_NO_REPLACE;

    uint32_t h = REG(HINF_CFG_DATA1);
    h = (h & ~HINF_SDIO_VER_M) | (0x232u << HINF_SDIO_VER_S) | HINF_HIGHSPEED;
    REG(HINF_CFG_DATA1) = h;
    sdio_slave_set_timing(g_timing);

    g_shared[LLP_SDIO_SH_MAGIC0] = 'L';
    g_shared[LLP_SDIO_SH_MAGIC1] = 'L';
    g_shared[LLP_SDIO_SH_MAGIC2] = 'P';
    g_shared[LLP_SDIO_SH_NBUF] = (uint8_t)NRX;
    g_shared[LLP_SDIO_SH_BUF64] = (uint8_t)(LLP_SDIO_BUF / 64u);
    random_bytes(&g_gen, 1);
    g_started = true;
    channel_reset();
    REG(HINF_CFG_DATA1) |= HINF_IOREADY1;
    return 0;
}

void sdio_slave_report(void) {
    if (!g_started) { cprintf("sdio slave: not started (`llp radio` starts it)\n"); return; }
    static const char *const tn[4] = { "send+/sample+", "send+/sample-", "send-/sample+", "send-/sample-" };
    uint32_t loaded = 0;
    for (uint32_t i = 0; i < NRX; i++) if (g_rxd[i].ctl & D_OWNER) loaded++;
    cprintf("sdio slave: gen %u, timing %s, %u/%u receive buffers loaded, send %s (PKT_LEN %u)\n",
            g_gen, tn[g_timing], (unsigned)loaded, (unsigned)NRX, g_tx_busy ? "busy" : "idle",
            (unsigned)g_tx_total);
    cprintf("  in: %u packets, %u bytes, %u too big; out: %u packets, %u bytes; host resets %u\n",
            (unsigned)g_st.rx_pkts, (unsigned)g_st.rx_bytes, (unsigned)g_st.rx_too_big,
            (unsigned)g_st.tx_pkts, (unsigned)g_st.tx_bytes, (unsigned)g_st.resets);
    cprintf("  shared: link %u rssi %d; raw int 0x%08x\n", g_shared[LLP_SDIO_SH_LINK],
            (int)(int8_t)g_shared[LLP_SDIO_SH_RSSI], (unsigned)REG(SLC_INT_RAW));
}

#else

void sdio_slave_report(void) { cprintf("sdio slave: this board has none\n"); }

#endif
