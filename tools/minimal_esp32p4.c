/*
 * Standalone Minimal Hardware Test for ESP32-P4 (Waveshare ESP32-P4-NANO)
 *
 * E1, plan/phase27_esp32p4_bringup.md. The direct counterpart of
 * tools/minimal_rp2350.c, and it exists for the same reason: to be the
 * smallest thing that can prove the board is alive, so that every later
 * failure has something known-good to be compared against.
 *
 * Actions:
 *   1. Writes a banner to UART0
 *   2. Reports misa/mtvec/mhartid/mstatus straight off the silicon
 *   3. Drives GPIO20 and reads the pad back, both levels
 *   4. Echoes received characters back, toggling GPIO20 on each heartbeat
 *
 * ## Why this is so much smaller than the RP2350 one
 *
 * minimal_rp2350.c has to bring up clk_peri, unreset three peripherals, mux
 * two pads and program a baud divisor before it can emit a byte. This file
 * does none of that, and the difference is not laziness -- it is the whole
 * finding of E1's first step.
 *
 * The ESP32-P4's boot ROM prints its own banner on UART0 before handing
 * over ("ESP-ROM:esp32p4-eco2-20240710", observed on this board 2026-09-05),
 * and UART0's default pins are GPIO37/GPIO38, which is exactly where the
 * NANO's CH343P USB-UART bridge sits. So by the time our code runs, the
 * UART is already clocked, muxed and running at 115200 8N1. Writing to the
 * TX FIFO is sufficient.
 *
 * That is a deliberate choice for the *first* program and not a claim about
 * what the kernel should do: E2 configures the UART itself, because a kernel
 * that silently depends on a bootloader's leftovers is a kernel that breaks
 * the first time it is booted a different way. Here, depending on it is the
 * point -- it removes every variable except "does our code execute at all".
 *
 * ## Where this runs
 *
 * In L2MEM, loaded over the download protocol with `esptool load-ram`, and
 * never written to flash. See tools/build_minimal_esp32p4.sh. Flash still
 * holds whatever it held; nothing here can brick the board, and the factory
 * image survives.
 *
 * The load address is constrained by the ROM, which keeps static data in
 * L2MEM (IDF's components/bootloader/subproject/main/ld/esp32p4/
 * bootloader.memory.ld.in records the map):
 *
 *   0x4ff296b8 - 0x4ff3afc0  ROM shared buffers -- LIVE during UART/USB/SPI
 *                            download mode, which is exactly how we get
 *                            loaded, so this is the one that matters
 *   0x4ff3afc0 - 0x4ff3fba4  CPU1 stack
 *   0x4ff3fba4 - 0x4ff40000  ROM .bss and .data
 *
 * tools/minimal_esp32p4.ld therefore keeps everything below 0x4ff28000.
 *
 * Register offsets below are confirmed against BOTH the TRM (§45, registers
 * 45.1 and 45.21) and IDF's soc/esp32p4/register/hw_ver1/soc/uart_reg.h,
 * per §3.2 of the plan -- this tree has gotten register layouts wrong before
 * by trusting prose, and phase 24 paid for it.
 */

#include <stdint.h>
#include <stdbool.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

/* DR_REG_HPPERIPH1_BASE (0x500C0000) + 0xA000. */
#define UART0_BASE          0x500CA000UL
#define UART_FIFO_REG       (UART0_BASE + 0x00)
#define UART_STATUS_REG     (UART0_BASE + 0x1C)

/* UART_STATUS_REG: RXFIFO_CNT is bits [7:0], TXFIFO_CNT is bits [23:16].
 *
 * Note for anyone checking: both the TRM and IDF's header describe
 * TXFIFO_CNT as "the number of valid data bytes in RX FIFO". That is a
 * copy-paste error in the vendor documentation, propagated into the
 * generated header; the field is the TX count. Recorded rather than
 * silently worked around, because the next person will hit it too. */
#define RXFIFO_CNT(status)  ((status) & 0xFFu)
#define TXFIFO_CNT(status)  (((status) >> 16) & 0xFFu)

/* The FIFO is 128 bytes deep. Leave a margin rather than filling it to the
 * brim: a full FIFO on this part discards, it does not block. */
#define TXFIFO_LIMIT        120u

static void uart_putc(char c) {
    while (TXFIFO_CNT(REG(UART_STATUS_REG)) >= TXFIFO_LIMIT) { }
    REG(UART_FIFO_REG) = (uint32_t)(uint8_t)c;
}

static bool uart_has_char(void) {
    return RXFIFO_CNT(REG(UART_STATUS_REG)) != 0u;
}

static char uart_getc(void) {
    while (!uart_has_char()) { }
    return (char)(REG(UART_FIFO_REG) & 0xFFu);
}

static void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

static void uart_puthex(uint32_t v) {
    static const char digits[] = "0123456789abcdef";
    uart_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        uart_putc(digits[(v >> shift) & 0xFu]);
}

/* Reads a CSR by name into a variable. */
#define READ_CSR(name) ({ uint32_t __v; __asm__ volatile ("csrr %0, " name : "=r"(__v)); __v; })

/* ---- GPIO -------------------------------------------------------------
 *
 * The milestone asks for a GPIO toggle. On this board that is not a matter
 * of finding an LED: **the ESP32-P4-NANO has no user LED.** The schematic's
 * only LED1 is a 5 V power indicator hardwired through R1 to VCC_5V, and
 * LED0/LED3/LEDMOD are pins of the IP101GRI Ethernet PHY. So the toggle is
 * made observable a better way -- by reading the pad back through the input
 * buffer, which proves drive *and* sense in one step and needs no scope.
 *
 * Pin choice, from ESP32-P4-NANO-schematic.pdf: GPIO20, which reaches
 * header P1 pin 13 and touches nothing else on the board. GPIO7/GPIO8 are
 * the I2C the factory demo fails on; GPIO34-38 are strapping pins
 * (datasheet Table 3-1) and GPIO36 carries a 10 kOhm pull-up (R41); GPIO54
 * resets the C6; GPIO14-19 are its SDIO. GPIO20-23 are the clean ones, all
 * four on P1, connected to the P4 and the header and nothing besides.
 *
 * Every offset below is from the TRM, chapter 10 "GPIO Matrix and IO MUX",
 * not from inference -- §3.2 of the plan, and phase 24's 0x88888888 bug,
 * are why. GPIO Matrix at 0x500E_0000 and IO MUX at 0x500E_1000 are from
 * the TRM's own peripheral address table. */
#define GPIO_BASE               0x500E0000UL
#define GPIO_OUT_W1TS_REG       (GPIO_BASE + 0x0008)   /* Register 10.2 */
#define GPIO_OUT_W1TC_REG       (GPIO_BASE + 0x000C)   /* Register 10.3 */
#define GPIO_ENABLE_W1TS_REG    (GPIO_BASE + 0x0024)   /* Register 10.8 */
#define GPIO_ENABLE_W1TC_REG    (GPIO_BASE + 0x0028)   /* Register 10.9 */
#define GPIO_IN_REG             (GPIO_BASE + 0x003C)   /* Register 10.13 */
#define GPIO_FUNC_OUT_SEL_CFG(n) (GPIO_BASE + 0x0558 + 4u * (n))  /* Reg 10.30 */

#define IOMUX_BASE              0x500E1000UL
#define IOMUX_GPIO_REG(n)       (IOMUX_BASE + 0x0004 + 4u * (n))  /* Reg 10.43 */

/* IO_MUX_GPIOn_REG fields (Register 10.43): MCU_SEL [14:12], FUN_DRV
 * [11:10] (reset 0x2, ~20 mA), FUN_IE [9]. Function 1 is the GPIO function
 * "for all pins" per the TRM's programming procedure, and MCU_SEL resets to
 * 0 -- so selecting it is required, not a formality. */
#define IOMUX_MCU_SEL_SHIFT     12
#define IOMUX_MCU_SEL_MASK      (7u << IOMUX_MCU_SEL_SHIFT)
#define IOMUX_FUN_GPIO          (1u << IOMUX_MCU_SEL_SHIFT)
#define IOMUX_FUN_IE            (1u << 9)
#define IOMUX_FUN_PU            (1u << 8)
#define IOMUX_FUN_PD            (1u << 7)

/* GPIO_FUNCn_OUT_SEL_CFG_REG (Register 10.30): OUT_SEL [8:0] picks which of
 * 256 peripheral signals drives the pin, and 256 is the special index
 * meaning "drive from GPIO_OUT_REG" (IDF's SIG_GPIO_OUT_IDX). It is also
 * the reset value, 0x100 -- written anyway rather than inherited, on the
 * same principle that makes E2 configure the UART instead of living off the
 * ROM's leftovers. OE_SEL [10] makes the output enable ours rather than a
 * peripheral's. */
#define GPIO_SIG_OUT_IDX        256u
#define GPIO_FUNC_OE_SEL        (1u << 10)

#define GPIO_TOGGLE_PIN         20u   /* header P1 pin 13 */

static void gpio_out_init(unsigned pin) {
    uint32_t cfg = REG(IOMUX_GPIO_REG(pin));
    cfg &= ~IOMUX_MCU_SEL_MASK;
    /* FUN_IE stays on deliberately: with the input buffer enabled the pad
     * can be read back while we are driving it, which is what turns this
     * from "we wrote a register" into a measurement. */
    REG(IOMUX_GPIO_REG(pin)) = cfg | IOMUX_FUN_GPIO | IOMUX_FUN_IE;
    REG(GPIO_FUNC_OUT_SEL_CFG(pin)) = GPIO_SIG_OUT_IDX | GPIO_FUNC_OE_SEL;
    REG(GPIO_ENABLE_W1TS_REG) = 1u << pin;
}

static void gpio_write(unsigned pin, bool high) {
    REG(high ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG) = 1u << pin;
}

static bool gpio_read(unsigned pin) {
    return (REG(GPIO_IN_REG) >> pin) & 1u;
}

/* Release the pin and let a weak internal resistor decide its level.
 *
 * This exists to falsify the obvious objection to the drive test above: that
 * GPIO_IN might simply be echoing GPIO_OUT back to us, in which case
 * "drive 1 reads 1" proves only that a register remembers what was written
 * to it. With the output disabled and GPIO_OUT left high, a mirror would
 * still read 1 -- so if the pad instead follows a pull-down to 0 and a
 * pull-up to 1, GPIO_IN is reading the physical pin.
 *
 * It doubles as a check on the schematic: a pin that follows a weak internal
 * pull in *both* directions has nothing else driving it, which is what
 * "GPIO20 touches only the P4 and header P1" predicts. */
static bool gpio_float_reads(unsigned pin, bool pull_up) {
    uint32_t cfg = REG(IOMUX_GPIO_REG(pin)) & ~(IOMUX_FUN_PU | IOMUX_FUN_PD);
    REG(IOMUX_GPIO_REG(pin)) = cfg | (pull_up ? IOMUX_FUN_PU : IOMUX_FUN_PD);
    REG(GPIO_ENABLE_W1TC_REG) = 1u << pin;
    /* The pad is driven through a ~45 kOhm resistor into whatever
     * capacitance the header and a probe present; give it time to settle
     * rather than sampling the edge. */
    for (volatile unsigned i = 0; i < 20000u; i++) { }
    bool v = gpio_read(pin);
    REG(IOMUX_GPIO_REG(pin)) = cfg;              /* pulls off again */
    REG(GPIO_ENABLE_W1TS_REG) = 1u << pin;       /* back to driving */
    return v;
}

void minimal_main(void) {
    /* Drain whatever the ROM's download session left in the RX FIFO.
     *
     * Found the hard way, 2026-09-05: the first run echoed correctly but
     * never printed its banner or heartbeat. The cause was leftover bytes
     * from the load protocol sitting in the RX FIFO -- the echo loop read
     * them, concluded it had been spoken to, and went quiet. The program was
     * working; the evidence that it was working had been consumed by the
     * mechanism that delivered it.
     *
     * **Bounded, and that bound is not paranoia.** An unbounded drain hung
     * the board outright, 2026-09-05: if UART0's baud is wrong the RX FIFO
     * refills with noise as fast as it is emptied, `uart_has_char()` never
     * returns false, and the program spins here forever having printed
     * nothing. That is indistinguishable, from the far end of a cable, from
     * "our code never ran" -- the single most expensive thing a bring-up
     * program can be ambiguous about. Drain what a load can plausibly have
     * left, then give up and start talking. */
    for (unsigned i = 0; i < 256u && uart_has_char(); i++)
        (void)uart_getc();

    uart_puts("\n[P4_MINIMAL] ESP32-P4 alive, running from L2MEM.\n");

    /* Print what the hardware says about itself. Every one of these is a
     * claim E0 made from documents; this is the first chance to hear the
     * silicon agree or disagree, which is worth more than the echo test.
     *
     *   misa    -- expect bit 20 (U) set, bit 18 (S) clear: M+U, no S-mode.
     *              U-mode is the assumption phase 12's whole driver model
     *              rests on (plan §3.5).
     *   mtvec   -- expect low two bits = 0b11: CLIC mode 3, which the TRM
     *              says is the only available mode (plan §3.4). The factory
     *              firmware's crash dump showed 0x4ff00003, so this should
     *              agree.
     *   mhartid -- expect 0. */
    uart_puts("  misa    = "); uart_puthex(READ_CSR("misa"));    uart_puts("\n");
    uart_puts("  mtvec   = "); uart_puthex(READ_CSR("mtvec"));   uart_puts("\n");
    uart_puts("  mhartid = "); uart_puthex(READ_CSR("mhartid")); uart_puts("\n");
    uart_puts("  mstatus = "); uart_puthex(READ_CSR("mstatus")); uart_puts("\n");

    /* Drive GPIO20 and read the pad back. Both levels, because a stuck-high
     * pin passes a test that only ever checks for 1 -- the same trap as a
     * heartbeat that cannot fail. */
    gpio_out_init(GPIO_TOGGLE_PIN);
    gpio_write(GPIO_TOGGLE_PIN, true);
    bool hi = gpio_read(GPIO_TOGGLE_PIN);
    gpio_write(GPIO_TOGGLE_PIN, false);
    bool lo = gpio_read(GPIO_TOGGLE_PIN);
    uart_puts("  gpio20  = drive 1 reads ");
    uart_putc(hi ? '1' : '0');
    uart_puts(", drive 0 reads ");
    uart_putc(lo ? '1' : '0');
    uart_puts(hi && !lo ? "  PASS\n" : "  FAIL\n");

    /* ...and prove that was the pad, not GPIO_OUT reflected back. GPIO_OUT
     * is left high throughout; only the pulls change. */
    gpio_write(GPIO_TOGGLE_PIN, true);
    bool pd = gpio_float_reads(GPIO_TOGGLE_PIN, false);
    bool pu = gpio_float_reads(GPIO_TOGGLE_PIN, true);
    uart_puts("  gpio20  = released, pull-down reads ");
    uart_putc(pd ? '1' : '0');
    uart_puts(", pull-up reads ");
    uart_putc(pu ? '1' : '0');
    uart_puts(!pd && pu ? "  PASS (reading the pad, and the pin is free)\n"
                        : "  FAIL\n");

    uart_puts("[P4_MINIMAL] echo test -- type, and it comes back:\n");

    /* Until the first character arrives, say so again periodically.
     *
     * Not decoration. This program is delivered by `esptool load-ram`, which
     * owns the serial port right up to the instant it jumps to us -- so the
     * banner above is emitted into a port the host is still closing, and is
     * usually lost. Re-announcing means a terminal attached a second later
     * still learns the board is alive, which is the entire question E1
     * exists to answer.
     *
     * The counter is a spin, not a calibrated delay: nothing here depends on
     * how long it actually takes, only on it being long enough to read and
     * short enough to notice.
     *
     * **Unconditional, and it was not always.** The first version stopped
     * beating once any character arrived, on the theory that a heartbeat is
     * noise during interactive use. That made it useless: merely *opening*
     * the host serial port is enough to put a stray byte in the RX FIFO, so
     * the heartbeat switched itself off before anyone could see it, and the
     * board looked dead while it was in fact echoing perfectly. A bring-up
     * instrument that goes quiet when observed is not an instrument. */
    uint32_t spin = 0;
    uint32_t beats = 0;

    while (1) {
        if (uart_has_char()) {
            char c = uart_getc();
            if (c == '\r') uart_putc('\n');
            uart_putc(c);
            continue;
        }

        if (++spin >= 300000u) {
            spin = 0;
            /* The toggle the milestone asks for, on the heartbeat: GPIO20
             * carries a square wave at half the beat rate, so header P1 pin
             * 13 says the same thing the console does to anyone holding a
             * meter instead of a terminal. */
            ++beats;
            gpio_write(GPIO_TOGGLE_PIN, (beats & 1u) != 0u);
            uart_puts("[P4_MINIMAL] alive, beat ");
            uart_puthex(beats);
            uart_puts(" gpio20=");
            uart_putc(gpio_read(GPIO_TOGGLE_PIN) ? '1' : '0');
            uart_puts("\n");
        }
    }
}
