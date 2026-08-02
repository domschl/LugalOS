#ifndef LUGALOS_ARCH_TRAP_H
#define LUGALOS_ARCH_TRAP_H

#include <stdint.h>

/* Register frame layout saved on stack during traps */
typedef struct trap_frame {
    uintptr_t ra;
    uintptr_t sp;
    uintptr_t gp;
    uintptr_t tp;
    uintptr_t t0, t1, t2;
    uintptr_t s0, s1;
    uintptr_t a0, a1, a2, a3, a4, a5, a6, a7;
    uintptr_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uintptr_t t3, t4, t5, t6;
    uintptr_t epc;
    uintptr_t status;
    uintptr_t cause;
    uintptr_t tval;
} trap_frame_t;

void trap_init(void);
void trap_handler(trap_frame_t *frame);

#endif /* LUGALOS_ARCH_TRAP_H */
