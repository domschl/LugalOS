# LugalOS System Programmer Manual

[Documentation](index.md) > **System Programmer Manual**

---

Welcome to the **LugalOS System Programmer Manual**. This guide provides comprehensive technical documentation for software developers building applications for LugalOS as well as kernel engineers extending the operating system.

---

## Manual Structure

```
doc/dev/
|-- on_device/          # Part A: User-Space Application Development
|   |-- c_programming.md   # C11 programming with chibicc, lugal.h, syscall ABI, ELF
|   \-- lisp_scripting.md  # Lisp machine runtime, zero-SRAM builtins, stdlib, CAS
|-- kernel/             # Part B: LugalOS Kernel & Driver Architecture
|   |-- architecture.md    # Microkernel model, domains, channels, build layers, memory rules
|   |-- scheduler.md       # Priority tiers, preemption, lock hand-off, SMP, service host
|   |-- concurrency.md     # Ownership comments, racescan, locks and lock order, waits, stress
|   |-- logging.md         # Tokenized kernel logging macro system, klogd, rodata dictionary
|   |-- drivers.md         # Driver tasks, U-mode domains (RP2350), folding, IRQs
|   |-- ui_framework.md    # Views, window events, input routing, ugui, apps/icons/settings
|   \-- porting.md         # Board bringup: linker scripts, clock trees, XIP flash
|-- protocols/          # Part D: Interconnect & Wire Protocols
|   \-- llp.md             # LugalOS Link Protocol (LLP): framing, flow control, 9P/Ethernet mux
|-- releasing.md        # Part C: Making a release (version, tag, GitHub release)
\-- testing.md          # Part C: Testing Framework & QA
    |-- Unit tests & pure algorithmic verifiers
    |-- Host harnesses with ASan/UBSan (FAT32, cc, 9P, IP)
    |-- Automated QEMU regression runner (tests/runner.py)
    \-- Hardware-in-the-loop silicon suites (tests/hw/)
```

---

## 1. [Part A: On-Device Application Development](c_programming.md)
* **[C Programming on LugalOS](c_programming.md)**: Writing native C programs compiled either on the host or directly on the device using `chibicc`. Covers `lugal.h`, available standard library functions, process arguments, and U-mode memory confinement.
* **[Lisp Scripting & Language Extensions](lisp.md)**: LugalOS Lisp language reference, special forms, zero-SRAM builtins in Flash `.rodata`, modular packages (`seq`, `dict`, `matrix`), and scripting the CAS.

---

## 2. [Part B: LugalOS Kernel & Driver Architecture](architecture.md)
* **[Microkernel Architecture](architecture.md)**: Tasks and memory domains (PMP regions on NOMMU, Sv39 page tables on MMU), copy-always channel endpoints, the syscall boundary, where isolation applies per board, the board/profile/preset layers, and the memory rules (reserve at start, the service host, folding, the RAM ledger).
* **[Scheduler & Multicore SMP](scheduler.md)**: Preemptive round-robin scheduler with static priority tiers, atomic primitives (`spinlock_t`, `ylock_t`), the critical cross-hart scheduler lock hand-off across `ctx_switch()`, Lazy SMP, and the service host that runs small periodic jobs on one task.
* **[Concurrency: Shared State, Locks and Waits](concurrency.md)**: The ownership comment on every shared static and `racescan.py`, spinlocks vs ylocks and the learned lock order, `chan_call_wait()` and timed polls, and the stress harness.
* **[Tokenized Kernel Logging Macro System](logging.md)**: Zero-overhead logging capturing format-string addresses in `.rodata` and vararg binary blobs in the ring buffer, expanded on drain by `klogd`.
* **[Driver Framework & Fault Isolation](drivers.md)**: The four parts of a driver, where drivers run per board (U-mode domains on the RP2350, kernel-mode tasks elsewhere), folding a driver into its callers per preset, interrupts, and calling a driver.
* **[UI Framework: Views, Window Events, Input and `ugui`](ui_framework.md)**: How a view is written (its ops, its window, the views task), what every window is told (`win_event_t`, claims, system events), where touch, mouse and keys go, the on-screen keyboard as a service, and the `ugui` toolkit, the app registry, icons and settings.
* **[Board Porting Guide](porting.md)**: Step-by-step instructions for porting LugalOS to new RISC-V microcontrollers, clock trees, and XIP memory layouts.

---

## 3. [Part C: Testing Framework & QA](testing.md)
* **[Testing Guide](testing.md)**: How to run and extend the LugalOS test suite across unit tests, host harnesses with AddressSanitizer, automated QEMU regression tests, and physical hardware-in-the-loop tests.
* **[Making a Release](releasing.md)**: The version, a clean build id, the tests, installing on the boards, and the tag and GitHub release.

---

## 4. [Part D: Interconnect & Wire Protocols](llp.md)
* **[The web admin transport](web_admin.md)**: 9P over WebSocket on HTTP and HTTPS, the endpoints, and the TLS 1.3 profile.
* **[Protected 9P sessions](protected_9p.md)**: Integrity for 9P over TCP and WebSocket without TLS: the `9P2000.lp` version, the session key derived from the grant key and the auth challenge, the per-frame tag, test vectors, and why secrets still stay off a readable link.
* **[LugalOS Link Protocol (LLP)](llp.md)**: Point-to-point interconnect protocol multiplexing 9P filesystem calls and 802.3 Ethernet frames between coprocessors (e.g. ESP32-P4 and ESP32-C6) with credit-based flow control, session negotiation, and link liveness.

---

**Navigation:** [<- Documentation Index](index.md)
