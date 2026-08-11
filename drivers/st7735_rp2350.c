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
#include "lugalos_config.h"
#include <stdint.h>
#include <stddef.h>

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

void st7735_init(void) {
    st7735_init_hardware();
    st7735_fill_screen(ST7735_BLACK);
}

void st7735_fill_screen(uint16_t color) {
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

void st7735_draw_pixel(int x, int y, uint16_t color) {
    if (x < 0 || x >= ST7735_WIDTH || y < 0 || y >= ST7735_HEIGHT) return;
    st7735_set_window((uint8_t)x, (uint8_t)y, (uint8_t)x, (uint8_t)y);
    uint8_t data[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };
    st7735_write_data_buf(data, 2);
}

void st7735_draw_rect(int x, int y, int w, int h, uint16_t color) {
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

void st7735_draw_bitmap_mono(int x, int y, int w, int h, const uint16_t *bitmap, uint16_t fg_color, uint16_t bg_color) {
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

/* Standard 5x7 pixel ASCII font, covering ASCII 32 to 127. */
static const uint8_t font5x7[96][5] = {
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

void st7735_draw_char(int x, int y, char c, uint16_t color, int size) {
    if (c < 32 || c > 127) return;
    int font_idx = c - 32;

    for (int col = 0; col < 5; col++) {
        uint8_t line = font5x7[font_idx][col];
        for (int row = 0; row < 8; row++) {
            if (line & 1) {
                if (size == 1) {
                    st7735_draw_pixel(x + col, y + row, color);
                } else {
                    st7735_draw_rect(x + col * size, y + row * size, size, size, color);
                }
            }
            line >>= 1;
        }
    }
}

void st7735_draw_string(int x, int y, const char *str, uint16_t color, int size) {
    while (*str) {
        st7735_draw_char(x, y, *str, color, size);
        x += 6 * size; /* 5 pixels width + 1 pixel padding */
        if (x > 120) break; /* wrap limit */
        str++;
    }
}
