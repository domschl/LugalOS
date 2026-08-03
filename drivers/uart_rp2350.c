/*
 * LugalOS Hardware Driver: PL011 UART + Onboard LED for RP2350 (Pico 2)
 *
 * Exact RP2350 Register Definitions verified from Pico SDK rp2350 headers:
 *   RESETS_BASE      = 0x40020000
 *   RESETS_UART0     = bit 26 (0x04000000)
 *   RESETS_IO_BANK0  = bit 6  (0x00000040)
 *   RESETS_PADS_BANK0= bit 9  (0x00000200)
 *
 *   SIO_BASE         = 0xd0000000
 *   SIO_GPIO_OUT_SET = 0x018
 *   SIO_GPIO_OUT_CLR = 0x020
 *   SIO_GPIO_OE_SET  = 0x038
 *
 *   IO_BANK0_BASE    = 0x40028000
 *   GPIO_CTRL(n)     = IO_BANK0_BASE + 0x004 + n*8
 *   FUNCSEL_SIO      = 5
 *   FUNCSEL_UART     = 2
 *
 *   UART0_BASE       = 0x40034000
 *   CLK_PERI default = 150 MHz
 *   IBRD=81, FBRD=24 for 115200 8N1 at 150 MHz
 */

#include "drivers/uart.h"
#include <stdint.h>
#include <stdbool.h>

/* --- RESETS (0x40020000) ------------------------------------------------ */
#define RESETS_BASE        0x40020000UL
#define RESETS_RESET       (*(volatile uint32_t *)(RESETS_BASE + 0x00))
#define RESETS_RESET_DONE  (*(volatile uint32_t *)(RESETS_BASE + 0x08))

#define RESET_MASK_UART0      (1u << 26)
#define RESET_MASK_IO_BANK0   (1u << 6)
#define RESET_MASK_PADS_BANK0 (1u << 9)

/* --- IO_BANK0 (0x40028000) ---------------------------------------------- */
#define IO_BANK0_BASE  0x40028000UL
#define GPIO_CTRL(n)   (*(volatile uint32_t *)(IO_BANK0_BASE + 0x004 + (n)*8))

#define GPIO_FUNC_UART 2
#define GPIO_FUNC_SIO  5

/* --- SIO (0xd0000000) --------------------------------------------------- */
#define SIO_BASE           0xD0000000UL
#define SIO_GPIO_OUT_SET   (*(volatile uint32_t *)(SIO_BASE + 0x018))
#define SIO_GPIO_OUT_CLR   (*(volatile uint32_t *)(SIO_BASE + 0x020))
#define SIO_GPIO_OE_SET    (*(volatile uint32_t *)(SIO_BASE + 0x038))

#define LED_PIN   25
#define LED_MASK  (1u << LED_PIN)

/* --- PL011 UART0 (0x40034000) ------------------------------------------- */
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

static void delay_cycles(volatile uint32_t n) {
    while (n--) { __asm__ volatile("nop"); }
}

void led_on(void)  { SIO_GPIO_OUT_SET = LED_MASK; }
void led_off(void) { SIO_GPIO_OUT_CLR = LED_MASK; }

void led_blink_phase(int count) {
    for (int i = 0; i < count; i++) {
        led_on();
        delay_cycles(2000000);
        led_off();
        delay_cycles(2000000);
    }
    delay_cycles(6000000);
}

void uart_init(uintptr_t base_addr) {
    if (base_addr != 0) {
        uart_base = (volatile uint32_t *)base_addr;
    }

    /* 1. Unreset IO_BANK0 and PADS_BANK0 first so GPIOs work */
    uint32_t gpio_resets = RESET_MASK_IO_BANK0 | RESET_MASK_PADS_BANK0;
    RESETS_RESET &= ~gpio_resets;
    while ((RESETS_RESET_DONE & gpio_resets) != gpio_resets);

    /* 2. Configure GPIO25 LED for SIO output and signal phase 1 (alive!) */
    GPIO_CTRL(LED_PIN) = GPIO_FUNC_SIO;
    SIO_GPIO_OE_SET    = LED_MASK;
    led_blink_phase(1);

    /* 3. Unreset UART0 peripheral */
    RESETS_RESET &= ~RESET_MASK_UART0;
    while ((RESETS_RESET_DONE & RESET_MASK_UART0) != RESET_MASK_UART0);

    /* 4. Configure GPIO0 -> UART0 TX, GPIO1 -> UART0 RX */
    GPIO_CTRL(0) = GPIO_FUNC_UART;
    GPIO_CTRL(1) = GPIO_FUNC_UART;

    /* 5. Configure PL011 UART0: 115200 8N1 at 150 MHz CLK_PERI */
    uart_base[UARTCR] = 0;             /* Disable UART */
    uart_base[UARTIBRD] = 81;          /* 150MHz / (16 * 115200) = 81.3802 */
    uart_base[UARTFBRD] = 24;          /* 0.3802 * 64 = 24.33 -> 24 */
    uart_base[UARTLCR_H] = 0x70;       /* 8-bit, FIFO enabled */
    uart_base[UARTCR] = (1u << 0) | (1u << 8) | (1u << 9); /* Enable UART, TX, RX */

    /* Signal phase 2: UART initialized! */
    led_blink_phase(2);

    /* Print immediate test message directly to UART */
    uart_puts("\r\n[RP2350 Hardware UART0 Online] Hello, worlds!\r\n");
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
