#include "kernel/shell.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "drivers/uart.h"
#include "lisp.h"

static int streq(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2 == 0;
}

void shell_init(void) {
    printk("[Shell] Interactive Lugal Shell (lsh) initialized.\n");
}

static void cmd_help(void) {
    printk("\nAvailable LugalOS Shell Commands:\n");
    printk("  help    - Display this command manual\n");
    printk("  uname   - Show OS build target and architecture details\n");
    printk("  ps      - Show active processes and scheduler state\n");
    printk("  meminfo - Show memory heap allocation info\n");
    printk("  clear   - Clear terminal screen\n");
    printk("  lisp    - Enter interactive Scheme / Lisp REPL environment\n\n");
}

static void cmd_uname(void) {
    printk("LugalOS v0.2.0 (Microkernel Core)\n");
#if defined(CONFIG_TARGET_RV32)
    printk("Architecture: RISC-V 32-bit (RV32IMAC)\n");
#elif defined(CONFIG_TARGET_RV64)
    printk("Architecture: RISC-V 64-bit (RV64GC)\n");
#endif

#if defined(CONFIG_NOMMU)
    printk("Memory Mode: NOMMU Physical Direct Execution\n");
#elif defined(CONFIG_MMU)
    printk("Memory Mode: Sv39 Virtual Memory Page Tables\n");
#endif
}

static void cmd_ps(void) {
    printk("PID  State    Name\n");
    printk("---  -------  ------------\n");
    printk(" 0   RUNNING  kernel_idle\n");
    printk(" 1   READY    lsh_console\n");
    printk(" 2   READY    lisp_engine\n");
}

static void cmd_meminfo(void) {
    printk("Heap Memory Status:\n");
    printk("  System Architecture: RISC-V Bare-Metal\n");
    printk("  Page Size: 4096 bytes\n");
    printk("  VMM Status: Active\n");
}

void shell_run(void) {
    char buf[128];
    printk("\nLugalOS Interactive Console Shell (`lsh`)\n");
    printk("Type 'help' for available commands or 'lisp' to start Scheme REPL.\n");

    while (1) {
        printk("lsh> ");
        int idx = 0;
        while (1) {
            char c = uart_getc();
            if (c == '\r' || c == '\n') {
                uart_puts("\r\n");
                buf[idx] = '\0';
                break;
            } else if (c == 0x08 || c == 0x7F) {
                if (idx > 0) {
                    idx--;
                    uart_puts("\b \b");
                }
            } else if (c >= 32 && c <= 126) {
                if (idx < 127) {
                    buf[idx++] = c;
                    uart_putc(c);
                }
            }
        }

        if (idx == 0) continue;

        if (streq(buf, "help")) {
            cmd_help();
        } else if (streq(buf, "uname")) {
            cmd_uname();
        } else if (streq(buf, "ps")) {
            cmd_ps();
        } else if (streq(buf, "meminfo")) {
            cmd_meminfo();
        } else if (streq(buf, "clear")) {
            uart_puts("\033[2J\033[H");
        } else if (streq(buf, "lisp")) {
            lisp_repl();
        } else {
            printk("lsh: command not found: '%s'. Type 'help' for list.\n", buf);
        }
    }
}
