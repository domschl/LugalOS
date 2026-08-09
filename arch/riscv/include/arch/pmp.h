#ifndef LUGALOS_ARCH_PMP_H
#define LUGALOS_ARCH_PMP_H

#include <stdbool.h>

/* RISC-V Physical Memory Protection discovery (B3 prep, D2 in
 * plan/phase5_distributed_design.md).
 *
 * D2 resolved to "PMP early, NOMMU leads": RV32/RP2350 gets U-mode plus PMP
 * enforcement in B3, ahead of Sv39 in B5. The region count bounds how many
 * isolated servers a NOMMU node can host, and the RISC-V spec allows 0, 16
 * or 64 entries -- so it must be measured on the actual core rather than
 * assumed. See arch/riscv/common/pmp_probe.c for how, and tests/hw/ for the
 * hardware-in-the-loop test that records the answer for real silicon. */

typedef struct {
    bool mode_m;           /* false when this build runs in S-mode (no PMP access) */
    int  num_entries;      /* CONFIGURABLE (writable) entries -- B3's real budget */
    int  num_hardwired;    /* read-only entries fixed by the silicon (RP2350: 3) */
    int  granularity_log2; /* log2(bytes); 2 means 4-byte granularity */
    bool any_locked;       /* an entry was already locked at boot */
} pmp_info_t;

void pmp_probe(pmp_info_t *out);

/* Prints a human- and test-readable summary. */
void pmp_report(void);

#endif /* LUGALOS_ARCH_PMP_H */
