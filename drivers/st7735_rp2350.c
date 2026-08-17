/*
 * LugalOS Hardware Driver: ST7735 SPI TFT Canvas for RP2350 (Pico 2)
 * Vendored and adapted from ~/gith/domschl/LugalChess (firmware/st7735.c, H1,
 * plan/phase9_chess_computer.md).
 *
 * Hardware Mapping (cmake/board-rp2350.cmake is the source of truth):
 *   GP18 : SPI0 SCK  (Clock, Function 1)
 *   GP19 : SPI0 TX   (MOSI,  Function 1)
 *   GP17 : CS        (Chip Select, Function 5 - SIO GPIO)
 *   GP20 : DC        (Data/Command, Function 5 - SIO GPIO)
 *   GP21 : RST       (Reset, Function 5 - SIO GPIO)
 *
 * SPI0 (0x40080000) is a second, independent PL022 controller from SPI1
 * (0x40088000, the SD card, drivers/spisd_rp2350.c) -- no bus contention, no
 * arbitration needed (see H0's feasibility note, plan/phase9_chess_computer.md).
 * The panel is write-only (no MISO wired), so unlike spisd's spi_transfer()
 * this driver never needs the received byte, only the RX-FIFO drain that
 * keeps the PL022 from overrunning.
 */

#include "drivers/st7735.h"
#include "kernel/time.h"
#include "kernel/sched.h"
#include "kernel/chan.h"
#include "kernel/printk.h"
#include "kernel/mem_domain.h"
#include "kernel/device.h"
#include "kernel/ipc.h"
#include "kernel/palloc.h"
#include "arch/umode.h"
#include "lugalos_config.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

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

#define SPI0_BASE               ((uintptr_t)CONFIG_SPI0_BASE)
#define SSPCR0                  (SPI0_BASE + 0x00)
#define SSPCR1                  (SPI0_BASE + 0x04)
#define SSPDR                   (SPI0_BASE + 0x08)
#define SSPSR                   (SPI0_BASE + 0x0C)
#define SSPCPSR                 (SPI0_BASE + 0x10)

#define REG(addr) (*(volatile uint32_t *)(addr))

/* M5 Phase 4, plan/phase12_microkernel_migration.md: RP2350's Secure/
 * Non-secure split -- see drivers/uart_rp2350.c's ACCESSCTRL_GPIO_NSMASK0
 * comment (Phase 1) for the datasheet citation, and drivers/i2c_rtc.c's
 * ACCESSCTRL_I2C_RTC comment (Phase 3) for the write-password one. SPI0
 * is the same per-peripheral register shape as I2C0/I2C1 -- checked
 * directly against ~/gith/pico/pico-sdk's accessctrl.h -- and so needs
 * the same 0xacce0000 write-password prefix I2C0/I2C1 did; the GPIO mask
 * below does not (GPIO_NSMASK0/1 are the documented exemption). */
#define ACCESSCTRL_BASE          0x40060000UL
#define ACCESSCTRL_GPIO_NSMASK0  (ACCESSCTRL_BASE + 0x0c)
#define ACCESSCTRL_SPI0          (ACCESSCTRL_BASE + 0x90)
#define ACCESSCTRL_SPI0_NSP      (1u << 1)
#define ACCESSCTRL_SPI0_NSU      (1u << 0)
#define ACCESSCTRL_WRITE_PASSWORD 0xacce0000UL

#define CS_PIN  CONFIG_ST7735_CS_GPIO
#define DC_PIN  CONFIG_ST7735_DC_GPIO
#define RST_PIN CONFIG_ST7735_RST_GPIO
#define CS_MASK  (1u << CS_PIN)
#define DC_MASK  (1u << DC_PIN)
#define RST_MASK (1u << RST_PIN)

static inline void cs_select(void)   { REG(SIO_GPIO_OUT_CLR) = CS_MASK; }
static inline void cs_deselect(void) { REG(SIO_GPIO_OUT_SET) = CS_MASK; }
static inline void dc_command(void)  { REG(SIO_GPIO_OUT_CLR) = DC_MASK; }
static inline void dc_data(void)     { REG(SIO_GPIO_OUT_SET) = DC_MASK; }

/* Push one byte and drain the reply -- the received byte is discarded (the
 * panel has no MISO wired), but the drain still has to happen or the PL022's
 * 8-entry RX FIFO overruns on anything longer than 8 bytes. */
static void spi_write_byte(uint8_t tx) {
    while (REG(SSPSR) & (1u << 2)) {
        (void)REG(SSPDR);
    }
    int timeout = 10000;
    while (!(REG(SSPSR) & (1u << 1)) && --timeout > 0);
    REG(SSPDR) = tx;
    timeout = 10000;
    while (!(REG(SSPSR) & (1u << 2)) && --timeout > 0);
    (void)REG(SSPDR);
}

static void spi_write_buf(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        spi_write_byte(buf[i]);
    }
}

static void st7735_write_cmd(uint8_t cmd) {
    dc_command();
    cs_select();
    spi_write_byte(cmd);
    cs_deselect();
}

static void st7735_write_data(uint8_t data) {
    dc_data();
    cs_select();
    spi_write_byte(data);
    cs_deselect();
}

static void st7735_write_data_buf(const uint8_t *buf, size_t len) {
    dc_data();
    cs_select();
    spi_write_buf(buf, len);
    cs_deselect();
}

static void st7735_set_window(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1) {
    st7735_write_cmd(0x2A); /* Column Address Set */
    st7735_write_data(0x00);
    st7735_write_data(x0);
    st7735_write_data(0x00);
    st7735_write_data(x1);

    st7735_write_cmd(0x2B); /* Row Address Set */
    st7735_write_data(0x00);
    st7735_write_data(y0);
    st7735_write_data(0x00);
    st7735_write_data(y1);

    st7735_write_cmd(0x2C); /* Memory Write */
}

static int st7735_init_hardware(void) {
    /* 1. Unreset SPI0 (bit 18 on RP2350), IO_BANK0 (bit 6), PADS_BANK0 (bit 9) */
    uint32_t unreset_mask = (1u << 18) | (1u << 6) | (1u << 9);
    REG(RESETS_ATOMIC_CLEAR) = unreset_mask;
    while ((REG(RESETS_RESET_DONE) & unreset_mask) != unreset_mask);

    /* 2. Configure SPI0 GPIO pins: SCK/MOSI as SPI function (F1); no MISO --
     * the panel is write-only. */
    REG(IO_BANK0_CTRL(CONFIG_ST7735_SCK_GPIO)) = 1;
    REG(IO_BANK0_CTRL(CONFIG_ST7735_MOSI_GPIO)) = 1;
    REG(PADS_BANK0_PAD(CONFIG_ST7735_SCK_GPIO)) = 0x5A;
    REG(PADS_BANK0_PAD(CONFIG_ST7735_MOSI_GPIO)) = 0x5A;

    /* 3. CS/DC/RST as plain SIO GPIO outputs (Function 5) */
    REG(IO_BANK0_CTRL(CS_PIN)) = 5;
    REG(IO_BANK0_CTRL(DC_PIN)) = 5;
    REG(IO_BANK0_CTRL(RST_PIN)) = 5;
    REG(PADS_BANK0_PAD(CS_PIN)) = 0x5A;
    REG(PADS_BANK0_PAD(DC_PIN)) = 0x5A;
    REG(PADS_BANK0_PAD(RST_PIN)) = 0x5A;
    REG(SIO_GPIO_OE_SET) = CS_MASK | DC_MASK | RST_MASK;
    cs_deselect();
    dc_command();
    REG(SIO_GPIO_OUT_SET) = RST_MASK;

    /* M5 Phase 4: CS/DC/RST and SPI0 need to be Non-secure-accessible for
     * the U-mode task's own serve loop below to actually touch them --
     * must happen here, from M-mode, before the task exists. See
     * ACCESSCTRL_SPI0's own comment above. */
    REG(ACCESSCTRL_GPIO_NSMASK0) |= (CS_MASK | DC_MASK | RST_MASK);
    REG(ACCESSCTRL_SPI0) = ACCESSCTRL_WRITE_PASSWORD | REG(ACCESSCTRL_SPI0)
                           | ACCESSCTRL_SPI0_NSP | ACCESSCTRL_SPI0_NSU;

    /* 4. Disable SPI0 before programming, then set the clock for ~25 MHz
     * (clk_peri 150 MHz / (CPSDVSR=6 * (1+SCR=0))), closest achievable step
     * to the 24 MHz this panel was already tested at under the Pico SDK. */
    REG(SSPCR1) = 0;
    REG(SSPCPSR) = 6;
    REG(SSPCR0) = 0x7; /* 8-bit data, Motorola SPI mode 0, SCR=0 */
    REG(SSPCR1) = (1u << 1); /* SSE = 1 */

    /* 5. Hardware reset */
    REG(SIO_GPIO_OUT_SET) = RST_MASK;
    time_delay_us(5000);
    REG(SIO_GPIO_OUT_CLR) = RST_MASK;
    time_delay_us(15000);
    REG(SIO_GPIO_OUT_SET) = RST_MASK;
    time_delay_us(15000);

    /* 6. Initialization sequence (ST7735 command set, unchanged from the
     * vendored source) */
    st7735_write_cmd(0x01); /* Software Reset */
    time_delay_us(120000);

    st7735_write_cmd(0x11); /* Sleep Out */
    time_delay_us(120000);

    st7735_write_cmd(0xB1); /* Frame Rate Control (normal mode) */
    st7735_write_data(0x01);
    st7735_write_data(0x2C);
    st7735_write_data(0x2D);

    st7735_write_cmd(0xB2); /* Frame Rate Control (idle mode) */
    st7735_write_data(0x01);
    st7735_write_data(0x2C);
    st7735_write_data(0x2D);

    st7735_write_cmd(0xB3); /* Frame Rate Control (partial mode) */
    st7735_write_data(0x01);
    st7735_write_data(0x2C);
    st7735_write_data(0x2D);
    st7735_write_data(0x01);
    st7735_write_data(0x2C);
    st7735_write_data(0x2D);

    st7735_write_cmd(0xB4); /* Display Inversion Control */
    st7735_write_data(0x07); /* No inversion */

    st7735_write_cmd(0xC0); /* Power Control 1 */
    st7735_write_data(0xA2);
    st7735_write_data(0x02);
    st7735_write_data(0x84);

    st7735_write_cmd(0xC1); /* Power Control 2 */
    st7735_write_data(0xC5);

    st7735_write_cmd(0xC2); /* Power Control 3 */
    st7735_write_data(0x0A);
    st7735_write_data(0x00);

    st7735_write_cmd(0xC3); /* Power Control 4 */
    st7735_write_data(0x8A);
    st7735_write_data(0x2A);

    st7735_write_cmd(0xC4); /* Power Control 5 */
    st7735_write_data(0x8A);
    st7735_write_data(0xEE);

    st7735_write_cmd(0xC5); /* VCOM Control 1 */
    st7735_write_data(0x0E);

    st7735_write_cmd(0x36); /* MADCTL (Orientation / Scan Direction) */
    /* Bit 3 (0x08) selects the panel's RGB/BGR sub-pixel order. The vendored
     * value (0xC8, "RGB") swapped red and blue on this physical unit --
     * green and white came out correct (both unaffected by an R/B swap: G
     * sits in the untouched middle field, and all-white is symmetric), but a
     * pure-red fill rendered blue. Cleared here (0xC0) for actual RGB order
     * on this panel; this is a real per-unit hardware property, not
     * something derivable from the datasheet -- nominally identical ST7735
     * breakout boards from different batches wire the sub-pixels either way. */
    st7735_write_data(0xC0); /* Portrait, RGB color order (bit 3 cleared), normal scan */

    st7735_write_cmd(0x3A); /* Interface Pixel Format */
    st7735_write_data(0x05); /* 16-bit color (RGB565) */

    st7735_write_cmd(0xE0); /* Gamma Adjust (+) */
    {
        static const uint8_t gamma_p[16] = {
            0x0f, 0x1a, 0x0f, 0x18, 0x2f, 0x28, 0x20, 0x22,
            0x1f, 0x1b, 0x23, 0x37, 0x00, 0x07, 0x02, 0x10
        };
        for (int i = 0; i < 16; i++) st7735_write_data(gamma_p[i]);
    }

    st7735_write_cmd(0xE1); /* Gamma Adjust (-) */
    {
        static const uint8_t gamma_n[16] = {
            0x0f, 0x1b, 0x0f, 0x17, 0x33, 0x2c, 0x29, 0x2e,
            0x30, 0x30, 0x39, 0x3f, 0x00, 0x07, 0x03, 0x10
        };
        for (int i = 0; i < 16; i++) st7735_write_data(gamma_n[i]);
    }

    st7735_write_cmd(0x29); /* Display On */
    time_delay_us(100000);

    return 0;
}

/* Forward declaration: st7735_init() (boot-time, before the task exists) is
 * the only caller of this outside the task body below. */
static void st7735_hw_fill_screen(uint16_t color);

void st7735_init(void) {
    st7735_init_hardware();
    st7735_hw_fill_screen(ST7735_BLACK);
}

static void st7735_hw_fill_screen(uint16_t color) {
    st7735_set_window(0, 0, ST7735_WIDTH - 1, ST7735_HEIGHT - 1);
    uint8_t color_bytes[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };

    static uint8_t chunk[512];
    for (int i = 0; i < 512; i += 2) {
        chunk[i] = color_bytes[0];
        chunk[i + 1] = color_bytes[1];
    }
    /* 128 * 160 pixels = 20480 pixels = 40960 bytes, sent in 512-byte chunks */
    for (int i = 0; i < 80; i++) {
        st7735_write_data_buf(chunk, 512);
    }
}

static void st7735_hw_draw_pixel(int x, int y, uint16_t color) {
    if (x < 0 || x >= ST7735_WIDTH || y < 0 || y >= ST7735_HEIGHT) return;
    st7735_set_window((uint8_t)x, (uint8_t)y, (uint8_t)x, (uint8_t)y);
    uint8_t data[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };
    st7735_write_data_buf(data, 2);
}

static void st7735_hw_draw_rect(int x, int y, int w, int h, uint16_t color) {
    if (x < 0 || x + w > ST7735_WIDTH || y < 0 || y + h > ST7735_HEIGHT) return;
    st7735_set_window((uint8_t)x, (uint8_t)y, (uint8_t)(x + w - 1), (uint8_t)(y + h - 1));
    uint8_t color_bytes[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };

    static uint8_t block[512];
    size_t block_pixels = 256;
    if (block_pixels > (size_t)(w * h)) block_pixels = (size_t)(w * h);
    for (size_t i = 0; i < block_pixels * 2; i += 2) {
        block[i] = color_bytes[0];
        block[i + 1] = color_bytes[1];
    }

    int total_pixels = w * h;
    int written = 0;
    while (written < total_pixels) {
        int to_write = total_pixels - written;
        if (to_write > (int)block_pixels) to_write = (int)block_pixels;
        st7735_write_data_buf(block, (size_t)to_write * 2);
        written += to_write;
    }
}

static void st7735_hw_draw_bitmap_mono(int x, int y, int w, int h, const uint16_t *bitmap, uint16_t fg_color, uint16_t bg_color) {
    if (x < 0 || x + w > ST7735_WIDTH || y < 0 || y + h > ST7735_HEIGHT) return;
    st7735_set_window((uint8_t)x, (uint8_t)y, (uint8_t)(x + w - 1), (uint8_t)(y + h - 1));

    static uint8_t buffer[512]; /* 16x16 pixels is 256 pixels, 512 bytes */
    int idx = 0;

    uint8_t fg_h = (uint8_t)(fg_color >> 8), fg_l = (uint8_t)(fg_color & 0xFF);
    uint8_t bg_h = (uint8_t)(bg_color >> 8), bg_l = (uint8_t)(bg_color & 0xFF);

    for (int r = 0; r < h; r++) {
        uint16_t row_bits = bitmap[r];
        for (int c = 0; c < w; c++) {
            if ((row_bits >> (15 - c)) & 1) {
                buffer[idx++] = fg_h;
                buffer[idx++] = fg_l;
            } else {
                buffer[idx++] = bg_h;
                buffer[idx++] = bg_l;
            }
        }
    }
    st7735_write_data_buf(buffer, (size_t)(w * h * 2));
}

/* M5 Phase 4, plan/phase12_microkernel_migration.md: this file's own
 * dedicated U-mode-executable page (linker: .st7735text,
 * board_st7735_text_region() -- kernel/board.c) rather than the shared
 * .utext heartbeat/tm1638/i2c already use, which ran out of room once
 * font5x7 below (480 bytes) needed to join them. Same
 * section/no_sanitize shape those files' own UATTR macros use.
 * ST7735_UDATA is separate from ST7735_UATTR for the same reason
 * drivers/tm1638_rp2350.c's TM1638_UDATA is separate from TM1638_UATTR:
 * GCC refuses to mix const data and executable code in one section (a
 * "section type conflict" error), even though both land in the same
 * linked page via the linker script's `*(.st7735text .st7735text.*)`
 * wildcard. */
#define ST7735_UATTR __attribute__((section(".st7735text"))) __attribute__((no_sanitize("undefined")))
#define ST7735_UDATA __attribute__((section(".st7735text.rodata")))

/* Standard 5x7 pixel ASCII font, covering ASCII 32 to 127. In
 * .st7735text (not ordinary .rodata) so the U-mode task's own draw_char
 * below can read it -- a single shared table, safe across the privilege
 * boundary because it is never written; the kernel-mode fallback path
 * below keeps reading the same one. */
ST7735_UDATA static const uint8_t font5x7[96][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5f, 0x00, 0x00},
    {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7f, 0x14, 0x7f, 0x14},
    {0x24, 0x2a, 0x7f, 0x2a, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1c, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1c, 0x00},
    {0x08, 0x2a, 0x1c, 0x2a, 0x08}, {0x08, 0x08, 0x3e, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08},
    {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4b, 0x31},
    {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1e},
    {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x79, 0x41, 0x3e}, {0x7e, 0x11, 0x11, 0x11, 0x7e},
    {0x7f, 0x49, 0x49, 0x49, 0x36}, {0x3e, 0x41, 0x41, 0x41, 0x22},
    {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41},
    {0x7f, 0x09, 0x09, 0x09, 0x01}, {0x3e, 0x41, 0x49, 0x49, 0x7a},
    {0x7f, 0x08, 0x08, 0x08, 0x7f}, {0x00, 0x41, 0x7f, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41},
    {0x7f, 0x40, 0x40, 0x40, 0x40}, {0x7f, 0x02, 0x0c, 0x02, 0x7f},
    {0x7f, 0x04, 0x08, 0x10, 0x7f}, {0x3e, 0x41, 0x41, 0x41, 0x3e},
    {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e},
    {0x7f, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7f, 0x01, 0x01}, {0x3f, 0x40, 0x40, 0x40, 0x3f},
    {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07},
    {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7f, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7f, 0x00},
    {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7f, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7f}, {0x38, 0x54, 0x54, 0x54, 0x18},
    {0x08, 0x7e, 0x09, 0x01, 0x02}, {0x0c, 0x52, 0x52, 0x52, 0x3e},
    {0x7f, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7d, 0x40, 0x00},
    {0x20, 0x40, 0x44, 0x3d, 0x00}, {0x7f, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7f, 0x40, 0x00}, {0x7c, 0x04, 0x18, 0x04, 0x78},
    {0x7c, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7c, 0x14, 0x14, 0x14, 0x08}, {0x08, 0x14, 0x14, 0x18, 0x7c},
    {0x7c, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3f, 0x44, 0x40, 0x20}, {0x3c, 0x40, 0x40, 0x20, 0x7c},
    {0x1c, 0x20, 0x40, 0x20, 0x1c}, {0x3c, 0x40, 0x30, 0x40, 0x3c},
    {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0c, 0x50, 0x50, 0x50, 0x3c},
    {0x44, 0x64, 0x54, 0x4c, 0x44}, {0x08, 0x36, 0x41, 0x41, 0x00},
    {0x00, 0x00, 0x77, 0x00, 0x00}, {0x00, 0x41, 0x41, 0x36, 0x08},
    {0x08, 0x08, 0x2a, 0x1c, 0x08}, {0x00, 0x00, 0x00, 0x00, 0x00}
};

static void st7735_hw_draw_char(int x, int y, char c, uint16_t color, int size) {
    if (c < 32 || c > 127) return;
    int font_idx = c - 32;

    for (int col = 0; col < 5; col++) {
        uint8_t line = font5x7[font_idx][col];
        for (int row = 0; row < 8; row++) {
            if (line & 1) {
                if (size == 1) {
                    st7735_hw_draw_pixel(x + col, y + row, color);
                } else {
                    st7735_hw_draw_rect(x + col * size, y + row * size, size, size, color);
                }
            }
            line >>= 1;
        }
    }
}

static void st7735_hw_draw_string(int x, int y, const char *str, uint16_t color, int size) {
    while (*str) {
        st7735_hw_draw_char(x, y, *str, color, size);
        x += 6 * size; /* 5 pixels width + 1 pixel padding */
        if (x > 120) break; /* wrap limit */
        str++;
    }
}

/* M4.5, plan/phase12_microkernel_migration.md, Part B: the driver as a task.
 * Unlike RTC/EEPROM's low-frequency wire, a full chess-board redraw makes
 * ~64 of these calls back to back (one draw_bitmap_mono or draw_rect per
 * square) -- still nowhere near uart's original per-character mistake (a
 * "how many calls does one real operation cost" question, not "does this
 * scale with bytes/pixels"): each call already carries one whole logical
 * drawing operation, same granularity the direct-call API always had.
 * draw_char()/draw_string()'s internal pixel-level fan-out (already present
 * before this conversion, see st7735_hw_draw_char/string above) stays
 * exactly that -- plain C calls on the task's own stack, never re-entering
 * chan_call() on this same endpoint (which the busy-endpoint check would
 * refuse anyway, see kernel/chan.h's own reentrancy note).
 *
 * Wire protocol, one opcode byte then a fixed-shape payload per op. x/y/w/h/
 * size travel as plain int16_t (not the uint8_t the hardware register
 * writes ultimately narrow to) so a negative or oversized value reaches the
 * same bounds checks st7735_hw_draw_pixel()/rect() already had, unchanged,
 * rather than wrapping earlier on the wire. Like drivers/i2c_rtc.c's wire,
 * there is no real byte-order boundary to defend (kernel-internal IPC, not
 * a real wire), so no explicit big-endian encoding either.
 *
 *   'F' fill_screen      req: [op] + color(2)
 *   'P' draw_pixel       req: [op] + x(2) + y(2) + color(2)
 *   'R' draw_rect        req: [op] + x(2) + y(2) + w(2) + h(2) + color(2)
 *   'B' draw_bitmap_mono req: [op] + x(2) + y(2) + w(2) + h(2) + fg(2) + bg(2) + bitmap(h*2)
 *   'C' draw_char        req: [op] + x(2) + y(2) + c(1) + color(2) + size(2)
 *   'S' draw_string      req: [op] + x(2) + y(2) + color(2) + size(2) + str(len)
 *
 * Every reply is a 0-length ack -- these all return void today, so there is
 * nothing to report beyond "the call reached the task", which chan_call()'s
 * own >=0-vs--1 return already signals. ST7735_BITMAP_MAX_ROWS/
 * ST7735_STR_MAX bound the two variable-length ops; every real caller in
 * this tree (user/chess/src/chess_ui.c) only ever draws 16-row piece
 * bitmaps and status strings well under 64 bytes, so these are headroom,
 * not a measured ceiling -- a request over either bound falls back to
 * direct access below rather than being refused outright, same shape as
 * drivers/spisd_rp2350.c's BLK_MAX_COUNT. */
#define ST7735_OP_FILL_SCREEN ((uint8_t)'F')
#define ST7735_OP_DRAW_PIXEL  ((uint8_t)'P')
#define ST7735_OP_DRAW_RECT   ((uint8_t)'R')
#define ST7735_OP_DRAW_BITMAP ((uint8_t)'B')
#define ST7735_OP_DRAW_CHAR   ((uint8_t)'C')
#define ST7735_OP_DRAW_STRING ((uint8_t)'S')

#define ST7735_BITMAP_MAX_ROWS 32u
#define ST7735_STR_MAX         64u

#define ST7735_REQ_CAP  (9u + (ST7735_BITMAP_MAX_ROWS * 2u > ST7735_STR_MAX ? \
                               ST7735_BITMAP_MAX_ROWS * 2u : ST7735_STR_MAX))
#define ST7735_RESP_CAP 4u

static uint8_t         g_st7735_req[ST7735_REQ_CAP];
static uint8_t         g_st7735_resp[ST7735_RESP_CAP];
static chan_endpoint_t *g_st7735_ep;
static int              g_st7735_task_pid = -1;

/* M4.5 verify: counts chan_call()s actually served -- see
 * drivers/uart_16550.c's g_uart_write_calls comment for the reasoning. */
static uint32_t g_st7735_calls;

uint32_t st7735_task_call_count(void) { return g_st7735_calls; }

static bool st7735_task_alive(void) {
    if (g_st7735_task_pid < 0) return false;
    int st = sched_task_state(g_st7735_task_pid);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

static void put_i16(uint8_t *p, int v) { uint16_t u = (uint16_t)(int16_t)v; p[0] = (uint8_t)(u >> 8); p[1] = (uint8_t)u; }
static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* ---- U-mode implementation, M5 Phase 4, plan/phase12_microkernel_migration.md ----
 *
 * A second, independent copy of the SPI/GPIO helpers above
 * (cs_select/cs_deselect/dc_command/dc_data, spi_write_byte/spi_write_buf,
 * st7735_write_cmd/_data/_data_buf, st7735_set_window) and the six
 * st7735_hw_* draw functions, tagged ST7735_UATTR and reachable only
 * from the U-mode task's own serve loop below -- not a refactor of the
 * existing kernel-mode ones, which keep serving the direct-hardware
 * fallback path exactly as before, unreachable from U-mode. The two
 * copies never run concurrently -- the facade functions below route to
 * one or the other depending on st7735_task_alive() -- so nothing needs
 * to agree between them beyond the wire protocol and font5x7, both
 * already shared.
 *
 * The three `static uint8_t foo[512]` buffers the kernel-mode versions
 * use (fill_screen/draw_rect/draw_bitmap_mono) are not persistent state
 * -- each is fully overwritten before use on every call, unlike
 * drivers/tm1638_rp2350.c's RAM-cache, which genuinely has to survive
 * across separate chan_serve_wait()/chan_serve_reply() round trips. Here
 * they are plain U-mode-stack locals instead -- no fourth PMP region or
 * stack-reservation trick needed. */
ST7735_UATTR static void u_cs_select(void)   { REG(SIO_GPIO_OUT_CLR) = CS_MASK; }
ST7735_UATTR static void u_cs_deselect(void) { REG(SIO_GPIO_OUT_SET) = CS_MASK; }
ST7735_UATTR static void u_dc_command(void)  { REG(SIO_GPIO_OUT_CLR) = DC_MASK; }
ST7735_UATTR static void u_dc_data(void)     { REG(SIO_GPIO_OUT_SET) = DC_MASK; }

ST7735_UATTR static void u_spi_write_byte(uint8_t tx) {
    while (REG(SSPSR) & (1u << 2)) {
        (void)REG(SSPDR);
    }
    int timeout = 10000;
    while (!(REG(SSPSR) & (1u << 1)) && --timeout > 0);
    REG(SSPDR) = tx;
    timeout = 10000;
    while (!(REG(SSPSR) & (1u << 2)) && --timeout > 0);
    (void)REG(SSPDR);
}

ST7735_UATTR static void u_spi_write_buf(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) u_spi_write_byte(buf[i]);
}

ST7735_UATTR static void u_write_cmd(uint8_t cmd) {
    u_dc_command();
    u_cs_select();
    u_spi_write_byte(cmd);
    u_cs_deselect();
}

ST7735_UATTR static void u_write_data(uint8_t data) {
    u_dc_data();
    u_cs_select();
    u_spi_write_byte(data);
    u_cs_deselect();
}

ST7735_UATTR static void u_write_data_buf(const uint8_t *buf, size_t len) {
    u_dc_data();
    u_cs_select();
    u_spi_write_buf(buf, len);
    u_cs_deselect();
}

ST7735_UATTR static void u_set_window(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1) {
    u_write_cmd(0x2A);
    u_write_data(0x00);
    u_write_data(x0);
    u_write_data(0x00);
    u_write_data(x1);

    u_write_cmd(0x2B);
    u_write_data(0x00);
    u_write_data(y0);
    u_write_data(0x00);
    u_write_data(y1);

    u_write_cmd(0x2C);
}

ST7735_UATTR static void u_fill_screen(uint16_t color) {
    u_set_window(0, 0, ST7735_WIDTH - 1, ST7735_HEIGHT - 1);
    uint8_t color_bytes[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };

    uint8_t chunk[512];
    for (int i = 0; i < 512; i += 2) {
        chunk[i] = color_bytes[0];
        chunk[i + 1] = color_bytes[1];
    }
    for (int i = 0; i < 80; i++) {
        u_write_data_buf(chunk, 512);
    }
}

ST7735_UATTR static void u_draw_pixel(int x, int y, uint16_t color) {
    if (x < 0 || x >= ST7735_WIDTH || y < 0 || y >= ST7735_HEIGHT) return;
    u_set_window((uint8_t)x, (uint8_t)y, (uint8_t)x, (uint8_t)y);
    uint8_t data[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };
    u_write_data_buf(data, 2);
}

ST7735_UATTR static void u_draw_rect(int x, int y, int w, int h, uint16_t color) {
    if (x < 0 || x + w > ST7735_WIDTH || y < 0 || y + h > ST7735_HEIGHT) return;
    u_set_window((uint8_t)x, (uint8_t)y, (uint8_t)(x + w - 1), (uint8_t)(y + h - 1));
    uint8_t color_bytes[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };

    uint8_t block[512];
    size_t block_pixels = 256;
    if (block_pixels > (size_t)(w * h)) block_pixels = (size_t)(w * h);
    for (size_t i = 0; i < block_pixels * 2; i += 2) {
        block[i] = color_bytes[0];
        block[i + 1] = color_bytes[1];
    }

    int total_pixels = w * h;
    int written = 0;
    while (written < total_pixels) {
        int to_write = total_pixels - written;
        if (to_write > (int)block_pixels) to_write = (int)block_pixels;
        u_write_data_buf(block, (size_t)to_write * 2);
        written += to_write;
    }
}

ST7735_UATTR static void u_draw_bitmap_mono(int x, int y, int w, int h, const uint16_t *bitmap, uint16_t fg_color, uint16_t bg_color) {
    if (x < 0 || x + w > ST7735_WIDTH || y < 0 || y + h > ST7735_HEIGHT) return;
    u_set_window((uint8_t)x, (uint8_t)y, (uint8_t)(x + w - 1), (uint8_t)(y + h - 1));

    uint8_t buffer[512];
    int idx = 0;

    uint8_t fg_h = (uint8_t)(fg_color >> 8), fg_l = (uint8_t)(fg_color & 0xFF);
    uint8_t bg_h = (uint8_t)(bg_color >> 8), bg_l = (uint8_t)(bg_color & 0xFF);

    for (int r = 0; r < h; r++) {
        uint16_t row_bits = bitmap[r];
        for (int c = 0; c < w; c++) {
            if ((row_bits >> (15 - c)) & 1) {
                buffer[idx++] = fg_h;
                buffer[idx++] = fg_l;
            } else {
                buffer[idx++] = bg_h;
                buffer[idx++] = bg_l;
            }
        }
    }
    u_write_data_buf(buffer, (size_t)(w * h * 2));
}

ST7735_UATTR static void u_draw_char(int x, int y, char c, uint16_t color, int size) {
    if (c < 32 || c > 127) return;
    int font_idx = c - 32;

    for (int col = 0; col < 5; col++) {
        uint8_t line = font5x7[font_idx][col];
        for (int row = 0; row < 8; row++) {
            if (line & 1) {
                if (size == 1) {
                    u_draw_pixel(x + col, y + row, color);
                } else {
                    u_draw_rect(x + col * size, y + row * size, size, size, color);
                }
            }
            line >>= 1;
        }
    }
}

ST7735_UATTR static void u_draw_string(int x, int y, const char *str, uint16_t color, int size) {
    while (*str) {
        u_draw_char(x, y, *str, color, size);
        x += 6 * size;
        if (x > 120) break;
        str++;
    }
}

/* Hand-rolled per translation unit, not shared with any other driver's
 * own usys_*() stubs or user/progs/usys.h -- an ST7735_UATTR function
 * must not call anything the compiler might place outside .st7735text,
 * and a cross-file inline is not a guarantee. */
__attribute__((always_inline)) static inline long st7735_usys_chan_serve_wait(const char *name, uint8_t *buf, long buf_max) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_WAIT;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = buf_max;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}
__attribute__((always_inline)) static inline long st7735_usys_chan_serve_reply(const char *name, const uint8_t *buf, long len) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_REPLY;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = len;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}
__attribute__((always_inline)) static inline int st7735_usys_get_i16(const uint8_t *p) {
    return (int)(int16_t)(((uint16_t)p[0] << 8) | p[1]);
}
__attribute__((always_inline)) static inline uint16_t st7735_usys_get_u16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

ST7735_UATTR static void st7735_umode_body(void) {
    /* Not a string literal: a literal lands in ordinary .rodata, outside
     * every region this task's domain grants -- the bug that hung the
     * board the first time this exact mechanism ran on real hardware in
     * M5 Phase 2 (see drivers/tm1638_rp2350.c's own comment). volatile,
     * for the reason kernel/shell.c's user_deputy() already documents:
     * gcc recognises a run of consecutive stores and turns it back into
     * a copy from a .rodata blob otherwise. */
    volatile char name[8];
    name[0]='s'; name[1]='t'; name[2]='7'; name[3]='7';
    name[4]='3'; name[5]='5'; name[6]='\0';

    for (;;) {
        uint8_t req[ST7735_REQ_CAP];
        long req_len = st7735_usys_chan_serve_wait((const char *)name, req, sizeof(req));
        if (req_len < 1) {
            st7735_usys_chan_serve_reply((const char *)name, NULL, 0);
            continue;
        }

        /* Not a switch: with six cases, GCC compiles a switch into a
         * jump table -- an array of addresses stored in ordinary
         * .rodata, outside .st7735text, that the dispatch then does an
         * indirect jump through. Found on real hardware, not predicted
         * by the disassembly check (which greps for jal/call targets --
         * an indirect jump through a loaded table entry is neither): a
         * real load access fault (cause 5) reading the table itself,
         * which lives outside every region this task's domain grants.
         * tm1638_umode_body()/i2c_umode_body() have fewer cases and
         * happened not to trigger this GCC codegen choice, not because
         * a switch is inherently safe here -- an if/else chain is,
         * structurally, since GCC never turns one into a jump table. */
        uint8_t op = req[0];
        if (op == ST7735_OP_FILL_SCREEN) {
            if (req_len >= 3) u_fill_screen(st7735_usys_get_u16(&req[1]));
        } else if (op == ST7735_OP_DRAW_PIXEL) {
            if (req_len >= 7) {
                u_draw_pixel(st7735_usys_get_i16(&req[1]), st7735_usys_get_i16(&req[3]),
                            st7735_usys_get_u16(&req[5]));
            }
        } else if (op == ST7735_OP_DRAW_RECT) {
            if (req_len >= 11) {
                u_draw_rect(st7735_usys_get_i16(&req[1]), st7735_usys_get_i16(&req[3]),
                           st7735_usys_get_i16(&req[5]), st7735_usys_get_i16(&req[7]),
                           st7735_usys_get_u16(&req[9]));
            }
        } else if (op == ST7735_OP_DRAW_BITMAP) {
            int h = req_len >= 13 ? st7735_usys_get_i16(&req[7]) : 0;
            if (req_len >= 13 && h > 0 && (uint32_t)h <= ST7735_BITMAP_MAX_ROWS &&
                (uint32_t)req_len >= 13u + (uint32_t)h * 2u) {
                uint16_t bitmap[ST7735_BITMAP_MAX_ROWS];
                for (int r = 0; r < h; r++) bitmap[r] = st7735_usys_get_u16(&req[13 + r * 2]);
                u_draw_bitmap_mono(st7735_usys_get_i16(&req[1]), st7735_usys_get_i16(&req[3]),
                                  st7735_usys_get_i16(&req[5]), h, bitmap,
                                  st7735_usys_get_u16(&req[9]), st7735_usys_get_u16(&req[11]));
            }
        } else if (op == ST7735_OP_DRAW_CHAR) {
            if (req_len >= 10) {
                u_draw_char(st7735_usys_get_i16(&req[1]), st7735_usys_get_i16(&req[3]),
                           (char)req[5], st7735_usys_get_u16(&req[6]),
                           st7735_usys_get_i16(&req[8]));
            }
        } else if (op == ST7735_OP_DRAW_STRING) {
            if (req_len >= 9) {
                /* Not memcpy(): under -fno-builtin (this whole tree's
                 * build flags), a memcpy() call is not inlined -- it
                 * compiles to a real call outside .st7735text. Found
                 * this exact class of bug in drivers/tm1638_rp2350.c
                 * (M5 Phase 2); written out explicitly here from the
                 * start instead of rediscovering it. */
                char str[ST7735_STR_MAX + 1];
                uint32_t len = (uint32_t)req_len - 9;
                if (len > ST7735_STR_MAX) len = ST7735_STR_MAX;
                for (uint32_t i = 0; i < len; i++) str[i] = (char)req[9 + i];
                str[len] = '\0';
                u_draw_string(st7735_usys_get_i16(&req[1]), st7735_usys_get_i16(&req[3]),
                             str, st7735_usys_get_u16(&req[5]), st7735_usys_get_i16(&req[7]));
            }
        }
        st7735_usys_chan_serve_reply((const char *)name, NULL, 0);
    }
}

static uint8_t      g_st7735_ustack[4096] __attribute__((aligned(4096)));
static mem_domain_t g_st7735_domain;

/* This task's own kernel-mode entry point: task_create_sized() calls this
 * (ordinary kernel stack, kernel privilege) to build the domain and make
 * the one-way jump into U-mode. Mirrors drivers/i2c_rtc.c's
 * i2c_task_body() shape, with a fourth region: this is the first M5
 * driver needing both an SIO grant (CS/DC/RST) and a hardware-controller
 * grant (SPI0) at once. */
static void st7735_task_body(void *arg) {
    (void)arg;
    while (!g_st7735_ep) sched_yield();

    mem_domain_init(&g_st7735_domain);
    mem_domain_add(&g_st7735_domain, (uintptr_t)g_st7735_ustack, sizeof(g_st7735_ustack),
                   MEM_R | MEM_W);

    uintptr_t tbase, tsize;
    board_st7735_text_region(&tbase, &tsize);
    mem_domain_add(&g_st7735_domain, tbase, tsize, MEM_R | MEM_X);

    mem_domain_add(&g_st7735_domain, SIO_BASE, 4096, MEM_R | MEM_W);
    mem_domain_add(&g_st7735_domain, SPI0_BASE, 4096, MEM_R | MEM_W);

    if (task_set_domain(sched_current_pid(), &g_st7735_domain) != 0) {
        printk("[ST7735] Refusing to enter U-mode: memory domain not enforceable; canvas stays on direct hardware access.\n");
        return;
    }
    arch_enter_user(st7735_umode_body, (uintptr_t)g_st7735_ustack + sizeof(g_st7735_ustack), 0, 0, 0);
}

/* Called from kernel/main.c, after sched_init(). Not fatal if it fails:
 * every function below falls back to direct hardware access whenever the
 * task is not alive, same as every boot-time draw before this ever ran. */
int st7735_task_start(void) {
    int pid = task_create_sized("st7735", st7735_task_body, NULL, 1);
    if (pid < 0) {
        printk("[ST7735] Could not start the st7735 task; canvas stays on direct hardware access.\n");
        return -1;
    }
    if (chan_register_task("st7735", pid, g_st7735_req, sizeof(g_st7735_req),
                           g_st7735_resp, sizeof(g_st7735_resp)) != 0) {
        printk("[ST7735] Could not register the st7735 channel endpoint; falling back to direct hardware access.\n");
        return -1;
    }
    g_st7735_ep = chan_lookup("st7735");
    g_st7735_task_pid = pid;
    printk("[ST7735] Driver running as task #%d, reachable via chan_call(\"st7735\", ...)\n", pid);
    return pid;
}

static int st7735_call_with_retry(const uint8_t *req, uint32_t req_len) {
    uint8_t resp[ST7735_RESP_CAP];
    for (int attempt = 0; attempt < 8; attempt++) {
        int n = chan_call(g_st7735_ep, req, req_len, resp, sizeof(resp));
        /* M5 Phase 4: counted here, on the client side -- see
         * drivers/tm1638_rp2350.c's tm1638_call_with_retry() comment,
         * same reasoning: a U-mode server cannot touch g_st7735_calls,
         * an ordinary kernel .bss global no domain grants it. */
        if (n >= 0) { g_st7735_calls++; return n; }
        sched_yield();
    }
    return -1;
}

/* M5 Phase 4's own "Verify" deliverable: does the real st7735 domain
 * shape (stack + .st7735text + SIO + SPI0) actually confine the task to
 * its own hardware, or does one of the two MMIO grants' width
 * accidentally cover more? Modeled directly on drivers/i2c_rtc.c's
 * i2c_isolation_test() -- same idea (a deliberate out-of-domain store,
 * asserted to fault), a separate canary rather than reaching into
 * another file's, for the same reason the syscall stubs above are
 * hand-rolled per file. */
static volatile uintptr_t g_st7735_canary = 0xC0FFEE;

ST7735_UATTR static void st7735_intruder(void) {
    g_st7735_canary = 0xDEAD;
    for (;;) { } /* only reached if the store was NOT stopped */
}

static volatile bool g_st7735_intruder_entered;

/* `arg` is the U-mode stack -- allocated by st7735_isolation_test()
 * below, not here, so it can free it once the task is confirmed DEAD
 * (same shape as i2c_isolation_test()'s own probe). */
static void st7735_intruder_task_body(void *arg) {
    uint8_t *ustack = (uint8_t *)arg;
    mem_domain_t dom;
    mem_domain_init(&dom);
    mem_domain_add(&dom, (uintptr_t)ustack, 4096, MEM_R | MEM_W);
    uintptr_t tbase, tsize;
    board_st7735_text_region(&tbase, &tsize);
    mem_domain_add(&dom, tbase, tsize, MEM_R | MEM_X);
    /* The exact grants real st7735 runs under -- this is what's on trial. */
    mem_domain_add(&dom, SIO_BASE, 4096, MEM_R | MEM_W);
    mem_domain_add(&dom, SPI0_BASE, 4096, MEM_R | MEM_W);

    if (task_set_domain(sched_current_pid(), &dom) != 0) {
        printk("[ST7735Iso] Refusing to enter U-mode: memory domain not enforceable\n");
        return;
    }
    g_st7735_intruder_entered = true;
    arch_enter_user(st7735_intruder, (uintptr_t)ustack + 4096, 0, 0, 0);
}

/* Runs the probe to completion and reports what actually happened. Returns
 * false if the task never reached U-mode (domain not enforceable on this
 * build/core, or the one-page stack could not be allocated) -- in that
 * case *out_canary and *out_exited_clean say nothing about isolation,
 * matching cmd_usertest_isolation()'s own "INCONCLUSIVE" case. */
bool st7735_isolation_test(uintptr_t *out_canary, bool *out_exited_clean) {
    g_st7735_canary = 0xC0FFEE;
    g_st7735_intruder_entered = false;

    void *ustack = palloc_pages(1);
    if (!ustack) {
        *out_canary = g_st7735_canary;
        *out_exited_clean = true;
        return false;
    }

    int pid = task_create("st7735_intruder", st7735_intruder_task_body, ustack);
    if (pid < 0) {
        palloc_free(ustack, 1);
        *out_canary = g_st7735_canary;
        *out_exited_clean = true;
        return false;
    }
    for (int i = 0; i < 10000 && sched_task_state(pid) != TASK_DEAD; i++) {
        sched_yield();
    }
    long status;
    *out_exited_clean = sched_task_exited_cleanly(pid, &status);
    *out_canary = g_st7735_canary;
    palloc_free(ustack, 1);
    return g_st7735_intruder_entered;
}

void st7735_fill_screen(uint16_t color) {
    if (st7735_task_alive()) {
        uint8_t req[3];
        req[0] = ST7735_OP_FILL_SCREEN;
        put_u16(&req[1], color);
        if (st7735_call_with_retry(req, sizeof(req)) >= 0) return;
    }
    st7735_hw_fill_screen(color);
}

void st7735_draw_pixel(int x, int y, uint16_t color) {
    if (st7735_task_alive()) {
        uint8_t req[7];
        req[0] = ST7735_OP_DRAW_PIXEL;
        put_i16(&req[1], x);
        put_i16(&req[3], y);
        put_u16(&req[5], color);
        if (st7735_call_with_retry(req, sizeof(req)) >= 0) return;
    }
    st7735_hw_draw_pixel(x, y, color);
}

void st7735_draw_rect(int x, int y, int w, int h, uint16_t color) {
    if (st7735_task_alive()) {
        uint8_t req[11];
        req[0] = ST7735_OP_DRAW_RECT;
        put_i16(&req[1], x);
        put_i16(&req[3], y);
        put_i16(&req[5], w);
        put_i16(&req[7], h);
        put_u16(&req[9], color);
        if (st7735_call_with_retry(req, sizeof(req)) >= 0) return;
    }
    st7735_hw_draw_rect(x, y, w, h, color);
}

void st7735_draw_bitmap_mono(int x, int y, int w, int h, const uint16_t *bitmap, uint16_t fg_color, uint16_t bg_color) {
    if (h > 0 && (uint32_t)h <= ST7735_BITMAP_MAX_ROWS && st7735_task_alive()) {
        uint8_t req[13 + ST7735_BITMAP_MAX_ROWS * 2];
        req[0] = ST7735_OP_DRAW_BITMAP;
        put_i16(&req[1], x);
        put_i16(&req[3], y);
        put_i16(&req[5], w);
        put_i16(&req[7], h);
        put_u16(&req[9], fg_color);
        put_u16(&req[11], bg_color);
        for (int r = 0; r < h; r++) put_u16(&req[13 + r * 2], bitmap[r]);
        if (st7735_call_with_retry(req, 13u + (uint32_t)h * 2u) >= 0) return;
    }
    st7735_hw_draw_bitmap_mono(x, y, w, h, bitmap, fg_color, bg_color);
}

void st7735_draw_char(int x, int y, char c, uint16_t color, int size) {
    if (st7735_task_alive()) {
        uint8_t req[10];
        req[0] = ST7735_OP_DRAW_CHAR;
        put_i16(&req[1], x);
        put_i16(&req[3], y);
        req[5] = (uint8_t)c;
        put_u16(&req[6], color);
        put_i16(&req[8], size);
        if (st7735_call_with_retry(req, sizeof(req)) >= 0) return;
    }
    st7735_hw_draw_char(x, y, c, color, size);
}

void st7735_draw_string(int x, int y, const char *str, uint16_t color, int size) {
    size_t len = strlen(str);
    if (len <= ST7735_STR_MAX && st7735_task_alive()) {
        uint8_t req[9 + ST7735_STR_MAX];
        req[0] = ST7735_OP_DRAW_STRING;
        put_i16(&req[1], x);
        put_i16(&req[3], y);
        put_u16(&req[5], color);
        put_i16(&req[7], size);
        memcpy(&req[9], str, len);
        if (st7735_call_with_retry(req, 9u + (uint32_t)len) >= 0) return;
    }
    st7735_hw_draw_string(x, y, str, color, size);
}
