# ADR 003: Tokenized In-Kernel Dictionary Logging

## Status
Accepted (2026-09-11)

## Context
Microkernel diagnostics and driver event logging produce significant amounts of text. Measuring RV32 boot logs showed that 89% of the emitted bytes consist of static format strings (`"[palloc] Initialized..."`), while actual variable arguments represent only a fraction of the total size. In an 8 KB circular log ring, rendered text logging only holds ~3 boots before wrapping around.

## Decision
LugalOS adopts an in-kernel tokenized dictionary logging model:
1. `printk()` and `cprintf()` are annotated with `__attribute__((format(printf, 1, 2)))` to guarantee argument width safety.
2. The format string's pointer in Flash `.rodata` is treated as an immutable, globally unique dictionary identifier.
3. Instead of storing rendered text in the circular ring, `printk()` captures `{ render_len, const char *fmt, vararg_blob }` in binary form via `klog_emit_tok()`.
4. **Expansion occurs inside the kernel upon drain**: When `cat /proc/kmsg` or the background consumer `klogd` reads a record, the kernel expands the binary record back into human-readable text on the fly.

## Consequences
* **Positive**: Ring buffer storage density improves by nearly **6×**, retaining ~18 boots in an 8 KB ring.
* **Positive**: `printk()` never blocks, making logging safe from any interrupt or server context.
* **Positive**: Zero external host dependency: log files remain directly human-readable over serial and Plan 9 network links.
* **Negative**: Slightly higher CPU overhead at drain time to format strings dynamically.
