#ifndef LUGALOS_ARCH_CSR_H
#define LUGALOS_ARCH_CSR_H

#include <stdint.h>

#define read_csr(reg) ({ \
    uintptr_t __tmp; \
    __asm__ __volatile__ ("csrr %0, " #reg : "=r"(__tmp)); \
    __tmp; \
})

#define write_csr(reg, val) ({ \
    uintptr_t __val = (uintptr_t)(val); \
    __asm__ __volatile__ ("csrw " #reg ", %0" :: "rK"(__val)); \
})

#define set_csr(reg, bit) ({ \
    uintptr_t __val = (uintptr_t)(bit); \
    __asm__ __volatile__ ("csrs " #reg ", %0" :: "rK"(__val)); \
})

#define clear_csr(reg, bit) ({ \
    uintptr_t __val = (uintptr_t)(bit); \
    __asm__ __volatile__ ("csrc " #reg ", %0" :: "rK"(__val)); \
})

/* Common CSR register definitions depending on M-mode vs S-mode */
#if defined(CONFIG_MODE_S)
  #define READ_STATUS()        read_csr(sstatus)
  #define WRITE_STATUS(v)      write_csr(sstatus, v)
  #define READ_CAUSE()         read_csr(scause)
  #define READ_EPC()           read_csr(sepc)
  #define WRITE_EPC(v)         write_csr(sepc, v)
  #define READ_TVEC()          read_csr(stvec)
  #define WRITE_TVEC(v)        write_csr(stvec, v)
#else
  #define READ_STATUS()        read_csr(mstatus)
  #define WRITE_STATUS(v)      write_csr(mstatus, v)
  #define READ_CAUSE()         read_csr(mcause)
  #define READ_EPC()           read_csr(mepc)
  #define WRITE_EPC(v)         write_csr(mepc, v)
  #define READ_TVEC()          read_csr(mtvec)
  #define WRITE_TVEC(v)        write_csr(mtvec, v)
#endif

#endif /* LUGALOS_ARCH_CSR_H */
