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

### Keyboard Shortcuts
| Hotkey | Serial ANSI Sequence | Function |
| :--- | :--- | :--- |
| `Cmd+Right` | `ESC [ 1 ; 9 C` | Pan ribbon right, focusing next window |
| `Cmd+Left` | `ESC [ 1 ; 9 D` | Pan ribbon left, focusing previous window |
| `Cmd+1`..`Cmd+9` | `ESC [ 49 ; 9 u` | Jump directly to window index 1..9 |
| `Cmd+Shift+3` | `Super+Shift+3` | Capture full-screen PBM screenshot to `/sd0/screenshots/` |

### Touch Gestures (GT911 Capacitive Touch)
* **Tap Window**: Tapping anywhere inside a window pane transfers focus to it.
* **Tap Minimap**: Tapping a window block in the top minimap immediately scrolls the ribbon to that window.
* **Swipe Left / Right**: Swiping sideways with a finger smoothly scrolls the ribbon.

---

## 3. Graphical Canvases & Visualization

Applications can open dedicated 1-bpp or RGB565 graphics canvases alongside interactive terminal shells:

### Managing Canvases from the Shell
* `canvas full`: Maximizes the canvas to fill the entire ribbon viewport.
* `canvas split`: Divides viewport equally between terminal shell and canvas.
* `canvas wide`: Allocates 66% width to canvas, 33% to terminal.
* `canvas narrow`: Allocates 33% width to canvas, 66% to terminal.
* `canvas close`: Closes the graphical canvas pane.

### Demos & Plots
* Cellular Automata: `(load "/sd0/demos/ca.lisp")` renders Rule 30 or Rule 90 fractals on the canvas.
* Chaos Dynamics: `(load "/sd0/demos/lorenz.lisp")` plots the 3D Lorenz attractor live.
* CAS Function Plotting: `(plot '(sin x) 'x -3.14 3.14)` renders mathematical functions directly to the canvas ([CAS manual](doc/user/cas.md)).

---

## 4. Backlight & Power Management

* `lcd backlight <0-100>`: Sets panel LEDC PWM brightness (0 turns backlight completely off).
* `lcd blank [min|Ns|off]`: Sets idle inactivity timeout before backlight turns off. Pressing any key or touching the screen wakes the display immediately.
* `screenshot [path]`: Saves an uncompressed PBM image of the active display buffer directly to the filesystem.
