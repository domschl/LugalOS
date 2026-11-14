#ifndef LUGALOS_MPHALPORT_H
#define LUGALOS_MPHALPORT_H

#include <stdint.h>
#include <stddef.h>

#define mp_hal_pin_obj_t int

static inline void mp_hal_set_interrupt_char(int c) {
    (void)c;
}

static inline uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags) {
    (void)poll_flags;
    return 0;
}

#endif /* LUGALOS_MPHALPORT_H */
