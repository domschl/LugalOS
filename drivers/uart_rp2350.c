/*
 * LugalOS Hardware Driver: PL011 UART0 + Dual Onboard/External LEDs for RP2350 (Pico 2)
 * Adopted from working bare-metal RP2350 driver (rp2350_cld).
 *
 * Hardware Map:
 *   GP0  : UART0 TX (Function 2)
 *   GP1  : UART0 RX (Function 2)
 *   GP25 : Onboard LED (Function 5 - SIO)
 *   GP16 : External LED (Function 5 - SIO, active-high pulse)
 */

#include "drivers/uart.h"
#include "drivers/uart_net.h"
#include "fs/p9_link.h"
#include "kernel/sched.h"
#include "kernel/time.h"
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

#define LED_MASK  ((1u << 25) | (1u << 16))

#include "drivers/usb_cdc.h"

static void delay_cycles(volatile uint32_t n) {
    while (n--) {
        if ((n & 0xFFFF) == 0) {
            usb_cdc_task();
        }
        __asm__ volatile("nop");
    }
}

static inline uint32_t read_mcycle(void) {
    uint32_t c;
    __asm__ volatile("csrr %0, mcycle" : "=r"(c));
    return c;
}

void led_on(void) {
    REG(SIO_GPIO_OUT_SET) = LED_MASK;
}

void led_off(void) {
    REG(SIO_GPIO_OUT_CLR) = LED_MASK;
}

void led_blink_phase(int count) {
    for (int i = 0; i < count; i++) {
        led_on();
        delay_cycles(4000000);
        led_off();
        delay_cycles(4000000);
    }
    delay_cycles(8000000);
}

/* Heartbeat on GP16: 0.5 Hz, brief flash.
 *
 * Both numbers here used to be counted in loop iterations and nop-spins --
 * "5000000 polls ~ 2s" and "7500000 cycles = 50 ms". Neither measures time:
 * one counts how often something calls this function, the other how fast an
 * empty loop runs, and both are calibrations against a particular clock rate
 * and flash wait-state configuration. When the oscillator setup changed, the
 * period stretched to ~20 s and the flash to ~1 s -- not because the timing
 * broke, but because there was no timing, only two constants that used to
 * come out about right.
 *
 * Driven from the monotonic clock now, so the period is 2 s of real time on
 * whatever clock the board ends up running at.
 *
 * And non-blocking, which matters more than the accuracy. The old pulse was a
 * spin-wait sitting inside the character-poll path: for its whole duration
 * the UART went unpolled and usb_cdc_task() ran only as often as
 * delay_cycles() happened to call it. At the intended 50 ms that was
 * survivable; at the 1 s it had drifted to, it starves USB servicing for a
 * second at a time. The LED is a two-state machine stepped by this function
 * instead, so nothing here ever waits. */
#define HEARTBEAT_PERIOD_MS 2000u  /* 0.5 Hz */
#define HEARTBEAT_ON_MS       40u  /* brief flash, not a duty cycle */

void gp16_alive_tick(void) {
    usb_cdc_task();

    static uint64_t next_on_ms;
    static uint64_t off_at_ms;
    static bool     led_is_on;

    uint64_t now = time_get_ms();
    if (led_is_on) {
        if (now >= off_at_ms) {
            REG(SIO_GPIO_OUT_CLR) = (1u << 16);
            led_is_on = false;
        }
    } else if (now >= next_on_ms) {
        REG(SIO_GPIO_OUT_SET) = (1u << 16);
        led_is_on  = true;
        off_at_ms  = now + HEARTBEAT_ON_MS;
        next_on_ms = now + HEARTBEAT_PERIOD_MS;
    }
}

// Raw physical-UART byte access (defined below, after uart_putc()); handed
// to drivers/uart_net.c's A3b demux here in uart_init() so it needs a
// forward declaration.
static bool hw_uart_has_char(void);
static uint8_t hw_uart_getc(void);

void uart_init(uintptr_t base_addr) {
    (void)base_addr;

    /* 1. Explicitly enable clk_peri and attach it to clk_sys (150 MHz) */
    REG(CLOCKS_BASE + 0x48) = (1u << 11);

    /* 2. Unreset IO_BANK0 (bit 6), PADS_BANK0 (bit 9), UART0 (bit 26) */
    uint32_t unreset_mask = (1u << 6) | (1u << 9) | (1u << 26);
    REG(RESETS_ATOMIC_CLEAR) = unreset_mask;
    
    /* Wait for RESET_DONE status register @ 0x40020008 */
    while ((REG(RESETS_RESET_DONE) & unreset_mask) != unreset_mask);

    /* 3. Configure LEDs (GP25 onboard + GP16 external) for SIO (Function 5) */
    REG(IO_BANK0_CTRL(25)) = 5;
    REG(IO_BANK0_CTRL(16)) = 5;
    REG(PADS_BANK0_PAD(25)) = 0x56;
    REG(PADS_BANK0_PAD(16)) = 0x56;
    REG(SIO_GPIO_OE_SET) = LED_MASK;

    /* Default GP16 to LOW (OFF in active-high configuration) */
    REG(SIO_GPIO_OUT_CLR) = (1u << 16);

    /* 4. Mux GP0 to UART0 TX (Function 2), GP1 to UART0 RX (Function 2) */
    REG(IO_BANK0_CTRL(0)) = 2;
    REG(PADS_BANK0_PAD(0)) = 0x56;

    REG(IO_BANK0_CTRL(1)) = 2;
    REG(PADS_BANK0_PAD(1)) = 0x56;

    /* 5. Disable UART before programming baud rate and line control */
    REG(UART0_BASE + 0x30) = 0;

    /* 6. Configure Baud Rate for 150MHz clk_peri -> 115200 baud */
    REG(UART0_BASE + 0x24) = 81;  // UARTIBRD
    REG(UART0_BASE + 0x28) = 24;  // UARTFBRD

    /* 7. 8 bits, no parity, 1 stop bit, enable FIFOs */
    REG(UART0_BASE + 0x2C) = (3u << 5) | (1u << 4); // UARTLCR_H

    /* 8. Enable UART0, Transmit (TXE) & Receive (RXE) */
    REG(UART0_BASE + 0x30) = (1u << 0) | (1u << 8) | (1u << 9); // UARTCR

    /* Signal phase: Hardware & LEDs initialized! */
    led_blink_phase(2);

    uart_demux_init(hw_uart_has_char, hw_uart_getc);

    uart_puts("\r\n[RP2350 Hardware UART0 Online] LugalOS Microkernel Starting...\r\n");
}

static void uart_hw_putc(char c) {
    while (REG(UART0_BASE + 0x18) & (1u << 5));
    REG(UART0_BASE + 0x00) = (uint8_t)c;
}

void uart_putc(char c) {
    uart_hw_putc(c);
    usb_cdc_putc(c); // mirror interactive shell output to /dev/ttyACM0 when connected
}

void uart_debug_putc(char c) {
    uart_hw_putc(c); // kernel debug log: physical UART only, no USB mirror
}

// Raw physical-UART byte access (forward-declared above uart_init(), which
// hands these to drivers/uart_net.c's A3b demux) -- kept separate from
// uart_has_char()/uart_getc() below so the demux (once a user opts in via
// `p9share`) can pull straight from the PL011 registers without going
// through the console-ring indirection it is itself responsible for
// providing.
static bool hw_uart_has_char(void) {
    return (REG(UART0_BASE + 0x18) & (1u << 4)) == 0; // physical UART RX FIFO not empty
}

static uint8_t hw_uart_getc(void) {
    return (uint8_t)(REG(UART0_BASE + 0x00) & 0xFF);
}

// The A3b demux only ever applies to the physical UART0 wire -- USB CDC
// ACM0 (below) is a channel of its own with no shared-wire ambiguity, so it
// keeps being checked directly and unconditionally, same as before.
bool uart_has_char(void) {
    gp16_alive_tick();
    if (uart_demux_is_enabled()) {
        if (uart_demux_console_has_char()) return true;
    } else if (hw_uart_has_char()) {
        return true;
    }
    return usb_cdc_has_char(); // or a byte typed over /dev/ttyACM0
}

char uart_getc(void) {
    while (!uart_has_char()) {
        gp16_alive_tick();
        usb_cdc_task();
        sched_yield(); /* B2: see drivers/uart_16550.c's equivalent */
    }
    if (uart_demux_is_enabled()) {
        if (uart_demux_console_has_char()) return uart_demux_console_getc();
    } else if (hw_uart_has_char()) {
        return (char)hw_uart_getc();
    }
    return usb_cdc_getc();
}

void uart_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_debug_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_debug_putc('\r');
        uart_debug_putc(*s++);
    }
}
