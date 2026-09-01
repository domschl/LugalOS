## Bugs

- ~~both `cat` and `e` (editor) have hard content limits. Current `init.el` exceeds those and gets cut off on 'cat' or 'e'. `cat` should read a stream, and editor should make use of heap allocations to adapt to the actual size, and refuse if there is not enough memory to avoid corruptions.~~
  **Fixed 2026-08-11.** `cat` (`user/lisp/lisp.c:prim_cat`) now opens the path
  through the handle API and streams fixed-size chunks by offset when the
  source reports a real size (FAT32/proc/remote9p); device nodes that report
  no size (`/dev/uart` and friends, some of which never signal EOF) still get
  a single bounded read, matching the old behavior exactly rather than
  hanging. `e` (`kernel/shell.c:shell_run_editor`) replaced the fixed 2 KB
  stack buffer with a `palloc_pages()` arena sized to the target file (stat'd
  first, doubled, page-rounded, 8 KB floor), released on exit, and refuses
  with a message instead of opening on allocation failure -- the same
  heap-on-demand shape C6/C7 already established for `cc`/`ed`. Verified live
  on QEMU: the 4378-byte `init.lisp` (old caps: 4096 for `cat`, 2048 for `e`)
  now reads and renders in full, including the previously-truncated tail.

- ~~prime.c example prints into kernel log (putchar, putnum)~~
  **Fixed 2026-08-11.** Root cause was one level down from the example:
  `SYS_PUTNUM`/`SYS_PUTCHAR` (`arch/riscv/common/trap.c`) went through
  `printk()`/`uart_putc()` -- the former appends everything it emits to the
  kernel log ring (by design for kernel/shell text, `kernel/printk.c`), so a
  tight print loop flooded `/proc/kmsg` with program output; the latter
  hard-coded the physical UART regardless of what the console is actually
  bound to (C8 port binding), the same class of bug already fixed once for
  `line_editor.c` (§6.3, `plan/phase6_memory_and_processes.md`). Both now
  route through `console_putc()` directly, bypassing the log ring entirely.
  Fixes `fib.c`'s identical, unreported instance of the same defect for
  free -- no example source needed to change. Verified live on QEMU: prime
  numbers appear on the console and do not appear in `cat /proc/kmsg`
  afterward.

  All three targets (rv32/rv64/rp2350) build clean; full QEMU suite still
  181/181.

## Phases x.. idea collection

### Kernel CONFIG

- Kernel CONFIG (with a menuconfig like tool) to adapt
  1. ~~platform (RP2350, QEMU, ...). Option to set defaults for platform (buffer sizes, resource allocation)~~
     **1 & 2 done, 2026-08-11** — see `plan/phase7_kernel_config.md` (K0-K3):
     a generated per-board header (`cmake/board-*.cmake` -> `lugalos_config.h`)
     now centralizes platform defaults (`PALLOC_MAX_PAGES`) and RP2350's pin
     map (UART0/SPI1/LEDs), replacing scattered `#ifdef`s and duplicated
     literals. Introspectable via `cat /proc/config`. No interactive
     menuconfig tool yet -- the config source is a hand-edited CMake file,
     which was a deliberate phase-1 scope decision, not an oversight.
  2. hardware to port mapping: port-to-pin/gp mapping (UARTn[Tx->pin_a, Rx, pin_b], IC2m[..], GPIOi[pin_x], SPIj[...]
  3. port_to_driver mapping: GPIOi=>heart_beat, I2Cm[auto] (autodetects hardware and binds drivers), alternative list of I2Cm[port_a]=eeprom, SPIj=>sd_card (or network(W5500), display types, interconnect)
  4. software options, possibility to enable or disable functionality (e.g. sensor node doesn't need cc, chess computer doesnt need networking, etc.)

### Extensions

- True allocator (malloc(), free()), not primarily for programs that use extensive dynamic allocation (with all the defragmentation issues arising from that), but to allow specific, overhead-free, allocation of state. 
- Multi-core support
  **Planned 2026-08-29** — see `plan/phase22_smp_locking_foundation.md` (a
  real cross-hart lock, replacing every `irq_save()`-as-mutex site) and
  `plan/phase23_multicore_scheduling.md` (waking RP2350's second Hazard3
  core). Planning documents only; sequenced after phase 19's R4/R5 and
  phase 21's I7, all three blocked on hardware access today.
- database server (sqlite, possible?)
- NTP server: a DCF-77-disciplined clock serving time to the segment
  **Planned 2026-09-01** -- see `plan/phase24_dcf77_precision_and_ntp_server.md`.
  The NTP *client* landed as phase 19's R6. The server needs a continuously
  disciplined clock rather than a protocol, which is what that phase is about;
  it also evaluates and closes the phase-modulation (PZF) question, and holds
  PIO capture in reserve behind a measured trigger.

### New hardware

- PSRAM support
  - maybe first as RAM disk only?
- Displays (SPI, SP7735, ...) usage via a. canvas library (graphics primitives, text, bitblt from C and Lisp), b. console, (Plan 9 display server?)
   c. raw? (for high perf. apps?)
- Keyboard support: QYF-TM1638 (4x4 matrix + 8 7segment), PS/2 standard, I2C-keyboard (M5 Stack CardKB)
- Network (W5500 ethernet), blackbox support for RP2350 W wireless
- I2C environment sensors
- DVI graphics (RP2350 PIO programming)
  
### Integration

- Host computer libraries (Linux, macOS) to allow rich access of Plan 9 protocol (Prio 1: python package, 2: C lib). fuse-fs?. Extension of test-infrastructure
  
### New platforms

- K210 MAIX bit (to consider: niche hardware, older, huge SRAM) ('compute-node'?, can maybe even do simply AI stuff, image-recog?)
- ESP32 P4 (to consider: hardware (probably!) not fully open, binary blobs, esp. for ESP32 C6 wifi modules that are connected, comes with ethernet): Model: Waveshare ESP32-P4-NANO + Lautsprecher, RISC-V,32MB PSRAM, HMI, MIPI-CSI/ DS, 28 GPIOs
- VisionFive 2 (entirely different class of hardware, contains many more complex problems, allows bare metal)

### Application scenarios:

- Stand-alone 'workstation' with keyboard, display, and P9 connectivity
- Chess computer (port of ~/gith/domschl/LugalChess) to LugalOS (uses SP7735 128x160 graphics and QYF-TM1638 for input and additional status infos)
- remote environment sensors
- clock with matrix led display (waveshare pico-clock-green: https://www.waveshare.com/wiki/Pico-Clock-Green )
  
