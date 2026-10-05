#ifndef LUGALOS_ARCH_ESP32C6_INTR_H
#define LUGALOS_ARCH_ESP32C6_INTR_H

#include <stdint.h>

/* ESP32-C6 interrupt plumbing a driver has to know about (45.4.3,
 * plan/phase45_esp32c6.md).
 *
 * The C6 is neither the P4's CLIC nor the QEMU PLIC. A peripheral raises one of
 * ~80 *sources*; the **interrupt matrix** (INTMTX, 0x60010000) routes each to one
 * of 32 *CPU interrupt lines*, and a small machine-level controller ("PLIC_MX",
 * 0x20001000 -- Espressif's own, not the standard PLIC) enables lines, sets
 * level/edge, and gives each a priority against one threshold. mtvec is forced to
 * *vectored* mode: interrupt n enters at BASE + 4n, and mcause is simply n
 * (arch/riscv/common/entry.S has the table). There is no `mie` to set and no
 * claim register.
 *
 * Both halves of the allocation are written down here, in one place, for the
 * reason the P4's header gives: a line is a shared resource, two drivers picking
 * the same one would each be handed the other's interrupts silently, and a table
 * in a header makes a collision a visible edit.
 *
 * Lines 1, 3, 4, 6 and 7 are not available (IDF components/esp_hw_support/port/
 * esp32c6/esp_cpu_intr.c): 1 is the Wi-Fi blob's by convention of the hardware
 * bring-up (the blob asks for its own routing through the OS table, plan §4.5, so
 * it must be free), 3/4/7 are bound to the core-local CLINT, 6 is permanently
 * disabled. Line 0 is the synchronous-exception slot of the vector table.
 *
 * **Line 8 is refused too, and that is a measurement, not a document.** The system
 * timer's alarm was routed to line 8 with everything else in order -- matrix map
 * read back, controller enable/priority/threshold set, mie bit 8 set, mstatus.MIE
 * on, the source's raw and status bits both 1 -- and never reached the core
 * (ticks=0, mip=0). The same source on line 10 ticks at exactly 100 Hz, and a
 * different source on line 9 was delivered. Nothing in the TRM says why; mie bit 8
 * is where the standard puts the user-external enable, which is the likeliest
 * reading. It costs one line out of 31 to leave it alone (45.4.3). */

#define ESP32C6_IRQ_MIN   1u
#define ESP32C6_IRQ_MAX   31u
#define ESP32C6_IRQ_RESERVED_MASK  ((1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 7) | (1u << 8))

/* The allocation. */
#define ESP32C6_IRQ_TICK     10u    /* the preemption tick: system timer comparator 0 */
#define ESP32C6_IRQ_USBJTAG  11u    /* the console: USB-Serial/JTAG */

/* Interrupt-matrix source numbers (soc/interrupts.h, confirmed against the
 * INTMTX_CORE0_*_MAP_REG offsets in interrupt_matrix_reg.h: register / 4). */
#define ESP32C6_SRC_USB_SERIAL_JTAG        48u
#define ESP32C6_SRC_SYSTIMER_TARGET0       57u

/* Routes peripheral `src` to CPU interrupt line `line`. Refuses a reserved or
 * out-of-range line (writing 0 would not misroute, it would disconnect). Does not
 * enable anything: arch_irq_enable(line) unmasks it, and the peripheral's own
 * enable register is the driver's. Returns 0 on success. */
int esp32c6_intmtx_route(uint32_t src, uint32_t line);

/* Masks and unmasks a line at the controller without touching its priority or its
 * mie bit: what a level-triggered line's handler thread needs between "it fired"
 * and "it has been serviced" (kernel/radio_intr.c). Callable from the trap handler. */
void esp32c6_irq_mask(uint32_t line);
void esp32c6_irq_unmask(uint32_t line);

#endif
