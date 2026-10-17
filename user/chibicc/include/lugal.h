/*
 * <lugal.h> - LugalOS Microkernel C System Call & User API Header
 * Copyright (c) 2026 LugalOS Developers
 * License: MIT License
 */

#ifndef _LUGAL_H
#define _LUGAL_H

#include <stdint.h>
#include <stddef.h>

/* Syscall numbers */
#define SYS_CHAN_CALL  5
#define SYS_PRINT      10
#define SYS_PUTNUM     11
#define SYS_PUTCHAR    12
#define SYS_READ_FILE  13
#define SYS_WRITE_FILE 14
#define SYS_TICKS      21

/* Direct RISC-V ecall system call intrinsics */
long lugal_syscall(long sys_nr, long a1, long a2, long a3);
int print(const char *s);
int puts(const char *s);
int printf(const char *s);
int print_str(const char *s);
int putnum(long n);
int print_num(long n);
int print_int(long n);
int putchar(char c);
int print_char(char c);
int print_nl(void);
int print_sp(void);
int read_file(const char *path, void *buf, int max_len);
int write_file(const char *path, const void *buf, int len);

#endif /* _LUGAL_H */
