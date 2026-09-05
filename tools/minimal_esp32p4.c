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
 *   2. Echoes received characters back
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
            uart_puts("[P4_MINIMAL] alive, beat ");
            uart_puthex(++beats);
            uart_puts("\n");
        }
    }
}
