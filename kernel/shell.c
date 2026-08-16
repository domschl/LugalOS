#include "kernel/shell.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/klog.h"
#include "kernel/palloc.h"
#include "kernel/balloc.h"
#include "kernel/mem_domain.h"
#include "kernel/device.h"
#include "kernel/ticker.h"
#include "kernel/line_editor.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "drivers/i2c_rtc.h"
#include "drivers/uart.h"
#include "drivers/uart_net.h"
#if defined(CONFIG_BOARD_RP2350)
#include "drivers/spisd.h"
#else
#include "drivers/virtio_blk.h"
#endif
#include "fs/vfs.h"
#include "fs/p9_link.h"
#include "arch/elf.h"
#include "kernel/path.h"
#include "arch/pmp.h"
#include "arch/umode.h"
#include "arch/trap.h"
#include "kernel/ipc.h"
#include "lugalos_config.h"
#include "lisp.h"
#if CONFIG_ENABLE_CC
#include "chibicc.h"
#endif
#if CONFIG_ENABLE_ED
#include "ed.h"
#endif
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
#if CONFIG_ENABLE_CC
    cprintf("  cc <src> <dst>  - Compile C11 source file to native RISC-V ELF binary (chibicc)\n");
#endif
    cprintf("  exec <elf>      - Run a RISC-V ELF binary as a U-mode task, confined to its own pages\n");
    cprintf("  <name>          - Run <vol>/system/bin/<name>.elf from the first volume on the\n");
    cprintf("                    search path that has it (see 'cat /proc/path'); a path with a\n");
    cprintf("                    '/' in it is always taken literally\n");
    cprintf("  e [file]        - Launch Emacs-style full-screen editor\n");
#if CONFIG_ENABLE_ED
    cprintf("  ed [file]       - Launch teletype line editor\n");
#endif
    cprintf("  lisp            - Enter interactive Scheme / Lisp REPL environment\n");
    cprintf("  p9serve         - Headless 9P server over UART/SLIP (does not return; reset to exit)\n");
    cprintf("  p9share [off]   - Share this UART between the console and 9P (SLIP demux)\n");
    cprintf("  klog [attach|detach <sink>] - Kernel log sinks; read the log via /proc/kmsg\n");
    cprintf("  write /srv/console <txt>    - Emit via the console server (a channel service)\n");
    cprintf("  taskdemo        - Spawn two cooperative tasks and show them interleave\n");
    cprintf("  preempttest     - Prove the timer preempts a task that never yields\n");
    cprintf("  priotest        - Prove a high-priority task wins the next reschedule over hogs\n");
    cprintf("  priostress      - Prove two same-tier busy tasks share the CPU fairly\n");
    cprintf("  pmpinfo         - Report this core's usable PMP regions and granularity\n");
    cprintf("  pmpdump         - Per-register PMP dump (reset value, readback, verdict)\n");
    cprintf("  usertest        - Run a task in U-mode and syscall back into the kernel\n");
    cprintf("  isolationtest   - U-mode task stores into kernel memory; must fault\n");
    cprintf("  deputytest      - U-mode task asks the kernel to WRITE kernel memory; must be refused\n");
    cprintf("  i2c [scan]      - Scan the I2C bus for devices\n");
    cprintf("  time            - Show system uptime\n");
    cprintf("  version         - Alias for 'cat /proc/version'\n");
    cprintf("  (which \"name\")   - Where a bare name resolves to, without running it\n");
    cprintf("  (path-set \"a b\") - Reorder the search path (usually set in system/etc/init.lisp)\n");
    cprintf("  (help)          - List every bound Lisp primitive (works from 'lisp' or as a (...) line here)\n");
    cprintf("  reboot          - Restart the board (works even if the evaluator is inert)\n");
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
    /* ra = 0: every probe below ends with usys_exit() and an infinite loop,
     * so none of them ever returns. The ELF loader is what needs a real
     * return address. */
    arch_enter_user(entry, (uintptr_t)g_user_stack + sizeof(g_user_stack), 0, 0, 0);
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
    /* Polls actual task state rather than spinning a fixed guessed count of
     * yields -- see cmd_taskdemo()'s comment (this same fix, applied
     * earlier) for why a fixed count assumes every yield here hands off
     * directly to the new task, which round-robin among other READY tasks
     * (p9srv above all) does not guarantee. A safety cap still bounds this
     * in case a probe somehow never reaches usys_exit(). */
    for (int i = 0; i < 10000 && sched_task_state(pid) != TASK_DEAD; i++) {
        sched_yield();
    }
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

/* B6: preemption, tested by something that cannot work without it.
 *
 * The spinner never yields. The waiter never yields either. Under cooperative
 * scheduling whichever starts first runs to completion and the other never
 * advances -- so if the flag is ever observed set, a timer interrupt must have
 * switched tasks at an arbitrary instruction. That is the whole claim, and it
 * is not something a passing "tasks interleave" test could already cover:
 * taskdemo's tasks yield explicitly.
 *
 * Both loops are bounded, so a failure reports rather than wedging the
 * machine and taking every later test down with it. */
static volatile uint32_t g_preempt_flag;

static void preempt_spinner(void *arg) {
    (void)arg;
    for (volatile uint32_t i = 0; i < 300000u; i++) { /* no yield, on purpose */ }
    g_preempt_flag = 1;
}

static void cmd_preempttest(void) {
    if (!ticker_enabled()) {
        cprintf("[Preempt] No preemption timer on this build -- cannot test.\n");
        return;
    }
    g_preempt_flag = 0;
    uint64_t t0 = ticker_ticks();

    if (task_create("spinner", preempt_spinner, NULL) < 0) {
        cprintf("[Preempt] Could not create the spinner task\n");
        return;
    }

    /* Deliberately no sched_yield() in this loop. */
    volatile uint64_t spins = 0;
    while (!g_preempt_flag && spins < 400000000ULL) spins++;

    uint64_t ticks = ticker_ticks() - t0;
    cprintf("[Preempt] ticks=%lu flag=%u -- %s\n",
            (unsigned long)ticks, (unsigned)g_preempt_flag,
            g_preempt_flag ? "PREEMPTED (a task ran without anyone yielding)"
                           : "NOT PREEMPTED");
}

/* M3, plan/phase12_microkernel_migration.md: proves next_runnable() picks by
 * priority tier, not merely by ring position. Four never-yielding hogs are
 * created first specifically to sit in the ring between the boot task and
 * "urgent" -- under the old plain round-robin, urgent would have to wait its
 * turn behind however many hogs came first; under tier-aware selection its
 * TASK_PRIO_INTERRUPT must win the very next reschedule regardless of how
 * many NORMAL-tier tasks are ahead of it. Both hog and urgent loops are
 * bounded, same reason as preempttest's: a failure reports rather than
 * wedging the machine.
 *
 * g_urgent_ran is uint32_t, not bool -- found the hard way while building
 * this test. A `volatile bool` flag set by urgent and polled by a
 * non-yielding busy-wait in the boot task was not reliably observed as
 * updated after the boot task was preempted mid-spin and later resumed:
 * the task genuinely ran (visible in the log) and the flag was genuinely
 * set, but the resumed spin loop kept spinning to its full bound as though
 * it never had been. Switching only the flag's type to `uint32_t` --
 * nothing else, verified by isolating every other variable one at a time --
 * made it reliable. preempttest's own g_preempt_flag already happened to be
 * uint32_t, which is presumably why B6 never hit this. Not chased to a root
 * cause in the toolchain; recorded as a real constraint instead of
 * papered over: a volatile flag polled across a preemption boundary in this
 * tree should be word-sized, not bool. */
static volatile uint32_t g_urgent_ran;
static volatile uint64_t g_urgent_woken_tick;

static void priotest_hog_body(void *arg) {
    (void)arg;
    /* No yield, on purpose -- these exist purely to occupy ring slots a
     * plain round-robin would have to cycle through, and must still be
     * READY (not already exited) at the moment urgent is unblocked, or this
     * test would pass without ever actually exercising the tie-break it
     * claims to. Bounded so they don't outlive the command by much and
     * dilute round-robin for whatever runs next. */
    for (volatile uint32_t i = 0; i < 20000000u; i++) { }
}

static void priotest_urgent_body(void *arg) {
    (void)arg;
    task_block(); /* parks until cmd_priotest() unblocks it below */
    g_urgent_woken_tick = ticker_ticks();
    g_urgent_ran = 1;
}

static void cmd_priotest(void) {
    if (!ticker_enabled()) {
        cprintf("[PrioTest] No preemption timer on this build -- cannot test.\n");
        return;
    }

    for (int i = 0; i < 4; i++) {
        if (task_create("hog", priotest_hog_body, NULL) < 0) {
            cprintf("[PrioTest] Could not create hog tasks\n");
            return;
        }
    }

    int urgent = task_create("urgent", priotest_urgent_body, NULL);
    if (urgent < 0 || task_set_priority(urgent, TASK_PRIO_INTERRUPT) < 0) {
        cprintf("[PrioTest] Could not create/prioritize the urgent task\n");
        return;
    }

    /* Let urgent actually reach its task_block() -- it must be BLOCKED, not
     * merely READY, before task_unblock() below means anything. Urgent's
     * TASK_PRIO_INTERRUPT wins it the very first of these yields regardless
     * of the hogs, so a handful is generous margin, not a real wait. */
    for (int i = 0; i < 8; i++) sched_yield();

    g_urgent_ran = 0;
    uint64_t before = ticker_ticks();
    task_unblock(urgent);

    /* Deliberately no sched_yield() here: the hogs never yield either, so
     * only the preemption timer -- and now, which tier it picks -- can give
     * urgent a chance to run. */
    volatile uint64_t spins = 0;
    while (!g_urgent_ran && spins < 400000000ULL) spins++;

    uint64_t ticks_to_run = g_urgent_ran ? (g_urgent_woken_tick - before) : 0;
    cprintf("[PrioTest] ran=%u ticks_to_run=%lu -- %s\n",
            (unsigned)g_urgent_ran, (unsigned long)ticks_to_run,
            (g_urgent_ran && ticks_to_run <= 2) ? "BOUNDED (priority won)"
                                                : "NOT BOUNDED");
}

/* M4.5, plan/phase12_microkernel_migration.md, Part A: priotest above only
 * ever proves one TASK_PRIO_INTERRUPT task against a field of NORMAL-tier
 * hogs -- it says nothing about what happens when *two* INTERRUPT-tier
 * tasks are both genuinely busy at once, which M4's driver-task
 * conversions are about to make a real, not hypothetical, situation (more
 * than one driver task plausibly wants this tier). That case has never
 * actually been exercised: `uart` is the only TASK_PRIO_INTERRUPT task
 * that has ever existed in this tree.
 *
 * Same self-measurement idea user/progs/uspin.c uses (read the tick
 * counter, spin with no yields and no syscalls, read it again), run by two
 * same-tier tasks concurrently instead of one task measuring itself in
 * isolation. Deliberately does NOT add any bookkeeping to kernel/sched.c
 * to measure this -- the whole point is to find out whether the tiers
 * already built are sufficient using only what a task can observe about
 * itself, not to build new scheduler introspection for the occasion. If
 * next_runnable()'s plain round-robin tie-break shares the CPU between two
 * equal-tier ready tasks the way it is supposed to, both should finish
 * their equal, fixed amount of work at close to the same tick -- each
 * effectively getting about half of it, interleaved. If one task-table
 * slot is favoured over the other (the specific failure shape table
 * position bias took before, when a repeatedly-reused server task's own
 * index was the scan-start -- see kernel/sched.c's next_runnable()
 * comment), the favoured task finishes quickly while the other is starved
 * until it, which shows up as a large gap between the two finish ticks
 * instead of a small one. */
static volatile uint32_t g_stress_done[2];
static volatile uint64_t g_stress_finish_tick[2];

/* Same order of magnitude as uspin.c's SPIN_ITERATIONS, not copied
 * verbatim: this runs in kernel mode (no syscall trap overhead at all,
 * unlike a U-mode program) and must comfortably span dozens of ticks even
 * when *sharing* the CPU with an equally-busy sibling, not just when
 * running alone -- calibrated on QEMU, see cmd_priostress()'s own
 * completion notes for measured values before trusting this margin on
 * real RP2350 hardware too. */
#define PRIOSTRESS_ITERATIONS 150000000L

static volatile long g_stress_sink[2]; /* per-task: keeps the optimizer from eliminating the loop below without sharing a write target between the two concurrently-running tasks */

static void priostress_body(void *arg) {
    int id = (int)(intptr_t)arg;
    /* Overwrite, not accumulate: `long` is 32 bits on rv32, and summing i
     * across PRIOSTRESS_ITERATIONS iterations overflows it almost
     * immediately -- UBSan caught this as a real fault (signed integer
     * addition overflow) the first time this ran on rv32. Only a volatile
     * write is actually needed to keep the optimizer from eliminating the
     * loop; the value itself was never meaningful. */
    for (long i = 0; i < PRIOSTRESS_ITERATIONS; i++) g_stress_sink[id] = i;
    g_stress_finish_tick[id] = ticker_ticks();
    g_stress_done[id] = 1;
}

static void cmd_priostress(void) {
    if (!ticker_enabled()) {
        cprintf("[PrioStress] No preemption timer on this build -- cannot test.\n");
        return;
    }

    g_stress_done[0] = 0;
    g_stress_done[1] = 0;
    g_stress_finish_tick[0] = 0;
    g_stress_finish_tick[1] = 0;

    int a = task_create("stressA", priostress_body, (void *)(intptr_t)0);
    int b = task_create("stressB", priostress_body, (void *)(intptr_t)1);
    if (a < 0 || b < 0 ||
        task_set_priority(a, TASK_PRIO_INTERRUPT) < 0 ||
        task_set_priority(b, TASK_PRIO_INTERRUPT) < 0) {
        cprintf("[PrioStress] Could not create/prioritize both stress tasks\n");
        return;
    }

    uint64_t start = ticker_ticks();

    /* Bounded the same way preempttest/priotest are: a failure reports
     * rather than wedging the machine. Generous relative to either task's
     * own expected duration -- this is a safety cap on the *test*, not
     * part of the fairness measurement itself, which is the tick gap
     * computed below. */
    for (int i = 0; i < 20000 &&
                    (sched_task_state(a) != TASK_DEAD || sched_task_state(b) != TASK_DEAD);
         i++) {
        sched_yield();
    }

    uint64_t total_a = g_stress_done[0] ? g_stress_finish_tick[0] - start : 0;
    uint64_t total_b = g_stress_done[1] ? g_stress_finish_tick[1] - start : 0;
    uint64_t lo = total_a < total_b ? total_a : total_b;
    uint64_t hi = total_a < total_b ? total_b : total_a;
    /* Fair sharing: both finish within a small factor of each other (some
     * skew is expected -- they don't start in exactly the same tick, and
     * ties still resolve by scan order). One task starved until the other
     * finishes looks like hi ~= 2*lo (the starved task waits out ~all of
     * the other's run, then does its own on top) -- comfortably outside
     * this bound. */
    bool fair = g_stress_done[0] && g_stress_done[1] && lo > 0 && hi <= lo + lo / 2;
    cprintf("[PrioStress] done=%u,%u total_ticks=%lu,%lu -- %s\n",
            (unsigned)g_stress_done[0], (unsigned)g_stress_done[1],
            (unsigned long)total_a, (unsigned long)total_b,
            fair ? "FAIR (both same-tier tasks shared the CPU)"
                 : "UNFAIR (one task-table slot starved the other)");
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
     * actually exited, not for a fixed guessed number of turns -- a fixed
     * count assumes every yield from here hands off directly to demoA/demoB,
     * which round-robin among other READY tasks (p9srv above all) does not
     * guarantee. With a fixed count, "Done" could fire while both tasks were
     * still alive and mid-exchange, reporting pages still legitimately in
     * use by their live stacks as though they'd leaked. A safety cap still
     * bounds this -- if task creation somehow left one of them permanently
     * unschedulable, this must not hang the shell. */
    for (int spin = 0;
         spin < 10000 &&
         (sched_task_state(a) != TASK_DEAD || sched_task_state(b) != TASK_DEAD);
         spin++) {
        sched_yield();
    }

    uint32_t after_total = 0, after_free = 0;
    palloc_stats(&after_total, &after_free);
    printk("[TaskDemo] Done. Heap pages free before=%u after=%u\n",
           before_free, after_free);
}

/* M0, plan/phase12_microkernel_migration.md: proves task_create_sized() works
 * at a stack size other than TASK_STACK_PAGES, and that palloc_free() is
 * called for exactly the number of pages that were actually allocated (not
 * TASK_STACK_PAGES, which sched.c's task_exit()/sched_reap() path used to be
 * able to assume back when every task had the same size). One page is
 * enough for this body -- one printk/sched_yield frame, nothing recursive. */
static void sizedtaskdemo_body(void *arg) {
    (void)arg;
    printk("[SizedTaskDemo] Running on a 1-page stack\n");
    sched_yield();
}

static void cmd_sizedtaskdemo(void) {
    uint32_t before_total = 0, before_free = 0;
    palloc_stats(&before_total, &before_free);

    int pid = task_create_sized("sized1", sizedtaskdemo_body, NULL, 1);
    if (pid < 0) {
        printk("[SizedTaskDemo] Could not create the task\n");
        return;
    }

    /* Drive it to completion the same way cmd_taskdemo() does -- see that
     * function's comment for why this polls sched_task_state() rather than
     * spinning a fixed count of yields. before_free/after_free matching
     * (asserted by tests/runner.py) is the actual proof that a non-default
     * page count was both allocated and freed correctly -- a leak here
     * would show up as a page short on the "after" count. */
    for (int spin = 0; spin < 10000 && sched_task_state(pid) != TASK_DEAD; spin++) {
        sched_yield();
    }

    uint32_t after_total = 0, after_free = 0;
    palloc_stats(&after_total, &after_free);
    printk("[SizedTaskDemo] Done. Heap pages free before=%u after=%u\n",
           before_free, after_free);
}

/* M1, plan/phase12_microkernel_migration.md: exercises balloc_alloc()/
 * balloc_free() -- rounding to the next power of two, self-alignment to
 * that size (Rule 6: this is what makes a block usable as a PMP region
 * later), and coalescing all the way back to one arena-sized free block
 * after a mixed-size churn. A leftover fragment at the end would mean
 * split or merge lost track of a block somewhere in the run. */
static void cmd_ballocdemo(void) {
    uint32_t arena_bytes = 0, free_before = 0;
    balloc_stats(&arena_bytes, &free_before);

    void *a = balloc_alloc(33);   /* rounds up to 64 B */
    void *b = balloc_alloc(65);   /* rounds up to 128 B */
    void *c = balloc_alloc(1025); /* rounds up to 2048 B */
    if (!a || !b || !c) {
        printk("[BAllocDemo] Allocation failed\n");
        return;
    }
    bool aligned = ((uintptr_t)a % 64) == 0 &&
                  ((uintptr_t)b % 128) == 0 &&
                  ((uintptr_t)c % 2048) == 0;
    printk("[BAllocDemo] Rounding/alignment: %s\n", aligned ? "OK" : "FAIL");

    balloc_free(a);
    balloc_free(b);
    balloc_free(c);

    /* Churn: repeatedly allocate and free a mix of sizes across a fixed set
     * of slots, so split and merge are stressed together rather than one
     * pair at a time. */
    static const uint32_t sizes[8] = {32, 96, 200, 500, 1500, 4000, 100, 60};
    void *slots[8] = {0};
    for (int round = 0; round < 50; round++) {
        for (int i = 0; i < 8; i++) {
            if (slots[i]) {
                balloc_free(slots[i]);
                slots[i] = NULL;
            } else {
                slots[i] = balloc_alloc(sizes[(i + round) % 8]);
            }
        }
    }
    for (int i = 0; i < 8; i++) {
        if (slots[i]) { balloc_free(slots[i]); slots[i] = NULL; }
    }

    uint32_t free_after = 0;
    balloc_stats(NULL, &free_after);
    printk("[BAllocDemo] Done. After churn: largest=%u arena=%u\n",
           free_after, arena_bytes);
}

/* `e`'s edit buffer used to be a fixed 2 KB stack array (bug: init.lisp is
 * 4378 bytes and silently lost its tail). Heap-on-demand instead, sized to
 * the file it is about to load rather than a guess -- room for the file to
 * roughly double under editing, rounded up to a page, with a floor for new
 * files. Refuses outright on allocation failure rather than falling back to
 * a smaller buffer that would just reintroduce the truncation. */
#define EDITOR_MIN_CAPACITY  8192u
static void shell_run_editor(const char *filename) {
    vfs_stat_t st;
    uint32_t existing = (vfs_stat(filename, &st) == 0) ? st.size : 0;
    uint32_t want = existing * 2;
    if (want < EDITOR_MIN_CAPACITY) want = EDITOR_MIN_CAPACITY;

    uint32_t pages = (want + (uint32_t)PAGE_SIZE - 1) / (uint32_t)PAGE_SIZE;
    char *edit_buf = (char *)palloc_pages(pages);
    if (!edit_buf) {
        cprintf("e: no memory for a %u KB edit buffer (file is %u bytes)\n",
                pages * (uint32_t)PAGE_SIZE / 1024, existing);
        return;
    }

    int mlen = edit_multiline_box(filename, edit_buf, (int)(pages * PAGE_SIZE));
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
    palloc_free(edit_buf, pages);
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
    } else if (strcmp(cmd_line, "reboot") == 0) {
        /* A builtin, and its position in this chain is the point: it is
         * handled here, before the Lisp fallthrough, so it still works when
         * the evaluator has stopped answering. That is exactly the state the
         * hardware suite's node-pool test leaves the board in, and rebooting
         * out of it is the whole reason this command exists (C5). */
        cprintf("[Reboot] Restarting...\n");
        if (!board_reset()) {
            cprintf("reboot: not supported on this target\n");
        }
        return;
    } else if (strcmp(cmd_line, "clear") == 0) {
        uart_puts("\033[2J\033[H");
        return;
#if CONFIG_ENABLE_ED
    } else if (strcmp(cmd_line, "ed") == 0) {
        ed_main(NULL);
        return;
    } else if (strncmp(cmd_line, "ed ", 3) == 0) {
        ed_main(&cmd_line[3]);
        return;
#endif
    } else if (strcmp(cmd_line, "e") == 0) {
        shell_run_editor("/ram0/system/scratch.lisp");
        return;
    } else if (strncmp(cmd_line, "e ", 2) == 0) {
        const char *fn = &cmd_line[2];
        while (*fn == ' ') fn++;
        shell_run_editor(fn);
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
    } else if (strcmp(cmd_line, "preempttest") == 0) {
        cmd_preempttest();
        return;
    } else if (strcmp(cmd_line, "priotest") == 0) {
        cmd_priotest();
        return;
    } else if (strcmp(cmd_line, "priostress") == 0) {
        cmd_priostress();
        return;
    } else if (strcmp(cmd_line, "taskdemo") == 0) {
        cmd_taskdemo();
        return;
    } else if (strcmp(cmd_line, "sizedtaskdemo") == 0) {
        cmd_sizedtaskdemo();
        return;
    } else if (strcmp(cmd_line, "ballocdemo") == 0) {
        cmd_ballocdemo();
        return;
    } else if (strcmp(cmd_line, "uartstats") == 0) {
        /* M4 verify, plan/phase12_microkernel_migration.md: exposes
         * uart_write_call_count() so a test can confirm console output
         * generates chan_call() traffic on the order of messages/lines,
         * not characters -- the property the batching redesign exists to
         * guarantee. */
        printk("[UartStats] write_calls=%u\n", uart_write_call_count());
        return;
    } else if (strcmp(cmd_line, "blkstats") == 0) {
        /* M4.5 verify, plan/phase12_microkernel_migration.md, Part B:
         * exposes blk_task_call_count() so a test can confirm the sdblk/blk
         * task is genuinely serving requests (a nonzero, growing count)
         * rather than every caller silently using the direct-hardware
         * fallback the whole time. */
        printk("[BlkStats] calls=%u\n", blk_task_call_count());
        return;
    } else if (strcmp(cmd_line, "i2cstats") == 0) {
        /* M4.5 verify, plan/phase12_microkernel_migration.md, Part B:
         * exposes i2c_task_call_count() so a test can confirm the shared
         * RTC/EEPROM task is genuinely serving requests, same reasoning as
         * blkstats above. */
        printk("[I2cStats] calls=%u\n", i2c_task_call_count());
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

    /* A bare word that names a program on the search path runs it (C1).
     *
     * Precedence is builtins, then the path, then Lisp -- and the order is
     * the interesting part:
     *
     *   Builtins first, so that until `cc` and `e` actually become
     *   executables (C6/C7) the compiled-in versions keep working. When a
     *   builtin is replaced by a binary its `else if` arm has to go in the
     *   same change, or the builtin silently shadows the new file and the
     *   extraction looks like it did nothing.
     *
     *   The path before Lisp, because at a shell prompt a bare word means
     *   "run this". The cost is that a program shadows a Lisp variable of the
     *   same name, which is the usual shell trade and only bites if someone
     *   ships a binary named after their variable.
     *
     *   Anything containing a '/' is skipped entirely: a path typed out in
     *   full is an instruction to run exactly that file, never a name to be
     *   searched for. That is what makes a specific build always reachable
     *   even when a higher-priority volume shadows its name.
     */
    if (cmd_line[0] != '(') {
        /* Split off the first word and treat the rest as arguments (C3).
         * argv[0] is the name as typed, which is what a program expects and
         * what the loader deliberately does not invent for itself. */
        char argbuf[256];
        char *argp[USER_ARGV_LIMIT];
        int argn = 0;
        {
            uint32_t n = 0;
            while (cmd_line[n] && n < sizeof(argbuf) - 1) { argbuf[n] = cmd_line[n]; n++; }
            argbuf[n] = '\0';
        }
        char *q = argbuf;
        while (*q && argn < USER_ARGV_LIMIT) {
            while (*q == ' ' || *q == '\t') *q++ = '\0';
            if (!*q) break;
            argp[argn++] = q;
            while (*q && *q != ' ' && *q != '\t') q++;
        }

        bool name_has_slash = false;
        for (const char *c = argn ? argp[0] : ""; *c; c++) {
            if (*c == '/') { name_has_slash = true; break; }
        }

        if (argn > 0 && !name_has_slash) {
            char prog[128];
            if (path_resolve("bin", argp[0], ".elf", prog, sizeof(prog)) == 0) {
                elf_load_and_run_argv(prog, argn, (const char *const *)argp);
                return;
            }
        }
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
        /* A bare name that resolves to something callable (a primitive
         * or a user-defined lambda) rather than a plain value -- found
         * live, not theoretical: a zero-argument command with no
         * dedicated shell alias (e.g. `chess-console`) evaluated as a
         * bare symbol lookup and printed the closure object itself
         * (`=> <#primitive>`) instead of ever running, silently doing
         * nothing. Standard Lisp semantics (a bare symbol is just a
         * reference, not a call) are exactly right for a plain value --
         * typing a variable's name to see what it holds is a real,
         * intentional REPL feature this must not break -- but for
         * something callable the user is at a *shell* prompt and almost
         * certainly meant to invoke it, the same as every other bare
         * command does via the POSIX->S-expr wrap below. The lookup
         * above has no side effects (it never called anything, only
         * looked up the binding), so re-evaluating as a proper zero-
         * argument call is safe -- nothing runs twice. */
        if (res && (res->type == LISP_PRIMITIVE || res->type == LISP_LAMBDA)) {
            char call[600];
            int n = 0;
            call[n++] = '(';
            for (const char *p = cmd_line; *p && n < (int)sizeof(call) - 2; p++) {
                call[n++] = *p;
            }
            call[n++] = ')';
            call[n] = '\0';
            res = lisp_eval_string(call);
        }
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


