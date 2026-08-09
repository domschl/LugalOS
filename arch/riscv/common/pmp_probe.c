#include "arch/pmp.h"
#include "arch/trap.h"
#include "kernel/printk.h"

/* Discovers what PMP this silicon actually implements (B3 prep,
 * plan/phase5_distributed_design.md §5.4 / D2).
 *
 * D2 chose "PMP early, NOMMU leads": RV32/RP2350 gets U-mode plus PMP
 * enforcement in B3, before Sv39 in B5. How many isolated servers a NOMMU
 * node can host is bounded by the region count, and the RISC-V privileged
 * spec permits 0, 16, or 64 entries, so it is a property of the specific
 * core rather than something the plan can assume. Hazard3's count in
 * particular has to be read off real silicon.
 *
 * Two things are measured:
 *
 *   Entry count -- write all-ones to each pmpaddrN and read it back. An
 *   unimplemented entry reads as zero (WARL). Some cores also raise an
 *   illegal-instruction trap on the CSR access rather than reading zero,
 *   which is why every access here runs under arch_probe_begin() (see
 *   arch/trap.h): a trap is a valid answer, not a crash.
 *
 *   Granularity -- with pmp0cfg zeroed (A=OFF, unlocked), write all-ones to
 *   pmpaddr0 and read it back; the index of the least-significant set bit is
 *   G, and the granularity is 2^(G+2) bytes. This is the algorithm the
 *   privileged spec itself prescribes. G > 0 means regions cannot be aligned
 *   more finely than that, which directly constrains how tightly B3 can fence
 *   a task's stack.
 *
 * Nothing is left enforced: every register touched is restored, and pmpcfg0
 * is written back last so no region can be active while addresses change
 * underneath it.
 */

#if defined(CONFIG_MODE_M)

/* Reads, all-ones-writes, re-reads and restores one pmpaddr CSR. Sets
 * `*faulted` if the access is not legal on this core. */
#define PROBE_ONE(n)                                                        \
    do {                                                                    \
        uintptr_t old = 0, back = 0;                                        \
        arch_probe_begin();                                                 \
        __asm__ __volatile__("csrr %0, pmpaddr" #n : "=r"(old));            \
        if (arch_probe_faulted()) { faulted = true; break; }                \
        arch_probe_begin();                                                 \
        __asm__ __volatile__("csrw pmpaddr" #n ", %0" :: "r"(all_ones));    \
        __asm__ __volatile__("csrr %0, pmpaddr" #n : "=r"(back));           \
        __asm__ __volatile__("csrw pmpaddr" #n ", %0" :: "r"(old));         \
        if (arch_probe_faulted()) { faulted = true; break; }                \
        readback = back;                                                    \
    } while (0)

void pmp_probe(pmp_info_t *out) {
    if (!out) return;
    out->mode_m = true;
    out->num_entries = 0;
    out->granularity_log2 = 0;
    out->any_locked = false;

    const uintptr_t all_ones = (uintptr_t)-1;
    uintptr_t readback = 0;
    bool faulted = false;

    /* pmpcfg0 must be zeroed before touching pmpaddr0: a locked or active
     * entry would make the address write a no-op (and, if active, could
     * change what memory is reachable while we do it). Saved and restored. */
    uintptr_t cfg0_saved = 0;
    arch_probe_begin();
    __asm__ __volatile__("csrr %0, pmpcfg0" : "=r"(cfg0_saved));
    if (arch_probe_faulted()) {
        out->mode_m = true;
        out->num_entries = 0;
        printk("[PMP] pmpcfg0 not accessible: no PMP on this core\n");
        return;
    }
    /* Bit 7 of each 8-bit config byte is L (locked). A locked entry cannot be
     * modified until reset, which would make the counts below meaningless. */
    for (unsigned i = 0; i < sizeof(uintptr_t); i++) {
        if ((cfg0_saved >> (i * 8)) & 0x80u) out->any_locked = true;
    }
    __asm__ __volatile__("csrw pmpcfg0, zero");

    /* --- granularity, from pmpaddr0 with A=OFF --- */
    readback = 0; faulted = false;
    PROBE_ONE(0);
    if (faulted || readback == 0) {
        __asm__ __volatile__("csrw pmpcfg0, %0" :: "r"(cfg0_saved));
        printk("[PMP] No PMP entries implemented\n");
        return;
    }
    int g = 0;
    while (g < (int)(sizeof(uintptr_t) * 8) && !((readback >> g) & 1u)) g++;
    out->granularity_log2 = g + 2;

    /* --- entry count --- */
#define PROBE_STEP(n) do { readback = 0; faulted = false; PROBE_ONE(n); \
                           if (!faulted && readback != 0) counted++; } while (0);
    int counted = 0;
    /* Enumerated rather than macro-generated: CSR names must be assembler
     * literals, so there is no loop to write, and token-pasting a counter
     * would be harder to read than the list it replaces. */
    PROBE_STEP(0)  PROBE_STEP(1)  PROBE_STEP(2)  PROBE_STEP(3)  PROBE_STEP(4)
    PROBE_STEP(5)  PROBE_STEP(6)  PROBE_STEP(7)  PROBE_STEP(8)  PROBE_STEP(9)
    PROBE_STEP(10) PROBE_STEP(11) PROBE_STEP(12) PROBE_STEP(13) PROBE_STEP(14)
    PROBE_STEP(15) PROBE_STEP(16) PROBE_STEP(17) PROBE_STEP(18) PROBE_STEP(19)
    PROBE_STEP(20) PROBE_STEP(21) PROBE_STEP(22) PROBE_STEP(23) PROBE_STEP(24)
    PROBE_STEP(25) PROBE_STEP(26) PROBE_STEP(27) PROBE_STEP(28) PROBE_STEP(29)
    PROBE_STEP(30) PROBE_STEP(31) PROBE_STEP(32) PROBE_STEP(33) PROBE_STEP(34)
    PROBE_STEP(35) PROBE_STEP(36) PROBE_STEP(37) PROBE_STEP(38) PROBE_STEP(39)
    PROBE_STEP(40) PROBE_STEP(41) PROBE_STEP(42) PROBE_STEP(43) PROBE_STEP(44)
    PROBE_STEP(45) PROBE_STEP(46) PROBE_STEP(47) PROBE_STEP(48) PROBE_STEP(49)
    PROBE_STEP(50) PROBE_STEP(51) PROBE_STEP(52) PROBE_STEP(53) PROBE_STEP(54)
    PROBE_STEP(55) PROBE_STEP(56) PROBE_STEP(57) PROBE_STEP(58) PROBE_STEP(59)
    PROBE_STEP(60) PROBE_STEP(61) PROBE_STEP(62) PROBE_STEP(63)
#undef PROBE_STEP
    out->num_entries = counted;

    __asm__ __volatile__("csrw pmpcfg0, %0" :: "r"(cfg0_saved));
}

#else /* !CONFIG_MODE_M */

void pmp_probe(pmp_info_t *out) {
    if (!out) return;
    /* PMP CSRs are M-mode only. This build enters S-mode in entry.S, so
     * reading them here would trap. Not a limitation worth working around:
     * D2 puts PMP enforcement on the M-mode (RV32/RP2350) targets, and the
     * S-mode target gets Sv39 in B5 instead. */
    out->mode_m = false;
    out->num_entries = 0;
    out->granularity_log2 = 0;
    out->any_locked = false;
}

#endif

void pmp_report(void) {
    pmp_info_t info;
    pmp_probe(&info);

    if (!info.mode_m) {
        printk("PMP: unavailable -- this build runs in S-mode; PMP CSRs are M-mode only.\n");
        printk("     (D2 puts PMP enforcement on the M-mode RV32/RP2350 targets; this\n");
        printk("      target gets Sv39 paging in B5 instead.)\n");
        return;
    }

    printk("PMP: entries=%d granularity=%lu bytes (G=%d) locked_at_boot=%s\n",
           info.num_entries,
           (unsigned long)(1UL << info.granularity_log2),
           info.granularity_log2 - 2,
           info.any_locked ? "yes" : "no");

    if (info.num_entries == 0) {
        printk("     No usable PMP: B3 would have no enforcement mechanism on this core.\n");
    } else {
        /* B3 needs, at minimum, one region per protected task plus one
         * covering the kernel. Reporting the implied ceiling here is the
         * whole point of running this on real silicon. */
        printk("     Usable isolated regions for B3: %d (1 reserved for the kernel)\n",
               info.num_entries - 1);
    }
}
