#include "kernel/shell.h"
#include "kernel/printk.h"
#include "kernel/klog.h"
#include "kernel/palloc.h"
#include "kernel/line_editor.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "drivers/i2c_rtc.h"
#include "drivers/uart.h"
#include "drivers/uart_net.h"
#include "fs/vfs.h"
#include "fs/p9_link.h"
#include "arch/elf.h"
#include "arch/pmp.h"
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
    printk("  p9serve         - Headless 9P server over UART/SLIP (does not return; reset to exit)\n");
    printk("  p9share [off]   - Share this UART between the console and 9P (SLIP demux, A3b)\n");
    printk("  klog [attach|detach <sink>] - Kernel log sinks; read the log via /proc/kmsg\n");
    printk("  taskdemo        - Spawn two cooperative tasks and show them interleave (B2)\n");
    printk("  pmpinfo         - Probe this core's PMP entry count and granularity (B3 prep)\n");
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

/* A3a "headless" 9P mode (plan/phase5_distributed_design.md): dedicates
 * this UART entirely to SLIP-framed 9P traffic and never returns to the
 * shell. This is the low-risk way to get a real 9P wire over a plain UART
 * (QEMU serial, RP2350 UART, a CP2102 dongle) without the RX
 * demultiplexing A3b would need to share the wire with the interactive
 * console -- that demux is deliberately deferred (see the A3 completion
 * notes). Only reachable by explicit user command; a reset is the only way
 * back. */
static void cmd_p9serve(void) {
    printk("\n[9P] Entering headless UART/SLIP 9P server mode -- this session will not\n");
    printk("     return to the shell. Reset the device to get the console back.\n\n");
    p9_link_t *link = uart_slip_get_link();
    for (;;) {
        p9_link_service(link);
    }
}

/* A3b "shared-wire" mode (plan/phase5_distributed_design.md): unlike
 * p9serve above, this returns to the shell immediately. It arms
 * drivers/uart_net.c's RX demux (off by default -- see its own doc
 * comments for the tradeoff this opts into) and registers the demuxed link
 * as a background 9P server, so SLIP-framed 9P traffic and normal
 * keystrokes can now share this UART: whichever arrives, uart_getc()
 * routes it to the right place. This is the single-cable story A3a's
 * headless mode doesn't cover (a real CP2102/RP2350 deployment with only
 * one wire back to the host). `p9share off` reverses both steps. */
static void cmd_p9share(const char *arg) {
    bool enable = true;
    if (arg) {
        while (*arg == ' ') arg++;
        if (strcmp(arg, "off") == 0) enable = false;
    }

    p9_link_t *link = uart_demux_get_link();
    uart_demux_set_enabled(enable);
    if (enable) {
        p9_link_register_background(link);
        printk("\n[9P] Shared-wire 9P active on this UART alongside the console -- type\n");
        printk("     normally; a SLIP-framed 9P peer can attach on the same wire at any\n");
        printk("     time. Run 'p9share off' to disable.\n\n");
    } else {
        p9_link_unregister_background(link);
        printk("\n[9P] Shared-wire 9P disabled; this UART is console-only again.\n\n");
    }
}

/* B0 (plan/phase5_distributed_design.md §5.4): inspect and rebind kernel log
 * output sinks at runtime. `klog` lists them; `klog detach console` stops
 * kernel output reaching this terminal -- which is what makes handing this
 * UART to 9P (`p9serve`) or, later, to a login shell non-destructive: the
 * ring keeps accumulating regardless, so `cat /proc/kmsg` still has the full
 * log afterwards, and so does a remote node reading it over 9P. */
static void cmd_klog(const char *arg) {
    if (arg) {
        while (*arg == ' ') arg++;
    }

    if (!arg || *arg == '\0') {
        printk("\nKernel log sinks:\n");
        const char *name;
        bool attached;
        for (uint32_t i = 0; klog_sink_info(i, &name, &attached); i++) {
            /* No '-' (left-justify) flag in this printk engine -- see
             * kernel/printk.c's format parser, which accepts only '0',
             * width, '.prec' and 'l'. Plain "name: state" instead. */
            printk("  %s: %s\n", name, attached ? "attached" : "detached");
        }
        printk("\n  Ring: %lu bytes buffered (%lu total written)\n",
               (unsigned long)(klog_total() - klog_oldest()),
               (unsigned long)klog_total());
        printk("  Usage: klog [attach|detach] <sink>   (read it with: cat /proc/kmsg)\n\n");
        return;
    }

    bool detach;
    const char *name;
    if (strncmp(arg, "detach ", 7) == 0) {
        detach = true;
        name = arg + 7;
    } else if (strncmp(arg, "attach ", 7) == 0) {
        detach = false;
        name = arg + 7;
    } else {
        printk("[klog] Usage: klog [attach|detach] <sink>\n");
        return;
    }

    while (*name == ' ') name++;
    int rc = detach ? klog_sink_detach(name) : klog_sink_attach(name);
    if (rc < 0) {
        printk("[klog] No such sink: '%s'\n", name);
        return;
    }
    /* If the console sink was just detached this message goes nowhere, which
     * is the point -- it is still recorded in the ring for /proc/kmsg. */
    printk("[klog] Sink '%s' %s\n", name, detach ? "detached" : "attached");
}

/* B2: proves the cooperative scheduler actually switches, rather than
 * merely bookkeeping as the pre-B2 shim did.
 *
 * Two tasks each emit their own marker three times, yielding between each.
 * If switching is real the output interleaves (A1 B1 A2 B2 A3 B3); if
 * sched_yield() were still a no-op the first task would run to completion
 * before the second started (A1 A2 A3 B1 B2 B3). The interleaving is
 * therefore the assertion, not the fact that output appears at all --
 * tests/runner.py checks for exactly that ordering. */
static void taskdemo_body(void *arg) {
    const char *tag = (const char *)arg;
    for (int i = 1; i <= 3; i++) {
        printk("[TaskDemo] %s%d\n", tag, i);
        sched_yield();
    }
}

static void cmd_taskdemo(void) {
    uint32_t before_total = 0, before_free = 0;
    palloc_stats(&before_total, &before_free);

    int a = task_create("demoA", taskdemo_body, (void *)"A");
    int b = task_create("demoB", taskdemo_body, (void *)"B");
    if (a < 0 || b < 0) {
        printk("[TaskDemo] Could not create both tasks\n");
        return;
    }

    /* Drive the demo from this (the boot) task by yielding until both have
     * exited. Cooperative scheduling means they only advance when we do. */
    for (int spin = 0; spin < 64; spin++) sched_yield();

    uint32_t after_total = 0, after_free = 0;
    palloc_stats(&after_total, &after_free);
    printk("[TaskDemo] Done. Heap pages free before=%u after=%u\n",
           before_free, after_free);
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
    } else if (strcmp(cmd_line, "p9serve") == 0) {
        cmd_p9serve();
        return;
    } else if (strcmp(cmd_line, "p9share") == 0) {
        cmd_p9share(NULL);
        return;
    } else if (strncmp(cmd_line, "p9share ", 8) == 0) {
        cmd_p9share(&cmd_line[8]);
        return;
    } else if (strcmp(cmd_line, "pmpinfo") == 0) {
        pmp_report();
        return;
    } else if (strcmp(cmd_line, "pmpdump") == 0) {
        pmp_dump();
        return;
    } else if (strcmp(cmd_line, "taskdemo") == 0) {
        cmd_taskdemo();
        return;
    } else if (strcmp(cmd_line, "klog") == 0) {
        cmd_klog(NULL);
        return;
    } else if (strncmp(cmd_line, "klog ", 5) == 0) {
        cmd_klog(&cmd_line[5]);
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


