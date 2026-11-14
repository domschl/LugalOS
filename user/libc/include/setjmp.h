#ifndef LUGALOS_ULIBC_SETJMP_H
#define LUGALOS_ULIBC_SETJMP_H

#include <stdint.h>

#if __riscv_xlen == 64
typedef uint64_t jmp_buf[14];
#else
typedef uint32_t jmp_buf[14];
#endif

int  setjmp(jmp_buf env);
__attribute__((noreturn)) void longjmp(jmp_buf env, int val);

#endif /* LUGALOS_ULIBC_SETJMP_H */
