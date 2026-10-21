# LugalOS Window Ribbon & Terminal System

LugalOS features a native **Horizontal Tiling Window Ribbon** designed specifically for widescreen displays (such as the 7" 800×480 Waveshare RP2350-LCD-7 and the 7" 1024×600 Waveshare ESP32-P4-LCD-7B).

Instead of overlapping windows or rigid vertical splits, LugalOS tiles windows side-by-side along an infinite horizontal ribbon. Users and applications can seamlessly navigate between terminals, graphical canvases, and application panes.

---

## 1. Ribbon Layout & Screen Geometry

```
┌────────────────────────────────────────────────────────────────────────┐
│ [1: lsh]  [2: Lorenz]  [3: Chess]       Minimap: [■ ■ □]     12:45 UTC │ top menu bar
├───────────────────────────────┬────────────────────────────────────────┤
│ lsh> cat /proc/meminfo        │     * * * * *                          │
│ mem: 8192 pages total         │   *           *                        │ active / focused
│ free: 6420 pages              │  *    CANVAS   *                       │ windows on ribbon
│ lsh> _                        │   *           *                        │
│                               │     * * * * *                          │
└───────────────────────────────┴────────────────────────────────────────┘
```

The top of the screen displays a persistent status bar featuring:
* **Window Tabs**: Numbered indicators for active terminals and canvases.
* **Minimap**: Visual representation of the full ribbon layout.
* **System Clock**: Hardware RTC time synchronized at boot.

---

## 2. Navigation & Window Controls

### Window Controls & Macintosh Close Box
Every non-root window on the ribbon (terminals, canvases, and readers) features a classic Macintosh-style close box (`[■]`) at the left of its title bar:
* **Tap / Click Close Box**: Immediately closes the window, reclaims its backing store and layout slot, and smoothly reflows the ribbon.
* **Toggle Maximize (`Super+\` / `Cmd+\`)**: Toggles the active window between full-screen ribbon width and half-screen width.
* **Close Window (`Super+W` / `Cmd+W`)**: Closes the active non-root window from keyboard.

### Keyboard Shortcuts
| Hotkey | Serial ANSI Sequence | Function |
| :--- | :--- | :--- |
| `Cmd+Right` | `ESC [ 1 ; 9 C` | Pan ribbon right, focusing next window |
| `Cmd+Left` | `ESC [ 1 ; 9 D` | Pan ribbon left, focusing previous window |
| `Cmd+\` / `Super+\` | `ESC [ 92 ; 9 u` | Toggle Maximize / Restore width of active window |
| `Cmd+O` / `Super+O` | `ESC [ 111 ; 9 u` | Toggle display orientation between Portrait and Landscape (Tab5) |
| `Cmd+W` / `Super+W` | `ESC [ 119 ; 9 u` | Close focused non-root window |
| `Cmd+1`..`Cmd+9` | `ESC [ 49 ; 9 u` | Jump directly to window index 1..9 |
| `Cmd+Shift+3` | `Super+Shift+3` | Capture full-screen PBM screenshot to `/sd0/screenshots/` |

### Touch Gestures (GT911 Capacitive Touch)
* **Tap Window**: Tapping anywhere inside a window pane transfers focus to it.
* **Tap Close Box**: Tapping the close box (`[■]`) in the top-left title bar closes the window.
* **Tap Minimap**: Tapping a window block in the top minimap immediately scrolls the ribbon to that window.
* **Swipe Left / Right**: Swiping sideways with a finger smoothly scrolls the ribbon.

---

## 3. Decoupled Named Canvases & Visualization

Graphical canvases in LugalOS are decoupled from terminal lifetimes and can be assigned persistent names (such as `"plot"`, `"chess"`, or `"map"`). Plots and visualizations remain on the ribbon workspace when returning to the shell prompt `lsh>`, allowing interactive script evaluation while keeping graphics visible.

### Managing Canvases from the Shell
* `canvas list`: Lists all active named canvases, their dimensions, and PSRAM backing stores.
* `canvas open <name> [preset]`: Opens or focuses a named canvas (presets: `split`, `full`, `wide`, `narrow`).
* `canvas attach <name>`: Attaches an existing named canvas to the active ribbon view.
* `canvas close [name]`: Closes the named canvas (or active canvas if omitted).
* `canvas clear [name]`: Clears the canvas buffer.
* `canvas full`: Maximizes the canvas to fill the entire ribbon viewport.
* `canvas split`: Divides viewport equally between terminal shell and canvas.
* `canvas wide`: Allocates 66% width to canvas, 33% to terminal.
* `canvas narrow`: Allocates 33% width to canvas, 66% to terminal.

### Lisp Canvas Primitives
* `(canvas-open [name [layout]])`: Opens or switches to a named canvas.
* `(canvas-close [name])`: Closes the named canvas.
* `(canvas-select name)`: Directs subsequent drawing operations to the named canvas.
* `(canvas-window layout [name])`: Configures window layout preset and target canvas name.

### Demos & Plots
* Cellular Automata: `(load "/sd0/demos/ca.lisp")` renders Rule 30 or Rule 90 fractals on the canvas.
* Chaos Dynamics: `(load "/sd0/demos/lorenz.lisp")` plots the 3D Lorenz attractor live.
* CAS Function Plotting: `(plot '(sin x) 'x -3.14 3.14)` renders mathematical functions directly to the persistent `"plot"` canvas ([CAS manual](cas.md)).

---

## 4. Backlight & Power Management

* `lcd backlight <0-100>`: Sets panel LEDC PWM brightness (0 turns backlight completely off).
* `lcd blank [min|Ns|off]`: Sets idle inactivity timeout before backlight turns off. Pressing any key or touching the screen wakes the display immediately.
* `lcd reinit video|panel|phy|link` (ESP32-P4 panels): Redoes part of the panel's bring-up without a reboot: `video` restarts the pixel stream, `panel` also resets the panel and sends its init commands, `phy` restarts the MIPI-DSI link (PHY, clocks, timing) and leaves the panel alone, `link` does both. For a panel that stays black while everything reports normal.
* `screenshot [path]`: Saves an uncompressed PBM image of the active display buffer directly to the filesystem.
