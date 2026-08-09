#include "kernel/mem_domain.h"
#include "kernel/printk.h"
#include "arch/csr.h"
#include <string.h>

/* See kernel/include/kernel/mem_domain.h for the rationale. */

void mem_domain_init(mem_domain_t *d) {
    if (d) memset(d, 0, sizeof(*d));
}

int mem_domain_add(mem_domain_t *d, uintptr_t base, uintptr_t size, uint8_t perms) {
    if (!d || d->count >= MEM_DOMAIN_MAX_REGIONS) return -1;
    /* NAPOT requires a power-of-two size, self-aligned, at least the measured
     * 16-byte granularity. Rejected rather than rounded: a region quietly
     * widened to satisfy alignment is a hole in the isolation it was asked to
     * provide. */
    if (size < 16 || (size & (size - 1)) != 0) return -1;
    if (base & (size - 1)) return -1;

    d->regions[d->count].base  = base;
    d->regions[d->count].size  = size;
    d->regions[d->count].perms = perms;
    d->count++;
    return 0;
}

#if defined(CONFIG_MODE_M)

#define PMP_R       (1u << 0)
#define PMP_W       (1u << 1)
#define PMP_X       (1u << 2)
#define PMP_A_NAPOT (3u << 3)

/* NAPOT, with the configuration register written BEFORE the addresses.
 *
 * Both halves of that were established by measurement on Hazard3, and each
 * cost a confusing failure first:
 *
 * 1. TOR is not implemented. Writing A=TOR reads back as A=OFF -- the field is
 *    WARL and the mode is simply dropped, leaving every region inactive. The
 *    symptom was a U-mode task faulting on its first instruction fetch, with
 *    a config word that looked plausible until read back (0x05000300 where
 *    0x0d000b00 had been written). NAPOT is the only mode available here.
 *
 * 2. pmpaddr's low bits are masked at WRITE time according to the A mode in
 *    effect at that moment: with A=OFF they read back as ones, with A=NAPOT
 *    as zeros. Writing the address first (while the entry was still OFF) and
 *    the mode second corrupted the encoding -- a 1 KB region written as
 *    0x0800077f came back 0x0800077c, two trailing ones short, which NAPOT
 *    decodes as 8 bytes. That is why stores near the top of the user stack
 *    succeeded while a load a few bytes away faulted: only a sliver of the
 *    intended range was ever covered.
 *
 * Writing the config first makes the mode already correct when the masking is
 * applied, and the readback check turns any remaining disagreement into a
 * visible complaint instead of a silently wrong region.
 */

#define PMP_ENTRIES 8

static void pmpaddr_write(int idx, uintptr_t v) {
    switch (idx) {
        case 0: write_csr(pmpaddr0, v); break;
        case 1: write_csr(pmpaddr1, v); break;
        case 2: write_csr(pmpaddr2, v); break;
        case 3: write_csr(pmpaddr3, v); break;
        case 4: write_csr(pmpaddr4, v); break;
        case 5: write_csr(pmpaddr5, v); break;
        case 6: write_csr(pmpaddr6, v); break;
        case 7: write_csr(pmpaddr7, v); break;
        default: break;
    }
}

static uintptr_t pmpaddr_read(int idx) {
    switch (idx) {
        case 0: return read_csr(pmpaddr0);
        case 1: return read_csr(pmpaddr1);
        case 2: return read_csr(pmpaddr2);
        case 3: return read_csr(pmpaddr3);
        case 4: return read_csr(pmpaddr4);
        case 5: return read_csr(pmpaddr5);
        case 6: return read_csr(pmpaddr6);
        case 7: return read_csr(pmpaddr7);
        default: return 0;
    }
}

/* NAPOT: base>>2 with the low (log2(size) - 3) bits set to ones. */
static uintptr_t napot_encode(uintptr_t base, uintptr_t size) {
    return (base >> 2) | ((size >> 3) - 1);
}

int mem_domain_activate(const mem_domain_t *d) {
    int n = d ? d->count : 0;
    if (n > MEM_DOMAIN_MAX_REGIONS) n = MEM_DOMAIN_MAX_REGIONS;

    uint8_t cfg[PMP_ENTRIES];
    for (int i = 0; i < PMP_ENTRIES; i++) cfg[i] = 0;
    for (int i = 0; i < n; i++) {
        uint8_t byte = PMP_A_NAPOT;
        if (d->regions[i].perms & MEM_R) byte |= PMP_R;
        if (d->regions[i].perms & MEM_W) byte |= PMP_W;
        if (d->regions[i].perms & MEM_X) byte |= PMP_X;
        cfg[i] = byte;
    }

    /* Config first (see above), and every byte including the unused entries:
     * writing the whole word is what makes a switch to a smaller domain
     * actually shrink access rather than inherit the previous task's
     * leftovers -- the leak the hardware suite caught after `usertest`, made
     * structurally impossible. */
    uintptr_t w0 = 0, w1 = 0;
#if __riscv_xlen == 32
    for (int i = 0; i < 4; i++) w0 |= ((uintptr_t)cfg[i]) << (i * 8);
    for (int i = 0; i < 4; i++) w1 |= ((uintptr_t)cfg[4 + i]) << (i * 8);
    write_csr(pmpcfg0, w0);
    write_csr(pmpcfg1, w1);
#else
    for (int i = 0; i < 8; i++) w0 |= ((uintptr_t)cfg[i]) << (i * 8);
    write_csr(pmpcfg0, w0);
    (void)w1;
#endif

    int ok = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t want = napot_encode(d->regions[i].base, d->regions[i].size);
        pmpaddr_write(i, want);
        /* A region the hardware declined to represent exactly is not a
         * smaller region -- it is an unknown one. Report failure so the
         * caller can refuse to run a task under a restriction nobody has
         * verified, rather than granting something arbitrary. */
        uintptr_t got = pmpaddr_read(i);
        if (got != want) {
            printk("[MemDomain] pmpaddr%d: wrote %08lx, kept %08lx "
                   "(region %08lx+%08lx not representable on this core)\n",
                   i, (unsigned long)want, (unsigned long)got,
                   (unsigned long)d->regions[i].base,
                   (unsigned long)d->regions[i].size);
            ok = -1;
        }
    }
    return ok;
}

bool mem_domain_enforced(void) { return true; }

#else /* CONFIG_MODE_S */

int mem_domain_activate(const mem_domain_t *d) {
    (void)d;
    return 0;
    /* PMP CSRs are M-mode only and this build runs in S-mode; Sv39 is B5.
     * Deliberately a no-op rather than an error: the scheduler calls this on
     * every switch on both builds (Rule 0), and the difference belongs here,
     * in one place, not in the caller. mem_domain_enforced() reports the
     * truth so nothing claims isolation it does not have. */
}

bool mem_domain_enforced(void) { return false; }

#endif
