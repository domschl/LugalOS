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
* **Embedded Scheme / S-Expression Engine (`lugal-lisp`)**: Pure C S-expression parser, environment frames, closures, arithmetic/logic primitives, hardware `peek`/`poke`, and a `(compile-file)` S-expression compiler!
* **Native RISC-V ELF Compiler (`lisp-to-elf`)**: Compiles Lisp AST S-expressions directly to native RISC-V machine code (`add`, `sub`, `mul`, `ret`) and packages them into **ELF32 / ELF64** binaries on disk!
* **Extended Unix Teletype Line Editor (`ed`)**: Classic Thompson Unix `ed` editor with current line pointer `dot`, line range addressing (`.`, `$`, `,`, `%`, `N,M`), insert (`i`), append (`a`), change (`c`), delete (`d`), print (`p`), numbered print (`n`), substitution (`s/old/new/`), search (`/pattern/`), and file I/O (`e`, `w`, `f`).
* **Interactive Console Shell (`lsh`)**: Feature-rich shell supporting file and directory management (`ls`, `cat`, `touch`, `mkdir`, `rmdir`, `cp`, `rm`), Plan 9 `/proc/` introspection, Scheme REPL, `chibicc`, `ed` editor, and native `exec` binary execution.

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

### 3. Compiling C Source Files to RISC-V ELF Binaries (`chibicc`)
```bash
lsh> cc /sd0/hello.c /sd0/hello.elf
[chibicc] Compiled /sd0/hello.c -> /sd0/hello.elf

lsh> exec /sd0/hello.elf
[ELF] Executing binary '/sd0/hello.elf'...
Hello from Extended ed Editor!
```

---

## License & Acknowledgments

LugalOS is licensed under the [MIT License](LICENSE).

Special thanks to:
* **Rui Ueyama** for [`chibicc`](https://github.com/rui314/chibicc) (MIT License).
* **Ken Thompson & Bell Labs** for Unix `ed` and the Plan 9 Operating System architectural model.
