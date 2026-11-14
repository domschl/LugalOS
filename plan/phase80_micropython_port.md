# Phase 80 — MicroPython Port & Unified User-Space Bindings

**Status**: PROPOSED (Feasibility Evaluated)  
**Target Hardware**:
- **Primary**:
  - **StarFive VisionFive 2 v1.3B** (`visionfive2` preset: RV64GC `lp64d`, 8 GiB LPDDR4, Sv39 MMU, `/nvme0` & `/sd0`).
  - **ESP32-P4 Terminals** (`esp32p4`, `esp32p4-lcd7b`, `esp32p4-tab5`: RV32IMAC `ilp32`, 32 MB PSRAM bulk zone, `/sd0`).
  - **RP2350-LCD-7 Terminal** (`rp2350-terminal`: RV32IMAC `ilp32`, 8 MB PSRAM bulk zone, `/sd0`).
- **Automated CI / Regression**: QEMU `rv32`, `rv64`, `rv64-smp` (`tests/runner.py`, `tests/quick.py`).
- **Non-PSRAM Microcontroller Exclusion**: `rp2350`, `rp2350-clock`, `rp2350-gateway`, `rp2350-wifi`, `rp2350-sensor`, `rp2350-chess`, `rp2350-smp`, and `esp32c6*` remain excluded so their 520 KB SRAM and flash baselines remain untouched.

**License Policy**: Strictly **MIT-compatible** throughout.

---

## 0. Feasibility Summary

**Verdict: Feasible and architecturally beneficial**, provided MicroPython is deployed as an **isolated U-mode ELF binary** (`/sd0/bin/mpy.elf` / `/nvme0/bin/mpy.elf`) loaded into PSRAM/RAM rather than compiled into the kernel image.

### Key Technical Constraints Discovered
1. **ESP32-P4 Flash Slot Headroom**:
   - `cmake/flash_layout_esp32p4.cmake` defines `LUGALOS_P4_SLOT_SIZE = 0x200000` (2 MB; `LUGALOS_P4_OSIMAGE_SIZE = 0x1E0000` = 1.875 MB).
   - `build/esp32p4-tab5/lugalos-xip.bin` is already ~1.78 MB (`1,783,748` bytes). Embedding a ~250–400 KB MicroPython runtime directly into the kernel XIP image would overflow the 2 MB OTA slot unless the P4 flash map were repartitioned.
   - Running MicroPython as a U-mode ELF on `/sd0/` or `/nvme0/` keeps **kernel Flash and internal SRAM overhead at 0 bytes** across all presets.
2. **PSRAM Execution & Memory Isolation**:
   - `arch/riscv/common/elf.c` already places U-mode ELF segments in PSRAM via `palloc_pages_bulk_aligned()` on RP2350-terminal and falls back to PSRAM on ESP32-P4.
   - `MEM_DOMAIN_MAX_REGIONS = 5` (PMP NAPOT constraint on Hazard3/P4) requires `mpy.elf` segments (`PF_X` `.text`/`.rodata` and `PF_W` `.data`/`.bss`) to be padded/aligned to power-of-two page counts (e.g., 64 or 128 pages for `.text`+`.rodata`, 128–512 pages for `.bss`/GC heap) so each segment consumes only 1 PMP entry.
   - MicroPython's C parser/VM recursion requires ~16–32 KB of C stack (more than `elf.c`'s default 1-page 4 KB `p->ustack`), which can be solved cleanly by placing a dedicated stack in `.bss` and switching `sp` in `_start` (or passing a stack-size hint).
3. **Floating-Point & ISA Compatibility**:
   - **RV32 (`rv32imac` / `ilp32`)**: No hardware FPU; `mstatus.FS = 0` in kernel. MicroPython supports `MICROPY_FLOAT_IMPL_NONE` (integer-only) or `MICROPY_FLOAT_IMPL_FLOAT` / `DOUBLE` using GCC soft-float (`libgcc` integer ops) + MicroPython's permissive `lib/libm/` (no hardware `f*` instructions, so no Illegal Instruction traps).
   - **RV64 (`rv64gc` / `lp64d` on `visionfive2`)**: Hardware FPU is present; U-mode `sstatus.FS` must be initialized/context-switched cleanly if `lp64d` hardware float is used, or `mpy.elf` can use soft-float / `rv64imac` `lp64` initially.
   - **Non-Local Returns (`nlr`)**: Start with `MICROPY_NLR_SETJMP` (portable) and optionally add a fast `nlr_riscv.c` (saving `ra`, `sp`, `s0`–`s11`).

---

## 1. License Compatibility (`MIT`)

LugalOS is licensed under the **MIT License** (`LICENSE`).

- **MicroPython Core**: `py/`, `shared/runtime/`, `shared/readline/` (MicroPython's own MIT-licensed `mp-readline`, *not* GNU readline), and core `extmod/` (`modjson.c`, `modre.c`, `modbinascii.c`, `modhashlib.c`, `extmod/asyncio/`) are **MIT-licensed**.
- **Math (`lib/libm/`)**: Derived from Sun/FreeBSD `fdlibm` (permissive BSD/Sun-like license, compatible with MIT).
- **Submodules to EXCLUDE**: Upstream `micropython/lib/` contains third-party libraries under non-MIT or redundant licenses (`lwip`, `mbedtls`, `berkeley-db`, `littlefs`, `pico-sdk`, `stm32lib`, etc.). These **must not be vendored**—LugalOS already provides its own MIT-clean `net/`, `net/tls.c`, `third_party/crypto/`, and `fs/` VFS.
- **Vendoring Strategy**: Vendor a stripped, pinned snapshot of only the MIT-clean MicroPython core (`py/`, `shared/`, selected `extmod/`, `lib/libm/`) under `third_party/micropython/` and document it in `LICENSE`.

---

## 2. Unifying Bindings: Lisp, C, and MicroPython

### Current Divergence
Currently, LugalOS has two separate binding worlds:
1. **Lisp (`user/lisp/lisp.c`)**: Runs in-kernel, calling internal kernel functions directly (`console_canvas()`, `lispui_view.c`, `sensor_*`, `i2c_*`, `gpio_*`, `vfs_*`).
2. **U-mode C (`user/progs/usys.h`, `user/chibicc/include/lugal.h`)**: Runs in isolated U-mode via `ecall` (`SYS_CHAN_CALL`, `SYS_READ_FILE`/`SYS_WRITE_FILE` capped at 512 bytes, `SYS_KOBJ_*`), and **lacks access** to `console_canvas()`, `ugui`, `sensors`, `net`, and full VFS (`open`/`read`/`write`/`stat`/`readdir`).

Adding a third ad-hoc binding layer inside MicroPython would worsen this divergence. Instead, Phase 80 unifies U-mode OS access around a **single shared C client library (`liblugal`)** backed by Plan 9 channel endpoints and VFS syscalls:

```
  [ U-mode C / chibicc ]      [ MicroPython (mpy.elf) ]
            |                            |
            +------------+---------------+
                         |
                 [ liblugal (C API) ]
           (canvas, ui, sensor, vfs, net, hal)
                         |
            ecall (VFS syscalls + SYS_CHAN_CALL)
                         |
   ================== Kernel Boundary ==================
                         |
     /srv/canvas   /srv/ui   /srv/sensor   /srv/net   VFS
```

### Required Kernel & ABI Extensions
1. **POSIX-like VFS Syscalls** (`arch/riscv/common/trap.c`, `user/progs/usys.h`):
   - `SYS_OPEN`, `SYS_CLOSE`, `SYS_READ`, `SYS_WRITE`, `SYS_LSEEK`, `SYS_STAT`, `SYS_READDIR`, `SYS_MKDIR`, `SYS_UNLINK`, `SYS_RENAME`.
   - Benefits both MicroPython (`open()`, `os`, `import` from `/sd0/` or `/nvme0/`) and U-mode C programs (removes the 512-byte `SYS_READ_FILE` limit).
2. **Named Channel Endpoints (`chan_register()` + `SYS_CHAN_CALL`)**:
   - **`"canvas"`**: Exposes `console_canvas(req, n, reply)` directly! Because `console_canvas()` already uses a compact binary request/reply packet protocol, exposing it over `SYS_CHAN_CALL` requires zero protocol redesign.
   - **`"ui"`**: Exposes immediate-mode `ugui` ribbon window primitives (`lispui_view.c` protocol) to U-mode C and Python.
   - **`"sensor"`**: Queries cached sensor list, channel readings, and calibration state without blocking on I2C.
   - **`"net"`**: Exposes TCP/UDP/DNS/HTTP(S) socket operations for Python `socket` / `urequests` and U-mode C networking.
   - **`"hal"`**: Capability-gated I2C / GPIO / PWM access for board-level scripting.
3. **Shared `liblugal` C Library (`libc/lugal/` or `user/liblugal/`)**:
   - Implements `lugal_canvas_*()`, `lugal_sensor_*()`, `lugal_net_*()`, and `<stdio.h>`/`<unistd.h>` file I/O on top of `usys.h`.
   - U-mode C programs and `chibicc` link/include `liblugal` directly.
   - MicroPython C modules (`lugal_canvas`, `lugal_ui`, `lugal_sensor`, `os`, `socket`, `machine`) are thin `mp_obj_t` wrappers over `liblugal`—guaranteeing **zero API drift** between C and Python.
   - Lisp retains its in-kernel fast path so non-PSRAM boards (`esp32c6`, `rp2350`) continue to work with zero overhead.

---

## 3. Complementary Usage Scenarios: Lisp vs. MicroPython

Lisp and MicroPython serve distinct, complementary roles in LugalOS:

| Dimension | LugalOS Lisp (`user/lisp/`) | MicroPython (`mpy.elf`) |
| :--- | :--- | :--- |
| **Target Footprint** | All boards (including 520 KB SRAM `esp32c6`, `rp2350`) | PSRAM & high-RAM boards (`visionfive2`, `esp32p4*`, `rp2350-terminal`) |
| **Execution Context** | In-kernel REPL, zero-SRAM `.rodata` builtins | Isolated U-mode process with PSRAM GC heap |
| **Primary Strengths** | Symbolic math / **CAS (`cas/`)**, quick shell expressions, minimal `ui-*` applets | **Hardware/sensor integration**, `uasyncio` background services, JSON/HTTP/MQTT telemetry, file/data automation |
| **Ecosystem** | Custom Scheme-like dialect | Vast MicroPython driver & script ecosystem; low barrier for user customization |

**Target Integration Scenarios for MicroPython**:
1. **Sensor Configuration & Telemetry Pipelines**: Reading I2C sensors (`bme280`, `ina226`, `sgp30`), applying custom calibration/filtering, formatting JSON, and publishing over MQTT/HTTP without recompiling firmware.
2. **Board & UI Customization**: Custom dashboards, status monitors, and touch applets on Tab5 / LCD-7B / VF2 using `lugal_canvas` / `lugal_ui` + `uasyncio`.
3. **Workstation & Automation Scripting**: Batch file operations on `/nvme0` or `/sd0`, automated hardware-in-the-loop test scripts, and AI/agent tool scripts.

---

## 4. Python Package Management

Full `pip` / PyPI is out of scope (CPython wheels, `setuptools`, native C extensions). Instead, use MicroPython's proven 3-tier model:

1. **Frozen Core Modules**: Essential stdlib (`uasyncio`, `json`, `os`, `sys`, `re`, `lugal.*`) frozen into `mpy.elf` `.rodata` for instant `import` and minimal RAM use.
2. **Filesystem `sys.path`**: Search `/nvme0/lib/python`, `/sd0/lib/python`, `/flash0/lib/python`, and `.` for `.py` and `.mpy` files (synced via `tools/p9sync.py` or `git clone` from Phase 82).
3. **`mip`-Compatible Package Installer (`mpy -m mip install <pkg>`)**:
   - MicroPython's MIT-licensed `mip` protocol (~150 lines of Python) fetches `package.json` manifests over HTTPS (`net/http.c` + `net/tls.c` via `"net"` channel) from `micropython-lib` or GitHub repositories and installs `.py` / `.mpy` files into `/sd0/lib/python/` (or `/nvme0/lib/python/`).
   - Optional host-side `mpy-cross` integration in `tools/` to pre-compile `.py` -> `.mpy` bytecode for faster imports and lower GC heap pressure.

---

## 5. Milestones

### 80.1 — U-Mode OS Boundary & `liblugal` Unification
1. Add POSIX-style VFS syscalls (`SYS_OPEN`, `SYS_CLOSE`, `SYS_READ`, `SYS_WRITE`, `SYS_LSEEK`, `SYS_STAT`, `SYS_READDIR`, `SYS_MKDIR`, `SYS_UNLINK`, `SYS_RENAME`) in `arch/riscv/common/trap.c` and `user/progs/usys.h`.
2. Register `"canvas"` (`console_canvas` wrapper) and `"sensor"` (`sensor_*` cache wrapper) as `chan_register()` endpoints.
3. Extract shared `liblugal` C client wrappers (`lugal_canvas_*`, `lugal_sensor_*`, file I/O) and verify with a U-mode C test program (`user/progs/ucanvas.c`) on QEMU `rv32`/`rv64`.

### 80.2 — Freestanding `libc` & ELF Loader Enhancements
1. Extend `libc/` for U-mode ELFs: `malloc`/`free`/`realloc` backed by `uheap` over a dedicated `.bss` arena (or `SYS_BRK`/PSRAM heap), `vsnprintf`, `strtol`, `setjmp`/`longjmp`, and minimal `<stdio.h>`/`<errno.h>`.
2. Ensure `arch/riscv/common/elf.c` supports larger multi-page `.bss`/heap segments cleanly within `MEM_DOMAIN_MAX_REGIONS = 5` (linker script `linker/user_mpy.ld` aligning segments to power-of-two pages) and sufficient U-mode C stack (16–32 KB).

### 80.3 — MicroPython Core Port (`third_party/micropython/` + `user/mpy/`)
1. Vendor MIT-clean MicroPython core (`py/`, `shared/readline/`, `shared/runtime/`, `lib/libm/`, selected `extmod/`) into `third_party/micropython/`.
2. Implement `mpconfigport.h`, `mphalport.c` (console I/O via `SYS_PRINT`/`SYS_CHAN_CALL`, ticks/sleep via `SYS_TIME_MS`/`SYS_SLEEP_MS`), and `main.c` (REPL + script execution `mpy <script.py>`).
3. Build `mpy.elf` for `rv32` (`ilp32`, soft-float) and `rv64` (`lp64`/`lp64d`), install to `/sd0/bin/mpy.elf` (and `/nvme0/bin/mpy.elf`), and add `python` / `mpy` shell command alias.

### 80.4 — LugalOS Python Modules (`lugal.*`, `os`, `socket`, `machine`)
1. Wrap `liblugal` into MicroPython C modules:
   - `lugal.canvas` (`open`, `close`, `fill`, `pixel`, `rect`, `frame`, `line`, `circle`, `text`, `size`, `swap`, `on_redraw`).
   - `lugal.sensor` (`list`, `read`, `cal`).
   - `lugal.ui` (via `"ui"` channel endpoint, matching `ui-*` immediate-mode semantics).
2. Implement `os` / `io` VFS bindings, `time` (`ticks_ms`, `sleep_ms`), `socket` / `urequests` (via `"net"` channel endpoint), and `machine` (`I2C`, `Pin` via `"hal"`).

### 80.5 — `uasyncio`, Public HTTPS Root-Cert Store, `mip` Package Manager & Frozen Stdlib
1. Freeze core stdlib (`uasyncio`, `json`, `os`, `mip`) into `mpy.elf`.
2. **Default Root-Cert Collection & Public HTTPS (`net/tls.c`, `kernel/tls_cert.c`)**:
   - Currently `net/tls.c` only validates a single pinned ECDSA P-256 CA (`/dev/tlsca`) and a single leaf cert (`tls_cert_check()`), advertising only `SIG_ECDSA_P256`.
   - Extend `tls_cert_check()` to walk **X.509 certificate chains** (leaf → intermediate → trusted root) and support **RSA-PSS / RSA-PKCS#1 v1.5** (or P-256/P-384) verification for public CAs where needed.
   - Ship a default curated root-CA bundle (e.g., `/sd0/system/etc/ssl/cacert.pem` or compact DER table `/flash0/system/etc/ssl/roots.bin`, generated at build time from a permissive/Mozilla CA bundle) and load it as the default client trust store when `/dev/tlsca` is not overridden.
3. Wire `mip` to HTTPS (`"net"` channel using `net/http.c` + `net/tls.c` with the default root-CA store) and `/sd0/lib/python` / `/nvme0/lib/python`.
4. Add example integration scripts in `tools/sd_root/demos/` (e.g., sensor logger, canvas chart).

### 80.6 — Verification & Documentation
1. Automated QEMU tests (`tests/quick.py`, `tests/runner.py`) for `rv32` and `rv64`: REPL smoke test, arithmetic/float, GC stress, VFS `open`/`read`/`write`, `import` from `/sd0/lib/python`, `lugal.canvas` & `lugal.sensor`.
2. Hardware tests on `esp32p4-tab5` / `esp32p4-lcd7b`, `rp2350-terminal`, and `visionfive2` (`tests/hw/`).
3. Document `python` / `mpy` and `lugal.*` APIs in `doc/user/` and `doc/dev/on_device/`, and run `python3 tools/gendocs.py --all` + `python3 tools/precommit.py`.
