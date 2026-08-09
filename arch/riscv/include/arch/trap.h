#ifndef LUGALOS_ARCH_TRAP_H
#define LUGALOS_ARCH_TRAP_H

#include <stdint.h>
#include <stdbool.h>

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

/* --- Fault-tolerant hardware probing (B3 prep) ---
 *
 * Discovering what a chip actually implements sometimes means executing an
 * instruction that may not be legal on it. Reading an unimplemented PMP CSR
 * is the motivating case: the RISC-V privileged spec says unimplemented PMP
 * entries read as zero, but real cores differ on whether the *CSR* exists at
 * all, and an illegal-instruction trap would otherwise halt the system in
 * trap_handler()'s fatal path.
 *
 * Between begin() and end(), an illegal-instruction exception is swallowed:
 * the trap handler records it and skips the faulting instruction instead of
 * halting. end() reports whether one occurred and re-arms for the next probe.
 *
 * Only illegal-instruction (cause 2) is caught. Anything else still takes the
 * fatal path -- a probe should never mask a genuine bug such as a bad load.
 * Assumes the probed instruction is 4 bytes, which every CSR instruction is
 * (there is no compressed encoding for them). */
void arch_probe_begin(void);
bool arch_probe_faulted(void);

#endif /* LUGALOS_ARCH_TRAP_H */
