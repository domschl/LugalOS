/*
 * Standalone Minimal Hardware Test for RP2350 (Raspberry Pi Pico 2)
 * Adopted from working bare-metal RP2350 driver (rp2350_cld).
 *
 * Actions:
 *   1. Initializes UART0 (115200 baud on GP0 TX / GP1 RX)
 *   2. Toggles dual LEDs: GP25 (on-board LED) and GP16 (external LED)
 *   3. Sends test feedback messages over UART0 and echoes RX characters
 */

#include <stdint.h>
#include <stdbool.h>

#define CLOCKS_BASE             0x40010000UL
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

#define UART0_BASE              0x40070000UL

#define REG(addr) (*(volatile uint32_t *)(addr))

#define PIN_MASK ((1u << 25) | (1u << 16))

static void delay(volatile uint32_t count) {
    while (count--) {
        __asm__ volatile ("nop");
    }
}

static void uart_init(uint32_t baudrate) {
    (void)baudrate;

    /* 1. Explicitly enable clk_peri and attach it to clk_sys (150 MHz) */
    REG(CLOCKS_BASE + 0x48) = (1u << 11);

    /* 2. Unreset IO_BANK0 (bit 6), PADS_BANK0 (bit 9), and UART0 (bit 26) */
    uint32_t unreset_mask = (1u << 6) | (1u << 9) | (1u << 26);
    REG(RESETS_ATOMIC_CLEAR) = unreset_mask;
    
    /* Wait for RESET_DONE status register @ 0x40020008 */
    while ((REG(RESETS_RESET_DONE) & unreset_mask) != unreset_mask);

    /* 3. Mux GP0 to UART0 TX (Function 2), GP1 to UART0 RX (Function 2) */
    REG(IO_BANK0_CTRL(0)) = 2;
    REG(PADS_BANK0_PAD(0)) = 0x56;
    REG(IO_BANK0_CTRL(1)) = 2;
    REG(PADS_BANK0_PAD(1)) = 0x56;

    /* 4. Disable UART before programming baud rate and line control */
    REG(UART0_BASE + 0x30) = 0;

    /* 5. Configure Baud Rate for 150MHz clk_peri -> 115200 baud */
    REG(UART0_BASE + 0x24) = 81;  // UARTIBRD
    REG(UART0_BASE + 0x28) = 24;  // UARTFBRD

    /* 6. 8 bits, no parity, 1 stop bit, enable FIFOs */
    REG(UART0_BASE + 0x2C) = (3u << 5) | (1u << 4); // UARTLCR_H

    /* 7. Enable UART0, TX & RX */
    REG(UART0_BASE + 0x30) = (1u << 0) | (1u << 8) | (1u << 9); // UARTCR
}

static void uart_putc(char c) {
    while (REG(UART0_BASE + 0x18) & (1u << 5));
    REG(UART0_BASE + 0x00) = (uint8_t)c;
}

static bool uart_has_char(void) {
    return (REG(UART0_BASE + 0x18) & (1u << 4)) == 0;
}

static char uart_getc(void) {
    while (!uart_has_char());
    return (char)(REG(UART0_BASE + 0x00) & 0xFF);
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
    /* Initialize UART0 (115200 baud on GP0) */
    uart_init(115200);

    /* Unreset IO_BANK0 (bit 6) and PADS_BANK0 (bit 9) */
    uint32_t unreset_mask = (1u << 6) | (1u << 9);
    REG(RESETS_ATOMIC_CLEAR) = unreset_mask;
    while ((REG(RESETS_RESET_DONE) & unreset_mask) != unreset_mask);

    /* Mux GP25 (onboard LED) and GP16 (external LED) to SIO (Function 5) */
    REG(IO_BANK0_CTRL(25)) = 5;
    REG(IO_BANK0_CTRL(16)) = 5;

    /* Enable pad output buffers */
    REG(PADS_BANK0_PAD(25)) = 0x56;
    REG(PADS_BANK0_PAD(16)) = 0x56;

    /* Enable GP25 and GP16 output buffers in SIO */
    REG(SIO_GPIO_OE_SET) = PIN_MASK;

    uart_puts("[RP2350_MINIMAL] UART Echo & Dual LED Test Initialized!\n");

    uint32_t timer = 0;
    bool led_state = false;

    /* Interactive Echo Loop */
    while (1) {
        if (uart_has_char()) {
            char c = uart_getc();
            if (c == '\r') uart_putc('\n');
            uart_putc(c);
        }

        timer++;
        if (timer >= 500000) {
            timer = 0;
            led_state = !led_state;
            if (led_state) {
                REG(SIO_GPIO_OUT_SET) = PIN_MASK;
            } else {
                REG(SIO_GPIO_OUT_CLR) = PIN_MASK;
            }
        }
    }
}
