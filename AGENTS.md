# AGENTS.md — LugalOS AI Developer & Agent Guide

This document is the reference guide for AI pair programmers, autonomous agents, and subagents working in the LugalOS repository. It describes the environment, hardware targets, documentation resources, toolchains, build presets, hardware flashing protocols, 9P filesystem synchronization, and testing procedures.

For deep board-specific specifications, peripheral registers, and hardware errata, see the dedicated [Board Reference Guides](doc/admin/boards/index.md).

---

## 1. System & Environment

* **Host Environment:** Linux (x86_64).
* **Repository & Resource Roots:**
  * LugalOS and external platform SDKs may reside under either `~/gith/` or `~/Source/gith/`:
    * Repository Root: `~/gith/domschl/lugalos` or `~/Source/gith/domschl/lugalos`
    * Pico Resources: `~/gith/pico` or `~/Source/gith/pico`
    * ESP Resources: `~/gith/esp` or `~/Source/gith/esp`
    * Sensors Resources: `~/gith/sensors` or `~/Source/gith/sensors`
  * Agents should dynamically check both roots when resolving file paths.
* **Compilers & Toolchains:**
  * 32-bit RISC-V: `riscv32-unknown-elf-gcc`
  * 64-bit RISC-V: `riscv64-unknown-elf-gcc`
  * Build System: `cmake` with `ninja` generator.
  * Python: Python 3.11+, managed via `uv`.
    * Default type checker: `pyrefly`.
    * Type hints: Always use modern typing syntax (`| None`, `list`, `dict`; never legacy `Optional`, `List`, `Dict`).
* **Critical Architectural Rules:**
  * **Freestanding C:** Built with `-nostdlib -ffreestanding`.
  * **Zero Floating-Point in Kernel / M-Mode:** The RISC-V boot code intentionally boots with `mstatus.FS = 0` (FPU disabled). Hardware float instructions or compiler soft-float `libm` calls will trigger **Illegal Instruction** traps. All mathematical routines must use pure fixed-point (e.g. scale $10^6$ for transcendental functions) or exact integer/bignum/rational arithmetic.
  * **Zero SRAM Overhead:** Optional features, symbols, and builtins must be stored in Flash `.rodata` tables sorted in ASCII order for $O(\log N)$ `bsearch()`.
  * **Web Architecture:** Simple HTML, vanilla JavaScript, and CSS. Avoid third-party web frameworks and heavy dependencies. For bidirectional real-time communication, prefer WebSockets + JSON.

---

## 2. External Hardware Resources & Documentation

* **RP2350 (Raspberry Pi Pico 2):** `${PICO_ROOT}/datasheet/`, `${PICO_ROOT}/pico-sdk/`
  * Flashing tool: `${PICO_ROOT}/pico-sdk-tools/picotool/picotool` (or discoverable via `find ~/gith/pico ~/Source/gith/pico -name picotool -type f`).
* **ESP32-P4 & ESP32-C6:** `${ESP_ROOT}/datasheet/`, `${ESP_ROOT}/esp-idf/`, `${ESP_ROOT}/esptool/`
* **Environmental Sensors Datasheets:** Stored in `${SENSORS_ROOT}/` (BME280, BME680, CCS811, HDC1080, MCP9808, MH-Z19B, MiCS-6814, SGP30, TMP117, TSL2561, TSL2591).
  * See [Sensor Datasheets Catalog](doc/user/sensors.md#4-hardware-datasheet-references) for full filenames.

---

## 3. Connected Hardware Targets & TTY Port Mappings

> [!IMPORTANT]
> **Dynamic Port Numbers & Plug-in Order:**
> Linux device node numbers (`/dev/ttyACM*`, `/dev/ttyUSB*`) **depend entirely on the order hardware is plugged in or enumerated by the kernel**.
> Agents must **never assume fixed port indices** (e.g. assuming `/dev/ttyACM0` is always a specific interface).
> Always identify ports by inspecting `/dev/serial/by-id/` or using the auto-detection logic in `tools/p4run.py`, `tools/c6run.py`, and `tools/p9sync.py`.

> [!WARNING]
> **Modem Control Reset Hazard:**
> Opening ESP32-P4 or ESP32-C6 serial ports with naive tools (`screen`, `cu`, raw `pyserial`) asserts RTS/DTR and resets or holds the chip in permanent reset.
> **ALWAYS use [`tools/p4run.py`](file:///home/dsc/Source/gith/domschl/lugalos/tools/p4run.py) or [`tools/c6run.py`](file:///home/dsc/Source/gith/domschl/lugalos/tools/c6run.py)**.

### Target Reference Matrix

| Target Board | Preset | Default Console & Baud | Reset / Special Notes | Detailed Guide |
| :--- | :--- | :--- | :--- | :--- |
| **RP2350-LCD-7 / Pico 2** | `rp2350-terminal`, `rp2350` | CDC ACM `if00` (115200) | CDC ACM `if02` is 9P link. 1200-baud touch enters BOOTSEL. | [RP2350 Guide](doc/admin/boards/rp2350.md) |
| **ESP32-P4-WIFI6-LCD-7B** | `esp32p4-lcd7b` | CH343P UART0 (921600) | Rev v3.2. Single port for console & reset. Parked at B0. | [LCD-7B Guide](doc/admin/boards/esp32p4_lcd7b.md) |
| **M5Stack Tab5** | `esp32p4-tab5` | USB-JTAG / UART0 (921600) | Rev v1.3. Dual expanders, ST7121 TDDI, IMU, battery fuel gauge. | [Tab5 Guide](doc/admin/boards/esp32p4_tab5.md) |
| **ESP32-P4-NANO** | `esp32p4` | CP2102 UART0 (921600) | Rev v1.3. CH34x port controls RTS/DTR. RMII Ethernet. | [P4 Consoles](doc/admin/consoles.md) |
| **ESP32-C6-Zero / MINI** | `esp32c6`, `esp32c6-mini` | USB-Serial/JTAG (115200) | Dual-image XIP. DTR low resets chip. Mirrored to UART0. | [ESP32-C6 Guide](doc/admin/boards/esp32c6.md) |

When multiple P4 or C6 boards are connected, specify `--board <name>` using port mappings configured in `~/.config/lugalos/p4-ports.env` and `c6-ports.env`.

---

## 4. Hardware Presets & Agent Requests

LugalOS supports distinct hardware presets. If a task requires testing against a specific board, the agent can request the user to attach or plug in hardware conforming to the required preset:

| Preset Name | Target Architecture | Description |
| :--- | :--- | :--- |
| `rp2350-terminal` | RP2350 Hazard3 RV32 | Waveshare RP2350-LCD-7 (7" LCD terminal, PSRAM, USB console, SD card) |
| `rp2350-chess` | RP2350 Hazard3 RV32 | Pico 2 with ST7735 LCD, TM1638 keypad, SD card, chess engine |
| `esp32p4` | ESP32-P4 RV32 | Waveshare ESP32-P4-NANO (dual-core RISC-V @ 360–400 MHz, RMII Ethernet, SDMMC, writable flash) |
| `esp32p4-lcd7b` | ESP32-P4 RV32 (rev v3.2) | Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (7" 1024×600 MIPI-DSI, GT911 touch, USB host, 32 MB PSRAM, C6 SDIO) |
| `esp32p4-tab5` | ESP32-P4 RV32 (rev v1.3) | M5Stack Tab5 (5" 720×1280 MIPI-DSI ST7121 touch, USB host, SDMMC, 32 MB PSRAM, dual expanders) |
| `rp2350-clock` | RP2350 LED Clock | Waveshare Pico-Clock-Green (SM16106 LED matrix + DCF77 radio) |
| `rp2350-gateway` | RP2350 Gateway | Network gateway persona (ENC28J60 Ethernet / USB 9P / UART1 downlink) |
| `rp2350-wifi` | RP2350 CYW43439 | Raspberry Pi Pico 2 W with CYW43439 Wi-Fi |
| `esp32c6` | ESP32-C6 RV32 | Waveshare ESP32-C6-Zero (RV32IMAC @ 160 MHz, 512 KB SRAM, 8 MB flash, USB-Serial/JTAG) |
| `rv32` | RISC-V 32-bit NOMMU | QEMU virt machine (32-bit microkernel test target) |
| `rv64` | RISC-V 64-bit Sv39 MMU | QEMU virt machine (64-bit virtual memory target) |
| `rv64-smp` | RISC-V 64-bit SMP | Dual-core SMP virtual machine |

To build any target:
```bash
ninja -C build/<preset>
```

---

## 5. Flashing & Storage Synchronization Procedures

* **RP2350 (Pico 2, RP2350-LCD-7):**
  1. Trigger BOOTSEL via 1200-baud touch on `if00` (or hold physical BOOTSEL button).
  2. Flash kernel: `picotool load -f build/rp2350-terminal/lugalos.uf2 -x`
* **ESP32-P4 Boards (Tab5, LCD-7B, NANO):**
  * Flash bootloader and kernel: `python3 tools/p4flash.py --board <board> --build build/<preset> --only boot,os --verify`
  * Interactive console: `python3 tools/p4run.py --board <board> --run`
* **ESP32-C6 Boards (Zero, MINI):**
  * Permanent flash: `tools/c6flash.py build/esp32c6`
  * Fast dev loop: `tools/c6run.py --kernel build/esp32c6 --cmd "preempttest" --cmd-wait 8`
* **MicroSD 9P Storage Sync (`tools/p9sync.py`):**
  * RP2350: `uv run --project host/p9lib python3 tools/p9sync.py --serial /dev/serial/by-id/...-if02 --src build/rp2350-terminal/sd_root --dst /sd0`
  * ESP32-P4: `tools/p9sync.py --p4 <board> --src build/<preset>/sd_root`
  * Policy: Additive and idempotent; strictly preserves user files (`/sd0/CHESS/`, history logs).

See [Firmware Flashing Guide](doc/admin/flashing.md) and [Plan 9 Networking](doc/admin/networking.md) for full options.

---

## 6. Testing Framework & Scripts

* **Pure Algorithmic Unit Tests:**
  ```bash
  python3 tests/test_lisp_cas.py        # CAS foundations, exact arithmetic, limits
  python3 tests/test_cas_simplify.py    # Canonical AST simplification, polynomials
  ```
* **Comprehensive Automated Test Runner:**
  ```bash
  python3 tests/runner.py               # Host harnesses (ASan/UBSan) and automated QEMU regression tests
  ```
* **Hardware-in-the-Loop Silicon Suites (`tests/hw/`):**
  Safe to run speculatively (skips cleanly when hardware is absent):
  * `python3 tests/hw/test_rp2350.py`: RP2350 peripherals, memory pools, console.
  * `python3 tests/hw/test_esp32p4.py --board <board>`: ESP32-P4 CLIC, PMP, I2C, PSRAM, SDMMC.
  * `python3 tests/hw/test_esp32c6.py --port <port>`: ESP32-C6 Wi-Fi and LLP radio.
  * `python3 tests/hw/test_wifi.py`: CYW43439 Wi-Fi and network stack.
  * `python3 tests/hw/lcdterm_shots.py`: Visual PBM screenshot regression suite. See [Testing Guide](doc/dev/testing.md#visual-regression-testing-lcdterm_shotspy) for scenario options and hotkeys.

---

## 7. Lisp & CAS Architectural Notes

* **Interpreter Dialect:** Compact Scheme-like Lisp.
  * Supported Special Forms: `quote`, `quasiquote`, `unquote`, `unquote-splicing`, `lambda`, `if`, `cond`, `let`, `let*`, `while`, `and`, `or`, `define`, `set!`.
  * **Recursion:** Use named `let` (`(let loop ((x 0)) ... (loop (+ x 1)))`) for tail-recursive loops. `letrec` is not built-in.
  * **File I/O:** `prim_load` and `prim_read_file` use `vfs_stat` to allocate scratch buffers dynamically according to file size.
* **Computer Algebra System (`cas/`):**
  * Stored in `cas/` in repository root. Embedded in images as `/sd0/cas` and `/flash0/cas`.
  * Core entry point: `(load "/sd0/cas/simplify.lisp")` followed by `(simplify expr)`.
  * Full manual: [CAS Manual](doc/user/cas.md).

---

## 8. Documentation System & Code Commentary Rules

### Detailed Topic Documentation Pointers
* **User Manual:** [`doc/user/`](doc/user/) — Shell commands, editors, ribbon window manager, chess engine, CAS, sensor telemetry.
* **Administrator Manual:** [`doc/admin/`](doc/admin/) — Hardware presets, flashing protocols, baud rates, 9P networking, storage.
* **Board Reference Guides:** [`doc/admin/boards/`](doc/admin/boards/) — Dedicated guides for Tab5, LCD-7B, C6, and RP2350.
* **Developer Manual:** [`doc/dev/`](doc/dev/) — Microkernel IPC, memory domains, scheduler & SMP lock hand-off, testing guide.
* **Architecture Decision Records (ADRs):** [`doc/history/adr/`](doc/history/adr/) — Durable architectural decisions.

### Source Code Commentary Standard
Source code comments must have a high signal-to-noise ratio:

1. **What to Include in Comments:**
   * **Invariants & Contracts:** Preconditions, postconditions, pointer nullability, and buffer ownership (who allocates and who frees).
   * **Hardware Quirks & Errata:** Silicon errata, register timing delays, revision differences (e.g. ESP32-P4 v1.3 vs v3.2).
   * **Concurrency & Memory Isolation:** Which lock guards what, why a lock is held across context switches, PMP granularity and alignment rules.
   * **Static Storage Ownership:** Every writable static or global reached from more than one task/handler requires a verdict comment:
     * `single task: <who>`, `guarded by: <lock>`, `interrupt-shared: <protocol>`, or `accepted: <why>`.
     * Verified by `tools/racescan.py --check` in `tests/runner.py`. Native shell commands take a `session_claim_t` (kernel/lock.h).
   * **Driver Tasks & Waits:** Call busy endpoints via `chan_call_wait()` (kernel/chan.h), never hand-written retry loops. Read clock *before* checking condition (`time_poll_until_us()`, kernel/time.h).
   * **Interrupt-Shared State:** Guard with `spin_lock_irqsave()` on both task and handler sides (or atomic aliases `+0x2000`/`+0x3000` on RP2350). Wake tasks only after releasing spinlock.
   * **Cache Maintenance (P4):** Serialized exclusively through `esp32p4_dcache_*()` / `esp32p4_extmem_*()`.
   * **Shared System Registers & Lock Order:** Read-modify-write on shared clock/reset gates via `sysreg_update()` (kernel/sysreg.h). Follow learned lock order (`lockcheck [pairs]`). On P4 panel, `g_frame_lock` is taken after `console_lock`.
   * **Non-Obvious Algorithms:** Mathematical derivations, pure fixed-point scaling rationale.

2. **What NEVER to Include in Comments:**
   * **No Milestone or Phase References:** Never write `/* M3, plan/phase12... */` or `/* Phase 48 */`.
   * **No Historical Narratives / Archaeology:** Never document obsolete shims ("Replaces the pre-B2 shim...").
   * **No Changelogs in Code:** Avoid "Fixed bug where X happened". Use git commit messages for history.
   * **No Dead Code:** Delete obsolete or commented-out code.

### Documentation as Code (Agent PR Rules)
* Whenever adding or altering shell commands, update [`doc/user/shell.md`](doc/user/shell.md).
* Whenever adding or modifying Lisp primitives, update [`doc/dev/on_device/lisp_scripting.md`](doc/dev/on_device/lisp_scripting.md).
* Whenever updating documentation in `doc/`, run `python3 tools/gendocs.py --all` to regenerate on-device plain-text help files in `tools/sd_root/help/`.
* Run `python3 tools/gendocs.py --check` before committing to verify zero documentation drift.
