#include "kernel/shell.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/klog.h"
#include "kernel/palloc.h"
#include "kernel/mem_domain.h"
#include "kernel/device.h"
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
#include "arch/umode.h"
#include "arch/trap.h"
#include "kernel/ipc.h"
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
    cprintf("\nAvailable LugalOS Shell Commands (Plan 9 Model):\n");
    cprintf("  help            - Display command manual\n");
    cprintf("  uname           - Show OS build target and architecture details\n");
    cprintf("  ps              - Alias for 'cat /proc/ps'\n");
    cprintf("  meminfo         - Alias for 'cat /proc/meminfo'\n");
    cprintf("  df              - Alias for 'cat /proc/df'\n");
    cprintf("  top             - System process, memory & storage monitor\n");
    cprintf("  date [YYYY-MM-DD HH:MM:SS] - Get or set system date and RTC time\n");
    cprintf("  ls [path]       - List directory (/flash0/, /sd0/, /ram0/, /proc/, /dev/, /srv/)\n");
    cprintf("  cat <path>      - Read and display path (/sd0/file, /proc/ps, /dev/uart)\n");
    cprintf("  touch <file>    - Create a new empty file\n");
    cprintf("  mkdir <path>    - Create a new directory\n");
    cprintf("  rmdir <path>    - Remove an empty directory\n");
    cprintf("  cp <src> <dst>  - Copy file from source path to destination path\n");
    cprintf("  write <p> <txt> - Write payload to a file, /dev/uart, or a /srv/ service endpoint\n");
    cprintf("  rm <file>       - Delete file from disk\n");
    cprintf("  format <path>   - Initialize a blank/corrupt volume as FAT32 (/sd0 or /ram0; DESTROYS existing data)\n");
    cprintf("  cc <src> <dst>  - Compile C11 source file to native RISC-V ELF binary (chibicc)\n");
    cprintf("  exec <elf>      - Load and execute native RISC-V ELF binary\n");
    cprintf("  e [file]        - Launch Emacs-style full-screen editor\n");
    cprintf("  ed [file]       - Launch teletype line editor\n");
    cprintf("  lisp            - Enter interactive Scheme / Lisp REPL environment\n");
    cprintf("  p9serve         - Headless 9P server over UART/SLIP (does not return; reset to exit)\n");
    cprintf("  p9share [off]   - Share this UART between the console and 9P (SLIP demux)\n");
    cprintf("  klog [attach|detach <sink>] - Kernel log sinks; read the log via /proc/kmsg\n");
    cprintf("  write /srv/console <txt>    - Emit via the console server (a channel service)\n");
    cprintf("  taskdemo        - Spawn two cooperative tasks and show them interleave\n");
    cprintf("  pmpinfo         - Report this core's usable PMP regions and granularity\n");
    cprintf("  pmpdump         - Per-register PMP dump (reset value, readback, verdict)\n");
    cprintf("  usertest        - Run a task in U-mode and syscall back into the kernel\n");
    cprintf("  isolationtest   - U-mode task stores into kernel memory; must fault\n");
    cprintf("  deputytest      - U-mode task asks the kernel to WRITE kernel memory; must be refused\n");
    cprintf("  i2c [scan]      - Scan the I2C bus for devices\n");
    cprintf("  time            - Show system uptime\n");
    cprintf("  version         - Alias for 'cat /proc/version'\n");
    cprintf("  (help)          - List every bound Lisp primitive (works from 'lisp' or as a (...) line here)\n");
    cprintf("  clear           - Clear terminal screen\n\n");
}

static void cmd_uname(void) {
    char buf[128];
    int len = vfs_read("/proc/version", buf, sizeof(buf));
    if (len >= 0) cprintf("%s", buf);
#if defined(CONFIG_TARGET_RV32)
    cprintf("Architecture: RISC-V 32-bit (RV32IMAC)\n");
#elif defined(CONFIG_TARGET_RV64)
    cprintf("Architecture: RISC-V 64-bit (RV64GC)\n");
#endif

#if defined(CONFIG_NOMMU)
    cprintf("Memory Mode: NOMMU Physical Direct Execution\n");
#elif defined(CONFIG_MMU)
    cprintf("Memory Mode: Sv39 Virtual Memory Page Tables\n");
#endif
    cprintf("Namespace: Universal Path Resolver (/flash0/, /sd0/, /ram0/, /proc/, /dev/, /srv/)\n");
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
    cprintf("\n[9P] Entering headless UART/SLIP 9P server mode -- this session will not\n");
    cprintf("     return to the shell. Reset the device to get the console back.\n\n");
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
        cprintf("\n[9P] Shared-wire 9P active on this UART alongside the console -- type\n");
        cprintf("     normally; a SLIP-framed 9P peer can attach on the same wire at any\n");
        cprintf("     time. Run 'p9share off' to disable.\n\n");
    } else {
        p9_link_unregister_background(link);
        cprintf("\n[9P] Shared-wire 9P disabled; this UART is console-only again.\n\n");
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
        cprintf("\nKernel log sinks:\n");
        const char *name;
        bool attached;
        for (uint32_t i = 0; klog_sink_info(i, &name, &attached); i++) {
            /* No '-' (left-justify) flag in this printk engine -- see
             * kernel/printk.c's format parser, which accepts only '0',
             * width, '.prec' and 'l'. Plain "name: state" instead. */
            cprintf("  %s: %s\n", name, attached ? "attached" : "detached");
        }
        cprintf("\n  Ring: %lu bytes buffered (%lu total written)\n",
               (unsigned long)(klog_total() - klog_oldest()),
               (unsigned long)klog_total());
        cprintf("  Usage: klog [attach|detach] <sink>   (read it with: cat /proc/kmsg)\n\n");
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

/* B3: proves the M->U transition and the trap path's stack swap.
 *
 * The user function below runs with the privilege level actually lowered: it
 * cannot touch CSRs, cannot execute privileged instructions, and traps into
 * the kernel for every ecall. Each ecall exercises the scratch-CSR swap added
 * in B3's first commit -- entering the kernel on the task's kernel stack
 * rather than on the user stack it was using.
 *
 * SYS_PUTCHAR is used deliberately: it passes a *value*, not a pointer. The
 * pointer-taking syscalls still dereference user addresses directly, and
 * calling one from here would "work" only because nothing separates the
 * address spaces yet. That is B3's copy-in/copy-out step, not this one. */
/* --- U-mode probe code ---
 *
 * These live in .utext, the one page U-mode is granted execute on, and must
 * be genuinely self-contained: anything they reach outside that page is a
 * fault. Two things break that by default and neither is obvious:
 *
 *   - UBSan instrumentation (on for the QEMU builds) inserts calls to
 *     __ubsan_handle_*_abort in kernel .text. Hence no_sanitize here.
 *   - String literals are emitted into kernel .rodata and loaded from there,
 *     so every message has to be built by explicit assignment instead. Ugly,
 *     and the ugliness is the point: it is visible rather than depending on
 *     what the optimiser happened to do. RV32 inlined the stores and RV64
 *     loaded from .rodata for identical source, which is exactly the kind of
 *     difference that turns into a mystery fault on one target only.
 *
 * B6's ELF work replaces all of this with separately linked user programs,
 * where self-containment is structural rather than hand-maintained. */
#define UATTR __attribute__((section(".utext"))) __attribute__((no_sanitize("undefined")))

/* Value-only syscall: no pointer, so nothing to validate and nothing outside
 * .utext to reach. */
/* always_inline, not merely inline: at -Os the compiler emitted these as real
 * functions in kernel .text, which U-mode cannot execute. A helper that is
 * "obviously" inlined is not a guarantee. */
__attribute__((always_inline)) static inline void usys_putc(char c) {
    __asm__ __volatile__("mv a0, %0\n mv a1, %1\n ecall"
                         :: "r"(12), "r"((uintptr_t)c) : "a0", "a1");
}
__attribute__((always_inline)) static inline long usys_read(const char *path, void *buf, long len) {
    long rc;
    __asm__ __volatile__("mv a0, %1\n mv a1, %2\n mv a2, %3\n mv a3, %4\n"
                         "ecall\n mv %0, a0"
                         : "=r"(rc)
                         : "r"(13), "r"((uintptr_t)path), "r"((uintptr_t)buf), "r"(len)
                         : "a0", "a1", "a2", "a3", "memory");
    return rc;
}
__attribute__((always_inline)) static inline void usys_exit(void) {
    __asm__ __volatile__("mv a0, %0\n ecall" :: "r"(SYS_UEXIT) : "a0");
}

UATTR static void user_probe(void) {
    usys_putc('U'); usys_putc('M'); usys_putc('O'); usys_putc('D');
    usys_putc('E'); usys_putc('_'); usys_putc('O'); usys_putc('K');
    usys_exit();
    for (;;) { }
}

/* The isolation probe: a store into kernel memory the task was never granted.
 * Under a correct domain it faults and the task is terminated. */
static volatile uintptr_t g_kernel_canary = 0xC0FFEE;

UATTR static void user_intruder(void) {
    g_kernel_canary = 0xDEAD;
    /* Only reached if the store was NOT stopped. Report explicitly: silence
     * would look identical to a correctly terminated task. */
    usys_putc('N'); usys_putc('O'); usys_putc('T'); usys_putc('_');
    usys_putc('I'); usys_putc('S'); usys_putc('O');
    usys_exit();
    for (;;) { }
}

/* The confused-deputy probe: can a U-mode task get the KERNEL to write to
 * kernel memory on its behalf? SYS_READ_FILE's `buf` is a destination the
 * kernel writes into, validated by copy_to_user() against this task's domain.
 * Both outcomes are asserted -- a syscall layer refusing every pointer would
 * pass the "refused" half while being useless. */
static volatile uintptr_t g_deputy_target = 0xFEEDFACE;

UATTR static void user_deputy(void) {
    /* volatile, because gcc recognises a run of consecutive char stores and
     * turns it back into a copy from a .rodata blob -- defeating the whole
     * point of writing them out. RV32 did not do this and RV64 did, for
     * identical source. */
    volatile char path[16];
    path[0]='/'; path[1]='p'; path[2]='r'; path[3]='o'; path[4]='c';
    path[5]='/'; path[6]='v'; path[7]='e'; path[8]='r'; path[9]='s';
    path[10]='i'; path[11]='o'; path[12]='n'; path[13]='\0';
    char mybuf[64];

    long rc = usys_read((const char *)path, (void *)&g_deputy_target, 16);
    if (rc < 0) {
        usys_putc('D'); usys_putc('E'); usys_putc('P'); usys_putc('U');
        usys_putc('T'); usys_putc('Y'); usys_putc('_');
        usys_putc('R'); usys_putc('E'); usys_putc('F'); usys_putc('U');
        usys_putc('S'); usys_putc('E'); usys_putc('D');
    } else {
        usys_putc('D'); usys_putc('E'); usys_putc('P'); usys_putc('U');
        usys_putc('T'); usys_putc('Y'); usys_putc('_');
        usys_putc('W'); usys_putc('R'); usys_putc('O'); usys_putc('T'); usys_putc('E');
    }

    rc = usys_read((const char *)path, mybuf, 32);
    usys_putc(' ');
    if (rc >= 0) {
        usys_putc('O'); usys_putc('W'); usys_putc('N'); usys_putc('B');
        usys_putc('U'); usys_putc('F'); usys_putc('_');
        usys_putc('O'); usys_putc('K');
    } else {
        usys_putc('O'); usys_putc('W'); usys_putc('N'); usys_putc('B');
        usys_putc('U'); usys_putc('F'); usys_putc('_');
        usys_putc('B'); usys_putc('A'); usys_putc('D');
    }
    usys_exit();
    for (;;) { }
}

/* A full page, page-aligned: one rule that satisfies both backends. PMP needs
 * power-of-two and self-aligned; Sv39 cannot grant anything finer than a page,
 * so a sub-page region would silently hand U-mode the rest of the page. Taking
 * the stricter of the two keeps a single description working on both. */
static uint8_t g_user_stack[4096] __attribute__((aligned(4096)));
static mem_domain_t g_user_domain;

/* Set only once a task has actually reached U-mode. Without it the isolation
 * report is a false positive on any core where the domain could not be
 * installed: the canary is untouched because nothing ran, which reads
 * identically to "the write was correctly blocked". */
static volatile bool g_user_entered;

static void user_task_common(void (*entry)(void)) {
    mem_domain_init(&g_user_domain);

    /* Order matters: PMP resolves against the lowest-numbered matching
     * region, so the narrow RW grant for this task's own stack must come
     * before the broad RX grant that also covers it. */
    mem_domain_add(&g_user_domain, (uintptr_t)g_user_stack, sizeof(g_user_stack),
                   MEM_R | MEM_W);

    uintptr_t tbase, tsize;
    board_text_region(&tbase, &tsize);
    mem_domain_add(&g_user_domain, tbase, tsize, MEM_R | MEM_X);

    if (task_set_domain(sched_current_pid(), &g_user_domain) != 0) {
        /* The hardware did not install the domain as written, so nothing here
         * knows what this task would actually be allowed to touch. Refusing is
         * the only honest option -- entering U-mode anyway would report
         * "isolated" on the strength of a restriction that was never verified. */
        printk("[UserTest] Refusing to enter U-mode: memory domain not enforceable\n");
        return;
    }
    g_user_entered = true;
    arch_enter_user(entry, (uintptr_t)g_user_stack + sizeof(g_user_stack));
}

static void usertest_body(void *arg)  { (void)arg; user_task_common(user_probe); }
static void intruder_body(void *arg)  { (void)arg; user_task_common(user_intruder); }
static void deputy_body(void *arg)    { (void)arg; user_task_common(user_deputy); }

static void run_user_task(const char *name, void (*body)(void *)) {
    g_user_entered = false;
    int pid = task_create(name, body, NULL);
    if (pid < 0) {
        printk("[UserTest] Could not create the task\n");
        return;
    }
    for (int i = 0; i < 64; i++) sched_yield();
}

static void cmd_usertest(void) {
    if (!mem_domain_enforced()) {
        printk("[UserTest] NOTE: this build cannot enforce domains (S-mode; Sv39 is B5).\n");
    }
    run_user_task("usertest", usertest_body);

    uintptr_t c = arch_last_ecall_cause();
    cprintf("\n[UserTest] Last ecall trap cause: %lu (%s)\n", (unsigned long)c,
           c == 8 ? "U-mode -- privilege level really dropped"
                  : "NOT U-mode -- the transition did not happen");
    printk("[UserTest] Returned to kernel mode; task ended cleanly.\n");
}

static void cmd_deputytest(void) {
    g_deputy_target = 0xFEEDFACE;
    printk("[Deputy] Kernel target at %p holds 0x%lx before.\n",
           (const void *)&g_deputy_target, (unsigned long)g_deputy_target);
    run_user_task("deputy", deputy_body);
    if (!g_user_entered) {
        cprintf("\n[Deputy] INCONCLUSIVE -- the task never entered U-mode.\n");
        return;
    }
    cprintf("\n[Deputy] Kernel target holds 0x%lx after -- %s\n",
           (unsigned long)g_deputy_target,
           g_deputy_target == 0xFEEDFACE ? "UNTOUCHED"
                                         : "OVERWRITTEN VIA THE KERNEL");
}

static void cmd_usertest_isolation(void) {
    /* State the enforcement capability here, not only in usertest: this is
     * the command whose result is meaningless without it. A "BREACHED" line
     * with no explanation reads as a bug rather than as the documented state
     * of a build whose mechanism (Sv39) is still B5. */
    if (!mem_domain_enforced()) {
        printk("[Isolation] NOTE: this build cannot enforce domains (S-mode; Sv39 is B5),\n");
        printk("[Isolation]       so the write below is EXPECTED to succeed.\n");
    }
    g_kernel_canary = 0xC0FFEE;
    printk("[Isolation] Canary before: 0x%lx\n", (unsigned long)g_kernel_canary);
    run_user_task("intruder", intruder_body);

    if (!g_user_entered) {
        /* The task never reached U-mode, so the canary proves nothing about
         * isolation. Saying "ISOLATED" here would be a false positive of
         * exactly the kind this whole command exists to rule out. */
        printk("[Isolation] INCONCLUSIVE -- the task never entered U-mode; "
               "the canary was never at risk.\n");
        return;
    }
    printk("[Isolation] Canary after:  0x%lx -- %s\n",
           (unsigned long)g_kernel_canary,
           g_kernel_canary == 0xC0FFEE ? "ISOLATED (kernel memory untouched)"
                                       : "BREACHED (user task wrote kernel memory)");
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
                    cprintf("=> ");
                    lisp_print(res);
                    cprintf("\n");
                } else {
                    cprintf("\n");
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
                    cprintf("=> ");
                    lisp_print(res);
                    cprintf("\n");
                } else {
                    cprintf("\n");
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
        cprintf("%s\n", isostr);
        return;
    } else if (strncmp(cmd_line, "date ", 5) == 0) {
        const char *arg = &cmd_line[5];
        while (*arg == ' ') arg++;
        rtc_time_t tm;
        if (time_parse_iso(arg, &tm)) {
            time_set_rtc(&tm);
            i2c_rtc_write_time(&tm);
            cprintf("System clock updated: %s\n", arg);
        } else {
            cprintf("Invalid date format. Expected: YYYY-MM-DD HH:MM:SS\n");
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
    } else if (strcmp(cmd_line, "usertest") == 0) {
        cmd_usertest();
        return;
    } else if (strcmp(cmd_line, "isolationtest") == 0) {
        cmd_usertest_isolation();
        return;
    } else if (strcmp(cmd_line, "deputytest") == 0) {
        cmd_deputytest();
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
                cprintf("=> ");
                lisp_print(res);
                cprintf("\n");
            } else {
                cprintf("\n");
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
            cprintf("=> ");
            lisp_print(res);
            cprintf("\n");
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
        cprintf("lsh: command line too long, ignored\n");
        return;
    }

    /* Evaluate translated S-Expression in Lisp engine */
    lisp_val_t *res = lisp_eval_string(sexpr);
    if (res) {
        if (res->type != LISP_NIL) {
            cprintf("=> ");
            lisp_print(res);
            cprintf("\n");
        } else {
            cprintf("\n");
        }
    }
}


void shell_run(void) {
    char buf[512];
    line_editor_init();
    cprintf("\nLugalOS Interactive Console Shell (`lsh`)\n");
    cprintf("Type 'help' for commands, 'cat /proc/ps' for tasks, 'ls /dev/' for devices.\n");

    while (1) {
        int idx = readline_interactive("lsh> ", buf, sizeof(buf));
        if (idx == 0) continue;
        parse_and_eval_cmd(buf);
    }
}


