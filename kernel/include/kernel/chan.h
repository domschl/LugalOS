#ifndef LUGALOS_KERNEL_CHAN_H
#define LUGALOS_KERNEL_CHAN_H

#include <stdint.h>
#include <stdbool.h>

/* Copy-always message channels (B1, plan/phase5_distributed_design.md §5.1).
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
 * Track A is the existing proof (a 9P server across a USB cable), and B1's
 * local channel deliberately reuses that same machinery rather than
 * shortcutting past it.
 *
 * ## Shape
 *
 * Synchronous request/response only. There is no queue depth: this kernel is
 * still single-call-stack (kernel/sched.c is a bookkeeping shim), so a
 * message can never sit waiting for a receiver that is not already running.
 * A depth-N ring would be untestable capacity today, which is a liability
 * rather than a feature. **B2 must revisit this** -- once tasks exist,
 * chan_call() splits into a real blocking send/receive rendezvous and the
 * busy flag below becomes a wait queue.
 *
 * Buffers are supplied by the registrant, not allocated here: this kernel has
 * no malloc (each arch's vmm.c is a bump allocator that never frees), and it
 * keeps this layer free of any assumption about how large a message any
 * particular protocol needs.
 */

/* M0, plan/phase12_microkernel_migration.md: raised from 4 for headroom as
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

/* Synchronous call: copy `req` in, run the handler against endpoint-owned
 * buffers, copy the response back out to `resp`. Returns the response length,
 * or -1 on failure -- including when the request exceeds the endpoint's
 * capacity, or when the endpoint is already executing a call.
 *
 * That re-entrancy check is a real safety property, not defensive noise: a
 * locally-mounted 9P namespace can be walked into recursively (/local/local/…),
 * which would otherwise re-enter this endpoint and clobber the request buffer
 * out from under the outer call. Failing the inner call is the correct answer
 * and keeps the recursion bounded. */
int chan_call(chan_endpoint_t *ep, const uint8_t *req, uint32_t req_len,
              uint8_t *resp, uint32_t resp_max);

/* Enumeration for /proc and the /srv/ namespace. Returns false once
 * exhausted. */
bool chan_info(uint32_t index, const char **name_out, bool *busy_out);

#endif /* LUGALOS_KERNEL_CHAN_H */
