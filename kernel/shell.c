#include "kernel/shell.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "drivers/uart.h"
#include "fs/vfs.h"
#include "lisp.h"
#include "ed.h"
#include <string.h>

void shell_init(void) {
    printk("[Shell] Interactive Lugal Shell (lsh) initialized with File System & Editor support.\n");
}

static void cmd_help(void) {
    printk("\nAvailable LugalOS Shell Commands:\n");
    printk("  help            - Display command manual\n");
    printk("  uname           - Show OS build target and architecture details\n");
    printk("  ps              - Show active processes and scheduler state\n");
    printk("  meminfo         - Show memory heap allocation info\n");
    printk("  ls              - List files in FAT32 directory\n");
    printk("  cat <file>      - Read and display contents of a text file\n");
    printk("  touch <file>    - Create a new empty file\n");
    printk("  write <f> <txt> - Write text string to file\n");
    printk("  rm <file>       - Delete file from disk\n");
    printk("  ed [file]       - Launch teletype line editor\n");
    printk("  lisp            - Enter interactive Scheme / Lisp REPL environment\n");
    printk("  clear           - Clear terminal screen\n\n");
}

static void cmd_uname(void) {
    printk("LugalOS v0.3.0 (Microkernel VFS Core)\n");
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
    printk("Storage Driver: FAT32 RAMDisk / Block Device\n");
}

static void cmd_ps(void) {
    printk("PID  State    Name\n");
    printk("---  -------  ------------\n");
    printk(" 0   RUNNING  kernel_idle\n");
    printk(" 1   READY    lsh_console\n");
    printk(" 2   READY    lisp_engine\n");
    printk(" 3   READY    vfs_server (FAT32)\n");
}

static void cmd_meminfo(void) {
    printk("Heap & Storage Status:\n");
    printk("  System Architecture: RISC-V Bare-Metal\n");
    printk("  Page Size: 4096 bytes\n");
    printk("  Storage Driver: FAT32 Volume\n");
    printk("  VFS Status: Active (PID %d)\n", VFS_PID);
}

void shell_run(void) {
    char buf[128];
    printk("\nLugalOS Interactive Console Shell (`lsh`)\n");
    printk("Type 'help' for commands, 'ls' for files, 'ed' for editor, 'lisp' for Scheme REPL.\n");

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
            cmd_ps();
        } else if (strcmp(buf, "meminfo") == 0) {
            cmd_meminfo();
        } else if (strcmp(buf, "ls") == 0) {
            vfs_ls();
        } else if (strncmp(buf, "cat ", 4) == 0) {
            char file_data[512];
            int read_len = vfs_read(&buf[4], file_data, 512);
            if (read_len >= 0) {
                printk("%s\n", file_data);
            } else {
                printk("cat: '%s': No such file\n", &buf[4]);
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
                printk("Usage: write <filename> <text>\n");
            }
        } else if (strncmp(buf, "rm ", 3) == 0) {
            if (vfs_remove(&buf[3]) == 0) {
                printk("File '%s' removed.\n", &buf[3]);
            } else {
                printk("rm: failed to remove '%s'\n", &buf[3]);
            }
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
