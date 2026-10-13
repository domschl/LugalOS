# LugalOS System Programmer Manual

Welcome to the **LugalOS System Programmer Manual**. This guide provides comprehensive technical documentation for software developers building applications for LugalOS as well as kernel engineers extending the operating system.

---

## Manual Structure

```
doc/dev/
├── on_device/          # Part A: User-Space Application Development
│   ├── c_programming.md   # C11 programming with chibicc, lugal.h, syscall ABI, ELF
│   └── lisp_scripting.md  # Lisp machine runtime, zero-SRAM builtins, stdlib, CAS
├── kernel/             # Part B: LugalOS Kernel & Driver Architecture
│   ├── architecture.md    # Microkernel model, memory domains (PMP/Sv39), IPC channels
│   ├── scheduler.md       # Priority tiers, preemption, cross-hart lock hand-off, SMP
│   ├── logging.md         # Tokenized kernel logging macro system, klogd, rodata dictionary
│   ├── drivers.md         # Confined U-mode driver framework, PMP register grants, IRQs
│   └── porting.md         # Board bringup: linker scripts, clock trees, XIP flash
├── protocols/          # Part D: Interconnect & Wire Protocols
│   └── llp.md             # LugalOS Link Protocol (LLP): framing, flow control, 9P/Ethernet mux
└── testing.md          # Part C: Testing Framework & QA
    ├── Unit tests & pure algorithmic verifiers
    ├── Host harnesses with ASan/UBSan (FAT32, cc, 9P, IP)
    ├── Automated QEMU regression runner (tests/runner.py)
    └── Hardware-in-the-loop silicon suites (tests/hw/)
```

---

## 1. [Part A: On-Device Application Development](on_device/c_programming.md)
* **[C Programming on LugalOS](on_device/c_programming.md)**: Writing native C programs compiled either on the host or directly on the device using `chibicc`. Covers `lugal.h`, available standard library functions, process arguments, and U-mode memory confinement.
* **[Lisp Scripting & Language Extensions](on_device/lisp_scripting.md)**: LugalOS Lisp language reference, special forms, zero-SRAM builtins in Flash `.rodata`, modular packages (`seq`, `dict`, `matrix`), and scripting the CAS.

---

## 2. [Part B: LugalOS Kernel & Driver Architecture](kernel/architecture.md)
* **[Microkernel Architecture](kernel/architecture.md)**: Hardware-isolated tasks, PMP regions on NOMMU, Sv39 page tables on MMU, copy-always channels (`chan_t`), and the syscall validation boundary.
* **[Scheduler & Multicore SMP](kernel/scheduler.md)**: Preemptive round-robin scheduler with static priority tiers, atomic primitives (`spinlock_t`, `ylock_t`), the critical cross-hart scheduler lock hand-off across `ctx_switch()`, and Lazy SMP.
* **[Tokenized Kernel Logging Macro System](kernel/logging.md)**: Zero-overhead logging capturing format-string addresses in `.rodata` and vararg binary blobs in the ring buffer, expanded on drain by `klogd`.
* **[Driver Framework & Fault Isolation](kernel/drivers.md)**: Running drivers as confined U-mode tasks with fine-grained PMP hardware register grants and asynchronous message queues.
* **[Board Porting Guide](kernel/porting.md)**: Step-by-step instructions for porting LugalOS to new RISC-V microcontrollers, clock trees, and XIP memory layouts.

---

## 3. [Part C: Testing Framework & QA](testing.md)
* **[Testing Guide](testing.md)**: How to run and extend the LugalOS test suite across unit tests, host harnesses with AddressSanitizer, automated QEMU regression tests, and physical hardware-in-the-loop tests.

---

## 4. [Part D: Interconnect & Wire Protocols](protocols/llp.md)
* **[LugalOS Link Protocol (LLP)](protocols/llp.md)**: Point-to-point interconnect protocol multiplexing 9P filesystem calls and 802.3 Ethernet frames between coprocessors (e.g. ESP32-P4 and ESP32-C6) with credit-based flow control, session negotiation, and link liveness.
