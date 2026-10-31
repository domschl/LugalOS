# LugalOS Architecture Decision Records (ADRs)

[Documentation](../../README.md) > **Architecture Decision Records**

---

Architecture Decision Records (ADRs) capture important architectural choices, their underlying context, trade-offs, and historical rationale. When source code comments are trimmed of historical narrative, the underlying reasoning is preserved here.

---

## Index of Architecture Decision Records

| ADR | Title | Status | Date |
| :--- | :--- | :--- | :--- |
| **[ADR 001](001_zero_floating_point.md)** | Zero Floating-Point in Kernel / M-Mode (`mstatus.FS = 0`) | Accepted | 2026-08-10 |
| **[ADR 002](002_sched_lock_handoff.md)** | Scheduler Lock Hand-Off Across Context Switches | Accepted | 2026-08-25 |
| **[ADR 003](003_tokenized_kernel_logging.md)** | Tokenized In-Kernel Dictionary Logging | Accepted | 2026-09-11 |
| **[ADR 004](004_copy_always_channels.md)** | Copy-Always Message Channels for Microkernel IPC | Accepted | 2026-08-15 |
| **[ADR 005](005_zero_sram_builtins.md)** | Zero SRAM Builtins in Flash `.rodata` Tables | Accepted | 2026-09-28 |
| **[ADR 006](006_remote_admin_trust_model.md)** | Remote Administration Trust Model | Accepted | 2026-10-31 |

---

**Navigation:** [<- Documentation Index](../../README.md)
