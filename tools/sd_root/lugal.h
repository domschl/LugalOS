#ifndef _LUGAL_H
#define _LUGAL_H

/* 1-4 were the register-IPC entry points; permanently retired (C3), so a
 * program built against the old ABI gets a clean "unknown syscall" rather
 * than whatever took the number. */
#define SYS_CHAN_CALL  5

#define SYS_PRINT      10
#define SYS_PUTNUM     11
#define SYS_PUTCHAR    12
#define SYS_READ_FILE  13
#define SYS_WRITE_FILE 14

long lugal_syscall(long sys_nr, long a1, long a2, long a3);

int print(char *s);
int puts(char *s);
int printf(char *s);
int putnum(long n);
int putchar(char c);
int read_file(char *path, void *buf, int max_len);
int write_file(char *path, void *buf, int len);

#endif
