# LugalOS Documentation

Welcome to the documentation for **LugalOS**, a bare-metal, capability-isolated RISC-V microkernel operating system with Plan 9 IPC, exact Lisp algebra, on-device compilation, and horizontal tiling window ribbons.

---

## Documentation Structure

The documentation is organized into three comprehensive manuals, an Architecture Decision Record (ADR) repository, and a generated on-device help system:

```
doc/
|-- user/             User Manual: Day-to-day operation, shell, and applications
|-- admin/            Administrator Manual: Hardware presets, flashing, and networking
|-- dev/              System Programmer Manual: C & Lisp APIs, kernel internals, and testing
|-- history/          Architecture Decision Records (ADRs)
\-- assets/           Hardware photographs, diagrams, and terminal screenshots
```

* **[About LugalOS](about.md)**: High-level architecture overview, hardware targets, on-device tools, and system concepts.

---

## 1. [User Manual](user/index.md)

Guides for operating LugalOS interactively via terminals and serial consoles:

* **[Shell & Commands](user/shell.md)**: Interactive `lsh` shell, file manipulation, pipeline & redirection model, procfs/devfs navigation, job control.
* **[Text Editors](user/editors.md)**: Full-screen Emacs editor (`e`), teletype line editor (`ed`), keybindings, and buffer management.
* **[Window Ribbon & Terminal](user/ribbon.md)**: Horizontal tiling window manager, touch gestures (tap, swipe), keyboard navigation (`Cmd+Left/Right`), split canvases, screenshots.
* **[Chess Computer](user/chess.md)**: Operating the chess engine on RP2350 and display terminals, Lazy SMP dual-core search, TM1638 keypad controls, and game history.
* **[Computer Algebra System (CAS)](user/cas.md)**: Exact symbolic mathematics, canonical polynomial simplification, equation solving, series expansion, and plotting.
* **[Environmental Sensors & Telemetry](user/sensors.md)**: Autonomous sensor node operation, I2C environmental telemetry (BME280/BMP280), and MQTT 3.1.1 publishing.
* **[Z-Machine & Interactive Fiction](user/zmachine.md)**: Playing Infocom and v3 stories, split-screen canvas auto-mapper, and save slot persistence.

---

## 2. [Administrator Manual](admin/index.md)

Guides for hardware provisioning, flashing, networking, and storage:

* **[Hardware Presets Matrix](admin/presets.md)**: Comparison of all supported silicon targets (RP2350, ESP32-P4, ESP32-C6, QEMU RV32/RV64).
* **[Firmware Flashing Guide](admin/flashing.md)**: Step-by-step flashing procedures via `picotool`, `p4flash.py`, and `c6flash.py`.
* **[Consoles & Serial Connections](admin/consoles.md)**: Dynamic TTY port mapping, CDC-ACM dual ports, CH343P/CP2102 baud rates (921600 on ESP32-P4), and modem control (DTR/RTS).
* **[Networking & Plan 9 Services](admin/networking.md)**: 9P filesystem protocol, `p9sync.py` synchronization, `fuse-p9` host mounts, SLIP demultiplexing, and Wi-Fi / Ethernet setups.
* **[Identity & Security Records](admin/identity.md)**: Board provisioning, credential storage (`idstore`), Wi-Fi PSK derivation, and mutual authentication.
* **[Storage & Filesystems](admin/storage.md)**: MicroSD FAT32 formatting, flashfs ROM disks, and RAM disks.
* **[Updating Nodes over 9P](admin/updates.md)**: A/B partition updates, trial booting, and rollback.
* **[HTTPS Certificates](admin/tls.md)**: Your own certificate authority (`host/ca`), a node's key and certificate, and trusting the root on each system.
* **[Target Hardware & Board Reference Guides](admin/boards/index.md)**: Operational guides for [ESP32-P4-LCD-7B](admin/boards/esp32p4_lcd7b.md), [M5Stack Tab5](admin/boards/esp32p4_tab5.md), [ESP32-C6](admin/boards/esp32c6.md), and [RP2350](admin/boards/rp2350.md).

---

## 3. [System Programmer Manual](dev/index.md)

Guides for developing user-space applications and extending the LugalOS kernel:

### Part A: On-Device Application Development
* **[C Programming on LugalOS](dev/on_device/c_programming.md)**: On-device compilation with `chibicc`, freestanding C subset (`lugal.h`), system call ABI, and ELF execution under PMP domains.
* **[Lisp Scripting & Extensions](dev/on_device/lisp_scripting.md)**: LugalOS Scheme/Lisp dialect, zero-SRAM builtins in Flash `.rodata`, standard library modules, macros, and CAS extensions.

### Part B: Kernel & Driver Architecture
* **[Microkernel Architecture](dev/kernel/architecture.md)**: Capability-style memory domains (PMP on NOMMU, Sv39 page tables on MMU), copy-always message channels (`chan_t`), and U-mode task isolation.
* **[Scheduler & SMP Multicore](dev/kernel/scheduler.md)**: Priority tiers, preemptive timer scheduling, cross-hart scheduler lock hand-off, and atomic primitives (`spinlock_t`, `ylock_t`).
* **[Tokenized Kernel Logging](dev/kernel/logging.md)**: High-efficiency dictionary logging, `.rodata` format string pointers, varargs capturing in `klog_emit_tok()`, and deferred expansion via `klogd`.
* **[Driver Framework](dev/kernel/drivers.md)**: Confined U-mode driver tasks, hardware register window grants, IRQ dispatch, and fault containment.
* **[Board Porting Guide](dev/kernel/porting.md)**: Adding new SoCs and boards, linker layouts, clock trees, stage 2 bootloaders, and XIP flash mapping.

### Part C: Testing Framework & QA
* **[Testing Guide](dev/testing.md)**: Unit tests, ASan/UBSan host harnesses, automated QEMU regression runner (`tests/runner.py`), and hardware-in-the-loop silicon suites (`tests/hw/`).

### Part D: Wire & Interconnect Protocols
* **[Protected 9P sessions](dev/protocols/protected_9p.md)**: Integrity for 9P over TCP and WebSocket without TLS (`9P2000.lp`): key derivation, per-frame tags, test vectors, and why secrets stay off a readable link.
* **[LugalOS Link Protocol (LLP)](dev/protocols/llp.md)**: Point-to-point link protocol multiplexing 9P filesystem calls and raw 802.3 Ethernet frames between coprocessors with credit-based flow control.

---

## 4. [Architecture Decision Records (ADRs)](history/adr/index.md)

Formal records documenting critical architectural choices, trade-offs, and historical rationale:

* **[ADR 001: Zero Floating-Point in Kernel / M-Mode](history/adr/001_zero_floating_point.md)**
* **[ADR 002: Scheduler Lock Hand-Off Across Context Switches](history/adr/002_sched_lock_handoff.md)**
* **[ADR 003: Tokenized Dictionary Logging in Kernel Ring](history/adr/003_tokenized_kernel_logging.md)**
* **[ADR 004: Copy-Always Message Channels for IPC](history/adr/004_copy_always_channels.md)**
* **[ADR 005: Zero SRAM Builtins in Flash .rodata](history/adr/005_zero_sram_builtins.md)**

---

## 5. On-Device Help System

All chapters from the User and Administrator manuals are automatically compiled by [`tools/gendocs.py`](../tools/gendocs.py) into 78-column plain-text help files stored on device storage at `/sd0/help/` (and `/flash0/help/`).

On a running LugalOS terminal or serial console:
* Type `help` for an overview of shell commands.
* Type `help <topic>` or `man <topic>` to read interactive help with terminal paging and single-key `[1..9]` link jumps.
