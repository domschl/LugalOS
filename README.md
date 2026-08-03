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
│   └── rv64_mmu/            # 64-bit Sv39 3-level page table manager
├── cmake/                   # Cross-compilation toolchains (RV32 & RV64)
├── drivers/                 # VirtIO MMIO Block Driver, 16550 UART driver & RAMDisk driver
├── fs/                      # FAT32 filesystem engine (Subdirectories, BPB) & Plan 9 VFS Server
├── kernel/                  # Microkernel main, scheduler, IPC, shell, printk
├── libc/                    # Freestanding C string library
├── user/
│   ├── chibicc/             # Native C11 compiler (`chibicc`)
│   ├── ed/                  # Extended Unix teletype line editor (`ed`)
│   └── lisp/                # Scheme REPL & RISC-V S-expression ELF compiler
├── tools/                   # SD root template (`sd_root/`) & FAT32 disk image generator script
├── linker/                  # QEMU virt linker scripts
└── scripts/                 # QEMU launcher scripts
```

---

## Building and Running in QEMU

### Prerequisites
* `riscv64-elf-gcc` or `riscv32-elf-gcc`
* `cmake` and `ninja`
* `python3` (for FAT32 SD disk image pre-population)
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
* `(ls path)`: Performs directory listing across `/sd0/`, `/ram0/`, `/proc/`, `/dev/`, `/srv/`.
* `(mkdir path)`: Creates directory in FAT32 storage engine.
* `(rm path)`: Removes file from VFS.
* `(cp src dst)`: Copies file content between VFS locations.
* `(cat path)`: Reads and prints file content to UART console.
* `(ps)`: Displays task scheduler state (`/proc/ps`).
* `(meminfo)`: Displays physical memory allocation metrics (`/proc/meminfo`).

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

Special thanks to:
* **Rui Ueyama** for [`chibicc`](https://github.com/rui314/chibicc) (MIT License).
* **Ken Thompson & Bell Labs** for Unix `ed` and the Plan 9 Operating System architectural model.
