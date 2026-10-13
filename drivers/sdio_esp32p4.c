/* The ESP32-P4's SDIO host: the SD/MMC controller's slot 1, wired to the
 * board's ESP32-C6. The controller
 * itself belongs to drivers/sdmmc_esp32p4.c (slot 0, the microSD card); this
 * file is a client of it through drivers/sdhost_esp32p4.h, and everything it
 * does is under that controller's lock with card 1 selected.
 *
 * What it does: the SDIO bring-up (CMD5 until the card is ready, CMD3, CMD7),
 * CMD52 (one byte, any function), CMD53 (byte and block mode, through the
 * controller's DMA), the card's CIS (who it is), 4-bit mode, function 1, and
 * the C6's reset line, and the host side of LLP's SDIO binding. Not yet: the
 * card interrupt -- the LLP pump polls.
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
#include "kernel/time.h"
#include "arch/esp32p4_intr.h"   /* esp32p4_dcache_*, ESP32P4_L1_CACHE_LINE */
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
#define GPIO_OUT1_W1TC      (GPIO_BASE + 0x18)
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
    const uint32_t bit = 1u << (g - 32u);
    pad_gpio(g, false);
    REG(GPIO_OUT_SEL(g)) = GPIO_OUT_SEL_GPIO | GPIO_OUT_OEN_SEL;
    REG(GPIO_OUT1_W1TC) = bit;
    REG(GPIO_ENABLE1_W1TS) = bit;          /* EN low */
    time_delay_us(hold_ms * 1000u);
    REG(GPIO_ENABLE1_W1TC) = bit;          /* released: the pull-up raises it */
}

/* --- CMD52 / CMD53 ---------------------------------------------------------------- */

#define CMD52_WRITE    (1u << 31)
#define CMD52_RAW      (1u << 27)
#define CMD53_WRITE    (1u << 31)
#define CMD53_BLOCK    (1u << 27)
#define CMD53_INCR     (1u << 26)
#define R5_ERRORS      0xCB00u       /* COM_CRC, ILLEGAL, ERROR, FUNCTION_NUMBER, OUT_OF_RANGE */

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

static int probe_locked(uint32_t khz) {
    memset(&g_info, 0, sizeof g_info);
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
    g_info.khz = sdhost_set_clock(khz);
    g_info.stage = 9;
    g_info.present = true;
    return 0;
}

int sdio_probe(uint32_t khz) {
    if (!khz) khz = (uint32_t)CONFIG_SDIO_FREQ_KHZ;
    sdhost_acquire(1);
    int rc = probe_locked(khz);
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
    uint32_t polls, writes, reads, wbytes, rbytes, no_room, resyncs, failures;
} g_llp;

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One packet through the FIFO: whole blocks while 512 or more remain, then the
 * rest in byte mode, each CMD53 addressed so that the slave sees how much is
 * still to come (FIFO_END - remaining). `len` is a multiple of 4. */
static int fifo_xfer_locked(bool write, uint8_t *buf, uint32_t len) {
    uint32_t done = 0;
    while (done < len) {
        uint32_t rem = len - done;
        uint32_t n = rem >= g_info.block_size ? (rem / g_info.block_size) * g_info.block_size : rem;
        if (n > SDIO_XFER_MAX) n = SDIO_XFER_MAX;
        if (cmd53_locked(write, LLP_SDIO_FN, LLP_SDIO_FIFO_END - rem, buf + done, n) != 0) return -1;
        done += n;
    }
    return 0;
}

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
    sdhost_acquire(1);
    int rc = -1;
    if (g_info.present || probe_locked((uint32_t)CONFIG_SDIO_FREQ_KHZ) == 0) rc = channel_reset_locked();
    if (rc != 0) g_llp.need_reset = true;
    sdhost_release();
    return rc;
}

/* One status read. A slave that stopped answering is probed again, at most
 * once a second; one that restarted by itself is followed. 0 when up, 1 when
 * up with both counters just restarted (whatever was in flight is gone), -1
 * when down. */
int sdio_llp_poll(void) {
    static uint64_t next_probe;
    sdhost_acquire(1);
    int rc = -1;
    if (!g_info.present) {
        uint64_t now = time_get_ms();
        if (now < next_probe) goto out;
        next_probe = now + 1000u;
        if (probe_locked((uint32_t)CONFIG_SDIO_FREQ_KHZ) != 0) goto out;
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
    sdhost_release();
    return rc;
}

bool sdio_llp_up(void) { return g_llp.up; }

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
    cprintf("  sdio: %u polls, out %u packets %u B (no room %u), in %u packets %u B, resyncs %u, failures %u, bus errors %u\n",
            (unsigned)g_llp.polls, (unsigned)g_llp.writes, (unsigned)g_llp.wbytes, (unsigned)g_llp.no_room,
            (unsigned)g_llp.reads, (unsigned)g_llp.rbytes, (unsigned)g_llp.resyncs, (unsigned)g_llp.failures,
            (unsigned)g_info.errors);
}

/* --- Shell --------------------------------------------------------------------------- */

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
    cprintf("  function 1 %s, block size %u; bus %u-bit at %u kHz; errors %u\n",
            i->fn1_ready ? "ready" : "NOT ready", (unsigned)i->block_size, (unsigned)i->width,
            (unsigned)i->khz, (unsigned)i->errors);
}

void sdio_bench(uint32_t kilobytes, uint32_t fn, uint32_t addr, uint32_t chunk) {
    if (!g_info.present) { cprintf("sdio bench: probe first (`sdio probe`)\n"); return; }
    if (!chunk) chunk = g_info.block_size ? g_info.block_size : 512u;
    if (chunk > SDIO_XFER_MAX) chunk = SDIO_XFER_MAX;
    uint32_t total = kilobytes * 1024u, done = 0;
    static uint8_t scratch[SDIO_XFER_MAX];
    uint64_t t0 = time_get_us();
    while (done < total) {
        if (sdio_cmd53(false, fn, addr, scratch, chunk) != 0) break;
        done += chunk;
    }
    uint64_t us = time_get_us() - t0;
    if (!us) us = 1;
    cprintf("sdio bench: %u KB of CMD53 reads (fn%u 0x%x, %u B each) in %u ms = %u KB/s, %u-bit at %u kHz%s\n",
            (unsigned)(done / 1024u), (unsigned)fn, (unsigned)addr, (unsigned)chunk, (unsigned)(us / 1000u),
            (unsigned)((uint64_t)done * 1000000u / 1024u / us), (unsigned)g_info.width, (unsigned)g_info.khz,
            done < total ? " -- STOPPED on an error" : "");
}

#else

void sdio_report(void) { cprintf("SDIO: no slot 1 on this board\n"); }
int sdio_probe(uint32_t khz) { (void)khz; return -1; }
const sdio_info_t *sdio_info(void) { return NULL; }
int sdio_cmd52(bool w, uint32_t fn, uint32_t a, uint8_t in, uint8_t *out) { (void)w; (void)fn; (void)a; (void)in; (void)out; return -1; }
int sdio_cmd53(bool w, uint32_t fn, uint32_t a, uint8_t *d, uint32_t l) { (void)w; (void)fn; (void)a; (void)d; (void)l; return -1; }
void sdio_c6_reset(uint32_t ms) { (void)ms; }
void sdio_bench(uint32_t kb, uint32_t fn, uint32_t addr, uint32_t len) { (void)kb; (void)fn; (void)addr; (void)len; sdio_report(); }
int sdio_llp_open(void) { return -1; }
int sdio_llp_poll(void) { return -1; }
bool sdio_llp_up(void) { return false; }
uint32_t sdio_llp_readable(void) { return 0; }
bool sdio_llp_room(uint32_t len) { (void)len; return false; }
int sdio_llp_write(const uint8_t *pkt, uint32_t len) { (void)pkt; (void)len; return -1; }
int sdio_llp_read(uint8_t *buf, uint32_t cap) { (void)buf; (void)cap; return -1; }
void sdio_llp_report(void) {}

#endif
