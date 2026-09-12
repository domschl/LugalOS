#ifndef LUGALOS_DRIVERS_DRIVER_TASK_H
#define LUGALOS_DRIVERS_DRIVER_TASK_H

/* What a driver is in this kernel, and how to write one (G5,
 * plan/phase30_driver_framework.md).
 *
 * Read this file before writing a driver. It should answer the question
 * without your having to read nine existing ones; if it does not, that is a
 * bug in this comment. Two companions: drivers/README.md for *which kind of
 * thing* you are writing and the rule about when to share code, and
 * plan/hardware_seams.md for the inventory of what already exists.
 *
 * ## A driver here has up to four parts, and only one of them is shared
 *
 *   1. **The register half.** Which bits in which register on this exact
 *      chip. Never shared, across chips or vendors -- see drivers/README.md
 *      category A for why sharing it produces plausible wrong values rather
 *      than merge conflicts.
 *
 *   2. **The task half.** A long-lived task owning the hardware, serving
 *      requests over a chan_call() endpoint. **This is the part this file
 *      is.** Zero board-specific content, which is exactly why it could be
 *      extracted while part 1 could not.
 *
 *   3. **The facade.** The ordinary functions everyone else calls --
 *      uart_putc(), blk_read(), i2c_xfer(). Each checks
 *      driver_task_alive() and either calls the task or falls back to direct
 *      hardware access. The fallback is not a nicety: it is how the system
 *      boots before the task exists and keeps working if it never starts.
 *
 *   4. **A U-mode domain, on boards where that means something.** Optional,
 *      and see the note further down before reaching for it.
 *
 * ## A worked example: the whole of a driver's task half
 *
 *     // 1. The buffers the endpoint owns, and the framework's handle.
 *     static uint8_t       g_widget_req[8];
 *     static uint8_t       g_widget_resp[4];
 *     static driver_task_t g_widget_task;
 *
 *     // 2. The serve callback: your wire protocol, and nothing else.
 *     //    Runs on the task, one request at a time. Return the number of
 *     //    response bytes written.
 *     static uint32_t widget_serve(void *ctx, const uint8_t *req,
 *                                  uint32_t req_len,
 *                                  uint8_t *resp, uint32_t resp_cap) {
 *         (void)ctx; (void)resp_cap;
 *         switch (req[0]) {
 *             case WIDGET_REQ_READ:
 *                 resp[0] = widget_hw_read();     // registers: part 1
 *                 return 1;
 *             case WIDGET_REQ_WRITE:
 *                 for (uint32_t i = 1; i < req_len; i++)
 *                     widget_hw_write(req[i]);
 *                 return 0;
 *             default:
 *                 return 0;
 *         }
 *     }
 *
 *     // 3. Start it, from kernel/main.c after sched_init().
 *     int widget_task_start(void) {
 *         const driver_task_spec_t spec = {
 *             .name        = "widget",      // task name AND endpoint name
 *             .serve       = widget_serve,
 *             .req         = g_widget_req,  .req_cap  = sizeof(g_widget_req),
 *             .resp        = g_widget_resp, .resp_cap = sizeof(g_widget_resp),
 *             .min_req_len = 1,             // shorter gets an empty reply
 *             .stack_pages = 1,
 *             .priority    = DRIVER_PRIO_DEFAULT,
 *         };
 *         return driver_task_start(&g_widget_task, &spec);
 *     }
 *
 *     // 4. The facade, for everyone else. Falls back when the task is not
 *     //    serving -- during boot, or if it never started.
 *     uint8_t widget_read(void) {
 *         if (driver_task_alive(&g_widget_task)) {
 *             uint8_t req[1] = { WIDGET_REQ_READ }, resp[1];
 *             if (driver_task_call(&g_widget_task, req, 1, resp, 1) == 1)
 *                 return resp[0];
 *         }
 *         return widget_hw_read();
 *     }
 *
 * That is a complete driver task. Failure is never fatal by construction:
 * driver_task_start() returning -1 leaves every facade on its fallback path,
 * which is what the system did before the driver was a task at all.
 *
 * ## What this replaced
 *
 * Nine drivers each wrote the same loop by hand:
 *
 *     static void X_task_body(void *arg) {
 *         while (!g_X_ep) sched_yield();
 *         for (;;) {
 *             uint32_t req_len = chan_serve_wait(g_X_ep);
 *             ... switch on g_X_req[0] ...
 *             chan_serve_reply(g_X_ep, resp_len);
 *         }
 *     }
 *
 * The switch is the driver. Everything around it is not: the registration
 * handshake, the serve loop, the reply, the liveness check, the bounded
 * retry, and the three invariants below.
 *
 * Category C of plan/hardware_seams.md -- zero board-specific content, which
 * is exactly why it is shared and why the register halves are not.
 *
 * ## The three invariants
 *
 * They were a comment repeated in every driver. Here is what each one is
 * now:
 *
 * 1. **printk() freely; never cprintf() from inside the serve callback.**
 *    This inverted, and the half that inverted is worth knowing. printk() is
 *    a ring append that reaches no blocking primitive from any context, so a
 *    serve callback may log as freely as anything else (Y5c,
 *    plan/phase31_concurrency_hierarchy.md). The *console* stream still
 *    reaches the uart task through chan_call(), so a callback writing it
 *    while a caller is blocked on this endpoint closes the cycle -- the
 *    original hazard, in the one place it survives.
 *
 *    **Checked**, not remembered: driver_task.c brackets every callback with
 *    lock_serve_enter()/leave(), and console_lock() reports a named fault if
 *    it is reached from inside one (kernel/lock.h). It fires on the
 *    violation, not on the collision, so it is deterministic on QEMU instead
 *    of being the bug that only ever reproduces on hardware.
 *
 * 2. **Never call back into anything that could chan_call() this same
 *    endpoint.** Refused since Y3: chan_call() will not close a cycle in the
 *    wait-for graph (kernel/lock.h). The callback is handed its request and
 *    its response buffer and needs no facade function to reach either, which
 *    is what made this easy to get wrong before.
 *
 * 3. **Only this task may touch the hardware while it is alive.** Structural
 *    now rather than stated: the framework owns the loop, so the only code
 *    that runs inside it is the serve callback. Facade functions check
 *    driver_task_alive() and fall back to direct access when it is not.
 *
 * ## What this deliberately does not do
 *
 * It carries bytes and does not read them. The wire protocol -- 'H'/'R'/'W'
 * for uart, the block opcodes, the i2c ops -- stays in the driver, because a
 * framework that knew about opcodes would be a second place to change when
 * one is added. */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "kernel/chan.h"

/* Serves one request. Returns the number of response bytes written, which
 * may be 0; anything larger than resp_cap is clamped by the framework.
 *
 * `req` is the endpoint's own request buffer, already filled, and `resp` its
 * response buffer -- the same two the spec handed over, so a callback that
 * wants to parse in place may. Runs with invariant 1 checked (see above). */
typedef uint32_t (*driver_serve_fn)(void *ctx,
                                    const uint8_t *req, uint32_t req_len,
                                    uint8_t *resp, uint32_t resp_cap);

/* Leave `priority` as this to keep whatever task_create_driver() gave, which
 * is what six of the nine drivers want. The three UARTs raise it to
 * TASK_PRIO_INTERRUPT: a caller that chan_call()s them unblocks the driver
 * and then blocks itself, and at TASK_PRIO_NORMAL the scheduler could put an
 * arbitrary queue between the two halves of what is meant to be a near-
 * instant hardware handoff. */
#define DRIVER_PRIO_DEFAULT (-1)

typedef struct {
    /* Both the task name and the endpoint name -- they have never differed,
     * and a driver that needed them to differ would be worth a second look
     * rather than a second field. */
    const char     *name;

    driver_serve_fn serve;
    void           *ctx;

    /* The buffers registered with the endpoint. chan_call() copies a
     * caller's request straight into `req`, so the serve callback reads its
     * request out of the same array the driver registered. */
    uint8_t        *req;
    uint32_t        req_cap;
    uint8_t        *resp;
    uint32_t        resp_cap;

    /* Requests shorter than this never reach the callback; they get an empty
     * reply, and are not counted as calls served. Every driver had this as
     * its own first line -- `if (req_len < N) { reply(0); continue; }`. */
    uint32_t        min_req_len;

    uint32_t        stack_pages;   /* 0 means 1, which is what all nine use */
    int             priority;      /* DRIVER_PRIO_DEFAULT, or a TASK_PRIO_* */
} driver_task_spec_t;

/* The driver declares one of these static and hands it to
 * driver_task_start(). It holds what the driver used to hold in three
 * separate globals: the endpoint, the pid, and the count of calls served. */
typedef struct {
    driver_task_spec_t spec;
    chan_endpoint_t   *ep;
    int                pid;

    /* Requests handed to the serve callback since boot. M4.5's verification
     * question -- "is the task genuinely serving requests, or did every
     * caller silently fall back to direct access?" -- is answered by this
     * being nonzero and growing, and several tests assert exactly that. */
    uint32_t           calls;
} driver_task_t;

/* Creates the task, registers the endpoint, and returns the pid (or -1).
 *
 * Failure is not fatal by construction: every caller's facade checks
 * driver_task_alive() and falls back to direct hardware access, which is
 * what it did before the driver was a task at all. The framework prints the
 * same "falling back" line the nine drivers each printed. */
int driver_task_start(driver_task_t *dt, const driver_task_spec_t *spec);

/* False before the task starts, after it dies, and for a driver whose start
 * failed -- the one question every facade function asks before choosing
 * between the endpoint and the hardware. */
bool driver_task_alive(const driver_task_t *dt);

static inline uint32_t driver_task_call_count(const driver_task_t *dt) {
    return dt ? dt->calls : 0u;
}

/* chan_call() with a bounded retry on a transient busy (the endpoint's
 * single in-flight slot occupied by someone else), yielding between
 * attempts. Returns the response length, or -1 once the retries are
 * exhausted -- which is a real if unlikely degradation and the only case a
 * caller should fall back to direct access for. A dead or unstarted task is
 * driver_task_alive()'s question, asked before this one. */
int driver_task_call(driver_task_t *dt, const uint8_t *req, uint32_t req_len,
                     uint8_t *resp, uint32_t resp_max);

/* The endpoint itself, for the one caller whose retry policy is a driver
 * fact rather than a framework one: uart_flush() must never give up while
 * another WRITE is actually in flight (it would interleave bytes on the
 * wire), but must fall back promptly when the endpoint is merely busy with a
 * pending READ that is waiting on a human. That distinction is
 * g_uart_write_in_flight's, and a framework that took an opinion on it would
 * be taking one about UARTs. NULL before the task is registered. */
static inline chan_endpoint_t *driver_task_endpoint(const driver_task_t *dt) {
    return dt ? dt->ep : NULL;
}

/* --- The U-mode domain (G3, plan/phase30_driver_framework.md §2.2) ---------
 *
 * ## Read this before reaching for it
 *
 * U-mode is not a hardening option you sprinkle on. On this kernel it is the
 * *only* place a memory domain means anything -- PMP restricts privilege
 * levels below the one that programs it, and the RP2350 kernel runs in
 * M-mode, so a domain attached to a kernel-mode task confines nothing and
 * "activates" successfully while doing it (plan/phase12_microkernel_migration
 * .md's M5 opens by retracting its own scope over exactly this).
 *
 * So the choice is real isolation or a kernel-mode task, and it is a choice,
 * because U-mode takes things away that a serve loop may have been relying
 * on:
 *
 *   - **It cannot block.** No task_block(), no irq_save(). A U-mode server
 *     cannot offer a blocking read; uart_rp2350.c's 'R' is "read if ready"
 *     with a two-byte reply for this reason, and the waiting moved into the
 *     client. If your protocol has a blocking operation, U-mode changes your
 *     protocol -- see drivers/uart_proto.h, which documents that divergence
 *     rather than hiding it.
 *   - **It cannot reach .rodata.** No string literals (build them into a
 *     `volatile char[]`), and no `switch` (its jump table lands there too --
 *     use if/else, and add the file to CMakeLists.txt's
 *     -fno-jump-tables list).
 *   - **It cannot call another driver's kernel .text.** Anything your task
 *     did by calling into a different driver has to move to the facade,
 *     which still runs in kernel mode. That is what M5 Phase 6 did to the
 *     RP2350 UART's USB mirror.
 *
 * A U-mode driver therefore cannot use driver_task_start()'s serve loop; it
 * writes its own, in .utext, over the usys_* syscalls. Six drivers do, and
 * that duplication is noted in G4 as a separate extraction nobody has done.
 *
 * Seven drivers built a mem_domain_t by hand and the shape never varied: the
 * task's own U-mode stack (R/W), the shared `.utext` page from
 * board_text_region() (R/X), one to four MMIO windows, then
 * task_set_domain(), then either arch_enter_user() or a refusal.
 *
 * The refusal is the part most worth sharing. It is a safety property --
 * *refuse rather than claim unverified isolation* -- and it was seven separate
 * implementations of one policy, each with its own wording. A driver that got
 * it subtly wrong would report isolation it did not have, which is the one
 * failure mode this whole mechanism exists to prevent.
 *
 * `.utext` is added by the framework rather than by the caller: every one of
 * the seven needs it, none of them may omit it (U-mode cannot execute
 * anything else), and a driver that forgot would fault on its first
 * instruction. */

typedef struct {
    uintptr_t base;
    uint32_t  size;
    uint32_t  perms;   /* MEM_R | MEM_W | MEM_X, kernel/mem_domain.h */
} driver_region_t;

/* Four is what the widest of the seven needs (usb_cdc: two MMIO windows plus
 * a shared data region, on top of stack and .utext). */
#define DRIVER_UMODE_MAX_REGIONS 4

/* Drivers that enter U-mode, across every persona. Seven today; the clock
 * persona builds the most at once. */
#define DRIVER_UMODE_MAX_DOMAINS 8

typedef struct {
    /* Named in the refusal, so it reads as the driver's own message. */
    const char *name;

    /* What the caller falls back to, e.g. "keypad/display stay on direct
     * hardware access" -- the second half of the refusal line. */
    const char *fallback;

    void (*body)(void);          /* the U-mode entry point */

    /* Which `.utext` region to grant R/X, or NULL for board_text_region().
     *
     * There are five: the shared one, plus a dedicated region each for
     * st7735, spisd, usb_cdc and pico_clock_green -- kernel/board.c, and
     * §D of plan/hardware_seams.md for why they are separate (the shared page
     * ran out of room). The framework always grants one, because U-mode can
     * execute nothing else; *which* one is the driver's to say. */
    void (*text_region)(uintptr_t *base, uintptr_t *size);

    /* The task's U-mode stack, granted R/W. `stack_top` is where execution
     * starts; 0 means base + size, which is what six of the seven want.
     * tm1638 passes a lower top because the last bytes of its page are its
     * own scratch RAM rather than stack. */
    uintptr_t stack_base;
    uint32_t  stack_size;
    uintptr_t stack_top;

    driver_region_t regions[DRIVER_UMODE_MAX_REGIONS];
    uint32_t        region_count;

    /* arch_enter_user()'s `argc`, which the ABI lands in a0 -- so a body
     * declared to take one parameter receives this. Six of the seven pass
     * nothing; spisd passes whether the card is SDHC, because its U-mode half
     * cannot read the kernel-side flag. */
    uintptr_t arg;
} driver_umode_spec_t;

/* Builds the domain, attaches it, and drops to U-mode.
 *
 * **Does not return on success** -- arch_enter_user() does not come back.
 * Returns -1 when the domain could not be enforced, having said so, and the
 * caller should then return from its task body so the driver keeps working
 * through the direct-access path its facade functions already have. */
int driver_umode_enter(const driver_umode_spec_t *spec);

#endif /* LUGALOS_DRIVERS_DRIVER_TASK_H */
