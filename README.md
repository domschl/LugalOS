# LugalOS: Bare-Metal RISC-V Microkernel Operating System

**LugalOS** is a bare-metal, dependency-free microkernel operating system written in pure freestanding C11 and RISC-V assembly.

It is designed to scale dynamically from embedded **NOMMU** microcontrollers (like the **RP2350** / Pico 2) up to 64-bit **MMU** application processors (like the **Kendryte K210** and **VisionFive 2**).

---

## Key Features & Architecture

* **Microkernel Core & Fast IPC**: L4/seL4-style zero-copy register IPC rendezvous (`sys_ipc_call`, `sys_ipc_reply`) using RISC-V `ecall`.
* **Plan 9 Inspired Universal Namespace**: Everything is addressed through top-level resource paths:
  * `/sd0/` — FAT32 VirtIO persistent SD storage volume (`/sd0/docs/readme.txt`).
  * `/ram0/` — FAT32 in-memory RAMDisk storage volume (`/ram0/notes.txt`).
  * `/proc/` — Synthetic process and kernel metrics (`/proc/ps`, `/proc/meminfo`, `/proc/version`).
  * `/dev/` — Hardware device nodes (`/dev/uart`, `/dev/null`, `/dev/zero`).
  * `/srv/` — Named process IPC channel registry (`/srv/lisp` $\rightarrow$ PID 2).
* **Storage Engine & VirtIO Block Device**:
  * Native FAT32 filesystem engine supporting 32-bit cluster allocation, subdirectories (`.`, `..`), BPB formatting, file read/write, deletion, directory creation (`mkdir`), removal (`rmdir`), and copying (`cp`).
  * Hardware **VirtIO MMIO Block Driver** backed by a persistent shared disk image (`build/lugalos_sd.img`) used across both 32-bit and 64-bit builds.
* **Native C11 Compiler (`chibicc`)**: Integrated C11 compiler (`cc <src.c> <dst.elf>`) generating native RISC-V ELF binaries directly on LugalOS!
* **Unified Lisp Machine Shell (`lsh`)**:
  * **POSIX $\rightarrow$ S-Expression Transformation**: All standard POSIX shell inputs (`ls /sd0`, `cp a b`, `cc src dst`) are automatically transformed into Lisp S-Expressions (`(ls "/sd0")`, `(cp "a" "b")`) and executed directly by the core Lisp engine!
  * **Complete Scheme / Lisp Core**: Full support for `define`, `lambda`, `quote` (`'`), `if`, `begin`, `let`, `cond`, arithmetic (`+`, `-`, `*`, `=`), memory `peek`/`poke`, and string data types.
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
* **Automated Integration Test Harness**: Non-interactive QEMU PTY integration runner (`tests/runner.py`) executing 37 automated test cases across RV32 (NOMMU) and RV64 (Sv39 MMU) builds.




---

## Directory Structure

```
lugalos/
├── arch/riscv/
│   ├── common/              # RISC-V assembly entry point, traps, ELF loader
│   ├── include/arch/        # CSRs, Trap frames, VMM, ELF headers
│   ├── rv32_nommu/          # 32-bit physical identity memory mapping
├── cmake/                   # Cross-compilation toolchains (RV32, RV64, RP2350)
├── drivers/                 # UART drivers (16550 / PL011 / RP2350), VirtIO Block, RAMDisk
├── fs/                      # FAT32 filesystem engine (Subdirectories, BPB) & Plan 9 VFS Server
├── kernel/                  # Microkernel main, scheduler, IPC, shell, printk
├── libc/                    # Freestanding C string library
├── linker/                  # Linker scripts (QEMU virt RV32/64, RP2350 XIP Flash)
├── tools/                   # SD root template, FAT32 disk image generator, UF2 packager
└── user/
    ├── chibicc/             # Native C11 compiler (`chibicc`)
    ├── ed/                  # Extended Unix teletype line editor (`ed`)
    └── lisp/                # Scheme REPL & RISC-V S-expression ELF compiler
```

---

## Building and Running in QEMU

### Toolchain & Prerequisites
LugalOS uses a **single, unified 64-bit cross-compiler toolchain** (`riscv64-elf-gcc`) for all targets (both 64-bit MMU and 32-bit NOMMU / RP2350). The 64-bit toolchain target compiler compiles 32-bit RISC-V code cleanly via `-march=rv32imac_zicsr_zbs -mabi=ilp32`.

* `riscv64-elf-gcc` (Unified 64-bit cross-compiler toolchain)
* `cmake` and `ninja`
* `python3` (for FAT32 SD disk image pre-population and Flash ROMDisk generation)
* `qemu-system-riscv32` and `qemu-system-riscv64`

### Build & Run RV32 (NOMMU) Target
```bash
cmake -B build/rv32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rv32-nommu.cmake
ninja -C build/rv32
./scripts/run-qemu-rv32.sh
```

### Build & Run RV64 (Sv39 MMU) Target
```bash
cmake -B build/rv64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rv64-mmu.cmake
ninja -C build/rv64
./scripts/run-qemu-rv64.sh
```

---

## Running on Hardware: Raspberry Pi Pico 2 (RP2350)

LugalOS boots on the **Raspberry Pi Pico 2** (RP2350 RISC-V Hazard3 core) with UART console over USB-Serial.

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

### Build for RP2350

```bash
cmake -B build/rp2350 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rp2350.cmake \
    -DLUGALOS_TARGET=RP2350
ninja -C build/rp2350
# Generates: build/rp2350/lugalos.uf2
```

### Flash to Pico 2

1. Hold **BOOTSEL** button on Pico 2 while plugging in USB (or while powering on via CP2101 5V).  
   The board mounts as a USB mass storage device called `RP2350`.

2. Copy the UF2 firmware:
   ```bash
   cp build/rp2350/lugalos.uf2 /media/$USER/RP2350/
   ```

3. The Pico 2 will flash, reboot automatically, and start LugalOS.

### Open Serial Console

```bash
picocom -b 115200 /dev/ttyUSB0
# or
minicom -b 115200 -D /dev/ttyUSB0
```

Expected output after boot:
```
LugalOS Microkernel Lisp Machine v0.4.0
[VFS Server] ...
lsh>
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
* `(let ((var val) ...) body...)`: Establishes local lexical bindings.
* `(cond (clause1) (clause2) ... (else default))`: Multi-branch conditional selection.

### Built-in Primitives & Standard Library

#### Arithmetic & Logic
`+`, `-`, `*`, `/`, `=`, `<`, `>`, `<=`, `>=`

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
* `(ps)`: Displays task scheduler state (`/proc/ps`).
* `(meminfo)`: Displays physical memory allocation metrics (`/proc/meminfo`).
* `(df)`: Displays mounted volume capacity and cluster usage (`/proc/df`).
* `(top)`: Displays system process, memory, and storage monitor dashboard.

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
* **Rui Ueyama's chibicc**: C11 compiler architecture adapted from [`chibicc`](https://github.com/rui314/chibicc) (MIT License).
* **Ken Thompson & Bell Labs**: Unix `ed` teletype editor and the Plan 9 Operating System universal namespace model.
