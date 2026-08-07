#include "kernel/shell.h"
#include "kernel/printk.h"
#include "kernel/line_editor.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "drivers/i2c_rtc.h"
#include "drivers/uart.h"
#include "fs/vfs.h"
#include "arch/elf.h"
#include "chibicc.h"
#include "lisp.h"
#include "ed.h"
#include <string.h>

/* Bounded output buffer for the POSIX -> S-expression command transformer.
 * Every character bound for `sexpr[]` goes through sb_putc() so a long or
 * adversarial input degrades to a flagged, dropped write instead of walking
 * off the end of a fixed-size stack buffer. */
typedef struct {
    char *buf;
    int idx;
    int cap;         /* buf[cap - 1] is reserved for the terminating NUL */
    bool overflowed;
} sexpr_buf_t;

static void sb_init(sexpr_buf_t *sb, char *buf, int cap) {
    sb->buf = buf;
    sb->idx = 0;
    sb->cap = cap;
    sb->overflowed = false;
}

static void sb_putc(sexpr_buf_t *sb, char c) {
    if (sb->idx >= sb->cap - 1) {
        sb->overflowed = true;
        return;
    }
    sb->buf[sb->idx++] = c;
}

void shell_init(void) {
    printk("[Shell] Interactive Lugal Shell (lsh) initialized with Plan 9 Universal Namespace.\n");
}

static void cmd_help(void) {
    printk("\nAvailable LugalOS Shell Commands (Plan 9 Model):\n");
    printk("  help            - Display command manual\n");
    printk("  uname           - Show OS build target and architecture details\n");
    printk("  ps              - Alias for 'cat /proc/ps'\n");
    printk("  meminfo         - Alias for 'cat /proc/meminfo'\n");
    printk("  df              - Alias for 'cat /proc/df'\n");
    printk("  top             - System process, memory & storage monitor\n");
    printk("  date [YYYY-MM-DD HH:MM:SS] - Get or set system date and RTC time\n");
    printk("  ls [path]       - List directory (/flash0/, /sd0/, /ram0/, /proc/, /dev/, /srv/)\n");
    printk("  cat <path>      - Read and display path (/sd0/file, /proc/ps, /dev/uart)\n");
    printk("  touch <file>    - Create a new empty file\n");
    printk("  mkdir <path>    - Create a new directory\n");
    printk("  rmdir <path>    - Remove an empty directory\n");
    printk("  cp <src> <dst>  - Copy file from source path to destination path\n");
    printk("  write <p> <txt> - Write payload to file, /dev/uart, or /srv/lisp IPC channel\n");
    printk("  rm <file>       - Delete file from disk\n");
    printk("  format <path>   - Initialize a blank/corrupt volume as FAT32 (/sd0 or /ram0; DESTROYS existing data)\n");
    printk("  cc <src> <dst>  - Compile C11 source file to native RISC-V ELF binary (chibicc)\n");
    printk("  exec <elf>      - Load and execute native RISC-V ELF binary\n");
    printk("  e [file]        - Launch Emacs-style full-screen editor\n");
    printk("  ed [file]       - Launch teletype line editor\n");
    printk("  lisp            - Enter interactive Scheme / Lisp REPL environment\n");
    printk("  (help)          - List every bound Lisp primitive (works from 'lisp' or as a (...) line here)\n");
    printk("  clear           - Clear terminal screen\n\n");
}

static void cmd_uname(void) {
    char buf[128];
    int len = vfs_read("/proc/version", buf, sizeof(buf));
    if (len >= 0) printk("%s", buf);
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
    printk("Namespace: Universal Path Resolver (/flash0/, /sd0/, /ram0/, /proc/, /dev/, /srv/)\n");
}

static void parse_and_eval_cmd(const char *cmd_line) {
    while (*cmd_line == ' ' || *cmd_line == '\t') cmd_line++;
    if (*cmd_line == '\0') return;

    if (strcmp(cmd_line, "help") == 0) {
        cmd_help();
        return;
    } else if (strcmp(cmd_line, "uname") == 0) {
        cmd_uname();
        return;
    } else if (strcmp(cmd_line, "clear") == 0) {
        uart_puts("\033[2J\033[H");
        return;
    } else if (strcmp(cmd_line, "ed") == 0) {
        ed_main(NULL);
        return;
    } else if (strncmp(cmd_line, "ed ", 3) == 0) {
        ed_main(&cmd_line[3]);
        return;
    } else if (strcmp(cmd_line, "e") == 0) {
        char edit_buf[2048];
        int mlen = edit_multiline_box("/ram0/system/scratch.lisp", edit_buf, sizeof(edit_buf));
        if (mlen > 0) {
            lisp_val_t *res = lisp_eval_string(edit_buf);
            if (res) {
                if (res->type != LISP_NIL) {
                    printk("=> ");
                    lisp_print(res);
                    printk("\n");
                } else {
                    printk("\n");
                }
            }
        }
        return;
    } else if (strncmp(cmd_line, "e ", 2) == 0) {
        char edit_buf[2048];
        const char *fn = &cmd_line[2];
        while (*fn == ' ') fn++;
        int mlen = edit_multiline_box(fn, edit_buf, sizeof(edit_buf));
        if (mlen > 0) {
            lisp_val_t *res = lisp_eval_string(edit_buf);
            if (res) {
                if (res->type != LISP_NIL) {
                    printk("=> ");
                    lisp_print(res);
                    printk("\n");
                } else {
                    printk("\n");
                }
            }
        }
        return;
    } else if (strcmp(cmd_line, "i2c") == 0 || strcmp(cmd_line, "i2c scan") == 0) {
        i2c_scan_bus();
        return;
    } else if (strcmp(cmd_line, "date") == 0) {
        rtc_time_t tm;
        time_get_rtc(&tm);
        char isostr[32];
        time_format_iso(&tm, isostr, sizeof(isostr));
        printk("%s\n", isostr);
        return;
    } else if (strncmp(cmd_line, "date ", 5) == 0) {
        const char *arg = &cmd_line[5];
        while (*arg == ' ') arg++;
        rtc_time_t tm;
        if (time_parse_iso(arg, &tm)) {
            time_set_rtc(&tm);
            i2c_rtc_write_time(&tm);
            printk("System clock updated: %s\n", arg);
        } else {
            printk("Invalid date format. Expected: YYYY-MM-DD HH:MM:SS\n");
        }
        return;
    } else if (strcmp(cmd_line, "lisp") == 0) {
        lisp_repl();
        return;
    }


    /* Direct S-Expression evaluation if line starts with '(' */
    if (*cmd_line == '(') {
        lisp_val_t *res = lisp_eval_string(cmd_line);
        if (res) {
            if (res->type != LISP_NIL) {
                printk("=> ");
                lisp_print(res);
                printk("\n");
            } else {
                printk("\n");
            }
        }
        return;
    }


    /* Single token variable or evaluation lookup */
    bool has_space = false;
    for (const char *c = cmd_line; *c; c++) {
        if (*c == ' ' || *c == '\t') { has_space = true; break; }
    }

    if (!has_space && strcmp(cmd_line, "ls") != 0 &&
        strcmp(cmd_line, "ps") != 0 &&
        strcmp(cmd_line, "meminfo") != 0 &&
        strcmp(cmd_line, "version") != 0 &&
        strcmp(cmd_line, "df") != 0 &&
        strcmp(cmd_line, "top") != 0 &&
        strcmp(cmd_line, "date") != 0 &&
        strcmp(cmd_line, "time") != 0 &&
        strcmp(cmd_line, "i2c") != 0) {
        lisp_val_t *res = lisp_eval_string(cmd_line);
        if (res && res->type != LISP_NIL) {
            printk("=> ");
            lisp_print(res);
            printk("\n");
            return;
        }
    }



    /* POSIX/Plan9 command -> S-Expression transformer.
     *
     * Every write goes through sb_putc() so a long or adversarial command
     * line degrades to a clean "too long" rejection instead of walking off
     * the end of sexpr[] -- the delimiter/quote writes below used to be
     * unguarded while only content-character writes were bounds-checked,
     * which let e.g. `ls` followed by ~160 short tokens smash the stack
     * (see B2 in plan/completed/2026-08-07_review_and_remediation.md). */
    char sexpr[512];
    sexpr_buf_t sb;
    sb_init(&sb, sexpr, sizeof(sexpr));
    sb_putc(&sb, '(');

    const char *p = cmd_line;
    /* Verb */
    while (*p && *p != ' ' && *p != '\t') {
        sb_putc(&sb, *p);
        p++;
    }

    /* Special case: write <path> <payload...> */
    if (strncmp(cmd_line, "write ", 6) == 0) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p) {
            sb_putc(&sb, ' ');
            sb_putc(&sb, '"');
            while (*p && *p != ' ' && *p != '\t') {
                sb_putc(&sb, *p);
                p++;
            }
            sb_putc(&sb, '"');
            while (*p == ' ' || *p == '\t') p++;
            if (*p) {
                sb_putc(&sb, ' ');
                sb_putc(&sb, '"');
                while (*p) {
                    if (*p == '"') {
                        sb_putc(&sb, '\\');
                        sb_putc(&sb, '"');
                    } else {
                        sb_putc(&sb, *p);
                    }
                    p++;
                }
                sb_putc(&sb, '"');
            }
        }
    } else {
        /* General argument tokenizer */
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0') break;

            sb_putc(&sb, ' ');
            bool quoted = (*p == '"');
            if (!quoted) sb_putc(&sb, '"');

            while (*p) {
                if (quoted && *p == '"') {
                    sb_putc(&sb, *p++);
                    break;
                } else if (!quoted && (*p == ' ' || *p == '\t')) {
                    break;
                } else {
                    sb_putc(&sb, *p);
                    p++;
                }
            }
            if (!quoted) sb_putc(&sb, '"');
        }
    }

    sb_putc(&sb, ')');
    sexpr[sb.idx] = '\0';

    if (sb.overflowed) {
        printk("lsh: command line too long, ignored\n");
        return;
    }

    /* Evaluate translated S-Expression in Lisp engine */
    lisp_val_t *res = lisp_eval_string(sexpr);
    if (res) {
        if (res->type != LISP_NIL) {
            printk("=> ");
            lisp_print(res);
            printk("\n");
        } else {
            printk("\n");
        }
    }
}


void shell_run(void) {
    char buf[512];
    line_editor_init();
    printk("\nLugalOS Interactive Console Shell (`lsh`)\n");
    printk("Type 'help' for commands, 'cat /proc/ps' for tasks, 'ls /dev/' for devices.\n");

    while (1) {
        int idx = readline_interactive("lsh> ", buf, sizeof(buf));
        if (idx == 0) continue;
        parse_and_eval_cmd(buf);
    }
}


