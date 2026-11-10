# LugalOS Window Ribbon & Terminal System

[Documentation](../README.md) > [User Manual](index.md) > **Window Ribbon & Terminal System**

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

### Stacks: windows one above the other (landscape)
Across a landscape screen the ribbon is a row of **columns**, and a column holds one window or a **stack** of two or three, one above the other (as in the Niri window manager). The windows of a stack share the column's width; its rows are shared equally unless one has a height set.

* **`Cmd+[` / `Cmd+]`**: the focused window, alone in its column, joins the column on its left / right, at its bottom. A window in a stack leaves it, for a column of its own on that side.
* **`Cmd+Up` / `Cmd+Down`** focus the window above / below in the stack; with `Ctrl` they move the focused window up / down in it. A vertical swipe that starts on a title bar does the same as `Cmd+Up` / `Cmd+Down`.
* **`Cmd+Left` / `Cmd+Right`** go to the next column, at the window last focused in it; with `Ctrl` they move the whole column.
* **Widths** (`Cmd+R`, `Cmd+-`, `Cmd+=`, `Cmd+F`) apply to the whole column.
* **Heights**: `Cmd+Shift+R` gives the focused window the next of 1/3, 1/2 and 2/3 of the column, round; `Cmd+Shift+-` / `Cmd+Shift+=` the next smaller / larger. The other windows share the rest. Joining or leaving a stack shares it equally again.
* **How deep:** as many windows as get at least 4 text rows each, at most 3: three on the RP2350-LCD-7 (7-8 rows each), the LCD-7B and the Tab5 in landscape (5-6 rows each).
* **The on-screen keyboard** gives the focused window its whole column while it is up; the others in the stack come back when it goes.
* **Turning to portrait** lays a stack out window by window down the ribbon (a stack of two is a top/bottom pair); turning back restores it. The minimap shows a stack as one block, split.
* `ribbon list` shows each stacked window's place: `stack: 2 of 2, 13 rows, y 244..471`.

### Full screen
**`Cmd+Shift+F`** gives the focused window -- a terminal, a canvas, a reader, the launcher, any view -- the whole screen, with no menu bar, title bar or other window; a terminal gets the screen's full text grid (100 x 30 on the RP2350-LCD-7). Full screen follows the focus: going to another window (`Cmd+Left` / `Right`, a swipe from a side edge, a minimap tap) shows the ribbon again, and coming back covers the screen again.

The ways out:
* **Keyboard**: `Cmd+Shift+F` again.
* **Touch**: a swipe down from the top edge leaves full screen.
* **Mouse**: point at the screen's top edge. The menu bar and the window's title bar come over it (for 4 seconds, longer while the pointer stays on them); a click on the title bar leaves full screen, its close box closes the window.
* Going full screen shows a hint for 2.5 seconds naming the way out for the board's input: the swipe where there is touch, the pointer where there is a mouse, the key otherwise.

`Cmd+F` (maximize) is different: the column at the full width, its title bar and the menu bar kept.

### Portrait: the ribbon runs down the screen (Tab5)
On a screen turned to portrait (the Tab5, `Cmd+O` or the IMU), the ribbon runs from top to bottom instead: every window is as wide as the screen, and its size is counted in text rows. A window is either alone on the screen (37 rows on the Tab5) or one of a **top/bottom pair** (17 rows each) -- never a narrow slice side by side.

* **Turning keeps the windows**: their order, the focus, and their shape. A window alone stays alone; two side by side become the two of a top/bottom pair, and turning back makes them a side-by-side pair again.
* **`Cmd+F`** switches the focused window between alone and half of a pair; `Cmd+R`, `Cmd+-` and `Cmd+=` do the same in one step each.
* **`Cmd+Up` / `Cmd+Down`** focus the window above / below (`Cmd+Left` / `Cmd+Right` still work, as previous / next); with `Ctrl` they move the focused window up / down.
* **Touch**: a vertical swipe that starts on a title bar (or the menu bar, or within 3 mm of the screen's edge) moves to the window above or below. A vertical swipe inside a window scrolls it, as in landscape.
* A window shows only when the whole of it is on the screen; the ribbon jumps to the next window rather than sliding.
* The 7B and the RP2350-LCD-7 have no portrait ribbon; on the RP2350 it is not built.

---

## 2. Navigation & Window Controls

### Window Controls & Macintosh Close Box
Every non-root window on the ribbon (terminals, canvases, and readers) features a classic Macintosh-style close box (`[X]`) at the left of its title bar:
* **Tap / Click Close Box**: Immediately closes the window, reclaims its backing store and layout slot, and smoothly reflows the ribbon.
* **Zoom box** (the box at the title bar's right end): a tap or click is `Cmd+F` -- the column at the full width and back; down a portrait screen, the window alone and back to one of a pair.
* **Title-bar menu**: a long press or a right click on a title bar opens a menu under it, as a Macintosh pull-down: the widths (2/5, 1/2, 2/3, Full; down a portrait screen Alone and One of a Pair), in a stack the heights (1/3, 1/2, 2/3), Move Left / Right (Up / Down), Join Left / Right or, in a stack, Leave Stack Left / Right, Full Screen, Close. The window's width and height now are ticked, what cannot apply is dimmed (Close on the launcher, Move Right on the last column), and each item shows its key. A tap or click on an item does it; anywhere else closes the menu, and that tap goes nowhere else. With a mouse the item under the pointer is highlighted.
* **Maximize (`Cmd+F`)**: Toggles the active window between the full ribbon width and half of it.
* **Close Window (`Super+W` / `Cmd+W`)**: Closes the active non-root window from keyboard.
* **A program's window** (a game, chess): the close box and `Cmd+W` ask the program to close it, and it closes it itself -- ending, or saving first. A program that has not read the request after 200 ms -- or read it and still has its window 5 seconds later -- gets an alert over the launcher, with a bomb: "*Ceres* is not responding." A game shutting its sound down takes longer than 200 ms; having read the request, it is answering.
  * **Wait** (the default; `Enter` or `Esc`): the window stays, and the alert comes back after 5 seconds if the program still has not answered. It goes away by itself as soon as the program closes its window.
  * **Close Window**: the window goes at once. The program keeps running -- a program stuck in the kernel cannot be stopped from outside safely -- and keeps drawing into a canvas no one sees, freed when the program lets go of it or ends. Meanwhile the program cannot be started again: it says it is in use.
  * A window no program reads (a canvas opened with `canvas open`, or one whose program has ended) closes at once, as before.

### Keyboard Shortcuts
`Cmd` is the keyboard's Super / Windows key. These keys belong to the window manager whatever has the focus; programs keep to the agreed ones (`Cmd+A`, `C`, `V`, `X`, `Z`, `S`, `Q`, `G`). A serial terminal sends them as CSI-u (`ESC [ code ; 9 u`, `10` with Shift, `13` with Ctrl) or, for the arrows, as xterm's `ESC [ 1 ; 9 X`.

| Hotkey | Function |
| :--- | :--- |
| `Cmd+Left` / `Cmd+Right` | Focus the window left / right |
| `Cmd+Up` / `Cmd+Down` | Focus the window above / below (in a stack, or down a portrait screen) |
| `Cmd+Ctrl+Left` / `Right` | Move the focused window's column |
| `Cmd+Ctrl+Up` / `Down` | Move the focused window up / down (in its stack, or down a portrait screen) |
| `Cmd+[` / `Cmd+]` | Join the column on the left / right, or leave the stack to that side |
| `Cmd+R` | The next width (2/5, 1/2, 2/3, full, then round again; portrait: alone, pair) |
| `Cmd+-` / `Cmd+=` | The next narrower / wider width |
| `Cmd+Shift+R` | In a stack: the next height (1/3, 1/2, 2/3, round) |
| `Cmd+Shift+-` / `Cmd+Shift+=` | In a stack: the next lower / taller height |
| `Cmd+F` | Maximize: the full width and back |
| `Cmd+Shift+F` | Full screen and back (any window) |
| `Cmd+Enter` | New terminal |
| `Cmd+O` | Turn the display between portrait and landscape (Tab5) |
| `Cmd+W` | Close the focused non-root window |
| `Cmd+1`..`Cmd+9` | Jump to window 1..9 |
| `Cmd+Shift+3` | Screenshot (PBM) to `/sd0/screenshots/` |

### Touch Gestures (GT911 / ST7121 Capacitive Touch)
* **Tap Window**: Tapping anywhere inside a window pane transfers focus to it.
* **Tap System Title (`LugalOS`)**: Tapping the top-left `LugalOS` title in the menu bar opens or focuses the Touch Launcher window.
* **Tap Close Box**: Tapping the close box (`[X]`) in the top-left title bar closes the window.
* **Tap Minimap**: Tapping a window block in the top minimap immediately scrolls the ribbon to that window.
* **Swipe Left / Right**: Swiping sideways with a finger smoothly scrolls the ribbon.
* **Long Press**: Holding a finger still for half a second opens the context menu of what is under it: on a document in the launcher, its action sheet (Open, Open in Editor or Open in Reader, Cancel).

### Mouse (USB; the P4 panels, the RP2350-LCD-7, the VisionFive 2; `ms` on every screen)
A USB mouse works beside the keyboard, on its own or behind a hub, and beside touch: a finger hides the arrow until the mouse moves again, and unplugging the mouse hides it. Wireless receivers that carry both a keyboard and a mouse work too. `kbd` lists the mouse and how its reports are laid out; `cursor` says where the arrow is, `cursor show|hide` shows or hides it, `cursor speed PCT` (25..400 %, the `input.pointer_speed` setting) sets how far it goes for a move. On a board without touch (an RP2350-LCD-7 without its panel, the VisionFive 2) controls are clicked where they are drawn; where there is touch their grown touch areas stay.
* **Hover**: The control under the pointer is outlined; nothing needs to be pressed.
* **Left Click**: The same as a tap: it focuses the window, presses buttons, and works the close box, the minimap and the menu bar. Dragging with the left button held scrolls a list or a document; sliders follow it.
* **Right Click**: The context menu, as a long press.
* **Wheel**: Scrolls the window under the pointer, even when another window has the focus (the reader by three lines a notch). A sideways wheel moves along the ribbon a window at a time. Scrolling is natural by default, as on a Macintosh: the content moves the way the wheel turns; `settings set input.natural_scroll off` turns it round.
* **Games**: A game that takes the pointer (moonbase) gets the mouse's moves, buttons and wheel itself while it has the focus.
* **Scrollbars**: the launcher's document scrollbar can be pressed or dragged, by the mouse or a finger: the list goes there.

### Touch UX Mode & Launcher (`RIBBON_WIN_LAUNCHER`)
On touch-enabled terminal boards (such as the M5Stack Tab5 and Waveshare ESP32-P4-LCD-7B), LugalOS supports a **Touch UX Mode** powered by the classic Macintosh micro-toolkit (`ugui`):
* **HiDPI Window Chrome (`CHROME_SCALE`)**: On high-DPI displays (`FONT_CELL_W == 16` on Tab5), the menu bar, window title bars, racing stripes, close boxes, and status icons automatically scale by $2\times$ (`CHROME_SCALE = 2`) for ergonomic finger touch targets.
* **Touch Launcher (`launcher [open|close|status]`)**: A dedicated Ribbon window providing touch-friendly tabs:
  * **Apps**: Icon cards to launch **Reader**, **Moonbase Lambda**, and **New Shell**.
  * **Library**: Scrollable list of local documentation and handbooks (`/sd0/help/*.md`, `/sd0/pub/help/*.gmi`, `/sd0/docs/*.md`) that open directly in the Reader window.
  * **Settings**: Touch steppers and toggles for audio **Volume** (`0-100%`), panel **Brightness** (`10-100%`), display **Orientation** (`Landscape` / `Portrait`), and IMU **Auto-Rotate** (`ON` / `OFF`).
* **Press feedback**: a launcher control, a reader action-bar button, a chess button or an on-screen keyboard key shows inverted while a finger is on it; a character key also shows its label enlarged above it. Holding Delete or an arrow key repeats it: after half a second, every 80 ms, until the finger lifts or slides off. A swipe or drag that starts on a control lets it go.
* **Text fields**: a text field that gets the focus by touch, with no USB keyboard attached, brings the on-screen keyboard up under its window (the window's content gets shorter to make room); its keys go to the field (and on a terminal, to the terminal it is drawn in), and the keyboard goes away when the field loses the focus. Cmd+C, Cmd+X and Cmd+V copy, cut and paste through the system clipboard (the one the line editor, `e`, Lisp and `/dev/clipboard` share), Cmd+A selects all; a long press or a right click on the field opens Cut, Copy, Paste and Select All. `launcher gallery`'s text tab has one to try.
* **Touch targets**: every control is touched over at least 7 mm square (47 px on the LCD-7B, 81 px on the Tab5), centred on it, even where it is drawn smaller; where two such areas meet, the control drawn nearer the finger has it. `launcher status` lists each control's touch area. What is drawn under the finger wins: a link just below the reader's action bar stays the link's. On the on-screen keyboard, a tap between keys or in its margin goes to the nearest key.
* **Key click**: `settings set ui.click_sound on` makes each on-screen keyboard key click (ESP32-P4 boards; off by default). A game playing sound keeps it: the keys are silent meanwhile. `audiotest click` tries it.
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

**Navigation:** [<- User Manual](index.md) | [Documentation Index](../README.md)
