# LugalOS Board Porting & Bringup Guide

This guide provides step-by-step instructions for porting LugalOS to a new RISC-V microcontroller or development board.

---

## 1. Prerequisites & Architectural Invariants

Before porting, verify that the target hardware satisfies LugalOS core invariants:
1. **Target Architecture**: RISC-V (RV32I/E/M/A/C or RV64I/M/A/C).
2. **Machine Mode Boot**: The CPU boots in M-Mode with no firmware underneath (QEMU too: `-bios none`); on RV64 the kernel then runs in S-Mode for Sv39.
3. **Hardware Floating Point Disabled**: Boot code must explicitly initialize `mstatus.FS = 0`. All floating-point instructions are forbidden; mathematical routines must use pure fixed-point or exact integer math.
4. **Physical Memory Protection (PMP)**: The core should support standard RISC-V PMP registers for hardware driver isolation.

---

## 2. Porting Checklist

```
Step 1: Linker Script (linker/<target>.ld)
   |
Step 2: Boot Entry (arch/riscv/common/entry.S, a board boot header)
   |
Step 3: UART & Debug Console (drivers/uart_<target>.c)
   |
Step 4: Clock Tree & System Timer Bringup
   |
Step 5: Memory Domain Setup (PMP / Sv39)
   |
Step 6: CMake Preset & Board Definition (cmake/board-<target>.cmake)
   |
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
The script must define `_kernel_end` (the first page after the image) and `_heap_end`: `kernel_main()` hands that range to the page allocator (`palloc_init(_kernel_end, _heap_end)`). The heap may end above the region loadable sections use, where RAM is free only once the boot ROM is done with it (the ESP32-P4 v3's download buffers, `P4_HEAP_TOP`). Assert the floor you need (`ASSERT(_heap_end - _kernel_end >= ...)`), as the existing scripts do.

### Step 2: Boot Entry (`arch/riscv/common/entry.S`)
The shared entry point `_start` (with a per-board boot header where the ROM needs one, such as `arch/riscv/rp2350/boot_header.S`, or the second-stage loaders in `arch/riscv/esp32c6/` and `arch/riscv/esp32p4/`):
1. Clear BSS segment to zero.
2. Set up initial boot stack pointer (`sp`).
3. Leave `mstatus.FS = 0` (FPU disabled).
4. Point `mtvec` (or the CLIC) at `trap_vector_entry`.
5. Jump to `kernel_main()` (`kernel/main.c`).

### Step 3: UART Console
Implement `drivers/include/drivers/uart.h` for the chip: `uart_init(base)` brings the hardware up and polls (it runs long before the scheduler, so `printk()` works from the first lines of boot), and `uart_task_start()` later hands the UART to its driver task ([Drivers](drivers.md)).

### Step 4: Clock Tree & Hardware Timer
1. Configure system PLLs to the target frequency (e.g. 150 MHz on RP2350, 400 MHz on ESP32-P4).
2. Configure timer interrupts (RISC-V `mtimecmp` or silicon timer) to generate the periodic OS tick (typically 100 Hz to 1000 Hz).

### Step 5: Memory Domain PMP Integration
The PMP backend is `arch/riscv/common/mem_domain.c`; `arch/riscv/common/pmp_probe.c` measures the core's entry count and granularity at boot (`/proc/pmp`). Set `MEM_DOMAIN_MAX_REGIONS` (`kernel/include/kernel/mem_domain.h`) to what the core can hold beside the kernel's own entries.

### Step 6: CMake Preset
Add a preset in `CMakePresets.json` naming your board file `cmake/board-<target>.cmake` and a profile (`LUGALOS_PROFILE`, a `cmake/profile-<name>.cmake`). The board holds hardware facts only (`CONFIG_CLK_SYS_HZ`, pins, memory sizes, the drivers its hardware needs); policy and software choices belong in the profile (see [Hardware Presets](../../admin/presets.md#2-boards-and-profiles)). Target macros such as `CONFIG_BOARD_<TARGET>` come from `CMakeLists.txt`.
