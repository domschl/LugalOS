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
#include "lugalos_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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

static int spisd_read_blocks(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    if (!g_sd_initialized) return -1;
    /* Unlike the other three block-device drivers (virtio_blk, flashdisk,
     * ramdisk), this one sent whatever LBA it was given straight to the
     * card over SPI with no bound check at all. On real hardware that's
     * confined to the SD card's own address space (not a kernel
     * memory-safety issue), but a filesystem-layer bug that computes a
     * bad cluster number -- e.g. the fat_alloc_cluster() bug fixed
     * alongside this, see B9 in
     * plan/completed/2026-08-07_review_and_remediation.md -- could still silently
     * read/write sectors outside the mounted FAT32 volume's real bounds. */
    if (!buf || lba + count < lba || lba + count > dev->num_blocks) return -1;

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

static int spisd_write_blocks(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    if (!g_sd_initialized) return -1;
    if (!buf || lba + count < lba || lba + count > dev->num_blocks) return -1;

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

#endif // CONFIG_ENABLE_SPISD
