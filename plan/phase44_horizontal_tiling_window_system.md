# Phase 44 — Horizontal Scrollable Tiling Window System (Niri for LugalOS)

**Status: Draft / Proposed (2026-10-03).**  
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
| **`Cmd + W`** | **Close Window** | Closes active window and terminates its task (cannot close the last root shell). |
| **`Cmd + Shift + 3`** | **Screenshot** | Captures current visible viewport to `/sd0/` as PNG (existing feature preserved). |

---

## 3. Milestones

### Milestone 44.1: Virtual Console & Multi-Terminal Subsystem
* **Goal:** Allow multiple independent terminal sessions to run concurrently with separated I/O.
* **Tasks:**
  1. Define `vterm_t` abstraction in `kernel/include/kernel/vterm.h` encapsulating input FIFO, `vtterm_t` state, and task association.
  2. Implement `vterm_create()`, `vterm_destroy()`, `vterm_write()`, and `vterm_read()`.
  3. Modify `kernel/shell.c` so `shell_run()` can run as an independent worker task attached to a specific `vterm`.
  4. Ensure `console_lock` and `printk` safely coexist with multiple active virtual terminals.
* **Verification:** Host harness and QEMU test validating concurrent execution and independent output streams of two separate shells.

---

### Milestone 44.2: Ribbon Data Structures & Geometry Manager
* **Goal:** Implement the 1D horizontal window ribbon data structure and layout calculations.
* **Tasks:**
  1. Define `ribbon_t` and `ribbon_win_t` in `drivers/include/drivers/screen.h`.
  2. Implement ribbon operations: `ribbon_insert()`, `ribbon_remove()`, `ribbon_swap()`, `ribbon_move()`.
  3. Calculate column widths and horizontal offsets:
     - Preset widths: 38 cols (318 px), 48 cols (398 px), 64 cols (526 px), 98 cols (798 px).
     - Title bar chrome, borders, and 1-px drop shadows.
  4. Implement viewport positioning logic: given focused window index, compute target $X_{\text{view}}$ so the active window is fully visible on screen.
* **Verification:** Standalone geometry unit test verifying coordinate math, ribbon bounds, and viewport alignment.

---

### Milestone 44.3: Keyboard Navigation & Hotkey Routing
* **Goal:** Intercept and route all window management keyboard combinations.
* **Tasks:**
  1. Extend `drivers/usbkbd.c` to decode `Cmd + Enter`, `Cmd + Left/Right`, `Cmd + Ctrl + Left/Right`, and `Cmd + W`.
  2. Update `kernel/include/kernel/console.h` with new `CONSOLE_HOTKEY_*` definitions.
  3. Wire hotkey handlers into `kernel/console.c` to dispatch focus changes, insertions, and layout adjustments to the ribbon manager.
* **Verification:** `vtselftest` / `keyselftest` automated tests verifying correct event generation and hotkey interception.

---

### Milestone 44.4: Viewport Compositing & Smooth Sliding Animation
* **Goal:** Render the active portion of the horizontal ribbon into the physical 1-bpp framebuffer with high performance.
* **Tasks:**
  1. Implement scanline-clipped tile blitter in `drivers/screen.c` rendering visible ribbon windows into the 800 × 480 framebuffer.
  2. Implement discrete viewport snap: instant switch (< 1.5 ms) when scrolling to an adjacent window.
  3. Implement smooth sliding transition: step $X_{\text{view}}$ across 6–8 frames at 56 Hz when shifting focus, rendering smooth horizontal ribbon motion.
  4. Preserve menu bar (y 0..19) static at the top while windows slide underneath.
* **Verification:** Frame timing benchmark on RP2350 measuring render duration per frame during sliding transitions.

---

### Milestone 44.5: Canvas & Application Integration
* **Goal:** Seamlessly integrate graphical canvas applications into the horizontal ribbon.
* **Tasks:**
  1. Update `canvas-window` Lisp primitive and `screen_canvas()` protocol to support inserting Canvas Windows directly into the ribbon.
  2. Enable running CAS `plot` / `plot-diff` in one window while keeping the Lisp REPL in the neighboring window:
     `[ Lisp REPL ] ↔ [ Function Plot Canvas ] ↔ [ Shell / Editor ]`.
  3. Adapt Chess GUI and showcase demos (`ca.lisp`, `lorenz.lisp`) to attach to ribbon canvas tiles.
* **Verification:** QEMU automated test and silicon test opening a canvas window alongside multiple terminal windows.

---

### Milestone 44.6: Hardware Verification & Benchmarks on Real Silicon
* **Goal:** Complete end-to-end verification on the physical Waveshare RP2350-LCD-7 workstation.
* **Tasks:**
  1. Verify multi-terminal concurrency on physical silicon:
     - Open 4+ concurrent terminals via `Cmd + Enter`.
     - Run a long Lisp computation in Terminal 0 while editing a file with `e` in Terminal 1.
  2. Test ribbon sliding and navigation using physical USB keyboard.
  3. Verify CAS mathematical plotting and calculus visualizer in side-by-side ribbon panes.
  4. Capture hardware screenshots (`Cmd + Shift + 3`) over 9P demonstrating multi-window ribbon layouts.
  5. Audit memory overhead: confirm **0 bytes SRAM impact** on non-terminal targets.

---

## 4. Risks & Mitigations

| Risk | Impact | Mitigation |
| :--- | :---: | :--- |
| **Scheduler Task Table Overflow** | Med | `CONFIG_SCHED_MAX_TASKS` is currently 24. With ~8 driver tasks, spawning 4–6 shells is well within bounds. `vterm_create()` gracefully refuses new terminals if tasks are exhausted. |
| **PSRAM Latency During Smooth Scrolling** | Low | Framebuffer is 48 KB in SRAM. Sliding blits only need to copy active scanlines from PSRAM to SRAM, taking < 1 ms per frame—well within the 17.8 ms frame window (56 Hz). |
| **Headless Target Regressions** | High | The multi-window ribbon manager is compiled conditionally under `CONFIG_ENABLE_SCREEN` and `CONFIG_BOARD_RP2350_TERMINAL`. QEMU virt targets retain existing single-console behavior unless configured. |
| **Terminal Output Contention** | Low | Each `vterm` maintains its own independent lock and shadow buffer. `printk` and klog continue to output to the system console device without corrupting per-window shells. |
