#ifndef _LUGAL_H
#define _LUGAL_H

/* Syscall numbers */
#define SYS_CHAN_CALL  5
#define SYS_PRINT      10
#define SYS_PUTNUM     11
#define SYS_PUTCHAR    12
#define SYS_READ_FILE  13
#define SYS_WRITE_FILE 14
#define SYS_TICKS      21

/* Direct RISC-V ecall system call invocation */
long lugal_syscall(long sys_nr, long a1, long a2, long a3);

/* chibicc compiler intrinsics */
int print(char *s);
int puts(char *s);
int printf(char *s);
int print_str(char *s);
int putnum(long n);
int print_num(long n);
int print_int(long n);
int putchar(char c);
int print_char(char c);
int print_nl(void);
int print_sp(void);
int read_file(char *path, void *buf, int max_len);
int write_file(char *path, void *buf, int len);

#endif
