/* The libgcc pieces this tree cannot link against on the K210, replaced
 * here rather than repaired there (65.3).
 *
 * The symptom on the first k210 link:
 *
 *   libgcc.a(_ctzsi2.o): in function `__ctzdi2':
 *   relocation truncated to fit: R_RISCV_HI20 against symbol `__clz_tab'
 *
 * Cause, confirmed by isolating the link: the Arch riscv64-elf-gcc 16
 * multilib builds its libgcc with the small (medlow) code model, so
 * __ctzdi2 reaches __clz_tab through an unrelaxable absolute `la` pair,
 * and an absolute R_RISCV_HI20 cannot denote any address at or above
 * 0x8000_0000 in RV64 -- exactly where every RV64 kernel, this one
 * included, runs. The RV32 targets never see this (there HI20 covers the
 * whole address space); on RV64 it is a property of the toolchain, not of
 * this code, and the fix upstream would be a libgcc rebuilt medany.
 *
 * Until then: define the two table users ourselves, table-free. rv64imac
 * has no Zbb (the 65.1 probe's misa says so), so a loop is what the
 * hardware gives everyone anyway; __ctzdi2's own callers (kernel/
 * mem_domain.c bitmap scans) run at domain create/destroy, not hot paths.
 * Defining these in a regular object keeps the archive members -- and the
 * offending table reference -- out of the image entirely (--gc-sections
 * never even sees them).
 *
 * If a future link pulls in another medlow-table libgcc member (the same
 * failure shape: HI20 against a .rodata table), replace it here the same
 * way and say which one. */

#include <stdint.h>

int __ctzdi2(uint64_t x) {
    if (x == 0) return 64;
    int n = 0;
    while (!(x & 1)) { x >>= 1; n++; }
    return n;
}

int __clzdi2(uint64_t x) {
    if (x == 0) return 64;
    int n = 0;
    while (!(x & (1ULL << 63))) { x <<= 1; n++; }
    return n;
}

int __ffsdi2(uint64_t x) { return x ? __ctzdi2(x) + 1 : 0; }
