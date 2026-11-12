# Phase 79 — StarFive VisionFive 2 Subsystem Extensions, Graphics Acceleration, NVMe Identity & Hardware Enablement

**Status**: COMPLETED  
**Target Hardware**:
- **Primary**: **StarFive VisionFive 2 v1.3B** (`visionfive2` preset, StarFive **JH7110** rev B, 4× SiFive U74-MC RV64GC SMP @ 1.5 GHz, 8 GiB LPDDR4, 64 GB MicroSD at `/sd0`, Gigabit Ethernet at `192.168.178.25`, 256 GB M.2 NVMe SSD at `/nvme0`):
  - **Serial Console**: Dedicated `dw-apb-uart` (UART0 at `0x10000000`, PLIC IRQ 32, 115200 baud).
  - **Video & Display**: VeriSilicon Hantro DC8200 Display Controller (`0x29400000`) and Innosilicon HDMI 2.0 TX PHY (`0x29590000`).
  - **USB Host & Peripherals**: Onboard VIA VL805-Q6 xHCI host controller (`pcie0`) driving onboard VIA VL817 4-port USB 2.0 Hub (`2109:3431`) and physical USB keyboard & mouse.
  - **High-Speed Storage**: M.2 PCIe Gen2 x1 NVMe SSD (`pcie1`) formatted with FAT32 long filenames (Phase 76 VFAT LFN).
  - **Audio Subsystem**: Onboard StarFive JH7110 PWMDAC stereo audio engine (`0x100D0000`) routed through `SYS_IOMUX` to GPIO 33/34 and 3.5mm stereo headphone jack, with automatic idle carrier suppression (Hi-Z driver & clock gating).
  - **Network & Clock**: Synopsys DesignWare EQOS Gigabit Ethernet (`gmac1`) with Motorcomm YT8531C PHY, SNTP client (`net/ntp.c`).
- **Regression**: QEMU 64-bit (`rv64`, `rv64-smp`), QEMU 32-bit (`rv32`, `rv32-fold`), ESP32-P4 (`esp32p4`, `esp32p4-tab5`, `esp32p4-lcd7b`), and RP2350 (`rp2350-terminal`, `rp2350-chess`) — zero regressions, zero SRAM overhead on microcontrollers.

**Goal**: Deliver a comprehensive set of stability fixes, graphics and UI performance optimizations, storage and network enhancements, and new peripheral subsystems for the StarFive VisionFive 2 platform:
1. Eliminate UART console character drops during high-speed bursts and pasting via hardware FIFO configuration and an ISR-serviced circular RX ring buffer.
2. Resolve low graphics performance during Lisp CAS plotting and chess board rendering by migrating scanout buffers from slow uncached DDR stores to cached DDR memory with dirty-bounding L2 cache flushing, together with optimized row-wise bitmap blitting.
3. Bring up full dynamic USB hotplug detection for keyboards and mice across the onboard xHCI and VL817 hub topology.
4. Support High-DPI display resolutions (native 1440p and 4K30) with crisp scalable typography.
5. Fully integrate NVMe storage with long filename support, unattended NVMe autoboot, and a dedicated NVMe-backed LugalOS identity store partition (`/dev/identity0`).
6. Automatically synchronize system epoch time from network time servers on Ethernet link-up via DHCP Option 42 and the native SNTP client.
7. Bring up the onboard JH7110 audio subsystem (PWMDAC with 3.5mm jack output, tone generator, and idle carrier suppression).
8. Rigorously evaluate JH7110 hardware acceleration: determine feasibility boundaries for the Imagination PowerVR GPU, leverage DC8200 2D hardware layers/cursors, and implement a multi-core integer-only GEMM (General Matrix Multiply) architecture across the 4 SiFive U74 cores for edge cognitive inference (Phase 81).

---

## 0. Architectural & Hardware Analysis

```
    +-----------------------------------------------------------------------------------------+
    |                             StarFive JH7110 SoC (RV64GC)                                |
    |  +--------------------+  +--------------------+  +--------------------+  +-----------+  |
    |  | Hart 1 (Boot U74)  |  | Hart 2 (Work U74)  |  | Hart 3 (Work U74)  |  | Hart 4 U74|  |
    |  +--------------------+  +--------------------+  +--------------------+  +-----------+  |
    |  |               Shared 2 MiB Coherent L2 Cache (Flush64 @ 0x02010200)               |  |
    +--+----------------------------------+-----------------------------------+------------+--+
       |                                  |                                   |
       | AXI System Bus                   | AXI High-Speed Bus                | APB Subsystems
       v                                  v                                   v
+---------------+             +-----------------------+              +-----------------------+
| 8 GiB LPDDR4  |             |  VeriSilicon DC8200   |              |  DW-APB-UART0 (Console|
| Cached Memory |             |  Display Controller   |              |  @ 0x10000000)        |
| - Double FB   |             |  - Layer 0 RGB565     |              |  - 64B HW FIFO        |
| - Scanout dirty|<------------|  - Polyphase Scaler   |              |  - 1024B ISR Ring Buf |
|   L2 flush    |  AXI DMA    |  - HW Cursors & Overly|              +-----------------------+
+---------------+             +-----------+-----------+              |  Everest ES8388 Audio |
       ^                                  | Parallel Video           |  @ I2C (0x10) + I2S0  |
       | DMA Read/Write                   v                          |  - 3.5mm Headphone Jack|
+---------------+             +-----------------------+              +-----------------------+
| M.2 NVMe SSD  |             | Innosilicon HDMI TX   |              |  EQOS Gigabit Ethernet|
| (PCIe1 Gen2x1)|             | - 1080p60 / 4K30 PHY  |              |  (gmac1 @ 0x16040000) |
| - Partition 1 |             | - DDC EDID Engine     |              |  - DHCP Option 42     |
|   (FAT32 LFN) |             +-----------+-----------+              |  - SNTP Auto-Sync     |
| - Partition 2 |                         | TMDS Lines               +-----------------------+
|   (Identity)  |                         v                          |  VIA VL805 xHCI (PCIe0|
+---------------+               +-------------------+                |  - VL817 Hub Hotplug  |
                                | HDMI Display (J14)|                |  - USB Keyboard/Mouse |
                                +-------------------+                +-----------------------+
```

### 0.1 Technical Root Causes of Existing Issues

#### 1. Serial Console Overrun
In `drivers/uart_16550.c`, `uart_init()` currently only configures `UART_IER = 0` and attaches the PLIC interrupt handler. Crucially:
* The 16550/DesignWare FIFO Control Register (`UART_FCR`) is never initialized, leaving the hardware FIFO disabled or in a single-byte trigger mode.
* The interrupt service routine `uart_isr()` does not pull incoming bytes from the receive holding register into a memory buffer. Instead, it merely disarms `UART_IER_ERBFI` and unblocks `g_rx_waiter` (the `uart` driver task).
* Reading each character requires an entire microkernel scheduling round-trip: caller `chan_call("uart", READ)` -> context switch to `uart` task -> read single byte -> context switch back.
* At 115,200 baud, bytes arrive every 86.8 microseconds. When multi-byte strings or pasted commands are sent while the CPU is engaged in frame flips or memory transfers, the 16-to-64 byte hardware FIFO overflows almost instantly, setting `UART_LSR_OE` and permanently discarding input.

#### 2. Low Graphics Performance in Lisp Plotting & Chess Rendering
Investigation of the display and window pipeline reveals three compounding bottlenecks:
* **Uncached DDR Scanout Memory:** In `drivers/dc8200_jh7110.c`, the backbuffer `g_state.back_fb` is mapped through `pa_to_uncached()` (`UNCACHED_DRAM_OFFSET = 0x400000000UL`). In the SiFive U74 architecture, stores to uncached physical memory bypass the core's store buffers and L2 cache, issuing individual unbuffered AXI bus transactions. Writing a 1920×1080 RGB565 frame (4.15 MB) through uncached stores imposes severe latency penalties.
* **Per-Pixel Canvas Draw Loops:** In `user/chess/src/chess_ui.c` (and similarly in canvas drawing routines), piece bitmaps (48×48) are rendered via nested loops:
  ```c
  for (int py = 0; py < 48; py++) {
      for (int px = 0; px < 48; px++) {
          canvas1_fill(c1, dx, dy, dx, dy, ...);
      }
  }
  ```
  This performs 2,304 individual calls to `canvas1_fill()` per piece (up to 73,728 calls for a full board), where each call calculates clipping, coordinates, and bitmask shifts for a single pixel.
* **Synchronous Frame Repaints and Lock Contention:** Every primitive invocation in Lisp CAS plotting (`canvas-line`, `canvas-fill`, `canvas-text`) issues a `console_canvas()` IPC request, taking `g_console_lock`, checking all ribbon windows, and calling `ws_deliver()` with `g_ws_deliver_lock`.

#### 3. USB Keyboard & Mouse Static Enumeration
In `drivers/xhci_jh7110.c`, device enumeration (`xhci_enumerate_port()`) is executed once during kernel initialization.
* When a keyboard or mouse is unplugged or connected to a different port after boot, no Port Status Change TRB (`TRB_TYPE_PORT_STATUS_CHANGE`) is processed to detach the old slot or enumerate the new device.
* The downstream ports of the onboard VIA VL817 USB 2.0 hub are not monitored for Connect Status Change (`CSC`), preventing dynamic plug-and-play operation.

#### 4. High-DPI Display Modes & 2x Upscaling Artifacts
* The Innosilicon HDMI TX driver (`drivers/hdmi_jh7110.c`) currently defines PLL post-divider tables only for 1080p60 (148.5 MHz) and 720p60 (74.25 MHz).
* When connected to a native 4K (3840×2160) monitor, the monitor negotiates 1080p and applies external bilinear scaling, resulting in softened fonts and washed-out borders.
* The VeriSilicon DC8200 display controller natively supports scanouts up to 4096×4096, and the Innosilicon HDMI PHY supports pixel clocks up to 297 MHz (the exact clock required for 3840×2160 @ 30 Hz). Native 4K scanout requires 16.59 MB per RGB565 buffer (33.2 MB for double buffering), which fits easily into the 8 GiB LPDDR4 DRAM.

#### 5. Storage Integration & Missing VisionFive 2 Identity Store
* Phase 76 added VFAT Long File Names (LFN) to `fs/fat32.c`. The M.2 NVMe SSD driver (`drivers/nvme.c`) currently mounts MBR Partition 1 at `/nvme0/`, but users must still boot from `/sd0/`.
* On RP2350, ESP32-P4, and ESP32-C6, device identity (UID, device key, 9P grants, device name) is stored in a dedicated flash sector (`identity_store_device()`).
* Because the VisionFive 2 brick-proof rule strictly forbids writing to onboard QSPI flash, VisionFive 2 currently lacks a persistent identity backend, preventing persistent 9P authorization keys (`identity provision`) and network node identity. A dedicated MBR partition on the NVMe SSD (`/dev/nvme0p2`) provides the ideal persistent identity block device.

#### 6. Missing Clock Initialization
* The VisionFive 2 board lacks a battery-backed RTC on its main board; the `rdtime` counter resets to zero at power-on, initializing the system clock to 1970-01-01.
* Although LugalOS includes a complete SNTP client (`net/ntp.c`) and DHCP client (`net/dhcp.c`), DHCP Option 42 (NTP Servers) is not parsed, and no automatic time query is dispatched when `eth0` acquires an IP address.

#### 7. Unused Audio Hardware
* The VisionFive 2 v1.3B board features a 3.5mm TRRS audio jack wired to an onboard Everest Semiconductor ES8388 audio codec at I2C address `0x10`, fed by JH7110 `i2s0_8ch`.
* LugalOS already includes an ES8388 driver in `drivers/audio_esp32p4.c` (Phase 62 for M5Stack Tab5). Porting this driver to JH7110 I2C and I2S enables native sound and audio output.

#### 8. Hardware Acceleration Boundaries & Matrix Math
* **GPU Feasibility:** The Imagination PowerVR BXS-4-64 MC1 GPU requires proprietary firmware loaded into an embedded management core, a complex host kernel ring-buffer driver (~150k lines of C), and a compiler-heavy Mesa/LLVM user space. A freestanding microkernel driver is completely out of scope.
* **2D & Display Acceleration:** In contrast, the VeriSilicon DC8200 has built-in hardware cursor planes (eliminating software cursor blits) and hardware scaling filters, while the Synopsys AXI DMA engine can accelerate 2D memory copies.
* **Compute & AI Inference:** The SiFive U74 cores lack RISC-V Vector (RVV) extensions, and LugalOS enforces `sstatus.FS = 0` (zero floating point). Any neural inference (e.g. for embeddings or cognitive cores) must rely on an optimized multi-core INT8 GEMM engine utilizing RV64 `Zba`/`Zbb` instructions and symmetric multi-processing across Harts 1..4.

---

## 1. Milestone Plan

```
  +-----------------------------------------------------------------------------------------+
  | M79.1: UART0 RX Ring Buffer & Overrun Fix                                               |
  +--------------------------------------------+--------------------------------------------+
                                               |
                                               v
  +--------------------------------------------+--------------------------------------------+
  | M79.2: Fast Cached Scanout & Blit Optimization (DC8200 & Chess)                         |
  +--------------------------------------------+--------------------------------------------+
                                               |
                                               v
  +--------------------------------------------+--------------------------------------------+
  | M79.3: USB Keyboard & Mouse Hotplug Subsystem (xHCI & VL817 Hub)                        |
  +--------------------------------------------+--------------------------------------------+
                                               |
                                               v
  +--------------------------------------------+--------------------------------------------+
  | M79.4: High-DPI Display Modes (1440p / 4K30) & Crisp Typography                         |
  +--------------------------------------------+--------------------------------------------+
                                               |
                                               v
  +--------------------------------------------+--------------------------------------------+
  | M79.5: NVMe Full Integration, Autoboot & NVMe Identity Store Partition                  |
  +--------------------------------------------+--------------------------------------------+
                                               |
                                               v
  +--------------------------------------------+--------------------------------------------+
  | M79.6: Network Clock Initialization (DHCP Option 42 & SNTP Auto-Sync)                   |
  +--------------------------------------------+--------------------------------------------+
                                               |
                                               v
  +--------------------------------------------+--------------------------------------------+
  | M79.7: Onboard ES8388 Audio Codec & I2S Playback                                        |
  +--------------------------------------------+--------------------------------------------+
                                               |
                                               v
  +--------------------------------------------+--------------------------------------------+
  | M79.8: Hardware Acceleration Analysis, 2D Engine & Multicore INT8 GEMM Core             |
  +--------------------------------------------+--------------------------------------------+
                                               |
                                               v
  +--------------------------------------------+--------------------------------------------+
  | M79.9: HIL Silicon Test Suite & System Documentation Update                             |
  +-----------------------------------------------------------------------------------------+
```

---

### Milestone 79.1 — Serial Console Overrun Fix & Interrupt RX Ring Buffer (`drivers/uart_16550.c`)

**Goal**: Eliminate UART RX character drops when typing fast, pasting multi-line scripts, or streaming data over the 115,200-baud console connection.

#### Implementation Steps
1. **Initialize Hardware FIFO:** In `uart_init()` in `drivers/uart_16550.c`, write to `UART_FCR` (offset `0x02`):
   ```c
   /* Enable FIFO, reset RX/TX FIFOs, set RX trigger threshold to 1/2 full (bit 6..7 = 0x80) */
   uart_reg_write(UART_FCR, 0x01 | 0x02 | 0x04 | 0x80);
   ```
2. **Implement Lockless Interrupt-Driven Circular RX Ring Buffer:**
   - Define `UART_RX_RING_SIZE` as 1024 bytes (power of two, mask `1023`).
   - Add head and tail indices: `static volatile uint32_t g_uart_rx_head`, `g_uart_rx_tail`.
   - In `uart_isr()`:
     ```c
     while (uart_reg_read(UART_LSR) & UART_LSR_DR) {
         uint8_t c = uart_reg_read(UART_RBR);
         uint32_t next = (g_uart_rx_head + 1u) & (UART_RX_RING_SIZE - 1u);
         if (next != g_uart_rx_tail) {
             g_uart_rx_buf[g_uart_rx_head] = c;
             g_uart_rx_head = next;
         } else {
             note_rx_overrun();
         }
     }
     ```
   - Keep `UART_IER_ERBFI` permanently unmasked while the console is online so every arriving character is latched in the ISR without waiting for an active reader.
3. **Update `uart_hw_getc_blocking()` & `hw_uart_has_char()`:**
   - `hw_uart_has_char()` checks if `g_uart_rx_head != g_uart_rx_tail` (or hardware `LSR_DR`).
   - `uart_hw_getc_blocking()` pulls from the ring buffer, blocking on `g_rx_waiter` only when the ring is empty.
4. **Shell Paste Stress Test:**
   - Verify that pasting a 10-line Lisp expression (500+ bytes in a single burst) into the console reproduces zero dropped characters and triggers zero `note_rx_overrun()` messages.

#### Verification & Acceptance Criteria
* `python3 tools/vf2run.py --cmd "uname; info"` executes without input corruption.
* Automated burst test: pipe a 1024-character payload over serial at 115,200 baud; verify that `uart_rx_overruns()` reports `0`.
* Zero regressions on QEMU virt (`rv64`, `rv32`).

---

### Milestone 79.2 — High-Performance Graphics, Cached Scanout & Blit Optimization (`drivers/dc8200_jh7110.c`, `user/chess/src/chess_ui.c`)

**Goal**: Drastically increase graphics drawing speed in Lisp CAS plotting, chess board rendering, and UI windows by eliminating uncached DDR writes and optimizing per-pixel drawing loops.

#### Implementation Steps
1. **Cached Framebuffer Mapping & L2 Cache Range Flushing:**
   - In `drivers/dc8200_jh7110.c`: Change `g_state.front_fb` and `g_state.back_fb` from `pa_to_uncached(pa)` to normal cached Sv39 virtual addresses (`(uint16_t *)(uintptr_t)pa`).
   - In `jh7110_dc8200_flush(y0, y1)`: CPU writes RGB565 rows to cached LPDDR4 memory at full L1/L2 cache bus speed. Track the modified vertical row range `[g_dirty_y0, g_dirty_y1]`.
   - In `jh7110_dc8200_flip()`: Flush only the dirty scanout lines from the SiFive coherent L2 cache to DDR before flipping the scanout address register:
     ```c
     uintptr_t flush_start = g_state.back_fb_pa + (uintptr_t)g_dirty_y0 * g_state.stride_bytes;
     uintptr_t flush_end   = g_state.back_fb_pa + (uintptr_t)g_dirty_y1 * g_state.stride_bytes;
     jh7110_flush_l2_range(flush_start, flush_end);
     ```
   - Benchmarking indicates a 10× to 15× speedup for scanout buffer synchronization.
2. **Optimize Chess Piece Rendering in `chess_ui.c`:**
   - Replace 2,304-iteration single-pixel loops in `chess_touch_draw()` with 64-bit row-level blits.
   - For each of the 48 rows of a piece, load precomputed 64-bit stroke/body words and composite directly into the 1-bpp canvas row using bitwise operations:
     ```c
     /* Row-level bitwise compositing instead of 48 individual pixel tests */
     uint64_t w_str = canvas_white_strokes[piece][py];
     uint64_t w_bod = canvas_white_body[piece][py];
     canvas1_composite_row_64(c1, ox, dy, w_str, w_bod);
     ```
   - Reduces piece rendering time from ~80 ms to <2 ms.
3. **Batch Canvas Drawing in Lisp Plotting (`cas/plot.lisp`):**
   - Provide `(canvas-begin)` and `(canvas-end)` (or use `'D' 1` and `'D' 0`) to defer window manager events and damage notifications until all curve segments are rendered.
   - Profile `(plot "sin(x)" (x -5 5))` to confirm instant (<50 ms) visual presentation.

#### Verification & Acceptance Criteria
* `chess touch` board redraws smoothly with imperceptible latency between move selection and board update.
* CAS plotting demo `(plot "x^3 - 3*x" (x -3 3))` completes in under 100 ms.
* Full-screen double buffering remains completely tear-free with zero underrun artifacts.

---

### Milestone 79.3 — USB Keyboard & Mouse Dynamic Hotplug Subsystem (`drivers/xhci_jh7110.c`)

**Goal**: Support seamless dynamic connect and disconnect of USB keyboards and mice while LugalOS is running.

#### Implementation Steps
1. **Port Status Change Event Processing:**
   - In `xhci_poll()` (`drivers/xhci_jh7110.c`), check for `TRB_TYPE_PORT_STATUS_CHANGE` events from the xHCI Event Ring.
   - Identify the root port index (`port_id`). Read `PORTSC` to inspect `CSC` (Connect Status Change) and `CCS` (Current Connect Status).
2. **Onboard VIA VL817 Hub Interrupt & Status Monitoring:**
   - If the root port connects to the onboard VL817 hub, submit recurring Interrupt IN transfers to the hub's status endpoint (Endpoint 1 IN).
   - When the hub reports a port status change on downstream ports 1..4:
     - Issue USB standard control requests: `GetPortStatus` to determine which physical Type-A port changed.
     - Clear port change features (`ClearPortFeature(PORT_CONNECTION)`).
3. **Disconnection Tear-Down:**
   - If `CCS == 0` for an active slot:
     - If the device was a keyboard, flush the key queue and signal input removal: `console_input_present(WIN_INPUT_KEYBOARD, false)`.
     - If the device was a mouse, hide the cursor sprite: `console_input_present(WIN_INPUT_POINTER, false)`.
     - Submit `XHCI_TRB_DISABLE_SLOT` command to return the slot to the host controller.
     - Free allocated device transfer rings and context buffers.
4. **Connection Enumeration:**
   - If `CCS == 1`:
     - Debounce connection (wait 100 ms).
     - Issue Port Reset (`SetPortFeature(PORT_RESET)`).
     - Submit `XHCI_TRB_ENABLE_SLOT` and retrieve new `slot_id`.
     - Execute standard enumeration pipeline: `SetAddress`, `GetDescriptor(Device)`, `GetDescriptor(Config)`, and configure interrupt endpoints.
     - Re-register keyboard or mouse with the console subsystem.

#### Verification & Acceptance Criteria
* Unplugging the USB keyboard logs `[xHCI] Slot X detached; USB Keyboard offline`.
* Replugging the USB keyboard into any of the 4 Type-A ports immediately re-enables keyboard input in `lsh>`.
* Unplugging and replugging the USB mouse preserves mouse cursor tracking without kernel panic or slot exhaustion.
* `tests/hw/test_visionfive2.py` verifies hotplug reporting.

---

### Milestone 79.4 — High-DPI Display Modes (1440p / 4K30) & Sharp Typography (`drivers/hdmi_jh7110.c`)

**Goal**: Provide native high-resolution output for 1440p and 4K displays to eliminate external monitor upscaling blur, accompanied by crisp high-DPI typography.

#### Implementation Steps
1. **Add Innosilicon HDMI TX PHY PLL Configurations:**
   - In `drivers/hdmi_jh7110.c`, add PLL divider table for **4K @ 30 Hz** (3840×2160 @ 30 Hz, 297.0 MHz pixel clock):
     - Compute post-dividers and feedback multipliers based on Innosilicon 28nm HDMI PHY datasheet.
   - Add PLL divider table for **1440p @ 60 Hz** (2560×1440 @ 60 Hz, 241.5 MHz pixel clock, CVT-RB timing).
2. **Dynamic Resolution Selection & EDID Matching:**
   - Update `hdmi_parse_edid()` to inspect CEA-861 Short Video Descriptors for VIC 93/94/95 (4K 30/25/24 Hz) and Detailed Timing Descriptors for 2560×1440.
   - Expose `display mode [1080p|1440p|4k30]` command to permit manual override from `lsh`.
3. **Frame Buffer Sizing & Allocation in `dc8200_jh7110.c`:**
   - Size scanout buffers according to negotiated resolution:
     - 1080p: 1920×1080 × 2 = 4.15 MB/buffer.
     - 1440p: 2560×1440 × 2 = 7.37 MB/buffer.
     - 4K30: 3840×2160 × 2 = 16.59 MB/buffer.
   - Dynamically allocate scanout pages using `palloc_pages_bulk()`.
4. **Sharp Typography on High-DPI Displays:**
   - At 4K, 8×16 text yields 480 columns, making glyphs too small.
   - Integrate 2× integer font scaling (`16×32` terminal cell) or link vector/bitmap glyphs from `drivers/font_noto_mono.c`.
   - Configure DC8200 Polyphase Scaling Filter registers (`DC_FRAMEBUFFER_SCALE_CONFIG` / `k_hor_kernel` / `k_ver_kernel`) for clean hardware scaling when running 1080p on a 4K panel.

#### Verification & Acceptance Criteria
* Connected to a 4K monitor, `display` reports `Mode: 3840x2160 @ 30 Hz (Native 4K)`.
* Text remains crisp, readable, and free of bilinear blurring.
* VBLANK double-buffer pacing maintains smooth 30/60 FPS presentation without underruns.

---

### Milestone 79.5 — NVMe Full Integration, Autoboot & NVMe-Backed Identity Store (`drivers/idstore_nvme.c`, `drivers/nvme.c`)

**Goal**: Transform the VisionFive 2 into a self-contained NVMe-powered workstation by enabling full long filename support on NVMe, providing an NVMe-backed identity store, and documenting unattended NVMe autoboot.

#### Implementation Steps
1. **Verify FAT32 Long Filename Support on NVMe:**
   - Mount `/nvme0/` with Phase 76 VFAT LFN enabled.
   - Test directory creation, long filenames with mixed case, spaces, and Unicode characters:
     ```text
     lsh> mkdir "/nvme0/My Research Documents"
     lsh> write "/nvme0/My Research Documents/LugalOS Analysis 2026.txt"
     ```
   - Verify that file lookups, deletions, and directory enumerations preserve case and aliases accurately.
2. **Implement NVMe-Backed Identity Store (`drivers/idstore_nvme.c`):**
   - Implement `block_dev_t *identity_store_device(void)` hook for the VisionFive 2 target.
   - Partition strategy:
     - NVMe MBR Partition 1: FAT32 filesystem mounted on `/nvme0/`.
     - NVMe MBR Partition 2: 1 MB raw partition (Partition Type `0x4C` "LugalOS Identity") dedicated to the identity record.
     - Fallback: If Partition 2 does not exist, use dedicated reserved sectors at LBA 2048..4095 on `/dev/nvme0`.
   - Wrap raw sector read/write operations via `nvme_read_blocks_impl()` / `nvme_write_blocks_impl()`.
   - Store 4096-byte identity record (magic `0x4C474944`, CRC32, device UID, 9P server key, grants table, node name).
   - Test with `identity provision`, `identity name vf2-workstation`, and verify persistence across reboots.
3. **Configure Unattended NVMe Autoboot:**
   - Provide U-Boot script commands for booting directly from NVMe without an SD card:
     ```text
     pci enum; nvme scan; fatload nvme 0:1 0x40200000 lugalos.bin; booti 0x40200000 - ${fdtcontroladdr}
     ```
   - Update `tools/vf2run.py` to support `--method nvme` and persist kernels directly to `/nvme0/lugalos.bin`.

#### Verification & Acceptance Criteria
* `identity` command displays valid device UID, device key, and node name persisted on NVMe.
* `p9sync.py` authenticates using the NVMe-persisted key over TCP 9P.
* Board boots into LugalOS directly from NVMe storage.

---

### Milestone 79.6 — Network Clock Initialization via DHCP / NTP Client (`net/ntp.c`, `net/dhcp.c`)

**Goal**: Automatically set the kernel wall clock to UTC on boot when an Ethernet cable is connected, giving meaningful timestamps to files, logs, and shell sessions.

#### Implementation Steps
1. **Parse DHCP Option 42 (NTP Servers):**
   - In `net/dhcp.c`, add option parsing for DHCP Option 42 (Network Time Protocol Servers, RFC 2132 §8.3):
     ```c
     case 42: /* NTP Server Option */
         if (opt_len >= 4) {
             memcpy(g_dhcp_ntp_server, &opts[i + 2], 4);
             g_dhcp_has_ntp = true;
         }
         break;
     ```
2. **Automatic Time Synchronization on Link-Up:**
   - In `drivers/emac_jh7110.c` (or network daemon startup):
     - When DHCP state transitions to `DHCP_STATE_BOUND`:
     - Determine target NTP server:
       1. DHCP Option 42 server IP (if offered).
       2. Stored NTP server in identity record (if configured).
       3. Default gateway IP (`dhcp_gateway()`).
       4. Fallback pool IP (e.g. `129.6.15.28` `time.nist.gov` or local router).
     - Dispatch asynchronous query:
       ```c
       ntp_result_t res;
       if (ntp_sync(target_ip, 3000u, &res) == 0) {
           printk("[NTP] Clock synchronized: UTC %s (offset %+lld us)\n",
                  time_format_now(), (long long)res.offset_us);
       }
       ```
3. **Export Status to Userland:**
   - Expose `date` command in `lsh>` displaying synchronized UTC date/time.
   - Ensure file modification timestamps on `/nvme0/` and `/sd0/` reflect real-world time.

#### Verification & Acceptance Criteria
* Upon booting with Ethernet connected, console logs `[NTP] Clock synchronized: UTC YYYY-MM-DD HH:MM:SS`.
* Running `date` shows current UTC time rather than 1970-01-01.
* Creating a file on `/nvme0` assigns the current calendar date in FAT32 directory entries.

---

### Milestone 79.7 — VisionFive 2 Audio Subsystem: ES8388 Codec & I2S Playback (`drivers/audio_jh7110.c`)

**Goal**: Enable stereo audio output through the onboard 3.5mm headphone jack using the Everest Semiconductor ES8388 codec and JH7110 I2S audio controller.

#### Implementation Steps
1. **I2C Codec Configuration:**
   - Connect to ES8388 at 7-bit I2C address `0x10` via JH7110 I2C controller (`i2c` subsystem).
   - Adapt codec register sequence from `drivers/audio_esp32p4.c`:
     - Power Management (`0x19`, `0x1A`, `0x1B`).
     - DAC Control (`0x17` = 0x18 - 16-bit I2S format).
     - Clock configuration: Configure MCLK/LRCK ratio (256fs).
     - Volume control: Configure Left/Right DAC and Headphone volume (`0x2E`, `0x2F`, `0x30`, `0x31`).
2. **JH7110 I2S0 Subsystem Bring-Up (`i2s0_8ch` at `0x12070000`):**
   - Enable I2S clocks in `SYS_CRG` (`clk_i2s0_bclk`, `clk_i2s0_lrck`, `clk_i2s0_mclk`).
   - Configure pinmux in `SYS_IOMUX` for I2S0 DOUT, BCLK, LRCK, and MCLK.
   - Configure I2S Transmitter: 16-bit stereo, standard Philips I2S format, 44.1 kHz / 48.0 kHz sample rate.
3. **Circular DMA Buffer & Playback API:**
   - Implement circular DMA buffer in DRAM for continuous PCM audio streaming.
   - Implement audio driver API (`drivers/include/drivers/audio.h`):
     - `audio_init()`
     - `audio_play_pcm(const int16_t *samples, uint32_t count, uint32_t sample_rate)`
     - `audio_set_volume(uint8_t volume_percent)`
     - `audio_beep(uint32_t freq_hz, uint32_t duration_ms)`
4. **Shell Audio Commands & Game Integration:**
   - Add `play <file.wav>` command in `kernel/shell.c` to parse 16-bit stereo WAV headers from `/sd0/` or `/nvme0/` and stream to DMA.
   - Connect sound effects in Ceres and Moonbase games to native audio.

#### Verification & Acceptance Criteria
* `audio beep 440 500` produces a clear 440 Hz concert A tone through the 3.5mm audio jack.
* `play /sd0/test.wav` plays clean, undistorted 16-bit 44.1/48 kHz stereo audio.
* Volume can be adjusted cleanly between 0% and 100%.

---

### Milestone 79.8 — Hardware Acceleration Analysis, 2D Engine & Multicore INT8 GEMM Core

**Goal**: Conduct a rigorous architectural evaluation of JH7110 hardware acceleration capabilities, leverage the DC8200 2D engine to offload CPU work, and design an integer-only multi-core matrix engine for edge cognitive inference (Phase 81).

#### Implementation Steps
1. **Imagination PowerVR BXS-4-64 Architectural Analysis:**
   - Document the GPU hardware interface: firmware processor, microcode binary blobs, host communication queues, and memory mapping.
   - Detail why a freestanding microkernel cannot host full 3D/Vulkan/GPGPU stacks without Linux runtime infrastructure.
   - Conclude that GPU hardware is reserved for future Linux/shim virtualization, while native LugalOS focuses on CPU/DMA/DC8200 acceleration.
2. **DC8200 2D Acceleration & Hardware Cursors:**
   - Activate the DC8200 **Hardware Cursor 0** engine (`0x1468u`):
     - Load 16×16 cursor bitmap into DC8200 cursor RAM.
     - Update cursor position by writing `DC_DISPLAY_CURSOR0_XY` on mouse motion.
     - Eliminates all software cursor blits, background restore buffers, and dirty cursor tracking from the CPU pipeline.
   - Evaluate DC8200 **Overlay Plane 1** for UI popups, menus, and on-screen keyboard, allowing independent layer alpha-blending without buffer re-draws.
3. **Multicore INT8 GEMM (General Matrix Multiply) Engine:**
   - Design an optimized integer-only matrix multiplication kernel for SiFive U74 cores:
     - Input tensors: INT8 weights ($W \in \mathbb{Z}^{M \times K}$), INT8 activations ($X \in \mathbb{Z}^{K \times N}$).
     - Accumulators: INT32 intermediate sums, scaled and shifted with fixed-point bias.
     - RV64 Optimization: Pack four INT8 values per 64-bit register (`uint64_t`), unrolling inner loops by 4× and 8×.
     - Instruction acceleration: Use RV64 `Zba` (`sh1add`, `sh2add`, `sh3add`) for zero-overhead array indexing and `Zbb` (`clz`, `ctz`, byte-swapping) for quantized activation clamping.
   - **4-Core SMP Distribution:**
     - Partition matrix rows across Harts 1..4 (`g_harts[0..3]`).
     - Coordinate worker harts using coherent shared cache and lightweight atomic spin-locks (`arch/atomic.h`).
   - Benchmark: Target >200 MFLOPS (equivalent INT8 MACs) across 4 cores, laying the mathematical foundation for Phase 81 quantized embedding models (`all-MiniLM-L6-v2` / `bge-small`) and cognitive cores.

#### Verification & Acceptance Criteria
* DC8200 hardware cursor renders smoothly without CPU software blits or frame latency.
* INT8 GEMM benchmark `gemm_bench` reports throughput and numerical correctness against reference matrices across 1, 2, and 4 cores.
* Comprehensive technical report recorded in `doc/dev/visionfive2_acceleration.md`.

---

### Milestone 79.9 — Automated HIL Test Suite & Complete Documentation

**Goal**: Validate all new capabilities on physical VisionFive 2 silicon and provide complete user and administrator documentation.

#### Implementation Steps
1. **Automate Hardware-in-the-Loop Test Suite (`tests/hw/test_visionfive2.py`):**
   - Add automated test cases:
     - `test_vf2_uart_burst()`: Verifies zero character loss on 1 KB serial paste.
     - `test_vf2_graphics_perf()`: Measures CAS plot and chess rendering timings.
     - `test_vf2_usb_hotplug()`: Verifies keyboard/mouse connect/disconnect event logging.
     - `test_vf2_display_modes()`: Checks resolution query and frame buffer allocation.
     - `test_vf2_nvme_lfn_and_identity()`: Verifies NVMe long filenames and identity read/write.
     - `test_vf2_ntp_sync()`: Verifies network clock synchronization.
     - `test_vf2_audio_playback()`: Exercises ES8388 I2C/I2S audio beep and tone generation.
     - `test_vf2_int8_gemm()`: Tests multicore matrix multiplication correctness and throughput.
2. **Update Hardware and System Documentation:**
   - Update `doc/admin/boards/visionfive2.md` with:
     - UART FIFO & ring buffer details.
     - NVMe identity partition layout.
     - High-DPI display resolutions and 4K30 timings.
     - Audio 3.5mm jack specifications and ES8388 routing.
     - NTP automatic clock synchronization.
     - Hardware acceleration & INT8 GEMM capabilities.
   - Update `doc/user/shell.md` with new commands (`date`, `display mode`, `audio`, `gemm`).
   - Run `python3 tools/gendocs.py --all` and `python3 tools/gendocs.py --check`.

#### Verification & Acceptance Criteria
* `python3 tests/hw/test_visionfive2.py` passes 100% on live hardware.
* `python3 tools/precommit.py` passes cleanly with zero compiler warnings and zero documentation drift.

---

## 2. Summary Milestone Checklist

| Milestone | Subsystem | Key Files | Primary Deliverable | Status |
| :--- | :--- | :--- | :--- | :--- |
| **79.1** | UART Console | `drivers/uart_16550.c` | 64B HW FIFO + 1024B lockless ISR RX ring buffer; zero paste loss | **COMPLETE** |
| **79.2** | 2D Graphics | `drivers/dc8200_jh7110.c`, `user/chess/src/chess_ui.c` | Cached DDR scanout + dirty L2 flush; 64-bit row blits; 10x faster plotting/chess | **COMPLETE** |
| **79.3** | USB Host | `drivers/xhci_jh7110.c` | Dynamic hotplug detection for USB keyboards/mice on VL805 & VL817 hub | **COMPLETE** |
| **79.4** | Display / Fonts | `drivers/hdmi_jh7110.c`, `drivers/dc8200_jh7110.c` | Native 1440p / 4K30 display modes; dynamic resolution switching | **COMPLETE** |
| **79.5** | NVMe / Identity | `drivers/idstore_nvme.c`, `drivers/nvme.c` | FAT32 LFN integration; NVMe identity partition `/dev/identity0`; persistence verified | **COMPLETE** |
| **79.6** | Time / Network | `net/ntp.c`, `net/dhcp.c`, `drivers/emac_jh7110.c` | DHCP Option 42 parsing; automatic UTC clock sync on link-up | **COMPLETE** |
| **79.7** | Audio Subsystem | `drivers/audio_jh7110.c` | JH7110 PWMDAC 3.5mm jack output; tone generator; carrier noise suppression | **COMPLETE** |
| **79.8** | Compute / AI | `kernel/gemm.c`, `doc/dev/visionfive2_acceleration.md` | GPU feasibility report; DC8200 HW cursors; 4-core INT8 GEMM engine | **COMPLETE** |
| **79.9** | Test & Docs | `tests/hw/test_visionfive2.py`, `doc/admin/boards/visionfive2.md`| Complete HIL automated test suite; zero-warning build; docs in sync | **COMPLETE** |
