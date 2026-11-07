/*
 * LugalOS Hardware Driver: StarFive JH7110 Synopsys DesignWare MSHC (sdio1).
 *
 * Drives the microSD card slot on the StarFive VisionFive 2 v1.3B, wired to
 * `sdio1@16020000` (Synopsys DesignWare Mobile Storage Host Controller 2.90a,
 * 4-bit bus, 32-word FIFO, 3.3 V SDR signaling).
 *
 * Unlike the ESP32-P4 wrapper where the CPU FIFO read port is unpoppable, the
 * JH7110's `snps,dw-mshc` exposes a working 32-bit FIFO data port at offset
 * +0x200 (`SD_DATA`). Driving block transfers via polled 32-bit FIFO PIO
 * avoids non-coherent DMA descriptor rings and L2 cache flush hazards across
 * the four U74 SMP application harts.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "drivers/sdmmc.h"
#include "drivers/driver_task.h"
#include "kernel/lock.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "lugalos_config.h"

#if CONFIG_ENABLE_SDMMC

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* --- JH7110 SYSCRG clocks and resets for sdio1 (0x13020000) ---------------
 *
 * From `dt-bindings/clock/starfive-jh7110-clkgen.h` and `clk-jh7110.c`:
 *   - JH7110_SDIO1_CLK_AHB    = 92 (SYSCRG + 0x170): bit 31 = clock gate
 *   - JH7110_SDIO1_CLK_SDCARD = 94 (SYSCRG + 0x178): bit 31 = clock gate,
 *     bits [3:0] = 1-based divider from `axi_cfg0` (396 MHz; U-Boot sets
 *     divider = 2 -> ~198 MHz CIU source clock).
 *   - RSTN_U1_DW_SDIO_AHB     = 65 (bit 1 of SYSCRG_RESET_ASSERT2 at
 *     SYSCRG + 0x300).
 */
#define SYSCRG_BASE                 0x13020000UL
#define SYSCRG_SDIO1_CLK_AHB        (SYSCRG_BASE + 0x170UL)
#define SYSCRG_SDIO1_CLK_SDCARD     (SYSCRG_BASE + 0x178UL)
#define SYSCRG_RESET_ASSERT2        (SYSCRG_BASE + 0x300UL)
#define SYSCRG_RESET_STATUS2        (SYSCRG_BASE + 0x310UL)
#define  SYSCRG_CLK_ENABLE          (1u << 31)
#define  SYSCRG_RST_SDIO1_AHB       (1u << 1)
#define JH7110_AXI_CFG0_HZ          396000000u

/* --- Synopsys DesignWare MSHC registers (`sdio1` at 0x16020000) ----------- */
#define SD_BASE             ((uintptr_t)CONFIG_SDMMC_BASE)
#define SD_CTRL             (SD_BASE + 0x000UL)
#define  CTRL_RESET             (1u << 0)
#define  CTRL_FIFO_RESET        (1u << 1)
#define  CTRL_DMA_RESET         (1u << 2)
#define  CTRL_INT_ENABLE        (1u << 4)
#define  CTRL_DMA_ENABLE        (1u << 5)
#define  CTRL_USE_INTERNAL_DMA  (1u << 25)
#define SD_PWREN            (SD_BASE + 0x004UL)
#define SD_CLKDIV           (SD_BASE + 0x008UL)
#define SD_CLKSRC           (SD_BASE + 0x00cUL)
#define SD_CLKENA           (SD_BASE + 0x010UL)
#define  CLKENA_CCLK_EN_0       (1u << 0)
#define  CLKENA_LOW_PWR_0       (1u << 16)
#define SD_TMOUT            (SD_BASE + 0x014UL)
#define SD_CTYPE            (SD_BASE + 0x018UL)
#define  CTYPE_WIDTH4_0         (1u << 0)
#define SD_BLKSIZ           (SD_BASE + 0x01cUL)
#define SD_BYTCNT           (SD_BASE + 0x020UL)
#define SD_INTMASK          (SD_BASE + 0x024UL)
#define SD_CMDARG           (SD_BASE + 0x028UL)
#define SD_CMD              (SD_BASE + 0x02cUL)
#define SD_RESP(n)          (SD_BASE + 0x030UL + 4u * (uint32_t)(n))
#define SD_RINTSTS          (SD_BASE + 0x044UL)
#define SD_STATUS           (SD_BASE + 0x048UL)
#define  STATUS_FIFO_EMPTY      (1u << 2)
#define  STATUS_FIFO_FULL       (1u << 3)
#define  STATUS_DATA_BUSY       (1u << 9)
#define  STATUS_DATA_FSM_BUSY   (1u << 10)
#define  STATUS_FIFO_COUNT_S    17
#define  STATUS_FIFO_COUNT_M    0x1fffu
#define SD_FIFOTH           (SD_BASE + 0x04cUL)
#define SD_CDETECT          (SD_BASE + 0x050UL)
#define SD_WRTPRT           (SD_BASE + 0x054UL)
#define SD_VERID            (SD_BASE + 0x06cUL)
#define SD_HCON             (SD_BASE + 0x070UL)
#define SD_UHS_REG          (SD_BASE + 0x074UL)
#define SD_RST_N            (SD_BASE + 0x078UL)
#define  RST_N_CARD_RESET_0     (1u << 0)
#define SD_BMOD             (SD_BASE + 0x080UL)
#define  BMOD_SWR               (1u << 0)
#define SD_IDSTS            (SD_BASE + 0x08cUL)
#define SD_IDINTEN          (SD_BASE + 0x090UL)
#define SD_UHS_REG_EXT      (SD_BASE + 0x108UL)
#define SD_DATA             (SD_BASE + 0x200UL)

#define SD_FIFO_DEPTH       32u

/* SD_CMD fields */
#define  CMD_INDEX_M            0x3fu
#define  CMD_RESP_EXPECT        (1u << 6)
#define  CMD_RESP_LONG          (1u << 7)
#define  CMD_CHECK_RESP_CRC     (1u << 8)
#define  CMD_DATA_EXPECTED      (1u << 9)
#define  CMD_WRITE              (1u << 10)
#define  CMD_WAIT_PRVDATA       (1u << 13)
#define  CMD_STOP_ABORT         (1u << 14)
#define  CMD_SEND_INIT          (1u << 15)
#define  CMD_UPDATE_CLK_ONLY    (1u << 21)
#define  CMD_USE_HOLD_REG       (1u << 29)
#define  CMD_START              (1u << 31)

/* SD_RINTSTS fields */
#define  INT_CD                 (1u << 0)
#define  INT_RESP_ERR           (1u << 1)
#define  INT_CMD_DONE           (1u << 2)
#define  INT_DATA_OVER          (1u << 3)
#define  INT_TXDR               (1u << 4)
#define  INT_RXDR               (1u << 5)
#define  INT_RCRC               (1u << 6)
#define  INT_DCRC               (1u << 7)
#define  INT_RTO                (1u << 8)
#define  INT_DRTO               (1u << 9)
#define  INT_HTO                (1u << 10)
#define  INT_FRUN               (1u << 11)
#define  INT_HLE                (1u << 12)
#define  INT_SBE                (1u << 13)
#define  INT_EBE                (1u << 15)
#define  INT_CMD_ERRORS         (INT_RESP_ERR | INT_RCRC | INT_RTO)
#define  INT_DATA_ERRORS        (INT_DCRC | INT_DRTO | INT_HTO | INT_FRUN | \
                                 INT_HLE | INT_SBE | INT_EBE)

typedef enum {
    RESP_NONE = 0,
    RESP_R1,
    RESP_R1B,
    RESP_R2,
    RESP_R3,
} sd_resp_t;

static ylock_t  g_host_lock;
/* guarded by: g_host_lock for all controller/card state. */
static bool     g_hw_ready;
static bool     g_present;
static bool     g_block_addressed;
static uint32_t g_ciu_hz = 198000000u;
static uint32_t g_rca;
static uint32_t g_blocks;
static uint32_t g_bus_width = 1;
static uint32_t g_bus_khz;
static uint32_t g_cid[4];
static uint32_t g_stale_transfers;
static uint8_t  g_probe_block[512];

static uint32_t sdmmc_fifo_count(void) {
    return (REG(SD_STATUS) >> STATUS_FIFO_COUNT_S) & STATUS_FIFO_COUNT_M;
}

static void sdmmc_clk_init(void) {
    /* Ensure AHB BIU and SDCARD CIU clocks are ungated in SYSCRG. */
    REG(SYSCRG_SDIO1_CLK_AHB) |= SYSCRG_CLK_ENABLE;
    uint32_t sdcard_clk = REG(SYSCRG_SDIO1_CLK_SDCARD);
    uint32_t div = sdcard_clk & 0x0fu;
    if (div == 0u) div = 2u;
    REG(SYSCRG_SDIO1_CLK_SDCARD) = SYSCRG_CLK_ENABLE | div;
    g_ciu_hz = JH7110_AXI_CFG0_HZ / div;

    /* Ensure RSTN_U1_DW_SDIO_AHB is deasserted. */
    if (REG(SYSCRG_RESET_ASSERT2) & SYSCRG_RST_SDIO1_AHB) {
        REG(SYSCRG_RESET_ASSERT2) &= ~SYSCRG_RST_SDIO1_AHB;
        for (uint32_t i = 0; i < 10000u; i++) {
            if (REG(SYSCRG_RESET_STATUS2) & SYSCRG_RST_SDIO1_AHB) break;
        }
    }
}

static int sdmmc_cmd_start(uint32_t cmd, uint32_t arg) {
    uint64_t deadline = time_get_us() + 100000u;

    if (!time_poll_until_us(!(REG(SD_CMD) & CMD_START), deadline)) return -1;
    REG(SD_CMDARG) = arg;
    REG(SD_CMD) = cmd | CMD_START | CMD_USE_HOLD_REG;
    if (!time_poll_until_us(!(REG(SD_CMD) & CMD_START), deadline)) return -1;
    return 0;
}

static int sdmmc_clock_update(void) {
    return sdmmc_cmd_start(CMD_UPDATE_CLK_ONLY | CMD_WAIT_PRVDATA, 0);
}

static uint32_t sdmmc_set_bus_clock(uint32_t want_khz) {
    uint32_t want_hz = want_khz * 1000u;
    uint32_t card_div = 0;
    if (want_hz < g_ciu_hz) {
        card_div = (g_ciu_hz + 2u * want_hz - 1u) / (2u * want_hz);
        if (card_div > 255u) card_div = 255u;
    }

    REG(SD_CLKENA) = 0;
    if (sdmmc_clock_update() != 0) return 0;

    REG(SD_CLKSRC) = 0;
    REG(SD_CLKDIV) = card_div;
    if (sdmmc_clock_update() != 0) return 0;

    REG(SD_CLKENA) = CLKENA_CCLK_EN_0 | CLKENA_LOW_PWR_0;
    if (sdmmc_clock_update() != 0) return 0;

    REG(SD_TMOUT) = 0xffffffffu;
    uint32_t real_hz = g_ciu_hz / (card_div ? (2u * card_div) : 1u);
    return real_hz / 1000u;
}

static int sdmmc_host_reset(uint32_t mask) {
    REG(SD_CTRL) |= mask;
    if (!time_poll_until_us(!(REG(SD_CTRL) & mask), time_get_us() + 100000u))
        return -1;
    return 0;
}

static void sdmmc_data_reset(void) {
    uint64_t deadline = time_get_us() + 1500000u;
    while ((REG(SD_STATUS) & STATUS_DATA_FSM_BUSY) && time_get_us() < deadline) { }
    if (REG(SD_STATUS) & STATUS_DATA_FSM_BUSY) {
        printk("[SDMMC] data state machine stuck; resetting controller\n");
        (void)sdmmc_host_reset(CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET);
        (void)sdmmc_clock_update();
    }
    (void)sdmmc_host_reset(CTRL_FIFO_RESET | CTRL_DMA_RESET);
    REG(SD_BMOD) = BMOD_SWR;
    REG(SD_BMOD) = 0;
    REG(SD_CTRL) &= ~(CTRL_DMA_ENABLE | CTRL_USE_INTERNAL_DMA);
    REG(SD_IDSTS) = 0xffffffffu;
    REG(SD_RINTSTS) = 0xffffffffu;
}

static void sdmmc_set_width4(bool four) {
    REG(SD_CTYPE) = four ? CTYPE_WIDTH4_0 : 0u;
}

static int sd_command(uint8_t index, uint32_t arg, sd_resp_t rt,
                      uint32_t data_flags, uint32_t *resp, uint32_t *out_status) {
    uint32_t cmd = (uint32_t)index & CMD_INDEX_M;

    switch (rt) {
        case RESP_NONE: break;
        case RESP_R1:
        case RESP_R1B:  cmd |= CMD_RESP_EXPECT | CMD_CHECK_RESP_CRC; break;
        case RESP_R2:   cmd |= CMD_RESP_EXPECT | CMD_RESP_LONG | CMD_CHECK_RESP_CRC; break;
        case RESP_R3:   cmd |= CMD_RESP_EXPECT; break;
    }
    cmd |= data_flags;
    if (!(data_flags & CMD_STOP_ABORT)) cmd |= CMD_WAIT_PRVDATA;

    if ((data_flags & CMD_DATA_EXPECTED) &&
        (REG(SD_STATUS) & (STATUS_DATA_BUSY | STATUS_DATA_FSM_BUSY))) {
        uint64_t bwait = time_get_us() + 500000u;
        while ((REG(SD_STATUS) & (STATUS_DATA_BUSY | STATUS_DATA_FSM_BUSY)) &&
               time_get_us() < bwait) { }
        if (REG(SD_STATUS) & STATUS_DATA_FSM_BUSY) {
            g_stale_transfers++;
            printk("[SDMMC] data path busy from an earlier transfer; resetting\n");
            sdmmc_data_reset();
        }
    }

    REG(SD_RINTSTS) = 0xffffffffu;
    if (sdmmc_cmd_start(cmd, arg) != 0) {
        if (out_status) *out_status = 0;
        return -1;
    }

    uint64_t deadline = time_get_us() + 500000u;
    uint32_t status;
    for (;;) {
        bool late = time_get_us() >= deadline;
        status = REG(SD_RINTSTS);
        if (status & (INT_CMD_DONE | INT_CMD_ERRORS)) break;
        if (late) {
            if (out_status) *out_status = status;
            return -1;
        }
    }
    if (out_status) *out_status = status;
    if (status & INT_CMD_ERRORS) return -1;

    if (resp) {
        if (rt == RESP_R2) {
            resp[0] = REG(SD_RESP(0));
            resp[1] = REG(SD_RESP(1));
            resp[2] = REG(SD_RESP(2));
            resp[3] = REG(SD_RESP(3));
        } else if (rt != RESP_NONE) {
            resp[0] = REG(SD_RESP(0));
        }
    }

    if (rt == RESP_R1B) {
        if (!time_poll_until_us(!(REG(SD_STATUS) & STATUS_DATA_BUSY),
                                time_get_us() + 1000000u))
            return -1;
    }
    return 0;
}

static int sd_acommand(uint8_t index, uint32_t arg, sd_resp_t rt,
                       uint32_t *resp, uint32_t *out_status) {
    if (sd_command(55, g_rca << 16, RESP_R1, 0, NULL, out_status) != 0) return -1;
    return sd_command(index, arg, rt, 0, resp, out_status);
}

static uint32_t sd_bits(const uint32_t *r, uint32_t start, uint32_t len) {
    uint32_t mask = (len % 32u == 0u) ? 0xffffffffu : (0xffffffffu >> (32u - (len % 32u)));
    uint32_t word = start / 32u;
    uint32_t shift = start % 32u;
    if (word > 3u) return 0;
    uint32_t right = r[word] >> shift;
    uint32_t left = (len + shift <= 32u || word >= 3u) ? 0u : (r[word + 1u] << ((32u - shift) % 32u));
    return (left | right) & mask;
}

static uint32_t sd_csd_blocks(const uint32_t *csd) {
    uint32_t ver = sd_bits(csd, 126, 2);
    if (ver == 1u) {
        return (sd_bits(csd, 48, 22) + 1u) << 10;
    }
    if (ver == 0u) {
        uint32_t c_size = sd_bits(csd, 62, 12);
        uint32_t c_mult = sd_bits(csd, 47, 3);
        uint32_t bl_len = sd_bits(csd, 80, 4);
        uint32_t blocks = (c_size + 1u) << (c_mult + 2u);
        if (bl_len > 9u) blocks <<= (bl_len - 9u);
        return blocks;
    }
    return 0;
}

static void sdmmc_pio_prepare(uint32_t blksz, uint32_t bytes) {
    (void)sdmmc_host_reset(CTRL_FIFO_RESET);
    REG(SD_CTRL) &= ~(CTRL_DMA_ENABLE | CTRL_USE_INTERNAL_DMA);
    REG(SD_BMOD) = 0;
    REG(SD_BLKSIZ) = blksz;
    REG(SD_BYTCNT) = bytes;
}

static int sdmmc_pio_read(uint32_t tag, uint8_t *dst, uint32_t bytes) {
    uint32_t words_left = bytes / 4u;
    uint64_t deadline = time_get_us() + 3000000u;

    for (;;) {
        bool late = time_get_us() >= deadline;
        uint32_t st = REG(SD_RINTSTS);
        if (st & INT_DATA_ERRORS) {
            printk("[SDMMC] read lba %u failed (RINTSTS 0x%x)\n",
                   (unsigned)tag, (unsigned)st);
            return -1;
        }
        uint32_t fcnt = sdmmc_fifo_count();
        while (fcnt > 0u && words_left > 0u) {
            uint32_t w = REG(SD_DATA);
            memcpy(dst, &w, 4u);
            dst += 4u;
            words_left--;
            fcnt--;
        }
        if (words_left == 0u && (st & INT_DATA_OVER)) {
            REG(SD_RINTSTS) = 0xffffffffu;
            return 0;
        }
        if (late) {
            printk("[SDMMC] read lba %u timed out (RINTSTS 0x%x FIFO %u left %u)\n",
                   (unsigned)tag, (unsigned)st, (unsigned)sdmmc_fifo_count(),
                   (unsigned)words_left);
            return -1;
        }
    }
}

static int sdmmc_pio_write(uint32_t tag, const uint8_t *src, uint32_t bytes) {
    uint32_t words_left = bytes / 4u;
    uint64_t deadline = time_get_us() + 3000000u;

    for (;;) {
        bool late = time_get_us() >= deadline;
        uint32_t st = REG(SD_RINTSTS);
        if (st & INT_DATA_ERRORS) {
            printk("[SDMMC] write lba %u failed (RINTSTS 0x%x)\n",
                   (unsigned)tag, (unsigned)st);
            return -1;
        }
        if (words_left > 0u) {
            uint32_t status = REG(SD_STATUS);
            if (!(status & STATUS_FIFO_FULL)) {
                uint32_t fcnt = (status >> STATUS_FIFO_COUNT_S) & STATUS_FIFO_COUNT_M;
                uint32_t free_w = (fcnt < SD_FIFO_DEPTH) ? (SD_FIFO_DEPTH - fcnt) : 0u;
                if (free_w > words_left) free_w = words_left;
                while (free_w > 0u) {
                    uint32_t w;
                    memcpy(&w, src, 4u);
                    REG(SD_DATA) = w;
                    src += 4u;
                    words_left--;
                    free_w--;
                }
            }
            if (st & INT_TXDR) REG(SD_RINTSTS) = INT_TXDR;
        }
        if (words_left == 0u && (st & INT_DATA_OVER)) {
            REG(SD_RINTSTS) = 0xffffffffu;
            break;
        }
        if (late) {
            printk("[SDMMC] write lba %u timed out (RINTSTS 0x%x FIFO %u left %u)\n",
                   (unsigned)tag, (unsigned)st, (unsigned)sdmmc_fifo_count(),
                   (unsigned)words_left);
            return -1;
        }
    }

    if (!time_poll_until_us(!(REG(SD_STATUS) & STATUS_DATA_BUSY),
                            time_get_us() + 1000000u)) {
        printk("[SDMMC] write lba %u: card still busy after 1 s\n", (unsigned)tag);
        return -1;
    }
    return 0;
}

static int sdmmc_hw_read_block(uint32_t lba, uint8_t *dst) {
    sdmmc_pio_prepare(512u, 512u);
    uint32_t addr = g_block_addressed ? lba : (lba * 512u);
    uint32_t status = 0;
    if (sd_command(17, addr, RESP_R1, CMD_DATA_EXPECTED, NULL, &status) != 0) {
        printk("[SDMMC] CMD17 lba %u failed (status 0x%x)\n",
               (unsigned)lba, (unsigned)status);
        return -1;
    }
    if (sdmmc_pio_read(lba, dst, 512u) != 0) {
        sdmmc_data_reset();
        return -1;
    }
    return 0;
}

static int sdmmc_hw_write_block(uint32_t lba, const uint8_t *src) {
    sdmmc_pio_prepare(512u, 512u);
    uint32_t addr = g_block_addressed ? lba : (lba * 512u);
    uint32_t status = 0;
    if (sd_command(24, addr, RESP_R1, CMD_DATA_EXPECTED | CMD_WRITE, NULL, &status) != 0) {
        printk("[SDMMC] CMD24 lba %u failed (status 0x%x)\n",
               (unsigned)lba, (unsigned)status);
        return -1;
    }
    if (sdmmc_pio_write(lba, src, 512u) != 0) {
        sdmmc_data_reset();
        return -1;
    }
    return 0;
}

static bool sdmmc_switch_high_speed(void) {
    uint8_t status_blk[64];
    sdmmc_pio_prepare(64u, 64u);
    uint32_t status = 0;
    if (sd_command(6, 0x80FFFFF1u, RESP_R1, CMD_DATA_EXPECTED, NULL, &status) != 0) {
        printk("[SDMMC] CMD6 (high speed) failed (status 0x%x)\n", (unsigned)status);
        sdmmc_data_reset();
        return false;
    }
    if (sdmmc_pio_read(0, status_blk, 64u) != 0) {
        sdmmc_data_reset();
        return false;
    }
    return (status_blk[16] & 0x0Fu) == 1u;
}

static void sdmmc_set_bus_width(void) {
    uint32_t want = (uint32_t)CONFIG_SDMMC_BUS_WIDTH;
    uint32_t status = 0;

    if (want == 4u) {
        if (sd_acommand(6, 2, RESP_R1, NULL, &status) == 0) {
            sdmmc_set_width4(true);
            g_bus_width = 4;
        } else {
            printk("[SDMMC] ACMD6 refused 4-bit (status 0x%x); staying 1-bit\n",
                   (unsigned)status);
        }
    }

    uint32_t khz = (uint32_t)CONFIG_SDMMC_FREQ_KHZ;
    if (khz > 25000u && !sdmmc_switch_high_speed()) {
        printk("[SDMMC] card did not switch to high speed; 25 MHz instead of %u\n",
               (unsigned)khz);
        khz = 25000u;
    }
    g_bus_khz = sdmmc_set_bus_clock(khz);

    if (sdmmc_hw_read_block(0, g_probe_block) == 0) return;

    if (g_bus_width == 4u) {
        printk("[SDMMC] 4-bit bus did not read back; falling back to 1-bit\n");
        if (sd_acommand(6, 0, RESP_R1, NULL, &status) == 0) {
            sdmmc_set_width4(false);
            g_bus_width = 1;
            if (sdmmc_hw_read_block(0, g_probe_block) == 0) return;
        }
    }

    printk("[SDMMC] block 0 unreadable at %u kHz, %u-bit\n",
           (unsigned)g_bus_khz, (unsigned)g_bus_width);
}

static void sdmmc_host_init(void) {
    if (g_hw_ready) return;

    sdmmc_clk_init();
    REG(SD_PWREN) = 1u;
    REG(SD_RST_N) = RST_N_CARD_RESET_0;
    REG(SD_UHS_REG) = 0u;

    if (sdmmc_host_reset(CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET) != 0) {
        printk("[SDMMC] controller reset never completed -- no module clock?\n");
        return;
    }

    REG(SD_INTMASK) = 0u;
    REG(SD_CTRL) &= ~(CTRL_INT_ENABLE | CTRL_DMA_ENABLE | CTRL_USE_INTERNAL_DMA);
    REG(SD_RINTSTS) = 0xffffffffu;
    REG(SD_IDINTEN) = 0u;
    REG(SD_IDSTS) = 0xffffffffu;
    REG(SD_BMOD) = BMOD_SWR;
    REG(SD_BMOD) = 0u;

    /* 32-word FIFO: MSIZE = 2 (8 transfers), RX_WMARK = 15, TX_WMARK = 16. */
    REG(SD_FIFOTH) = (2u << 28) | ((SD_FIFO_DEPTH / 2u - 1u) << 16) | (SD_FIFO_DEPTH / 2u);

    sdmmc_set_width4(false);
    g_bus_width = 1;
    g_bus_khz = sdmmc_set_bus_clock(400);
    g_hw_ready = true;
}

static int sdmmc_card_init_locked(void) {
    uint32_t resp[4] = {0, 0, 0, 0};
    uint32_t status = 0;

    sdmmc_host_init();
    if (!g_hw_ready) return -1;

    g_present = false;
    g_block_addressed = false;
    g_rca = 0;
    g_blocks = 0;

    sdmmc_set_width4(false);
    g_bus_width = 1;
    g_bus_khz = sdmmc_set_bus_clock(400);

    if (sd_command(0, 0, RESP_NONE, CMD_SEND_INIT, NULL, &status) != 0) {
        printk("[SDMMC] CMD0 was not accepted (status 0x%x)\n", (unsigned)status);
        return -1;
    }

    bool v2 = false;
    if (sd_command(8, 0x1AAu, RESP_R1, 0, resp, &status) == 0 &&
        (resp[0] & 0xffu) == 0xAAu) {
        v2 = true;
    }

    uint32_t ocr = 0;
    uint64_t deadline = time_get_ms() + 1500u;
    unsigned tries = 0;
    for (;;) {
        uint32_t arg = 0x00FF8000u | (v2 ? 0x40000000u : 0u);
        if (sd_acommand(41, arg, RESP_R3, resp, &status) != 0) {
            if (tries == 0) return -1;
            printk("[SDMMC] ACMD41 stopped answering after %u tries (status 0x%x)\n",
                   tries, (unsigned)status);
            return -1;
        }
        tries++;
        ocr = resp[0];
        if (ocr & 0x80000000u) break;
        if (time_get_ms() > deadline) {
            printk("[SDMMC] ACMD41 did not leave idle within 1500 ms (OCR 0x%x)\n",
                   (unsigned)ocr);
            return -1;
        }
    }
    g_block_addressed = v2 && (ocr & 0x40000000u) != 0u;

    if (sd_command(2, 0, RESP_R2, 0, g_cid, &status) != 0) {
        printk("[SDMMC] CMD2 (CID) failed (status 0x%x)\n", (unsigned)status);
        return -1;
    }
    if (sd_command(3, 0, RESP_R1, 0, resp, &status) != 0) {
        printk("[SDMMC] CMD3 (publish RCA) failed (status 0x%x)\n", (unsigned)status);
        return -1;
    }
    g_rca = resp[0] >> 16;

    if (sd_command(9, g_rca << 16, RESP_R2, 0, resp, &status) != 0) {
        printk("[SDMMC] CMD9 (CSD) failed (status 0x%x)\n", (unsigned)status);
        return -1;
    }
    g_blocks = sd_csd_blocks(resp);
    if (g_blocks == 0) {
        printk("[SDMMC] CSD structure %u not understood; capacity unknown\n",
               (unsigned)sd_bits(resp, 126, 2));
        return -1;
    }

    if (sd_command(7, g_rca << 16, RESP_R1B, 0, NULL, &status) != 0) {
        printk("[SDMMC] CMD7 (select) failed (status 0x%x)\n", (unsigned)status);
        return -1;
    }

    if (sd_command(16, 512, RESP_R1, 0, NULL, &status) != 0) {
        printk("[SDMMC] CMD16 (512-byte blocks) failed (status 0x%x)\n", (unsigned)status);
        return -1;
    }

    sdmmc_set_bus_width();

    g_present = true;
    printk("[SDMMC] MicroSD %u MB (%s, %u-bit, %u kHz) on JH7110 sdio1@%lx\n",
           (unsigned)(g_blocks / 2048u),
           g_block_addressed ? "SDHC/SDXC block-addressed" : "SD byte-addressed",
           (unsigned)g_bus_width, (unsigned)g_bus_khz,
           (unsigned long)SD_BASE);
    return 0;
}

static int sdmmc_card_init(void) {
    ylock_acquire(&g_host_lock);
    int rc = sdmmc_card_init_locked();
    ylock_release(&g_host_lock);
    return rc;
}

/* --- The "sdblk" driver task --------------------------------------------- */
#define BLK_REQ_READ  ((uint8_t)'R')
#define BLK_REQ_WRITE ((uint8_t)'W')
#define BLK_MAX_COUNT 4u
#define BLK_HDR_LEN   13u
#define BLK_RESP_HDR  5u
#define BLK_REQ_CAP   (BLK_HDR_LEN + BLK_MAX_COUNT * 512u)
#define BLK_RESP_HDR_CAP (BLK_RESP_HDR + BLK_MAX_COUNT * 512u)

/* guarded by: atomic increments (g_blk_tag, g_blk_mixups). */
static uint32_t g_blk_tag, g_blk_mixups;

/* single task: owned by the sdblk driver task (`blk_serve`). */
static uint8_t       g_blk_req[BLK_REQ_CAP];
static uint8_t       g_blk_resp[BLK_RESP_HDR_CAP];
/* guarded by: g_host_lock and driver_task internal state. */
static driver_task_t g_blk_task;

uint32_t blk_task_call_count(void) { return driver_task_call_count(&g_blk_task); }

static bool blk_task_alive(void) { return driver_task_alive(&g_blk_task); }

static uint32_t be32_load(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void be32_store(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int sdmmc_hw_read_blocks(uint32_t lba, uint32_t count, uint8_t *dst) {
    if (!g_present) return -1;
    int rc = 0;
    ylock_acquire(&g_host_lock);
    for (uint32_t i = 0; i < count && rc == 0; i++) {
        if (sdmmc_hw_read_block(lba + i, dst + (size_t)i * 512u) != 0) rc = -1;
    }
    ylock_release(&g_host_lock);
    return rc;
}

static int sdmmc_hw_write_blocks(uint32_t lba, uint32_t count, const uint8_t *src) {
    if (!g_present) return -1;
    int rc = 0;
    ylock_acquire(&g_host_lock);
    for (uint32_t i = 0; i < count && rc == 0; i++) {
        if (sdmmc_hw_write_block(lba + i, src + (size_t)i * 512u) != 0) rc = -1;
    }
    ylock_release(&g_host_lock);
    return rc;
}

static uint32_t blk_serve(void *ctx, const uint8_t *req, uint32_t req_len,
                          uint8_t *resp, uint32_t resp_cap) {
    (void)ctx; (void)resp_cap;

    uint8_t  op    = req[0];
    uint32_t lba   = be32_load(&req[1]);
    uint32_t count = be32_load(&req[5]);
    be32_store(&resp[1], req_len >= BLK_HDR_LEN ? be32_load(&req[9]) : 0u);

    if (op == BLK_REQ_READ && req_len >= BLK_HDR_LEN && count >= 1 && count <= BLK_MAX_COUNT) {
        int rc = sdmmc_hw_read_blocks(lba, count, &resp[BLK_RESP_HDR]);
        resp[0] = (rc == 0) ? 0 : 1;
        return (rc == 0) ? (BLK_RESP_HDR + count * 512u) : BLK_RESP_HDR;
    }
    if (op == BLK_REQ_WRITE && count >= 1 && count <= BLK_MAX_COUNT &&
        req_len >= BLK_HDR_LEN + count * 512u) {
        int rc = sdmmc_hw_write_blocks(lba, count, &req[BLK_HDR_LEN]);
        resp[0] = (rc == 0) ? 0 : 1;
        return BLK_RESP_HDR;
    }
    resp[0] = 1;
    return BLK_RESP_HDR;
}

int sdmmc_task_start(void) {
    if (!g_present) return -1;
    const driver_task_spec_t spec = {
        .name        = "sdblk",
        .serve       = blk_serve,
        .ctx         = NULL,
        .req         = g_blk_req,  .req_cap  = sizeof(g_blk_req),
        .resp        = g_blk_resp, .resp_cap = sizeof(g_blk_resp),
        .min_req_len = BLK_HDR_LEN,
        .stack_pages = 1,
        .priority    = DRIVER_PRIO_DEFAULT,
    };
    return driver_task_start(&g_blk_task, &spec);
}

#define BLK_CALL_WAIT_MS 500u

static bool blk_reply_is_mine(int n, const uint8_t *resp, uint32_t tag, uint32_t lba, const char *what) {
    if (n < (int)BLK_RESP_HDR || be32_load(&resp[1]) == tag) return true;
    __atomic_fetch_add(&g_blk_mixups, 1u, __ATOMIC_RELAXED);
    printk("[SDMMC] %s lba %lu: reply tag mismatch (got %lu, sent %lu)\n", what,
           (unsigned long)lba, (unsigned long)be32_load(&resp[1]), (unsigned long)tag);
    return false;
}

static int blk_read_chunk(uint32_t lba, uint32_t count, uint8_t *dst) {
    uint8_t req[BLK_HDR_LEN];
    uint32_t tag = __atomic_add_fetch(&g_blk_tag, 1u, __ATOMIC_RELAXED);
    req[0] = BLK_REQ_READ;
    be32_store(&req[1], lba);
    be32_store(&req[5], count);
    be32_store(&req[9], tag);
    uint8_t resp[BLK_RESP_HDR_CAP];
    uint64_t t0 = time_get_ms();
    int n = driver_task_call_wait(&g_blk_task, req, sizeof(req), resp, sizeof(resp),
                                  BLK_CALL_WAIT_MS);
    if (!blk_reply_is_mine(n, resp, tag, lba, "read")) return -1;
    if (n >= 1 && resp[0] == 0 && (uint32_t)n >= BLK_RESP_HDR + count * 512u) {
        memcpy(dst, &resp[BLK_RESP_HDR], count * 512u);
        return (int)count;
    }
    printk("[SDMMC] read lba %lu x%lu failed after %lu ms (call %d, status %d)\n",
           (unsigned long)lba, (unsigned long)count, (unsigned long)(time_get_ms() - t0), n,
           n >= 1 ? (int)resp[0] : -1);
    return -1;
}

static int blk_write_chunk(uint32_t lba, uint32_t count, const uint8_t *src) {
    uint8_t req[BLK_REQ_CAP];
    uint32_t tag = __atomic_add_fetch(&g_blk_tag, 1u, __ATOMIC_RELAXED);
    req[0] = BLK_REQ_WRITE;
    be32_store(&req[1], lba);
    be32_store(&req[5], count);
    be32_store(&req[9], tag);
    memcpy(&req[BLK_HDR_LEN], src, count * 512u);
    uint8_t resp[BLK_RESP_HDR];
    uint64_t t0 = time_get_ms();
    int n = driver_task_call_wait(&g_blk_task, req, BLK_HDR_LEN + count * 512u,
                                  resp, sizeof(resp), BLK_CALL_WAIT_MS);
    if (!blk_reply_is_mine(n, resp, tag, lba, "write")) return -1;
    if (n >= 1 && resp[0] == 0) return (int)count;
    printk("[SDMMC] write lba %lu x%lu failed after %lu ms (call %d, status %d)\n",
           (unsigned long)lba, (unsigned long)count, (unsigned long)(time_get_ms() - t0), n,
           n >= 1 ? (int)resp[0] : -1);
    return -1;
}

static int sdmmc_read_blocks(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    if (!buf || count == 0 || lba + count < lba || lba + count > dev->num_blocks) return -1;

    uint32_t done = 0;
    uint8_t *dst = (uint8_t *)buf;
    if (blk_task_alive()) {
        while (done < count) {
            uint32_t chunk = (count - done) > BLK_MAX_COUNT ? BLK_MAX_COUNT : (count - done);
            int n = blk_read_chunk(lba + done, chunk, dst + (size_t)done * 512u);
            if (n < 0) return -1;
            done += (uint32_t)n;
        }
        return 0;
    }
    return sdmmc_hw_read_blocks(lba, count, dst);
}

static int sdmmc_write_blocks(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    if (!buf || count == 0 || lba + count < lba || lba + count > dev->num_blocks) return -1;

    uint32_t done = 0;
    const uint8_t *src = (const uint8_t *)buf;
    if (blk_task_alive()) {
        while (done < count) {
            uint32_t chunk = (count - done) > BLK_MAX_COUNT ? BLK_MAX_COUNT : (count - done);
            int n = blk_write_chunk(lba + done, chunk, src + (size_t)done * 512u);
            if (n < 0) return -1;
            done += (uint32_t)n;
        }
        return 0;
    }
    return sdmmc_hw_write_blocks(lba, count, src);
}

/* guarded by: g_host_lock for num_blocks; other fields are constant. */
static block_dev_t g_sdmmc_dev = {
    .name = "sdmmc0",
    .block_size = 512,
    .num_blocks = 0,
    .read_blocks = sdmmc_read_blocks,
    .write_blocks = sdmmc_write_blocks,
};

block_dev_t *sdmmc_get_device(void) {
    if (!g_present) {
        if (sdmmc_card_init() != 0) return NULL;
        g_sdmmc_dev.num_blocks = g_blocks;
    }
    return &g_sdmmc_dev;
}

/* --- `sdinfo` and `sdbench` ---------------------------------------------- */

void sdmmc_bench_report(uint32_t kilobytes) {
    block_dev_t *dev = sdmmc_get_device();
    if (!dev) {
        cprintf("sdbench: no card\n");
        return;
    }
    if (kilobytes == 0 || kilobytes > 8192u) kilobytes = 1024u;

    uint32_t blocks = kilobytes * 2u;
    if (blocks > dev->num_blocks) blocks = dev->num_blocks;

    /* single task: shell command (`sdbench`), serialized by the console session. */
    static uint8_t buf[512 * 4];
    uint32_t done = 0;
    uint64_t t0 = time_get_us();
    while (done < blocks) {
        uint32_t chunk = (blocks - done) > 4u ? 4u : (blocks - done);
        if (dev->read_blocks(dev, buf, done, chunk) != 0) {
            cprintf("sdbench: read failed at lba %u\n", (unsigned)done);
            return;
        }
        done += chunk;
    }
    uint64_t us = time_get_us() - t0;
    if (us == 0) us = 1;

    uint32_t kb = done / 2u;
    uint32_t kb_per_s = (uint32_t)(((uint64_t)kb * 1000000u) / us);
    cprintf("sdbench: %u KB in %u ms = %u KB/s (%u-bit at %u kHz, %u blocks, "
            "4 per request)\n",
            (unsigned)kb, (unsigned)(us / 1000u), (unsigned)kb_per_s,
            (unsigned)g_bus_width, (unsigned)g_bus_khz, (unsigned)done);
}

void sdmmc_info_report(void) {
    if (!g_present) {
        (void)sdmmc_get_device();
    }
    if (!g_present) {
        cprintf("SDMMC sdio1@%lx: no card\n", (unsigned long)SD_BASE);
        return;
    }

    char pnm[6];
    for (int i = 0; i < 5; i++) {
        pnm[i] = (char)sd_bits(g_cid, (uint32_t)(96 - 8 * i), 8);
        if (pnm[i] < 0x20 || pnm[i] > 0x7e) pnm[i] = '?';
    }
    pnm[5] = '\0';

    uint32_t mdt = sd_bits(g_cid, 8, 12);
    cprintf("SDMMC sdio1@%lx: %s, %u MB (%u blocks of 512 B)\n",
            (unsigned long)SD_BASE,
            g_block_addressed ? "SDHC/SDXC" : "SD", (unsigned)(g_blocks / 2048u),
            (unsigned)g_blocks);
    cprintf("  card   MID 0x%02x OID '%c%c' name '%s' rev %u.%u serial 0x%08x %u/%u\n",
            (unsigned)sd_bits(g_cid, 120, 8),
            (char)sd_bits(g_cid, 112, 8), (char)sd_bits(g_cid, 104, 8), pnm,
            (unsigned)sd_bits(g_cid, 60, 4), (unsigned)sd_bits(g_cid, 56, 4),
            (unsigned)sd_bits(g_cid, 24, 32),
            (unsigned)(mdt & 0xfu), (unsigned)(2000u + (mdt >> 4)));
    cprintf("  bus    %u-bit at %u kHz (CIU %u kHz), RCA 0x%04x, addressing %s\n",
            (unsigned)g_bus_width, (unsigned)g_bus_khz, (unsigned)(g_ciu_hz / 1000u),
            (unsigned)g_rca, g_block_addressed ? "block" : "byte");
    cprintf("  host   VERID 0x%08x HCON 0x%08x CDETECT 0x%x WRTPRT 0x%x STATUS 0x%x, "
            "sdblk task %s (%u calls)\n",
            (unsigned)REG(SD_VERID), (unsigned)REG(SD_HCON),
            (unsigned)REG(SD_CDETECT), (unsigned)REG(SD_WRTPRT),
            (unsigned)REG(SD_STATUS),
            blk_task_alive() ? "serving" : "not running",
            (unsigned)blk_task_call_count());
    cprintf("  sdblk: %u stale resets, %u replies reached the wrong caller\n",
            (unsigned)g_stale_transfers, (unsigned)g_blk_mixups);

    ylock_acquire(&g_host_lock);
    int rd = sdmmc_hw_read_block(0, g_probe_block);
    ylock_release(&g_host_lock);
    if (rd != 0) {
        cprintf("  block0 UNREADABLE\n");
        return;
    }
    cprintf("  block0 ");
    for (int i = 0; i < 16; i++) cprintf("%02x ", g_probe_block[i]);
    cprintf("... sig %02x %02x %s\n",
            g_probe_block[510], g_probe_block[511],
            (g_probe_block[510] == 0x55 && g_probe_block[511] == 0xAA)
                ? "(MBR/VBR)" : "(no boot signature)");
}

#else /* !CONFIG_ENABLE_SDMMC */

block_dev_t *sdmmc_get_device(void) { return NULL; }
int sdmmc_task_start(void) { return -1; }
uint32_t blk_task_call_count(void) { return 0; }
void sdmmc_info_report(void) {
    cprintf("SDMMC: built out of this image (LUGALOS_ENABLE_SDMMC=OFF)\n");
}
void sdmmc_bench_report(uint32_t kilobytes) { (void)kilobytes; sdmmc_info_report(); }

#endif /* CONFIG_ENABLE_SDMMC */
