/*
 * LugalOS Hardware Driver: PL011 UART Engine for RP2350 (Raspberry Pi Pico 2)
 * Controls UART0 on GPIO0 (TXD, Pico Pin 1) and GPIO1 (RXD, Pico Pin 2) at 115,200 baud.
 *
 * Clock strategy: We explicitly set CLK_PERI source to XOSC (12 MHz crystal) so the
 * baud rate is independent of any PLL configuration done (or not done) by the BootROM.
 *   IBRD = 12,000,000 / (16 * 115200) = 6.510 => 6
 *   FBRD = round(0.510 * 64)                   = 33
 */

#include "drivers/uart.h"

/* RP2350 Peripheral Reset Controller */
#define RESETS_BASE        0x40020000UL
#define RESETS_RESET       (*(volatile uint32_t *)(RESETS_BASE + 0x00))
#define RESETS_RESET_DONE  (*(volatile uint32_t *)(RESETS_BASE + 0x08))

/* IO_BANK0: GPIO function selection */
#define IO_BANK0_BASE      0x40028000UL
#define GPIO0_CTRL         (*(volatile uint32_t *)(IO_BANK0_BASE + 0x004))
#define GPIO1_CTRL         (*(volatile uint32_t *)(IO_BANK0_BASE + 0x00c))

/* RP2350 Crystal Oscillator (XOSC) — 12 MHz */
#define XOSC_BASE          0x40048000UL
#define XOSC_CTRL          (*(volatile uint32_t *)(XOSC_BASE + 0x00))
#define XOSC_STATUS        (*(volatile uint32_t *)(XOSC_BASE + 0x04))
#define XOSC_STARTUP       (*(volatile uint32_t *)(XOSC_BASE + 0x0C))
#define XOSC_FREQ_RANGE    0xAA0   /* 1–15 MHz */
#define XOSC_ENABLE_MAGIC  0xFAB   /* magic enable word */
#define XOSC_STATUS_STABLE (1u << 31)

/* RP2350 Clock system */
#define CLOCKS_BASE        0x40010000UL
/* CLK_PERI_CTRL — peripheral clock control register */
#define CLK_PERI_CTRL      (*(volatile uint32_t *)(CLOCKS_BASE + 0x48))
/* CLK_PERI_CTRL bits: AUXSRC[7:5], KILL[10], ENABLE[11] */
#define CLK_PERI_AUXSRC_XOSC  (3u << 5)   /* AUXSRC=3 → XOSC */
#define CLK_PERI_ENABLE        (1u << 11)
#define CLK_PERI_KILL          (1u << 10)

/* PL011 UART0 */
#define UART0_BASE_DEFAULT 0x40034000UL
static volatile uint32_t *uart_base = (volatile uint32_t *)UART0_BASE_DEFAULT;

#define UARTDR    (0x00 / 4)
#define UARTFR    (0x18 / 4)
#define UARTIBRD  (0x24 / 4)
#define UARTFBRD  (0x28 / 4)
#define UARTLCR_H (0x2c / 4)
#define UARTCR    (0x30 / 4)

#define UARTFR_TXFF (1u << 5)   /* TX FIFO full */
#define UARTFR_RXFE (1u << 4)   /* RX FIFO empty */

/* RESETS bit positions for RP2350 */
#define RESET_BIT_UART0     22
#define RESET_BIT_IO_BANK0  11
#define RESET_BIT_PADS_BANK0 8

static void xosc_init(void) {
    /* Startup delay: ~1 ms at 12 MHz = 47 cycles of 256 XOSC periods */
    XOSC_STARTUP = 47;
    /* Enable XOSC: set ENABLE magic + frequency range */
    XOSC_CTRL = (XOSC_ENABLE_MAGIC << 12) | XOSC_FREQ_RANGE;
    /* Wait until crystal is stable */
    while (!(XOSC_STATUS & XOSC_STATUS_STABLE));
}

static void clk_peri_from_xosc(void) {
    /* Kill CLK_PERI briefly to safely switch source */
    CLK_PERI_CTRL = CLK_PERI_KILL;
    /* Set source to XOSC (12 MHz), enable */
    CLK_PERI_CTRL = CLK_PERI_AUXSRC_XOSC | CLK_PERI_ENABLE;
}

void uart_init(uintptr_t base_addr) {
    if (base_addr != 0) {
        uart_base = (volatile uint32_t *)base_addr;
    }

    /* Ensure XOSC is running and route CLK_PERI from it (12 MHz, known clock) */
    xosc_init();
    clk_peri_from_xosc();

    /* Unreset IO_BANK0, PADS_BANK0, and UART0 */
    uint32_t mask = (1u << RESET_BIT_UART0)
                  | (1u << RESET_BIT_IO_BANK0)
                  | (1u << RESET_BIT_PADS_BANK0);
    RESETS_RESET &= ~mask;
    while ((RESETS_RESET_DONE & mask) != mask);

    /* Configure GPIO0 → UART0 TX (func 2), GPIO1 → UART0 RX (func 2) */
    GPIO0_CTRL = 2;
    GPIO1_CTRL = 2;

    /* Disable UART before configuration */
    uart_base[UARTCR] = 0;

    /*
     * Baud rate: 115,200 with CLK_PERI = XOSC = 12 MHz
     *   BRD = 12,000,000 / (16 * 115200) = 6.5104
     *   IBRD = 6
     *   FBRD = round(0.5104 * 64) = 33
     */
    uart_base[UARTIBRD] = 6;
    uart_base[UARTFBRD] = 33;

    /* 8N1, FIFO enabled: WLEN=0b11<<5=0x60, FEN=0x10 → 0x70 */
    uart_base[UARTLCR_H] = 0x70;

    /* Enable UART, TX, RX: UARTEN=bit0, TXE=bit8, RXE=bit9 */
    uart_base[UARTCR] = (1u << 0) | (1u << 8) | (1u << 9);
}

void uart_putc(char c) {
    if (!uart_base) return;
    while (uart_base[UARTFR] & UARTFR_TXFF);
    uart_base[UARTDR] = (uint8_t)c;
}

bool uart_has_char(void) {
    if (!uart_base) return false;
    return (uart_base[UARTFR] & UARTFR_RXFE) == 0;
}

char uart_getc(void) {
    while (!uart_has_char());
    return (char)(uart_base[UARTDR] & 0xFF);
}

void uart_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}
