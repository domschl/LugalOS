/*
 * LugalOS Hardware Driver: PL011 UART + LED Boot Indicator for RP2350
 * UART0: GPIO0 (TX, Pin 1) / GPIO1 (RX, Pin 2) at 115,200 baud
 * LED:   GPIO25 (onboard Pico 2 LED) — blinks boot phase codes
 *
 * Boot LED codes (blinks separated by a long pause):
 *   1 blink  = entered uart_init
 *   2 blinks = UART fully configured and enabled
 *   3 blinks = kernel_main reached (printed from main.c)
 *
 * Clock note: RP2350 BootROM configures PLL_SYS → 150 MHz, CLK_PERI = 150 MHz.
 *   IBRD = 150,000,000 / (16 * 115200) = 81.38 → 81
 *   FBRD = round(0.38 * 64)                    = 24
 */

#include "drivers/uart.h"
#include <stdint.h>
#include <stdbool.h>

/* --- Peripheral Reset Controller ---------------------------------------- */
#define RESETS_BASE        0x40020000UL
#define RESETS_RESET       (*(volatile uint32_t *)(RESETS_BASE + 0x00))
#define RESETS_RESET_DONE  (*(volatile uint32_t *)(RESETS_BASE + 0x08))
#define RESET_BIT_UART0      22
#define RESET_BIT_IO_BANK0   11
#define RESET_BIT_PADS_BANK0  8

/* --- IO_BANK0: GPIO function selection ---------------------------------- */
#define IO_BANK0_BASE  0x40028000UL
/* GPIO_CTRL for GPIO N: base + N*8 + 4 */
#define GPIO_CTRL(n)   (*(volatile uint32_t *)(IO_BANK0_BASE + (n)*8 + 4))
#define GPIO_FUNC_UART 2   /* UART function select for GPIO0/GPIO1 */
#define GPIO_FUNC_SIO  5   /* SIO function select for GPIO25 LED   */

/* --- SIO (Single-Cycle I/O): GPIO output/OE ----------------------------- */
#define SIO_BASE           0xD0000000UL
#define SIO_GPIO_OUT_SET   (*(volatile uint32_t *)(SIO_BASE + 0x014))
#define SIO_GPIO_OUT_CLR   (*(volatile uint32_t *)(SIO_BASE + 0x018))
#define SIO_GPIO_OE_SET    (*(volatile uint32_t *)(SIO_BASE + 0x024))

/* --- Onboard LED -------------------------------------------------------- */
#define LED_PIN   25
#define LED_MASK  (1u << LED_PIN)

/* --- PL011 UART0 -------------------------------------------------------- */
#define UART0_BASE_DEFAULT 0x40034000UL
static volatile uint32_t *uart_base = (volatile uint32_t *)UART0_BASE_DEFAULT;

#define UARTDR    (0x00 / 4)
#define UARTFR    (0x18 / 4)
#define UARTIBRD  (0x24 / 4)
#define UARTFBRD  (0x28 / 4)
#define UARTLCR_H (0x2c / 4)
#define UARTCR    (0x30 / 4)
#define UARTFR_TXFF (1u << 5)
#define UARTFR_RXFE (1u << 4)

/* ======================================================================== */
/* LED boot indicator                                                        */
/* ======================================================================== */

static void delay_cycles(volatile uint32_t n) {
    while (n--) { __asm__ volatile("nop"); }
}

static void led_on(void)  { SIO_GPIO_OUT_SET = LED_MASK; }
static void led_off(void) { SIO_GPIO_OUT_CLR = LED_MASK; }

/* Blink the LED `count` times then pause — call before UART is ready */
void led_blink_phase(int count) {
    for (int i = 0; i < count; i++) {
        led_on();
        delay_cycles(200000);
        led_off();
        delay_cycles(200000);
    }
    delay_cycles(600000);  /* inter-phase gap */
}

static void led_init(void) {
    /* Route GPIO25 to SIO, enable as output */
    GPIO_CTRL(LED_PIN) = GPIO_FUNC_SIO;
    SIO_GPIO_OE_SET    = LED_MASK;
    led_off();
}

/* ======================================================================== */
/* UART driver                                                               */
/* ======================================================================== */

void uart_init(uintptr_t base_addr) {
    if (base_addr != 0) {
        uart_base = (volatile uint32_t *)base_addr;
    }

    /* Phase 1: LED init + 1 blink = we are alive and in uart_init */
    led_init();
    led_blink_phase(1);

    /* Unreset IO_BANK0, PADS_BANK0, UART0 */
    uint32_t mask = (1u << RESET_BIT_UART0)
                  | (1u << RESET_BIT_IO_BANK0)
                  | (1u << RESET_BIT_PADS_BANK0);
    RESETS_RESET &= ~mask;
    while ((RESETS_RESET_DONE & mask) != mask);

    /* GPIO0 → UART0 TX, GPIO1 → UART0 RX */
    GPIO_CTRL(0) = GPIO_FUNC_UART;
    GPIO_CTRL(1) = GPIO_FUNC_UART;

    /* Disable UART before configuration */
    uart_base[UARTCR] = 0;

    /*
     * 115,200 baud — RP2350 BootROM leaves CLK_PERI = 150 MHz (PLL_SYS).
     *   IBRD = 150,000,000 / (16 * 115200) = 81.38 → 81
     *   FBRD = round(0.38 * 64)                    = 24
     * Actual rate: 150,000,000 / (16 * (81 + 24/64)) = 115,207 baud ✓
     */
    uart_base[UARTIBRD] = 81;
    uart_base[UARTFBRD] = 24;

    /* 8N1, FIFOs enabled: WLEN=0b11<<5=0x60, FEN=0x10 → 0x70 */
    uart_base[UARTLCR_H] = 0x70;

    /* Enable UART, TX, RX */
    uart_base[UARTCR] = (1u << 0) | (1u << 8) | (1u << 9);

    /* Phase 2: 2 blinks = UART configured */
    led_blink_phase(2);
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
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}
