/* The ESP32-P4's SDIO host: the SD/MMC controller's slot 1, wired to the
 * board's ESP32-C6. The controller
 * itself belongs to drivers/sdmmc_esp32p4.c (slot 0, the microSD card); this
 * file is a client of it through drivers/sdhost_esp32p4.h, and everything it
 * does is under that controller's lock with card 1 selected.
 *
 * What it does: the SDIO bring-up (CMD5 until the card is ready, CMD3, CMD7),
 * CMD52 (one byte, any function), CMD53 (byte and block mode, through the
 * controller's DMA), the card's CIS (who it is), 4-bit mode, function 1, and
 * the C6's reset line. Not yet: the card interrupt, and anything that knows
 * what the C6 *is* -- that is 49.6, LLP over SDIO.
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
    if (sdhost_dma_wait(addr, write ? "sdio write" : "sdio read") != 0) { g_info.errors++; return -1; }
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

#endif
