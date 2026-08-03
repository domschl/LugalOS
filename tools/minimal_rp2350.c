/*
 * Standalone Minimal Hardware Test for RP2350 (Raspberry Pi Pico 2)
 * Pure bare-metal RISC-V C file (No OS kernel, No interrupts, No dependencies).
 *
 * Actions:
 *   1. Toggles onboard LED (GPIO 25) at 2 Hz
 *   2. Outputs "RP2350 Bare-Metal Hardware Test OK!\r\n" to UART0 at 115200 baud
 */

#include <stdint.h>
#include <stdbool.h>

#define RESETS_BASE        0x40020000UL
#define RESETS_RESET       (*(volatile uint32_t *)(RESETS_BASE + 0x00))
#define RESETS_RESET_DONE  (*(volatile uint32_t *)(RESETS_BASE + 0x08))

#define RESET_MASK_UART0      (1u << 26)
#define RESET_MASK_IO_BANK0   (1u << 6)
#define RESET_MASK_PADS_BANK0 (1u << 9)

#define IO_BANK0_BASE      0x40028000UL
#define GPIO_CTRL(n)       (*(volatile uint32_t *)(IO_BANK0_BASE + 0x004 + (n)*8))
#define GPIO_FUNC_UART     2
#define GPIO_FUNC_SIO      5

#define PADS_BANK0_BASE    0x40038000UL
#define PAD_GPIO(n)        (*(volatile uint32_t *)(PADS_BANK0_BASE + 0x004 + (n)*4))

#define SIO_BASE           0xD0000000UL
#define SIO_GPIO_OUT_SET   (*(volatile uint32_t *)(SIO_BASE + 0x018))
#define SIO_GPIO_OUT_CLR   (*(volatile uint32_t *)(SIO_BASE + 0x020))
#define SIO_GPIO_OE_SET    (*(volatile uint32_t *)(SIO_BASE + 0x038))

#define LED_PIN            25
#define LED_MASK           (1u << LED_PIN)

#define UART0_BASE         0x40034000UL
#define UARTDR             (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UARTFR             (*(volatile uint32_t *)(UART0_BASE + 0x18))
#define UARTIBRD           (*(volatile uint32_t *)(UART0_BASE + 0x24))
#define UARTFBRD           (*(volatile uint32_t *)(UART0_BASE + 0x28))
#define UARTLCR_H          (*(volatile uint32_t *)(UART0_BASE + 0x2c))
#define UARTCR             (*(volatile uint32_t *)(UART0_BASE + 0x30))

#define UARTFR_TXFF        (1u << 5)

static void delay(volatile uint32_t count) {
    while (count--) {
        __asm__ volatile("nop");
    }
}

static void uart_putc(char c) {
    while (UARTFR & UARTFR_TXFF);
    UARTDR = (uint8_t)c;
}

static void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void trap_handler(void *tf) {
    (void)tf;
    while (1);
}

void minimal_main(void) {
    /* 1. Release RESETS for IO_BANK0, PADS_BANK0, and UART0 */
    uint32_t mask = RESET_MASK_IO_BANK0 | RESET_MASK_PADS_BANK0 | RESET_MASK_UART0;
    RESETS_RESET &= ~mask;
    while ((RESETS_RESET_DONE & mask) != mask);

    /* 2. Configure PADS_BANK0 for GPIO25: clear ISO bit (bit 8) and OD bit (bit 7), set drive 4mA */
    PAD_GPIO(LED_PIN) = 0x00000056;

    /* 3. Configure GPIO25 for SIO output */
    GPIO_CTRL(LED_PIN) = GPIO_FUNC_SIO;
    SIO_GPIO_OE_SET    = LED_MASK;

    /* 4. Configure GPIO0 -> UART0 TX, GPIO1 -> UART0 RX */
    GPIO_CTRL(0) = GPIO_FUNC_UART;
    GPIO_CTRL(1) = GPIO_FUNC_UART;

    /* 5. Configure UART0: 115200 8N1 at 150 MHz CLK_PERI */
    UARTCR = 0;
    UARTIBRD = 81;
    UARTFBRD = 24;
    UARTLCR_H = 0x70;
    UARTCR = (1u << 0) | (1u << 8) | (1u << 9);

    /* 6. Blink LED and print Hello message continuously */
    uint32_t counter = 0;
    while (1) {
        SIO_GPIO_OUT_SET = LED_MASK;
        uart_puts("RP2350 Bare-Metal Test: LED ON!\r\n");
        delay(3000000);

        SIO_GPIO_OUT_CLR = LED_MASK;
        uart_puts("RP2350 Bare-Metal Test: LED OFF!\r\n");
        delay(3000000);

        counter++;
    }
}
