#ifndef _LUGAL_H
#define _LUGAL_H

#define SYS_IPC_CALL   1
#define SYS_IPC_REPLY  2
#define SYS_IPC_SEND   3
#define SYS_IPC_RECV   4

#define SYS_PRINT      10
#define SYS_PUTNUM     11
#define SYS_PUTCHAR    12
#define SYS_READ_FILE  13
#define SYS_WRITE_FILE 14

#define IPC_ANY       -1

struct ipc_msg {
    long tag;
    long d0;
    long d1;
    long d2;
    long d3;
    long d4;
};

long lugal_syscall(long sys_nr, long a1, long a2, long a3);

int print(char *s);
int puts(char *s);
int printf(char *s);
int putnum(long n);
int putchar(char c);
int read_file(char *path, void *buf, int max_len);
int write_file(char *path, void *buf, int len);

#endif
