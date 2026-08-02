# LugalOS (`lugalos`)

A scalable bare-metal operating system written from scratch in C for RISC-V architectures. Designed to scale seamlessly across **nommu** microcontrollers (like RP2350) and **MMU** application processors (like Kendryte K210 and VisionFive 2).

---

## Key Features

* **Pure C11 & RISC-V Assembly Core**: Zero external SDKs or vendor HAL dependencies.
* **Dual Architecture & Privilege Abstraction**:
  * **RV32 NOMMU (M-mode)**: Direct physical memory execution for microcontroller-class chips.
  * **RV64 MMU (S-mode & Sv39 Paging)**: Virtual memory page table management for application-class chips.
* **Build System**: Built using **CMake** and **Ninja**.
* **Serial Diagnostic Logging**: MMIO 16550 UART driver with formatted `printk`.
* **Process Management**: Basic round-robin process scheduler and context-switching framework.

---

## Directory Structure

```
lugalos/
├── CMakeLists.txt              # Root CMake configuration
├── README.md                   # Project documentation
├── cmake/
│   ├── toolchain-rv32-nommu.cmake # Toolchain for 32-bit NOMMU target
│   └── toolchain-rv64-mmu.cmake   # Toolchain for 64-bit MMU target
├── arch/
│   └── riscv/
│       ├── common/             # Boot assembly (_start), trap vectors, handlers
│       ├── include/arch/       # Abstraction headers (csr.h, trap.h, vmm.h)
│       ├── rv32_nommu/         # NOMMU memory manager implementation
│       └── rv64_mmu/           # Sv39 MMU memory manager implementation
├── kernel/
│   ├── include/kernel/         # Kernel headers (printk.h, sched.h)
│   ├── main.c                  # Kernel entry point
│   ├── printk.c                # Serial console formatter
│   └── sched.c                 # Process scheduler
├── drivers/
│   ├── include/drivers/        # Device driver interfaces (uart.h)
│   ├── uart_16550.c            # QEMU / K210 / VF2 16550 UART driver
│   └── uart_rp2350.c           # RP2350 hardware UART driver stub
├── linker/
│   ├── qemu-rv32.ld            # Linker script for 32-bit targets
│   └── qemu-rv64.ld            # Linker script for 64-bit targets
└── scripts/
    ├── run-qemu-rv32.sh        # Build & launch script for RV32 NOMMU
    └── run-qemu-rv64.sh        # Build & launch script for RV64 MMU
```

---

## Building & Verification

### Prerequisites
Ensure you have CMake, Ninja, and a RISC-V cross-compiler (`riscv64-elf-gcc` or `riscv-none-elf-gcc`) installed.

```bash
# Build RV32 NOMMU Target
cmake -B build/rv32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rv32-nommu.cmake
ninja -C build/rv32

# Build RV64 MMU Target
cmake -B build/rv64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rv64-mmu.cmake
ninja -C build/rv64
```

### Running in QEMU

```bash
# RV32 Target
qemu-system-riscv32 -M virt -nographic -bios none -kernel build/rv32/lugalos.elf

# RV64 Target
qemu-system-riscv64 -M virt -nographic -bios none -kernel build/rv64/lugalos.elf
```

---

## Development Roadmap

1. **Phase 1**: Bare-metal microkernel with UART, basic interrupts, and scheduling on RP2350.
2. **Phase 2**: Sv39 Virtual Memory, kernel/user isolation (`ecall` syscalls), page tables on K210.
3. **Phase 3**: Embedded RAMDisk, POSIX syscall subset (`read`, `write`, `open`, `sbrk`, `execve`).
4. **Phase 4**: Self-hosting (embedding TinyCC to compile and install itself).
