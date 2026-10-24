# C Programming on LugalOS: Compiler, Intrinsics & Syscall ABI

[Documentation](../../README.md) > [Developer Manual](../index.md) > **C Programming & Compiler**

---

LugalOS features an on-device native C compiler (`chibicc`) capable of compiling C11 programs directly to unprivileged RISC-V user ELF binaries.

## 1. On-Device Compilation & Execution

On hardware targets with sufficient RAM (`esp32p4-lcd7b`, `esp32p4-tab5`, `rp2350-terminal`, `rv32`, `rv64`), the `cc` command is built into the `lsh` shell:

```text
# Compile source to native RISC-V ELF binary
lsh> cc /sd0/hello.c /sd0/hello.elf

# Execute directly or via exec
lsh> /sd0/hello.elf
Hello from native RISC-V user space!
```

Memory Efficiency: The compiler allocates AST nodes and code generation pools dynamically from page alloc (`palloc`) and frees all memory immediately upon completion.

## 2. Supported C11 Language Subset

`chibicc` implements a lightweight, self-contained freestanding C11 subset:
* Types: `int`, `char`, `long`, `void`, single- and multi-level pointers (`int *`, `char **`), arrays (`int arr[10]`), and `struct` types.
* Declarations: Standard declarators and comma-separated declarations (e.g. `int a = 1, b = 2;`).
* Control Flow: `if`, `else`, `while`, `for`, `return`, `break`, `continue`.
* Expressions & Operators:
  * Arithmetic: `+`, `-`, `*`, `/`, `%`
  * Assignment: `=`, `+=`, `-=`, `*=`, `/=`, `%=`
  * Increment & Decrement: `++`, `--` (both prefix and postfix)
  * Comparisons: `==`, `!=`, `<`, `<=`, `>`, `>=`
  * Logic & Unary: `!`, unary minus `-`, address-of `&`, pointer dereference `*`
  * Member Access: Struct dot `.` and pointer arrow `->`
  * Array Indexing: `arr[i]`
* Preprocessor: `#include <...>`, `#include "..."`, `#define`, `#undef`, `#ifdef`, `#ifndef`, `#else`, `#endif`. Headers are searched in the source directory, `/sd0/include/`, `/sd0/`, `/ram0/include/`, and `/ram0/`.

## 3. The `lugal.h` Header & Compiler Intrinsics

User binaries are freestanding (no heavy external libc required). The compiler provides direct zero-overhead intrinsics for console I/O, file access, and kernel syscalls:

```c
#include <lugal.h>

/* Direct RISC-V ecall system call invocation */
long lugal_syscall(long sys_nr, long a1, long a2, long a3);

/* Console Output Intrinsics */
int print(char *s);         /* Print null-terminated string */
int puts(char *s);          /* Print string (alias for print) */
int printf(char *s);        /* Print string (alias for print) */
int print_str(char *s);     /* Print string */
int putnum(long n);         /* Print signed decimal integer */
int print_num(long n);      /* Print signed decimal integer (alias) */
int print_int(long n);      /* Print signed decimal integer (alias) */
int putchar(char c);        /* Print single character */
int print_char(char c);     /* Print single character (alias) */
int print_nl(void);         /* Print newline character '\n' */
int print_sp(void);         /* Print space character ' ' */

/* File I/O Intrinsics */
int read_file(char *path, void *buf, int max_len);
int write_file(char *path, void *buf, int len);
```

## 4. How to Print Formatted Output & Numbers

Because standard `printf` format specifiers (`%d`, `%s`) are not linked into freestanding binaries, use `putnum()` and `print()` to output values:

```c
#include <lugal.h>

int main(void) {
    int a = 12, b = 30;
    print("The sum of ");
    putnum(a);
    print(" and ");
    putnum(b);
    print(" is ");
    putnum(a + b);
    print_nl();
    return 0;
}
```

### Integer to String Conversion (`itoa`)
For formatting numbers into memory buffers before writing to files via `write_file()`:

```c
char *itoa(long val, char *buf) {
    char *p = buf;
    if (val < 0) {
        *p++ = '-';
        val = -val;
    }
    char tmp[24];
    int i = 0;
    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) {
            tmp[i++] = '0' + (val % 10);
            val /= 10;
        }
    }
    while (i > 0) {
        *p++ = tmp[--i];
    }
    *p = '\0';
    return buf;
}
```

## 5. System Call ABI & Memory Confinement

User applications execute in unprivileged RISC-V User Mode (U-Mode):
* Memory Confinement: Hardware Physical Memory Protection (PMP) on NOMMU microcontrollers, Sv39 page tables on RV64.
* Fault Containment: Null pointer dereferences or out-of-bounds memory writes trigger a hardware access fault trapped by the microkernel. The faulty user task is safely terminated and its resources reclaimed without compromising system stability.
* Microkernel Syscalls:
  * `SYS_CHAN_CALL` (5): Channel IPC service call.
  * `SYS_PRINT` (10): Write string buffer to console.
  * `SYS_PUTNUM` (11): Format and output integer.
  * `SYS_PUTCHAR` (12): Write single character to console.
  * `SYS_READ_FILE` (13): Read file from VFS.
  * `SYS_WRITE_FILE` (14): Write file to VFS.
  * `SYS_TICKS` (21): Read system preemption tick counter.

---

**Navigation:** [<- Developer Manual](../index.md) | [Lisp Scripting ->](lisp_scripting.md) | [Documentation Index](../../README.md)
