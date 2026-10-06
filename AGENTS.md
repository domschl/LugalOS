# AGENTS.md — LugalOS AI Developer & Agent Guide

This document is the reference guide for AI pair programmers, autonomous agents, and subagents working in the LugalOS repository. It describes the environment, hardware targets, documentation resources, toolchains, build presets, hardware flashing protocols, 9P filesystem synchronization, and testing procedures.

---

## 1. System & Environment

* **Host Environment:** Linux (x86_64).
* **Repository & Resource Roots:**
  * LugalOS and external platform SDKs may reside under either `~/gith/` or `~/Source/gith/`:
    * Repository Root: `~/gith/domschl/lugalos` or `~/Source/gith/domschl/lugalos`
    * Pico Resources: `~/gith/pico` or `~/Source/gith/pico`
    * ESP Resources: `~/gith/esp` or `~/Source/gith/esp`
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

Reference documentation and vendor SDKs are located in `~/gith/` or `~/Source/gith/`:

### RP2350 (Raspberry Pi Pico 2)
* **Path:** `~/gith/pico/` or `~/Source/gith/pico/`
* **Datasheets & Hardware Docs:** `${PICO_ROOT}/datasheet/`
  * Contains RP2350 datasheet, architecture details, Hazard3 dual-core RISC-V specs, and pinouts.
* **Official SDK:** `${PICO_ROOT}/pico-sdk/`
* **Flashing Tool (`picotool`):**
  * Binary path: `${PICO_ROOT}/pico-sdk-tools/picotool/picotool` (or discoverable via `find ~/gith/pico ~/Source/gith/pico -name picotool -type f`).
  * Use this binary to query or flash connected RP2350 boards via USB.

### ESP32-P4 (Espressif ESP32-P4-NANO)
* **Path:** `~/gith/esp/` or `~/Source/gith/esp/`
* **Datasheets & Hardware Docs:** `${ESP_ROOT}/datasheet/`
  * Technical reference manual, memory map, peripheral registers, and pinouts.
* **ESP-IDF & Platform:** `${ESP_ROOT}/esp-idf/`, `${ESP_ROOT}/ESP32-P4-Platform/`
* **Flashing Tools:** `${ESP_ROOT}/esptool/`
* **Reference Factory Flash Backup:** `${ESP_ROOT}/p4nano-factory-flash/`

### Environmental Sensors Datasheets
* **Path:** `~/gith/sensors/` or `~/Source/gith/sensors/`
  * Contains official component datasheets for all supported environmental and light sensors:
    * BME280: `bst-bme280-ds002.pdf`
    * BME680: `bst-bme680-ds001.pdf`
    * CCS811: `CCS811_Datasheet-DS000459.pdf`
    * SGP30: `Sensirion_Gas_Sensors_Datasheet_SGP30.pdf`
    * TSL2561: `TSL2561.pdf`
    * TSL2591: `TSL25911_Datasheet_EN_v1.pdf`


---

## 3. Connected Hardware Targets & TTY Port Mappings

> [!IMPORTANT]
> **Dynamic Port Numbers & Plug-in Order:**
> Linux device node numbers (`/dev/ttyACM*`, `/dev/ttyUSB*`) **depend entirely on the order hardware is plugged in or enumerated by the kernel**.
> Agents must **never assume fixed port indices** (e.g. assuming `/dev/ttyACM0` is always a specific interface).
> Always identify ports by inspecting `/dev/serial/by-id/` or using the auto-detection logic in `tools/p4run.py`, `tools/p4flash.py`, and `tools/p9sync.py`.

### Port Topologies

#### 1. RP2350 Hardware (e.g. `rp2350-terminal`, `rp2350-chess`)
RP2350 devices running LugalOS expose **two CDC-ACM USB serial ports**:
* **Interface 0 (`if00`):** Interactive `lsh` shell & Lisp REPL console (115200 baud).
  * Symlink: `/dev/serial/by-id/usb-LugalOS_*_Dual_CDC_ACM_*-if00`
* **Interface 2 (`if02`):** USB CDC network / 9P file server link (`link_usb_cdc`).
  * Symlink: `/dev/serial/by-id/usb-LugalOS_*_Dual_CDC_ACM_*-if02`

#### 2. ESP32-P4-NANO (`esp32p4`)
ESP32-P4-NANO connects via **two separate serial bridges**:
* **CP2102 Bridge (`/dev/ttyUSB*`):** Connected to ESP32-P4 UART0 (interactive console).
  * Symlink: `/dev/serial/by-id/usb-Silicon_Labs_CP2102_*-port0`
* **CH34x Bridge (`/dev/ttyACM*`):** Connected to hardware control lines:
  * `RTS` controls `ESP_EN` (hardware chip reset).
  * `DTR` controls `GPIO35/BOOT` (download boot mode).
  * Symlink: `/dev/serial/by-id/usb-1a86_USB_Single_Serial_*-if00`
  > [!WARNING]
  > Opening the CP2102 or CH34x adapter with naive terminal utilities (such as `cu`, `screen`, or raw serial open without careful modem control) can inadvertently assert RTS/DTR and hold the ESP32-P4 in permanent reset.
  > **ALWAYS use `tools/p4run.py` or `tools/p4flash.py`** to communicate with or flash the ESP32-P4.

#### 3. ESP32-C6-Zero (`esp32c6`, phase 45)
One USB-C cable, one port: the chip's native USB-Serial/JTAG (VID:PID `303a:1001`),
console, loading and reset all on it.
* Symlink: `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00`
* **Always go through `tools/c6run.py` / `tools/c6flash.py`**, which open the port with
  DTR high / RTS low — opening with DTR low resets the chip (plan/phase45 §45.1).
* The kernel is **two images**: `.text`/`.rodata` execute in place from flash at
  `0x42000000`, the rest lives in SRAM. The map is `build/esp32c6/flash.manifest`
  (from `cmake/flash_layout_esp32c6.cmake`), never typed on a command line.
* Development loop (nothing written but the flash half, and only when it changed):
  ```bash
  ninja -C build/esp32c6
  tools/c6run.py --kernel build/esp32c6 --cmd "preempttest" --cmd-wait 8
  ```
* Leave it running by itself (stage 2 at `0x0` + the OS image, verified, then reset):
  `tools/c6flash.py build/esp32c6`
* Test it: `python3 tests/hw/test_esp32c6.py` (loads the kernel itself; `--no-load` tests
  what is running). 15 checks; skips when no board is attached.
* The top 16 KB of SRAM (`0x4087c000`..) is the **ROM's data** — never hand it out; the
  ROM's Wi-Fi code keeps its state there.
* Quick standalone checks (no kernel): `tools/build_minimal_esp32c6.sh run`,
  `tools/build_umode_probe_esp32c6.sh run`.

---

## 4. Hardware Presets & Agent Requests

LugalOS supports distinct hardware presets. If a task requires testing against a specific board, the agent can request the user to attach or plug in hardware conforming to the required preset:

| Preset Name | Target Architecture | Description |
| :--- | :--- | :--- |
| `rp2350-terminal` | RP2350 Hazard3 RV32 | Waveshare RP2350-LCD-7 (7" LCD terminal, PSRAM, USB console, SD card) |
| `rp2350-chess` | RP2350 Hazard3 RV32 | Pico 2 with ST7735 LCD, TM1638 keypad, SD card, chess engine |
| `esp32p4` | ESP32-P4 RV32 | Waveshare ESP32-P4-NANO (dual-core RISC-V @ 360–400 MHz, RMII Ethernet, SDMMC, writable flash) |
| `rp2350-clock` | RP2350 LED Clock | Waveshare Pico-Clock-Green (SM16106 LED matrix + DCF77 radio) |
| `rp2350-gateway` | RP2350 Gateway | Network gateway persona (ENC28J60 Ethernet / USB 9P / UART1 downlink) |
| `rp2350-wifi` | RP2350 CYW43439 | Raspberry Pi Pico 2 W with CYW43439 Wi-Fi |
| `esp32c6` | ESP32-C6 RV32 | Waveshare ESP32-C6-Zero (RV32IMAC @ 160 MHz, 512 KB SRAM, 8 MB flash, USB-Serial/JTAG console) |
| `rv32` | RISC-V 32-bit NOMMU | QEMU virt machine (32-bit microkernel test target) |
| `rv64` | RISC-V 64-bit Sv39 MMU | QEMU virt machine (64-bit virtual memory target) |
| `rv64-smp` | RISC-V 64-bit SMP | Dual-core SMP virtual machine |

To build any target:
```bash
ninja -C build/<preset>
```

---

## 5. Flashing & Storage Synchronization Procedures

### Flashing RP2350 Targets (e.g. `rp2350-terminal`)
1. **Build the Preset:**
   ```bash
   ninja -C build/rp2350-terminal
   ```
   > [!CAUTION]
   > Always flash `build/rp2350-terminal/lugalos.uf2` for the terminal board. Flashing the generic `rp2350` build will leave the LCD uninitialized (blank/black).

2. **Reboot to BOOTSEL via 1200-baud Touch:**
   LugalOS USB CDC stack implements the 1200-baud touch protocol on the console port (`if00`). Opening and immediately closing the console port at 1200 baud arms a deferred bootrom reset directly into BOOTSEL mode:
   ```python
   import serial
   with serial.Serial("/dev/ttyACM1", 1200):
       pass
   ```
3. **Flash using `picotool`:**
   ```bash
   /home/dsc/gith/pico/pico-sdk-tools/picotool/picotool load -f build/rp2350-terminal/lugalos.uf2 -x
   ```
4. **Manual BOOTSEL Fallback:**
   If the running firmware is unresponsive or in a fault state, ask the user for a manual BOOTSEL button press, or copy `build/rp2350-terminal/lugalos.uf2` to the mounted mass-storage volume (`RPI-RP2`).

### Updating the MicroSD Card via 9P (`tools/p9sync.py`)
To update files on the board's microSD card (`/sd0`) without removing the physical card or interrupting console sessions, use the LugalOS 9P synchronization utility:
```bash
uv run --project host/p9lib python3 tools/p9sync.py \
    --serial /dev/ttyACM2 \
    --src build/rp2350-terminal/sd_root \
    --dst /sd0
```
* **Preservation Policy:** `tools/p9sync.py` is additive and idempotent. It creates missing directories (e.g. `/sd0/cas/`), updates files that differ in size, and **strictly preserves user files** (such as `/sd0/CHESS/` games and `/sd0/SYSTEM/HISTORY.LIS`).

### Flashing & Running ESP32-P4-NANO
1. **Build the Preset:**
   ```bash
   ninja -C build/esp32p4
   ```
2. **Flash Second-Stage Bootloader and XIP OS Image:**
   ```bash
   python3 tools/p4flash.py --only boot,os --verify
   ```
3. **Run Interactive Console & Monitor:**
   ```bash
   python3 tools/p4run.py
   ```

---

## 6. Testing Framework & Scripts

### Quick Standalone Unit Tests
* **Phase 43.1 Math Foundations:**
  ```bash
  python3 tests/test_lisp_cas.py
  ```
  Verifies exact `expt`, `^`, `isqrt`, `to-decimal`, `symbol<?`, `string<?`, `math-sin`, `math-cos`, etc.
* **Phase 43.2 CAS Simplifier:**
  ```bash
  python3 tests/test_cas_simplify.py
  ```
  Verifies canonical AST forms, like-term collection, exact constant folding, identities, radicals, and ordering in pure Lisp.

### Comprehensive Test Runner
* **Full Automated Test Suite:**
  ```bash
  python3 tests/runner.py
  ```
  Runs host harnesses (ASan/UBSan FAT32, cc, 9P, IP) and automated QEMU regression tests across RV64, RV32, networking, filesystem, and Lisp engine.

### Hardware-in-the-Loop Test Suites
Located in `tests/hw/` (safe to run speculatively; skips rather than fails when hardware is absent):
* `tests/hw/test_rp2350.py`: Silicon verification suite for RP2350 peripherals, memory pools, and console.
* `tests/hw/test_esp32p4.py`: Comprehensive silicon test suite for ESP32-P4 CLIC, PMP, I2C, and SDMMC.
* `tests/hw/test_wifi.py`: CYW43 Wi-Fi and network interface testing.

---

## 7. Lisp & CAS Architectural Notes

* **Interpreter Dialect:** LugalOS Lisp is a compact Scheme-like Lisp.
  * Supported Special Forms: `quote`, `quasiquote`, `unquote`, `unquote-splicing`, `lambda`, `if`, `cond`, `let`, `let*`, `while`, `and`, `or`, `define`, `set!`.
  * **Recursion:** Use named `let` (`(let loop ((x 0)) ... (loop (+ x 1)))`) for tail-recursive loops. `letrec` is not built-in.
  * **File I/O:** `prim_load` and `prim_read_file` use `vfs_stat` to allocate scratch buffers dynamically according to the file size.
* **Computer Algebra System (`cas/`):**
  * Stored in `cas/` in the repository root.
  * Automatically staged by CMake into `SD_STAGE/cas` and embedded in `lugalos_sd.img` and `flashfs.bin` as `/sd0/cas` and `/flash0/cas`.
  * Core entry point: `(load "/sd0/cas/simplify.lisp")` followed by `(simplify expr)`.
