# Phase 55: Tear-Free Display Subsystem — PSRAM Double-Buffering & Multi-Color Depth Pipeline

## Status: PLANNED
**Goal**: Eliminate display tearing, flicker, and color-fringing artifacts across all LCD/DSI targets (ESP32-P4 and RP2350) by implementing hardware-synchronized **ping-pong double-buffering** in PSRAM. Decouple rendering from scanout via VSYNC-timed buffer flipping, re-enable silky smooth 60 FPS ribbon animations, and establish a clean foundation for higher color depths (GRAY8, RGB565, and RGB888).

---

## 1. Context & Problem Analysis

LugalOS currently drives two high-resolution graphical panels:
1. **ESP32-P4-WIFI6-Touch-LCD-7B** (Phase 47): 7" 1024×600 MIPI-DSI panel driven at 60 Hz by the Synopsys DSI host and DW-GDMA controller.
2. **Waveshare RP2350-LCD-7** (Phase 36): 7" 800×480 RGB panel driven by RP2350 PIO state machines and DMA.

### The Root Cause of Tearing & Color Artifacts
* **Single-Buffered Scanout**:
  * In `drivers/dsi_esp32p4.c`, the DW-GDMA continuously scans out a single frame buffer from PSRAM at 60 Hz (refresh period = 16.6 ms).
  * In `drivers/lcd7_rp2350.c`, PIO DMA continuously scans out a single 1-bpp frame buffer from SRAM.
* **Scanline Collision**:
  * When the CPU updates the display (such as during multi-step window slide animations, fast terminal scrolling, or full-window repaints), it writes directly into the buffer that the hardware DMA is actively reading.
  * Whenever the DMA scanline pointer overtakes or lags behind the CPU write pointer, horizontal tearing occurs.
* **Color Fringing / Blueish Artifacts on P4**:
  * The ESP32-P4 terminal translates 1-bpp character cells and canvas bitmaps into GRAY8 or RGB565 in PSRAM.
  * In RGB565 (16-bit) or multi-byte pixel formats, a pixel write consists of multiple bytes. When the DMA controller reads memory in the middle of a multi-byte write or across un-flushed cache line boundaries, partial color bytes are transmitted over DSI lanes. This results in the characteristic blueish/cyan color flashes reported during animation.
* **Why Third-Party Frameworks (e.g. LVGL) Do Not Solve This**:
  * LVGL running on a single frame buffer exhibits the exact same tearing and color artifacts.
  * LVGL eliminates tearing only when configured with two buffers and a hardware VSYNC swap (`disp_drv.full_refresh = 1`).
  * Adopting LVGL would introduce significant binary bloat (~200 KB), heavy dynamic heap usage, and libc impedance with LugalOS's `-nostdlib -ffreestanding` microkernel architecture, while breaking the ultra-fast 1-bpp terminal grid model.
  * Solving tearing requires a **buffering architecture fix**, which is cleaner, smaller, and faster to implement natively.

---

## 2. Architecture & Design Principles

```
CPU / Rendering Engine (lcdterm, screen.c)
                │
                ▼ (draws into Back Buffer)
       +──────────────────+
       │   Back Buffer    │  (Hidden, non-tearing canvas)
       +──────────────────+
                │
                │ VSYNC Event (Vertical Blanking Interrupt)
                ▼ (Atomic pointer swap / DMA descriptor flip)
       +──────────────────+
       │   Front Buffer   │  (Active scanout)
       +──────────────────+
                │
                ▼
       DW-GDMA / PIO Scanout ──► MIPI-DSI / RGB Panel (60 Hz)
```

### 1. Ping-Pong Buffers in PSRAM
* **ESP32-P4 (32 MB PSRAM)**:
  * Ample memory headroom:
    * 1024×600 GRAY8 (8-bpp): 600 KB per buffer $\times$ 2 = **1.2 MB** (3.7% of 32 MB PSRAM).
    * 1024×600 RGB565 (16-bpp): 1.2 MB per buffer $\times$ 2 = **2.4 MB** (7.5% of 32 MB PSRAM).
    * 1024×600 RGB888 (24-bpp): 1.8 MB per buffer $\times$ 2 = **3.6 MB** (11.2% of 32 MB PSRAM).
* **RP2350-LCD-7 (8 MB PSRAM, 512 KB SRAM)**:
  * 800×480 1-bpp: 48 KB per buffer $\times$ 2 = **96 KB** (fits cleanly in SRAM, or moved to PSRAM).
  * 800×480 RGB565 (16-bpp): 768 KB per buffer $\times$ 2 = **1.5 MB** in PSRAM.

### 2. VSYNC-Synchronized Hardware Flip
* The hardware DMA engine is configured with ping-pong descriptors.
* On vertical blanking (VSYNC interrupt):
  * If a new rendered frame is ready (`flip_pending == true`), the DMA scanout address is flipped to the back buffer during vertical blanking.
  * The previous front buffer becomes the new back buffer for subsequent rendering passes.
  * CPU cache lines are flushed prior to arming the flip (`dsi_lcd_fb_flush()`).
* Zero CPU busy-waiting: rendering tasks queue a flip and yield or proceed with next computations.

### 3. Impact on Future Multi-Color Depth Modes
Double-buffering is not only compatible with higher color depths—**it is a mandatory prerequisite**:
* **Monochrome (1-bpp)**: Tearing manifests as subtle line shifts.
* **Grayscale (GRAY8, 256 levels)**: Tearing manifests as gray band mismatch; double-buffering enables smooth subpixel anti-aliasing and shading.
* **High Color (RGB565, 65,536 colors)**: Without double-buffering, writing a 16-bit pixel takes 2 byte writes; scanout during a write reads corrupt RGB bitfields (e.g. green bits from new pixel, red/blue from old), producing vivid magenta/cyan flashes. Double-buffering guarantees 100% color integrity.
* **True Color (RGB888, 16.7M colors)**: 3-byte writes are even more susceptible to byte-tearing. Double-buffering is required for high-fidelity photo rendering in the Gemini reader and graphical Lisp demos.

---

## 3. Milestones & Implementation Tasks

### Milestone 55.1: Hardware VSYNC & Ping-Pong DMA Setup
* **`drivers/dsi_esp32p4.c`**:
  * Configure DW-GDMA channel descriptors for circular ping-pong operation or dynamic next-descriptor updates.
  * Hook the DSI/GDMA frame-complete interrupt (CLIC 18) to detect vertical blanking.
  * Implement `dsi_lcd_flip(uint8_t buffer_idx)` to atomically switch the active DMA read address during VSYNC.
* **`drivers/lcd7_rp2350.c`**:
  * Hook the vertical sync interrupt (line 480 counter) to swap the PIO DMA source pointer.

### Milestone 55.2: Double-Buffered Frame Manager in `drivers/lcdterm.c`
* Allocate front and back frame buffers from the PSRAM bulk zone (`palloc_bulk_alloc()`).
* Expose `lcdterm_swap_buffers()` to the drawing engine.
* Maintain clean separation between the working draw surface and the scanout surface.

### Milestone 55.3: Decoupled Render & Flush Pipeline
* Update `lcdflush` task:
  * Only blit changed/dirty rows into the back buffer.
  * Flush L2/PSRAM cache for changed rows only (`dsi_lcd_fb_flush()`).
  * Signal `flip_pending` to the VSYNC interrupt handler.

### Milestone 55.4: Re-Enable & Refine Ribbon Slide Animation
* Re-enable `CONFIG_ENABLE_RIBBON_ANIMATION`:
  * Set default to `ON` once double-buffering is verified.
  * Perform multi-step interpolations (e.g. 4–6 steps at 60 Hz = 66–100 ms total transition).
  * Each animation step renders to back buffer $\rightarrow$ flips at VSYNC $\rightarrow$ zero tearing, zero blue flashes.

### Milestone 55.5: Multi-Color Depth Pipeline Abstraction
* Extend `lcdterm_backend_t` to support selectable color depths:
  * Mode 0: `1-bpp` native (minimal bandwidth, retro Mac aesthetic).
  * Mode 1: `GRAY8` (anti-aliased fonts, smooth window shadows).
  * Mode 2: `RGB565` (ANSI 256 colors, full-color Gemini images, syntax highlighting).
* Shell command `lcd bpp [1|8|16]` to switch pixel formats dynamically.

### Milestone 55.6: Verification & Hardware Testing
* **Benchmark & Frame-Pacing**:
  * Measure frame flip latency and verify exact 60 Hz pacing without dropped frames.
* **Visual Inspection on ESP32-P4-LCD-7B**:
  * Rapid window cycling (`Super+Left`/`Right`, `Super+Ctrl+Left`/`Right`) to verify zero tearing or color artifacts.
* **Visual Inspection on RP2350-LCD-7**:
  * Verify tear-free slide animation and clean title-bar rendering.
* **Automated Hardware Suite**:
  * Add double-buffering integrity test to `tests/hw/test_esp32p4.py` and `tests/hw/test_rp2350.py`.

---

## 4. Verification Checkpoints

1. **Zero Tearing**: High-contrast vertical bars moving across the screen at varying speeds show no horizontal line breaks or tearing artifacts under high-speed video capture.
2. **Zero Color Fringing**: Rapid window animation in RGB565 mode shows crisp borders and zero blue/cyan flashes during transitions.
3. **Paced 60 FPS**: Animations run locked to the panel's 60 Hz VSYNC clock without skipping or micro-stutters.
4. **Memory Headroom Preserved**: PSRAM utilization stays well under 10% of total capacity, leaving $>28\text{ MB}$ free for user programs and Lisp heaps.
