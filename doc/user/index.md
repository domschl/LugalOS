# LugalOS User Manual

Welcome to the **LugalOS User Manual**. This guide covers interactive usage, daily shell operation, application software, and runtime tools on LugalOS workstations and terminals.

---

## Manual Chapters

1. **[Shell & Command Environment](shell.md)**
   * Launching commands, path resolution (`/system/bin`), background jobs, pipeline and redirection concepts.
   * Virtual filesystems: inspecting processes via `/proc/ps`, memory via `/proc/meminfo`, and hardware devices in `/dev/`.
2. **[Text Editors: Full-Screen & Line Editors](editors.md)**
   * Full-screen interactive editor (`e`): Emacs-style keybindings, buffer editing, search, save, and exit.
   * Teletype line editor (`ed`): Standard line-oriented editing for headless or low-bandwidth serial links.
3. **[Window Ribbon & Terminal System](ribbon.md)**
   * The horizontal tiling window ribbon on 7" LCD displays (RP2350-LCD-7 and ESP32-P4-LCD-7B).
   * Touch gestures: window focus, panning, minimap tapping.
   * Keyboard shortcuts (`Cmd+Left/Right`, window splits), graphical canvases, and PBM screenshots.
4. **[LugalOS Chess Computer](chess.md)**
   * Interactive chess engine on RP2350 and display terminals.
   * Dual-core Lazy SMP parallel search, TM1638 keypad/LED controls, game storage, and opening book.
5. **[Computer Algebra System (CAS)](cas.md)**
   * Exact symbolic computation in pure Lisp without floating point.
   * Canonical simplification, polynomial factorization, equation solving, Taylor/Laurent series, and matrix algebra.
6. **[Environmental Sensors & Telemetry](sensors.md)**
   * Reading I2C temperature, humidity, pressure, and gas sensors.
   * Configuring autonomous telemetry nodes, filtering rules, and publishing over MQTT 3.1.1.
7. **[Z-Machine & Interactive Fiction](zmachine.md)**
   * Playing Infocom and modern Version 3 (.z3) interactive fiction classics.
   * Real-time split-screen auto-mapper (`zmap`): dynamic graph discovery, room positioning, detail and zoom overview modes.
   * Save slots (0–9), autosave, persistent sidecar `.map` files, and Lisp expression evaluation at the game prompt.

---

## On-Device Help Access

When working on a physical LugalOS terminal or serial console:
* Type `help` in the shell for a summary of built-in commands.
* Type `man <topic>` or `help <topic>` (e.g. `man chess` or `help shell`) to open the interactive help viewer with single-key link jumping and page scrolling.
