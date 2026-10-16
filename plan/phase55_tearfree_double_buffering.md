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

### 3. Key Use-Cases & Architectural Solutions

#### Use-Case 1: Grayscale Anti-Aliased Typography (E-Reader Print Quality)
* **Design Goal**: Keep the clean, distraction-free black & white / paper aesthetic, but dramatically improve readability and typographic beauty in the **Reader** application.
* **Mechanism**:
  * Tools like `tools/gen_serif_font.py` generate anti-aliased glyph bitmaps with 4-bit (16 levels) or 8-bit (256 levels) coverage masks instead of 1-bpp bilevel thresholds.
  * In the master RGB565 frame buffer, grayscale is represented by equal $R, G, B$ intensities (or custom paper tinting, e.g. subtle warm cream `#FDFBF7` background and crisp dark slate text).
  * The Reader blits glyphs with fast integer alpha-blending:
    $$\text{pixel} = \text{bg} \times (1 - \alpha) + \text{text\_color} \times \alpha$$
  * Result: Serif fonts (such as Noto Serif and Liberation Serif) render with silky-smooth curves, perfect contrast, and zero jaggies, matching high-end e-readers.

#### Use-Case 2: Hybrid Ribbon Compositing (Per-Window Color Depth & Color Canvases)
* **Design Goal**: Allow a single window (e.g., a color game, a photo viewer, or a scientific plot) to render in full 16-bit RGB565 color, while neighboring terminal and reader windows preserve their crisp monochrome or grayscale styling.
* **Mechanism**:
  * **Master Scanout Buffer**: Set to RGB565 (16-bpp).
  * **Per-Window Format Metadata**:
    * `RIBBON_WIN_TERM`: 1-bpp character cell grid $\rightarrow$ compositor expands fg/bg colors into RGB565.
    * `RIBBON_WIN_READER`: 4-bpp/8-bpp anti-aliased text surface $\rightarrow$ compositor alpha-blends onto paper background.
    * `RIBBON_WIN_CANVAS`: Can be allocated as **1-bpp** (monochrome plot/chess, 48 KB) OR **RGB565** (game/graphics canvas, e.g. 400×300 RGB565 = 240 KB in PSRAM).
  * **Compositor Blit (`screen.c`)**:
    * When composing the visible ribbon segment, `draw_all_from()` blits each visible window using its native format:
      * 1-bpp canvas/term $\rightarrow$ 1-to-16 bit expansion with palette lookup.
      * RGB565 color canvas $\rightarrow$ direct 16-bit `memcpy` / DMA row copy into the back buffer.
  * **CLI / Lisp Control**:
    * Shell: `canvas open <name> [cols] [--rgb565]`
    * Lisp: `(canvas-open "game" :format 'rgb565 :width 400 :height 300)`
  * Allows a high-speed color game canvas to live right beside a retro monochrome shell on the same ribbon!

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

### Milestone 55.3: Anti-Aliased Typography Pipeline (`drivers/reader_view.c`)
* Update `tools/gen_serif_font.py` to generate 4-bit / 8-bit alpha coverage glyph tables.
* Implement fast integer glyph alpha-blending in `reader_view.c`.
* Render smooth serif fonts on cream/white backgrounds with zero color distractions.

### Milestone 55.4: Hybrid Ribbon Compositing & Color Canvases (`drivers/screen.c`)
* Extend `screen_canvas_slot_t` with format descriptor: `CANVAS_FMT_1BPP` vs `CANVAS_FMT_RGB565`.
* Allocate RGB565 canvas backings dynamically from PSRAM.
* Update `screen.c` window blitter to blit 1-bpp, anti-aliased grayscale, and direct RGB565 side-by-side into the master RGB565 frame.
* Add shell commands: `canvas open <name> [cols] [--rgb565]` and Lisp primitive bindings.

### Milestone 55.5: Re-Enable & Refine Ribbon Slide Animation
* Re-enable `CONFIG_ENABLE_RIBBON_ANIMATION`:
  * Set default to `ON` once double-buffering is verified.
  * Perform multi-step interpolations (e.g. 4–6 steps at 60 Hz = 66–100 ms total transition).
  * Each animation step renders to back buffer $\rightarrow$ flips at VSYNC $\rightarrow$ zero tearing, zero blue flashes.

### Milestone 55.6: Verification & Hardware Testing
* **Benchmark & Frame-Pacing**:
  * Measure frame flip latency and verify exact 60 Hz pacing without dropped frames.
* **Visual Inspection on ESP32-P4-LCD-7B**:
  * Verify anti-aliased serif text in `reader`.
  * Open simultaneous B/W Terminal, Anti-aliased Reader, and RGB565 Color Game canvas on the ribbon.
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
