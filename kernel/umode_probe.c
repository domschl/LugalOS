/* Verifies that hardware isolation actually
 * isolate, on this board, today?
 *
 * The question is not answered by a driver running in U-mode. A driver that
 * works proves its *allowed* accesses are allowed; it says nothing about
 * whether a forbidden one would be refused, and that is the half the whole
 * mechanism exists for. This runs both directions in one task:
 *
 *   1. enter U-mode under a three-region domain,
 *   2. write to memory the domain grants, and record that it worked,
 *   3. touch memory the domain grants to nobody.
 *
 * Step 3 must fault. The kernel must then kill the task and keep running --
 * `trap_handler()`'s `from_user` path, which is B3's whole argument: "a
 * kernel that halts whenever a user task misbehaves has enforcement but no
 * benefit from it".
 *
 * The result is read back out of the granted block afterwards, which is the
 * only channel available: a U-mode task on this board cannot printk(), cannot
 * take a lock, and by construction cannot reach any kernel variable that is
 * not in its domain.
 *
 * ## Why this file has its own compile flags
 *
 * Everything tagged UPROBE_UATTR is compiled into `.utext` -- the one page
 * `board_text_region()` grants U-mode execute on -- and this whole
 * translation unit is built with `-fno-jump-tables` (CMakeLists.txt). That is
 * not caution, GCC compiles a switch
 * with enough cases into a jump table stored in ordinary `.rodata`, which is
 * outside every granted region, and the disassembly check that greps for
 * `jal` targets does not catch it because the jump goes through a loaded
 * pointer. Converting the switch to if/else does not help -- GCC rebuilds the
 * same table from the comparison chain. The fix has to be structural.
 *
 * No string literals in U-mode code either, for the same reason: a literal
 * lands in .rodata, outside the domain. Any text is built character by
 * character into a volatile array on the granted stack.
 */

#include "kernel/sched.h"
#include "kernel/printk.h"
#include "kernel/mem_domain.h"
#include "kernel/device.h"
#include "kernel/ipc.h"
#include "kernel/printk.h"
#include "kernel/umode_probe.h"
#include "arch/umode.h"
#include "lugalos_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define UPROBE_UATTR __attribute__((section(".utext"))) \
                     __attribute__((no_sanitize("undefined")))

/* Region sizes, per backend, because "the smallest region the hardware can
 * express" is a different number on each and getting it wrong does not fail
 * loudly -- it grants something other than what was asked for, or nothing.
 *
 * **PMP (NOMMU targets).** A region is NAPOT: a naturally-aligned power of
 * two. 512 bytes for the stack and 128 for the shared block, where 128 is the
 * coarsest granularity any board here reports -- the ESP32-P4's, and measured
 * rather than assumed: `pmpinfo` prints the pmpaddr0 readback the number was
 * derived from. Anything finer would be silently rounded up by that chip into
 * a region larger than intended, which is the direction that quietly grants
 * more than was asked for.
 *
 * **Sv39 (the RV64 MMU target).** Grants are pages. A 512-byte stack cannot
 * be a page, so the first write from U-mode faults before the probe records
 * anything at all -- which is exactly what it did, `cause 15` at the first
 * store, the first time this ran there. This probe is the first kernel-side
 * U-mode task on that backend; every earlier one is a PMP-only driver, and
 * `uisolate.elf` gets page-aligned memory from the ELF loader. */
#if defined(CONFIG_NOMMU)
#define UPROBE_STACK_BYTES  512
#define UPROBE_SHARED_BYTES 128
#define UPROBE_STACK_SECTION __attribute__((section(".ustacks512")))
#else
#define UPROBE_STACK_BYTES  4096
#define UPROBE_SHARED_BYTES 4096
#define UPROBE_STACK_SECTION
#endif

/* The task's U-mode stack, in the section the linker script aligns for
 * exactly this on the PMP targets. */
static uint8_t g_uprobe_stack[UPROBE_STACK_BYTES]
    __attribute__((aligned(UPROBE_STACK_BYTES)))
    UPROBE_STACK_SECTION;

/* The one data grant, and the only thing the U-mode half can say to the
 * kernel half. */
/* accepted: the probe's one grant, written by its U-mode half and read by the
 * kernel half; one probe at a time. */
static volatile uint8_t g_uprobe_shared[UPROBE_SHARED_BYTES]
    __attribute__((aligned(UPROBE_SHARED_BYTES)));

/* Offsets within the shared block, written by U-mode and read by the kernel.
 * Indices rather than a struct: a struct would be fine, but plain byte
 * offsets make it obvious that nothing here is a pointer. */
#define UPROBE_MARK_ENTERED   0   /* got into U-mode at all            */
#define UPROBE_MARK_STACK_OK  1   /* wrote and read back its own stack */
#define UPROBE_MARK_GRANT_OK  2   /* wrote and read back this block    */
#define UPROBE_MARK_ABOUT_TO  3   /* about to make the illegal access  */
#define UPROBE_MARK_SURVIVED  4   /* still running after it -- a FAIL  */

static mem_domain_t g_uprobe_domain;
/* accepted: a selftest's state; one run at a time. */
static int          g_uprobe_pid = -1;

/* An address inside RAM that the domain does not grant.
 *
 * Deliberately not NULL and not a device register. A null dereference can
 * fault for reasons that have nothing to do with PMP (no mapping at all), and
 * an MMIO address can fault as a bus error instead -- either would let a
 * board with PMP switched off pass this test. The kernel's own .bss is
 * ordinary readable RAM that the kernel touches constantly, so a refusal
 * there can only be the domain doing its job. */
extern char _bss_start[];

/* Set by the kernel half before the jump, read by the U-mode half. Lives in
 * the granted block rather than in a global, because a global would be a
 * kernel address the domain does not cover -- the exact mistake this file is
 * built to catch. Byte offsets 8..11 of the shared block. */
#define UPROBE_FORBIDDEN_SLOT 8
/* The read probe's word, bytes 16..19, little-endian. */
#define UPROBE_READ_VALUE 16

UPROBE_UATTR static void uprobe_umode_body(void) {
    volatile uint8_t *shared = g_uprobe_shared;

    shared[UPROBE_MARK_ENTERED] = 1;

    /* Its own stack: a local, written and read back. If the stack grant were
     * wrong this would already have faulted, before anything was recorded. */
    volatile uint8_t local[8];
    for (int i = 0; i < 8; i++) local[i] = (uint8_t)(i + 1);
    uint8_t sum = 0;
    for (int i = 0; i < 8; i++) sum = (uint8_t)(sum + local[i]);
    if (sum == 36) shared[UPROBE_MARK_STACK_OK] = 1;

    /* The granted block, both directions. */
    shared[64] = 0xA5;
    if (shared[64] == 0xA5) shared[UPROBE_MARK_GRANT_OK] = 1;

    /* And now the point of the whole exercise. The address came from the
     * kernel half, through the granted block -- reassembled byte by byte
     * rather than read as a word, because the block is a byte array and a
     * misaligned word load would fault for a reason that is not the one
     * being tested. */
    shared[UPROBE_MARK_ABOUT_TO] = 1;
    uintptr_t addr = 0;
    for (int i = 3; i >= 0; i--) {
        addr = (addr << 8) | (uintptr_t)shared[UPROBE_FORBIDDEN_SLOT + i];
    }
    volatile uint8_t *forbidden = (volatile uint8_t *)addr;
    *forbidden = 0x5A;

    /* Not reached on a board whose PMP works. If it is reached, the write
     * above was allowed, and saying so is the entire value of this line. */
    shared[UPROBE_MARK_SURVIVED] = 1;
    for (;;) { }
}

/* The read probe: one 32-bit load from the address in the forbidden slot, the word recorded, then a
 * clean exit. Whether it faults, reads zeros or reads what the kernel reads is the answer. */
UPROBE_UATTR static void uprobe_read_body(void) {
    volatile uint8_t *shared = g_uprobe_shared;
    shared[UPROBE_MARK_ENTERED] = 1;
    uintptr_t addr = 0;
    for (int i = 3; i >= 0; i--) {
        addr = (addr << 8) | (uintptr_t)shared[UPROBE_FORBIDDEN_SLOT + i];
    }
    shared[UPROBE_MARK_ABOUT_TO] = 1;
    uint32_t v = *(volatile uint32_t *)addr;
    for (int i = 0; i < 4; i++) shared[UPROBE_READ_VALUE + i] = (uint8_t)(v >> (8 * i));
    shared[UPROBE_MARK_SURVIVED] = 1;
    register long a0 __asm__("a0") = SYS_UEXIT;
    register long a1 __asm__("a1") = 0;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1) : "memory");
    for (;;) { }
}

/* accepted: set by the kernel half before the probe task starts, read by that task; one probe at a
 * time (g_uprobe_busy). */
static void (*g_uprobe_entry)(void);
/* guarded by: itself, an atomic exchange: the write probe (the shell) and the read probe (/proc/pmp,
 * on the 9P task) share the stack, the block and the domain. */
static int g_uprobe_busy;

/* Kernel-mode half: build the domain, then make the one-way jump.
 *
 * Same shape as driver tasks (drivers/tm1638_rp2350.c is the
 * smallest), including the refusal: if task_set_domain() cannot enforce the
 * domain, this does *not* enter U-mode and pretend. An unenforced "U-mode"
 * task would sail through the illegal access and report a pass. */
static void uprobe_task_body(void *arg) {
    (void)arg;

    /* A static domain reused by every run: the previous run's page tables
     * are returned first . mem_domain_init() zeroes the
     * struct, so on rv64 it used to drop them -- four pages a run, 14 -> 30
     * over four `usertest`s. Refused if a live task is still in it. */
    if (sched_domain_in_use(&g_uprobe_domain)) {
        printk("[UProbe] Refusing: the previous probe's task is still running\n");
        return;
    }
    mem_domain_destroy(&g_uprobe_domain);
    mem_domain_init(&g_uprobe_domain);
    mem_domain_add(&g_uprobe_domain, (uintptr_t)g_uprobe_stack,
                   sizeof(g_uprobe_stack), MEM_R | MEM_W);

    uintptr_t tbase, tsize;
    board_text_region(&tbase, &tsize);
    mem_domain_add(&g_uprobe_domain, tbase, tsize, MEM_R | MEM_X);

    mem_domain_add(&g_uprobe_domain, (uintptr_t)g_uprobe_shared,
                   sizeof(g_uprobe_shared), MEM_R | MEM_W);

    /* The address the U-mode half will try to touch, chosen here where the
     * domain can be interrogated rather than guessed at.
     *
     * `_bss_start` is ordinary RAM the kernel writes constantly, so a refusal
     * there can only be the domain doing its job -- unlike NULL (which faults
     * for want of any mapping) or an MMIO register (which can fault as a bus
     * error), either of which would let a board with PMP switched off pass
     * this test. Verified against the domain rather than assumed, because
     * g_uprobe_shared also lives in .bss and could in principle sit at its
     * start. */
    uintptr_t forbidden = (uintptr_t)_bss_start;
    if (mem_domain_permits(&g_uprobe_domain, forbidden, 1, MEM_W)) {
        forbidden += 4096;
    }
    if (mem_domain_permits(&g_uprobe_domain, forbidden, 1, MEM_W)) {
        printk("[UProbe] Could not find an address outside the domain to test with.\n");
        return;
    }
    if (g_uprobe_entry == uprobe_read_body) {
        forbidden = 0;                       /* the read probe's address is already in the block */
        for (int i = 3; i >= 0; i--) forbidden = (forbidden << 8) | g_uprobe_shared[UPROBE_FORBIDDEN_SLOT + i];
    }
    for (int i = 0; i < 4; i++) {
        g_uprobe_shared[UPROBE_FORBIDDEN_SLOT + i] =
            (uint8_t)((forbidden >> (8 * i)) & 0xffu);
    }

    if (task_set_domain(sched_current_pid(), &g_uprobe_domain) != 0) {
        printk("[UProbe] Refusing to enter U-mode: the domain is not enforceable "
               "on this build, so the test could not fail even if PMP were off.\n");
        return;
    }

    arch_enter_user(g_uprobe_entry,
                    (uintptr_t)g_uprobe_stack + sizeof(g_uprobe_stack), 0, 0, 0);
}

void umode_probe_run(void) {
    if (!mem_domain_enforced()) {
        printk("[UProbe] No enforced memory domains on this build -- nothing to test.\n");
        return;
    }
    if (__atomic_exchange_n(&g_uprobe_busy, 1, __ATOMIC_ACQUIRE)) {
        printk("[UProbe] Another probe is running\n");
        return;
    }

    for (uint32_t i = 0; i < UPROBE_SHARED_BYTES; i++) g_uprobe_shared[i] = 0;

    g_uprobe_entry = uprobe_umode_body;
    g_uprobe_pid = task_create("uprobe", uprobe_task_body, NULL);
    if (g_uprobe_pid < 0) {
        printk("[UProbe] Could not create the probe task\n");
        __atomic_store_n(&g_uprobe_busy, 0, __ATOMIC_RELEASE);
        return;
    }

    /* Bounded, like preempttest and priostress: a failure reports rather than
     * wedging the machine. The task is expected to die -- killed by the fault
     * handler -- so waiting for TASK_DEAD is waiting for the pass. */
    for (int i = 0; i < 200000 && sched_task_state(g_uprobe_pid) != TASK_DEAD; i++) {
        sched_yield();
    }

    bool entered  = g_uprobe_shared[UPROBE_MARK_ENTERED]  != 0;
    bool stack_ok = g_uprobe_shared[UPROBE_MARK_STACK_OK] != 0;
    bool grant_ok = g_uprobe_shared[UPROBE_MARK_GRANT_OK] != 0;
    bool tried    = g_uprobe_shared[UPROBE_MARK_ABOUT_TO] != 0;
    bool survived = g_uprobe_shared[UPROBE_MARK_SURVIVED] != 0;
    bool dead     = sched_task_state(g_uprobe_pid) == TASK_DEAD;

    printk("[UProbe] entered=%d own_stack=%d granted_block=%d attempted=%d "
           "survived=%d killed=%d\n",
           entered, stack_ok, grant_ok, tried, survived, dead);

    if (entered && stack_ok && grant_ok && tried && !survived && dead) {
        printk("[UProbe] PASS: U-mode ran under its domain, and the access "
               "outside it was refused and the task killed.\n");
    } else if (survived) {
        printk("[UProbe] FAIL: the out-of-bounds write was ALLOWED. This board "
               "is not isolating anything.\n");
    } else if (!entered) {
        printk("[UProbe] FAIL: never reached U-mode.\n");
    } else {
        printk("[UProbe] FAIL: incomplete -- see the flags above.\n");
    }
    __atomic_store_n(&g_uprobe_busy, 0, __ATOMIC_RELEASE);
}

int umode_probe_read(uintptr_t addr, uint32_t *value) {
    if (!mem_domain_enforced()) return -1;
    if (__atomic_exchange_n(&g_uprobe_busy, 1, __ATOMIC_ACQUIRE)) return -1;
    for (uint32_t i = 0; i < UPROBE_SHARED_BYTES; i++) g_uprobe_shared[i] = 0;
    for (int i = 0; i < 4; i++) g_uprobe_shared[UPROBE_FORBIDDEN_SLOT + i] = (uint8_t)(addr >> (8 * i));
    g_uprobe_entry = uprobe_read_body;
    int rc = -1;
    g_uprobe_pid = task_create("uprobe", uprobe_task_body, NULL);
    if (g_uprobe_pid >= 0) {
        for (int i = 0; i < 200000 && sched_task_state(g_uprobe_pid) != TASK_DEAD; i++) sched_yield();
        if (sched_task_state(g_uprobe_pid) == TASK_DEAD && g_uprobe_shared[UPROBE_MARK_ABOUT_TO]) {
            rc = g_uprobe_shared[UPROBE_MARK_SURVIVED] ? 1 : 0;
            uint32_t v = 0;
            for (int i = 3; i >= 0; i--) v = (v << 8) | g_uprobe_shared[UPROBE_READ_VALUE + i];
            if (value) *value = v;
        }
    }
    __atomic_store_n(&g_uprobe_busy, 0, __ATOMIC_RELEASE);
    return rc;
}

#if defined(CONFIG_BOARD_ESP32C6)
/* What a U-mode task with no grant for them reaches, read by the kernel and then from U-mode: the
 * machine-mode interrupt controller and timer (in the CPU subsystem, 0x20000000-0x2fffffff), two
 * peripherals no driver domain is granted, and kernel RAM as the control (must fault). An old ESP-IDF
 * bootloader (the C6's factory one) leaves PMP entries locked that grant the first two ranges to
 * U-mode; no later entry can take a lower-numbered locked grant back. */
uint32_t umode_reach_render(char *buf, uint32_t cap) {
    static const struct { const char *name; uintptr_t addr; } probes[] = {
        { "plic_m_enable", 0x20001000u },   /* PLIC_MXINT_ENABLE */
        { "clint_m_mtime", 0x20001808u },   /* CLINT_MINT_MTIME_L */
        { "intmtx",        0x60010000u },   /* the interrupt matrix's first map register */
        { "gpio_in",       0x6009103Cu },   /* GPIO_IN */
    };
    uint32_t n = (uint32_t)ksnprintf(buf, cap, "umode reach (kernel read / U-mode read):\n");
    for (unsigned i = 0; i <= sizeof(probes) / sizeof(probes[0]) && n + 80 < cap; i++) {
        const char *name = i < sizeof(probes) / sizeof(probes[0]) ? probes[i].name : "kernel_ram";
        uintptr_t a = i < sizeof(probes) / sizeof(probes[0]) ? probes[i].addr : (uintptr_t)_bss_start + 4096;
        uint32_t m = *(volatile uint32_t *)a, u = 0;
        int rc = umode_probe_read(a, &u);
        const char *verdict = rc < 0 ? "not run" : rc == 0 ? "blocked (fault)"
                            : (u == 0 && m != 0) ? "zeros (filtered on the bus?)" : "REACHED";
        n += (uint32_t)ksnprintf(buf + n, cap - n, "  %-14s %08lx  m=%08lx u=%08lx  %s\n", name,
                                 (unsigned long)a, (unsigned long)m, rc == 1 ? (unsigned long)u : 0ul, verdict);
    }
    return n;
}
#endif
