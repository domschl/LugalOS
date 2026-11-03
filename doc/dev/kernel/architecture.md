# LugalOS Microkernel Architecture & Memory Domains

LugalOS is engineered as a true capability-isolated microkernel operating system. Unlike monolithic embedded systems where all code shares flat memory in privileged machine mode, LugalOS confines applications and drivers to isolated unprivileged domains.

---

## 1. Architectural Philosophy

The LugalOS kernel adheres to three core tenets:
1. **Minimal Privileged Core**: Only the scheduler, physical page allocator, IPC channel engine, and trap dispatcher execute in RISC-V Machine Mode (M-Mode) or Supervisor Mode (S-Mode).
2. **Hardware-Enforced Memory Domains**: User programs, and the drivers and vendor code that have been moved into domains, execute in unprivileged User Mode (U-Mode), strictly constrained by hardware memory windows. Which components that is differs by board; see [section 5](#5-where-isolation-applies-today).
3. **Copy-Always IPC Channels**: Shared memory concurrency across tasks is disallowed. All cross-task communication takes place through copy-always message channels (`chan_t`), eliminating pointer race conditions and use-after-free vulnerabilities.

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
* Each task's `mem_domain_t` configures 4 to 8 hardware PMP registers during context switches.
* Grants are structured to enforce W^X (Write XOR Execute):
  * **Code region**: Read + Execute (`R | X`).
  * **Data & Stack regions**: Read + Write (`R | W`).
  * **Peripheral registers**: Explicit window grants (e.g. only UART0 or SPI1 registers).
* Any attempt by a U-mode task to store into kernel memory or ungranted hardware registers triggers an immediate RISC-V Access Fault trap.

---

## 3. Copy-Always Message Channels (`chan_t`)

IPC in LugalOS is built upon message channels inspired by Plan 9 and CSP:
* **Message Channels**: Unidirectional or bidirectional channels connecting client tasks to server tasks.
* **Kernel Copy Guarantee**: When a task calls `chan_call(chan, send_buf, send_len, recv_buf, recv_len)`, the kernel copies data from the sender's domain into an internal kernel message buffer, blocks the sender, wakes the server, and copies the data into the server's domain.
* **Buffer Validation**: The kernel validates every pointer and byte range against the calling task's `mem_domain_t` using `uaccess_validate()` before copying. Invalid pointers result in syscall failure (`-EFAULT`), never a kernel panic.

---

## 4. System Call Boundary

System calls cross from U-Mode into the kernel via the RISC-V `ecall` instruction:
1. Hardware saves user context and switches to the task's private kernel stack.
2. The trap handler (`trap_dispatch()`) validates the syscall number.
3. User argument pointers are copied and checked against domain bounds.
4. Syscall executes synchronously in the kernel or yields to a message server.
5. Control returns to U-Mode via `mret` / `sret`.

---

## 5. Where Isolation Applies Today

The domain machinery is the same on every board, but not every component runs in a domain. Services and many drivers are **kernel-mode tasks**: they have their own stack and are reached through channels, but they share the kernel's address space. The `Isol` column of `ps` shows which is which on a running board (`PMP` or `Sv39` for a confined task, `-` for a kernel-mode one).

| Component | RP2350 | ESP32-P4 / ESP32-C6 |
| :--- | :--- | :--- |
| Drivers: UART, USB CDC, SPI SD, I2C, ST7735, LCD terminal, USB keyboard, clock display | U-mode, own domain (`driver_umode_enter()`, `drivers/driver_task.c`) | kernel-mode tasks (UART, SDMMC via `driver_task_start()`) |
| Services: httpd, MQTT, the 9P server, sensor hub, DHCP, setup, klogd, LLP | kernel-mode tasks | kernel-mode tasks |
| Vendor Wi-Fi code | - | U-mode on the C6 ([ADR 007](../../history/adr/007_c6_peripheral_confinement.md)) |
| ELF user programs | U-mode | U-mode |

For a kernel-mode task the channel still copies every message (ADR 004), so moving it into a domain later changes no caller. What the microkernel structure costs such a task in RAM (its stack, its endpoint buffers) is measured per board by the RAM ledger ([Testing Guide](../testing.md#ram-ledger)).

