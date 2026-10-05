/*
 * Standalone Minimal Hardware Test for ESP32-C6 (Waveshare ESP32-C6-Zero)
 *
 * 45.1, plan/phase45_esp32c6.md. The counterpart of tools/minimal_esp32p4.c,
 * for the same reason: the smallest thing that proves the board is alive, so
 * every later failure has a known-good to be compared against.
 *
 * Actions:
 *   1. Banner on the USB-Serial/JTAG console (the C6-Zero's USB-C port)
 *   2. misa / mtvec / mhartid / mstatus straight off the silicon
 *   3. The CPU clock, measured: the cycle counter against the 16 MHz system timer
 *   4. A heartbeat, forever
 *
 * ## Console
 *
 * The C6-Zero has no UART bridge: its USB-C connector goes straight to the
 * chip's USB-Serial/JTAG peripheral (VID:PID 303a:1001), which is also what
 * the ROM prints on and what esptool loads over. By the time we run it is
 * enumerated and attached, so writing to its EP1 FIFO is sufficient.
 *
 * **Every wait here is bounded.** With no host reading the port the FIFO never
 * drains; an unbounded wait would hang the program silently, and "our code
 * never ran" and "nobody is listening" would be indistinguishable from the far
 * end of a cable -- the same trap tools/minimal_esp32p4.c documents for its
 * RX drain. On timeout a byte is dropped, and the FIFO is flushed once the host
 * returns.
 *
 * Register offsets: IDF components/soc/esp32c6/register/soc/
 * {usb_serial_jtag_reg.h, systimer_reg.h}, read 2026-10-05. Not yet checked
 * against the TRM (that is 45.4's job, where more of them matter).
 *
 * ## Where this runs
 *
 * HP SRAM, via `esptool load-ram`; flash is not written. A reset restores
 * whatever is in flash. tools/c6run.py does the loading.
 */

#include "c6_standalone.h"

static void report(void) {
    puts_("\n[C6_MINIMAL] ESP32-C6 alive, running from HP SRAM.\n");

    puts_("  misa    = "); puthex(READ_CSR("misa"));    puts_("\n");
    puts_("  mtvec   = "); puthex(READ_CSR("mtvec"));   puts_("\n");
    puts_("  mhartid = "); puthex(READ_CSR("mhartid")); puts_("\n");
    puts_("  mstatus = "); puthex(READ_CSR("mstatus")); puts_("\n");

    /* CPU clock: count the cycle counter across a fixed 100 ms of systimer
     * time, assuming the documented 16 MHz tick (XTAL/2.5). If the tick
     * assumption is wrong this reports a wrong MHz, which is a finding about
     * the tick and gets recorded as one. Integer division truncates: 159 means
     * "just under 160". */
    uint32_t t0 = systimer_now(), c0 = cycles_now();
    while ((uint32_t)(systimer_now() - t0) < 1600000u) { }
    uint32_t cycles = cycles_now() - c0;
    puts_("  cpu     = "); putdec(cycles / 100000u); puts_(" MHz (cycle counter over 100 ms of systimer @16 MHz)\n");
}

void minimal_main(void) {
    /* The ROM leaves the system timer's clock on in download mode, but
     * "leaves" is exactly the kind of inheritance E2 on the P4 refused; turn
     * it on explicitly. */
    REG(SYSTIMER_CONF_REG) |= SYSTIMER_CLK_EN | SYSTIMER_UNIT0_WORK_EN;
    cycles_start();

    /* This program is delivered by `esptool load-ram`, which owns the port
     * until the instant it jumps to us, so the first report is usually lost.
     * Repeat it: every 5th beat, so a monitor attached a few seconds later
     * still sees the CSRs. (Same device as tools/minimal_esp32p4.c's
     * unconditional heartbeat.) */
    uint32_t beats = 0;
    report();
    while (1) {
        uint32_t t = systimer_now();
        while ((uint32_t)(systimer_now() - t) < 16000000u) { }   /* ~1 s */
        puts_("[C6_MINIMAL] alive, beat "); puthex(++beats); puts_("\n");
        if (beats % 5u == 0) report();
    }
}
