# ADR 004: Copy-Always Message Channels for Microkernel IPC

## Status
Accepted (2026-08-15)

## Context
Microkernels rely on Inter-Process Communication (IPC) to pass data between applications and isolated driver services. Monolithic designs or shared-memory IPC architectures allow tasks to map shared buffers. However, shared-memory IPC introduces subtle pointer race conditions, cache coherence invalidation overhead, and security risks if an untrusted task mutates a shared buffer while the server is validating it.

## Decision
LugalOS adopts **copy-always message channels (`chan_t`)** as its sole IPC primitive:
1. Cross-task shared memory is disallowed.
2. System calls (`SYS_CHAN_CALL`) explicitly copy message payloads across domain boundaries: from the client's memory domain into a kernel intermediate buffer, and from there into the server's domain.
3. Every user pointer and memory length is strictly validated against the caller's memory domain (`mem_domain_t`) via `uaccess_validate()` before any copy operation.

## Consequences
* **Positive**: Absolute isolation: memory bugs, out-of-order writes, or race conditions in one task cannot corrupt another task's state.
* **Positive**: Predictable, clean API inspired by Plan 9 and CSP.
* **Negative**: Memory copy overhead for large payloads (mitigated by bulk block transfers and hardware DMA for storage and display frames).
