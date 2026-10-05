#ifndef LUGALOS_KERNEL_RADIO_INTR_H
#define LUGALOS_KERNEL_RADIO_INTR_H

#include <stdint.h>

/* Kernel side of the radio's interrupts, reached from KOBJ_OP_INTR_* and KOBJ_OP_ISR_* (kernel/kobj_sys.c).
 * C6 only: on any other build these do not exist and the syscalls answer KO_FAIL. */
long radio_intr_set(uint32_t source, uint32_t name);
long radio_intr_clear(uint32_t source, uint32_t name);
long radio_isr_set(uint32_t name, uintptr_t fn, uintptr_t arg);
long radio_ints_on(uint32_t mask);
long radio_ints_off(uint32_t mask);
long radio_isr_wait(uintptr_t out);
long radio_isr_done(uint32_t slot);
void radio_intr_reset(void);
void radio_intr_report(void);

#endif
