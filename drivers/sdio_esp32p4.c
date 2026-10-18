/* The ESP32-P4's SDIO host: the SD/MMC controller's slot 1, wired to the
 * board's ESP32-C6. The controller
 * itself belongs to drivers/sdmmc_esp32p4.c (slot 0, the microSD card); this
 * file is a client of it through drivers/sdhost_esp32p4.h, and everything it
 * does is under that controller's lock with card 1 selected.
 *
 * What it does: the SDIO bring-up (CMD5 until the card is ready, CMD3, CMD7),
 * CMD52 (one byte, any function), CMD53 (byte and block mode, through the
 * controller's DMA), the card's CIS (who it is), 4-bit mode, function 1, and
 * the C6's reset line, the card interrupt, and the host side of LLP's SDIO
 * binding.
 *
 * ## Proven against Espressif's own slave first
 *
 * D7 of the plan: bring the host up against a known-good slave before ours
 * exists. The C6's boot ROM has one: in download mode (`reboot download` on a
 * LugalOS C6, 49.5) it runs an SDIO slave -- its banner says
 * DOWNLOAD(USB/UART0/SDIO_FEI_FEO) -- so a host bug cannot hide behind a slave
 * bug. FEI_FEO is the ROM's timing on this module (MTMS and MTDI both low):
 * the slave samples and drives on the falling edge (C6 datasheet Table 3-4).
 *
 * Register shapes: the P4 TRM and IDF's hw_ver1 gpio_reg.h (FUNCn_OUT_SEL_CFG:
 * [8:0] signal, bit 10 OEN_SEL = 0 for "the peripheral drives output enable").
 * The SDIO protocol: the SD Specifications Part E1, SDIO Simplified 3.00. */

#include "drivers/sdio_esp32p4.h"
#include "drivers/sdhost_esp32p4.h"
#include "kernel/llp_sdio.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/devirq.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "arch/esp32p4_intr.h"   /* esp32p4_dcache_*, ESP32P4_L1_CACHE_LINE */
#include "arch/trap.h"           /* arch_irq_enable */
#include "fs/vfs.h"
#include "lugalos_config.h"
#include <string.h>

#if CONFIG_ENABLE_SDMMC && defined(CONFIG_SDIO_CLK_GPIO)

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* --- Pads and the GPIO matrix ----------------------------------------------- */

#define GPIO_BASE           0x500E0000UL
#define IOMUX_BASE          0x500E1000UL
#define IOMUX_PAD(n)        (IOMUX_BASE + 0x4 + 4u * (uint32_t)(n))
#define  IOMUX_MCU_SEL_S        12
#define  IOMUX_MCU_SEL_M        (7u << IOMUX_MCU_SEL_S)
#define  IOMUX_FUN_IE           (1u << 9)
#define  IOMUX_FUN_PU           (1u << 8)
#define  IOMUX_FUN_PD           (1u << 7)
#define  IOMUX_FUN_DRV_S        10
#define  IOMUX_FUN_DRV_M        (3u << IOMUX_FUN_DRV_S)
#define  IOMUX_FUNC_GPIO        1u
#define GPIO_IN_SEL(sig)    (GPIO_BASE + 0x158 + 4u * (uint32_t)(sig))
#define  GPIO_IN_SEL_ROUTE      (1u << 7)
#define GPIO_OUT_SEL(n)     (GPIO_BASE + 0x558 + 4u * (uint32_t)(n))
#define  GPIO_OUT_SEL_GPIO      256u
#define  GPIO_OUT_OEN_SEL       (1u << 10)   /* 1 = GPIO_ENABLE decides; 0 = the peripheral */
#define GPIO_OUT0_W1TC      (GPIO_BASE + 0x0C)
#define GPIO_OUT1_W1TC      (GPIO_BASE + 0x18)
#define GPIO_ENABLE0_W1TS   (GPIO_BASE + 0x24)
#define GPIO_ENABLE0_W1TC   (GPIO_BASE + 0x28)
#define GPIO_ENABLE1_W1TS   (GPIO_BASE + 0x30)
#define GPIO_ENABLE1_W1TC   (GPIO_BASE + 0x34)
#define GPIO_MATRIX_CONST_ZERO  0x3Eu
#define GPIO_MATRIX_CONST_ONE   0x3Fu

/* Slot 1's signals (IDF esp32p4 gpio_sig_map.h, SD_CARD_*_2_PAD_*). */
#define SIG_CLK      0u
#define SIG_CMD      1u
#define SIG_D0       2u              /* D0..D3: 2..5 */
#define SIG_DETECT_N 127u
#define SIG_INT_N    129u
#define SIG_WP       131u

static void pad_gpio(uint32_t gpio, bool pull_up) {
    uint32_t v = REG(IOMUX_PAD(gpio));
    v &= ~(IOMUX_MCU_SEL_M | IOMUX_FUN_PU | IOMUX_FUN_PD | IOMUX_FUN_DRV_M);
    v |= (IOMUX_FUNC_GPIO << IOMUX_MCU_SEL_S) | IOMUX_FUN_IE | (3u << IOMUX_FUN_DRV_S);
    if (pull_up) v |= IOMUX_FUN_PU;
    REG(IOMUX_PAD(gpio)) = v;
}

/* A bidirectional line: out through the matrix with the controller's own
 * output enable, and in from the same pad. */
static void route_bidir(uint32_t gpio, uint32_t sig) {
    pad_gpio(gpio, true);
    REG(GPIO_OUT_SEL(gpio)) = sig;                       /* OEN_SEL 0: the peripheral's */
    REG(GPIO_IN_SEL(sig)) = gpio | GPIO_IN_SEL_ROUTE;
}

static void sdio_pads_init(void) {
    pad_gpio(CONFIG_SDIO_CLK_GPIO, false);
    REG(GPIO_OUT_SEL(CONFIG_SDIO_CLK_GPIO)) = SIG_CLK;
    route_bidir(CONFIG_SDIO_CMD_GPIO, SIG_CMD);
    route_bidir(CONFIG_SDIO_D0_GPIO, SIG_D0 + 0u);
    route_bidir(CONFIG_SDIO_D1_GPIO, SIG_D0 + 1u);
    route_bidir(CONFIG_SDIO_D2_GPIO, SIG_D0 + 2u);
    route_bidir(CONFIG_SDIO_D3_GPIO, SIG_D0 + 3u);
    /* Card detect (active low) and write protect are constants: the card is
     * soldered down. INT_N stays 1 -- the SDIO interrupt is sensed on DAT1 by
     * the controller itself. */
    REG(GPIO_IN_SEL(SIG_DETECT_N)) = GPIO_MATRIX_CONST_ZERO | GPIO_IN_SEL_ROUTE;
    REG(GPIO_IN_SEL(SIG_WP))       = GPIO_MATRIX_CONST_ZERO | GPIO_IN_SEL_ROUTE;
    REG(GPIO_IN_SEL(SIG_INT_N))    = GPIO_MATRIX_CONST_ONE  | GPIO_IN_SEL_ROUTE;
}

/* --- The C6's reset -------------------------------------------------------------
 * Open-drain by construction: the output latch is 0 and only the enable is
 * toggled, so the pin either pulls EN low or floats to the module's pull-up.
 * Nothing on this net is ever driven high (plan §5.1). */
void sdio_c6_reset(uint32_t hold_ms) {
    const uint32_t g = (uint32_t)CONFIG_C6_CHIP_PU_GPIO;
    const uint32_t bit = 1u << (g < 32u ? g : (g - 32u));
    pad_gpio(g, false);
    REG(GPIO_OUT_SEL(g)) = GPIO_OUT_SEL_GPIO | GPIO_OUT_OEN_SEL;
    if (g < 32u) {
        REG(GPIO_OUT0_W1TC) = bit;
        REG(GPIO_ENABLE0_W1TS) = bit;          /* EN low */
    } else {
        REG(GPIO_OUT1_W1TC) = bit;
        REG(GPIO_ENABLE1_W1TS) = bit;          /* EN low */
    }
    time_delay_us(hold_ms * 1000u);
    if (g < 32u) {
        REG(GPIO_ENABLE0_W1TC) = bit;          /* released: the pull-up raises it */
    } else {
        REG(GPIO_ENABLE1_W1TC) = bit;          /* released: the pull-up raises it */
    }
}

/* --- CMD52 / CMD53 ---------------------------------------------------------------- */

#define CMD52_WRITE    (1u << 31)
#define CMD52_RAW      (1u << 27)
#define CMD53_WRITE    (1u << 31)
#define CMD53_BLOCK    (1u << 27)
#define CMD53_INCR     (1u << 26)
#define R5_ERRORS      0xCB00u       /* COM_CRC, ILLEGAL, ERROR, FUNCTION_NUMBER, OUT_OF_RANGE */

/* guarded by: the SD host's lock (sdhost_acquire(1)), which every *_locked()
 * function runs under. */
static sdio_info_t g_info;
static sdhost_desc_t g_desc __attribute__((aligned(ESP32P4_L1_CACHE_LINE)));
static uint8_t g_buf[SDIO_XFER_MAX] __attribute__((aligned(ESP32P4_L1_CACHE_LINE)));

static int cmd52_locked(bool write, uint32_t fn, uint32_t addr, uint8_t in, uint8_t *out) {
    uint32_t arg = (write ? CMD52_WRITE : 0u) | ((fn & 7u) << 28) | ((addr & 0x1ffffu) << 9) | in;
    uint32_t r = 0, st = 0;
    if (sdhost_cmd(52, arg, SDHOST_RESP_R1, 0, &r, &st) != 0) { g_info.errors++; return -1; }
    if (r & R5_ERRORS) { g_info.errors++; return -1; }
    if (out) *out = (uint8_t)r;
    return 0;
}

int sdio_cmd52(bool write, uint32_t fn, uint32_t addr, uint8_t in, uint8_t *out) {
    sdhost_acquire(1);
    int rc = cmd52_locked(write, fn, addr, in, out);
    sdhost_release();
    return rc;
}

/* A data phase that did not finish leaves the controller waiting for it, and
 * the next command on either slot -- the microSD's too -- would meet that. The
 * SDIO way out: an I/O abort of the function (CCCR 0x06), sent as a
 * stop/abort command so that it does not wait for the data that is not
 * coming, then the FIFO and the DMA. */
static void abort_locked(uint32_t fn) {
    uint32_t r = 0, st = 0;
    (void)sdhost_cmd(52, CMD52_WRITE | (0x06u << 9) | (fn & 7u), SDHOST_RESP_R1, SDHOST_STOP_ABORT, &r, &st);
    sdhost_data_reset();
}

/* `len` bytes at `addr` of function `fn`: byte mode up to the function's block
 * size, block mode (whole blocks) above it. Incrementing address. */
static int cmd53_locked(bool write, uint32_t fn, uint32_t addr, uint8_t *data, uint32_t len) {
    if (len == 0 || len > SDIO_XFER_MAX) return -1;
    uint32_t bs = g_info.block_size ? g_info.block_size : 64u;
    bool block = len > bs;
    if (block && len % bs) return -1;
    uint32_t count = block ? len / bs : (len == 512u ? 0u : len);
    uint32_t arg = (write ? CMD53_WRITE : 0u) | ((fn & 7u) << 28) | (block ? CMD53_BLOCK : 0u) |
                   CMD53_INCR | ((addr & 0x1ffffu) << 9) | (count & 0x1ffu);
    uint32_t span = (len + ESP32P4_L1_CACHE_LINE - 1u) & ~(ESP32P4_L1_CACHE_LINE - 1u);

    if (write) {
        memcpy(g_buf, data, len);
        esp32p4_dcache_writeback((uintptr_t)g_buf, span);
    } else {
        esp32p4_dcache_invalidate((uintptr_t)g_buf, span);
    }
    sdhost_dma_arm(&g_desc, g_buf, len, block ? bs : len);

    uint32_t r = 0, st = 0;
    if (sdhost_cmd(53, arg, SDHOST_RESP_R1, SDHOST_DATA | (write ? SDHOST_WRITE : 0u), &r, &st) != 0 ||
        (r & R5_ERRORS)) {
        g_info.errors++;
        printk("[SDIO] CMD53 %s fn%u 0x%x len %u: status 0x%x R5 0x%x\n", write ? "write" : "read",
               (unsigned)fn, (unsigned)addr, (unsigned)len, (unsigned)st, (unsigned)r);
        return -1;
    }
    if (sdhost_dma_wait(addr, write ? "sdio write" : "sdio read") != 0) {
        g_info.errors++;
        abort_locked(fn);
        return -1;
    }
    if (!write) {
        esp32p4_dcache_invalidate((uintptr_t)g_buf, span);
        memcpy(data, g_buf, len);
    }
    return 0;
}

int sdio_cmd53(bool write, uint32_t fn, uint32_t addr, uint8_t *data, uint32_t len) {
    sdhost_acquire(1);
    int rc = cmd53_locked(write, fn, addr, data, len);
    sdhost_release();
    return rc;
}

/* --- The CIS: who the card says it is ---------------------------------------------- */

static uint32_t rd24(uint32_t fn, uint32_t addr) {
    uint8_t a = 0, b = 0, c = 0;
    cmd52_locked(false, fn, addr, 0, &a);
    cmd52_locked(false, fn, addr + 1u, 0, &b);
    cmd52_locked(false, fn, addr + 2u, 0, &c);
    return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16);
}

/* Walks a CIS chain for the MANFID tuple (0x20): vendor and device. Bounded:
 * a card that returns garbage cannot keep this walking. */
static void cis_manfid(uint32_t cis, uint16_t *vendor, uint16_t *device) {
    for (int t = 0; t < 32 && cis; t++) {
        uint8_t code = 0, link = 0;
        if (cmd52_locked(false, 0, cis, 0, &code) != 0 || code == 0xFF) return;
        if (cmd52_locked(false, 0, cis + 1u, 0, &link) != 0) return;
        if (code == 0x20 && link >= 4) {
            uint8_t b[4];
            for (uint32_t i = 0; i < 4; i++) cmd52_locked(false, 0, cis + 2u + i, 0, &b[i]);
            *vendor = (uint16_t)(b[0] | (b[1] << 8));
            *device = (uint16_t)(b[2] | (b[3] << 8));
            return;
        }
        if (link == 0xFF) return;
        cis += 2u + link;
    }
}

/* --- Bring-up ----------------------------------------------------------------------- */

static bool g_hosted_tried;   /* guarded by: sdhost_acquire(1); hosted_detect_locked() ran since the last probe */

static int probe_locked(uint32_t khz) {
    memset(&g_info, 0, sizeof g_info);
    g_hosted_tried = false;
    if (sdhost_controller_init() != 0) return -1;
    sdio_pads_init();
    sdhost_set_width4(false);
    g_info.khz = sdhost_set_clock(400);
    uint32_t r = 0, st = 0;

    /* A card that was already brought up -- an address published, selected --
     * does not answer CMD5 at all: it is illegal outside the idle state (found
     * on the first re-probe, 2026-10-12). So every probe starts with an I/O
     * reset, RES (CCCR 0x06 bit 3), whose own response may never come and is
     * not waited for. On a card fresh from power-up it is a no-op. */
    (void)sdhost_cmd(52, CMD52_WRITE | (0x06u << 9) | 0x08u, SDHOST_RESP_R1, SDHOST_SEND_INIT, &r, &st);
    time_delay_us(2000);

    /* CMD5, the IO_SEND_OP_COND: first with no voltage window to read the
     * card's OCR (and preceded by the 80 init clocks), then with the window
     * until the ready bit (31) says the card has finished powering up. R4 has
     * no CRC. A card that does not answer CMD5 is not an SDIO card -- or is not
     * there, or is not running anything that serves SDIO. */
    if (sdhost_cmd(5, 0, SDHOST_RESP_R3, 0, &r, &st) != 0) {
        g_info.stage = 1;
        g_info.last_status = st;
        return -1;
    }
    g_info.ocr = r;
    uint64_t deadline = time_get_ms() + 1000u;
    for (;;) {
        if (sdhost_cmd(5, r & 0x00FFFF00u ? (r & 0x00FFFF00u) : 0x00FF8000u, SDHOST_RESP_R3, 0, &r, &st) != 0) {
            g_info.stage = 2;
            g_info.last_status = st;
            return -1;
        }
        if (r & 0x80000000u) break;
        if (time_get_ms() > deadline) { g_info.stage = 2; return -1; }
    }
    g_info.ocr = r;
    g_info.functions = (r >> 28) & 7u;
    g_info.memory = (r >> 27) & 1u;

    /* CMD3: the card publishes its address. CMD7: select it. */
    if (sdhost_cmd(3, 0, SDHOST_RESP_R1, 0, &r, &st) != 0) { g_info.stage = 3; g_info.last_status = st; return -1; }
    g_info.rca = r >> 16;
    if (sdhost_cmd(7, g_info.rca << 16, SDHOST_RESP_R1B, 0, &r, &st) != 0) {
        g_info.stage = 4;
        g_info.last_status = st;
        return -1;
    }

    /* The CCCR: SDIO revision, capabilities, the common CIS. */
    uint8_t rev = 0, sd = 0, cap = 0;
    if (cmd52_locked(false, 0, 0x00, 0, &rev) != 0) { g_info.stage = 5; return -1; }
    cmd52_locked(false, 0, 0x01, 0, &sd);
    cmd52_locked(false, 0, 0x08, 0, &cap);
    g_info.cccr_rev = rev;
    g_info.sd_rev = sd;
    g_info.caps = cap;
    g_info.cis = rd24(0, 0x09);
    cis_manfid(g_info.cis, &g_info.vendor, &g_info.device);

    /* Function 1 on, and wait for it to say it is ready. */
    uint8_t ioe = 0, ior = 0;
    cmd52_locked(false, 0, 0x02, 0, &ioe);
    cmd52_locked(true, 0, 0x02, (uint8_t)(ioe | 0x02u), NULL);
    deadline = time_get_ms() + 1000u;
    while (cmd52_locked(false, 0, 0x03, 0, &ior) == 0 && !(ior & 0x02u) && time_get_ms() < deadline) { }
    g_info.fn1_ready = (ior & 0x02u) != 0;

    /* The card interrupt: master enable and function 1's (CCCR 0x04, IENM
     * and IEN1). What raises it is the function's business (INT_ENA). */
    cmd52_locked(true, 0, 0x04, 0x03u, NULL);

    /* Function 1's block size: 512 (FBR1 0x110/0x111), read back. */
    cmd52_locked(true, 0, 0x110, 0x00, NULL);
    cmd52_locked(true, 0, 0x111, 0x02, NULL);
    uint8_t lo = 0, hi = 0;
    cmd52_locked(false, 0, 0x110, 0, &lo);
    cmd52_locked(false, 0, 0x111, 0, &hi);
    g_info.block_size = (uint32_t)lo | ((uint32_t)hi << 8);

    /* 4-bit, unless the card is low-speed-only without 4-bit support (CCCR
     * capability bits 6 LSC and 7 4BLS). Bus interface control, 0x07[1:0] = 2. */
    if (!((cap & 0x40u) && !(cap & 0x80u))) {
        uint8_t bic = 0;
        cmd52_locked(false, 0, 0x07, 0, &bic);
        if (cmd52_locked(true, 0, 0x07, (uint8_t)((bic & ~3u) | 2u), NULL) == 0) {
            sdhost_set_width4(true);
            g_info.width = 4;
        }
    }
    if (!g_info.width) g_info.width = 1;

    /* Above 25 MHz the card must be in high-speed timing first: CCCR 0x13,
     * EHS (bit 1), if it says it supports it (SHS, bit 0). */
    if (khz > 25000u) {
        uint8_t bss = 0;
        if (cmd52_locked(false, 0, 0x13, 0, &bss) == 0 && (bss & 0x01u) &&
            cmd52_locked(true, 0, 0x13, (uint8_t)(bss | 0x02u), NULL) == 0) {
            g_info.high_speed = true;
        } else {
            khz = 25000u;
        }
    }
    g_info.khz = sdhost_set_clock(khz);
    g_info.stage = 9;
    g_info.present = true;
    return 0;
}

static void hosted_detect_locked(void);

int sdio_probe(uint32_t khz) {
    if (!khz) khz = (uint32_t)CONFIG_SDIO_FREQ_KHZ;
    sdhost_acquire(1);
    int rc = probe_locked(khz);
    if (rc == 0 && g_info.fn1_ready) hosted_detect_locked();
    sdhost_release();
    return rc;
}

const sdio_info_t *sdio_info(void) { return &g_info; }

/* --- LLP's SDIO binding, host side ------------------------------------------------------
 * kernel/include/kernel/llp_sdio.h is the contract; drivers/sdio_slave_esp32c6.c
 * the other half. The host keeps two counters against the slave's: receive
 * buffers it has used (TOKEN counts those loaded) and bytes it has read
 * (PKT_LEN counts those offered). Both start from zero whenever the slave's
 * generation changes -- after the reset this side asks for, or after the C6
 * restarted by itself. */

static struct {
    bool     up;                /* an LLP slave answered and the counters are in step */
    bool     need_reset;        /* a transfer failed: start the channel over */
    uint8_t  gen;
    uint32_t used;              /* receive buffers written into, mod 4096 */
    uint32_t read;              /* bytes read, mod 2^20 */
    uint32_t token, pkt_len;    /* the slave's, as last polled */
    uint8_t  shared[8];
    uint32_t polls, writes, reads, wbytes, rbytes, no_room, resyncs, failures, irqs;
} g_llp;   /* guarded by: sdhost_acquire(1) */

/* The card interrupt. The handler masks it and wakes the pump; the next poll
 * clears the slave's bit, then the controller's, reads the status and
 * unmasks -- in that order, so a change during the read raises it again. */
#define SDHOST_INTR_SOURCE  23u            /* ETS_SDIO_HOST_INTR_SOURCE */
#define CARD_INT            (1u << 17)     /* RINTSTS: card 1's SDIO interrupt */
/* interrupt-shared: sdhost_isr() masks the card interrupt, then sets
 * g_irq_seen; the pump clears it under sdhost_acquire() before it clears the
 * slave's and the controller's bits and unmasks, so no set is lost. The pump
 * sets g_irq_waker once, when it starts. */
static volatile bool g_irq_seen;
static volatile int  g_irq_waker = -1;
static bool          g_irq_routed;

static void sdhost_isr(void *ctx) {
    (void)ctx;
    sdhost_sdio_irq(1, false);
    g_irq_seen = true;
    if (g_irq_waker >= 0) task_unblock(g_irq_waker);
}

static void irq_route(void) {
    if (g_irq_routed) return;
    if (esp32p4_intmtx_route(SDHOST_INTR_SOURCE, ESP32P4_CLIC_IRQ_SDHOST) != 0 ||
        devirq_attach(ESP32P4_CLIC_IRQ_SDHOST, sdhost_isr, NULL) != 0) {
        printk("[SDIO] card interrupt not routed: the LLP pump polls\n");
        return;
    }
    arch_irq_enable(ESP32P4_CLIC_IRQ_SDHOST);
    g_irq_routed = true;
}

void sdio_llp_set_waker(int pid) { g_irq_waker = pid; }
bool sdio_llp_irq_pending(void) { return g_irq_seen; }

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One packet through the FIFO: whole blocks while 512 or more remain, then the
 * rest in byte mode, each CMD53 addressed so that the slave sees how much is
 * still to come (FIFO_END - remaining). When a byte-mode transfer is not a
 * multiple of 4 (`esp-hosted-mcu` control packets), round the CMD53 byte count
 * up to a multiple of 4 while keeping `FIFO_END - rem` as the address (matching
 * `H_SDIO_RX/TX_LEN_TO_TRANSFER` in `esp-hosted-mcu`). */
static int fifo_xfer_locked(bool write, uint8_t *buf, uint32_t len) {
    uint32_t done = 0;
    while (done < len) {
        uint32_t rem = len - done;
        uint32_t n = rem >= g_info.block_size ? (rem / g_info.block_size) * g_info.block_size : rem;
        uint32_t xfer = (n + 3u) & ~3u;
        if (xfer > SDIO_XFER_MAX) {
            xfer = SDIO_XFER_MAX;
            n = xfer;
        }
        if (cmd53_locked(write, LLP_SDIO_FN, LLP_SDIO_FIFO_END - rem, buf + done, xfer) != 0) return -1;
        done += n;
    }
    return 0;
}

/* guarded by: sdhost_acquire(1) for the esp-hosted state (the receive count,
 * buffer, carry and error text, written by the detection under it and by
 * `sdio ota`); `sdio ota` also holds g_hosted_ota_busy, and the LLP pump
 * stands aside while it is set, so the few writes it makes between two holds
 * of the lock meet no other writer. */
static volatile bool g_hosted_ota_busy;
static uint32_t g_hosted_rx_bytes;

static int status_locked(void) {
    uint8_t st[LLP_SDIO_STATUS_LEN];
    if (cmd53_locked(false, LLP_SDIO_FN, LLP_SDIO_STATUS_ADDR, st, sizeof st) != 0) return -1;
    g_llp.polls++;
    g_llp.token = le32(st + (LLP_SDIO_REG_TOKEN - LLP_SDIO_STATUS_ADDR));
    g_llp.pkt_len = le32(st + (LLP_SDIO_REG_PKT_LEN - LLP_SDIO_STATUS_ADDR)) & LLP_SDIO_LEN_MASK;
    memcpy(g_llp.shared, st + (LLP_SDIO_REG_SHARED - LLP_SDIO_STATUS_ADDR), 8);
    return 0;
}

static bool magic_ok(void) {
    return g_llp.shared[LLP_SDIO_SH_MAGIC0] == 'L' && g_llp.shared[LLP_SDIO_SH_MAGIC1] == 'L' &&
           g_llp.shared[LLP_SDIO_SH_MAGIC2] == 'P' && g_llp.shared[LLP_SDIO_SH_GEN] != 0;
}

/* The slave's counters restarted (its generation is new): start ours over. */
static void adopt_gen(void) {
    g_llp.gen = g_llp.shared[LLP_SDIO_SH_GEN];
    g_llp.used = 0;
    g_llp.read = 0;
    g_llp.up = true;
    /* A restarted slave has forgotten which bits raise the card interrupt. */
    (void)cmd52_locked(true, LLP_SDIO_FN, LLP_SDIO_REG_INT_ENA, LLP_SDIO_INT_CHANGED, NULL);
}

/* Asks the slave to start the channel over and waits for its new
 * generation. 0, or -1 (no LLP slave, or it did not answer in time). */
static int channel_reset_locked(void) {
    g_llp.up = false;
    if (status_locked() != 0 || !magic_ok()) return -1;
    uint8_t old = g_llp.shared[LLP_SDIO_SH_GEN];
    if (cmd52_locked(true, LLP_SDIO_FN, LLP_SDIO_REG_SLV_INT, LLP_SDIO_SLV_RESET, NULL) != 0) return -1;
    uint64_t deadline = time_get_ms() + 500u;
    while (time_get_ms() < deadline) {
        if (status_locked() == 0 && magic_ok() && g_llp.shared[LLP_SDIO_SH_GEN] != old) {
            adopt_gen();
            g_llp.need_reset = false;
            return 0;
        }
    }
    return -1;
}

int sdio_llp_open(void) {
    if (g_hosted_ota_busy) return -1;
    irq_route();
    sdhost_acquire(1);
    int rc = -1;
    if (g_info.present || probe_locked((uint32_t)CONFIG_SDIO_FREQ_KHZ) == 0) rc = channel_reset_locked();
    if (rc != 0) {
        if (g_info.present && g_info.fn1_ready && !g_info.hosted_present) hosted_detect_locked();
        g_llp.need_reset = true;
    }
    sdhost_release();
    return rc;
}

/* One status read. A slave that stopped answering is probed again, at most
 * once a second; one that restarted by itself is followed. 0 when up, 1 when
 * up with both counters just restarted (whatever was in flight is gone), -1
 * when down. */
int sdio_llp_poll(void) {
    static uint64_t next_probe;
    if (g_hosted_ota_busy) return -1;
    sdhost_acquire(1);
    int rc = -1;
    if (!g_info.present) {
        uint64_t now = time_get_ms();
        if (now < next_probe) goto out;
        next_probe = now + 1000u;
        if (probe_locked((uint32_t)CONFIG_SDIO_FREQ_KHZ) != 0) goto out;
        if (g_info.fn1_ready) hosted_detect_locked();
    }
    if (g_irq_seen) {
        g_irq_seen = false;
        g_llp.irqs++;
        (void)cmd52_locked(true, LLP_SDIO_FN, LLP_SDIO_REG_INT_CLR, LLP_SDIO_INT_CHANGED, NULL);
        sdhost_clear_status(CARD_INT);
    }
    if (status_locked() != 0) {
        g_llp.failures++;
        g_info.present = false;
        g_llp.up = false;
        goto out;
    }
    if (!magic_ok()) { g_llp.up = false; goto out; }
    if (g_llp.need_reset) {
        if (channel_reset_locked() != 0) goto out;
        g_llp.resyncs++;
        rc = 1;
        goto out;
    }
    rc = 0;
    if (!g_llp.up || g_llp.shared[LLP_SDIO_SH_GEN] != g_llp.gen) {
        if (g_llp.up) g_llp.resyncs++;
        adopt_gen();
        rc = 1;
    }
out:
    if (g_irq_routed && rc >= 0) sdhost_sdio_irq(1, true);
    sdhost_release();
    return rc;
}

bool sdio_llp_up(void) { return g_llp.up; }

void sdio_llp_resync(void) { g_llp.need_reset = true; }

uint32_t sdio_llp_readable(void) {
    return g_llp.up ? ((g_llp.pkt_len - g_llp.read) & LLP_SDIO_LEN_MASK) : 0u;
}

bool sdio_llp_room(uint32_t len) {
    uint32_t free = (LLP_SDIO_TOKEN(g_llp.token) - g_llp.used) & (LLP_SDIO_TOKEN_MOD - 1u);
    return g_llp.up && (len + LLP_SDIO_BUF - 1u) / LLP_SDIO_BUF <= free;
}

int sdio_llp_write(const uint8_t *pkt, uint32_t len) {
    if (!g_llp.up || len == 0 || len % 4u || len > LLP_SDIO_PKT_MAX) return -1;
    if (!sdio_llp_room(len)) { g_llp.no_room++; return -2; }
    sdhost_acquire(1);
    int rc = fifo_xfer_locked(true, (uint8_t *)(uintptr_t)pkt, len);
    if (rc == 0) {
        g_llp.used = (g_llp.used + (len + LLP_SDIO_BUF - 1u) / LLP_SDIO_BUF) & (LLP_SDIO_TOKEN_MOD - 1u);
        g_llp.writes++;
        g_llp.wbytes += len;
    } else {
        g_llp.failures++;
        g_llp.need_reset = true;
    }
    sdhost_release();
    return rc;
}

int sdio_llp_read(uint8_t *buf, uint32_t cap) {
    uint32_t n = sdio_llp_readable();
    if (n == 0) return 0;
    if (n > cap || n % 4u) {
        /* Not a packet this binding sends: start the channel over. */
        g_llp.failures++;
        g_llp.need_reset = true;
        return -1;
    }
    sdhost_acquire(1);
    int rc = fifo_xfer_locked(false, buf, n);
    sdhost_release();
    if (rc != 0) { g_llp.failures++; g_llp.need_reset = true; return -1; }
    g_llp.read = (g_llp.read + n) & LLP_SDIO_LEN_MASK;
    g_llp.reads++;
    g_llp.rbytes += n;
    return (int)n;
}

void sdio_llp_report(void) {
    cprintf("  sdio: %s, gen %u, slave link %u rssi %d, %u x %u B receive buffers\n",
            g_llp.up ? "up" : "down", g_llp.gen, g_llp.shared[LLP_SDIO_SH_LINK],
            (int)(int8_t)g_llp.shared[LLP_SDIO_SH_RSSI], g_llp.shared[LLP_SDIO_SH_NBUF],
            g_llp.shared[LLP_SDIO_SH_BUF64] * 64u);
    if (g_info.present && g_info.fn1_ready && !g_llp.up && !magic_ok()) {
        if (g_info.hosted_present) {
            cprintf("  sdio: esp-hosted-mcu v%u.%u.%u on fn1 (run `sdio ota [path]`)\n",
                    g_info.hosted_major, g_info.hosted_minor, g_info.hosted_patch);
        } else {
            cprintf("  sdio: non-LLP slave on fn1 (esp-hosted-mcu? run `sdio ota [path]`)\n");
        }
    }
    cprintf("  sdio: %u polls, %u card interrupts%s, out %u packets %u B (no room %u), in %u packets %u B, resyncs %u, failures %u, bus errors %u\n",
            (unsigned)g_llp.polls, (unsigned)g_llp.irqs, g_irq_routed ? "" : " (not routed)", (unsigned)g_llp.writes, (unsigned)g_llp.wbytes, (unsigned)g_llp.no_room,
            (unsigned)g_llp.reads, (unsigned)g_llp.rbytes, (unsigned)g_llp.resyncs, (unsigned)g_llp.failures,
            (unsigned)g_info.errors);
}

/* --- Shell & esp-hosted-mcu SDIO OTA ------------------------------------------------ */

static const char *const stage_names[] = {
    "not probed", "CMD5 unanswered (no SDIO slave running?)", "CMD5 never ready",
    "CMD3 failed", "CMD7 failed", "CCCR unreadable", "", "", "", "up",
};

void sdio_report(void) {
    const sdio_info_t *i = &g_info;
    cprintf("SDIO slot 1 (CLK=GPIO%d CMD=GPIO%d D0-D3=GPIO%d-%d, C6 reset GPIO%d): %s\n",
            CONFIG_SDIO_CLK_GPIO, CONFIG_SDIO_CMD_GPIO, CONFIG_SDIO_D0_GPIO, CONFIG_SDIO_D3_GPIO,
            CONFIG_C6_CHIP_PU_GPIO, i->stage < 10 ? stage_names[i->stage] : "?");
    if (i->stage == 1 || i->stage == 2 || i->stage == 3 || i->stage == 4)
        cprintf("  last status 0x%x\n", (unsigned)i->last_status);
    if (!i->present) return;
    cprintf("  OCR 0x%08x: %u function(s)%s, RCA 0x%04x\n", (unsigned)i->ocr, (unsigned)i->functions,
            i->memory ? " + memory" : "", (unsigned)i->rca);
    cprintf("  CCCR rev 0x%02x, SD rev 0x%02x, caps 0x%02x, CIS at 0x%05x: vendor 0x%04x device 0x%04x\n",
            i->cccr_rev, i->sd_rev, i->caps, (unsigned)i->cis, i->vendor, i->device);
    cprintf("  function 1 %s, block size %u; bus %u-bit at %u kHz%s; errors %u\n",
            i->fn1_ready ? "ready" : "NOT ready", (unsigned)i->block_size, (unsigned)i->width,
            (unsigned)i->khz, i->high_speed ? ", high speed" : "", (unsigned)i->errors);
    if (i->hosted_present) {
        cprintf("  slave: esp-hosted-mcu v%u.%u.%u (chip 0x%02x, %s mode) -- run `sdio ota [path]`\n",
                i->hosted_major, i->hosted_minor, i->hosted_patch,
                i->hosted_chip_id, i->hosted_sdio_mode ? "streaming" : "packet");
    } else if (i->llp_present || g_llp.up || magic_ok()) {
        cprintf("  slave: LugalOS LLP (gen %u)\n", g_llp.shared[LLP_SDIO_SH_GEN]);
    } else if (i->fn1_ready) {
        cprintf("  slave: non-LLP SDIO slave (esp-hosted-mcu? run `sdio probe` or `sdio ota [path]`)\n");
    }
}

/* accepted: a diagnostic command's state; two terminals running it at once mix their
 * output. */
static uint8_t g_scratch[SDIO_XFER_MAX];

void sdio_bench(uint32_t kilobytes, uint32_t fn, uint32_t addr, uint32_t chunk) {
    if (!g_info.present) { cprintf("sdio bench: probe first (`sdio probe`)\n"); return; }
    if (!chunk) chunk = g_info.block_size ? g_info.block_size : 512u;
    if (chunk > SDIO_XFER_MAX) chunk = SDIO_XFER_MAX;
    uint32_t total = kilobytes * 1024u, done = 0;
    uint64_t t0 = time_get_us();
    while (done < total) {
        if (sdio_cmd53(false, fn, addr, g_scratch, chunk) != 0) break;
        done += chunk;
    }
    uint64_t us = time_get_us() - t0;
    if (!us) us = 1;
    cprintf("sdio bench: %u KB of CMD53 reads (fn%u 0x%x, %u B each) in %u ms = %u KB/s, %u-bit at %u kHz%s\n",
            (unsigned)(done / 1024u), (unsigned)fn, (unsigned)addr, (unsigned)chunk, (unsigned)(us / 1000u),
            (unsigned)((uint64_t)done * 1000000u / 1024u / us), (unsigned)g_info.width, (unsigned)g_info.khz,
            done < total ? " -- STOPPED on an error" : "");
}

/* --- esp-hosted-mcu SDIO OTA client ------------------------------------------------
 *
 * Speaks just enough of Espressif's `esp-hosted-mcu` SDIO + Protocomm +
 * Protobuf RPC protocol to stream a standard ESP-IDF application image
 * (`lugalos-c6-app.bin`) into a factory C6's passive OTA partition and
 * activate it without touching the C6's UART/USB pins.
 *
 * Wire layers:
 *   1. SDIO Function 1 registers (`0x044` TOKEN, `0x050` INT_RAW, `0x060`
 *      PKT_LEN, `0x08C` HOST_TO_SLAVE_INTR bit 0 = ESP_OPEN_DATA_PATH,
 *      `0x0D4` INT_CLR) and packet FIFO ending at `0x1F800`, 1536-byte slave
 *      RX buffers.
 *   2. 12-byte `esp_payload_header`: byte 0 = `if_type` (3 = `ESP_SERIAL_IF`,
 *      5 = `ESP_PRIV_IF`), bytes 2..3 = payload length, bytes 4..5 = 12,
 *      bytes 6..7 = 16-bit byte sum over header + payload, bytes 8..9 = seq.
 *   3. Protocomm TLV inside `ESP_SERIAL_IF`: TLV 1 (`EPNAME` = `"RPCRsp"`),
 *      TLV 2 (`DATA` = Protobuf `Rpc` message).
 *   4. Protobuf `Rpc`: field 1 `msg_type` (1 = Req, 2 = Resp, 3 = Event),
 *      field 2 `msg_id` (`Req_OTABegin = 272`, `Req_OTAWrite = 273`,
 *      `Req_OTAEnd = 274`, `Req_OTAActivate = 266`; responses `+ 256`),
 *      field 3 `uid`, and submessage at field `msg_id`.
 */

#define HOSTED_RX_BUF_SIZE      1536u
#define HOSTED_REG_SLV_INTR     0x08Cu      /* SLCHOST_CONF_W7: bit 0 = ESP_OPEN_DATA_PATH */
#define HOSTED_OPEN_DATA_PATH   0x01u
#define HOSTED_IF_SERIAL        3u
#define HOSTED_IF_PRIV          5u
#define HOSTED_PRIV_EVENT_INIT  0x22u
#define HOSTED_TAG_CHIP_ID      0x12u
#define HOSTED_TAG_FW_VER       0x17u
#define HOSTED_TAG_SDIO_MODE    0x18u

#define HOSTED_REQ_OTA_ACTIVATE 266u
#define HOSTED_REQ_OTA_BEGIN    272u
#define HOSTED_REQ_OTA_WRITE    273u
#define HOSTED_REQ_OTA_END      274u

typedef struct {
    uint32_t tx_buf_count;
    uint32_t rx_byte_count;
    uint16_t seq_num;
    uint32_t uid;
    bool     init_seen;
    uint8_t  chip_id;
    uint8_t  sdio_mode;
    uint32_t fw_major, fw_minor, fw_patch;
} hosted_ctx_t;

static uint16_t hosted_checksum(const uint8_t *buf, uint32_t len) {
    uint16_t s = 0;
    for (uint32_t i = 0; i < len; i++) s = (uint16_t)(s + buf[i]);
    return s;
}

static uint32_t pb_put_varint(uint8_t *dst, uint32_t v) {
    uint32_t n = 0;
    while (v >= 0x80u) {
        dst[n++] = (uint8_t)((v & 0x7fu) | 0x80u);
        v >>= 7;
    }
    dst[n++] = (uint8_t)v;
    return n;
}

static uint32_t pb_varint_len(uint32_t v) {
    uint32_t n = 1;
    while (v >= 0x80u) { v >>= 7; n++; }
    return n;
}

static const uint8_t *pb_get_varint(const uint8_t *p, const uint8_t *end, uint32_t *out) {
    uint32_t v = 0, shift = 0;
    while (p < end && shift < 35u) {
        uint8_t b = *p++;
        v |= (uint32_t)(b & 0x7fu) << shift;
        if (!(b & 0x80u)) { *out = v; return p; }
        shift += 7u;
    }
    return NULL;
}

static void hosted_parse_priv(hosted_ctx_t *ctx, const uint8_t *pay, uint32_t len) {
    if (len < 2 || pay[0] != HOSTED_PRIV_EVENT_INIT) return;
    uint32_t evt_len = pay[1];
    if (2u + evt_len < len) len = 2u + evt_len;
    const uint8_t *p = pay + 2, *end = pay + len;
    while (p + 2 <= end) {
        uint8_t tag = p[0], tlen = p[1];
        p += 2;
        if (p + tlen > end) break;
        if (tag == HOSTED_TAG_CHIP_ID && tlen >= 1) {
            ctx->chip_id = p[0];
        } else if (tag == HOSTED_TAG_SDIO_MODE && tlen >= 1) {
            ctx->sdio_mode = p[0];
        } else if (tag == HOSTED_TAG_FW_VER && tlen >= 4) {
            uint32_t v = le32(p);
            ctx->fw_major = (v >> 16) & 0xffu;
            ctx->fw_minor = (v >> 8) & 0xffu;
            ctx->fw_patch = v & 0xffu;
        }
        p += tlen;
    }
    ctx->init_seen = true;
}

static bool hosted_parse_serial_resp(const uint8_t *pay, uint32_t len,
                                     uint32_t want_resp_id, int32_t *out_resp) {
    const uint8_t *pos = pay, *end = pay + len;
    const uint8_t *pb = NULL, *pb_end = NULL;
    while (pos + 3 <= end) {
        uint8_t t = pos[0];
        uint16_t l = (uint16_t)pos[1] | ((uint16_t)pos[2] << 8);
        pos += 3;
        if (pos + l > end) return false;
        if (t == 2u) { pb = pos; pb_end = pos + l; break; }
        pos += l;
    }
    if (!pb) return false;

    uint32_t msg_type = 0, msg_id = 0;
    int32_t resp_code = 0;
    const uint8_t *p = pb;
    while (p < pb_end) {
        uint32_t tag = 0;
        p = pb_get_varint(p, pb_end, &tag);
        if (!p) return false;
        uint32_t fnum = tag >> 3, wtype = tag & 7u;
        if (wtype == 0u) {
            uint32_t v = 0;
            p = pb_get_varint(p, pb_end, &v);
            if (!p) return false;
            if (fnum == 1u) msg_type = v;
            else if (fnum == 2u) msg_id = v;
        } else if (wtype == 2u) {
            uint32_t slen = 0;
            p = pb_get_varint(p, pb_end, &slen);
            if (!p || p + slen > pb_end) return false;
            if (fnum == want_resp_id) {
                const uint8_t *sp = p, *send = p + slen;
                while (sp < send) {
                    uint32_t stag = 0, sval = 0;
                    sp = pb_get_varint(sp, send, &stag);
                    if (!sp) break;
                    if ((stag & 7u) == 0u) {
                        sp = pb_get_varint(sp, send, &sval);
                        if (!sp) break;
                        if ((stag >> 3) == 1u) resp_code = (int32_t)sval;
                    } else if ((stag & 7u) == 2u) {
                        uint32_t l2 = 0;
                        sp = pb_get_varint(sp, send, &l2);
                        if (!sp || sp + l2 > send) break;
                        sp += l2;
                    } else {
                        break;
                    }
                }
            }
            p += slen;
        } else if (wtype == 1u) {
            if (p + 8 > pb_end) return false;
            p += 8;
        } else if (wtype == 5u) {
            if (p + 4 > pb_end) return false;
            p += 4;
        } else {
            return false;
        }
    }
    if (msg_type == 2u && msg_id == want_resp_id) {
        *out_resp = resp_code;
        return true;
    }
    return false;
}

/* Polls the slave for any pending packet(s) and dispatches them. If
 * `want_resp_id != 0`, sets `*got_resp = true` and `*out_resp` when the
 * matching RPC response arrives. Under `sdhost_acquire(1)`. */
/* Why the last esp-hosted-mcu exchange failed (for `sdio ota`'s message).
 * guarded by: as g_hosted_ota_busy above, with the buffer and carry below. */
static const char *g_hosted_err = "";
/* Slave -> host bytes, separate from g_scratch (which holds the request being
 * built): streaming mode may hand over several packets at once, and a packet
 * may straddle two reads, so the tail is carried over. */
static uint8_t g_hosted_rx[SDIO_XFER_MAX];
static uint32_t g_hosted_carry;

static int hosted_poll_rx_locked(hosted_ctx_t *ctx, uint32_t want_resp_id,
                                 bool *got_resp, int32_t *out_resp) {
    uint8_t st[LLP_SDIO_STATUS_LEN];
    if (cmd53_locked(false, LLP_SDIO_FN, LLP_SDIO_STATUS_ADDR, st, sizeof st) != 0) {
        g_hosted_err = "status read failed";
        return -1;
    }
    uint32_t int_raw = le32(st + (0x050u - LLP_SDIO_STATUS_ADDR));
    if (int_raw) {
        uint8_t clr[4] = {
            (uint8_t)(int_raw & 0xffu), (uint8_t)((int_raw >> 8) & 0xffu),
            (uint8_t)((int_raw >> 16) & 0xffu), (uint8_t)((int_raw >> 24) & 0xffu)
        };
        (void)cmd53_locked(true, LLP_SDIO_FN, LLP_SDIO_REG_INT_CLR, clr, 4);
    }
    g_llp.token = le32(st + (LLP_SDIO_REG_TOKEN - LLP_SDIO_STATUS_ADDR));
    uint32_t pkt_len = le32(st + (LLP_SDIO_REG_PKT_LEN - LLP_SDIO_STATUS_ADDR));
    if (pkt_len == 0xffffffffu) { g_hosted_err = "PKT_LEN reads all ones (bus fault)"; return -1; }
    pkt_len &= LLP_SDIO_LEN_MASK;

    uint32_t avail = (pkt_len - ctx->rx_byte_count) & LLP_SDIO_LEN_MASK;
    while (avail) {
        /* A multiple of 4: fifo_xfer_locked() rounds a short read up to one. */
        uint32_t room = (SDIO_XFER_MAX - g_hosted_carry) & ~3u;
        uint32_t n = avail < room ? avail : room;
        if (n == 0) { g_hosted_err = "receive buffer full of an unparsable packet"; g_hosted_carry = 0; return -1; }
        if (fifo_xfer_locked(false, g_hosted_rx + g_hosted_carry, n) != 0) {
            g_hosted_err = "FIFO read failed";
            return -1;
        }
        ctx->rx_byte_count = (ctx->rx_byte_count + n) & LLP_SDIO_LEN_MASK;
        avail -= n;
        uint32_t have = g_hosted_carry + n, off = 0;
        while (off + 12u <= have) {
            uint8_t *h = g_hosted_rx + off;
            uint8_t if_type = h[0] & 0x0fu;
            uint16_t pay_len = (uint16_t)h[2] | ((uint16_t)h[3] << 8);
            uint16_t pay_off = (uint16_t)h[4] | ((uint16_t)h[5] << 8);
            if (pay_off != 12u || pay_len == 0) { off = have; break; }   /* padding or garbage: drop */
            if (off + pay_off + pay_len > have) break;                  /* straddles: carry */
            uint16_t rx_csum = (uint16_t)h[6] | ((uint16_t)h[7] << 8);
            h[6] = 0;
            h[7] = 0;
            /* Slaves built with CONFIG_ESP_SDIO_CHECKSUM=n leave checksum = 0. */
            bool ok = rx_csum == 0 || hosted_checksum(h, pay_off + pay_len) == rx_csum;
            const uint8_t *pay = h + pay_off;
            if (ok && if_type == HOSTED_IF_PRIV) {
                hosted_parse_priv(ctx, pay, pay_len);
            } else if (ok && if_type == HOSTED_IF_SERIAL && want_resp_id != 0) {
                int32_t rc = -1;
                if (hosted_parse_serial_resp(pay, pay_len, want_resp_id, &rc)) {
                    *got_resp = true;
                    *out_resp = rc;
                }
            }
            off += pay_off + pay_len;
        }
        g_hosted_carry = have - off;
        if (g_hosted_carry && off) memmove(g_hosted_rx, g_hosted_rx + off, g_hosted_carry);
    }
    return 0;
}

/* Under `sdhost_acquire(1)`: identifies whether the C6 slave is LugalOS LLP or
 * `esp-hosted-mcu` (and records its firmware version from
 * `ESP_PRIV_EVENT_INIT`). The slave sends that event once after its data path
 * opens, so the wait runs once per probe. The host is shared with the microSD:
 * it is released while waiting, and the caller gets it back held. */
static void hosted_detect_locked(void) {
    if (g_hosted_tried) return;
    if (status_locked() != 0) return;
    if (magic_ok()) {
        g_info.llp_present = true;
        return;
    }
    g_hosted_tried = true;
    hosted_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    if (g_llp.pkt_len < g_hosted_rx_bytes) g_hosted_rx_bytes = 0;
    ctx.rx_byte_count = g_hosted_rx_bytes;
    (void)cmd52_locked(true, LLP_SDIO_FN, HOSTED_REG_SLV_INTR, HOSTED_OPEN_DATA_PATH, NULL);
    uint64_t deadline = time_get_ms() + 300u;
    while (!ctx.init_seen && time_get_ms() < deadline) {
        bool dummy_got = false;
        int32_t dummy_rc = 0;
        if (hosted_poll_rx_locked(&ctx, 0, &dummy_got, &dummy_rc) != 0) break;
        if (ctx.init_seen) break;
        sdhost_release();
        task_sleep_ms(2);
        sdhost_acquire(1);
        if (g_hosted_ota_busy) return;   /* `sdio ota` took over while this slept */
    }
    if (ctx.init_seen) {
        /* Drain the trailing Event_ESPInit packet queued ~4 ms after ESP_PRIV_EVENT_INIT. */
        sdhost_release();
        task_sleep_ms(15);
        sdhost_acquire(1);
        if (g_hosted_ota_busy) return;
        bool dummy_got = false;
        int32_t dummy_rc = 0;
        (void)hosted_poll_rx_locked(&ctx, 0, &dummy_got, &dummy_rc);
        g_hosted_rx_bytes = ctx.rx_byte_count;
        g_info.hosted_present = true;
        g_info.hosted_chip_id = ctx.chip_id;
        g_info.hosted_sdio_mode = ctx.sdio_mode;
        g_info.hosted_major = (uint16_t)ctx.fw_major;
        g_info.hosted_minor = (uint16_t)ctx.fw_minor;
        g_info.hosted_patch = (uint16_t)ctx.fw_patch;
    }
}

static int hosted_rpc_call(hosted_ctx_t *ctx, uint32_t req_id,
                           const uint8_t *chunk, uint32_t chunk_len,
                           uint32_t timeout_ms) {
    ctx->uid++;
    uint8_t *pkt = g_scratch;
    uint8_t *pb = pkt + 24u;
    uint32_t pb_len = 0;

    /* Field 1: msg_type = 1 (Req); Field 2: msg_id; Field 3: uid. */
    pb[pb_len++] = 0x08u;
    pb[pb_len++] = 0x01u;
    pb[pb_len++] = 0x10u;
    pb_len += pb_put_varint(pb + pb_len, req_id);
    pb[pb_len++] = 0x18u;
    pb_len += pb_put_varint(pb + pb_len, ctx->uid);

    /* Oneof payload submessage at field `req_id` (wire type 2). */
    pb_len += pb_put_varint(pb + pb_len, (req_id << 3) | 2u);
    if (chunk_len == 0) {
        pb[pb_len++] = 0x00u;
    } else {
        uint32_t sub_len = 1u + pb_varint_len(chunk_len) + chunk_len;
        pb_len += pb_put_varint(pb + pb_len, sub_len);
        pb[pb_len++] = 0x0Au;   /* field 1: bytes ota_data */
        pb_len += pb_put_varint(pb + pb_len, chunk_len);
        memcpy(pb + pb_len, chunk, chunk_len);
        pb_len += chunk_len;
    }

    /* 12-byte Protocomm TLV header: EPNAME ("RPCRsp") + DATA (pb_len). */
    static const uint8_t ep_tlv[9] = { 0x01u, 0x06u, 0x00u, 'R', 'P', 'C', 'R', 's', 'p' };
    memcpy(pkt + 12u, ep_tlv, 9u);
    pkt[21] = 0x02u;
    pkt[22] = (uint8_t)(pb_len & 0xffu);
    pkt[23] = (uint8_t)(pb_len >> 8);

    /* 12-byte esp_payload_header. */
    uint16_t serial_len = (uint16_t)(12u + pb_len);
    uint32_t total_len = 12u + (uint32_t)serial_len;
    memset(pkt, 0, 12u);
    pkt[0] = HOSTED_IF_SERIAL;
    pkt[2] = (uint8_t)(serial_len & 0xffu);
    pkt[3] = (uint8_t)(serial_len >> 8);
    pkt[4] = 12u;
    pkt[8] = (uint8_t)(ctx->seq_num & 0xffu);
    pkt[9] = (uint8_t)(ctx->seq_num >> 8);
    ctx->seq_num++;
    uint16_t csum = hosted_checksum(pkt, total_len);
    pkt[6] = (uint8_t)(csum & 0xffu);
    pkt[7] = (uint8_t)(csum >> 8);

    uint32_t bufs_needed = (total_len + HOSTED_RX_BUF_SIZE - 1u) / HOSTED_RX_BUF_SIZE;
    uint64_t deadline = time_get_ms() + timeout_ms;
    sdhost_acquire(1);
    for (;;) {
        if (status_locked() != 0) { sdhost_release(); g_hosted_err = "status read failed"; return -1; }
        uint32_t free_bufs = (LLP_SDIO_TOKEN(g_llp.token) - ctx->tx_buf_count) & (LLP_SDIO_TOKEN_MOD - 1u);
        if (free_bufs >= bufs_needed) break;
        sdhost_release();
        if (time_get_ms() >= deadline) { g_hosted_err = "no slave receive buffer"; return -1; }
        task_sleep_ms(2);
        sdhost_acquire(1);
    }
    if (fifo_xfer_locked(true, pkt, total_len) != 0) {
        sdhost_release();
        g_hosted_err = "FIFO write failed";
        return -1;
    }
    ctx->tx_buf_count = (ctx->tx_buf_count + bufs_needed) & (LLP_SDIO_TOKEN_MOD - 1u);

    uint32_t want_resp_id = req_id + 256u;
    bool got_resp = false;
    int32_t resp_code = -1;
    while (!got_resp) {
        if (hosted_poll_rx_locked(ctx, want_resp_id, &got_resp, &resp_code) != 0) {
            sdhost_release();
            return -1;
        }
        if (got_resp) break;
        sdhost_release();
        if (time_get_ms() >= deadline) { g_hosted_err = "no response in time"; return -1; }
        task_sleep_ms(2);
        sdhost_acquire(1);
    }
    sdhost_release();
    if (resp_code != 0) {
        /* accepted: a diagnostic command's state; two terminals running it at once mix
         * their output. */
        static char why[40];
        ksnprintf(why, sizeof why, "slave answered esp_err 0x%x", (unsigned)resp_code);
        g_hosted_err = why;
        return -1;
    }
    return 0;
}

int sdio_hosted_ota(const char *path) {
    static const char *const defaults[] = {
        "/sd0/lugalos-c6-app.bin",
        "/sd0/firmware/lugalos-c6-mini.bin",
        "/ram0/lugalos-c6-app.bin",
    };
    int fd = -1;
    if (path && *path) {
        fd = vfs_open(path, VFS_O_READ);
    } else {
        for (unsigned i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
            fd = vfs_open(defaults[i], VFS_O_READ);
            if (fd >= 0) { path = defaults[i]; break; }
        }
    }
    if (fd < 0) {
        cprintf("sdio ota: cannot open %s\n", (path && *path) ? path : "/sd0/lugalos-c6-app.bin");
        return -1;
    }
    vfs_stat_t st = {0};
    if (vfs_fstat(fd, &st) != 0 || st.size < 0x100u ||
        vfs_pread(fd, g_scratch, 0x80u, 0) != 0x80) {
        vfs_close(fd);
        cprintf("sdio ota: cannot read header of %s\n", path);
        return -1;
    }
    if (g_scratch[0] != 0xE9u || le32(g_scratch + 0x20u) != 0xABCD5432u) {
        vfs_close(fd);
        cprintf("sdio ota: %s is not a valid ESP-IDF app image (magic 0x%02x, app_desc 0x%08x)\n",
                path, g_scratch[0], (unsigned)le32(g_scratch + 0x20u));
        return -1;
    }
    char proj[33], ver[33];
    memcpy(proj, g_scratch + 0x50u, 32); proj[32] = '\0';
    memcpy(ver,  g_scratch + 0x30u, 32); ver[32]  = '\0';
    cprintf("sdio ota: image %s (%s %s, %u bytes)\n", path, proj, ver, (unsigned)st.size);

    /* A C6 already on LLP is refused before the reset below would cut its link. */
    bool llp_now = sdio_llp_up();
    if (!llp_now && g_info.present) {
        sdhost_acquire(1);
        llp_now = status_locked() == 0 && magic_ok();
        sdhost_release();
    }
    if (llp_now) {
        vfs_close(fd);
        cprintf("sdio ota: C6 is already running LugalOS (LLP gen %u); use /c6/dev/ota instead\n",
                g_llp.shared[LLP_SDIO_SH_GEN]);
        return -1;
    }

    g_hosted_ota_busy = true;
    g_llp.up = false;

    /* Reset the C6 and let its bootloader + esp-hosted-mcu slave initialize. */
    cprintf("sdio ota: resetting C6 and probing esp-hosted-mcu SDIO slave...\n");
    g_hosted_rx_bytes = 0;
    g_hosted_carry = 0;
    sdio_c6_reset(20);
    task_sleep_ms(750);

    hosted_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);

    sdhost_acquire(1);
    if (probe_locked((uint32_t)CONFIG_SDIO_FREQ_KHZ) != 0 || !g_info.fn1_ready) {
        sdhost_release();
        vfs_close(fd);
        g_hosted_ota_busy = false;
        cprintf("sdio ota: SDIO probe failed (stage %u)\n", (unsigned)g_info.stage);
        return -1;
    }
    if (status_locked() == 0 && magic_ok()) {
        sdhost_release();
        vfs_close(fd);
        g_hosted_ota_busy = false;
        cprintf("sdio ota: C6 is already running LugalOS (LLP gen %u); use /c6/dev/ota instead\n",
                g_llp.shared[LLP_SDIO_SH_GEN]);
        return -1;
    }
    /* Open the slave data path (HOST_TO_SLAVE_INTR = 0x08C, bit 0). */
    (void)cmd52_locked(true, LLP_SDIO_FN, HOSTED_REG_SLV_INTR, HOSTED_OPEN_DATA_PATH, NULL);
    uint64_t init_deadline = time_get_ms() + 3000u;
    while (!ctx.init_seen && time_get_ms() < init_deadline) {
        bool dummy_got = false;
        int32_t dummy_rc = 0;
        if (hosted_poll_rx_locked(&ctx, 0, &dummy_got, &dummy_rc) != 0) break;
        if (ctx.init_seen) break;
        sdhost_release();
        task_sleep_ms(10);
        sdhost_acquire(1);
    }
    /* Drain any trailing startup event (e.g. Event_ESPInit on ESP_SERIAL_IF). */
    if (ctx.init_seen) {
        sdhost_release();
        task_sleep_ms(15);
        sdhost_acquire(1);
        bool dummy_got = false;
        int32_t dummy_rc = 0;
        (void)hosted_poll_rx_locked(&ctx, 0, &dummy_got, &dummy_rc);
    }
    sdhost_release();

    if (!ctx.init_seen) {
        vfs_close(fd);
        g_hosted_ota_busy = false;
        cprintf("sdio ota: timed out waiting for esp-hosted-mcu INIT event\n");
        return -1;
    }
    cprintf("sdio ota: esp-hosted-mcu slave ready (chip 0x%02x, fw v%u.%u.%u, %s mode)\n",
            ctx.chip_id, (unsigned)ctx.fw_major, (unsigned)ctx.fw_minor, (unsigned)ctx.fw_patch,
            ctx.sdio_mode ? "streaming" : "packet");

    uint64_t t0 = time_get_ms();
    cprintf("sdio ota: OTABegin (erasing target partition)...\n");
    if (hosted_rpc_call(&ctx, HOSTED_REQ_OTA_BEGIN, NULL, 0, 20000u) != 0) {
        vfs_close(fd);
        g_hosted_ota_busy = false;
        cprintf("sdio ota: Req_OTABegin failed (%s)\n", g_hosted_err);
        return -1;
    }

    uint32_t written = 0, next_report = 131072u;
    while (written < st.size) {
        uint32_t n = st.size - written;
        if (n > 1024u) n = 1024u;
        uint8_t *chunk = g_scratch + 2048u;
        if (vfs_pread(fd, chunk, n, written) != (int)n) {
            vfs_close(fd);
            g_hosted_ota_busy = false;
            cprintf("sdio ota: file read error at offset %u\n", (unsigned)written);
            return -1;
        }
        if (hosted_rpc_call(&ctx, HOSTED_REQ_OTA_WRITE, chunk, n, 10000u) != 0) {
            vfs_close(fd);
            g_hosted_ota_busy = false;
            cprintf("sdio ota: Req_OTAWrite failed at %u / %u (%s)\n",
                    (unsigned)written, (unsigned)st.size, g_hosted_err);
            return -1;
        }
        written += n;
        if (written >= next_report || written == st.size) {
            cprintf("sdio ota: %u / %u KB (%u%%)\n",
                    (unsigned)(written / 1024u), (unsigned)(st.size / 1024u),
                    (unsigned)((uint64_t)written * 100u / st.size));
            next_report += 131072u;
        }
    }
    vfs_close(fd);

    if (hosted_rpc_call(&ctx, HOSTED_REQ_OTA_END, NULL, 0, 15000u) != 0) {
        g_hosted_ota_busy = false;
        cprintf("sdio ota: Req_OTAEnd failed (%s)\n", g_hosted_err);
        return -1;
    }

    /* esp-hosted-mcu >= v2.6.0 separates OTAActivate from OTAEnd; earlier
     * versions (< 2.6.0) activate the partition inside OTAEnd. */
    if (ctx.fw_major > 2u || (ctx.fw_major == 2u && ctx.fw_minor >= 6u)) {
        if (hosted_rpc_call(&ctx, HOSTED_REQ_OTA_ACTIVATE, NULL, 0, 10000u) != 0) {
            g_hosted_ota_busy = false;
            cprintf("sdio ota: Req_OTAActivate failed (%s)\n", g_hosted_err);
            return -1;
        }
    }

    uint32_t elapsed_ms = (uint32_t)(time_get_ms() - t0);
    cprintf("sdio ota: flashed %u bytes in %u.%u s; rebooting C6 into LugalOS...\n",
            (unsigned)st.size, (unsigned)(elapsed_ms / 1000u), (unsigned)((elapsed_ms % 1000u) / 100u));

    task_sleep_ms(250);
    sdio_c6_reset(20);
    g_info.present = false;
    g_llp.need_reset = true;
    g_hosted_ota_busy = false;

    uint64_t llp_deadline = time_get_ms() + 5000u;
    while (time_get_ms() < llp_deadline) {
        if (sdio_llp_up()) break;
        task_sleep_ms(100);
    }
    if (sdio_llp_up()) {
        cprintf("sdio ota: LugalOS C6 is up over LLP (gen %u)\n", g_llp.gen);
        return 0;
    }
    cprintf("sdio ota: OTA succeeded; waiting for LLP pump to attach\n");
    return 0;
}

#else

void sdio_report(void) { cprintf("SDIO: no slot 1 on this board\n"); }
int sdio_probe(uint32_t khz) { (void)khz; return -1; }
const sdio_info_t *sdio_info(void) { return NULL; }
int sdio_cmd52(bool w, uint32_t fn, uint32_t a, uint8_t in, uint8_t *out) { (void)w; (void)fn; (void)a; (void)in; (void)out; return -1; }
int sdio_cmd53(bool w, uint32_t fn, uint32_t a, uint8_t *d, uint32_t l) { (void)w; (void)fn; (void)a; (void)d; (void)l; return -1; }
void sdio_c6_reset(uint32_t ms) { (void)ms; }
void sdio_bench(uint32_t kb, uint32_t fn, uint32_t addr, uint32_t len) { (void)kb; (void)fn; (void)addr; (void)len; sdio_report(); }
int sdio_hosted_ota(const char *path) { (void)path; sdio_report(); return -1; }
int sdio_llp_open(void) { return -1; }
void sdio_llp_set_waker(int pid) { (void)pid; }
bool sdio_llp_irq_pending(void) { return false; }
int sdio_llp_poll(void) { return -1; }
bool sdio_llp_up(void) { return false; }
void sdio_llp_resync(void) {}
uint32_t sdio_llp_readable(void) { return 0; }
bool sdio_llp_room(uint32_t len) { (void)len; return false; }
int sdio_llp_write(const uint8_t *pkt, uint32_t len) { (void)pkt; (void)len; return -1; }
int sdio_llp_read(uint8_t *buf, uint32_t cap) { (void)buf; (void)cap; return -1; }
void sdio_llp_report(void) {}

#endif
