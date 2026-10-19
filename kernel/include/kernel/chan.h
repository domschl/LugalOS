#ifndef LUGALOS_KERNEL_CHAN_H
#define LUGALOS_KERNEL_CHAN_H

#include <stdint.h>
#include <stdbool.h>

/* Copy-always message channels.
 *
 * This is the kernel's only IPC primitive: a named server endpoint that
 * consumes a request *message* and produces a response *message*. Messages
 * are opaque byte buffers. Nothing else is transferred -- in particular, no
 * pointer belonging to the caller is ever handed to a handler.
 *
 * ## Rule 1, and why it is enforced even where it costs something
 *
 * chan_call() performs TWO copies that are, on a NOMMU single-address-space
 * build, provably redundant: the request is copied from the caller's buffer
 * into the endpoint's own, and the response is copied back out again. The
 * handler only ever sees endpoint-owned memory.
 *
 * That redundancy is the point. The general paradigm this codebase follows
 * is that **the NOMMU build obeys the MMU build's design constraints**, so
 * that one set of server sources is correct under both. If the NOMMU path
 * took the obvious shortcut of passing the caller's pointer straight through,
 * the MMU build could not implement the same ABI without copying, the two
 * would diverge into separate IPC systems, and the cross-validation that
 * justifies building both would be worth nothing.
 *
 * The same discipline is what makes remoting nearly free, and this is not a
 * coincidence: an address-space boundary and a network boundary impose the
 * *same* requirement -- no shared pointers, everything explicitly
 * serialized. Code written to survive the first already survives the second.
 * A 9P server across a USB cable uses this same discipline, and local
 * channels deliberately reuse that same machinery rather than shortcutting past it.
 *
 * ## Shape
 *
 * Synchronous request/response only, one request in flight per endpoint:
 * messages are never queued. Callers can be -- see "Waiting for a turn"
 * below.
 *
 * Buffers are supplied by the registrant, not allocated here: this kernel has
 * no malloc (each arch's vmm.c is a bump allocator that never frees), and it
 * keeps this layer free of any assumption about how large a message any
 * particular protocol needs.
 *
 * ## Task-owned endpoints
 *
 * An endpoint registered with chan_register() runs its handler *inline, on
 * the caller's own stack* -- fine for a stateless protocol like local 9P,
 * wrong for a driver, because a bug in the handler then corrupts the
 * caller's context, not the driver's own. There is nothing to "kill and
 * restart" when the code never had an identity apart from whoever happened
 * to call it.
 *
 * chan_register_task() is the other kind: the request is handed to a named
 * task instead of a function pointer, the caller genuinely blocks
 * (task_block()), and the owning task calls chan_serve_wait() /
 * chan_serve_reply() to receive and answer it on its *own* stack, in its
 * own scheduling and memory-domain context. Rule 1 still holds --
 * both sides only ever touch the endpoint's own buffers, never a caller's
 * or a server's raw pointer.
 *
 * One request in flight per endpoint, enforced by chan_call()'s claim of
 * it. A second caller while one is already being served is refused by
 * chan_call(), or waits its turn in chan_call_wait().
 *
 * ## Waiting for a turn
 *
 * chan_call() refuses a busy endpoint at once. chan_call_wait() instead joins
 * the endpoint's FIFO of waiting tasks and blocks until its turn or its
 * deadline. A caller releasing the endpoint hands it straight to the head of
 * the queue (the endpoint never goes free in between), so a client calling
 * back to back cannot take it again ahead of one that was waiting: the touch
 * poller lost transfers to the BMI270's microcode upload that way while every
 * client polled.
 *
 * Refused at once even by chan_call_wait(), because waiting could never end:
 *   - the calling context already holds the endpoint (a recursive local 9P
 *     mount re-entering "p9" from inside its own handler);
 *   - no task is running (boot, a hart without a task) or the caller is an
 *     interrupt handler.
 * A wait is not an edge in the wait-for graph (kernel/lock.h): the holder can
 * change while a task waits. A cycle through a queue (the holder needs a lock
 * the waiter holds) ends at the waiter's deadline instead, with -1.
 *
 * The queue has CHAN_WAITQ_LEN places; a caller finding it full polls at 1 ms
 * until its deadline, without the FIFO's guarantee.
 *
 * Message-oriented, not byte-oriented: a caller with more than one thing to
 * say sends *one* call carrying all of it (see drivers/uart_16550.c's
 * batched write op for why this matters in practice -- the first version
 * of this milestone called a task-owned endpoint once per character and
 * turned an ordinary scheduling decision into a several-times-per-line
 * event, which is a load this primitive was never meant to carry routinely).
 *
 * ## Communication must be strictly top-down: no cycles
 *
 * A task-owned endpoint's owner must never itself become a caller of a task
 * that is (directly, or transitively through other endpoints) already
 * waiting on it. Concretely: task A may call B, or serve calls from B, but
 * never both -- and a longer chain (A calls B, B calls C, C calls A) is
 * exactly as forbidden as the direct case. A driver task that ever
 * chan_call()s a client while serving it, even indirectly, is one bad day
 * away from deadlocking.
 *
 * chan_call_task() enforces this structurally rather than leaving it to
 * driver-author discipline: it maintains a wait-for graph (which task is
 * blocked calling which) and refuses any call that would close a cycle in
 * it, before touching the endpoint. Keep new driver-task conversions
 * strictly layered underneath their callers (uart, then SD/SPI, then
 * display/keypad/RTC/EEPROM) precisely so this check is
 * never the thing standing between a caller and its answer.
 */

/* Raised from 4 for headroom as
 * more services register endpoints of their own. Cost is a static array of
 * chan_endpoint_t (~40 bytes each); the buffers a registrant supplies are
 * the real per-endpoint cost and are unaffected by this constant. */
#define CHAN_MAX_ENDPOINTS 16

/* Consumes `req_len` bytes at `req`, writes a response into `resp` (at most
 * `resp_max` bytes), returns the response length or <0 on failure. Both
 * pointers address endpoint-owned memory, never the caller's. */
typedef int (*chan_handler_fn)(void *ctx, const uint8_t *req, uint32_t req_len,
                               uint8_t *resp, uint32_t resp_max);

typedef struct chan_endpoint chan_endpoint_t;

/* Registers a named endpoint. `req_buf`/`resp_buf` must have static lifetime
 * and are owned by the endpoint for its whole life. Returns 0, or -1 if the
 * table is full, the name is taken, or an argument is missing. */
int chan_register(const char *name, chan_handler_fn handler, void *ctx,
                  uint8_t *req_buf, uint32_t req_cap,
                  uint8_t *resp_buf, uint32_t resp_cap);

chan_endpoint_t *chan_lookup(const char *name);

/* Registers a task-owned endpoint: `owner_pid` -- not a handler
 * function -- is what chan_call() wakes. `owner_pid` must already exist
 * (task_create() first); it does not need to have reached its serve loop
 * yet. Same buffer-ownership rules as chan_register(). */
int chan_register_task(const char *name, int owner_pid,
                       uint8_t *req_buf, uint32_t req_cap,
                       uint8_t *resp_buf, uint32_t resp_cap);

/* Synchronous call: copy `req` in, run the handler against endpoint-owned
 * buffers, copy the response back out to `resp`. Returns the response length,
 * or -1 on failure -- including when the request exceeds the endpoint's
 * capacity, when the endpoint is already executing a call, or (task-owned
 * endpoints only) when the owning task is not alive to answer.
 *
 * That re-entrancy check is a real safety property, not defensive noise: a
 * locally-mounted 9P namespace can be walked into recursively (/local/local/…),
 * which would otherwise re-enter this endpoint and clobber the request buffer
 * out from under the outer call. Failing the inner call is the correct answer
 * and keeps the recursion bounded. For a task-owned endpoint it also means a
 * caller can never be left waiting on an owner that is itself stuck waiting
 * on a nested call back into the same endpoint. */
int chan_call(chan_endpoint_t *ep, const uint8_t *req, uint32_t req_len,
              uint8_t *resp, uint32_t resp_max);

/* Waiting places per endpoint (see "Waiting for a turn" above). */
#define CHAN_WAITQ_LEN 8

/* The bound most clients want: long enough for any one transfer a driver task
 * serves (a 30-byte I2C write is 3 ms at 100 kHz, four SD blocks a few ms),
 * short enough that a stuck owner costs a caller a visible stall, not a hang. */
#define CHAN_WAIT_MS 50u

/* chan_call(), waiting up to `wait_ms` in the endpoint's FIFO when it is busy
 * instead of refusing. 0 is chan_call(). Same return values; -1 after the
 * deadline. The time spent waiting does not count the call's own run. */
int chan_call_wait(chan_endpoint_t *ep, const uint8_t *req, uint32_t req_len,
                   uint8_t *resp, uint32_t resp_max, uint32_t wait_ms);

/* Called by a task-owned endpoint's owner. Blocks until a request is
 * pending, then returns its length -- already copied into the endpoint's
 * own request buffer by chan_call(), so the caller reads it from there
 * directly rather than through a second copy. */
uint32_t chan_serve_wait(chan_endpoint_t *ep);

/* Called by a task-owned endpoint's owner once it has written up to
 * `resp_len` bytes into the endpoint's own response buffer. Wakes the
 * blocked caller with that response. */
void chan_serve_reply(chan_endpoint_t *ep, uint32_t resp_len);

/* Server operations for a task that cannot hold a raw pointer into the
 * endpoint's own req_buf/resp_buf -- such as a U-mode driver task reaching
 * these through SYS_CHAN_SERVE_WAIT/SYS_CHAN_SERVE_REPLY (arch/riscv/common/
 * trap.c). chan_endpoint_t stays opaque; both copy through a caller-owned
 * buffer instead, the same shape chan_call() already uses for its own
 * copy-in/copy-out.
 *
 * chan_serve_wait_copy() blocks exactly as chan_serve_wait() does, then
 * copies min(request length, out_max) bytes of the endpoint's own request
 * buffer into `out` and returns the *full* request length -- which may
 * exceed out_max if the caller's buffer was too small, so truncation is
 * detectable rather than silent (same discipline chan_call()'s resp_max
 * enforces). */
uint32_t chan_serve_wait_copy(chan_endpoint_t *ep, uint8_t *out, uint32_t out_max);

/* Copies min(resp_len, the endpoint's own resp_cap) bytes from `in` into
 * the endpoint's response buffer, then finishes the reply as
 * chan_serve_reply() does. */
void chan_serve_reply_copy(chan_endpoint_t *ep, const uint8_t *in, uint32_t resp_len);

/* The syscall forms: the same three operations, copying
 * straight between the calling U-mode task's memory -- validated against its
 * domain -- and the endpoint's own buffers, while the endpoint is claimed.
 * chan_call_user() waits its turn as chan_call_wait() with CHAN_WAIT_MS.
 * They replace staging through static kernel buffers in the trap handler,
 * which two harts could use at once, and which a task blocked in
 * SYS_CHAN_CALL could resume from on the other hart.
 *
 * chan_serve_wait_user() returns the request length like
 * chan_serve_wait_copy(), copies nothing when it exceeds `out_max`, and sets
 * *err to -1 if the copy faulted. chan_serve_reply_user() refuses a reply
 * larger than the endpoint's buffer instead of truncating it. */
int      chan_call_user(chan_endpoint_t *ep, uintptr_t ureq, uint32_t req_len,
                        uintptr_t uresp, uint32_t resp_max);
uint32_t chan_serve_wait_user(chan_endpoint_t *ep, uintptr_t uout, uint32_t out_max, int *err);
int      chan_serve_reply_user(chan_endpoint_t *ep, uintptr_t uin, uint32_t resp_len);

/* Enumeration for /proc and the /srv/ namespace. Returns false once
 * exhausted. */
bool chan_info(uint32_t index, const char **name_out, bool *busy_out);

/* Called from
 * task_exit() (kernel/sched.c) for every exiting task, clean or faulted.
 * chan_call_task()'s owner-liveness check only covers a caller *starting*
 * a call against an already-dead owner -- its own comment names the gap
 * this closes: "does not close the race where the owner dies after this
 * check but before it replies". That race stopped being rare the moment a
 * driver task's own code could take a real PMP fault (U-mode tasks): a caller mid-chan_call() into a task that then faults would
 * otherwise block forever, since nothing else will ever call
 * chan_serve_reply() on its behalf. Found this exact way on real
 * hardware -- a faulted tm1638 task left the calling shell task
 * permanently blocked, indistinguishable from a full board hang from the
 * console's own vantage point, needing a physical BOOTSEL recovery.
 *
 * Scans every endpoint `owner_pid` owns (at most one in practice; nothing
 * enforces that structurally) and, for one with a request currently
 * pending, unblocks its caller with an error rather than leaving it
 * blocked. The same task is also taken out of every endpoint's wait queue,
 * and a turn handed to it, or a claim it died holding, is passed on: a dead
 * waiter would otherwise keep the endpoint busy for good -- which in turn makes every existing "fall back to direct
 * hardware access if the task isn't alive" facade (uart_putc(),
 * tm1638_display_string(), ...) actually reachable when a task dies
 * mid-request, not just when it never started. */
void chan_owner_exited(int owner_pid);

/* Whether the endpoint is claimed: true from the moment a caller's request
 * is copied in until the last queued caller's reply is back (a hand-off keeps
 * it claimed). For selftests and diagnostics; a caller that wants a turn asks
 * chan_call_wait(), not this. */
bool chan_endpoint_busy(chan_endpoint_t *ep);

/* `chanqtest` (kernel/chan_selftest.c): chan_call_wait()'s queue under real
 * tasks. Prints CHANQ_SELFTEST_OK or _FAIL; 0 or -1. */
int chan_selftest(void);

#endif /* LUGALOS_KERNEL_CHAN_H */
