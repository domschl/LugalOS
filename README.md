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

**Working today**, verified by the automated test suite (`tests/runner.py`, 300 tests on QEMU RV32
NOMMU and RV64 MMU) and by hardware-in-the-loop suites (`tests/hw/`, against real RP2350 silicon:
24 core tests, 15 more for the wired gateway, 6 over the radio, 3 against a
GPS-disciplined reference clock):
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
  `(define (fn args...) body...)` signature form both work), `if`, `begin`, `let`/`let*`/named let,
  `while`, `cond`, `quote`, tail-call optimization, a mark-sweep collector, a standard library of
  list/string/predicate/comparison/integer-math primitives, and dozens of system/hardware
  primitives — run `(help)` for the current list.
- The native C11 compiler (`chibicc`), producing real RISC-V ELF binaries, and the Thompson
  `ed`-style line editor.
- The native RP2350 USB CDC ACM console (`/dev/ttyACM0`), written from scratch against the
  hardware, now running as a U-mode task like every other RP2350 driver.
- **An onboard chess engine** (`user/chess/`) with a console REPL (`chess-console`), alpha-beta
  search, checkmate/stalemate detection, and full game state (undo/redo/FEN save-load) — reachable
  interactively over the same `lsh` shell, on both QEMU and real RP2350 hardware. On the chess-computer
  persona the keypad/TFT board UI and the terminal are **one session with two live input devices and
  fully mirrored output**: every console command (`level 4`, `fen …`, `save`) works while a game is
  played on the board, a move entered on either device redraws the ASCII board, the TFT and the
  7-segment slots alike, and typing during an engine search is queued rather than discarded.
  Games are stored as **real PGN** (proper SAN, so any chess GUI opens them) under `/sd0/chess/`,
  auto-saved after every move and auto-restored when a session starts; `new` retires the previous
  game to `chess/games/`, and `save <name>` / `load <name>` / `games` archive by name.
- **RAM budgeting as a first-class concern, with the tooling to keep it that way**: on RP2350 `.bss`
  and the heap are literally the same budget — the page allocator starts where the image ends — so a
  static buffer serving an idle subsystem is heap no *other* subsystem can have. Reclaiming that took
  the managed heap from 53 pages (212 KB) to **89 pages (356 KB)** and the chess persona's peak from
  100% of the heap to 28 of 89 pages. Rare-but-large working memory (the compiler's pools, the chess
  engine's move-list pools and position scratch, Lisp's file buffers, the U-mode probe stacks) is
  taken from the heap on demand and given straight back via `kernel/scratch.h`; constant tables that
  were computed into RAM at boot now live in flash. Guarded going forward by a **link-time heap
  floor** (`linker/rp2350.ld` fails the build if the heap drops below 256 KB), a `sizecheck` build
  target that fails on any static-RAM growth against a recorded per-file baseline, and `/proc/meminfo`
  and `/proc/ps` reporting the static breakdown and per-task stack high-water marks.
- **A second RP2350 board persona**: `rp2350-clock` targets the Waveshare Pico-Clock-Green baseboard
  — a led-matrix clock display with LDR-driven auto-brightness (seven geometric levels the ambient
  reading walks up and down, with a deadband so a room sitting on a threshold does not flicker), wired
  through the same driver-task architecture as the default `rp2350-chess` persona, demonstrating that
  "which hardware this board has" is a per-persona table (`cmake/board-rp2350-clock.cmake`), not a
  fork of the kernel.
- **An IP stack of our own, over two different wires**: ARP, IPv4, ICMP, UDP and a server-side TCP,
  written here rather than bought in silicon — about 2,100 lines under `net/`, sized for an RP2350
  and developed against a packet-level QEMU peer before either piece of hardware existed. Two frame
  sources feed it through one `netif_t` seam: a wired **ENC28J60** (SPI, MAC-only, no closed
  firmware anywhere) and the **CYW43439** radio on a Pico 2 W. Above it, the 9P server answers over
  TCP with the same authentication a serial link uses, `host/fuse-p9` mounts a board's whole
  namespace onto a Linux host over either wire, and an **SNTP client** lets a board set its own
  clock from the segment. What the stack deliberately does *not* do is listed in
  [Networking](#networking-an-ip-stack-of-our-own) — an unstated limit gets credited as a feature.
- **An identity that belongs to the silicon**: on RP2350 the device UID comes from OTP `CHIPID` and
  the rest of the record lives in its own flash sector, so a node's identity, its device key, its
  peer grants, its address and its WLAN credential all survive a firmware reflash. `/flash0` is now
  its own independently flashable image for the same reason.
- **A radio-set clock that runs unattended**: a DCF-77 longwave receiver decodes the time signal from
  Mainflingen, a three-button menu drives the whole UI on the panel itself, and the kernel clock keeps
  **UTC** with local time computed from a POSIX `TZ` rule. Both RP2350 personas boot from a bare USB
  power adapter with no host attached — see [Appliance mode](#appliance-mode).

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
  * **Scheme / Lisp Core**: Support for `define`, `lambda`, `quote` (`'`), `if`, `begin`, `let`, `let*`, named let, `while`, `cond`, a standard library of list/string/predicate/comparison/integer-math primitives, memory `peek`/`poke`, and string data types. Tail calls are optimized (constant stack/call-depth for self- and mutually-recursive loops in tail position, including named-let loops), and a mark-sweep collector reclaims unreachable values between top-level commands.
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
* **Automated Integration Test Harness**: Non-interactive QEMU PTY integration runner (`tests/runner.py`) executing 300 automated test cases across RV32 (NOMMU) and RV64 (Sv39 MMU) builds (see `tests/runner.py` for the current count, as this grows over time), plus hardware-in-the-loop suites (`tests/hw/`: 24 core tests, 15 for the wired gateway, 6 over the radio, 3 against a GPS-disciplined reference clock) that drive real RP2350 silicon over USB — including flashing the board itself via the "1200-baud touch" and re-verifying against `/proc/buildid`.
* **Host-Side 9P File Utility (`host/p9lib`)**: a real, general-purpose Python 9P2000 client and CLI (`lugal9p`) for a host machine (macOS/Linux) to read, write, `mkdir`, and remove files on any LugalOS board's filesystems — over the same USB-CDC/UART links `link_usb_cdc` and `tests/hw/` already use, or a QEMU virtio-console socket for hardware-free use. `uv run lugal9p --serial /dev/ttyACM1 ls /sd0` (see [`host/p9lib/README.md`](host/p9lib/README.md)).
* **FUSE Filesystem (`host/fuse-p9`)**: mounts a board's entire 9P namespace as a real host directory, built on `host/p9lib` — `cat`, `cp`, editors, and other ordinary tools work against it unmodified. Over USB with `uv run lugal9pfuse --serial /dev/ttyACM1 /mnt/lugalos`, or over the network with `--tcp <addr>` against either a WiFi or a wired-Ethernet board (see [Mount the whole namespace on the host](#5-mount-the-whole-namespace-on-the-host-fuse-p9-over-tcp)). Verified on Linux, over TCP against a Pico 2 W on WiFi; macOS (via macFUSE) is written and reaches the same code path, but is untested end-to-end — see [`host/fuse-p9/README.md`](host/fuse-p9/README.md) for why (Apple Silicon's default security policy blocks third-party system extensions short of a Recovery Mode trip, which is the user's call, not ours to push).

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
├── host/                    # Host-side tooling: p9lib (9P2000 client + `lugal9p` CLI), fuse-p9 (Linux FUSE mount)
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

### Static RAM budgeting (`sizereport` / `sizecheck`)

On RP2350 the page allocator's heap begins where the image ends, so every byte of `.data`/`.bss` is a
byte the heap does not get. Two targets make that visible instead of something to rediscover:

```bash
cmake --build build/rp2350 --target sizereport   # per-source-file static RAM, and the heap it leaves
cmake --build build/rp2350 --target sizecheck    # fails if static RAM grew vs the recorded baseline
```

`sizecheck` compares against the baseline for **the persona being built** — `tools/sizereport-<board>.json`,
picked from the active preset's board file, so `build/rp2350` is checked against
`tools/sizereport-rp2350.json` and `build/rp2350-clock` against `tools/sizereport-rp2350-clock.json`. Each
persona builds a different set of drivers and therefore has its own budget; one shared baseline would have
to be the loosest of them, which is the same as not having one. Either exits non-zero on **any** growth.
When growth is intended, re-baseline deliberately so the new numbers land in a reviewable diff:

```bash
python3 tools/sizereport.py build/rp2350/lugalos.elf --update tools/sizereport-rp2350.json
```

The hard limit is enforced independently at link time: `linker/rp2350.ld` asserts the heap never falls
below 256 KB, so a regression is a build error naming the cause rather than a board that fails to start
something weeks later.

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

### ENC28J60 Ethernet Wiring (`rp2350-gateway` persona, Pico 2 SPI0)

The wired half of the network. The part in hand is a HanRun V823 HR911105A module — the common
ten-pin SPI breakout with magnetics-integrated RJ45, whose header is two rows of five in the
order printed on the board (`CLOUT WOL SI CS VCC` / `INT SO SCK RESET GND`).

```
ENC28J60 Module         Raspberry Pi Pico 2 (RP2350)
──────────────          ─────────────────────────────
      VCC ──────────►  Pin 36  3V3(OUT)      ** 3.3 V, NOT 5 V **
      GND ──────────►  Pin 38  GND
       SI ──────────►  Pin 25  GPIO19 (SPI0 MOSI, into the chip)
       SO ◄──────────  Pin 21  GPIO16 (SPI0 MISO, out of the chip)
      SCK ──────────►  Pin 24  GPIO18 (SPI0 SCK)
       CS ──────────►  Pin 22  GPIO17 (SPI0 CSn)
    RESET ──────────►  Pin 26  GPIO20 (active low)
      INT ◄──────────  Pin 27  GPIO21 (active low, open-drain)
     WOL     (n/c)
    CLOUT    (n/c)
```

> **3.3 V, and confirm it with a meter before first power-on.** This module has no onboard
> regulator and the ENC28J60 die is 3.3 V only; 5 V on VCC kills it. `WOL` and `CLOUT` are
> deliberately unconnected — nothing here wants wake-on-LAN or a clock from this chip, and
> leaving them floating is correct.
>
> `INT` needs the RP2350's internal pull-up, which the driver enables: the module has none.
> Decoupling is not optional at these edge rates — **220 µF bulk plus 100 nF ceramic across the
> module's own VCC/GND pins**, short SPI leads, and a ground return run alongside them. That
> figure was arrived at by measurement, not by habit; 100 µF was measurably worse.
>
> **One known, worked-around fault.** Left idle, these modules clear `MACON1.MARXEN` and/or
> `ECON1.RXEN` on their own within seconds. `enc_poll_locked()` notices and redoes the MAC/PHY
> init, rate-limited, escalating to a full reset — 0% loss under sustained traffic. A dedicated
> regulator, three capacitor values and SPI clock in both directions were all ruled out as
> causes; the same workaround was reached independently by `ntruchsess/arduino_uip#167` on
> genuine Microchip silicon. See `plan/open_issues.md`.

### Build for RP2350

```bash
cmake --preset rp2350-chess
cmake --build --preset rp2350-chess
# Generates: build/rp2350/lugalos.uf2 (the OS) and build/rp2350/flashfs.uf2
#            (the /flash0 filesystem -- its own flash region since I7a)
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

2. Copy the UF2 firmware. **On RP2350 there are two images, and a fresh board needs
   both** — see [Two images, flashed independently](#two-images-flashed-independently)
   just below for why:
   ```bash
   # Linux
   cp build/rp2350/flashfs.uf2  /media/$USER/RP2350/   # once; the /flash0 filesystem
   cp build/rp2350/lugalos.uf2  /media/$USER/RP2350/   # the OS itself
   # macOS
   cp build/rp2350/flashfs.uf2  /Volumes/RP2350/
   cp build/rp2350/lugalos.uf2  /Volumes/RP2350/
   ```
   Each copy reboots the board out of BOOTSEL, so hold the button again between the two
   (or use `flash.py` below, which handles that for you).

#### Two images, flashed independently

`/flash0` used to be a 512 KB FAT32 image compiled into the binary — over half of a
~982 KB firmware, read-only, and rewritten on every OS flash despite changing almost
never. It now lives in its own flash region and is flashed as its own UF2
(I7a, [`plan/phase21_identity_and_authentication.md`](plan/phase21_identity_and_authentication.md) §3.3):

| region | image | size | changes |
|---|---|---|---|
| `0x10000000` | `lugalos.uf2` | ~470 KB | every build |
| `0x10180000` | `flashfs.uf2` | 512 KB | only when `tools/sd_root` does |
| `0x103FF000` | *(nothing this build emits)* | 4 KB | reserved for the identity record |

So the OS image is half the size it was, updating the filesystem does not touch the OS,
and updating the OS does not touch the filesystem — measured on hardware, both ways.
The third region is deliberately never covered by any UF2 this build produces: a device
mints its own identity, and the machine doing the flashing must not need to hold it.

If you flash only `lugalos.uf2` onto a board that has never had `flashfs.uf2`, `/flash0`
is erased flash and the board says so at boot rather than reporting a corrupt filesystem.
`/sd0` and everything else still work.

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


## Networking: an IP stack of our own

Everything above the wire is written here. ARP, IPv4, ICMP, UDP and a server-side TCP live in
`net/`, and two very different parts feed them Ethernet frames through one seam (`net/netif.h`):
a wired **ENC28J60** and the **CYW43439** radio on a Pico 2 W. The stack was built and tested
against a packet-level peer under QEMU (`tests/netpeer.py`) before either part was in hand, which
is why the same code came up on both wires without a per-part IP path.
`plan/phase19_ip_stack_and_ethernet.md` is the design and the full account.

### Why write one at all, when a chip will sell you TCP

The phase this came from started by cancelling a W5500 — a part that puts a closed TCP/IP
implementation on its own die and hands the host a byte stream. The argument for keeping it was
that the CYW43439 is *also* a blob, so the difference is only blob size. That is half right, and
the half that is wrong decides the roadmap:

| | W5500 (cancelled) | CYW43439 (Pico 2 W) | ENC28J60 |
|---|---|---|---|
| Where the closed code runs | its own die | its own die | nowhere |
| Who ships the firmware | WIZnet, once, in the package | **we do** — 227 KB in our flash, uploaded at every boot | — |
| What it hands us | a **TCP byte stream** | **Ethernet frames** | **Ethernet frames** |
| What is left for us to implement | nothing | the entire IP stack | the entire IP stack |

**The distinction that matters is not blob size, it is what is left to implement.** A part whose
closed firmware ends at the MAC layer leaves the network to us; a part whose closed firmware ends
at TCP does not. "Bare metal" here has always meant *we wrote the thing*, not *no silicon anywhere
contains microcode* — every chip in this tree has a mask ROM, the RP2350 included.

A corollary worth stating because it surprises people: **the CYW43439 is strictly more work than
the W5500 was, not less.** It is a *fullmac* part — the blob does 802.11 association and hands
back Ethernet frames — so choosing it means a blob **and** a software IP stack. Anyone reaching
for a Pico 2 W expecting the W5500's "TCP arrives over SPI" bargain will not get it.

So the sentence this project can honestly say is: **LugalOS implements its own network stack, and
speaks it over a wire whose driver it also wrote.** On the ENC28J60 that is true down to the
magnetics. On the Pico 2 W it is true down to the 802.11 MAC, with the radio behind a blob that is
named, sized and licensed under **Blob accounting** in the Wireless section below.

### What the stack does

- **Ethernet II framing**, one MAC per interface, broadcast and own-address filtering — in the MAC
  where the part can do it, in software where it cannot.
- **ARP** — request, reply, an 8-entry cache with timeouts, gratuitous ARP on address change. No
  proxy ARP.
- **IPv4** — unicast send and receive, header checksum, TTL.
- **ICMP** — echo request/reply, and destination-unreachable *emitted* for a closed port, which is
  what turns a silent timeout at the far end into an immediate "connection refused".
- **UDP** — send and receive on a bound port, four bindings.
- **TCP, both directions of open** — one listener, up to **two** simultaneous connections, a fixed
  receive window of one MSS, retransmission with exponential backoff, correct FIN and RST handling.
  An outbound `connect()` exists too, which is what makes `(net-mount)` a distributed namespace
  over a network rather than over a cable.
- **9P over TCP**, authenticated, on port 564 — the same server, the same auth gate and the same
  grants a serial link uses.
- **An SNTP client** (`ntp [server]`, `(ntp-sync)`) — one query, applied as a step.

### What it deliberately does not do

Each of these is a decision, not an omission. An unstated limit gets credited as a feature, so:

- **IPv6.** No.
- **DHCP.** A server wants a stable address, and every DHCP failure mode presents as "the board is
  not on the network". Addresses are configured (`netcfg`) and kept in the identity record. This is
  no longer a blanket no — a *sensor* persona inverts every term of that argument — but it gets its
  own milestone when a board that needs it exists, not a corner of one that doesn't.
- **TCP options beyond MSS**: no window scaling, no SACK, no Nagle, no delayed ACK.
- **Out-of-order segment reassembly.** A segment that arrives out of order is dropped and not
  acknowledged; the peer retransmits. This costs throughput on a lossy link and saves a reassembly
  queue, which is both the largest RAM line item and the largest bug surface in a small TCP. **This
  is the important one** — it is written down here so a future throughput complaint is recognised
  as this decision rather than as a mystery.
- **IP fragmentation and reassembly.** An incoming fragment is counted and dropped; we never emit
  one (MSS is clamped so we do not have to).
- **Routing or forwarding between interfaces.** A board with two wires is a board with two wires.
- **TLS, or any confidentiality on the wire.** Authentication proves *who* attached; it hides
  nothing they then read. See [What it does not defend](#what-it-does-not-defend-and-will-not).
- **A writable `/net` socket filesystem.** Plan 9's shape, and only half of it lands here:
  `/proc/net` is a status file, not a control interface.
- **More than two concurrent TCP connections**, and a TIME_WAIT of 2 s rather than the RFC's
  2×MSL. Both are deliberate, both are commented where they are defined, and a client that
  reconnects faster than that will be refused. See `plan/open_issues.md`.

### Seeing what it did

`net` gives the summary and `cat /proc/net` gives every counter, and the counters are kept
**per decision** rather than as one drop total — right wire wrong address, truncated below its own
header, bad checksum, a fragment, a protocol we do not implement, no route, no listener on that
port. That separation is not tidiness: "the network does not work" is not a diagnosis, and a single
`dropped` total cannot become one. Phase 18 spent days learning that.

```
lsh> net
rv64-mmu-d89b: virtio-net, mac 52:54:00:12:34:56, link up
  addr 192.168.77.2/255.255.255.0 gw 192.168.77.1
  rx 5 frames, 300 bytes, 0 dropped
  tx 6 frames, 370 bytes, 0 errors
  arp 1/2, ip 4/4, icmp 3/4, udp 1/0 (rx/tx)
  9P listening on tcp/564 -- 0 connections, 0 accepted, 0 reset
  arp cache 1 entries; see /proc/net for every drop counter

lsh> cat /proc/net
interface: virtio-net
mac: 52:54:00:12:34:56
link: up
address: 192.168.77.2
netmask: 255.255.255.0
gateway: 192.168.77.1
rx: 5 frames, 300 bytes
tx: 6 frames, 370 bytes
arp: 1 rx, 2 tx
ip: 4 rx, 4 tx
icmp: 3 rx, 4 tx
udp: 1 rx, 0 tx
tcp: 0 rx
drop: 0 not-for-us, 0 short, 0 checksum, 0 fragment,
      0 proto, 0 no-route, 1 no-port
tcp: listening on 564, 0 open, 0 accepted, 0 reset
udp bindings: 0
arp cache: 1 entries
  192.168.77.1 02:00:00:00:00:42
```

(That capture is a real session: three pings and one datagram to a port nobody bound, which is
the `no-port 1` and the ICMP the board sent back in reply.)

### Setting the clock from the network

```
lsh> ntp
ntp: asking the gateway, 192.168.77.1
ntp: 192.168.77.1 stratum 2 (via 178.63.9.110)
  offset     : +28 d 01:03:40.187   (what was added to our clock)
  round trip : 6.389 ms
  clock set  : 2026-09-02 13:03:40.449 UTC

lsh> ntp
ntp: asking the gateway, 192.168.77.1
ntp: 192.168.77.1 stratum 2 (via 178.63.9.110)
  offset     : +6.796 ms   (what was added to our clock)
  round trip : 19.229 ms
  clock set  : 2026-09-02 13:03:40.493 UTC
```

The first offset is 28 days because a board that has never been told the time starts at the
instant compiled into `kernel/time.c`, and the client steps rather than slews — there is nothing
to protect. The scale follows the magnitude: microseconds are worth reading against a running
clock and useless against a quarter of a century, which is also why the offset is not printed as a
plain `%ld` — `long` is 32 bits on RV32 and RP2350, and a first sync overflows it. The second line
is what a sync against an already-set clock looks like. `stratum 2 (via …)`
names the server's *own* upstream, so a chain is visible from here; a stratum-1 server shows its
reference clock's four-character id instead, `(GPS)` or `(DCF)`.

With no argument it asks the gateway — the one address the board already knows, and a home router
runs an NTP server more often than not. `ntp <ip>` asks a specific one. `(ntp-sync)` is the same
thing from Lisp, returning the offset in milliseconds so a boot script can tell "already right"
from "two minutes out"; it belongs on the line after the network line in
`/sd0/system/etc/usr_init.lisp`.

Measured on hardware against a GPS-disciplined stratum-1 on the same LAN, over WiFi, consecutive
syncs report offsets of **a millisecond or less with a 6–9 ms round trip**. The kernel clock keeps
microseconds (`plan/phase24_dcf77_precision_and_ntp_server.md`, P2), so that is a measurement
rather than a resolution limit.

This is a client, and only a client: one query, applied as a step, no poll loop and no frequency
discipline. The thing worth knowing before relying on it is not the query's accuracy but what
happens between queries — the board's oscillator is a ±30 ppm crystal, so it drifts up to ~2.6 s/day.
A single sync at boot is not the same as a disciplined clock, and closing that gap is
`plan/phase24_dcf77_precision_and_ntp_server.md`. `tests/hw/test_ntp.py` is the repeatable check.

## Wireless: joining a WiFi network (RP2350W / Pico 2 W)

The `rp2350-wifi` persona drives the Pico 2 W's on-board CYW43439 over a bit-banged gSPI bus
(PIO0), uploads the chip's firmware at every boot, and presents the result as an ordinary
`netif_t` — so the IP stack, TCP and the 9P server above it are the same code the wired
ENC28J60 gateway uses. See `plan/phase19_ip_stack_and_ethernet.md` R5 for how it was built.

```bash
$ cmake --preset rp2350-wifi && cmake --build build/rp2350-wifi   # or rp2350-clock, which also carries the radio
$ cd tests/hw
$ uv run flash.py --uf2 ../../build/rp2350-wifi/flashfs.uf2   # once, or when tools/sd_root changes
$ uv run flash.py --uf2 ../../build/rp2350-wifi/lugalos.uf2
```

Two images because `/flash0` has its own flash region now — see
[Two images, flashed independently](#two-images-flashed-independently).

**Blob accounting.** This persona is the one place a LugalOS image contains something that is
not source: the CYW43439's firmware, its CLM regulatory table and its NVRAM — 227 KB in total,
uploaded to the chip at every boot because it has no flash of its own. They are redistributed
here under the Infineon Permissive Binary License, which permits binary redistribution with the
notice attached. Provenance, sizes and SHA-256s are in
[`firmware/cyw43/README.md`](firmware/cyw43/README.md). A project that makes a point of being
bare metal owes its readers that paragraph in the same place it makes the claim: **everything
else in this repository is source that is compiled here; those three files are not.**

### 1. Derive the PSK on the host

WPA2's PSK is PBKDF2-HMAC-SHA1 over the passphrase, salted with the SSID, 4096 iterations. The
board never sees a passphrase — only the 256-bit result — so the derivation happens here:

```bash
$ python3 -c 'import hashlib, getpass; s = input("SSID: "); \
print(hashlib.pbkdf2_hmac("sha1", getpass.getpass("passphrase: ").encode(), s.encode(), 4096, 32).hex())'
SSID: homenet
passphrase:
6e91faf94be6a5a4d58ae22f45b42f5f0fd5e97f1e46513ac0b9a039b4af480d
```

`getpass` keeps the passphrase off your terminal and out of your shell history. The same
derivation lives in `tools/provision.py` (`derive_wpa2_psk()`) if you would rather mint a whole
identity image — see [Provisioning walkthrough](#provisioning-walkthrough), step 5 — and
`python3 tools/provision.py --selftest` checks it against the published IEEE test vector.

### 2. Bring the radio up and join

```bash
lsh> wifi probe
cyw43: CYW43439 (chip id 0xa9af)
cyw43: uploading firmware (231077 bytes)...
cyw43: HT clock up after 14 ms
cyw43: F2 ready after 65 ms
cyw43: loading CLM (984 bytes)...
cyw43: wlan0 registered, mac 2c:cf:67:de:12:5e
wifi: ready

lsh> wifi join homenet 6e91faf94be6a5a4d58ae22f45b42f5f0fd5e97f1e46513ac0b9a039b4af480d
cyw43: joining "homenet"...
cyw43: joined, bssid 48:5d:35:9f:a9:46
wifi: joined
```

`wifi join` takes the **derived PSK as hex, never a passphrase** — there is no code path here
that accepts one. With no arguments it reads the SSID and PSK from the identity record instead,
which is the intended form; that needs I7 (an RP2350 backend for the identity store), so on real
hardware today the credentials are typed. See `plan/open_issues.md`.

Other commands: `wifi led [on|off]` blinks the user LED — which hangs off the *wireless chip's*
GPIO 0, not an RP2350 pin, so it only lights once the firmware is genuinely running and
answering ioctls. `wifi stats` reports the receive ring's high-water mark and any drops.

### 3. Give it an address

There is no DHCP client yet, so the address is static. From the Lisp REPL:

```bash
lsh> lisp
lisp> (net-config "192.168.178.21" "255.255.255.0" "192.168.178.1")
[Net] 192.168.178.21/255.255.255.0 gw 192.168.178.1
lisp> exit

lsh> net
rp2350-wifi-2662: wlan0, mac 2c:cf:67:de:12:5e, link up
  addr 192.168.178.21/255.255.255.0 gw 192.168.178.1
  rx 1679 frames, 134046 bytes, 0 dropped
  tx 646 frames, 50838 bytes, 0 errors
```

`net-config` also sends a gratuitous ARP, so the segment learns the board immediately rather
than after somebody's stale cache expires.

### 4. Serve 9P over the air

The 9P server requires authentication on a network link — an unauthenticated `attach` is
refused, which is the whole point of doing this over a radio. Install a key for this boot and
start listening:

```bash
lsh> p9key 000102030405060708090a0b0c0d0e0f
p9key: console key set (16 bytes), this boot only
lsh> net listen 564
net: listening for 9P on tcp/564
```

`p9key` takes hex only and holds the key **for this boot only** — it is deliberately not
persisted, so a board that reboots forgets it, and it overrides the key files while it is set.
`p9key` with no arguments reports whether a key is configured and where the persistent ones are
read from; `p9key clear` removes it. A key that should survive a reboot belongs in the identity
record instead (see [Identity and Authentication](#identity-and-authentication)) — `p9key` is
the bootstrap path for a board that has no record yet.
The key's *fingerprint* is shown by `identity`, which never prints the key itself.

From the host, mount or read it like any other node:

```bash
$ cd tests/hw && uv run test_wifi.py 192.168.178.21
  [PASS] icmp: 20 echoes -- round-trip min/avg/max = 4.960/7.117/18.148 ms
  [PASS] auth: unauthenticated attach refused
  [PASS] 9p: authenticated session, /proc/version -- LugalOS v0.13.1
  [PASS] 9p: directory read (multi-entry Rread) -- / has 5 entries, /proc 12
  [PASS] 9p: 10 consecutive sessions -- 10/10
```

`tests/hw/test_wifi.py` talks to the board only over the network — no serial interaction — so a
pass means the radio path worked, not that a console cable did. Add `--soak-minutes 15` for the
sustained run.

### 4b. Let the board bring itself up next time (`netcfg`)

Everything above is typed once. Store the address in the **identity record**
and the board applies it by itself from then on:

```bash
lsh> wlan DOSC <psk-hex>                                   # once, ever
lsh> netcfg 192.168.178.21 255.255.255.0 192.168.178.1     # once, ever
netcfg: stored -- applied at every boot, before the stack task starts
```

After that, `wifi probe` and `wifi join` — with **no arguments** — are the
whole bring-up: the SSID, the PSK and the address all come out of flash.
`netcfg` on its own shows what is stored, `netcfg clear` removes it, and
`cat /proc/node` reports it over 9P so you can ask a board what address it
will have *before* rebooting it.

**Why the record and not `init.lisp`.** Since I7a the `/flash0` image is
byte-identical on every board, and that is worth keeping: an address in a boot
script would make the filesystem per-board again and force a per-device
`flashfs.uf2`. The record is already per-board, already survives reflashing,
and already holds the credentials the address is reached with — so the boot
script stays the same everywhere and only the record differs. `tools/provision.py
--ipv4 IP/MASK[/GW]` writes the same field from the host.

The address is applied **once carrier appears**, not at probe time. That is
also when the gratuitous ARP is worth sending, so the segment learns the board
immediately rather than after somebody's stale cache expires.

### 4c. …and on every power-up, by itself

Once the credentials are stored, the board brings its own radio up at boot —
no `wifi probe`, no `wifi join`, nothing typed at all:

```bash
$ ping 192.168.178.22        # ~30 s after power-on, unattended
64 bytes from 192.168.178.22: icmp_seq=1 ttl=64 time=4.45 ms
```

**Stored credentials are the intent to join**, the same rule the address
uses; there is no separate enable flag, and a board with no stored SSID does
nothing. It runs as a background task rather than a step in the boot
sequence, because uploading 231 KB of firmware takes tens of seconds and the
console must stay usable meanwhile — `ps` shows it as `wifiup`, exiting `0`
once associated.

The join **retries with a backoff and does not give up**, which is the
power-cut case: a board and its router come back at the same moment and the
router takes longer, so the one attempt made at boot is exactly the attempt
that fails. Retries settle at 30 s apart and stop logging after the first
few, so `/proc/kmsg` does not fill up for a network that simply is not there.

There is still no DHCP client (deliberately — see
`plan/phase19_ip_stack_and_ethernet.md` §7), so a reservation in the router
keyed on the radio's own MAC (`net` reports it) is the companion to this.

### 5. Mount the whole namespace on the host (`fuse-p9` over TCP)

Once the board is listening, `host/fuse-p9` turns its entire 9P namespace into an ordinary
directory on a Linux host — the same tool that mounts a board over USB, pointed at an address
instead of a serial port. **The transport below it makes no difference:** WiFi (`rp2350-wifi`)
and wired Ethernet (`rp2350-gateway`'s ENC28J60) present the same `netif_t` to the same IP
stack, TCP and 9P server, so the command is identical apart from the address.

```bash
$ cd host/fuse-p9 && uv sync          # once; Linux also needs libfuse (fuse2 or fuse3)
$ mkdir -p /tmp/lugalos
$ printf '000102030405060708090a0b0c0d0e0f' > ~/.lugal9p.key && chmod 600 ~/.lugal9p.key

# over WiFi (the address from step 3):
$ uv run lugal9pfuse --key-file ~/.lugal9p.key --tcp 192.168.178.21 /tmp/lugalos

# over Ethernet, the gateway persona — same command, different address:
$ uv run lugal9pfuse --key-file ~/.lugal9p.key --tcp 192.168.77.2 /tmp/lugalos
```

`--tcp` takes `HOST` or `HOST:PORT` (564 by default). Use `--key-file` rather than `--key`: a
FUSE mount lives for as long as it is mounted, and a secret on the command line is visible in
`ps` and in shell history for that whole time. Drop both flags for a link that has no key
installed. `lugal9pfuse` runs in the foreground; unmount with `fusermount -u /tmp/lugalos` from
another shell, or Ctrl-C it.

```bash
$ ls /tmp/lugalos                       # dev  flash0  proc  sd0  srv
$ cat /tmp/lugalos/proc/net             # the board's own view of the link it is answering on
$ cp ~/notes.txt /tmp/lugalos/sd0/      # ordinary tools, unmodified
$ find /tmp/lugalos -type f | wc -l
```

Reads, writes, `mkdir`/`rm`/`rmdir`, arbitrary offsets and the write-temp-then-rename save that
editors use all work; a *directory* rename is refused (`ENOSYS`), and permissions and timestamps
are accepted but not stored, because the server has no model for either. Files are buffered
whole in host RAM between open and close, which is fine for what these cards hold and is not a
design for anything larger. `host/fuse-p9/README.md` has the complete list, including what was
measured over the radio (~20 KB/s for a 32 KB file, four concurrent readers, 0 TCP resets) and
the 9P server bug this tool found the first time it was run over TCP.

macOS is a different story, and not because of anything in this code — see
[`host/fuse-p9/README.md`](host/fuse-p9/README.md).

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
* `(let name ((var init) ...) body...)`: Named let — builds a self-recursive loop callable as `name`
  from within `body`; tail calls to `name` run in constant stack space (see Tail-Call Optimization
  below).
* `(while condition body...)`: Repeats `body` while `condition` is true; a plain loop, not recursion.
* `(cond (clause1) (clause2) ... (else default))`: Multi-branch conditional selection.

### Tail-Call Optimization & Garbage Collection

A call in tail position — the branch an `if` selects, the last form of `begin`/`let`/`let*`/a matched
`cond` clause/a lambda body, or a named-let's own recursive call to itself — does not grow the C call
stack: the interpreter loops in place instead of recursing, so an ordinary counting loop written as
self-recursion (`(define (loop n) (if (done? n) n (loop (next n))))`) or as a named let runs in constant
stack space regardless of how many iterations it performs. Non-tail recursion (a recursive call that is
itself an argument to something else, e.g. `(* n (factorial (- n 1)))`) is unaffected and still bounded
by the evaluator's stack-depth guard.

Values that become unreachable are reclaimed by a mark-sweep collector between top-level commands (not
mid-expression), so a long interactive session does not exhaust available memory the way a single
unbounded computation still can — a command that itself allocates more than the pool can hold still
degrades that one command to `()`, but the shell recovers on the next command rather than staying
degraded for the rest of the session.

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
* `(date)`: Returns ISO 8601 local date and time. The kernel clock itself runs on UTC — local time is computed from it through the timezone rule, never stored.
* `(date-utc)`: The same instant as UTC, which is what the kernel and the DS3231 actually hold.
* `(tz)` / `(tz "CET-1CEST,M3.5.0,M10.5.0/3")`: Read or set the POSIX TZ rule (default Europe/Berlin). Returns `#f` and keeps the old rule if the string does not parse.
* `(set-date "YYYY-MM-DD HH:MM:SS")`: Sets the clock from a **local** time, storing UTC in LugalOS and in the DS1307/DS3231 RTC hardware.
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

## Appliance mode

Both RP2350 personas boot standalone from a USB power adapter, with no computer and no console. That
is a feature with teeth: on a board whose only outputs are an LED matrix and a buzzer, anything that
goes wrong before the display exists is invisible. Two pieces of the system exist because of that:

- **`CONFIG_CLOCK_BOOT_BEACON`** — one buzzer click per `CLOCK_BOOT_MARK(n)` and a *latching* count on
  the top LED row, so a hang leaves its last mark lit instead of merely stopping. Off by default, with
  no call sites at rest; set it and scatter marks over whatever is suspect.
- **`tests/hw/flash.py`** — flashes over the 1200-baud DTR touch with no BOOTSEL press, so a debug
  cycle does not mean opening the case.

The clock persona additionally needs no console for anything routine: **SIG** (signal monitor) is two
presses from the clock face, and **SYNC**, **LAST**, **AUTO**, **TSET**, **BRT**, **OFFS**, **24H** and
**BEEP** are all in the same menu.

---

## Identity and Authentication

Every LugalOS node has three things worth keeping apart, and `plan/phase21_identity_and_authentication.md`
is the design that keeps them apart: an **identity** (who a node claims to be — a UID that belongs to
the silicon and a name that belongs to its current role), an **authentication** mechanism (a pre-shared-key
challenge over 9P's own `Tauth` extension point — `plan/phase18_networking_and_auth.md` — that proves a
peer holds a specific key without ever putting the key on the wire), and, since phase 21, an
**authorization** step (a short list of *grants* saying which peer may attach where, and whether it may
write). None of this defends against a network observer reading traffic — see
[What it does not defend, and will not](#what-it-does-not-defend-and-will-not) below — it defends the
exported namespace against a peer that never proves anything at all, and bounds what a peer that *did*
prove something is allowed to reach.

**Current status.** Everything below is implemented, covered by `tests/runner.py` against the QEMU
identity disk (`drivers/virtio_blk_id.c`, milestone I2), and — since 2026-09-01 — **backed by real
storage on RP2350** (milestone I7b): the device UID is read from OTP `CHIPID`, and the record itself
lives in a reserved flash sector that a firmware reflash does not touch. Two boards were checked to
report two different UIDs, and a provisioned identity was verified to survive a UF2 reflash, which is
the claim the whole design rests on. `wifi join` with no arguments now reads its SSID and PSK from
that record, and `netcfg` stores the address there too, so a board brings its own network up after a
power cut with nothing typed.

One verify item is deliberately still open and is not hand-waved: an *interrupted* flash write
leaving the store readable as corrupt. Cutting power inside a ~100 ms erase window has not been
attempted. Note that I7b narrowed what that would probe — a torn write leaves a sector whose magic
does not match, which reads as UNPROVISIONED rather than as a plausible record, so the dangerous
outcome needs the magic to survive while the fields do not.

### The record, and why one field can't answer two questions

The identity **record** is a small (4 KB), typed, versioned block on its own storage — the second
virtio-blk device in QEMU, the RP2350's reserved flash sector on hardware — never on the removable SD
card, and never served over 9P under any path (the same server-side guard that refuses to serve the
9P auth key directory refuses this too). It holds, per field: a device UID (minted once, meant to survive
a reflash), an instance name (freely rewritable — a board's *role* can change even when its silicon
doesn't), this node's own device key (used only to *prove* this node's identity to a peer), and,
optionally, one WLAN SSID and its derived PSK.

The one rule that shapes all of it: **a value used to prove who a node is must never also be the value
used to decide who else may attach.** Milestone I4 first let a node's own key answer "who may attach to
me" the same way a shared fallback key always had — coherent in isolation, and wrong the moment grants
(below) carry scope, because it would let anyone holding *a* node's key walk past every grant on
*another* node. I5 split the lookup in two (`p9_auth_own_key()` for proving yourself, `p9_auth_key_for()`
for verifying someone else) specifically to close that gap; see the milestone's own commit for the
full reasoning.

### Grants: authorization, not just authentication

Before phase 21, any peer that authenticated at all — proved it held *some* configured key — received
the entire exported namespace, including a directory that runs Lisp programs by design. The **grants
list** (`peers`, backed by the same file phase 18's key list always was, now with two more columns)
turns that into a real access-control decision: each entry names a peer, its key, the one subtree it
may attach at (`/` for unrestricted), and whether it is read-only. A peer granted `/sd0/pgn` cannot
attach at `/`; a peer granted `ro` gets a `Twrite`/`Tcreate`/`Tremove` refusal with a reason, not a
silent failure. The list holds at most eight entries — bounded like every other table in this kernel —
and revocation is deleting a line: there is no expiry and no revocation list, because nothing here is
ever cached, so a removed grant stops working on the very next attach attempt.

### Security implications, stated plainly

Written down because an unstated limit gets credited as a feature.

#### What it defends

The exported namespace, against anything on the same network that does not hold a
configured key. With grants, it additionally bounds *what* a holder of a particular key may reach.

#### What it does not defend, and will not

- **Physical access.** Every secret here — the device key, a WLAN PSK — sits in flash in the clear.
  Anyone holding the board has them. This is inherited unchanged from phase 18 and is exactly why key
  rotation is a deliberate gesture (`--force`) rather than a prohibition: a key that can never be
  rotated turns a single compromise into a hardware replacement.
- **Confidentiality on the wire.** 9P frames are cleartext. Authentication proves *who* attached; it
  hides nothing they read afterward. Anything on the same network segment sees every byte.
- **Traffic analysis**, and **denial of service** — the connection-slot table is a fixed size, and
  that is a fixed size regardless of who is asking.
- **A compromised peer.** A grant names a key, and a key is a bearer token. Nothing here distinguishes
  the legitimate holder of a key from whoever else obtained a copy of it.

#### What it depends on

- **Entropy.** The whole scheme rests on `random_bytes()`. A provisioner refuses to mint a key at all
  when `random_is_hardware()` is false — every QEMU target, today — rather than mint a guessable one;
  `identity key --generate` on QEMU reports exactly this and asks for a key supplied by hand instead.
- **The server-chosen nonce.** Replay across sessions is defeated by a fresh nonce per `Tauth`; replay
  of one identity's response as another's is defeated by binding `uname` and `aname` into the response
  MAC. Both are phase 18's and both are unchanged here.

#### Where it could go

In rough order of value per effort — none of this is planned work, only a
recorded sense of what would matter most if it were ever taken up:

1. **Encryption over the same seam.** A handshake producing a session key, wrapped around the transport
   rather than inside the 9P server itself, so it stays a driver-shaped addition rather than a protocol
   rewrite. This is the largest real gap — everything above proves *who*, nothing hides *what*.
2. **RP2350 secure boot and OTP-backed secrets.** The silicon supports signed images and OTP key
   storage, which would make "anyone holding the board has the key" false for the first time.
3. **Signing the identity record** with a key the record itself does not contain, so a swapped storage
   medium becomes detectable instead of silently authoritative.
4. **An expiry/rotation protocol**, so rotating a compromised key stops requiring a physical visit.

### Provisioning walkthrough

This is the QEMU path — attaching a second virtio-blk device as the identity disk — since that is what
exists and is tested today; I7 will add an equivalent flashing step for real RP2350 boards without
changing anything below the storage layer.

**1. Mint an identity from the host**, without ever booting the board:

```bash
$ python3 tools/provision.py clock.img --name clock-3f2a
wrote clock.img: name='clock-3f2a' uid=7a1c9e4f02b6d831
```

That file is a raw 4 KB image in the exact format `kernel/idstore.c` reads — attach it as QEMU's second
`virtio-blk-device`, or write it to the RP2350's reserved sector, and the board's `identity`
report already shows `clock-3f2a` with `name source: record` before a single shell command runs.

**2. Or provision interactively, from the console**, which is what a bootstrap with no host tooling
looks like:

```bash
lsh> identity
name: rv64-mmu-a219 (derived)
mac: 02:4c:47:...:.. (derived (build seed))
uid: none (none (unprovisioned, no silicon id))
key fingerprint: none

lsh> identity provision
identity: provisioned
name: rv64-mmu-a219 (record)
mac: 02:4c:47:...:.. (derived (build seed))
uid: 7a1c9e4f02b6d831 (record)
key fingerprint: none

lsh> identity provision
identity provision: already provisioned; use --force to overwrite
```

`identity provision` mints what is missing and refuses to overwrite an already-provisioned record
without `--force` — the second call above shows the refusal, which is deliberate: re-running
provisioning by accident must not silently mint a new UID out from under a board already in service.

**3. Install this node's own device key**, so it can prove itself to a peer:

```bash
lsh> identity key --generate
identity key: no hardware entropy source on this target; install a key by hand instead
lsh> identity key 3fa1c88de4b0269917cc5a4408f1e2b6a9d047c3e51b8fa2601dd3c8e6f7091a
identity: key installed
key fingerprint: 77749c0e26076cbf
```

`--generate` refuses outright on QEMU (no hardware entropy source), which is the point: a provisioner
that minted a key anyway would be minting a guessable one. On real RP2350 silicon `--generate` works;
either way the command **never prints the key itself**, only its fingerprint — the one thing safe to
read aloud, paste into a bug report, or compare against a paper note.

**4. Grant a peer access, scoped to what it actually needs:**

```bash
lsh> peers add clock-3f2a 3fa1c88de4b0269917cc5a4408f1e2b6a9d047c3e51b8fa2601dd3c8e6f7091a /sd0/pgn rw
peers: granted 'clock-3f2a' at /sd0/pgn (rw)
lsh> peers
name             fingerprint      aname                mode
clock-3f2a       77749c0e26076cbf /sd0/pgn             rw
```

That peer can now attach *only* at `/sd0/pgn` — an attach at `/` is refused with `"attach: not granted
at this aname"` — and the fingerprint shown is the same one `identity` reported on the peer's own
console, which is exactly how an operator confirms the two boards agree on the same key without either
one ever displaying it.

**5. Install a WLAN credential — the derived PSK, never the passphrase:**

```bash
$ python3 tools/provision.py --selftest
  [ok] SSID='IEEE' passphrase='password' -> f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e
  [ok] different ssid/passphrase differ; same inputs are deterministic
WPA2_PSK_SELFTEST_OK

$ python3 tools/provision.py station.img --name kitchen-sensor \
    --wlan-ssid homenet --wlan-passphrase "a real passphrase here"
wrote station.img: name='kitchen-sensor' uid=0ba739d697bf1710 wlan_ssid='homenet' \
  wlan_psk=6e91faf94be6a5a4d58ae22f45b42f5f0fd5e97f1e46513ac0b9a039b4af480d
```
(`uid` is freshly random each run — `secrets.token_bytes(8)` — so a second run mints a different one;
the `wlan_psk` above is exactly what this SSID/passphrase pair derives to, reproducibly, every time.)

The passphrase exists only in that host-side command line and is never written to the image or sent to
a board — `derive_wpa2_psk()` runs PBKDF2-HMAC-SHA1 (WPA2's own construction, 4096 iterations) once, on
the host, and only the 256-bit result is stored. Attach `station.img` as the identity disk and the
board's own report already shows what was provisioned, no shell command needed to install anything:

```bash
lsh> wlan
ssid: homenet
psk fingerprint: 98d1f828c7a4ebe2
```

`wlan` (no arguments) reports the SSID in full — it is not a secret, every access point broadcasts it —
and the PSK's fingerprint, never the PSK. The same round trip works from the shell directly
(`wlan <ssid> <psk-hex>`, taking hex only — there is no code path here that accepts a passphrase).

### What a wrong fingerprint looks like

A fingerprint is the first 8 bytes of SHA-256 over a secret, shown as 16 hex characters. Two operators
comparing notes on two different boards should see the **same** fingerprint for the same key:

```
board A> identity
key fingerprint: 6685009530c3e0f4

board B> identity
key fingerprint: 6685009530c3e0f4          ← matches: both boards hold the same key
```

If a key was mistyped, copied from the wrong line of a provisioning log, or a board was swapped for a
similar-looking one on the bench, the fingerprints simply do not match:

```
board A> identity
key fingerprint: 6685009530c3e0f4

board C> identity
key fingerprint: eb8a50f5e24c1baf          ← does not match — this is not the key you think it is
```

There is no partial match, no "close enough": SHA-256 has no structure that makes similar keys produce
similar fingerprints, so a one-character typo and a completely different key look identical — a mismatch
of any kind. **The correct response to a mismatched fingerprint is to stop, not to keep trying keys
against it**: re-derive or re-read the intended key from its actual source (the provisioning host's own
output, not a note transcribed by hand) rather than pasting variations until something works, since a
fingerprint that *happens* to match after several attempts is exactly as informative as one that
matched on the first attempt for the wrong reason.

---

## History

- **2026-08-24: Release 0.13.1 — The clock driver becomes a driver, and two bugs it took hardware to find.**
  A patch release with no new features: it is about the clock persona being *right* rather than
  bigger. Three days of living with the clock produced three complaints — the dark-room brightness
  floor was still glaring, the panel flickered at a threshold, and one scan line was momentarily
  brighter once a second — and chasing them ended in a structural fix and two real bugs.

  **Brightness**, rewritten around the fact that the eye is logarithmic in luminance: the seven
  levels are geometric (`8, 18, 40, 90, 200, 450 µs` of OE per 1000 µs row) instead of linear, so
  level 1 is ~0.8 % duty instead of 14 %. Automatic brightness gained a six-boundary ladder in place
  of the vendor's single threshold, plus an EMA and a ±150-count deadband — a room sitting on a
  boundary no longer flickers, and a genuinely dark room now reaches the bottom of the scale.

  **The clock task became a real driver task** (`plan/phase17b_clock_task_split.md`). Phase 12 had
  served the *entire appliance loop* as one long `chan_call`, which made the clock the only RP2350
  driver task still running in kernel mode — and made phase 17's `clockisotest` item impossible as
  written. The appliance now runs in the caller's task, exactly where chess's UI loop runs, and the
  clock task is a frame-buffer-and-row-scan server confined in U-mode under five PMP grants. An op
  that carries one whole *frame* (~125 calls/s) is what made that affordable; the per-row cadence
  phase 12 rightly refused to put on a channel never had to leave the driver.

  **Two bugs found only on hardware.** RP2350's **ACCESSCTRL** gates peripherals to Secure-privileged
  by default, upstream of PMP and unreachable from a task's own domain: the newly-confined driver
  faulted on its first `TIMER0` read, and because a dead driver task silently falls back to direct
  hardware access, *the panel kept working while USB died* — the display is not evidence about the
  driver. And `console_pump()` latched Ctrl-C **inside** the loop that stops when its 128-byte ring
  is full, so once that ring filled (with our own tooling's 9P port-probe frames, as it turned out)
  a long-running program could never be interrupted again. Both fixed; the interrupt latch now runs
  off a non-consuming peek that cannot be starved by unread input.

- **2026-08-23: Release 0.13.0 — A clock that sets itself, and appliance mode.** The Pico-Clock-Green
  persona becomes a finished appliance. A **DCF-77 receiver** decodes the longwave time signal and sets
  the clock; the whole UI moved onto the board itself with a three-button menu (`SIG` signal monitor,
  `SYNC`, `LAST`, `AUTO` nightly sync, `TSET`, `BRT`, `OFFS`, `24H`, `BEEP`), a proportional 7-row font,
  weekday and indicator LEDs, and idle-screen shortcuts for temperature and date. The kernel clock now
  keeps **UTC**, with local time derived from a POSIX `TZ` rule (`CET-1CEST,M3.5.0,M10.5.0/3` by
  default) — because GPS and NTP speak UTC and a stored local time has no correct value during the hour
  that repeats every October.

  Three bugs found along the way were not clock bugs at all. The RP2350's **TIMER0 tick divisor** was
  OR-ed rather than written, so every clock in the system — uptime, delays, the display refresh, chess's
  search budget — had been running at 42.9% of real time. The **DS3231's I²C wire format** memcpy'd a
  native struct onto a byte protocol, silently disabling the read path and byte-swapping the year. And
  the PMP-granted **`.ustacksN` driver regions were never zeroed at boot**, so `usb_cdc.c`'s state came
  up as whatever SRAM held — which is why neither persona would boot from a plain USB power adapter
  unless it had just been flashed. All three were found on hardware; none was visible from a build or
  from QEMU. See `plan/phase17_clock_ui_and_dcf77.md` §9.

- **2026-08-22: Release 0.12.1 — Chess two-device polish.** Completed 0.12.0's mirroring: the keypad
  no longer prints raw key codes to the terminal, completed moves and engine replies are named in SAN
  on both front ends, and level/auto-reply/save/load changed from the keypad menu now report there
  too. Fixes a data-loss bug where the keypad's own *new game* item discarded the previous game
  instead of archiving it.

- **2026-08-22: Release 0.12.0 — Chess as a two-headed appliance.** The keypad/TFT board and the
  terminal are now one session with two live inputs *and* mirrored outputs — a move entered on either
  device redraws the ASCII board, the TFT and the 7-segment display alike, so neither is ever silently
  stale. Games are stored as real PGN with proper SAN (any chess GUI opens them), auto-saved after
  every move and auto-restored when a session starts, with `new` retiring the previous game to an
  archive and `save <name>` / `load <name>` / `games` for named slots.

- **2026-08-22: Release 0.11.0 — Heap space optimization.** Reclaimed RP2350 RAM by moving rare-but-large
  working buffers out of `.bss` onto an on-demand heap, tiering the Lisp string pool, board-scaling the
  9P and chess engine limits, and putting constant tables in flash — taking the managed heap from 212 KB
  to 356 KB (53 → 89 pages) and the chess persona's peak from 100% of the heap down to 31%. Also brings
  the previously unreleased 0.10.0 work (Lisp tail-call optimization, mark-sweep GC, a C-primitive
  standard library, and the `host/p9lib` + `host/fuse-p9` 9P host tooling), a link-time heap floor and
  `sizecheck` target to keep the budget from silently eroding again, and a chess board UI that accepts
  keypad and terminal input in one session.

- **2026-08-17: Release 0.9.0 — Microkernel with hardware-isolated drivers.** Every RP2350 driver runs as
  an independent U-mode task confined to its own PMP domain, verified per driver by a deliberate fault.
  Ships two proof-of-concept board personas: a chess computer (1.8" TFT + 4x4 keypad with 7-segment move
  entry) and a Pico-Clock-Green LED-matrix clock.

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
