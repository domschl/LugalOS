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
    /* NAPOT requires a power-of-two size, self-aligned. The floor is 32 bytes
     * -- RP2350's granule (datasheet §3.8.3.1); QEMU's is 8, and taking the
     * stricter of the two keeps one rule for both builds rather than a region
     * that is legal on one target and silently widened on the other.
     * Rejected rather than rounded: a region quietly widened to satisfy
     * alignment is a hole in the isolation it was asked to provide. */
    if (size < 32 || (size & (size - 1)) != 0) return -1;
    if (base & (size - 1)) return -1;

    d->regions[d->count].base  = base;
    d->regions[d->count].size  = size;
    d->regions[d->count].perms = perms;
    d->count++;
    return 0;
}

/* Architecture-independent, and deliberately so: this is the check the
 * syscall boundary uses to decide whether a user-supplied pointer is one the
 * calling task may actually touch. It reads the region list, not the
 * hardware, so it behaves identically on both builds -- including on the
 * S-mode target where nothing is enforced yet, so that copy-in/copy-out is
 * exercised by the whole test suite rather than only where PMP exists.
 *
 * A NULL domain means an unrestricted (kernel) task, for which every address
 * is permitted -- kernel code calling a syscall on its own behalf. */
bool mem_domain_permits(const mem_domain_t *d, uintptr_t base, uintptr_t len,
                        uint8_t perms) {
    if (!d) return true;
    if (len == 0) return true;
    if (base + len < base) return false; /* wrap-around */

    /* The range must fall entirely inside ONE region. Stitching a range
     * together from several adjacent regions would be wrong: PMP resolves
     * each access against the lowest-numbered matching region, so a range
     * spanning two of them is not uniformly governed by either. */
    for (int i = 0; i < d->count; i++) {
        uintptr_t rb = d->regions[i].base;
        uintptr_t re = rb + d->regions[i].size;
        if (base >= rb && base + len <= re) {
            return (d->regions[i].perms & perms) == perms;
        }
    }
    return false;
}

#if defined(CONFIG_MODE_M)

/* Permission bit order differs by silicon -- this is erratum RP2350-E6, not a
 * choice. The RP2350 datasheet (§3.8.3.2) states:
 *
 *   "Due to RP2350-E6 the field order in the PMP configuration fields is
 *    R, W, X (MSB-first) rather than the standard X, W, R."
 *
 * So on RP2350 bit 0 is X and bit 2 is R -- the reverse of every other
 * RISC-V core, including QEMU's. Getting this wrong is not a subtle
 * degradation: a region asked for as R|W becomes X|W, which is why a U-mode
 * task's stores to its own stack succeeded while its loads from the same
 * stack faulted. That symptom cost a long detour through NAPOT encoding and
 * an abandoned attempt to use TOR before the datasheet named the cause. */
#if defined(CONFIG_BOARD_RP2350)
#define PMP_X       (1u << 0)
#define PMP_W       (1u << 1)
#define PMP_R       (1u << 2)
#else
#define PMP_R       (1u << 0)
#define PMP_W       (1u << 1)
#define PMP_X       (1u << 2)
#endif
#define PMP_A_NAPOT (3u << 3)

/* Granule: the smallest NAPOT region the hardware decodes. RP2350 hardwires
 * the low two pmpaddr bits to ones when a region is enabled, giving 8 << 2 =
 * 32 bytes (datasheet §3.8.3.1). Those bits are therefore not meaningful to
 * compare on readback. */
#if defined(CONFIG_BOARD_RP2350)
#define PMP_GRANULE       32u
#define PMP_ADDR_FIXED_LOW 3u   /* bits [1:0] hardwired */
#else
#define PMP_GRANULE       8u
#define PMP_ADDR_FIXED_LOW 0u
#endif

/* NAPOT only. The RP2350 datasheet (§3.8.3.1) is explicit: "The RP2350
 * configuration of Hazard3 supports only the OFF and NAPOT values for the
 * PMPCFG A fields." An earlier attempt here used TOR and every region came
 * back inactive -- writing A=TOR reads back as A=OFF, because the field is
 * WARL and the unsupported mode is simply dropped.
 *
 * Size is encoded as trailing one-bits: a value with k trailing ones matches
 * 8 << k bytes, naturally aligned. */

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
        /* Compare only the bits the hardware lets software own. RP2350
         * hardwires the low two to ones when the region is enabled (granule
         * 32), so they carry no information and differ by design. */
        uintptr_t got = pmpaddr_read(i);
        if ((got | PMP_ADDR_FIXED_LOW) != (want | PMP_ADDR_FIXED_LOW)) {
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
