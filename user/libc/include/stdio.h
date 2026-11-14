#ifndef LUGALOS_ULIBC_STDIO_H
#define LUGALOS_ULIBC_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct {
    int fd;
    int err;
    int eof;
} FILE;

extern FILE *const stdin;
extern FILE *const stdout;
extern FILE *const stderr;

#define EOF (-1)

int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int snprintf(char *str, size_t size, const char *format, ...);
int sprintf(char *str, const char *format, ...);
int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int vprintf(const char *format, va_list ap);
int puts(const char *s);
int putchar(int c);
int fputs(const char *s, FILE *stream);

#endif /* LUGALOS_ULIBC_STDIO_H */
