# LugalOS Microkernel Architecture & Memory Domains

LugalOS is a microkernel: components are tasks that own their state and talk through copy-always channels, and every component is written so that it can run confined to an unprivileged memory domain. User programs always run confined; drivers and services run confined where the board makes that worthwhile (section 5), and as kernel-mode tasks elsewhere. The microkernel build is the reference: what a smaller build leaves out (a folded driver, a release image without its test commands) is derived from it per preset, never written as a second design.

---

## 1. Architectural Philosophy

The LugalOS kernel adheres to three core tenets:
1. **A Small Core, and Components Around It**: The scheduler, the page allocator, the channel engine and the trap handler form the core. Everything else (drivers, services, the shell) is a task with its own stack and state, reached through channels. Kernel-mode tasks run privileged (M-mode on the microcontrollers, S-mode on RV64), so the structure keeps the components apart; a domain is what makes it enforced.
2. **Hardware-Enforced Memory Domains**: User programs, and the drivers and vendor code that have been moved into domains, execute in unprivileged User Mode (U-Mode), strictly constrained by hardware memory windows. Which components that is differs by board; see [section 5](#5-where-isolation-applies-today).
3. **Copy-Always IPC Channels**: Requests between components go through channel endpoints that copy every message in and out (`chan_call()`), so no component holds a pointer into another's buffers. Kernel-mode components do share some statics; each carries an ownership verdict (`single task`, `guarded by`, ...) that `tools/racescan.py --check` verifies ([Concurrency](concurrency.md)).

---

## 2. Hardware Memory Domains: PMP vs Sv39

LugalOS provides a unified memory domain interface (`mem_domain_t`) implemented by two hardware backends:

```
+------------------------------------------------------------------------+
|                        MEM_DOMAIN_T ABSTRACTION                        |
|------------------------------------+-----------------------------------+
|        PMP BACKEND (NOMMU)         |        SV39 BACKEND (MMU)         |
|        RP2350, ESP32-P4, ESP32-C6  |        QEMU RV64 Virt             |
|------------------------------------+-----------------------------------+
| * Physical Memory Protection (PMP) | * 3-Level Hierarchical Page Table |
| * Power-of-two (NAPOT) or TOR      | * 4 KB Virtual Page Frames        |
| * Hardware register grant windows  | * Hardware MMU translation        |
| * Zero page-table memory overhead  | * Full address space separation   |
\------------------------------------+-----------------------------------+
```

### PMP Domain Implementation (NOMMU)
On microcontrollers without an MMU:
* Each task's `mem_domain_t` holds up to `MEM_DOMAIN_MAX_REGIONS` regions (5; 14 on the ESP32-C6, whose radio domain needs them), programmed into PMP entries on every switch to the task. A region is NAPOT (a power of two on its own boundary) or TOR; NAPOT regions are why some allocations are aligned (`palloc_pages_aligned()`).
* Grants are structured to enforce W^X (Write XOR Execute):
  * **Code region**: Read + Execute (`R | X`).
  * **Data & Stack regions**: Read + Write (`R | W`).
  * **Peripheral registers**: Explicit window grants (e.g. only UART0 or SPI1 registers).
* Any attempt by a U-mode task to store into kernel memory or ungranted hardware registers triggers an immediate RISC-V Access Fault trap.

---

## 3. Copy-Always Message Channels

IPC in LugalOS is built upon named request/response endpoints (`kernel/chan.h`, [ADR 004](../../history/adr/004_copy_always_channels.md)):
* **One call, one reply**: `chan_call(ep, req, req_len, resp, resp_max)` copies the request into the endpoint's own request buffer, has it served, and copies the response back out. The server never sees the caller's pointer.
* **Two kinds of endpoint, one call path**: a *task-owned* endpoint is served by its task (`chan_serve_wait()`/`chan_serve_reply()`, or the driver framework's loop): the caller blocks until the reply. An *inline* endpoint is served by a handler in the caller's context (`chan_register()`): the `p9` and `console` endpoints, and every driver a preset folds ([Drivers](drivers.md#4-folded-endpoints-per-preset)). Both kinds go through the same claim and FIFO, so a caller cannot tell them apart.
* **Serialized, in order**: an endpoint serves one call at a time. A busy endpoint is waited for in arrival order (`chan_call_wait()`), or refused at once (`chan_call()`); a caller holding a spinlock, or in an interrupt handler, is refused. A call that would close a wait cycle is refused too (`kernel/lock.h`).
* **Buffer validation**: a call from U-mode is copied with `copy_from_user()`/`copy_to_user()` (`kernel/uaccess.c`), which check every byte range against the caller's domain (`mem_domain_permits()`). An invalid range fails the call with -1; it never faults the kernel.

---

## 4. System Call Boundary

System calls cross from U-Mode into the kernel via the RISC-V `ecall` instruction:
1. Hardware saves user context and switches to the task's private kernel stack.
2. The trap handler (`trap_handler()`, `arch/riscv/common/trap.c`) validates the syscall number.
3. User argument pointers are copied and checked against domain bounds.
4. Syscall executes synchronously in the kernel or yields to a message server.
5. Control returns to U-Mode via `mret` / `sret`.

---

## 5. Where Isolation Applies Today

The domain machinery is the same on every board, but not every component runs in a domain. Services and many drivers are **kernel-mode tasks**: they have their own stack and are reached through channels, but they share the kernel's address space. The `Isol` column of `ps` shows which is which on a running board (`PMP` or `Sv39` for a confined task, `-` for a kernel-mode one).

| Component | RP2350 | ESP32-P4 / ESP32-C6 |
| :--- | :--- | :--- |
| Drivers: UART, USB CDC, SPI SD, I2C, ST7735, LCD terminal, USB keyboard, clock display | U-mode, own domain (`driver_umode_enter()`, `drivers/driver_task.c`) | kernel-mode driver tasks (UART, SDMMC, I2C via `driver_task_start()`); a preset may fold them (section 6) |
| Services: httpd, MQTT, the 9P server, sensor hub, DHCP, setup, klogd, LLP | kernel-mode tasks | kernel-mode tasks |
| Small periodic jobs: watchdog feed, setup button, status LED | jobs on the service host task (`kernel/svchost.c`) | jobs on the service host task |
| Vendor Wi-Fi code | - | U-mode on the C6 ([ADR 007](../../history/adr/007_c6_peripheral_confinement.md)) |
| ELF user programs | U-mode | U-mode |

For a kernel-mode task the channel still copies every message (ADR 004), so moving it into a domain later changes no caller. What the microkernel structure costs such a task in RAM (its stack, its endpoint buffers) is measured per board by the RAM ledger ([Testing Guide](../testing.md#ram-ledger)).

---

## 6. Build Layers and Presets

A build is a **preset** (`CMakePresets.json`): a toolchain, a **board** and a **profile**, plus the build choices that are neither.

* **Board** (`cmake/board-<name>.cmake`): what the hardware is. Addresses, pins, memory sizes, and which drivers its hardware needs.
* **Profile** (`cmake/profile-<name>.cmake`): what the node is for. Which software it carries and the policy and sizes that follow (Lisp pools, TLS, the log ring, the RAM disk). `cmake/lugalos_layers.cmake` fails the configure when a key is in the wrong layer.
* **Preset choices**: `LUGALOS_ENABLE_SMP` (wake the second hart), `LUGALOS_DEV` (a dev image with the self-tests and demo commands, the default; OFF for a release image), and `LUGALOS_FOLD` (the endpoints folded into their callers).

`/proc/config` begins with what the running image is: `PROFILE=`, `DEV=` and `FOLD=`. Details and the full list: [Hardware Presets](../../admin/presets.md#2-boards-and-profiles).

---

## 7. Memory: What a Component Costs, and the Rules That Keep It Predictable

Memory is pages of 4 KB from `palloc` (`kernel/palloc.c`): a fast zone in SRAM, and on boards with PSRAM a bulk zone for large buffers that tolerate it (`palloc_pages_bulk()`, `BULK_BSS`). Each run of pages records its caller, so the RAM ledger (`/proc/pages`, `tools/ramledger.py`, [Testing Guide](../testing.md#ram-ledger)) attributes every page to a task's stack or a subsystem, idle and under a load scenario.

The rules a component follows:

1. **A service reserves at start, or allocates per request and refuses cleanly.** It takes what it needs to run when it starts (the C6's radio takes its 64 KB arena at boot, before the other services), or it allocates per request with a refusal the client can retry, under a margin measured with the load scenario (HTTPS sessions and MQTT over TLS take their buffers per connection). A service is never stopped while idle and restarted on demand: the restart would fail later, under a load nobody can reproduce, instead of at boot, where it is seen.
2. **A task only where it is needed.** Small periodic jobs share the service host's task ([Scheduler](scheduler.md#6-the-service-host-small-periodic-jobs-on-one-task)); a driver whose `serve()` qualifies can be folded by the preset ([Drivers](drivers.md#4-folded-endpoints-per-preset)). A task whose work is over for good may exit and return its pages.
3. **Stacks sized from measurement**: the ledger shows each stack's high-water mark, and a suite check fails above 90 %.
4. **Static RAM has a baseline per preset** (`tools/sizereport.py --all --check`, in the test runner), so growth is a decision, not an accident.

Where this leaves the smallest boards, measured under the loaded scenario: the ESP32-C6-Zero (`esp32c6-release`) ends with 16 of 74 pages free, the Pico 2 W with 40 of 83. A Wi-Fi node with HTTPS, 9P and MQTT needs about 63 pages at its peak; run on an ESP32-C3's budget (44 pages, `LUGALOS_HEAP_CAP_PAGES`), it does not fit. LugalOS does not target chips below that: a build small enough would have to give up the properties above (plan/phase66, 66.10).

