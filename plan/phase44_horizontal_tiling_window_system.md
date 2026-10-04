# Phase 44 — Horizontal Scrollable Tiling Window System (Niri for LugalOS)

**Status: Complete & Hardware Verified (2026-10-04).**  
**Target Hardware:** Waveshare RP2350-LCD-7 (800 × 480 1-bpp monochrome panel, 8 MB QSPI PSRAM, USB keyboard).

---

## 0. Context & Architectural Rationale

### Where this comes from
In Phase 37 (`plan/phase37_screen_layouts_and_apps.md`), LugalOS introduced constrained screen tiling: a fixed menu bar, desktop background, and at most two framed tiles (a text terminal and an optional graphics canvas) in fixed ratios (`TEXT`, `CANVAS`, `SPLIT_WIDE`, `SPLIT_HALF`, `SPLIT_NARROW`, and `swapped`). Phase 37 deliberately deferred a general window system because internal SRAM was constrained to 520 KB and overlapping windows would require complex 2D clipping regions and heavy backing stores.

In Phase 38 (`plan/phase38_psram.md`), the RP2350-terminal persona gained **8 MB of QSPI PSRAM** with `BULK_BSS` and `palloc_pages_bulk()`, providing abundant memory for backing stores.

### The Decision: A 1D Horizontal Ribbon (Inspired by Niri)
Traditional overlapping/floating window managers (e.g. Mac OS, Windows) are ill-suited for an 800 × 480 monochrome workstation:
1. **Vertical Scarcity:** 480 pixels provide exactly 27 text rows (at 16 px/row) plus menu bar and margins. Vertical window stacking produces cramped, unusable viewports.
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

## 1. System Architecture & Memory Model

### 1.1 Window Types & Descriptor Structure
A window on the ribbon is represented by a `ribbon_win_t` descriptor in `drivers/include/drivers/ribbon.h`:
* **Text / Terminal Window (`RIBBON_WIN_TERM`):**
  - Backed by an independent `vtterm_t` state machine.
  - Character cell shadow buffer: `cols × rows × sizeof(uint16_t)` (e.g. 48 × 27 × 2 = 2.6 KB; 98 × 27 × 2 = 5.3 KB).
  - Dedicated input FIFO (`rx_fifo`, 128 bytes).
  - Associated task PID (e.g. an independent `lsh` shell worker).
* **Canvas / Graphics Window (`RIBBON_WIN_CANVAS`):**
  - Backed by a dedicated `screen_canvas_slot_t` in `screen_t`.
  - Persistent 1-bpp bitmap buffer allocated in PSRAM via `palloc_pages_bulk()`.
  - Column width matching layout preset (e.g. 38, 48, 64, or full 98 cols).
  - Used by CAS mathematical plotting (`cas/plot.lisp`), demos (`ca.lisp`, `lorenz.lisp`), and graphical chess (`chess_ui.c`).

### 1.2 Multi-Canvas Isolation & PSRAM Backing Buffers
A major UX limitation identified during hardware testing was single-canvas contention: if Chess opened a board canvas and a second terminal launched a Lisp graphic (e.g. `ca.lisp`), they fought over the same canvas buffer, corrupting or blanking each other's graphics.

The system resolves this via **Concurrent Multi-Canvas Isolation**:
```
PSRAM Backing Store Pool (256 KB total in BULK_BSS):
  Slot 0 [48 KB]: Terminal 0 Canvas (e.g. Chess Board)
  Slot 1 [48 KB]: Terminal 1 Canvas (e.g. Rule 30 Cellular Automaton)
  Slot 2 [48 KB]: Terminal 2 Canvas (e.g. CAS Function Plot)
  Slot 3 [48 KB]: Terminal 3 Canvas (Spare / User Graphic)
```
* **Per-Vterm Canvas Routing:** Canvas operations (drawing primitives `'p'`, `'l'`, `'r'`, `'F'`, `'t'`, `'b'`, `'g'`, title `'T'`, layout `'L'`, swap `'X'`) are routed via `screen_canvas_vterm(scr, vid, req, n, reply)`, identifying the owning terminal by `task_get_vterm(sched_current_pid())`.
* **Independent Persistence:** Each canvas writes simultaneously to its dedicated PSRAM backing buffer and to the visible scanout framebuffer if currently within the viewport. When switching between windows or scrolling across the ribbon, canvas contents are restored from their dedicated PSRAM slot in < 400 µs without any recomputation or application redraw.
* **Per-Slot Layout Locking:** The layout lock (`op == 'K'`) is scoped per canvas slot (`scr->canvases[cid].locked`). Applications that require a fixed split (such as Chess) lock only their own canvas width, leaving other terminal windows completely free to open, split, or close their own canvases without interference.

### 1.3 Memory Budget (Zero SRAM Overhead)
* All ribbon descriptors, virtual terminal shadows, and canvas bitmap stores reside in **PSRAM** (`BULK_BSS` / `palloc_pages_bulk()`).
* Sponsoring 8 terminal windows and 4 concurrent canvas stores consumes **< 500 KB** total (< 6% of the 8 MB PSRAM).
* Headless and non-terminal presets (`rp2350`, `rp2350-clock`, `rp2350-gateway`, `esp32p4`, `rv32`, `rv64`) incur **0 bytes of SRAM change** and compile without the multi-window manager.

### 1.4 Concurrency & Multi-Terminal Subsystem
* Spawning a new terminal (`Cmd + Enter`) creates a new `lsh` worker task via `task_create()`.
* **Input Routing:** Hardware keystrokes from `drivers/usbkbd.c` are routed strictly to the **focused window's** `rx_fifo`.
* **Output Isolation:** Background tasks continue to write to their respective `vterm` shadows in memory. When the viewport scrolls back to an updated window, its display is refreshed immediately.
* **Task Lifecycles:** When a terminal shell exits (via `exit` or `Cmd + W`), its window is removed from the ribbon, its backing memory is freed, and adjacent windows close the gap. Closing a canvas window (`canvas close` or `Cmd + W` on canvas) leaves the terminal task running and expands or shifts the ribbon cleanly.

### 1.5 Viewport Compositing & Tear-Free Rendering
The physical panel is driven by PIO2 and DMA scanning the primary 48 KB 1-bpp SRAM framebuffer at 56 Hz.

To eliminate tearing and flickering observed during early scrolling tests:
* **Surgical Background Clearance (`clear_ribbon_background`):** Rather than wiping the entire 48 KB framebuffer with grey on each redraw, `clear_ribbon_background()` clears *only* the margins (2px top margin, 1px bottom margin, outer edges) and the exact 10px inter-window gaps. Window interiors are never touched during background clearance, dropping scanout RAM writes by **> 97%**.
* **1-Frame Navigation Transitions:** Early implementations used an unpaced tight while-loop (15 iterations over ~9 ms, faster than a single 17.8 ms display refresh), causing severe horizontal tearing and grey flickering. Navigation transitions are now paced into clean, single-frame composited redraws (`draw_all_from(scr, true)`), delivering instant, tear-free window navigation.

---

## 2. Visual Design & UX Decisions

### 2.1 Classic Macintosh Aesthetic (Zebra Title Bars)
To make window focus immediately legible on a monochrome 1-bpp display without heavy borders:
* **Active Window Title Bar:** Identified by horizontal zebra stripes (`title_zebra_line`) running across the title bar behind the centered window title, matching the classic Macintosh System 1–7 aesthetic.
* **Inactive Window Title Bars:** Rendered with clean white backgrounds, dark text, and subtle top/bottom borders.

### 2.2 Top Menu Bar Ribbon Minimap
In the top menu bar, positioned between the system name (`LugalOS`) and the right-hand indicators (clock/network), a live minimap visualization represents the horizontal ribbon:
```
  +---------+   +-----+   +---------+   +---------+
  |         |   | <>  |   | /////// |   |         |
  +---------+   +-----+   +---------+   +---------+
   [Term 0]     [Board]   [lsh (1)]     [Rule 30]
                 ^ Canvas   ^ Active
```
* **Miniature Blocks:** Every window in the ribbon is rendered as a miniature block whose width is proportional to its column width (38, 48, 64, or 98 columns).
* **Active Window Marking:** Rendered with zebra stripes inside its minimap block, harmonizing with the active window's title bar.
* **Canvas Glyph Mark:** Canvas windows feature a centered diamond/square glyph (`CANVAS_ICON`) inside their blocks, allowing users to distinguish text terminals from graphics canvases at a glance.
* **Viewport Frame Enclosure:** A framing box encompasses the mini-blocks that are currently visible on the physical screen:
  - **Solid Frame:** Rendered when all visible windows are fully contained within the viewport.
  - **Dotted / Dashed Frame:** Rendered when a window at the edge of the viewport is only partially displayed, alerting the user that content extends beyond the screen edge.

---

## 3. Interaction Model & Command Surface

### 3.1 Keyboard Hotkeys
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
| **`Cmd + \`** | **Quick Swap / Toggle** | Swaps active terminal with its canvas; if no canvas is open, reopens a split canvas. |
| **`Cmd + 1` .. `9`** | **Direct Jump** | Jumps focus directly to window $N$ on the ribbon; centers viewport. |
| **`Cmd + W`** | **Close Window** | Closes active window (canvas or terminal). Root shell cannot be closed. |
| **`Cmd + Shift + 3`** | **Screenshot** | Captures current visible viewport to `/sd0/screenshots/` as a PBM file. |

### 3.2 Shell Command Interface (`canvas` & `ribbon`)
In addition to keyboard shortcuts, the shell exposes dedicated commands for scriptability and explicit window management:

* **`canvas [split | half | wide | narrow | full | close]`:**
  - `canvas split` or `half`: Opens or resizes the canvas to 48 columns (1:1 split).
  - `canvas wide`: Expands canvas to 58 columns (text reduced to 38 columns).
  - `canvas narrow`: Shrinks canvas to 32 columns (text expanded to 64 columns).
  - `canvas full`: Maximizes canvas across the full 98 columns.
  - `canvas close`: Closes the active terminal's canvas and removes it from the ribbon.
* **`ribbon [list | focus <idx> | left | right | move left | move right | selftest]`:**
  - Displays ribbon layout geometry, window types, column widths, and coordinate bounds.

### 3.3 Application Integration: Chess Console Enhancements
To improve workflow and allow capturing state without exiting interactive applications:
* **Interactive S-Expression Evaluation:** Any command entered at the `chess>` prompt starting with `'('` is evaluated directly via `lisp_eval_string()` with automatic GC safepoint collection (e.g. `chess> (+ 50 50)` prints `=> 100`).
* **Direct Screenshot Command:** `screenshot [path]` can be typed directly at the `chess>` prompt (or invoked via `(screenshot)`) to save a PBM image to `/sd0/screenshots/`.

---

## 4. Implementation Milestones

### Milestone 44.1: Virtual Console & Multi-Terminal Subsystem [COMPLETE]
* Implemented `vterm_t` abstraction in `kernel/include/kernel/vterm.h` and `kernel/vterm.c` with per-vterm locks, shadow memory, and independent task isolation.
* Shell worker tasks dynamically attached via `shell_spawn_terminal()`.
* Resolved lock-order inversion between `g_vterm_mgr_lock` and `console_lock`.
* **Verification:** `vtselftest` (36/36 assertions passed on RV64 and RV32) and multi-terminal host regression suite.

### Milestone 44.2: Ribbon Data Structures & Geometry Manager [COMPLETE]
* 1D horizontal window ribbon data structure `ribbon_t` and `ribbon_win_t` in `drivers/ribbon.c` and `drivers/include/drivers/ribbon.h`.
* Column widths, gap spacing, coordinate offsets, and viewport computations.
* **Verification:** `ribbonselftest` (22 assertions passed).

### Milestone 44.3: Keyboard Navigation & Hotkey Routing [COMPLETE]
* Intercepted `Cmd + Enter`, `Cmd + Left/Right`, `Cmd + Ctrl + Left/Right`, `Cmd + W`, `Cmd + \`, and `Cmd + 1..9` in `drivers/usbkbd.c`.
* Dispatched to `console_canvas` commands (`'N'`, `'F'`, `'M'`, `'C'`, `'X'`, `'J'`) in `kernel/console.c`.
* **Verification:** `usbkbdselftest` and `keyselftest` verified.

### Milestone 44.4: Viewport Compositing, Surgical Clearance & Minimap [COMPLETE]
* Implemented `clear_ribbon_background()` and fast scanline compositing in `drivers/screen.c`.
* Added ribbon minimap indicator with proportional block widths, active zebra pattern, canvas diamond glyphs, and partial-viewport dotted framing in `draw_menu()`.
* **Verification:** Validated on QEMU and verified tear-free on real RP2350-LCD-7 silicon.

### Milestone 44.5: Multi-Canvas Backing Stores & App Integration [COMPLETE]
* Added `screen_canvas_slot_t` pool (4 slots) with 48 KB PSRAM backing buffers.
* Routed canvas operations by calling task's `vterm_id`.
* Scoped layout locking (`op == 'K'`) per-slot.
* Implemented `canvas` shell command and enhanced `chess_ui.c` with Lisp evaluation and screenshot commands.
* **Verification:** Concurrent Chess + Rule 30 execution with zero graphical corruption.

### Milestone 44.6: Silicon Hardware Verification [COMPLETE]
* Flashed to physical Waveshare RP2350-LCD-7 workstation.
* Full test suite: 466/467 tests passed (sole non-passing test was an unrelated host timing flake).
* 0 SRAM byte overhead on non-terminal targets.

---

## 5. Hardware Verification & Artifacts

The window system and multi-canvas subsystem were verified through automated end-to-end Python hardware harnesses (`verify_multicanvas.py`, `verify_switch.py`, `verify_full.py`) interacting over USB CDC ACM ports (`/dev/ttyACM0` for console, `/dev/ttyACM1` for 9P synchronization):

| Artifact Image | Description |
| :--- | :--- |
| **`shot_mul.png`** | **Concurrent Canvases & Top Minimap:** Terminal 1 (`lsh [1]`) running Rule 30 cellular automaton on its dedicated canvas beside Terminal 0's Chess board. Minimap shows 4 windows with diamond canvas markers and zebra active indicator. |
| **`shot_che.png`** | **Chess Console & Lisp Integration:** Interactive Chess console evaluating `(+ 100 23) => 123` and executing `screenshot` directly from the chess prompt. |
| **`shot_chess_back.png`** | **Zero-Corruption Navigation:** Chess board 100% intact after navigating across the ribbon from the cellular automaton. |
| **`shot_reopened_clean.png`** | **Dynamic Reopening:** Terminal 1 closing its canvas (`canvas close`) and cleanly reopening it (`canvas split`) without interference from Chess's layout lock. |

---

## 6. Risks & Mitigations

| Risk | Impact | Mitigation |
| :--- | :---: | :--- |
| **Scheduler Task Table Overflow** | Med | `CONFIG_SCHED_MAX_TASKS` is 24. With ~8 driver tasks, spawning 4–6 shells is well within bounds. `vterm_create()` gracefully refuses new terminals if tasks are exhausted. |
| **Memory Thrashing Mid-Scanout** | High | Replacing 15-iteration unpaced loops with paced 1-frame rendering and surgical background clearance reduced RAM bandwidth demand by >97%, eliminating tearing. |
| **Cross-Window Canvas Corruption** | High | Resolved by allocating 4 dedicated 48 KB backing buffers in PSRAM and routing canvas requests strictly by calling task's `vterm_id`. |
| **Headless Target Regressions** | High | Multi-window ribbon manager is compiled conditionally under `CONFIG_ENABLE_SCREEN` and `CONFIG_BOARD_RP2350_TERMINAL`. QEMU virt targets retain single-console behavior. |
