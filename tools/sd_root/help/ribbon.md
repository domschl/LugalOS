# LugalOS Window Ribbon & Terminal System

[Documentation](index.md) > [User Manual](user.md) > **Window Ribbon & Terminal System**

---

LugalOS features a native **Horizontal Tiling Window Ribbon** designed specifically for widescreen displays (such as the 7" 800x480 Waveshare RP2350-LCD-7 and the 7" 1024x600 Waveshare ESP32-P4-LCD-7B).

Instead of overlapping windows or rigid vertical splits, LugalOS tiles windows side-by-side along an infinite horizontal ribbon. Users and applications can seamlessly navigate between terminals, graphical canvases, and application panes.

---

## 1. Ribbon Layout & Screen Geometry

```
+------------------------------------------------------------------------+
| [1: lsh]  [2: Lorenz]  [3: Chess]       Minimap: [X X  ]     12:45 UTC | top menu bar
|-------------------------------+----------------------------------------+
| lsh> cat /proc/meminfo        |     * * * * *                          |
| mem: 8192 pages total         |   *           *                        | active / focused
| free: 6420 pages              |  *    CANVAS   *                       | windows on ribbon
| lsh> _                        |   *           *                        |
|                               |     * * * * *                          |
\-------------------------------+----------------------------------------+
```

The top of the screen displays a persistent status bar featuring:
* **Window Tabs**: Numbered indicators for active terminals and canvases.
* **Minimap**: Visual representation of the full ribbon layout.
* **System Clock**: Hardware RTC time synchronized at boot.

---

## 2. Navigation & Window Controls

### Window Controls & Macintosh Close Box
Every non-root window on the ribbon (terminals, canvases, and readers) features a classic Macintosh-style close box (`[X]`) at the left of its title bar:
* **Tap / Click Close Box**: Immediately closes the window, reclaims its backing store and layout slot, and smoothly reflows the ribbon.
* **Toggle Maximize (`Super+\` / `Cmd+\`)**: Toggles the active window between full-screen ribbon width and half-screen width.
* **Close Window (`Super+W` / `Cmd+W`)**: Closes the active non-root window from keyboard.
* **A program's window** (a game, chess): the close box and `Cmd+W` ask the program to close it, and it closes it itself -- ending, or saving first. A program that has not read the request after 200 ms -- or read it and still has its window 5 seconds later -- gets an alert over the launcher, with a bomb: "*Ceres* is not responding." A game shutting its sound down takes longer than 200 ms; having read the request, it is answering.
  * **Wait** (the default; `Enter` or `Esc`): the window stays, and the alert comes back after 5 seconds if the program still has not answered. It goes away by itself as soon as the program closes its window.
  * **Close Window**: the window goes at once. The program keeps running -- a program stuck in the kernel cannot be stopped from outside safely -- and keeps drawing into a canvas no one sees, freed when the program lets go of it or ends. Meanwhile the program cannot be started again: it says it is in use.
  * A window no program reads (a canvas opened with `canvas open`, or one whose program has ended) closes at once, as before.

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

### Touch Gestures (GT911 / ST7121 Capacitive Touch)
* **Tap Window**: Tapping anywhere inside a window pane transfers focus to it.
* **Tap System Title (`LugalOS`)**: Tapping the top-left `LugalOS` title in the menu bar opens or focuses the Touch Launcher window.
* **Tap Close Box**: Tapping the close box (`[X]`) in the top-left title bar closes the window.
* **Tap Minimap**: Tapping a window block in the top minimap immediately scrolls the ribbon to that window.
* **Swipe Left / Right**: Swiping sideways with a finger smoothly scrolls the ribbon.

### Touch UX Mode & Launcher (`RIBBON_WIN_LAUNCHER`)
On touch-enabled terminal boards (such as the M5Stack Tab5 and Waveshare ESP32-P4-LCD-7B), LugalOS supports a **Touch UX Mode** powered by the classic Macintosh micro-toolkit (`ugui`):
* **HiDPI Window Chrome (`CHROME_SCALE`)**: On high-DPI displays (`FONT_CELL_W == 16` on Tab5), the menu bar, window title bars, racing stripes, close boxes, and status icons automatically scale by $2\times$ (`CHROME_SCALE = 2`) for ergonomic finger touch targets.
* **Touch Launcher (`launcher [open|close|status]`)**: A dedicated Ribbon window providing touch-friendly tabs:
  * **Apps**: Icon cards to launch **Reader**, **Moonbase Lambda**, and **New Shell**.
  * **Library**: Scrollable list of local documentation and handbooks (`/sd0/help/*.md`, `/sd0/pub/help/*.gmi`, `/sd0/docs/*.md`) that open directly in the Reader window.
  * **Settings**: Touch steppers and toggles for audio **Volume** (`0-100%`), panel **Brightness** (`10-100%`), display **Orientation** (`Landscape` / `Portrait`), and IMU **Auto-Rotate** (`ON` / `OFF`).
* **Press feedback**: a launcher control, a reader action-bar button, a chess button or an on-screen keyboard key shows inverted while a finger is on it; a character key also shows its label enlarged above it. Holding Delete or an arrow key repeats it: after half a second, every 80 ms, until the finger lifts or slides off. A swipe or drag that starts on a control lets it go.
* **Touch targets**: every control is touched over at least 7 mm square (47 px on the LCD-7B, 81 px on the Tab5), centred on it, even where it is drawn smaller; where two such areas meet, the control drawn nearer the finger has it. `launcher status` lists each control's touch area. What is drawn under the finger wins: a link just below the reader's action bar stays the link's. On the on-screen keyboard, a tap between keys or in its margin goes to the nearest key.
* **Dimmed controls**: a control that cannot act right now is drawn dimmed (gray, flat) -- the reader's Back with no history and Top at the top, a stepper's - or + at its limit, chess's Back / Fwd at either end of the game. It shows no press feedback, the keyboard focus skips it, and a tap on it does nothing. Toggles name what they will do (the reader's Contents reads Close while the contents are shown) instead of staying inverted: inverted means pressed.
* **Launcher document list**: dragging a finger over the launcher scrolls the list with it, pixel by pixel; lifting while still moving glides it on a little.
* **Launcher by keyboard**: Tab / Shift-Tab and the arrows move a dotted focus ring between the launcher's controls; Enter or Space acts as a tap on the focused one would; Up / Down walk the document list (scrolling it), PgUp / PgDn page it, `r` rescans it. The ring appears with the first key and hides again at the next touch.
* **Reader Touch Sub-Header**: Every Reader window renders a fixed top action bar (`[< Back]`, `[Contents]`, `[Rotate]`, `[Top]`) for keyboard-free navigation.
* **Automatic Keyboard Hot-Plug Detection**: If no USB keyboard is attached ~1.5 seconds after boot (or when a USB keyboard is unplugged), LugalOS automatically opens and focuses the Touch Launcher window. Plugging a keyboard back in leaves open windows undisturbed.

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

---

**Navigation:** [<- User Manual](user.md) | [Documentation Index](index.md)
