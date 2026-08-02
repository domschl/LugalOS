#include "kernel/shell.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "drivers/uart.h"
#include "fs/vfs.h"
#include "arch/elf.h"
#include "lisp.h"
#include "ed.h"
#include <string.h>

void shell_init(void) {
    printk("[Shell] Interactive Lugal Shell (lsh) initialized with Plan 9 Universal Namespace.\n");
}

static void cmd_help(void) {
    printk("\nAvailable LugalOS Shell Commands (Plan 9 Model):\n");
    printk("  help            - Display command manual\n");
    printk("  uname           - Show OS build target and architecture details\n");
    printk("  ps              - Alias for 'cat /proc/ps'\n");
    printk("  meminfo         - Alias for 'cat /proc/meminfo'\n");
    printk("  ls [path]       - List directory (/ram0/, /proc/, /dev/, /srv/)\n");
    printk("  cat <path>      - Read and display path (/ram0/file, /proc/ps, /dev/uart)\n");
    printk("  touch <file>    - Create a new empty file in /ram0/\n");
    printk("  write <p> <txt> - Write payload to file, /dev/uart, or /srv/lisp IPC channel\n");
    printk("  rm <file>       - Delete file from disk\n");
    printk("  exec <elf>      - Load and execute native RISC-V ELF binary\n");
    printk("  ed [file]       - Launch teletype line editor\n");
    printk("  lisp            - Enter interactive Scheme / Lisp REPL environment\n");
    printk("  clear           - Clear terminal screen\n\n");
}

static void cmd_uname(void) {
    char buf[128];
    vfs_read("/proc/version", buf, sizeof(buf));
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
    printk("Namespace: Universal Path Resolver (/ram0/, /proc/, /dev/, /srv/)\n");
}

void shell_run(void) {
    char buf[128];
    printk("\nLugalOS Interactive Console Shell (`lsh`)\n");
    printk("Type 'help' for commands, 'cat /proc/ps' for tasks, 'ls /dev/' for devices.\n");

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

        if (strcmp(buf, "help") == 0) {
            cmd_help();
        } else if (strcmp(buf, "uname") == 0) {
            cmd_uname();
        } else if (strcmp(buf, "ps") == 0) {
            char pbuf[256];
            vfs_read("/proc/ps", pbuf, sizeof(pbuf));
        } else if (strcmp(buf, "meminfo") == 0) {
            char mbuf[256];
            vfs_read("/proc/meminfo", mbuf, sizeof(mbuf));
        } else if (strcmp(buf, "ls") == 0) {
            vfs_ls("/ram0/");
        } else if (strncmp(buf, "ls ", 3) == 0) {
            vfs_ls(&buf[3]);
        } else if (strncmp(buf, "cat ", 4) == 0) {
            char file_data[512];
            file_data[0] = '\0';
            int read_len = vfs_read(&buf[4], file_data, 512);
            if (read_len > 0) {
                printk("%s\n", file_data);
            } else if (read_len < 0) {
                printk("cat: '%s': Cannot read path\n", &buf[4]);
            }
        } else if (strncmp(buf, "touch ", 6) == 0) {
            if (vfs_write(&buf[6], "", 0) == 0) {
                printk("File '%s' created.\n", &buf[6]);
            } else {
                printk("touch: failed to create '%s'\n", &buf[6]);
            }
        } else if (strncmp(buf, "write ", 6) == 0) {
            char *space = strchr(&buf[6], ' ');
            if (space) {
                *space = '\0';
                const char *filename = &buf[6];
                const char *text = space + 1;
                if (vfs_write(filename, text, strlen(text)) == 0) {
                    printk("'%s': %d bytes written.\n", filename, (int)strlen(text));
                } else {
                    printk("write: failed to write '%s'\n", filename);
                }
            } else {
                printk("Usage: write <path> <text>\n");
            }
        } else if (strncmp(buf, "rm ", 3) == 0) {
            if (vfs_remove(&buf[3]) == 0) {
                printk("File '%s' removed.\n", &buf[3]);
            } else {
                printk("rm: failed to remove '%s'\n", &buf[3]);
            }
        } else if (strncmp(buf, "exec ", 5) == 0) {
            elf_load_and_run(&buf[5]);
        } else if (strcmp(buf, "ed") == 0) {
            ed_main(NULL);
        } else if (strncmp(buf, "ed ", 3) == 0) {
            ed_main(&buf[3]);
        } else if (strcmp(buf, "clear") == 0) {
            uart_puts("\033[2J\033[H");
        } else if (strcmp(buf, "lisp") == 0) {
            lisp_repl();
        } else {
            printk("lsh: command not found: '%s'. Type 'help' for list.\n", buf);
        }
    }
}
