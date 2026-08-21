# LugalOS: Bare-Metal RISC-V Microkernel Operating System

**LugalOS** is a bare-metal, dependency-free microkernel operating system written in pure freestanding C11 and RISC-V assembly.

It is designed to scale dynamically from embedded **NOMMU** microcontrollers (like the **RP2350** / Pico 2) up to 64-bit **MMU** application processors (QEMU only currently).

Note: this is a guided AI development project. Coding was done by Claude Code. It is in early proof-of-concept stage.

---

![LugalOS and chess hardware](https://github.com/domschl/LugalOS/blob/main/doc/LugalOS_chess.jpg)<br>
_LugalOS running driver tasks for TFT, keypad, leds, SD-card, and a chess application on RP2350_

## Implementation Status

LugalOS is early-stage. The section below reflects what's actually implemented today, not the
long-term architectural goal described in the rest of this document and in [`plan/`](plan/) — if
a feature isn't listed here as working, treat it as roadmap, not present-tense fact.

**Working today**, verified by the automated test suite (`tests/runner.py`, 217 tests on QEMU RV32
NOMMU and RV64 MMU) and by a hardware-in-the-loop suite (`tests/hw/`, 22 tests against real RP2350
silicon):
- **Microkernel core**: preemptive scheduler with per-task kernel stacks; copy-always message
  channels as the IPC primitive; U-mode tasks with **hardware-enforced per-task memory domains** —
  PMP regions on the M-mode targets, Sv39 page tables on RV64, behind one interface; a syscall
  boundary that validates and copies every user pointer; and the console and 9P/filesystem servers
  running as scheduled tasks rather than inline calls.
- **Every RP2350 driver task confined to its own PMP domain** (`plan/phase12_microkernel_migration.md`,
  milestone M5): console/UART, the native USB CDC dual-ACM stack, SD/SPI block storage, the shared
  I2C RTC/EEPROM controller, the ST7735 TFT canvas, the TM1638 keypad/display, and a heartbeat LED
  task each run as an independent U-mode task, PMP-restricted to only the registers and RAM it
  actually uses — a bug in one driver's code cannot corrupt another's state or touch hardware it
  doesn't own. Verified per driver by a real fault: each has its own `<driver>isotest` shell command
  (`heartbeatisotest`, `tm1638isotest`, `i2cisotest`, `st7735isotest`, `blkisotest`, `uartisotest`,
  `usbisotest`) that deliberately stores outside the driver's own grant and asserts the store faults
  and a canary in kernel memory stays untouched.
- **`ps` reports which tasks that isolation actually covers**: an `Isol` column shows `PMP`, `Sv39`,
  or `-` per task, reading the same domain state the scheduler and PMP/Sv39 backend already track —
  so the isolation claims above are something you can point at in a running system, not just in the
  test suite.
- **More than one user program at a time**: each loaded program gets its own image, user stack and
  memory domain, and hands all three back when it ends — including its Sv39 page-table tree on the
  MMU build. `(spawn "path")` starts one without waiting; `exec` still runs one to completion.
- **Ports bound to protocols at boot**: a physical channel can be a console, a dedicated 9P link,
  or both demultiplexed, chosen by name in `init.lisp` — with one owner per wire, so binding the
  same UART to two protocols is refused rather than silently allowed. `cat /proc/ports` shows what
  each name is and which wire it drives.
- **Memory taken only while it is used**: the C compiler and the editors hold their working
  memory (~150 KB together) on the heap for the duration of a command and return it afterwards,
  rather than reserving it in the image. Every driver task's own U-mode stack is sized from a real
  measured worst-case call depth (not a flat guess) and tiled into a zero-gap linker layout rather
  than scattered — together with the compiler/editor reclaim, this is what took a representative
  RP2350 board's idle free-page baseline from 33 pages to 55 after a driver-task memory audit
  (`plan/phase12_microkernel_migration.md`'s M5 Heap-Reclaim section has the full accounting).
- **User programs larger than two pages**: the image is sized from the program headers and rounded
  to a power-of-two page run, with each segment granted what its ELF `p_flags` declare — so W^X
  comes from the linker rather than from the loader assuming a layout. A segment whose page count
  is not a power of two is granted as several NAPOT pieces.
- **A real process ABI**: programs receive `argc`/`argv` (built in their own stack page, so no
  kernel pointer crosses the boundary), return an exit status the shell and `/proc/ps` report, and
  reach named services over copy-always channels via `SYS_CHAN_CALL` — with every buffer validated
  against the caller's own domain. The old register-IPC entry points are deleted, their numbers
  permanently retired.
- **User programs**: a separately linked ELF is loaded from the filesystem into pages the allocator
  hands out, and runs as a U-mode task confined to three of them — text (R|X), data (R|W), stack
  (R|W). `exec` is that path, so a program compiled on the machine by `cc` runs confined too. The
  loader validates every header offset against the file size before using it.
- Boots to an interactive shell (`lsh`) on all supported targets, including two distinct RP2350
  board personas (`rp2350-chess`, `rp2350-clock` — see [Build presets](#build-presets)).
- FAT32 filesystem engine — subdirectories, `mkdir`/`rmdir`/`cp`/`rm`, VirtIO and physical SPI SD
  backends, embedded flash ROM disk, RAM disk.
- The embedded Scheme/Lisp interpreter, including `define`/`lambda` (self-recursion and the
  `(define (fn args...) body...)` signature form both work), `if`, `begin`, `let`, `cond`,
  `quote`, and around 40 built-in primitives — run `(help)` for the current list.
- The native C11 compiler (`chibicc`), producing real RISC-V ELF binaries, and the Thompson
  `ed`-style line editor.
- The native RP2350 USB CDC ACM console (`/dev/ttyACM0`), written from scratch against the
  hardware, now running as a U-mode task like every other RP2350 driver.
- **An onboard chess engine** (`user/chess/`) with a console REPL (`chess-console`), alpha-beta
  search, checkmate/stalemate detection, and full game state (undo/redo/FEN save-load) — reachable
  interactively over the same `lsh` shell, on both QEMU and real RP2350 hardware.
- **A second RP2350 board persona**: `rp2350-clock` targets the Waveshare Pico-Clock-Green baseboard
  — a 7-segment clock display with LDR-driven auto-brightness, wired through the same driver-task
  architecture as the default `rp2350-chess` persona, demonstrating that "which hardware this board
  has" is a per-persona table (`cmake/board-rp2350-clock.cmake`), not a fork of the kernel.

---

## Key Features & Architecture

* **Microkernel Syscall Interface**: RISC-V `ecall`-routed syscall dispatch with validated copy-in/copy-out at the boundary — a user pointer is checked against the calling task's own memory domain and then copied, so the kernel never dereferences a caller-supplied address. Services are reached by *message passing* over copy-always channels (`kernel/chan.h`), which a U-mode program reaches through `SYS_CHAN_CALL`; the older register-based `sys_ipc_*` entry points were never more than stubs and have been deleted, their syscall numbers permanently retired.
* **Plan 9 Inspired Universal Namespace**: Everything is addressed through top-level resource paths:
  * `/sd0/` — FAT32 VirtIO persistent SD storage volume (`/sd0/docs/readme.txt`).
  * `/ram0/` — FAT32 in-memory RAMDisk storage volume (`/ram0/notes.txt`).
  * `/proc/` — Synthetic kernel metrics, generated on read and served as real byte streams (so a
    remote node can read them over 9P): `/proc/ps` (the live task table), `/proc/meminfo` (live page
    allocator counters), `/proc/version`, `/proc/df`, `/proc/kmsg` (the kernel log ring),
    `/proc/devices` (the probed device registry), `/proc/buildid`.
  * `/dev/` — Hardware device nodes (`/dev/uart`, `/dev/null`, `/dev/zero`).
  * `/srv/` — Named service endpoints, reached by copy-always message passing: `/srv/console` (the
    console server — anything that can write here emits on the terminal, including a remote node
    over 9P) and `/srv/p9` (this node's own 9P/filesystem server, which is what `(mount-local ...)`
    attaches).
* **Microkernel Core** (see [Implementation Status](#implementation-status) for what is not yet done):
  * **Preemptive scheduler** with per-task kernel stacks drawn from a real page allocator
    (`kernel/palloc.c`). A 100 Hz timer interrupt switches tasks at arbitrary instructions, so a
    task that never yields cannot monopolise the machine; tasks may also yield explicitly. The
    per-target timers (SIO `mtime`, CLINT, and Sstc `stimecmp`) sit behind one interface, and the
    RP2350 tick rate is *measured* at boot rather than assumed.
  * **Copy-always message channels** (`kernel/chan.h`) as the only IPC primitive. Both copies are
    performed even on NOMMU builds where they are provably redundant — the discipline is what lets
    one set of server sources be correct under both memory models, and it is why a local service and
    a service on another machine are the same code path.
  * **U-mode tasks with hardware-enforced per-task memory domains**, behind a single interface
    (`kernel/mem_domain.h`): **PMP regions** on the NOMMU/M-mode targets and **Sv39 page tables** on
    RV64. A task that stores into kernel memory faults and is terminated; the kernel survives. Not
    only user programs run confined this way — every RP2350 driver task does too (console/UART, USB
    CDC, SD/SPI block, I2C RTC/EEPROM, ST7735, TM1638, heartbeat), each restricted to only the
    registers and RAM it actually needs.
  * **Validated syscall boundary** (`kernel/uaccess.h`): every user pointer is checked against the
    calling task's own domain and then copied, so the kernel never dereferences a caller-supplied
    address and cannot be used as a confused deputy.
  * **Servers as scheduled tasks**: the console and the 9P/filesystem server run as tasks rather
    than inline calls. Kernel diagnostics (`printk`) and user-facing output (`cprintf`) are separate
    streams, so handing a channel to a login shell does not silence the log, and detaching the log
    does not silence the shell.

  **Verifying these claims yourself** — the shell exposes the same probes the test suite uses, so
  none of the above has to be taken on trust:

  | Command | Shows |
  |---|---|
  | `pmpinfo` / `pmpdump` | What PMP this silicon actually implements — usable regions, granularity, and a per-register dump. On RP2350 it reports 8 configurable regions and a 32-byte granule. |
  | `usertest` | A task really dropping to U-mode. Asserted on the *hardware-set trap cause* (8 = ecall from U-mode), which the kernel cannot fake. |
  | `isolationtest` | A U-mode task storing into kernel memory: it faults, the task is terminated, and a canary in kernel `.data` is verifiably untouched. |
  | `deputytest` | A U-mode task asking the *kernel* to write kernel memory on its behalf: refused — while a pointer the task does own still works. |
  | `taskdemo` | Two tasks interleaving at explicit yield points, which is what distinguishes real switching from a no-op yield. |
  | `preempttest` | A task that **never yields** still gets switched away from. This cannot pass without a timer interrupt, which is exactly why it exists separately from `taskdemo`. |
  | `klog detach console` | Kernel diagnostics stop reaching the terminal while the shell keeps working. |
  | `heartbeatisotest`, `tm1638isotest`, `i2cisotest`, `st7735isotest`, `blkisotest`, `uartisotest`, `usbisotest` (RP2350) | The same isolation-fault proof as `isolationtest`, run against each driver task's *actual production domain* — the exact PMP grant that driver runs under, not a synthetic stand-in. |
  | `ps` | The live task table, including an `Isol` column: `PMP`/`Sv39` for a task with a real memory domain attached, `-` for one that doesn't have one (the kernel task itself, or a driver not yet converted). |

* **Storage Engine & VirtIO Block Device**:
  * Native FAT32 filesystem engine supporting 32-bit cluster allocation, subdirectories (`.`, `..`), BPB formatting, file read/write, deletion, directory creation (`mkdir`), removal (`rmdir`), and copying (`cp`).
  * Hardware **VirtIO MMIO Block Driver** backed by a persistent shared disk image (`build/lugalos_sd.img`) used across both 32-bit and 64-bit builds.
* **Native C11 Compiler (`chibicc`)**: Integrated C11 compiler (`cc <src.c> <dst.elf>`) generating native RISC-V ELF binaries directly on LugalOS!
* **Unified Lisp Machine Shell (`lsh`)**:
  * **POSIX $\rightarrow$ S-Expression Transformation**: All standard POSIX shell inputs (`ls /sd0`, `cp a b`, `cc src dst`) are automatically transformed into Lisp S-Expressions (`(ls "/sd0")`, `(cp "a" "b")`) and executed directly by the core Lisp engine!
  * **Scheme / Lisp Core**: Support for `define`, `lambda`, `quote` (`'`), `if`, `begin`, `let`, `let*`, `while`, `cond`, arithmetic (`+`, `-`, `*`, `=`), memory `peek`/`poke`, and string data types. Tail calls are optimized (constant stack/call-depth for self- and mutually-recursive loops in tail position).
  * **System Boot Scripts**: Automatically loads `/sd0/system/stdlib.lisp` and executes `/sd0/system/init.lisp` at system startup.
  * **Dual-Mode Interactive Line Editor & Emacs Multi-Line Canvas**: Single-line editing with ANSI escape sequences (`Ctrl-A/E/K/L/P/N`, Arrow keys, Delete), clean session history logging, and a full Emacs-style multi-line editor (`e [filename]` or `Ctrl-X Ctrl-M`) featuring a top optical separator, line numbers (`%3d │ `), an active status line, and keybindings:
    * `Ctrl-X Ctrl-E`: Evaluate buffer in Lisp engine
    * `Ctrl-X Ctrl-S`: Save buffer to active filename
    * `Ctrl-X Ctrl-F`: Find/load file into editor (status line prompt)
    * `Ctrl-X Ctrl-R`: Insert file at cursor position (status line prompt)
    * `Ctrl-X Ctrl-W`: Write buffer to new filename (status line prompt)
    * `Ctrl-X Ctrl-C`: Exit editor (prompts on status line if buffer modified)
* **Native RISC-V ELF Compiler (`lisp-to-elf`)**: Compiles Lisp AST S-expressions directly to native RISC-V machine code (`add`, `sub`, `mul`, `ret`) and packages them into **ELF32 / ELF64** binaries on disk!
* **Extended Unix Teletype Line Editor (`ed`)**: Classic Thompson Unix `ed` editor with current line pointer `dot`, line range addressing (`.`, `$`, `,`, `%`, `N,M`), insert (`i`), append (`a`), change (`c`), delete (`d`), print (`p`), numbered print (`n`), substitution (`s/old/new/`), search (`/pattern/`), and file I/O (`e`, `w`, `f`).
* **Native RP2350 USB CDC ACM Driver**: Bare-metal USB 1.1 device stack (`drivers/usb_cdc.c`) driving the RP2350's onboard USB controller directly — no TinyUSB/Pico SDK runtime dependency. Enumerates as a composite dual-ACM device, presenting `/dev/ttyACM0` as a fully interactive `lsh` console over the same USB cable used for flashing (mirrored alongside the physical UART debug console), with DTR-gated output so a freshly-opened terminal never receives a stale backlog of boot-time log lines. `/dev/ttyACM1` is `link_usb_cdc` (plan/phase5_distributed_design.md's A3b): a real bulk 9P transport, verified against physical hardware by [`tests/hw/`](tests/hw/), including talking to a live QEMU node over it.
* **Automated Integration Test Harness**: Non-interactive QEMU PTY integration runner (`tests/runner.py`) executing 217 automated test cases across RV32 (NOMMU) and RV64 (Sv39 MMU) builds (see `tests/runner.py` for the current count, as this grows over time), plus a hardware-in-the-loop suite (`tests/hw/`, 22 tests) that drives real RP2350 silicon over USB — including flashing the board itself via the "1200-baud touch" and re-verifying against `/proc/buildid`.

---

## Directory Structure

```
lugalos/
├── arch/riscv/
│   ├── common/              # RISC-V assembly entry point, traps, PMP/Sv39 memory domains, ELF loader
│   ├── include/arch/        # CSRs, Trap frames, VMM, ELF headers
│   ├── rv32_nommu/          # 32-bit physical identity memory mapping (PMP-backed domains)
│   ├── rv64_mmu/            # Sv39 page-table backend for memory domains
│   ├── rp2350/              # RP2350 boot header, binary_info metadata, BOOTSEL/bootrom glue
├── cmake/                   # Cross-compilation toolchains and per-board-persona config (RV32, RV64, RP2350 x2)
├── drivers/                 # UART (16550/PL011/RP2350), USB CDC, SD/SPI block, I2C RTC/EEPROM,
│                             # ST7735 TFT, TM1638 keypad, VirtIO Block/Console, RAMDisk — every
│                             # RP2350 driver here also runs as its own U-mode task (see plan/phase12_*.md)
├── fs/                      # FAT32 filesystem engine (Subdirectories, BPB) & Plan 9 VFS Server
├── kernel/                  # Microkernel main, scheduler, IPC, shell, printk
├── libc/                    # Freestanding C string library
├── linker/                  # Linker scripts (QEMU virt RV32/64, RP2350 XIP Flash)
├── plan/                    # Dated, phase-by-phase design and completion notes (the project's own history)
├── tools/                   # SD root template, FAT32 disk image generator, UF2 packager
└── user/
    ├── chess/                # Onboard chess engine (movegen, search, eval) + console REPL
    ├── chibicc/               # Native C11 compiler (`chibicc`)
    ├── ed/                    # Extended Unix teletype line editor (`ed`)
    └── lisp/                  # Scheme REPL & RISC-V S-expression ELF compiler
```

---

## Building and Running in QEMU

### Toolchain & Prerequisites
LugalOS uses a **single, unified 64-bit cross-compiler toolchain** (`riscv64-elf-gcc`) for all targets (both 64-bit MMU and 32-bit NOMMU / RP2350). The 64-bit toolchain target compiler compiles 32-bit RISC-V code cleanly via `-march=rv32imac_zicsr_zbs -mabi=ilp32`.

* `riscv64-elf-gcc` (Unified 64-bit cross-compiler toolchain)
* `cmake` and `ninja`
* `python3` (for FAT32 SD disk image pre-population and Flash ROMDisk generation)
* `qemu-system-riscv32` and `qemu-system-riscv64`

#### Linux (Debian / Ubuntu)
```bash
sudo apt update
sudo apt install gcc-riscv64-unknown-elf cmake ninja-build python3 qemu-system-misc
```

#### macOS (Homebrew)
```bash
brew install riscv64-elf-gcc cmake ninja qemu python3
```

### Build presets

`CMakePresets.json` (L1, `plan/phase11_pico_clock_green.md`) bundles the toolchain file, target and board/feature-flag
choices for each build persona this tree supports, so a build is one named preset instead of a growing pile of `-D`
flags to remember:

```bash
cmake --list-presets
```
```
Available configure presets:

  "rv32-nommu"   - QEMU RV32 (NOMMU)
  "rv64-mmu"     - QEMU RV64 (Sv39 MMU)
  "rp2350-chess" - RP2350 (Pico 2) — chess-computer persona
  "rp2350-clock" - RP2350 (Pico 2 / Pico 2 W) — Pico-Clock-Green persona
```

Configure + build any of them the same way:
```bash
cmake --preset <name>
cmake --build --preset <name>
```

Each preset's `binaryDir` is under `build/` (`rp2350-chess` keeps the historical `build/rp2350` path the hardware
test tooling in `tests/hw/` already expects; the rest match their preset name). Adding a new board persona later
means adding one more entry to `CMakePresets.json`, not a new directory of hand-maintained CMakeLists.txt files —
see `cmake/board-rp2350-clock.cmake` for what a second RP2350 persona actually differs by (a handful of pin facts and
feature flags, not a different build).

### Build & Run RV32 (NOMMU) Target
```bash
cmake --preset rv32-nommu
cmake --build --preset rv32-nommu
./scripts/run-qemu-rv32.sh
```

### Build & Run RV64 (Sv39 MMU) Target
```bash
cmake --preset rv64-mmu
cmake --build --preset rv64-mmu
./scripts/run-qemu-rv64.sh
```

---

## Running on Hardware: Raspberry Pi Pico 2 (RP2350)

LugalOS boots on the **Raspberry Pi Pico 2** (RP2350 RISC-V Hazard3 core). The interactive `lsh` console is reachable two ways: over a CP2101/CP2102 UART-to-USB adapter wired to GPIO0/GPIO1 (below), or natively over the Pico 2's own USB port via the onboard USB CDC ACM driver — no extra adapter needed. Both are mirrored to the same shell session.

### Hardware Required

| Component | Details |
|---|---|
| Raspberry Pi Pico 2 | RP2350 board (Hazard3 RISC-V core) |
| USB–Serial adapter | CP2101 or CP2102 (3.3 V logic, 5 V power out) |
| 4 jumper wires | Female–female or as appropriate |

### CP2101 → RP2350 (Pico 2) Wiring

```
CP2101 Adapter          Raspberry Pi Pico 2 (RP2350)
──────────────          ─────────────────────────────
      5V  ──────────►  Pin 40  VBUS       (powers the board via onboard 3.3V regulator)
     GND  ──────────►  Pin 38  GND        (common ground)
     TXD  ──────────►  Pin  2  GPIO1      (UART0 RX — CP2101 transmits → RP2350 receives)
     RXD  ◄──────────  Pin  1  GPIO0      (UART0 TX — RP2350 transmits → CP2101 receives)
```

> **Note**: CP2101 signal levels are 3.3 V — connect directly to GPIO0/GPIO1 without level shifters.  
> **Do not** connect the adapter's `3V3` output to anything; `5V → VBUS` is the sole power source.

#### Pico 2 Pin Reference

```
  ┌───────────────────────────────────────┐
  │  [USB]                                │
  │                                       │
  │  Pin 1  GPIO0  UART0 TX  ◄─── RXD    │
  │  Pin 2  GPIO1  UART0 RX  ──► TXD     │
  │  Pin 3  GND              ──► GND      │
  │  Pin 6  GPIO4  I2C0 SDA  ◄─► SDA     │
  │  Pin 7  GPIO5  I2C0 SCL  ──► SCL     │
  │  ...                                  │
  │  Pin 14 GPIO10 SPI1 SCK  ──► CLK      │
  │  Pin 15 GPIO11 SPI1 MOSI ──► MOSI     │
  │  Pin 16 GPIO12 SPI1 MISO ◄── MISO     │
  │  Pin 17 GPIO13 SPI1 CS   ──► CS       │
  │  Pin 38 GND              (alt GND)    │
  │  Pin 40 VBUS  5V input   ◄─── 5V     │
  └───────────────────────────────────────┘
```

### SPI MicroSD Card Adapter Wiring (Pico 2 SPI1)

```
MicroSD Module          Raspberry Pi Pico 2 (RP2350)
──────────────          ─────────────────────────────
      VCC ──────────►  Pin 36  3V3(OUT) / VBUS
      GND ──────────►  Pin 18  GND
      CLK ──────────►  Pin 14  GPIO10 (SPI1 SCK)
     MOSI ──────────►  Pin 15  GPIO11 (SPI1 MOSI)
     MISO ◄──────────  Pin 16  GPIO12 (SPI1 MISO)
       CS ──────────►  Pin 17  GPIO13 (SPI1 CS)
```

### I2C RTC / EEPROM Wiring (Pico 2 I2C0)

```
DS1307/DS3231 RTC Module   Raspberry Pi Pico 2 (RP2350)
──────────────────────     ─────────────────────────────
      VCC ──────────►  Pin 36  3V3(OUT)
      GND ──────────►  Pin 38  GND
      SDA ──────────►  Pin  6  GPIO4  (I2C0 SDA)
      SCL ──────────►  Pin  7  GPIO5  (I2C0 SCL)
```

> Internal pull-ups on GP4/GP5 are enabled by the driver, so no external pull-up resistors are
> required. The same bus reaches the RTC at `0x68` (`i2c_rtc.c`) and, if present, an AT24C32
> EEPROM at `0x57` (`at24c32.c`) — `(i2c-scan)` lists whatever actually responds.

### Build for RP2350

```bash
cmake --preset rp2350-chess
cmake --build --preset rp2350-chess
# Generates: build/rp2350/lugalos.uf2
```

This is the default board persona: SD card via SPI1, ST7735 TFT + TM1638 keypad + chess engine all built in
(`plan/phase9_chess_computer.md`). The Waveshare Pico-Clock-Green baseboard (`plan/phase11_pico_clock_green.md`)
wires several of those same pins to different hardware, so it's a separate `rp2350-clock` preset rather than a
build flag on this one — see `cmake/board-rp2350-clock.cmake`.

**Skip building from source**: a maintainer build populates a local `dist/` directory with pre-built
UF2 images for both RP2350 personas, one per release (`dist/README.md` documents the exact naming and
how to regenerate it — the images themselves are build output, not checked into git, same as `build/`).
If you have one, flash `dist/lugalos-<version>-rp2350-chess.uf2` or `dist/lugalos-<version>-rp2350-clock.uf2`
directly with the same steps below.

### Flash to Pico 2

1. Hold **BOOTSEL** button on Pico 2 while plugging in USB (or while powering on via CP2101 5V).  
   The board mounts as a USB mass storage device called `RP2350`.

2. Copy the UF2 firmware:
   ```bash
   # Linux
   cp build/rp2350/lugalos.uf2 /media/$USER/RP2350/
   # macOS
   cp build/rp2350/lugalos.uf2 /Volumes/RP2350/
   ```

**After the first flash, this is automatic.** The firmware implements the Arduino-style
"1200-baud touch": opening the console CDC port at 1200 baud and dropping DTR makes the device
reboot itself into BOOTSEL via the bootrom, so no button press is needed.

```bash
cd tests/hw
uv run flash.py --verify        # touch -> wait for the volume -> copy -> confirm /proc/buildid
```

`--verify` compares the board's `/proc/buildid` against what the local tree builds, which turns
"the board is running older firmware" into an explicit message instead of a confusing test failure.
The bootstrap flash still has to be manual, because firmware that predates the touch cannot respond
to it.


3. The Pico 2 will flash, reboot automatically, and start LugalOS.

### Open Serial Console

Via the CP2101/CP2102 UART adapter:
```bash
# Linux
picocom -b 115200 /dev/ttyUSB0

# macOS
picocom -b 115200 /dev/tty.usbserial-*
```

Or, with no extra adapter, directly over the Pico 2's own USB port once it enumerates as a composite CDC ACM device:
```bash
# Linux
picocom -b 115200 /dev/ttyACM0

# macOS
picocom -b 115200 /dev/tty.usbmodem*
```
`/dev/ttyACM0` (Linux) or `/dev/tty.usbmodem*` (macOS) carries the same interactive `lsh` session as the UART console above (output is mirrored to both). Output only starts flowing once the terminal asserts DTR (i.e. once something actually opens the port), so connecting doesn't dump a backlog of boot-time log lines. `/dev/ttyACM1` / the second CDC ACM interface is `link_usb_cdc` — a real 9P transport, not a console; see [`tests/hw/`](tests/hw/) for hardware-in-the-loop tests exercising it (including bridging it to a live QEMU node). The `p9share` shell command offers the same coexisting-9P-and-console story over the single physical UART instead, for a one-cable setup.

Expected output after boot (`rp2350-chess` persona; `cat /proc/kmsg` shows the full log any time
after boot, this is what streams live):
```
==================================================
       LugalOS Lisp Machine v0.9.0 (build 252.f0b1a461)
==================================================
[Dev] Registry: rtc, eeprom, usb, uart, uartslip, uartdemux, usbnet, usbcon
[PAlloc] Page allocator: 55 pages of 4096 bytes at 0x20049000 (220 KB)
[9P Chan] Local 9P server endpoint '/srv/p9' online (copy-always IPC).
[Sched] Cooperative round-robin scheduler online (max 24 tasks)
[Sched] Created task #1 'usbcdc' (stack 20049000, 4 KB)
[USB] Background servicing task #1 running.
[Sched] Created task #2 'uart' (stack 2004a000, 4 KB)
[UART] Driver running as task #2, reachable via chan_call("uart", ...)
[Sched] Created task #3 'heartbeat' (stack 2004b000, 4 KB)
[Sched] Created task #4 'sdblk' (stack 2004c000, 4 KB)
[SPI SD] Driver running as task #4, reachable via chan_call("sdblk", ...)
[Sched] Created task #5 'i2c' (stack 2004d000, 4 KB)
[Sched] Created task #6 'st7735' (stack 2004e000, 4 KB)
[Sched] Created task #7 'tm1638' (stack 2004f000, 4 KB)
[Sched] Created task #8 'p9srv' (stack 20060000, 8 KB)
[Shell] Interactive Lugal Shell (lsh) initialized with Plan 9 Universal Namespace.
lsh> ps
PID  State    Name          Exit   Isol
---  -------  ------------  ----   ----
  0  RUNNING  kernel        -      -
  1  READY    usbcdc        -      PMP
  2  BLOCKED  uart          -      PMP
  3  READY    heartbeat     -      PMP
  4  BLOCKED  sdblk         -      PMP
  5  BLOCKED  i2c           -      PMP
  6  BLOCKED  st7735        -      PMP
  7  BLOCKED  tm1638        -      PMP
  8  READY    p9srv         -      -
```

### RP2350 UF2 Packager

The build system automatically invokes [`tools/elf2uf2_rp2350.py`](tools/elf2uf2_rp2350.py) which:
- Reads `_start` and `_stack_top` symbol addresses from the ELF
- Embeds a valid **PICOBIN IMAGE_DEF** block (RP2350 BootROM metadata) into the boot2 Flash region
- Generates a standard UF2 file with correct per-family block counters (`0xE48BFF5A` code, `0xE48BFF57` IMAGE_DEF)

---


## Interactive Workflow Examples

### 1. FAT32 Subdirectories and File Operations
```bash
lsh> mkdir /sd0/projects
lsh> cp /sd0/hello.c /sd0/projects/hello_copy.c
lsh> ls /sd0/projects
Name        Size (Bytes)  Attr   Type
----------  ------------  -----  -----
.            0  0x10   <DIR>
..           0  0x10   <DIR>
HELLO_CO     82  0x20   <FILE>

lsh> cat /sd0/projects/hello_copy.c
#include <lugal.h>

main() {
    printf("Hello from LugalOS FAT32 Storage!\n");
}
```

### 2. Editing Files with Extended Unix `ed` Editor
```bash
lsh> ed /sd0/hello.c
'/sd0/hello.c': 82 bytes (5 lines)

:1,$n
1: #include <lugal.h>
2: 
3: main() {
4:     printf("Hello from LugalOS FAT32 Storage!\n");
5: }

:3c
    printf("Hello from Extended ed Editor!\n");
.

:1,$n
1: #include <lugal.h>
2: 
3:     printf("Hello from Extended ed Editor!\n");
4:     printf("Hello from LugalOS FAT32 Storage!\n");
5: }

:w
'/sd0/hello.c': 121 bytes written
:q
```

### 4. Interactive Emacs Multi-Line Lisp Editor (`e [filename]`)
```text
lsh> e /sd0/math.lisp
─────────────────────────────────────────────────────────────────────────────
  1 │ (define (factorial n)
  2 │   (if (= n 0)
  3 │       1
  4 │       (* n (factorial (- n 1)))))
  5 │ (factorial 6)
─── /sd0/math.lisp ───────────────── C-X C-E: eval | C-X C-S: save | C-X C-C: exit ───
=> 720
```

---

## LugalOS Lisp Machine Engine & Standard Library

The LugalOS kernel hosts an embedded **Lisp Machine Engine** that serves as the microkernel's primary execution engine, REPL, and interactive shell environment (`lsh`).

### Supported Lisp / Scheme Special Forms
* `(define var val)` / `(define (fn args...) body...)`: Binds global symbols and procedure signatures.
* `(lambda (args...) body...)`: Constructs anonymous procedure closures.
* `(quote expr)` / `'expr`: Prevents evaluation of literal S-expressions and lists.
* `(if condition then-expr else-expr)`: Evaluates conditional branches.
* `(begin expr1 expr2 ...)`: Evaluates sequential expressions, returning the value of the final S-expression.
* `(let ((var val) ...) body...)`: Establishes local lexical bindings; each binding's initializer sees
  the outer scope, not the other bindings.
* `(let* ((var val) ...) body...)`: Like `let`, but each binding's initializer also sees every binding
  before it.
* `(while condition body...)`: Repeats `body` while `condition` is true; a plain loop, not recursion.
* `(cond (clause1) (clause2) ... (else default))`: Multi-branch conditional selection.

### Built-in Primitives & Standard Library

#### Arithmetic & Logic
`+`, `-`, `*`, `/`, `=`, `<`, `>`, `<=`, `>=`, `/=` (`<`/`>`/`<=`/`>=`/`=` chain across any number of
arguments, e.g. `(< 1 2 3)`; `/=` means every argument differs from every other), `quotient`,
`remainder`, `modulo`, `abs`, `min`, `max`.

#### Predicates
`null?`, `pair?`, `symbol?`, `string?`, `integer?`, `procedure?`, `zero?`, `boolean?`

#### List Processing
`cons`, `car`, `cdr`, `list`, `length`, `append`, `reverse`, `list-ref` (alias `nth`), `map`,
`filter`, `for-each`

#### String Processing
`string-append`, `string-length`, `substring`, `string->number`, `number->string`, `string=?`

#### Procedure Invocation
* `(apply fn arg-list)` / `(apply fn a1 a2 ... arg-list)`: Calls `fn` with the given arguments.
* `(eval expr)`: Evaluates an already-constructed S-expression against the global environment.

#### File I/O & Script Execution
* `(read-file path)`: Reads content from Plan 9 VFS into a string.
* `(write-file path content)`: Overwrites file content on Plan 9 VFS.
* `(load path)`: Evaluates a `.lisp` source file from disk (e.g. `(load "/sd0/system/stdlib.lisp")`).

#### Microkernel VFS & System Metrics
* `(ls path)`: Performs directory listing across `/flash0/`, `/sd0/`, `/ram0/`, `/proc/`, `/dev/`, `/srv/`.
* `(mkdir path)`: Creates directory in FAT32 storage engine.
* `(rm path)`: Removes file from VFS.
* `(cp src dst)`: Copies file content between VFS locations.
* `(cat path)`: Reads and prints file content to UART console.
* `(ps)`: Displays the live task table (`/proc/ps`) — real scheduler state, including the `p9srv` server task.
* `(meminfo)`: Displays live page-allocator counters — pages total, free and used (`/proc/meminfo`).
* `(df)`: Displays mounted volume capacity and cluster usage (`/proc/df`).
* `(top)`: Displays system process, memory, and storage monitor dashboard.

#### Hardware I2C, RTC & EEPROM Storage
* `(time)`: Returns monotonic milliseconds elapsed since system boot.
* `(date)`: Returns ISO 8601 formatted date and time string.
* `(set-date "YYYY-MM-DD HH:MM:SS")`: Updates LugalOS clock and persists to DS1307/DS3231 RTC hardware.
* `(i2c-scan)`: Scans I2C bus (`I2C0` on `GP4` SDA / `GP5` SCL) and outputs responsive slave matrix.
* `(eeprom-read [offset] [len])`: Reads non-volatile string from AT24C32 4KB I2C EEPROM (`0x57` / `/dev/eeprom`).
* `(eeprom-write offset string)`: Writes persistent string to AT24C32 4KB I2C EEPROM.
* `(p9-loopback payload)`: Evaluates 9P2000 RPC round-trip over in-memory transport gateway (`/srv/p9_loopback`).

#### Registries, Streams and Servers
These exist so that *policy* — which hardware is used, which link serves 9P, who owns the terminal —
lives in `init.lisp` rather than being compiled into the kernel.
* `(devices)`: Prints the probed device registry (same content as `/proc/devices`).
* `(dev-present? "name")`: Whether this board actually has a device — lets a boot script branch on
  the hardware instead of on which target it was built for.
* `(klog-sinks)`, `(klog-detach "console")`, `(klog-attach "console")`: Inspect and rebind kernel-log
  output. Detaching stops diagnostics reaching the terminal **without silencing the shell**; the log
  keeps accumulating in the ring either way, readable via `/proc/kmsg` (locally or over 9P).
* `(console-device)`, `(console-bind "uart"|"usb")`: Which device owns the interactive console, and
  hand it to another one at runtime.
* `(mount-local "name")`: Attach this node's *own* namespace at `/name/` through the local 9P
  channel — `/name/sd0/x` reaches the same bytes as `/sd0/x`, having crossed serialized frames and
  the copy-always channel. Mostly a demonstration that a local server and a remote one are the same
  code path.
* `(mount-remote "name" ["device"])`, `(unmount "name")`: Attach a peer's namespace over a named 9P
  link (omit the device to use this board's default).
* `(p9-serve "device")`, `(p9-unserve "device")`: Serve inbound 9P on a named link.
* `(spawn-pump n)`: Spawn a task that services background 9P links and yields. Exists to make the
  client/server concurrency hazard genuinely reachable in tests.

#### Native C11 Compiler & Binary Execution
* `(cc src dst)`: Invokes native `chibicc` C11 compiler on VFS C source files.
* `(exec path)`: Loads and executes native RISC-V ELF binaries in supervisor space.

### System Boot Initialization Sequence
On boot, LugalOS initializes the Lisp Machine engine and executes the boot lifecycle:
1. Loads `/sd0/system/stdlib.lisp` (standard library extensions written in pure Lisp).
2. Executes `/sd0/system/init.lisp` to initialize system settings and launch startup tasks.


---

## License & Acknowledgments

LugalOS is licensed under the [MIT License](LICENSE).

### Third-Party Tools & Origins

* **Microsoft UF2 Tools**: [`tools/uf2conv.py`](tools/uf2conv.py) and [`tools/uf2families.json`](tools/uf2families.json) are sourced from [Microsoft UF2 (USB Flashing Format)](https://github.com/microsoft/uf2) (MIT License), providing standard UF2 block conversion and family ID registry lookups.
* **Raspberry Pi Picotool**: [`tools/picotool`](tools/picotool) is sourced from the [Raspberry Pi Picotool Repository](https://github.com/raspberrypi/picotool) (BSD 3-Clause License), used for RP2350 image analysis, partition table parsing, and binary validation.
* **Igor Michalak's bare-metal-rp2350**: Reference bootloader headers, RISC-V XOSC/PLL clock tree setup, and dual-core reset patterns from [`bare-metal-rp2350`](https://github.com/igormichalak/bare-metal-rp2350).
* **hathach's TinyUSB**: The native RP2350 USB CDC ACM driver (`drivers/usb_cdc.c`) was implemented and debugged against DPRAM/endpoint-control register layouts and buffer-control write ordering cross-checked from [`rp2040_usb.c`/`usb_dpram.h`](https://github.com/hathach/tinyusb) (MIT License) — no TinyUSB code or runtime is linked into LugalOS; the USB device stack is written from scratch directly against the hardware.
* **Rui Ueyama's chibicc**: C11 compiler architecture adapted from [`chibicc`](https://github.com/rui314/chibicc) (MIT License).
* **Ken Thompson & Bell Labs**: Unix `ed` teletype editor and the Plan 9 Operating System universal namespace model.
