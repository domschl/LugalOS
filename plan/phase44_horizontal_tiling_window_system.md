# Phase 44 — Horizontal Scrollable Tiling Window System (Niri for LugalOS)

**Status: Complete (2026-10-03).**  
**Target Hardware:** Waveshare RP2350-LCD-7 (800 × 480 1-bpp monochrome panel, 8 MB QSPI PSRAM, USB keyboard).

---

## 0. Context & Architectural Rationale

### Where this comes from
In Phase 37 (`plan/phase37_screen_layouts_and_apps.md`), LugalOS introduced constrained screen tiling: a fixed menu bar, desktop background, and at most two framed tiles (a text terminal and an optional graphics canvas) in fixed ratios (`TEXT`, `CANVAS`, `SPLIT_WIDE`, `SPLIT_HALF`, `SPLIT_NARROW`, and `swapped`). Phase 37 deliberately deferred a general window system because SRAM was constrained to 520 KB and overlapping windows would require complex 2D clipping regions and heavy backing stores.

In Phase 38 (`plan/phase38_psram.md`), the RP2350-terminal persona gained **8 MB of QSPI PSRAM** with `BULK_BSS` and `palloc_pages_bulk()`, providing abundant memory for backing stores.

### The Decision: A 1D Horizontal Ribbon (Inspired by Niri)
Traditional overlapping/floating window managers (e.g. Mac OS, Windows) are ill-suited for an 800 × 480 monochrome workstation:
1. **Vertical Scarcity:** 480 pixels provide exactly 27 text rows (at 16 px/row). Vertical window stacking produces cramped, unusable viewports.
2. **Clipping & Occlusion Overhead:** Overlapping windows require recursive rectangular clipping, dirty-region tracking, and z-order management—heavy mechanisms for a freestanding microcontroller.
3. **Complex Pointer Navigation:** On an embedded terminal workstation without a mandatory mouse, moving and resizing floating windows by keyboard is notoriously cumbersome.

**The Solution: A Scrollable 1D Horizontal Ribbon (Scrollable Tiling).**
Inspired by modern scrollable-tiling compositors like **Niri**, windows are arranged in a single horizontal ribbon:
```
  [ Window 0 ]   [ Window 1 ]   [ Window 2 ]   ...   [ Window N ]
  (Terminal 0)     (Canvas)     (Terminal 1)           (Editor)
  ---------------------------------------------------------------
             <====== [ Visible Viewport (800 px) ] ======>
```
* **No Overlapping:** Windows never occlude each other; no 2D clipping or z-order math is needed.
* **Full Vertical Space Preserved:** Every window keeps the full 27-row height.
* **100% Deterministic Keyboard Navigation:** Navigation is purely 1-dimensional (`Left` / `Right`).
* **Dynamic Ribbon:** The physical 800 × 480 panel acts as a camera/viewport looking into the horizontal ribbon, scrolling smoothly or stepping as the user changes focus.

---

## 1. System Architecture

### 1.1 Window Types & Per-Window Backing Store
A window on the ribbon is represented by a `ribbon_win_t` descriptor:
* **Text / Terminal Window:**
  - Backed by an independent `vtterm_t` state machine.
  - Character cell shadow buffer: `cols × rows × sizeof(uint16_t)` (e.g. 48 × 27 × 2 = 2.6 KB; 98 × 27 × 2 = 5.3 KB).
  - Dedicated input FIFO (`rx_fifo`, 128 bytes).
  - Associated task PID (e.g. an independent `lsh` shell worker).
* **Canvas / Graphics Window:**
  - Backed by a `canvas1_t` 1-bpp bitmap buffer allocated in PSRAM via `palloc_pages_bulk()`.
  - Tile width matching current preset (e.g. 389 px, 469 px, or full 789 px).
  - Used by CAS mathematical plotting (`cas/plot.lisp`), demos (`ca.lisp`, `lorenz.lisp`), and the chess board (`chess_ui.c`).

### 1.2 Memory Budget (Zero SRAM Overhead)
* All ribbon descriptors, virtual terminal shadows, and canvas bitmap stores reside in **PSRAM** (`BULK_BSS` / `palloc_pages_bulk()`).
* Sponsoring 8 to 16 active ribbon windows consumes **< 500 KB** total (< 6% of the 8 MB PSRAM).
* Headless and non-terminal presets (`rp2350`, `rp2350-clock`, `rp2350-gateway`, `esp32p4`, `rv32`, `rv64`) incur **0 bytes of SRAM change** and compile without the multi-window manager.

### 1.3 Concurrency & Multi-Terminal Subsystem
* Spawning a new terminal (`Cmd + Enter`) creates a new `lsh` worker task via `task_create()`.
* **Input Routing:** Hardware keystrokes from `drivers/usbkbd.c` are routed strictly to the **focused window's** `rx_fifo`.
* **Output Isolation:** Background tasks continue to write to their respective `vterm` shadows in memory. When the viewport scrolls back to an updated window, its display is perfectly fresh.
* **Task Lifecycles:** When a terminal shell exits (via `exit` or `Cmd + W`), its window is removed from the ribbon, its backing memory is freed, and adjacent windows close the gap.

### 1.4 Viewport & Compositing
* The physical panel is driven by PIO2 and DMA scanning the primary 48 KB 1-bpp framebuffer.
* The viewport position $X_{\text{view}}$ specifies the horizontal pixel offset in ribbon space.
* **Blit Engine:** A fast 1-bpp scanline blitter renders the portion of the ribbon falling inside $[X_{\text{view}}, X_{\text{view}} + 800]$ into the framebuffer:
  - Repainting visible character cells takes < 1.5 ms.
  - Restoring a canvas bitmap from PSRAM takes < 400 µs.
* **Smooth Scrolling (Animation):** When focus changes to an off-screen window, the viewport slides over 6–8 frames (~100 ms total at 56 Hz), giving immediate physical feedback of ribbon movement.

---

## 2. Keyboard Interaction Model

All window management shortcuts use the **`Cmd` / `Super`** modifier (decoded in `drivers/usbkbd.c` and `kernel/keyseq.c`):

| Hotkey | Action | Description |
| :--- | :--- | :--- |
| **`Cmd + Enter`** | **New Terminal** | Spawns a new `lsh` shell window immediately to the right of the focused window. |
| **`Cmd + Left`** | **Focus Left** | Shifts keyboard focus to the left neighbor; scrolls viewport if target is off-screen. |
| **`Cmd + Right`** | **Focus Right** | Shifts keyboard focus to the right neighbor; scrolls viewport if target is off-screen. |
| **`Cmd + Ctrl + Left`** | **Move Window Left** | Reorders the active window one position to the left in the horizontal ribbon. |
| **`Cmd + Ctrl + Right`** | **Move Window Right** | Reorders the active window one position to the right in the horizontal ribbon. |
| **`Cmd + [`** | **Shrink Column** | Cycles width preset narrower (98 cols $\rightarrow$ 64 cols $\rightarrow$ 48 cols $\rightarrow$ 38 cols). |
| **`Cmd + ]`** | **Expand Column** | Cycles width preset wider (38 cols $\rightarrow$ 48 cols $\rightarrow$ 64 cols $\rightarrow$ 98 cols). |
| **`Cmd + \`** | **Quick Swap** | Swaps active window with the adjacent visible window. |
| **`Cmd + 1` .. `9`** | **Direct Focus Window** | Jumps focus directly to window $N$ on the ribbon; scrolls viewport into view. |
| **`Cmd + W`** | **Close Window** | Closes active window and terminates its task (cannot close the last root shell). |
| **`Cmd + Shift + 3`** | **Screenshot** | Captures current visible viewport to `/sd0/` as PNG (existing feature preserved). |

### 2.1 Menu Bar Ribbon Indicator
In the top menu bar, right of the system title (`LugalOS`) and left of the indicators, an icon stripe represents the ribbon:
- Each window in the ribbon is rendered as a miniature block.
- Block width reflects proportional column thickness (e.g. 38, 48, 64, or 98 cols).
- The active/focused window is filled solid black; inactive windows are rendered in outline (1-px border).
- A viewport bracket or highlight indicates which windows are currently visible on the physical screen.

---

## 3. Milestones & Implementation Status

### Milestone 44.1: Virtual Console & Multi-Terminal Subsystem [COMPLETE]
* **Implementation:**
  - `vterm_t` abstraction in `kernel/include/kernel/vterm.h` and `kernel/vterm.c` with per-vterm lock, shadow memory, and independent task isolation.
  - Shell worker tasks dynamically attached via `shell_spawn_terminal()`.
  - Fixed lock-order inversion between `g_vterm_mgr_lock` and `console_lock` by moving `console_set_title()` outside `g_vterm_mgr_lock`.
* **Verification:** `vtselftest` (36/36 assertions passed on RV64 and RV32) and multi-terminal host tests.

---

### Milestone 44.2: Ribbon Data Structures & Geometry Manager [COMPLETE]
* **Implementation:**
  - 1D horizontal window ribbon data structure `ribbon_t` and `ribbon_win_t` in `drivers/ribbon.c` and `drivers/include/drivers/ribbon.h`.
  - Column widths, gap spacing, coordinate offsets, and viewport computations.
* **Verification:** `ribbonselftest` (22 assertions passed).

---

### Milestone 44.3: Keyboard Navigation & Hotkey Routing [COMPLETE]
* **Implementation:**
  - Intercepted `Cmd + Enter`, `Cmd + Left/Right`, `Cmd + Ctrl + Left/Right`, `Cmd + W`, and `Cmd + 1..9` in `drivers/usbkbd.c`.
  - Dispatched to `console_canvas` commands (`'N'`, `'F'`, `'M'`, `'C'`, `'G'`) in `kernel/console.c`.
* **Verification:** `usbkbdselftest` verified in automated test suite.

---

### Milestone 44.4: Viewport Compositing, Indicator & Smooth Sliding Animation [COMPLETE]
* **Implementation:**
  - Fast scanline blitter and smooth scrolling step in `drivers/ribbon.c` and `drivers/screen.c`.
  - Ribbon stripe indicator drawn in top menu bar (`draw_ribbon_bar()`).
* **Verification:** Validated on QEMU and real silicon with smooth sliding animation.

---

### Milestone 44.5: Canvas & Application Integration [COMPLETE]
* **Implementation:**
  - Unified canvas and terminal windows into `scr->ribbon`.
  - Fixed canvas blanking on repaint, layout locking/unlocking in `chess_ui.c`, and width clamping.
  - Seamless coexistence with CAS plotting (`(plot '(sin x) ...)`), Lisp canvas graphics, and graphical chess.
* **Verification:** Automated tests in `tests/test_ribbon.py` and silicon verification on Waveshare RP2350-LCD-7.

---

### Milestone 44.6: Hardware Verification & Zero Regressions [COMPLETE]
* **Implementation:**
  - Flashed on physical Waveshare RP2350-LCD-7 workstation.
  - Full automated regression test suite: 467/467 tests passed in 291.60s.
  - 0 SRAM bytes overhead on non-terminal targets.

---

## 4. Risks & Mitigations

| Risk | Impact | Mitigation |
| :--- | :---: | :--- |
| **Scheduler Task Table Overflow** | Med | `CONFIG_SCHED_MAX_TASKS` is currently 24. With ~8 driver tasks, spawning 4–6 shells is well within bounds. `vterm_create()` gracefully refuses new terminals if tasks are exhausted. |
| **PSRAM Latency During Smooth Scrolling** | Low | Framebuffer is 48 KB in SRAM. Sliding blits only need to copy active scanlines from PSRAM to SRAM, taking < 1 ms per frame—well within the 17.8 ms frame window (56 Hz). |
| **Headless Target Regressions** | High | The multi-window ribbon manager is compiled conditionally under `CONFIG_ENABLE_SCREEN` and `CONFIG_BOARD_RP2350_TERMINAL`. QEMU virt targets retain existing single-console behavior unless configured. |
| **Terminal Output Contention** | Low | Each `vterm` maintains its own independent lock and shadow buffer. `printk` and klog continue to output to the system console device without corrupting per-window shells. |
