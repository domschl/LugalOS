/*
 * LugalOS Hardware Driver: SPI MicroSD Card Driver for RP2350 (Pico 2)
 * Hardware Mapping (K2, plan/phase7_kernel_config.md -- the numbers
 * themselves live in cmake/board-rp2350.cmake now, this is documentation,
 * not the source of truth):
 *   GP10 : SPI1 SCK (Clock, Function 6)
 *   GP11 : SPI1 TX  (MOSI, Function 6)
 *   GP12 : SPI1 RX  (MISO, Function 6)
 *   GP13 : SPI1 CS  (Chip Select, Function 5 - SIO GPIO)
 */

#include "drivers/spisd.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/chan.h"
#include "lugalos_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Some RP2350 board personas (e.g. the Pico-Clock-Green baseboard, phase11,
 * plan/phase11_pico_clock_green.md) wire GP10-13 to different hardware
 * entirely and build with LUGALOS_ENABLE_SPISD=OFF, so cmake/board-*.cmake
 * for those personas doesn't define CONFIG_SPI1_* at all. Stubbed here
 * rather than gated out of CMakeLists.txt's source list so
 * fs/vfs_server.c's spisd_get_device() call sites don't need a matching
 * #if -- same shape as i2c_rtc.c's non-RP2350 #else stub below its own
 * hardware implementation. */
#if CONFIG_ENABLE_SPISD

#define RESETS_BASE             0x40020000UL
#define RESETS_RESET_DONE       (RESETS_BASE + 0x08)
#define RESETS_ATOMIC_CLEAR     (RESETS_BASE + 0x3000)

#define IO_BANK0_BASE           0x40028000UL
#define IO_BANK0_CTRL(n)        (IO_BANK0_BASE + 0x004 + (n) * 8)

#define PADS_BANK0_BASE         0x40038000UL
#define PADS_BANK0_PAD(n)       (PADS_BANK0_BASE + 0x004 + (n) * 4)

#define SIO_BASE                0xD0000000UL
#define SIO_GPIO_OUT_SET        (SIO_BASE + 0x018)
#define SIO_GPIO_OUT_CLR        (SIO_BASE + 0x020)
#define SIO_GPIO_OE_SET         (SIO_BASE + 0x038)

#define SPI1_BASE               ((uintptr_t)CONFIG_SPI1_BASE)
#define SSPCR0                  (SPI1_BASE + 0x00)
#define SSPCR1                  (SPI1_BASE + 0x04)
#define SSPDR                   (SPI1_BASE + 0x08)
#define SSPSR                   (SPI1_BASE + 0x0C)
#define SSPCPSR                 (SPI1_BASE + 0x10)

#define REG(addr) (*(volatile uint32_t *)(addr))

#define CS_PIN CONFIG_SPI1_CS_GPIO
#define CS_MASK (1u << CS_PIN)

static bool g_sd_is_sdhc = false;
static bool g_sd_initialized = false;

static inline void cs_select(void) {
    REG(SIO_GPIO_OUT_CLR) = CS_MASK;
}

static inline void cs_deselect(void) {
    REG(SIO_GPIO_OUT_SET) = CS_MASK;
}

static uint8_t spi_transfer(uint8_t tx) {
    /* Drain any stale bytes from RX FIFO first */
    while (REG(SSPSR) & (1u << 2)) {
        (void)REG(SSPDR);
    }

    /* Wait for TX FIFO Not Full (TNF, bit 1) */
    int timeout = 10000;
    while (!(REG(SSPSR) & (1u << 1)) && --timeout > 0);

    /* Push byte to start clocking */
    REG(SSPDR) = tx;

    /* Wait for RX FIFO Not Empty (RNE, bit 2) */
    timeout = 10000;
    while (!(REG(SSPSR) & (1u << 2)) && --timeout > 0);

    /* Read single received byte */
    return (uint8_t)(REG(SSPDR) & 0xFF);
}

static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg, uint8_t crc) {
    uint8_t res;
    int retry = 0;

    cs_deselect();
    spi_transfer(0xFF);
    cs_select();
    spi_transfer(0xFF);

    spi_transfer(0x40 | (cmd & 0x3F));
    spi_transfer((uint8_t)(arg >> 24));
    spi_transfer((uint8_t)(arg >> 16));
    spi_transfer((uint8_t)(arg >> 8));
    spi_transfer((uint8_t)arg);
    spi_transfer(crc);

    do {
        res = spi_transfer(0xFF);
    } while ((res & 0x80) && (++retry < 1000));

    return res;
}

static int spisd_init_hardware(void) {
    /* 1. Unreset SPI1 (bit 19 on RP2350), IO_BANK0 (bit 6), PADS_BANK0 (bit 9) */
    uint32_t unreset_mask = (1u << 19) | (1u << 6) | (1u << 9);
    REG(RESETS_ATOMIC_CLEAR) = unreset_mask;
    while ((REG(RESETS_RESET_DONE) & unreset_mask) != unreset_mask);

    /* 2. Configure SPI1 GPIO pins: GP10 (SCK, F1), GP11 (TX/MOSI, F1), GP12 (RX/MISO, F1) */
    REG(IO_BANK0_CTRL(CONFIG_SPI1_SCK_GPIO)) = 1;
    REG(IO_BANK0_CTRL(CONFIG_SPI1_MOSI_GPIO)) = 1;
    REG(IO_BANK0_CTRL(CONFIG_SPI1_MISO_GPIO)) = 1;

    /* Configure pad pull-ups (0x5A = PUE=1, PDE=0, IE=1) so unattached bus reads 0xFF */
    REG(PADS_BANK0_PAD(CONFIG_SPI1_SCK_GPIO)) = 0x5A;
    REG(PADS_BANK0_PAD(CONFIG_SPI1_MOSI_GPIO)) = 0x5A;
    REG(PADS_BANK0_PAD(CONFIG_SPI1_MISO_GPIO)) = 0x5A;

    /* GP13 as CS (Function 5 - SIO) */
    REG(IO_BANK0_CTRL(CS_PIN)) = 5;
    REG(PADS_BANK0_PAD(CS_PIN)) = 0x5A;
    REG(SIO_GPIO_OE_SET) = CS_MASK;
    cs_deselect();

    /* 3. Disable SPI1 before programming */
    REG(SSPCR1) = 0;

    /* Set clock prescaler for slow init (~400 kHz): 150MHz / (150 * 2.5) */
    REG(SSPCPSR) = 150; // CPSDVSR prescaler
    REG(SSPCR0) = (0u << 8) | (0u << 7) | (0u << 6) | 0x7; // 8-bit, SCR=0

    /* Enable SPI1 */
    REG(SSPCR1) = (1u << 1); // SSE = 1

    /* Send 160+ dummy clock cycles (20 bytes) with CS high to power up SPI mode */
    cs_deselect();
    for (int i = 0; i < 20; i++) {
        spi_transfer(0xFF);
    }

    /* CMD0: Reset SD Card into SPI Mode (expects 0x01 = In Idle State) */
    uint8_t r1 = 0xFF;
    for (int retry = 0; retry < 100; retry++) {
        r1 = sd_send_cmd(0, 0, 0x95);
        if (r1 == 0x01) break;
    }

    if (r1 != 0x01) {
        cs_deselect();
        printk("[SPI SD Probe] SPI1 MicroSD probe CMD0 returned 0x%02x (%s)\n",
               r1, (r1 == 0xFF) ? "no card attached" : (r1 == 0x00) ? "MISO line grounded/stuck low" : "invalid response");
        return -1; // Card not responding or no MicroSD present
    }
    printk("[SPI SD Probe] CMD0 reset response: 0x01 (MicroSD Card detected in Idle State)\n");

    /* CMD8: Check SDv2 voltage range (0x1AA) */
    r1 = sd_send_cmd(8, 0x000001AA, 0x87);
    bool is_v2 = false;
    if (r1 <= 0x01) {
        uint32_t r7 = 0;
        r7 |= ((uint32_t)spi_transfer(0xFF) << 24);
        r7 |= ((uint32_t)spi_transfer(0xFF) << 16);
        r7 |= ((uint32_t)spi_transfer(0xFF) << 8);
        r7 |= (uint32_t)spi_transfer(0xFF);

        if ((r7 & 0xFF) == 0xAA) {
            is_v2 = true;
        }
    }

    /* ACMD41: Initialize SD Card */
    int retries = 0;
    do {
        sd_send_cmd(55, 0, 0x65); // CMD55 prefix
        r1 = sd_send_cmd(41, is_v2 ? 0x40000000 : 0, 0x77);
    } while (r1 != 0x00 && ++retries < 2000);

    if (r1 != 0x00) {
        cs_deselect();
        printk("[SPI SD Probe] ACMD41 initialization failed (r1 = 0x%02x)\n", r1);
        return -1;
    }

    /* CMD58: Read OCR register to determine if Card Capacity Status (CCS, bit 30) is set */
    g_sd_is_sdhc = false;
    if (is_v2) {
        r1 = sd_send_cmd(58, 0, 0xFD);
        if (r1 <= 0x01) {
            uint8_t ocr0 = spi_transfer(0xFF);
            spi_transfer(0xFF); spi_transfer(0xFF); spi_transfer(0xFF);
            if (ocr0 & 0x40) { // Bit 30 of OCR is bit 6 of first byte
                g_sd_is_sdhc = true; // SDHC/SDXC Card (Block Addressing)
            }
        }
    }

    /* CMD16: Set 512-byte block length for Standard Capacity cards */
    if (!g_sd_is_sdhc) {
        sd_send_cmd(16, 512, 0xFF);
    }

    cs_deselect();

    /* 4. Switch SPI clock prescaler to 12.5 MHz for solid signal integrity */
    REG(SSPCR1) = 0;
    REG(SSPCPSR) = 12; // Prescaler = 12 (150 MHz / 12 = 12.5 MHz SPI speed)
    REG(SSPCR1) = (1u << 1);

    g_sd_initialized = true;
    printk("[SPI SD Driver] MicroSD Card initialized on SPI1 (GP10-GP13, Mode: %s, Speed: 12.5 MHz)\n",
           g_sd_is_sdhc ? "SDHC/SDXC Block" : "Standard Byte");
    return 0;
}

/* M4.5, plan/phase12_microkernel_migration.md, Part B: the actual SPI/SD
 * hardware access, pulled out of the block_dev_t function pointers so both
 * the "sdblk" task's serve loop below and the direct-access fallback (task
 * not alive yet -- true for every read during boot, since vfs_server_init()
 * mounts the filesystem before sched_init() creates a task table at all)
 * can share one implementation, the same split uart_16550.c's
 * uart_hw_putc_blocking() established for the same reason. Unlike uart,
 * this needed no batching redesign: a read_blocks()/write_blocks() call
 * was already exactly one message's worth of work (a bounded lba+count
 * request), never one IPC round trip per byte -- the plan's own
 * lowest-risk-first reasoning for putting this driver ahead of uart's
 * remaining board-specific work. */
static int spisd_hw_read_blocks(uint32_t lba, uint32_t count, void *buf) {
    if (!g_sd_initialized) return -1;

    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = g_sd_is_sdhc ? (lba + i) : ((lba + i) * 512);
        uint8_t r1 = sd_send_cmd(17, addr, 0xFF);
        if (r1 != 0x00) {
            cs_deselect();
            return -1;
        }

        /* Wait for data start token (0xFE) */
        int timeout = 0;
        uint8_t token;
        do {
            token = spi_transfer(0xFF);
        } while (token != 0xFE && ++timeout < 10000);

        if (token != 0xFE) {
            cs_deselect();
            return -1;
        }

        /* Read 512 data bytes */
        for (int b = 0; b < 512; b++) {
            dst[b] = spi_transfer(0xFF);
        }

        /* Read 2 CRC bytes */
        spi_transfer(0xFF);
        spi_transfer(0xFF);

        cs_deselect();
        dst += 512;
    }
    return 0;
}

static int spisd_hw_write_blocks(uint32_t lba, uint32_t count, const void *buf) {
    if (!g_sd_initialized) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = g_sd_is_sdhc ? (lba + i) : ((lba + i) * 512);
        uint8_t r1 = sd_send_cmd(24, addr, 0xFF);
        if (r1 != 0x00) {
            cs_deselect();
            return -1;
        }

        /* Transmit data start token 0xFE */
        spi_transfer(0xFE);

        /* Transmit 512 data bytes */
        for (int b = 0; b < 512; b++) {
            spi_transfer(src[b]);
        }

        /* Transmit 2 dummy CRC bytes */
        spi_transfer(0xFF);
        spi_transfer(0xFF);

        /* Read data response token */
        uint8_t resp = spi_transfer(0xFF);
        if ((resp & 0x1F) != 0x05) { // 0x05 = Data accepted
            cs_deselect();
            return -1;
        }

        /* Wait for write completion (card holds MISO low while busy) */
        int timeout = 0;
        while (spi_transfer(0xFF) != 0xFF && ++timeout < 100000);

        cs_deselect();
        src += 512;
    }
    return 0;
}

/* --- M4.5 Part B: the "sdblk" driver task ---
 *
 * Wire protocol, one opcode byte then a fixed 8-byte lba+count header:
 *   'R', lba(4 BE), count(4 BE)          -> resp: status(1), data(count*512)
 *   'W', lba(4 BE), count(4 BE), data... -> resp: status(1)
 * status is 0 for success, 1 for failure -- matches read_blocks()/
 * write_blocks()'s own 0/-1 contract, just not signed (the wire is bytes).
 *
 * BLK_MAX_COUNT bounds both buffers: every real caller in this tree
 * (fs/fat32.c) only ever requests count=1 (checked directly: this
 * codebase's own fat32_format() always uses sec_per_clus = 1), so 4 is
 * headroom for a future multi-sector caller, not a measured need -- a
 * request larger than this falls back to direct access below rather than
 * being refused outright. */
#define BLK_REQ_READ  ((uint8_t)'R')
#define BLK_REQ_WRITE ((uint8_t)'W')
#define BLK_MAX_COUNT 4u
#define BLK_HDR_LEN   9u /* opcode + lba(4) + count(4) */
#define BLK_REQ_CAP   (BLK_HDR_LEN + BLK_MAX_COUNT * 512u)
#define BLK_RESP_CAP  (1u + BLK_MAX_COUNT * 512u)

static uint8_t         g_blk_req[BLK_REQ_CAP];
static uint8_t         g_blk_resp[BLK_RESP_CAP];
static chan_endpoint_t *g_blk_ep;
static int              g_blk_task_pid = -1;

/* M4.5 verify: counts chan_call()s actually served -- see
 * drivers/uart_16550.c's g_uart_write_calls comment for the reasoning
 * (a nonzero, growing count is what distinguishes "the task is genuinely
 * serving requests" from "every caller silently fell back to direct
 * access the whole time"). */
static uint32_t g_blk_calls;

uint32_t blk_task_call_count(void) { return g_blk_calls; }

static bool blk_task_alive(void) {
    if (g_blk_task_pid < 0) return false;
    int st = sched_task_state(g_blk_task_pid);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

static uint32_t be32_load(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void be32_store(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* This task, and only this task, may call spisd_hw_read_blocks()/
 * spisd_hw_write_blocks() while alive -- see uart_16550.c's uart_task_body()
 * for the fuller reasoning (never call back into anything that could
 * chan_call() this same endpoint; never take printk_lock() from here). */
static void blk_task_body(void *arg) {
    (void)arg;
    while (!g_blk_ep) sched_yield();

    for (;;) {
        uint32_t req_len = chan_serve_wait(g_blk_ep);
        if (req_len < BLK_HDR_LEN) { chan_serve_reply(g_blk_ep, 0); continue; }
        g_blk_calls++;

        uint8_t  op    = g_blk_req[0];
        uint32_t lba   = be32_load(&g_blk_req[1]);
        uint32_t count = be32_load(&g_blk_req[5]);

        if (op == BLK_REQ_READ && count >= 1 && count <= BLK_MAX_COUNT) {
            int rc = spisd_hw_read_blocks(lba, count, &g_blk_resp[1]);
            g_blk_resp[0] = (rc == 0) ? 0 : 1;
            chan_serve_reply(g_blk_ep, (rc == 0) ? (1u + count * 512u) : 1u);
        } else if (op == BLK_REQ_WRITE && count >= 1 && count <= BLK_MAX_COUNT &&
                  req_len >= BLK_HDR_LEN + count * 512u) {
            int rc = spisd_hw_write_blocks(lba, count, &g_blk_req[BLK_HDR_LEN]);
            g_blk_resp[0] = (rc == 0) ? 0 : 1;
            chan_serve_reply(g_blk_ep, 1);
        } else {
            g_blk_resp[0] = 1;
            chan_serve_reply(g_blk_ep, 1);
        }
    }
}

/* Called from kernel/main.c, after sched_init(). Not fatal if it fails:
 * spisd_read_blocks()/spisd_write_blocks() below fall back to direct
 * hardware access whenever the task is not alive, same as every boot-time
 * read before this ever runs. */
int spisd_task_start(void) {
    int pid = task_create_sized("sdblk", blk_task_body, NULL, 1);
    if (pid < 0) {
        printk("[SPI SD] Could not start the sdblk task; storage stays on direct hardware access.\n");
        return -1;
    }
    if (chan_register_task("sdblk", pid, g_blk_req, sizeof(g_blk_req),
                           g_blk_resp, sizeof(g_blk_resp)) != 0) {
        printk("[SPI SD] Could not register the sdblk channel endpoint; falling back to direct hardware access.\n");
        return -1;
    }
    g_blk_ep = chan_lookup("sdblk");
    g_blk_task_pid = pid;
    printk("[SPI SD] Driver running as task #%d, reachable via chan_call(\"sdblk\", ...)\n", pid);
    return pid;
}

static int blk_call_with_retry(const uint8_t *req, uint32_t req_len,
                               uint8_t *resp, uint32_t resp_max) {
    for (int attempt = 0; attempt < 8; attempt++) {
        int n = chan_call(g_blk_ep, req, req_len, resp, resp_max);
        if (n >= 0) return n;
        sched_yield();
    }
    return -1;
}

static int spisd_read_blocks(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    /* Unlike virtio_blk/flashdisk/ramdisk, this driver sends whatever LBA
     * it's given straight to the card over SPI with no bound check of its
     * own. On real hardware that's confined to the SD card's own address
     * space (not a kernel memory-safety issue), but a filesystem-layer bug
     * that computes a bad cluster number could still silently read/write
     * sectors outside the mounted FAT32 volume's real bounds -- checked
     * here, once, regardless of which path (task or direct) serves it. */
    if (!buf || count == 0 || lba + count < lba || lba + count > dev->num_blocks) return -1;

    if (count <= BLK_MAX_COUNT && blk_task_alive()) {
        uint8_t req[BLK_HDR_LEN];
        req[0] = BLK_REQ_READ;
        be32_store(&req[1], lba);
        be32_store(&req[5], count);
        uint8_t resp[BLK_RESP_CAP];
        int n = blk_call_with_retry(req, sizeof(req), resp, sizeof(resp));
        if (n >= 1 && resp[0] == 0 && (uint32_t)n >= 1u + count * 512u) {
            memcpy(buf, &resp[1], count * 512u);
            return 0;
        }
        /* IPC failed or the task answered with an error -- fall through to
         * direct access rather than propagate a failure that might only be
         * about the channel, not the card. */
    }
    return spisd_hw_read_blocks(lba, count, buf);
}

static int spisd_write_blocks(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    if (!buf || count == 0 || lba + count < lba || lba + count > dev->num_blocks) return -1;

    if (count <= BLK_MAX_COUNT && blk_task_alive()) {
        uint8_t req[BLK_HDR_LEN + BLK_MAX_COUNT * 512u];
        req[0] = BLK_REQ_WRITE;
        be32_store(&req[1], lba);
        be32_store(&req[5], count);
        memcpy(&req[BLK_HDR_LEN], buf, count * 512u);
        uint8_t resp[1];
        int n = blk_call_with_retry(req, BLK_HDR_LEN + count * 512u, resp, sizeof(resp));
        if (n >= 1 && resp[0] == 0) return 0;
    }
    return spisd_hw_write_blocks(lba, count, buf);
}

static block_dev_t g_spisd_dev = {
    .name = "spisd0",
    .block_size = 512,
    .num_blocks = 2097152, // Default 1 GB capacity estimate
    .read_blocks = spisd_read_blocks,
    .write_blocks = spisd_write_blocks,
};

block_dev_t *spisd_get_device(void) {
    if (!g_sd_initialized) {
        if (spisd_init_hardware() != 0) {
            return NULL;
        }
    }
    return &g_spisd_dev;
}

#else // !CONFIG_ENABLE_SPISD

block_dev_t *spisd_get_device(void) {
    return NULL;
}

int spisd_task_start(void) {
    return -1;
}

uint32_t blk_task_call_count(void) {
    return 0;
}

#endif // CONFIG_ENABLE_SPISD
