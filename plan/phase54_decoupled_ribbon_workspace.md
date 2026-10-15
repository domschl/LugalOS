# Phase 54: Decoupled Ribbon Workspace, Named Canvases & Multi-Instance Applications

## Status: PLANNED
**Goal**: Completely decouple the Ribbon window management subsystem from the legacy 1:1 terminal-canvas split model. Introduce first-class **named canvases** with session persistence, enable **multiple concurrent Reader instances**, integrate interactive graphics applications (**Lisp plots, Chess, Z-Machine auto-mapper**), and update keyboard UX to match modern tiling paradigms.

---

## 1. Context & Architectural Rationale

LugalOS evolved its display system across several major phases:
1. **Phase 37 (Screen Layouts & Split Modes)**: Introduced a binary display mode on a single screen (`TEXT`, `CANVAS`, `SPLIT_WIDE`, `SPLIT_HALF`). Canvases were strictly slave companion panes attached to a single terminal.
2. **Phase 44 & 47 (Horizontal Tiling Window System)**: Introduced the 1D scrollable Ribbon (Niri-inspired), allowing multiple virtual terminals and windows.
3. **Phase 53 (Typographic Ribbon Reader & Spartan Hypermedia)**: Introduced proportional serif document browsing as a new window type (`RIBBON_WIN_READER`).

### The Friction Points Identified
* **Resource Collision**: `reader` allocated a canvas backing slot (`screen_alloc_canvas(scr, 0)`), hijacking canvas slot 0 and confusing `ribbon_find_canvas_slot()`.
* **Cross-Talk Between Apps**: Running `chess` or a Lisp `(plot ...)` while `reader` was open resized the Reader window, overwrote its bitmap with chess/plot data, and quitting chess deleted the Reader window from the ribbon.
* **Canvas Slot Leaks**: Closing windows with `Cmd+W` vs `L TEXT` left `scr->canvases[].in_use` flags permanently set. With `SCREEN_MAX_CANVASES = 4`, slots quickly exhausted, causing canvas creation to break permanently until reboot.
* **Transient vs. Persistent Outputs**: A Lisp `plot` or `fractal` should remain visible on the ribbon after the script returns to the shell prompt, whereas interactive games (`chess`) should close their board upon `quit`.
* **Outdated Keyboard Shortcut**: `Super+\` ("swap terminal-canvas") was designed for the old 2-pane split mode and is counter-intuitive in an $N$-window horizontal ribbon.

---

## 2. Architecture & Design Principles

```
Ribbon (1D Horizontal Tiling Window Ribbon, up to 8 windows)
+----------------+----------------+----------------+----------------+
|  Terminal 0    |  Canvas "plot" |  Reader "help" |  Canvas "map"  |
|  (vterm 0)     |  (canvas 0)    |  (reader 0)    |  (canvas 1)    |
|  cols: 49      |  cols: 49      |  cols: 62      |  cols: 49      |
+----------------+----------------+----------------+----------------+
      ^                  ^                ^                ^
      |                  |                |                |
   Shell lsh      Lisp Plot Output   Doc Viewer     Z-Machine Mapper
```

### 1. Windows as First-Class Ribbon Peers
* Every window in the ribbon has an explicit type (`RIBBON_WIN_TERM`, `RIBBON_WIN_CANVAS`, `RIBBON_WIN_READER`) and its own geometry (`cols`, `rx0`, `rx1`).
* Windows are non-overlapping horizontal tiles.
* Window widths are managed individually; closing a neighboring window does not lock a terminal into an orphaned "split" state. If only one window remains, it automatically expands to full screen width.

### 2. Named Canvases (`canvas open <name>`, `canvas attach <name>`)
* Canvases are identified by an alphanumeric name (e.g., `"plot"`, `"chess"`, `"map"`, `"chart"`, `"fractal"`), up to 16 characters.
* If a canvas with the requested name already exists, the terminal attaches to it and brings it into view.
* If it does not exist, a free canvas slot is allocated, assigned the name, and inserted into the ribbon.
* Drawing commands without an explicit name automatically target the terminal's currently attached canvas.
* **Persistence**: Generating a plot leaves the canvas open on the ribbon when the script exits. Running the plot again updates the existing canvas in place.
* **Explicit Close**: Closed only via `Cmd+W`, touch close button `[X]`, CLI `canvas close [name]`, or programmatic exit (e.g. `chess` quit).

### 3. Multiple Reader Instances
* Reader is completely decoupled from canvas slots. It never touches `scr->canvases[]`.
* Up to 4 concurrent Reader instances (`READER_MAX_INSTANCES = 4`), each with its own independent file path, scroll position, history stack, and 1-bpp proportional text surface.
* Allows side-by-side reading of documentation, manuals, and books.

### 4. Modern Tiling Keyboard UX
* **Retire `Super+\` swap**: Moving windows is already handled by `Super+Ctrl+Left`/`Right`.
* **Repurpose `Super+\`**: Toggle Maximize/Restore (toggles active window width between Full screen and Half width).
* **Closing**: `Cmd+W` or touch `[X]` unconditionally reclaims all resources of the focused window. Note: we should add a Macintosh-style 'close-box' at the left side of the window-bar that can be used by touch to close a window.

---

## 3. Milestones & Implementation Tasks

### Milestone 54.1: Architecture & Data Structures Decoupling
* **`drivers/include/drivers/screen.h`**:
  * Decouple `screen_canvas_slot_t` from `vterm_id`. Add `char name[16]` and remove modal terminal binding.
  * Define `screen_reader_slot_t` (or `reader_instance_t`) pool (`SCREEN_MAX_READERS = 4`), completely separated from `SCREEN_MAX_CANVASES`.
  * Remove global modal split variables (`scr->layout`, `scr->swapped`) in favor of per-window geometry.
* **`drivers/screen.c`**:
  * Rewrite `screen_alloc_canvas()` to search by name or allocate an unassigned slot.
  * Rewrite `screen_free_canvas()` to unconditionally reset `in_use`, `locked`, and `name`.
  * Fix `ribbon_find_canvas()` and `ribbon_find_canvas_slot()` to match ONLY `RIBBON_WIN_CANVAS`.
  * Ensure `close_window()` cleans up both canvas and reader slots symmetrically with zero leaks.

### Milestone 54.2: Named Canvas Protocol & Command Interface
* **Shell Command (`kernel/shell.c`)**:
  * `canvas list`: enumerates all open canvases, names, dimensions, and active attachment.
  * `canvas open <name> [cols]`: opens or attaches to a named canvas.
  * `canvas attach <name>`: sets current terminal session target.
  * `canvas close [name]`: closes canvas by name (or active canvas if omitted).
  * `canvas clear [name]`: clears canvas bitmap.
* **Protocol Extensions (`console_canvas()`)**:
  * Request payload accepts canvas name string for open/attach.
  * Status replies confirm active canvas index and dimensions.

### Milestone 54.3: App Integrations (Plot, Chess, Z-Machine Auto-Mapper)
* **Lisp Engine (`user/lisp/lisp.c`)**:
  * Update `(plot ...)` and `(ca ...)` primitives to target named canvas `"plot"` (or explicit name passed via `(canvas-open "name")`).
  * Ensure Lisp scripts leave the canvas intact upon termination.
  * Add Lisp primitives: `(canvas-open name)`, `(canvas-select name)`, `(canvas-close name)`, `(canvas-list)`.
* **Chess UI (`user/chess/src/chess_ui.c`)**:
  * Automatically target canvas named `"chess"`.
  * When user quits game (`quit` command), cleanly close `"chess"` canvas and restore terminal width.
* **Z-Machine Auto-Mapper (`user/zmachine/zmap.c`)**:
  * Target canvas named `"map"`.
  * Draw live directed room graph in `"map"` canvas alongside the active game terminal.
  * On game shutdown, cleanly close `"map"` canvas.
  * Note: Applications should be examined to support consistent canvas management (e.g. User closes a canvas via Super-W, (e.g. chess-board), and then wants to re-open the board. Same with zmachine maps.

### Milestone 54.4: Multi-Instance Ribbon Reader
* **`drivers/reader_view.c`**:
  * Convert static `s_reader` into an array `s_readers[READER_MAX_INSTANCES]`.
  * Allocate 1-bpp surface for each reader instance from PSRAM / bulk zone (or dedicated static buffers).
  * Route keyboard navigation and touch gestures to the active reader instance based on focused window `reader_id`.
* **`kernel/shell.c` (`cmd_reader`)**:
  * `reader <path>`:
    * If active window is a reader, navigates it to `<path>`.
    * If a reader with `<path>` is already open, focuses it.
    * Otherwise, spawns a new Reader window in the ribbon.
  * `reader list`: lists active reader windows.
  * `q` / `Cmd+W`: closes focused reader window and frees its instance slot.

### Milestone 54.5: Keyboard & Navigation UX Overhaul
* **Repurpose `Super+\`**:
  * Change `CONSOLE_HOTKEY_SWAP` in `kernel/include/kernel/console.h` and `kernel/line_editor.c` to `CONSOLE_HOTKEY_TOGGLE_MAX`.
  * In `kernel/console.c` and `drivers/screen.c`, implement toggle between `RIBBON_COLS_FULL` and `RIBBON_COLS_HALF`.
* **Window Close Button**:
  * Title bar touch tap on `[X]` sends `CONSOLE_HOTKEY_CLOSE` targeting the clicked window.
* **Auto-expansion**:
  * When a canvas or reader window closes, if the adjacent terminal is the sole remaining window in that screen region, it expands to `scr->full_cols`.

### Milestone 54.6: Verification & Hardware Testing
* **Host Regression & Unit Tests**:
  * `tests/host/canvas_host.c`: verifies named canvas allocation, attachment, multiple canvases, and zero slot leaks over repeated cycles.
  * `tests/host/reader_host.c`: verifies multi-instance reader allocation, viewport independence, and reclamation.
* **Full Automated Suite**:
  * `python3 tests/runner.py`: verify QEMU RV32, RV64, and Lisp test suites.
* **Hardware-in-the-Loop on ESP32-P4-WIFI6-Touch-LCD-7B**:
  * `tests/hw/test_esp32p4.py`: verify concurrent Chess + Plot + Reader + Z-Machine mapping without collisions or crashes.

---

## 4. Verification Checkpoints

1. **Named Canvases**: Opening `"plot"`, `"chess"`, and `"map"` simultaneously displays 3 distinct canvas windows on the ribbon.
2. **Persistence**: Running `(plot ...)` in Lisp leaves the plot intact on the ribbon after returning to `lsh>`. Running it again updates the plot in place.
3. **Multi-Reader**: Opening `reader shell.gmi` and `reader spartan.gmi` produces two side-by-side reader windows with independent scroll and navigation.
4. **Zero Leaks**: Opening and closing 20 canvases and readers in sequence leaves all slots free (`in_use = false`) and never exhausts slots.
5. **Keyboard UX**: `Super+\` cleanly toggles the focused window between full-width and half-width; `Cmd+W` cleanly closes any active window.
