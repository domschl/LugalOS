# LugalOS: Bare-Metal RISC-V Microkernel Operating System

**LugalOS** is a bare-metal, dependency-free microkernel operating system written in pure freestanding C11 and RISC-V assembly.

It is designed to scale dynamically from embedded **NOMMU** microcontrollers (like the **RP2350** / Pico 2) up to 64-bit **MMU** application processors (like the **Kendryte K210** and **VisionFive 2**).

---

## Key Features & Architecture

* **Microkernel Core & Fast IPC**: L4/seL4-style zero-copy register IPC rendezvous (`sys_ipc_call`, `sys_ipc_reply`) using RISC-V `ecall`.
* **Plan 9 Inspired Universal Namespace**: Everything is addressed through top-level resource paths:
  * `/ram0/` — FAT32 RAMDisk storage volume (`/ram0/notes.txt`).
  * `/proc/` — Synthetic process and kernel metrics (`/proc/ps`, `/proc/meminfo`, `/proc/version`).
  * `/dev/` — Hardware device nodes (`/dev/uart`, `/dev/null`, `/dev/zero`).
  * `/srv/` — Named process IPC channel registry (`/srv/lisp` $\rightarrow$ PID 2).
* **Storage & FAT32 Engine**: Native FAT32 filesystem engine supporting 32-bit cluster allocation, BPB formatting, file read/write, deletion, and volume listing.
* **Embedded Scheme / S-Expression Engine (`lugal-lisp`)**: Pure C S-expression parser, environment frames, closures, arithmetic/logic primitives, hardware `peek`/`poke`, and a `(compile-file)` S-expression compiler!
* **Native RISC-V ELF Compiler (`lisp-to-elf`)**: Compiles Lisp AST S-expressions directly to native RISC-V machine code (`add`, `sub`, `mul`, `ret`) and packages them into **ELF32 / ELF64** binaries on disk!
* **Teletype Line Editor (`ed`)**: Classic Unix teletype line editor supporting append (`a`), print (`p`), numbered print (`n`), delete (`d`), and write (`w`) modes over serial I/O.
* **Interactive Console Shell (`lsh`)**: Feature-rich shell supporting file management, Plan 9 `/proc/` introspection, Scheme REPL, `ed` editor, and native `exec` binary execution.

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
├── drivers/                 # MMIO 16550 UART driver & 512KB RAMDisk driver
├── fs/                      # FAT32 filesystem engine & Plan 9 VFS Server
├── kernel/                  # Microkernel main, scheduler, IPC, shell, printk
├── libc/                    # Freestanding C string library
├── user/
│   ├── ed/                  # Teletype line editor (`ed`)
│   └── lisp/                # Scheme REPL & RISC-V S-expression ELF compiler
├── linker/                  # QEMU virt linker scripts
└── scripts/                 # QEMU launcher scripts
```

---

## Building and Running in QEMU

### Prerequisites
* `riscv64-elf-gcc` or `riscv32-elf-gcc`
* `cmake` and `ninja`
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

## Interactive Workflow Example

```bash
LugalOS Interactive Console Shell (`lsh`)

# 1. Write Lisp program using ed line editor
lsh> ed /ram0/prog.lisp
:a
(+ 100 200)
.
:w
:q

# 2. Compile S-expression to native RISC-V ELF binary
lsh> lisp
lisp> (compile-file /ram0/prog.lisp /ram0/prog.elf)
[Lisp Compiler] Successfully compiled S-expression to native RISC-V ELF binary (136 bytes)
=> #t
lisp> exit

# 3. Execute native RISC-V ELF binary directly from shell
lsh> exec /ram0/prog.elf
[ELF] Executing binary '/ram0/prog.elf'...
[ELF] Native RISC-V binary '/ram0/prog.elf' exited with return code: 300
```

---

## License & Acknowledgments

LugalOS is licensed under the [MIT License](LICENSE).

Special thanks to:
* **Rui Ueyama** for [`chibicc`](https://github.com/rui314/chibicc) (MIT License).
* **Bell Labs** for the Plan 9 Operating System architectural model.
