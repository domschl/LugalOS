#include "arch/trap.h"
#include "arch/csr.h"
#include "kernel/printk.h"
#include "kernel/hart.h"
#include "kernel/console.h"
#include "kernel/ipc.h"
#include "kernel/chan.h"
#include "kernel/sched.h"
#include "kernel/uaccess.h"
#include "kernel/ticker.h"
#include "kernel/devirq.h"
#include "kernel/time.h"
#include <stdbool.h>
#include "fs/vfs.h"

#if defined(CONFIG_BOARD_RP2350)
#include "drivers/usb_cdc.h"
#include "arch/rp2350_bootrom.h"
#define REG(addr) (*(volatile uint32_t *)(addr))
#endif

/* M5 Phase 5, plan/phase12_microkernel_migration.md: SYS_CHAN_SERVE_WAIT/
 * _REPLY's own kernel-side scratch buffer, shared by every U-mode driver
 * task's serve loop (SYS_CHAN_SERVE_WAIT/_REPLY cases below). Grown from
 * the original 256 bytes (M5 Phase 2) because drivers/spisd_rp2350.c's
 * blk moves whole 512-byte SD sectors -- even a single sector already
 * overflows 256 bytes before counting its 9-byte header. Sized to one
 * sector (512) plus 64 bytes of header/opcode headroom, comfortably
 * covering blk's 521-byte request (BLK_HDR_LEN 9 + 512) and every other
 * driver's much smaller messages -- not sized past a single block
 * transfer, since drivers/spisd_rp2350.c's own chunking keeps every wire
 * message capped at one sector regardless of caller-requested count. */
#define CHAN_SERVE_KBUF_CAP 576

/* M2, plan/phase12_microkernel_migration.md: identifying which external IRQ
 * fired is genuinely different hardware on each target -- there is no Rule 0
 * to apply here, only real controllers. What sits above this (devirq.h) is
 * arch-independent; this is the seam.
 *
 * The discriminator is CONFIG_BOARD_RP2350, deliberately NOT CONFIG_MODE_M:
 * QEMU RV32 is also CONFIG_MODE_M (a plain M-mode target, no S-mode
 * transition -- see entry.S), but it is not RP2350 and has none of Hazard3's
 * Xh3irq CSRs. Conflating "M-mode" with "Hazard3" here is exactly the
 * mistake that would make QEMU hide a real RP2350 divergence in the other
 * direction: not "QEMU is more permissive than hardware" but "QEMU is a
 * different CPU than hardware", and an meinext read on QEMU RV32 is an
 * illegal instruction, not a hardware validation of anything. Found by
 * exactly that: RV32 hung during boot the first time this shipped gated on
 * CONFIG_MODE_M, while RV64 (CONFIG_MODE_S, never reaches this branch) was
 * unaffected -- the asymmetry was the tell. */
#if defined(CONFIG_BOARD_RP2350)
/* Hazard3's Xh3irq external interrupt array (RP2350 datasheet §3.6.7.5
 * region; register layout confirmed against the Pico SDK's own
 * hardware/regs/rvcsr.h and hardware_irq/irq.c rather than guessed --
 * exactly the kind of RP2350-specific register work this tree has gotten
 * wrong before by not doing that (mem_domain.c's erratum E6 notes).
 *
 * meinext, plain-read (no UPDATE bit): bit 31 (NOIRQ) set means nothing is
 * both pending and enabled; otherwise bits [10:2] hold the IRQ number,
 * pre-shifted by 2 for a jump table this kernel does not use. UPDATE (bit 0)
 * is what the Pico SDK's own vectored dispatch writes to advance
 * meicontext's preemption-priority floor -- deliberately never written here:
 * this kernel has one flat interrupt priority, the same way the timer
 * interrupt is already handled with no nesting, so there is no floor to
 * track. A bare read has no side effect beyond returning the current
 * pending+enabled IRQ, which is all this needs. */
static inline uint32_t meinext_irq(bool *valid) {
    uintptr_t v;
    __asm__ __volatile__("csrr %0, 0xbe4" : "=r"(v)); /* RVCSR_MEINEXT */
    *valid = (v & 0x80000000u) == 0;
    return (uint32_t)((v >> 2) & 0x1ffu);
}
#else
/* QEMU virt's PLIC -- shared by both QEMU targets, which differ only in
 * which context and cause code they see: standard PLIC convention numbers
 * hart N's M-mode context 2N and its S-mode context 2N+1, and RV64 is the
 * only target that delegates external interrupts to S-mode at all (see
 * entry.S's mideleg, CONFIG_MODE_S-only). Base, context numbering and the
 * claim/complete protocol verified against qemu/include/hw/riscv/virt.h's
 * own memory map rather than assumed, since this is the one target where
 * "standard" still deserves a source. */
#define QEMU_PLIC_BASE     0x0c000000UL
#if defined(CONFIG_MODE_S)
#define QEMU_EXT_CAUSE     9u  /* Supervisor external interrupt */
#else
#define QEMU_EXT_CAUSE     11u /* Machine external interrupt */
#endif

/* The PLIC context of the hart executing this, by the convention documented
 * above: hart N's M-mode context is 2N and its S-mode context is 2N+1.
 *
 * X1, plan/phase23_multicore_scheduling.md §6.1. This used to be the
 * constant 1 (or 0), i.e. hart 0's context, baked into three separate
 * address computations -- the claim/complete register, the priority
 * threshold written at init, and the per-IRQ enable bit. §1 of that plan
 * said the interrupt controllers were "already per-hart-addressable in
 * principle", which was true of the hardware and not of this code.
 *
 * Getting it wrong is not a missing feature but active damage: a second
 * hart claiming from hart 0's context takes an interrupt out of hart 0's
 * queue, and writing hart 0's threshold from hart 1 reconfigures a context
 * that is not its own. */
static inline uint32_t plic_context(void) {
#if defined(CONFIG_MODE_S)
    return 2u * hart_id() + 1u;
#else
    return 2u * hart_id();
#endif
}

static inline volatile uint32_t *plic_claim_reg(void) {
    return (volatile uint32_t *)
        (QEMU_PLIC_BASE + 0x200000u + plic_context() * 0x1000u + 4u);
}

static inline uint32_t plic_claim(void) {
    return *plic_claim_reg();
}
static inline void plic_complete(uint32_t irq_num) {
    *plic_claim_reg() = irq_num;
}
#endif

void trap_init(void) {
#if defined(CONFIG_BOARD_RP2350)
    /* Clear this core's external-interrupt FORCE array before enabling
     * anything (phase 23 X7).
     *
     * meifa (0xbe2) is "a read-write bit for every interrupt request; writing
     * a 1 causes the corresponding bit to become pending in meipa" -- a
     * software-triggered interrupt latch, per core, with no guaranteed reset
     * value. Transcribed from the SDK's own
     * runtime_init_per_core_h3_irq_registers (pico_crt0/crt0_riscv.S), which
     * runs this on *every* core including core 1 at launch, iterating array
     * windows 3..0 for up to 64 IRQs.
     *
     * This kernel never did it, which was survivable while only core 0
     * existed and boot_header.S had left the core in a known state. Core 1
     * comes out of the bootrom instead, having just been driven through a
     * FIFO handshake, and a stale force bit there means it takes an external
     * interrupt the instant MIE goes on -- into a handler that, for an IRQ
     * with no registered owner, printk()s. From core 1. In a storm. That is
     * a wedged board whose stopping point moves from run to run, which is
     * exactly what X7's first three attempts produced.
     *
     * Harmless on core 0, where it has always been zero; the point is that
     * "has always been" is not a property core 1 inherits. */
    for (int w = 3; w >= 0; w--) {
        __asm__ __volatile__("csrw 0xbe2, %0" :: "r"((uintptr_t)w)); /* RVCSR_MEIFA */
    }

    /* Enable M-mode External Interrupts, and only those.
     *
     * csrw rather than csrr/or/csrw, matching the SDK: it also clears MSIE
     * and MTIE, so a core arrives with exactly one interrupt source armed and
     * the timer is switched on deliberately later by ticker_arm_this_hart().
     * The read-modify-write this replaced would have carried whatever the
     * bootrom left in mie on a secondary. */
    __asm__ __volatile__("csrw mie, %0" :: "r"((uintptr_t)(1u << 11)));

    /* Enable M-mode Global Interrupts (MIE bit 3 in mstatus) */
    uintptr_t mstatus_val;
    __asm__ __volatile__("csrr %0, mstatus" : "=r"(mstatus_val));
    mstatus_val |= (1u << 3);
    __asm__ __volatile__("csrw mstatus, %0" :: "r"(mstatus_val));
#else
    /* M2, plan/phase12_microkernel_migration.md: the arch-global gate for
     * QEMU's PLIC path (both targets -- see the CONFIG_MODE_S/M split above
     * for why this is one branch, not two), mirroring ticker.c's own STIE
     * enable for the same reason: unmasking the *source* here is what makes
     * it safe for a driver's later per-IRQ PLIC enable bit to actually take
     * effect, without that driver also needing to know this CSR exists. The
     * global interrupt gate itself (sstatus.SIE / mstatus.MIE) is left alone
     * here, same as it always was -- main.c's irq_restore(IRQ_ENABLE_BIT),
     * after ticker_init(), is still what turns any of this on. */
#if defined(CONFIG_MODE_S)
    set_csr(sie, 1UL << 9);  /* SEIE */
#else
    set_csr(mie, 1UL << 11); /* MEIE -- plain M-mode QEMU RV32, not RP2350 */
#endif

    /* PLIC priority threshold for this hart's context: 0 accepts any
     * interrupt with priority > 0, i.e. every priority a driver could set
     * (arch_irq_enable() below). One flat priority level, the same choice
     * trap_handler()'s meinext path makes for RP2350 -- there is no
     * nested/preemptive interrupt priority anywhere in this kernel yet. */
    *(volatile uint32_t *)(QEMU_PLIC_BASE + 0x200000u + plic_context() * 0x1000u) = 0;
#endif
}

/* See arch/trap.h. */
void arch_irq_enable(uint32_t irq_num) {
#if defined(CONFIG_BOARD_RP2350)
    /* MEIEA (0xbe0): a window-indexed enable array. Writing INDEX (bits
     * [4:0]) selects a 16-IRQ window; the same write's WINDOW bits
     * (dependent on the CSR instruction's semantics -- csrrs here, so this
     * *sets* bits rather than replacing the whole window) OR the given mask
     * into that window's enable bits. Confirmed against the Pico SDK's
     * hazard3_irqarray_set() macro (hardware/hazard3.h) rather than derived
     * from the field description alone. */
    uint32_t index = irq_num / 16u;
    uint32_t mask  = 1u << (irq_num % 16u);
    uintptr_t v = index | ((uintptr_t)mask << 16);
    __asm__ __volatile__("csrrs zero, 0xbe0, %0" :: "r"(v)); /* RVCSR_MEIEA */
#else
    /* PLIC: give the IRQ a nonzero priority (0 permanently masks it,
     * independent of any enable bit -- the PLIC spec's own "never
     * interrupt" value) and set its per-context enable bit. Priority 1 is
     * "the lowest priority that still fires", correct for a kernel with one
     * flat interrupt level. */
    *(volatile uint32_t *)(QEMU_PLIC_BASE + 4u * irq_num) = 1;
    /* The *calling* hart's context. Device drivers all initialise on hart 0,
     * so this enables where it always did -- and phase 23's X2 pins driver
     * tasks there deliberately, so routing a device IRQ to a second hart is
     * a policy decision that belongs with X2 rather than a side effect of
     * whoever happened to call this. */
    volatile uint32_t *enable = (volatile uint32_t *)
        (QEMU_PLIC_BASE + 0x2000u + plic_context() * 0x80u + (irq_num / 32u) * 4u);
    *enable |= (1u << (irq_num % 32u));
#endif
}

/* See arch/trap.h. Not a general exception-handling mechanism -- deliberately
 * narrow, so it cannot accidentally mask a real fault. */
static volatile bool g_probe_active;
static volatile bool g_probe_faulted;

void arch_probe_begin(void) {
    g_probe_faulted = false;
    g_probe_active = true;
}

bool arch_probe_faulted(void) {
    g_probe_active = false;
    return g_probe_faulted;
}

/* Cause code of the most recent environment call. The hardware picks it by
 * the privilege level the ecall came FROM -- 8 = U-mode, 9 = S-mode,
 * 11 = M-mode -- so it is direct evidence of the level, not an inference from
 * something the kernel set itself.
 *
 * That distinction is the whole reason this exists: a U-mode task making
 * syscalls produces output identical to a kernel task making the same
 * syscalls, so "it printed" proves nothing about the mode transition. The
 * cause code does. */
static uintptr_t g_last_ecall_cause = 0;

uintptr_t arch_last_ecall_cause(void) { return g_last_ecall_cause; }

void trap_handler(trap_frame_t *frame) {
    uintptr_t cause = frame->cause;
    uintptr_t is_interrupt = cause & ((uintptr_t)1 << (__riscv_xlen - 1));
    uintptr_t code = cause & ~((uintptr_t)1 << (__riscv_xlen - 1));

    if (is_interrupt) {
        /* Timer: 7 is machine-mode, 5 is supervisor-mode. This is the
         * preemption tick. */
        if (code == 7 || code == 5) {
            ticker_count_tick();
            /* Rearm FIRST. A RISC-V timer interrupt is level-triggered off
             * mtime >= mtimecmp, so it stays pending until the comparator
             * moves -- returning without rearming does not drop a tick, it
             * re-enters the handler forever. */
            ticker_next();

            /* Switch away from whatever was running. This works through the
             * ordinary cooperative ctx_switch() rather than needing a second,
             * preemption-specific path: the full register state is already
             * saved in the trap frame on this task's kernel stack, so
             * ctx_switch() only has to preserve what a C call would. When
             * something switches back, it returns here, unwinds into the trap
             * vector, and the frame restores the interrupted task exactly.
             *
             * sched_yield() is a no-op when nothing else is runnable, so an
             * idle system just takes the tick and returns. */
            sched_yield();
            return;
        }

        /* External (device) interrupt. Same CONFIG_BOARD_RP2350 discriminator
         * as the controller code above -- not CONFIG_MODE_M, for the same
         * reason: QEMU RV32 is CONFIG_MODE_M without being RP2350. M2,
         * plan/phase12_microkernel_migration.md. */
#if defined(CONFIG_BOARD_RP2350)
        if (code == 11) {
            bool valid;
            uint32_t irq_num = meinext_irq(&valid);
            if (valid) devirq_dispatch(irq_num);
            return;
        }
#else
        if (code == QEMU_EXT_CAUSE) {
            uint32_t irq_num = plic_claim();
            if (irq_num != 0) { /* 0 means "nothing claimable" per the PLIC spec */
                devirq_dispatch(irq_num);
                plic_complete(irq_num);
            }
            return;
        }
#endif
        printk("\n[Trap] Interrupt received: code 0x%lx\n", (unsigned long)code);
    } else {
        /* If ecall (Environment Call from U-mode, S-mode, or M-mode) */
        if (code == 8 || code == 9 || code == 11) {
            g_last_ecall_cause = code;
            uintptr_t sys_nr = frame->a0;

            long ret = 0;
            switch (sys_nr) {
                case 0: /* SYS_EXIT / yield */
                    ret = 0;
                    break;
                case SYS_CHAN_CALL: {
                    /* SYS_CHAN_CALL(name, req, req_len, resp, resp_max)
                     *
                     * U-mode's route to a service (C3,
                     * plan/phase6_memory_and_processes.md). Until this existed
                     * a user program could read and write files and print, and
                     * nothing else -- so "extract a kernel subsystem into a
                     * process" had no way for the result to be reachable. That
                     * is why the old register-IPC entry points were deleted
                     * rather than merely tidied: removing four stubs would have
                     * left U-mode exactly as isolated.
                     *
                     * Every buffer crosses the boundary by copy, validated
                     * against the calling task's own domain, exactly as
                     * SYS_READ_FILE does. The endpoint is named rather than
                     * addressed by pointer or pid, so a user program cannot
                     * express a reference to something it was not given.
                     *
                     * The copies are not redundant with the ones chan_call()
                     * already performs: those protect the *endpoint* from the
                     * caller's buffer changing under it, while these are what
                     * stop the kernel dereferencing a user address at all. */
                    char kname[32];
                    static uint8_t kreq[256];
                    static uint8_t kresp[256];
                    uint32_t req_len = (uint32_t)frame->a3;
                    uint32_t resp_max = (uint32_t)frame->a5;

                    if (strncpy_from_user(kname, frame->a1, sizeof(kname)) < 0) {
                        ret = -1;
                        break;
                    }
                    if (req_len > sizeof(kreq)) { ret = -1; break; }
                    if (resp_max > sizeof(kresp)) resp_max = sizeof(kresp);
                    if (req_len && copy_from_user(kreq, frame->a2, req_len) < 0) {
                        ret = -1;
                        break;
                    }

                    chan_endpoint_t *ep = chan_lookup(kname);
                    if (!ep) { ret = -1; break; }

                    int n = chan_call(ep, kreq, req_len, kresp, resp_max);
                    if (n < 0) { ret = n; break; }
                    /* Copied out only after the call succeeded, and only as
                     * many bytes as the service actually produced. */
                    ret = (resp_max && copy_to_user(frame->a4, kresp, (uint32_t)n) < 0)
                              ? -1 : n;
                    break;
                }
                case 10: { /* SYS_PRINT(const char *) */
                    /* Copied in, never dereferenced in place. A U-mode task
                     * cannot read kernel memory, but before this it could ask
                     * the kernel to print it -- the restriction intact and
                     * entirely bypassed. */
                    char kbuf[128];
                    if (strncpy_from_user(kbuf, frame->a1, sizeof(kbuf)) < 0) {
                        ret = -1;
                        break;
                    }
                    printk("%s", kbuf);
                    ret = 0;
                    break;
                }
                case 11: /* SYS_PUTNUM */
                    /* Was printk("%ld", ...): a user program's numeric output
                     * has nothing to do with kernel diagnostics, but printk()
                     * appends everything it emits to the kernel log ring
                     * (kernel/printk.c), so a tight print loop (see
                     * tools/sd_root/prime.c, fib.c) flooded /proc/kmsg with
                     * program output instead of the log staying a kernel
                     * diagnostic stream. console_putc() writes straight to
                     * whichever wire is actually bound as the console,
                     * without touching the log ring. */
                    {
                        long val = (long)frame->a1;
                        if (val < 0) { console_putc('-'); val = -val; }
                        char digits[20];
                        int n = 0;
                        do { digits[n++] = (char)('0' + (val % 10)); val /= 10; } while (val > 0);
                        while (n > 0) console_putc(digits[--n]);
                    }
                    ret = 0;
                    break;
                case 12: /* SYS_PUTCHAR */
                    /* Takes a value, not a pointer, so it is already safe to
                     * call from U-mode. The pointer-taking syscalls below are
                     * NOT -- they still dereference user-supplied addresses
                     * directly, which is what B3's copy-in/copy-out step
                     * exists to fix. Until then, U-mode code must stick to
                     * value-only syscalls.
                     *
                     * console_putc(), not uart_putc(): the latter hard-codes
                     * the physical UART regardless of what the console is
                     * actually bound to (C8 port binding), and also bypassed
                     * the kernel log ring inconsistently with SYS_PUTNUM
                     * above. */
                    console_putc((char)frame->a1);
                    ret = 0;
                    break;
                case 21: /* SYS_TICKS: the preemption tick counter */
                    /* A value, not a pointer, and read-only -- so it is safe
                     * to expose to U-mode as it stands.
                     *
                     * It exists to make U-mode preemptibility testable from
                     * inside a user program: ticker_count_tick() is reached
                     * only from the timer interrupt handler, so a program
                     * that reads this, spins without making a single syscall,
                     * and reads it again has direct evidence of whether a
                     * timer interrupt was taken while *it* was running. With
                     * interrupts masked in U-mode the count cannot move
                     * across that window, however long the spin. */
                    ret = (long)ticker_ticks();
                    break;
                case SYS_YIELD:
                    /* No pointer, no privileged state to hand out -- a plain
                     * cooperative yield. M5, plan/phase12_microkernel_migration.md:
                     * the first syscall a U-mode *driver* task (as opposed to
                     * a one-shot user program) needs, since its serve/poll
                     * loop must yield the way every kernel-mode driver task
                     * already does via sched_yield(). Completes case 0's own
                     * apparent original intent (see that case's comment)
                     * without touching it -- a new, unambiguous number costs
                     * nothing and leaves 0's existing (if dead) behavior
                     * alone. */
                    sched_yield();
                    ret = 0;
                    break;
                case SYS_TIME_MS:
                    /* A value, not a pointer -- same shape as SYS_TICKS
                     * above, just wall-clock-ish milliseconds instead of the
                     * raw preemption-tick count. M5: what a U-mode driver
                     * task's own pacing loop (e.g. the heartbeat LED's
                     * on/off timing) needs in place of calling time_get_ms()
                     * directly. */
                    ret = (long)time_get_ms();
                    break;
                case SYS_DELAY_US:
                    /* M5 Phase 2, plan/phase12_microkernel_migration.md:
                     * SYS_TIME_MS's millisecond granularity is useless for a
                     * bit-bang protocol's microsecond-wide pulses (tm1638's
                     * ~3us clock high/low times). time_delay_us() itself
                     * reads RP2350's TIMER0 peripheral directly and, on this
                     * build, also services usb_cdc_task() inline inside its
                     * busy loop -- both kernel-only operations no U-mode
                     * domain has ever needed to be granted directly, so the
                     * delay runs here, in the kernel, rather than exposing
                     * that MMIO region to U-mode. Costs one ecall round-trip
                     * per call; for a display that updates a few times a
                     * second this is immaterial, and "coarse but honest" beats
                     * a calibrated busy-loop that would only be as reliable
                     * as its calibration (see user/progs/uspin.c's own
                     * SPIN_ITERATIONS comment for why that's fragile). */
                    time_delay_us((uint64_t)frame->a1);
                    ret = 0;
                    break;
                case SYS_CHAN_SERVE_WAIT: {
                    /* SYS_CHAN_SERVE_WAIT(name, buf, buf_max) -> request
                     * length, or -1. M5 Phase 2: the server half of the
                     * channel API SYS_CHAN_CALL's client half has had since
                     * C3 -- what a U-mode *driver* task needs to receive the
                     * request a chan_call() sent it. Blocks (via
                     * chan_serve_wait_copy()'s task_block()) exactly as a
                     * kernel-mode driver task's own chan_serve_wait() call
                     * already does; SYS_CHAN_CALL calling into a task-owned
                     * endpoint already blocks its caller inside a syscall
                     * the same way, so this is not a new risk, just the
                     * same mechanism used from the other side.
                     *
                     * The endpoint's own request buffer is kernel memory
                     * (chan_register_task()'s req_buf) -- never directly
                     * readable from U-mode, so the request is copied out
                     * through a kernel-owned scratch buffer, validated
                     * against this task's domain the same way SYS_CHAN_CALL
                     * validates its own buffers. */
                    char kname[32];
                    static uint8_t kbuf[CHAN_SERVE_KBUF_CAP];
                    if (strncpy_from_user(kname, frame->a1, sizeof(kname)) < 0) {
                        ret = -1;
                        break;
                    }
                    chan_endpoint_t *ep = chan_lookup(kname);
                    if (!ep) { ret = -1; break; }
                    uint32_t buf_max = (uint32_t)frame->a3;
                    if (buf_max > sizeof(kbuf)) buf_max = sizeof(kbuf);
                    uint32_t len = chan_serve_wait_copy(ep, kbuf, buf_max);
                    /* Truncation is a failure, not a short read -- same
                     * discipline chan_call()'s resp_max enforces on the
                     * client side. */
                    if (len > buf_max) { ret = -1; break; }
                    ret = (len && copy_to_user(frame->a2, kbuf, len) < 0)
                              ? -1 : (long)len;
                    break;
                }
                case SYS_CHAN_SERVE_REPLY: {
                    /* SYS_CHAN_SERVE_REPLY(name, buf, len) -> 0, or -1. The
                     * response is copied in through a kernel-owned scratch
                     * buffer and written into the endpoint's own response
                     * buffer by chan_serve_reply_copy(), which also wakes
                     * the blocked caller -- mirrors SYS_CHAN_CALL's own
                     * copy-in-then-hand-to-the-endpoint shape. */
                    char kname[32];
                    static uint8_t kbuf[CHAN_SERVE_KBUF_CAP];
                    if (strncpy_from_user(kname, frame->a1, sizeof(kname)) < 0) {
                        ret = -1;
                        break;
                    }
                    chan_endpoint_t *ep = chan_lookup(kname);
                    if (!ep) { ret = -1; break; }
                    uint32_t resp_len = (uint32_t)frame->a3;
                    if (resp_len > sizeof(kbuf)) { ret = -1; break; }
                    if (resp_len && copy_from_user(kbuf, frame->a2, resp_len) < 0) {
                        ret = -1;
                        break;
                    }
                    chan_serve_reply_copy(ep, kbuf, resp_len);
                    ret = 0;
                    break;
                }
                case SYS_UEXIT:
                    /* A U-mode task asking to end. task_exit() switches away
                     * and never returns, so this call does not come back here
                     * and the trap frame on this kernel stack is simply
                     * abandoned along with the task.
                     *
                     * a1 carries the program's return value. Recording it
                     * *here* rather than in task_exit() is what makes it
                     * mean something: this path is only reached by a task
                     * that asked to end, so a status recorded on it can
                     * never be confused with one from a task the fault
                     * handler killed. */
                    task_set_exit_status((long)frame->a1);
                    task_exit();
                    ret = 0; /* unreachable */
                    break;
                case SYS_REBOOT_BOOTSEL:
                    /* A U-mode task asking to reboot into BOOTSEL --
                     * drivers/usb_cdc.c's own U-mode "1200-baud touch"
                     * handler can detect the condition but cannot call
                     * rp2350_reboot_to_bootsel() itself (a bootrom
                     * ROM-table lookup and jump, privileged), the same
                     * shape SYS_UEXIT's own comment already describes.
                     * Does not return on success, same as the function
                     * it wraps; ret is only meaningful on the failure
                     * path (bootrom function not found, or a non-RP2350
                     * build, where this syscall is simply refused). */
#if defined(CONFIG_BOARD_RP2350)
                    rp2350_reboot_to_bootsel();
                    ret = -1; /* only reached if the reboot failed */
#else
                    ret = -1;
#endif
                    break;
                case 13: { /* SYS_READ_FILE: vfs_read(path, buf, max_len) */
                    char kpath[128];
                    static char kdata[512]; /* static: too big for the trap stack */
                    uint32_t want = (uint32_t)frame->a3;
                    if (want > sizeof(kdata)) want = sizeof(kdata);
                    if (strncpy_from_user(kpath, frame->a1, sizeof(kpath)) < 0) {
                        ret = -1;
                        break;
                    }
                    int n = vfs_read(kpath, kdata, want);
                    if (n < 0) { ret = n; break; }
                    /* Copied out only after the read succeeded, and only as
                     * many bytes as were actually produced. */
                    ret = (copy_to_user(frame->a2, kdata, (uint32_t)n) < 0) ? -1 : n;
                    break;
                }
                case 14: { /* SYS_WRITE_FILE: vfs_write(path, buf, len) */
                    char kpath[128];
                    static char kdata[512];
                    uint32_t len = (uint32_t)frame->a3;
                    if (len > sizeof(kdata)) { ret = -1; break; }
                    if (strncpy_from_user(kpath, frame->a1, sizeof(kpath)) < 0) {
                        ret = -1;
                        break;
                    }
                    if (copy_from_user(kdata, frame->a2, len) < 0) { ret = -1; break; }
                    ret = vfs_write(kpath, kdata, len);
                    break;
                }
                default:
                    printk("[Syscall] Unknown syscall nr %ld requested\n", (long)sys_nr);
                    ret = -1;
                    break;
            }

            frame->a0 = (uintptr_t)ret;
            /* Advance EPC past 4-byte ecall instruction */
            frame->epc += 4;
            return;
        }

        /* B3: a fault taken *from U-mode* kills that task instead of halting
         * the machine. Containment is the entire point of running a task in
         * U-mode -- a kernel that halts whenever a user task misbehaves has
         * enforcement but no benefit from it.
         *
         * The previous privilege level comes from the saved status word, where
         * the hardware recorded it: MPP is mstatus[12:11], SPP is sstatus[8].
         * Zero means the trap came from U-mode. */
#if defined(CONFIG_MODE_S)
        bool from_user = ((frame->status >> 8) & 1u) == 0;
#else
        bool from_user = ((frame->status >> 11) & 3u) == 0;
#endif
        if (from_user) {
            /* X5: record what the other harts were doing at this instant,
             * before the kill switches us away. A no-op unless a load is
             * running (kernel/smp.c), so it costs an ordinary fault nothing
             * but a predictable branch. */
            smp_load_note_fault();
            printk("\n[Trap] User task faulted: cause %lu, epc=0x%lx, addr=0x%lx -- "
                   "terminating the task\n",
                   (unsigned long)code, (unsigned long)frame->epc,
                   (unsigned long)frame->tval);
            task_exit(); /* switches away; never returns */
        }

        /* An illegal instruction during a deliberate hardware probe is an
         * answer, not a failure: record it and step over the instruction.
         * See arch/trap.h for why this is narrowed to cause 2 only. */
        if (g_probe_active && code == 2) {
            g_probe_faulted = true;
            frame->epc += 4; /* CSR instructions have no compressed encoding */
            return;
        }

        /* Fatal exception hang */
        uint32_t inst_val = 0;
        if (frame->epc >= 0x10000000 && frame->epc < 0x20082000) {
            const uint8_t *p = (const uint8_t *)frame->epc;
            inst_val = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }
        printk("\n[Trap Exception] Cause: 0x%lx, epc=0x%lx, tval=0x%lx, inst=0x%08x\n",
               (unsigned long)code, (unsigned long)frame->epc, (unsigned long)frame->tval, (unsigned int)inst_val);
        printk("[Trap Register Dump] a0=0x%lx, a1=0x%lx, sp=0x%lx, ra=0x%lx\n",
               (unsigned long)frame->a0, (unsigned long)frame->a1, (unsigned long)frame->sp, (unsigned long)frame->ra);
        printk("[Fatal] System halted due to unhandled exception.\n");
        while (1) {
            __asm__ __volatile__("wfi");
        }
    }
}
