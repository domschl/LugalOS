#ifndef LUGALOS_ARCH_ESP32P4_INTR_H
#define LUGALOS_ARCH_ESP32P4_INTR_H

#include <stdint.h>

/* ESP32-P4 interrupt plumbing that a driver has to know about, E3,
 * plan/phase27_esp32p4_bringup.md.
 *
 * Everything else about this chip's interrupt controller lives inside
 * arch/riscv/common/trap.c beside the Hazard3 and PLIC arms, because that is
 * where Rule 0 puts "identifying which IRQ fired". Two things cannot stay
 * there, and both are here:
 *
 *   1. The interrupt *matrix*. The P4 has 128 peripheral interrupt sources
 *      and 32 CPU interrupt lines per core (TRM section 13.1), so a source
 *      does not reach the CPU at all until it has been routed to a line.
 *      That routing is a per-peripheral fact -- only the UART driver knows
 *      it is source 31 -- so the driver has to be able to ask for it.
 *
 *   2. The line allocation itself. A CLIC line is a shared resource: two
 *      drivers picking the same number would each get the other's
 *      interrupts, silently, and neither file would contain the evidence.
 *      So the assignment is written down once, here, where a collision is a
 *      visible edit rather than a coincidence.
 */

/* CLIC interrupt IDs. External interrupts occupy IDs 16..47 (TRM section
 * 2.9.2.6, footnote 6: "external interrupts are assigned to CLIC interrupt
 * IDs 16 and onwards"), and this kernel uses that ID as its devirq number
 * directly -- the number trap_handler() reads out of mcause is the number a
 * driver attached with, with no offset in between to get backwards.
 *
 * IDs 3 (software) and 7 (timer) are core-local and belong to E4. */
#define ESP32P4_CLIC_IRQ_MIN    16u
#define ESP32P4_CLIC_IRQ_MAX    47u

/* The allocation table. One entry so far. */
#define ESP32P4_CLIC_IRQ_UART0  16u

/* Routes peripheral interrupt source `src` (an interrupt-matrix source
 * number from TRM Table 13.4-1) to CLIC interrupt `clic_id` on the calling
 * core, and returns 0 on success.
 *
 * `clic_id` must be in [ESP32P4_CLIC_IRQ_MIN, ESP32P4_CLIC_IRQ_MAX]: the
 * mapping register takes the CLIC ID, not a 0-based external index, and the
 * TRM is explicit that values 0..15 mean "disabled" rather than "line 0".
 * Passing one of those would therefore not misroute the interrupt, it would
 * quietly switch it off -- so the range is checked and refused rather than
 * written through.
 *
 * This does not enable anything. arch_irq_enable(clic_id) is still what
 * unmasks the line, and the peripheral's own interrupt-enable register is
 * still the driver's business. */
int esp32p4_intmtx_route(uint32_t src, uint32_t clic_id);

#endif /* LUGALOS_ARCH_ESP32P4_INTR_H */
