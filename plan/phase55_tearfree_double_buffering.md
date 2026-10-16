# Phase 55: Tear-Free Display Subsystem — PSRAM Double-Buffering & Multi-Color Depth Pipeline

## Status: IN PROGRESS (Milestones 55.1–55.5 Complete & Hardware-Verified)
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

### Milestone 55.1: Hardware VSYNC & Ping-Pong DMA Setup (ESP32-P4 COMPLETE)
* **`drivers/dsi_esp32p4.c`** [DONE - Commit `34bf02c`]:
  * Configured DW-GDMA channel descriptors with atomic ping-pong flipping in `gdma_isr()` upon `GDMA_EV_DMA_TFR_DONE`.
  * Implemented `dsi_lcd_flip(bool wait_vsync)`: non-blocking flip queuing during normal terminal output, synchronous VSYNC wait for animation steps.
  * Added `dsi_lcd_is_double_buffered()` and CLI controls `lcd db [on|off]`.
  * Verified 60.03 Hz scanout with 0 DMA errors and zero bridge underruns across hundreds of frame flips.
* **`drivers/lcd7_rp2350.c`**:
  * Hook the vertical sync interrupt (line 480 counter) to swap the PIO DMA source pointer.

### Milestone 55.2: Double-Buffered Frame Manager & Dirty Row Synchronization (ESP32-P4 COMPLETE)
* **`drivers/dsi_esp32p4.c`** [DONE - Commit `34bf02c`]:
  * Allocated front and back frame buffers from the PSRAM bulk zone (`palloc_pages_bulk()`).
  * Implemented per-buffer dirty row tracking array (`uint64_t g_dirty[2][DIRTY_WORDS]`): when a row changes, both buffers are marked dirty; each buffer only converts rows marked dirty for itself, keeping both buffers in exact sync without redundant redrawing.
  * Separated `fb_front()` (active DMA scanout surface) and `fb_back()` (active CPU draw surface).
  * Maintained high performance: character conversion in 1894 us (well under 5000 us limit), terminal scrolling at 1897 lines/s.
  * Passed 41/41 hardware-in-the-loop tests in `tests/hw/test_esp32p4.py`.

### Milestone 55.3: Anti-Aliased Typography Pipeline (`drivers/reader_view.c`) [DONE - Commit `7e81425`]
* Updated `tools/gen_serif_font.py` to generate 4-bit / 16-level alpha coverage glyph tables (`font_serif_aa_glyphs[95][16][8]`).
* Implemented fast integer glyph alpha-blending in `font_serif.c` (`font_serif_aa_text_gray8` and `font_serif_aa_text_rgb565` using `((delta * a + 8) >> 4)`).
* Refactored `drivers/reader_view.c` to maintain an 8-bpp anti-aliased surface in PSRAM bulk zone (`aa_buf`) alongside the 1-bpp fallback backing, rendering smooth serif fonts with soft paper background (`#FAFAFA` / 250) and deep charcoal ink (10).
* Implemented visible Reader client area compositing directly into `fb_back()` during scanout conversion in `drivers/dsi_esp32p4.c` (`term_convert()`).
* Upgraded `kernel/screenshot.c` and `console_pixels_format()` to automatically capture hardware scanout buffer in native format:
  * `SCREEN_FORMAT_1BPP` $\rightarrow$ Netpbm PBM (`.pbm`, P4)
  * `SCREEN_FORMAT_GRAY8` $\rightarrow$ Netpbm PGM (`.pgm`, P5, 255 levels)
  * `SCREEN_FORMAT_RGB565` $\rightarrow$ Netpbm PPM (`.ppm`, P6, 24-bit RGB)
* Verified 100% pass on host test suite (506/506) and hardware-in-the-loop suite on ESP32-P4-LCD-7B (43/43). Verified crystal-clear grayscale captures (`shot-004.png`).

### Milestone 55.4: Hybrid Ribbon Compositing & Color Canvases (`drivers/screen.c`) [DONE]
* Extended `screen_canvas_slot_t` with `buf565`, `stride565`, `w565`, `h565`, and `format` (`CANVAS_FMT_1BPP` vs `CANVAS_FMT_RGB565`).
* Implemented dynamic 16-bit RGB565 canvas buffer allocation in PSRAM via `palloc_pages_bulk()` and free on window close (`screen_canvas_free()`).
* Implemented full suite of RGB565 drawing operations in `drivers/screen.c` (conditioned on `!defined(CONFIG_BOARD_RP2350)` preserving RP2350 32 KB PMP budget):
  * Fill (`'F'`), pixel (`'p'`), line (`'l'`), rect fill (`'r'`), rect outline (`'o'`), circle outline (`'c'`), invert (`'i'`), 8x16 font text (`'t'`), colored text (`'x'`), and direct blit (`'B'`).
* Integrated ribbon compositing in `drivers/dsi_esp32p4.c` (`term_convert()`):
  * Auto-promotes master scanout buffer to `FB_RGB565` when any visible color canvas is active on the ribbon.
  * Direct 16-bit row blit compositing visible RGB565 window client areas onto `fb_back()`.
* Extended shell and Lisp primitives:
  * Shell: `canvas open <name> [--rgb565]` (sets layout flag `0x80u`), `canvas list`, `ribbon`.
  * Lisp: `(canvas-rgb565? [name])`, `(rgb r g b)` (exact 16-bit RGB565 packing), `(canvas-open "name" 'split-half :rgb565)`, `(canvas-text x y "text" (rgb ...))`.
* Verified 100% pass on host test suite (`runner.py`: 506/506) and hardware-in-the-loop suite on ESP32-P4-LCD-7B (`test_esp32p4.py`: 44/44, including `test_color_canvas_rgb565`).
* Captured live hardware 24-bit Netpbm PPM screenshot (`SHOT-003.PPM` -> `shot-005.png`) showing retro monochrome terminal window side-by-side with full RGB565 color canvas.

### Milestone 55.5: Re-Enable & Refine Ribbon Slide Animation & Reader Smooth Inertia Scrolling [DONE]
* Re-enabled `CONFIG_ENABLE_RIBBON_ANIMATION`:
  * Default changed to `ON` in `CMakeLists.txt` now that double-buffering is verified.
  * Upgraded `screen_animate_ribbon()` to a 5-step ease-out deceleration curve (`weights = {280, 540, 750, 900, 1000}`), eliminating artificial busy delays when double-buffered.
  * Each animation step synchronizes to hardware VSYNC via `lcdterm_sync_post()` and `dsi_lcd_flip(true)` $\rightarrow$ zero tearing, zero artifacts.
  * In `term_sync_post()`, set `g_t_force = true` during ribbon animation to refresh all 600 scanlines into `fb_back()`, avoiding stale row ghosting.
* Implemented Reader smooth vertical scrolling & touch flick momentum:
  * Paced ease-out scrolling across 3–5 frames in `reader_scroll_instance()`.
  * Touch flick / momentum scrolling in `reader_touch_fling_instance()`: computes initial frame velocity from touch gesture delta and duration ($v = -dy \times 16 / ms$, capped to $\pm 50$ px/frame), decaying with 12% per-frame friction ($v \leftarrow v \times 0.88$) until rest.
  * Break instantly on new touch tap or key press.
  * Integrated GT911 touch swipe handling with `console_touch_scroll(dy, ms)`.
* Verified 100% pass on ESP32-P4 hardware-in-the-loop test suite (`test_esp32p4.py`: 45/45 passed, including `test_ribbon_slide_and_reader_smooth_scroll`).

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

---

## 5. Milestone Results, Hardware Artifacts & Platform Architecture Review

### Summary of Completed Milestones

| Milestone | Status | Key Deliverables | Verification |
| :--- | :--- | :--- | :--- |
| **55.1** | **DONE** | Ping-pong GDMA channel descriptors, atomic VSYNC flip, `dsi_lcd_flip()`, `lcd db on/off` | 60.03 Hz, 0 DMA errors |
| **55.2** | **DONE** | Dual PSRAM buffers (`fb_front` / `fb_back`), per-buffer dirty row tracking bitmap | 1894 µs/char, 1897 lines/s |
| **55.3** | **DONE** | 4-bpp / 16-level serif typography, integer alpha-blend, Netpbm PGM/PPM screenshot export | 43/43 tests, `shot-004.png` |
| **55.4** | **DONE** | Dual format canvas (`CANVAS_FMT_RGB565`), hybrid ribbon compositing, Lisp color primitives | 44/44 tests, `shot-005.png` |
| **55.5** | **DONE** | VSYNC-paced 5-step ease-out deceleration curve, Reader smooth vertical scrolling with touch momentum | 45/45 tests, 100% pass |

---

### Milestone 55.3 Results: Anti-Aliased Typography Pipeline & Scanout Capture

* **Overview**: Implemented a 4-bpp / 16-level grayscale anti-aliased font rendering pipeline for the Reader view, coupled with native multi-format screenshot capture (PBM, PGM, PPM).
* **Typography Engine**:
  * Emits 1-bpp fallback tables in `.lcdtermtext` (`LCDTERM_URODATA`) to obey the RP2350 32 KB U-mode NAPOT PMP limit.
  * Emits 4-bpp alpha coverage tables `font_serif_aa_glyphs[95][16][8]` in standard `.rodata`.
  * Fast integer blending formula: `((delta * a + 8) >> 4)`.
  * Reader maintains an anti-aliased backing buffer (`aa_buf`) in the PSRAM bulk zone (`palloc_pages_bulk()`).
  * Direct scanout compositing: `term_convert()` blits visible Reader client areas directly into `fb_back()` during the 16 ms flush pass.
* **Native Netpbm Screenshot Engine (`kernel/screenshot.c`)**:
  * `SCREEN_FORMAT_1BPP` (0) $\rightarrow$ Netpbm Binary Bitmap (`.pbm`, P4)
  * `SCREEN_FORMAT_GRAY8` (1) $\rightarrow$ Netpbm Binary Grayscale (`.pgm`, P5, 255 levels)
  * `SCREEN_FORMAT_RGB565` (2) $\rightarrow$ Netpbm Binary Pixmap (`.ppm`, P6, 24-bit RGB)
* **Visual Artifacts**:
  * Baseline 1-bpp bilevel typography: `doc/assets/screenshots/reader_1bpp_baseline.png`
  * Upgraded anti-aliased typography (captured from live hardware via `screenshot`):
    ![Anti-Aliased Typography in Reader](../doc/assets/screenshots/reader_antialiased_serif.png)
  * *Figure 55.3: Live 8-bpp PGM capture from ESP32-P4-LCD-7B showing smooth serif curves, soft cream paper background (#FAFAFA), and dark charcoal ink (#0A0A0A).*

---

### Milestone 55.4 Results: Hybrid Ribbon Compositing & RGB565 Color Canvases

* **Overview**: Enables high-performance 16-bit RGB565 color graphics windows (games, data visualizers, photo view) to live seamlessly side-by-side with retro 1-bpp monochrome terminal shells on the infinite workspace ribbon.
* **Canvas Format & Dynamic Allocation**:
  * Added `CANVAS_FMT_RGB565` (1) alongside `CANVAS_FMT_1BPP` (0) in `screen_canvas_slot_t`.
  * Backing buffers allocated dynamically in the PSRAM bulk zone (`palloc_pages_bulk()`) and freed on window close.
* **16-Bit Graphics Primitives (`drivers/screen.c`)**:
  * Implemented fast fill (`'F'`), pixel (`'p'`), Bresenham line (`'l'`), outline rect (`'o'`), filled rect (`'r'`), circle (`'c'`), invert (`'i'`), 8x16 font text (`'t'`), colored text (`'x'`), and direct bitmap blit (`'B'`).
  * Guarded by `CANVAS_SUPPORT_RGB565` (`!defined(CONFIG_BOARD_RP2350)`), preserving the 32 KB U-mode PMP budget on RP2350.
* **Hybrid Auto-Promotion Compositor (`drivers/dsi_esp32p4.c`)**:
  * Automatically elevates the hardware scanout frame buffer to `FB_RGB565` whenever a color canvas is visible on the glass.
  * Direct row blit compositing into `fb_back()` during the 16 ms flush pass.
* **Shell & Lisp Integration**:
  * Shell: `canvas open <name> [--rgb565]`, `canvas list`, `ribbon`.
  * Lisp: `(canvas-rgb565? [name])`, `(rgb r g b)` (exact 16-bit 5-6-5 packing), `(canvas-open "name" 'split-half :rgb565)`, `(canvas-text x y "text" (rgb ...))`.
* **Visual Artifacts**:
  * Live hardware 24-bit Netpbm PPM capture:
    ![Hybrid Ribbon Compositing: Retro Terminal and Color Canvas](../doc/assets/screenshots/ribbon_hybrid_color_canvas.png)
  * *Figure 55.4: Live 24-bit capture (`SHOT-003.PPM`, 1.84 MB) from ESP32-P4-LCD-7B. Left: Standard 1-bpp interactive terminal shell (`lsh`). Right: Active 16-bit RGB565 color canvas (`colortest`) rendered with Lisp primitives at 60 Hz without tearing.*

---

### Milestone 55.5 Results: Silky-Smooth Ribbon Animation & Reader Smooth Inertia Scrolling

* **Overview**: Re-enabled `CONFIG_ENABLE_RIBBON_ANIMATION` by default, locked transitions to hardware VSYNC, and introduced physics-modeled smooth vertical scrolling with touch momentum in the Reader view.
* **Hardware VSYNC Pacing**:
  * Upgraded `screen_animate_ribbon()` from 4-step linear interpolation to a 5-step ease-out deceleration curve (`weights = { 280, 540, 750, 900, 1000 }` out of 1000).
  * Removed redundant busy delays: each animation step synchronizes to hardware VSYNC via `lcdterm_sync_post()` and `dsi_lcd_flip(true)` (~83.3 ms total transition at 60 FPS).
  * Set `g_t_force = true` during ribbon animation in `dsi_esp32p4.c`, ensuring all 600 scanlines are refreshed into `fb_back()` and eliminating scanline ghosting.
* **Reader Smooth Inertia Scrolling**:
  * Ease-out vertical scrolling over 3–5 frames in `reader_scroll_instance()`, checking `console_has_char()` to break immediately on new key events.
  * Touch flick momentum in `reader_touch_fling_instance()`: computes initial frame velocity from touch gesture delta and duration ($v_0 = -dy \times 16 / ms$, clamped to $\pm 50$ px/frame), decaying with 12% per-frame friction ($v_{t+1} = v_t \times 0.88$) until rest.
  * Instant halt on touch tap or keystroke.
  * GT911 vertical swipe detection dispatched via `console_touch_scroll(dy, ms)`.
* **Hardware-in-the-Loop Verification**:
  * Executed `tests/hw/test_esp32p4.py --board lcd7b --no-load` on physical hardware:
  * **Result: 45 / 45 PASSED (100%)**.

---

### Platform Architecture Review: RP2350 Terminal vs. ESP32-P4

#### 1. RP2350 PSRAM & Bulk Page Allocator Status
The RP2350 terminal already features an active, hardware-verified PSRAM bulk page allocator in LugalOS:
* **Hardware**: APS6404-class 8 MB QSPI PSRAM on GP0 (`XIP_CS1`).
* **Kernel Allocator**: Initialized at boot via `psram_init()` and `palloc_init_bulk()` in `kernel/main.c`.
* **Current Allocations in PSRAM on RP2350**:
  * Lisp node pool: 65,536 cons nodes (**1 MB**) in `BULK_BSS`.
  * Lisp string pool: 3,072 strings (**144 KB**).
  * RAM disk (`/ram0`): 2 MB to 4 MB dynamically allocated from the PSRAM bulk zone.
  * Chess transposition table: **512 KB** in PSRAM.
  * Text editor line index: in PSRAM.

#### 2. Display Bus Architecture & Bandwidth Analysis

Why does the ESP32-P4 scan out a full 16-bpp RGB565 frame buffer from PSRAM while the RP2350 terminal scans out 1-bpp from SRAM?

| Dimension | RP2350-LCD-7 | ESP32-P4-LCD-7B |
| :--- | :--- | :--- |
| **Panel Interface** | 800×480 Raw RGB TTL (`ST7262`) | 1024×600 MIPI-DSI (`EK79007`) |
| **Panel Memory** | None (continuous raster scanout) | None (DSI bridge with internal FIFO) |
| **PSRAM Bus** | 72 MHz Quad-SPI (**shared with Flash XIP**) | 200 MHz Hex/Octal DDR (**dedicated MSPI bus**) |
| **PSRAM Read Bandwidth** | **~25–28 MB/s** sequential read | **400–800 MB/s** |
| **Bus Contention** | Flash instruction fetches collide with PSRAM on QMI | Completely decoupled from Flash |
| **Scanout Bandwidth Needed** | **46.08 MB/s** (for 800×480 RGB565 @ 60 Hz) | **122.8 MB/s** (for 1024×600 RGB565 @ 60 Hz) |
| **Live Scanout Feasibility** | **Physically impossible** ($46 > 25\text{ MB/s}$) | **Effortless** ($122 \ll 400\text{ MB/s}$, ~15% bus) |

* **The RP2350 Bandwidth Deficit**:
  * An 800×480 screen at 16-bpp (RGB565) requires $800 \times 480 \times 2 = 768\text{ KB}$ per frame.
  * At 60 Hz: $768\text{ KB} \times 60 = \mathbf{46.08\text{ MB/s}}$ continuous scanout bandwidth.
  * Peak bandwidth during active scanlines (24 MHz pixel clock) is **48 MB/s**.
  * The RP2350's QSPI PSRAM at 72 MHz SCK maxes out at **~25–28 MB/s**. Direct DMA scanout from PSRAM is physically impossible and triggers immediate FIFO underflow.

#### 3. How the Waveshare Demo Code "Did It"
Inspection of the vendor demo repository (`RP2350-Touch-LCD-7-Demo/examples/lcd/lcd_flush_rgb/`):
* **Overclocking**: Overclocks RP2350 from 150 MHz to **240 MHz**.
* **Huge SRAM Bounce Buffers**: Allocates **two 128 KB transfer buffers in SRAM (256 KB total)** — consuming half of the chip's total 520 KB SRAM.
* **Manual CPU Copy in ISR**: Slices the display into 6 horizontal strips. In `dma_complete_handler` (firing 360 times/second), the CPU runs a manual `for` loop copying 128 KB from PSRAM to SRAM.
* **Impact**: Slices burn ~100% of CPU 0, lock the bus against Flash instruction fetches, and prevent multitasking, USB host keyboards, and an OS.

#### 4. Features from Phase 55 Applicable to RP2350
1. **Double-Buffering in SRAM**:
   * Two 800×480 1-bpp buffers require $48\text{ KB} \times 2 = \mathbf{96\text{ KB}}$ of SRAM.
   * Readily fits within the 520 KB SRAM pool, enabling tear-free ribbon sliding on RP2350.
2. **Offscreen RGB565 Canvases in PSRAM**:
   * Offscreen canvases (e.g. 320×240 RGB565 = 153 KB) can be dynamically allocated in the PSRAM bulk page zone and dithered/quantized into the 1-bpp/2-bpp SRAM scanout buffer during composition.
3. **Anti-Aliased Typography / Glyph Caching in PSRAM**:
   * Glyph tables and rendered text layouts can be cached in the PSRAM bulk zone.

