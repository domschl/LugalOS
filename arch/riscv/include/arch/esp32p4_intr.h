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

/* The allocation table. Every line here is a claim on a shared resource, so
 * adding one is a visible edit rather than a coincidence -- that is the whole
 * reason this table is in a header instead of in each driver.
 *
 * The matrix *source* numbers these are fed from are board facts and live in
 * the board file (CONFIG_EMAC_INTR_SRC and friends); the CLIC *line* is a
 * kernel allocation and lives here. The two are deliberately not the same
 * number and must not be conflated. */
#define ESP32P4_CLIC_IRQ_UART0  16u
/* Z0, plan/phase28_esp32p4_ethernet.md. The EMAC raises four matrix sources
 * (89 GMII_PHY, 90 LPI, 91 PMT, 92 ETH_MAC); only ETH_MAC carries frame and
 * DMA events, so only it is routed, and one line is enough. */
#define ESP32P4_CLIC_IRQ_EMAC   17u

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

/* Arm hardware watchpoint 0 on stores to the 4 bytes at addr. Phase 27 E7
 * debug aid; see arch/riscv/common/trap.c for the encoding and its source. */
/* Shrink the L2 cache to 128 KB, which is what makes the memory above
 * 0x4ff80000 real RAM. Must run before the heap is used; see the long comment
 * in arch/riscv/common/trap.c. */
void esp32p4_l2_cache_shrink(void);

void esp32p4_watch_store(uintptr_t addr);
void esp32p4_watch_store_quiet(uintptr_t addr);
void esp32p4_watch_clear(void);
void esp32p4_watch_reenable(void);

/* Unmasks the core-local timer interrupt, CLIC ID 7 (E4).
 *
 * Separate from arch_irq_enable() on purpose, and that function refuses
 * IDs below 16 rather than passing them through: 3 and 7 are the software
 * and timer interrupts, they arrive from the CLINT rather than from the
 * interrupt matrix, and a device driver reaching them by arithmetic on its
 * own line number is a bug that would present as a tick nobody armed.
 *
 * The `mie` CSR does not exist on this core (TRM section 2.9.2.1), so this
 * is what `set_csr(mie, 1 << 7)` means here -- and unlike that write, it
 * either works or is visibly absent, rather than silently doing nothing. */
void esp32p4_clic_timer_enable(void);

#endif /* LUGALOS_ARCH_ESP32P4_INTR_H */
