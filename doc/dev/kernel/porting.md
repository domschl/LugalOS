# LugalOS Board Porting & Bringup Guide

This guide provides step-by-step instructions for porting LugalOS to a new RISC-V microcontroller or development board.

---

## 1. Prerequisites & Architectural Invariants

Before porting, verify that the target hardware satisfies LugalOS core invariants:
1. **Target Architecture**: RISC-V (RV32I/E/M/A/C or RV64I/M/A/C).
2. **Machine Mode Boot**: The CPU must boot in M-Mode (for microcontrollers) or S-Mode under OpenSBI (for virtual machines).
3. **Hardware Floating Point Disabled**: Boot code must explicitly initialize `mstatus.FS = 0`. All floating-point instructions are forbidden; mathematical routines must use pure fixed-point or exact integer math.
4. **Physical Memory Protection (PMP)**: The core should support standard RISC-V PMP registers for hardware driver isolation.

---

## 2. Porting Checklist

```
Step 1: Linker Script (linker/<target>.ld)
   │
Step 2: Low-Level Boot Assembly (arch/riscv/<target>/boot.S)
   │
Step 3: Early UART & Debug Console (drivers/uart_<target>.c)
   │
Step 4: Clock Tree & System Timer Bringup
   │
Step 5: Memory Domain Setup (PMP / Sv39)
   │
Step 6: CMake Preset & Board Definition (cmake/board-<target>.cmake)
   │
Step 7: Launch Microkernel & Shell (kernel/main.c)
```

---

## 3. Step-by-Step Implementation

### Step 1: Linker Script (`linker/<target>.ld`)
Define the memory map separating SRAM, Flash, and optional PSRAM:
```ld
MEMORY {
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 2048K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
    PSRAM (rwx) : ORIGIN = 0x48000000, LENGTH = 32M  /* If available */
}
```
Define symbols for `__heap_start` and `__heap_end` for the `palloc` page frame allocator.

### Step 2: Boot Code (`arch/riscv/<target>/boot.S`)
1. Clear BSS segment to zero.
2. Set up initial boot stack pointer (`sp`).
3. Set `mstatus.FS = 0` (disable FPU).
4. Configure vector base address register (`mtvec` or CLIC) to point to `trap_entry`.
5. Jump to `kmain()`.

### Step 3: Early UART Console
Implement two primitive functions:
* `early_uart_init()`: Configures baud rate and pin mux.
* `early_uart_putc(char c)`: Polls transmit FIFO and writes one byte.
This enables immediate `printk()` diagnostic logging before the scheduler starts.

### Step 4: Clock Tree & Hardware Timer
1. Configure system PLLs to the target frequency (e.g. 150 MHz on RP2350, 400 MHz on ESP32-P4).
2. Configure timer interrupts (RISC-V `mtimecmp` or silicon timer) to generate the periodic OS tick (typically 100 Hz to 1000 Hz).

### Step 5: Memory Domain PMP Integration
Ensure the core's PMP granularity is detected and wire it into `kernel/mem_domain_pmp.c`.

### Step 6: CMake Preset
Add a preset in `CMakePresets.json` referencing your board file `cmake/board-<target>.cmake`. Define board macros (`CONFIG_BOARD_<TARGET>`, `CONFIG_CLK_SYS_HZ`, etc.).
