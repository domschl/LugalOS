# Phase 70 — Touch UX Mode, HiDPI Window Chrome & Micro-Toolkit (`ugui`)

**Status**: Planned (Written 2026-11-04)  
**Target Hardware**:
- **Primary**: M5Stack Tab5 (`esp32p4-tab5`, 5" 720×1280 / 1280×720 MIPI-DSI, ~294 DPI, ST7121 TDDI touch, 32 MB PSRAM)
- **Secondary**: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (`esp32p4-lcd7b`, 7" 1024×600 MIPI-DSI, ~170 DPI, GT911 touch, 32 MB PSRAM)
- **Regression**: Waveshare RP2350-LCD-7 (`rp2350-terminal`, 7" 800×480, 133 DPI, 1-bpp monochrome — must remain pixel-identical)

**Follows**:
- Phase 44 & 47: Horizontal Tiling Window Ribbon (`drivers/ribbon.c`, `drivers/screen.c`)
- Phase 56 & 57: M5Stack Tab5 Dual-Orientation Display & HiDPI Typography (`drivers/dsi_esp32p4.c`, `drivers/font_noto_serif.c`, `drivers/font_noto_mono.c`)
- Phase 61: Restricted Markdown & LaTeX Reader (`drivers/reader_view.c`)
- Phase 62: ESP32-P4 Audio, Battery/INA226 & BMI270 Auto-Orientation (`drivers/audio_esp32p4.c`, `drivers/battery.c`, `kernel/orient.c`)
- Phase 64: Moonbase Lambda (`user/moonbase/`)

---

## 1. Executive Summary & Architectural Vision

Until now, LugalOS's graphical terminal presets (`esp32p4-tab5`, `esp32p4-lcd7b`, `rp2350-terminal`) have been modeled exclusively after a **Lisp Station**: a keyboard-centric workstation where `lsh`, Emacs (`ed`), Lisp, and the CAS are the primary entry points, and touch is merely an auxiliary pointer.

When a portable tablet like the **M5Stack Tab5** is operated standalone on battery without a USB keyboard attached, booting into a `lsh>` text prompt leaves the user stranded in front of a shell they cannot type into.

Phase 70 introduces a **second, independent UX persona — Touch Mode** — that coexists seamlessly with the horizontal Window Ribbon:
1. **Classic Macintosh Aesthetics at Native DPI (`CHROME_SCALE`)**:
   - In Phase 57, terminal and reader body fonts were scaled $2\times$ (`16×32` cells) for the Tab5's 294 DPI screen, but the **menu bar, window titlebars, Macintosh close boxes, status icons, and ribbon minimap** remained hardcoded to 133 DPI ($8 \times 16$) pixel dimensions—rendering $2\times$ too small on Tab5 and making touch targets (`11×11` px close boxes) nearly impossible to tap reliably with a finger.
   - Phase 70 introduces `CHROME_SCALE` (`2` on HiDPI `FONT_CELL_W == 16`, `1` on standard density), scaling all classic Macintosh window decorations, menu bars, close boxes, zebra stripes, and minimap blocks cleanly while keeping `rp2350-terminal` 100% pixel-identical.
2. **Stateless Classic-Mac Micro-Toolkit (`ugui`)**:
   - A compact, freestanding, immediate-mode / declarative layout and widget toolkit (`drivers/ugui.c`, `drivers/include/drivers/ugui.h`) built directly on LugalOS's 1-bpp (`canvas1_t`) and anti-aliased 8-bpp (`GRAY8`) / 16-bpp (`RGB565`) surfaces.
   - Standardizes window decorations, action bars, buttons, cards, segmented toggles, steppers/sliders, icons, and scrollable lists with zero heap allocation and automatic hit-box registration.
3. **Touch Launcher (`RIBBON_WIN_LAUNCHER`) & Keyboard Hot-Plug**:
   - If the system boots with no USB keyboard connected (or when a USB keyboard is unplugged at runtime), LugalOS automatically opens and focuses the **Launcher** window on the Ribbon.
   - The Launcher provides one-tap access to:
     - **Touch-Enabled Applications** (`Reader`, `Moonbase Lambda`, plus visual demos).
     - **Document Library** (curated & dynamically scanned Markdown `.md` and Gemtext `.gmi` manuals/books in `/sd0/help/`, `/sd0/pub/`, and `/flash0/help/`).
     - **Quick Settings & Telemetry** (Audio Volume, Display Brightness, Orientation `Landscape | Portrait | Auto`, Battery SoC / Power status).
   - Applications launched from the Launcher join the horizontal Ribbon; **left and right finger swipes** smoothly pan between the Launcher and open applications.
4. **Full-Screen Game Escape Hatch (`moonbase`) & Reader Touch Bar**:
   - Full-screen games (`moonbase`) remain an explicit exception to standard Ribbon tiling, using a dedicated `[Exit]` / `[Home]` GUI button in their top HUD bar to return cleanly to the Launcher/Ribbon.
   - Standard Ribbon touch apps like `reader` gain a `ugui` touch sub-header (`[< Back]`, `[Contents]`, `[Rotate]`) so every navigation feature works without a keyboard.

---

## 2. Architectural Critique & Design Principles

### 2.1 Why Tab5 Window Decorations Were 2× Too Small (and How We Fix It)

In [`drivers/screen.h`](file:///home/dsc/Source/gith/domschl/lugalos/drivers/include/drivers/screen.h) and [`drivers/screen.c`](file:///home/dsc/Source/gith/domschl/lugalos/drivers/screen.c), the chrome geometry was originally written for the 800×480 RP2350-LCD-7 (`FONT_CELL_W = 8`, `FONT_CELL_H = 16`):
- `SCREEN_MENU_H = 20` (18 px white + 1 px rule, using `font8x16` at `y = 2`).
- `SCREEN_TILE_Y = 22` (window frame top).
- Titlebar height hardcoded to `17` px (`y0 .. y0 + 16`), with an `11×11` px Macintosh close box at `(vx0 + 8, y0 + 3)` and `8×16` title text.
- `SCREEN_TEXT_Y = 40` (`SCREEN_TILE_Y + 17 + 1`), leaving zero room for a scaled titlebar!
- Minimap blocks in `draw_menu()` hardcoded to `y = 4..14` (11 px tall) and `8..22` px wide.
- Status bar icons (`icon_sd`, `icon_wifi`, `icon_p9`) drawn at $1\times$ (`11×12` px).

On the M5Stack Tab5 (`294 DPI`, `FONT_CELL_W = 16`, `FONT_CELL_H = 32`), a `17` px titlebar is physically **1.46 mm tall**, and an `11×11` px close box is **0.95 mm square** — less than atenth of a human fingertip pad (~9 mm).

#### The `CHROME_SCALE` Geometry Table

By defining `CHROME_SCALE` from `FONT_CELL_W` (`1` when `FONT_CELL_W == 8`, `2` when `FONT_CELL_W == 16`), every chrome metric scales by exact integer multiples:

| Chrome Metric | Macro / Symbol | Standard (`S = 1`) (RP2350, LCD-7B) | HiDPI (`S = 2`) (M5Stack Tab5) | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **Scale Factor** | `CHROME_SCALE` | `1` | `2` | `FONT_CELL_W / 8` |
| **Top Menu Bar Height** | `SCREEN_MENU_H` | `20` px | `40` px | Bottom rule at `SCREEN_MENU_H - 1` |
| **Window Tile Top Y** | `SCREEN_TILE_Y` | `22` px | `44` px | `22 * S` |
| **Window Titlebar Height** | `SCREEN_TITLEBAR_H` | `17` px | `34` px | `17 * S`; border at `y0` and `y0 + 17*S - 1` |
| **Text Content Start Y** | `SCREEN_TEXT_Y` | `40` px | `80` px | `40 * S` (`SCREEN_TILE_Y + SCREEN_TITLEBAR_H + 1*S`) |
| **Close Box Visual Size** | `CHROME_CLOSE_BOX` | `11×11` px | `22×22` px | Drawn at `(vx0 + 8*S, y0 + 3*S)` |
| **Close Box Touch Hit Area**| `CHROME_CLOSE_HIT` | `24×20` px | `48×36` px | Forgiving finger target around the close box |
| **Drop Shadow Thickness** | `CHROME_SHADOW_PX` | `1` px | `2` px | Classic Mac right & bottom window shadow |
| **Menu / Titlebar Font** | — | `8×16` (`font8x16`) | `16×32` (`font_noto_mono` / `2×` bold) | Crisp anti-aliased on P4, 1-bpp on RP2350 |
| **Status Bar Icons** | `draw_icon16_scaled`| $1\times$ (`11×12`) | $2\times$ (`22×24`) | Integer $2\times$ pixel-doubled bitmap icons |
| **Minimap Block Height** | — | `y = 4..14` (`11` px) | `y = 8..28` (`21` px) | Tappable window thumbnails in top menu bar |

> [!IMPORTANT]
> **Zero Regression on RP2350-LCD-7**:
> When `CHROME_SCALE == 1`, every single constant evaluates to the exact integer used in Phases 37–66 (`SCREEN_MENU_H = 20`, `SCREEN_TILE_Y = 22`, `SCREEN_TITLEBAR_H = 17`, `SCREEN_TEXT_Y = 40`). `tests/hw/lcdterm_shots.py` on RP2350 remains 100% pixel-identical.

---

### 2.2 The `ugui` Micro-Toolkit Architecture

To ensure a unified Classic Macintosh look-and-feel across the Window Manager, the Touch Launcher, the Document Reader, and future touch utilities without introducing dynamic heap allocations or heavy widget trees:

1. **Dual-Target Surface Abstraction (`ugui_surface_t`)**:
   - Wraps both a 1-bpp `canvas1_t *c1` (for the base screen/backing store and monochrome targets) and an optional 8-bpp `GRAY8` anti-aliased buffer (`uint8_t *aa_fb`, `uint32_t aa_stride`) or `RGB565` buffer.
   - Consolidates the anti-aliased `Noto Serif` (`font_noto_serif`) and `Noto Sans Mono` (`font_noto_mono`) text blitters currently private to `drivers/reader_view.c` (`rdraw_t`) so any `ugui` client (Launcher, Reader, dialogs) shares the same Flash `.rodata` font tables and rendering routines.
2. **Immediate-Mode Declarative Layout + Hit-Box Table (`ugui_ctx_t`)**:
   - Each view holds a lightweight `ugui_ctx_t` (stored in `.bulk_bss` PSRAM on P4) containing:
     - Active viewport dimensions `(w, h)` and `scale` (`CHROME_SCALE`).
     - A fixed-size hit-box table (`ugui_hit_t hits[UGUI_MAX_HITS]`, e.g., 64 entries $\times$ 12 bytes = 768 bytes) cleared at the start of each draw pass (`ugui_begin()`).
   - Whenever a widget (`ugui_button`, `ugui_card`, `ugui_stepper`, `ugui_toggle`, `ugui_list_row`) is drawn during the repaint pass, it automatically appends its `(x0, y0, x1, y1, widget_id, param)` bounding box to `ctx->hits[]`.
   - When a touch tap arrives at `(cx, cy)`, `ugui_hit_test(&ctx, cx, cy, &out_param)` returns the matching `widget_id` in $O(N)$ time with zero coordinate duplication between drawing code and touch-handling code!
3. **Classic Macintosh Visual Vocabulary**:
   - **Window Chrome & Titlebars**: Pinstripe/zebra active bars, double-pixel HiDPI borders, drop shadows, close boxes (`[X]`), and optional **Action Sub-Header Bars** directly beneath the titlebar for window-specific touch buttons.
   - **Buttons (`ugui_button`)**: 1-px/2-px rounded-corner black border, crisp serif/sans label, 2-px drop shadow, and inverted black-fill state when active/selected.
   - **App Cards (`ugui_app_card`)**: Framed tile with a $32\times32$ (or $64\times64$ HiDPI) 1-bit Macintosh-style icon, bold title, and caption.
   - **Steppers / Sliders (`ugui_stepper_bar`)**: Large touch-friendly `[-]` and `[+]` buttons flanking a segmented classic Mac thermometer/pill bar (10 segments, e.g., `0% .. 100%` in `10%` steps, also tappable directly along the bar).
   - **Segmented Pill (`ugui_segmented`)**: 2- or 3-way mutually exclusive selector (e.g., `[ Landscape | Portrait | Auto ]`) with inverted active segment.
   - **Scrollable Document List (`ugui_list_row`)**: Clean horizontal rules, document icon badge, bold title, right-aligned path/size badge, and classic Mac scrollbar track/thumb.

---

### 2.3 Keyboard Hot-Plug & Touch Mode Activation Policy

[`drivers/usbkbd_host.c`](file:///home/dsc/Source/gith/domschl/lugalos/drivers/usbkbd_host.c) already tracks USB keyboard enumeration and detachment (`k->dev[i].addr && k->dev[i].is_kbd`). We expose:
- `bool usbkbd_is_present(void)`: Returns `true` if at least one USB boot keyboard is currently enumerated and active.
- `uint32_t usbkbd_detach_count(void)`: Monotonic counter of keyboard detach events.

**Activation State Machine**:
1. **Boot Time (No Keyboard Connected)**:
   - On terminal presets with touch (`TOUCH_PRESENT`), after USB host initialization and a brief enumeration settle window (~400 ms after `usbkbd_start()`), if `!usbkbd_is_present()`, LugalOS automatically opens and focuses the **Launcher** (`RIBBON_WIN_LAUNCHER`) on the Ribbon.
   - On the Tab5, if the user holds the device in portrait or defaults to tablet mode, the Launcher adapts cleanly to both `1280×720` Landscape and `720×1280` Portrait.
2. **Runtime Keyboard Disconnect**:
   - Monitored by the display/touch housekeeping loop: when `usbkbd_is_present()` transitions from `true` $\rightarrow$ `false`:
     - Ensure `RIBBON_WIN_LAUNCHER` is open on the Ribbon.
     - If the currently focused window is a keyboard-only terminal (`RIBBON_WIN_TERM`), smoothly pan the Ribbon to focus `RIBBON_WIN_LAUNCHER`.
     - If the user is already focused on a touch-enabled window (`RIBBON_WIN_READER`, `RIBBON_WIN_LAUNCHER`, or `moonbase`), leave their active window focused.
3. **Runtime Keyboard Reconnect**:
   - When a USB keyboard is plugged in (`false` $\rightarrow$ `true`), all keyboard shortcuts (`Cmd+Left/Right`, `Cmd+Enter`, typing in `lsh`) work immediately. The Launcher window remains available on the Ribbon (or can be closed via `[X]` / `Cmd+W` or reopened via `launcher` command / top-bar `[Home]` tap).

---

### 2.4 Window Management & Full-Screen Game Exception

1. **Standard Touch Apps on the Ribbon (`Launcher`, `Reader`, Demos)**:
   - The **Launcher** is a first-class Ribbon window (`RIBBON_WIN_LAUNCHER`, or slot 0 of the touch workspace).
   - Tapping a document in the Launcher's Document Library calls `reader_open(path)`, which opens (or focuses) a `RIBBON_WIN_READER` window adjacent to the Launcher and smoothly animates the Ribbon viewport (`screen_animate_ribbon()`) to focus the Reader.
   - **Left / Right Swipes**: Swiping left or right anywhere in the Launcher or Reader pans across the Ribbon between the Launcher, open Reader manuals, and any open canvases/terminals.
   - **Top Menu Bar**: Tapping the `LugalOS` system title on the far left of the top menu bar acts as a universal **"Home"** shortcut that scrolls the Ribbon directly back to the Launcher (or spawns it if closed).
2. **Full-Screen Game Exception (`moonbase`)**:
   - Full-screen games (`SCREEN_LAYOUT_FULLSCREEN`, such as `moonbase`) own the entire screen and use 1-finger touch drag to pan their game world.
   - Per design decision, full-screen games are treated as an explicit exception to Ribbon swipe navigation:
     - `moonbase` adds a crisp **`[Exit]`** button in its top HUD bar (top-right / top-left corner).
     - Tapping `[Exit]` sets `game->running = false`, triggering `moon_platform_shutdown()`, which closes the fullscreen canvas, restores the Ribbon layout, and returns focus directly to the **Launcher** (if in Touch Mode) or calling terminal.
     - Launching `moonbase` from the Launcher signals the background shell/app runner task so the input pump and touch task never block each other.

---

## 3. Implementation Milestones

### Milestone 70.1: HiDPI Window Chrome (`CHROME_SCALE`) & `ugui` Foundation
1. **DPI-Aware Chrome Geometry (`drivers/include/drivers/screen.h`, `drivers/include/drivers/ribbon.h`, `drivers/screen.c`)**:
   - Introduce `CHROME_SCALE` (`(FONT_CELL_W == 16u) ? 2 : 1`).
   - Scale `SCREEN_MENU_H` (`20 * CHROME_SCALE`), `SCREEN_TILE_Y` (`22 * CHROME_SCALE`), `SCREEN_TITLEBAR_H` (`17 * CHROME_SCALE`), and `SCREEN_TEXT_Y` (`40 * CHROME_SCALE`).
   - Update `draw_menu()`, `draw_titlebar()`, `draw_tile()`, `minimap_hit()`, and close-box hit testing in `drivers/screen.c` to scale close boxes (`11×11` $\rightarrow$ `22×22`), zebra stripes, 2-px drop shadows, status bar icons ($2\times$ pixel scale), and minimap blocks on HiDPI panels.
   - Update `drivers/dsi_esp32p4.c` (`term_draw()`) so the top menu bar and window titlebar titles render with crisp anti-aliased `16×32` `font_noto_mono` on HiDPI (`CONFIG_FONT_CELL_H == 32`).
2. **Core `ugui` Micro-Toolkit (`drivers/include/drivers/ugui.h`, `drivers/ugui.c`)**:
   - Extract and unify the anti-aliased `GRAY8` + `canvas1_t` drawing primitives (`ugui_surface_t`: fill, hline, vline, frame, shadow_rect, invert, 1-bpp/2× icon blit, `Noto Serif` proportional text, and `Noto Sans Mono` monospace text).
   - Implement `ugui_ctx_t` hit-box table (`ugui_begin()`, `ugui_add_hit()`, `ugui_hit_test()`) and core Classic Mac widgets:
     - `ugui_toolbar_button()` (compact pill/bevel button for window action bars).
     - `ugui_card()` (application launch card with icon, bold title, and subtitle).
     - `ugui_stepper()` (`[-]` bar `[+]` control for Volume and Brightness).
     - `ugui_segmented()` (multi-option pill selector for Orientation).
     - `ugui_list_item()` & `ugui_scrollbar()` (touch-scrollable document rows).
3. **Verification**:
   - Host/QEMU unit tests (`uguiselftest`) verifying hit-box registration, clipping, and layout math at `S=1` (`800×480`, `1024×600`) and `S=2` (`1280×720`, `720×1280`).
   - Verify on RP2350/QEMU that `S=1` produces zero visual diff.
   - Verify on Tab5 (`S=2`) that the top menu bar, minimap, window titlebars, and close boxes render at full $2\times$ physical size.

---

### Milestone 70.2: Reader Touch Action Bar & Touch Ergonomics
1. **Reader Touch Sub-Header (`drivers/reader_view.c`)**:
   - Use `ugui` to render a Classic Mac action bar at the top of every `RIBBON_WIN_READER` window containing touch buttons:
     - **`[< Back]`**: Navigates back in `r->history[]` (dimmed when `history_depth == 0`).
     - **`[Contents]`**: Toggles the interactive Table of Contents (`r->toc_mode`).
     - **`[Rotate]`** (on rotatable panels like Tab5): Toggles Portrait (`0°`) $\leftrightarrow$ Landscape (`270°`).
     - **`[Top]`**: Scrolls back to `scroll_y = 0` when scrolled down.
   - Register these buttons in the Reader's hit table so `reader_tap(cx, cy)` dispatches them with one tap.
2. **Generous Touch Link Targets**:
   - Expand vertical touch padding on inline and block Markdown/Gemtext links in `reader_view.c` on HiDPI panels (`±6 px` vertical slop) so tapping links and footnotes is effortless.

---

### Milestone 70.3: Touch Launcher (`RIBBON_WIN_LAUNCHER`) — Apps, Documents & Settings
1. **Launcher Window Type & Backing Store (`drivers/launcher_view.c`, `drivers/include/drivers/launcher_view.h`, `drivers/screen.c`)**:
   - Add `RIBBON_WIN_LAUNCHER` (or dedicated Launcher view instance rendered via `ugui` into an anti-aliased `GRAY8` surface in PSRAM, composited identically to `reader_view.c`).
   - Responsive layout that adapts automatically between **Landscape (`1280×720` / `1024×600` two-column split layout)** and **Portrait (`720×1280` stacked vertical layout)**:
     - **Section 1 — Applications**:
       - **Moonbase Lambda** (Isometric Lunar Colony Builder — launches `moonbase`).
       - **Manuals & Reader** (Opens/focuses Reader with System Guide index `/sd0/help/index.md`).
       - **System Terminal (`lsh`)** (Focuses `vterm 0` on the Ribbon for inspection).
     - **Section 2 — Document Library (Scrollable List)**:
       - Dynamically lists `.md` and `.gmi` documents from `/sd0/help/`, `/sd0/pub/`, and `/flash0/help/` (with friendly titles parsed from the `# H1` heading or filename).
       - Supports vertical touch drag/flick scrolling and one-tap opening into `RIBBON_WIN_READER`.
     - **Section 3 — Quick Settings & System Status**:
       - **Volume**: `ugui_stepper` (`0% .. 100%`) wired to `audio_p4_set_volume()` / `audio_p4_get_volume()`, plus a `[Beep]` test button.
       - **Brightness**: `ugui_stepper` (`10% .. 100%`) wired to `dsi_lcd_set_backlight()` / `dsi_lcd_get_backlight()`.
       - **Orientation** (Tab5): `ugui_segmented` (`[ Landscape | Portrait | Auto ]`) wired to `dsi_lcd_set_orientation()` and `orient_set_auto()`.
       - **Battery & Network Status Footer**: Live readout of Battery SoC % / Voltage / Charging state (`battery_read()`), Wi-Fi IP, and SD card status.
2. **Shell & Top-Bar Integration**:
   - Add `launcher` shell command (`launcher [open|close|status]`) to open or focus the Launcher from `lsh` or test scripts.
   - Tapping the left `"LugalOS"` badge in the top menu bar focuses (or opens) the Launcher window from any Ribbon view.

---

### Milestone 70.4: USB Keyboard Hot-Plug Detection & Full-Screen Game Exit
1. **USB Keyboard Presence & Hot-Plug Policy (`drivers/usbkbd_host.c`, `kernel/console.c`)**:
   - Expose `bool usbkbd_is_present(void)` in `drivers/include/drivers/usbkbd.h`.
   - At boot (after USB host port reset/enumeration window) and in the periodic console/touch check:
     - If no keyboard is connected at boot on a touch-enabled display preset, automatically open and focus the Launcher.
     - If a connected keyboard is unplugged at runtime (`usbkbd_is_present()` drops from `true` to `false`), ensure the Launcher is open and, if a terminal window was focused, pan the Ribbon to focus the Launcher.
2. **Asynchronous App Launch Queue & `moonbase` Touch `[Exit]` Button**:
   - Add a clean launch request mailbox (`launcher_request_exec(const char *cmd)`) picked up by the root shell / app runner task so tapping **"Moonbase Lambda"** in the Launcher starts `moonbase_run()` on the shell task without blocking the touch or `lcdflush` tasks.
   - In `user/moonbase/src/moon_game.c` (`render_hud()` and `handle_touch()`), add a touch **`[EXIT]`** button in the top-right HUD bar. Tapping `[EXIT]` cleanly ends the game loop (`game->running = false`), runs `moon_platform_shutdown()`, and restores focus to the Launcher on the Ribbon.

---

### Milestone 70.5: Hardware Verification, Visual Regression & Documentation
1. **Automated & Hardware-in-the-Loop Tests (`tests/hw/test_esp32p4.py`, `tests/runner.py`)**:
   - Run `uguiselftest` in QEMU (`rv32`, `rv64`) via `tests/runner.py`.
   - Extend `tests/hw/test_esp32p4.py` (for `--board tab5` and `--board lcd7b`) to test:
     - Opening the Launcher (`launcher`), verifying its Ribbon slot and screenshot readback.
     - Simulating taps (`console_touch_tap`) on Launcher settings steppers (Volume, Brightness) and document list items (verifying a Reader window opens on the Ribbon).
     - Verifying Reader touch action bar buttons (`[< Back]`, `[Contents]`, `[Rotate]`).
     - Verifying `usbkbd_is_present()` reporting in `kbd`.
   - Run `python3 tools/racescan.py --check` to ensure all new static state in `ugui.c` and `launcher_view.c` carries valid ownership annotations.
2. **Documentation Updates**:
   - Update [`doc/user/ribbon.md`](file:///home/dsc/Source/gith/domschl/lugalos/doc/user/ribbon.md), [`doc/user/reader.md`](file:///home/dsc/Source/gith/domschl/lugalos/doc/user/reader.md), and [`doc/user/shell.md`](file:///home/dsc/Source/gith/domschl/lugalos/doc/user/shell.md) to document Touch Mode, the Launcher, the `launcher` command, and Reader touch controls.
   - Run `python3 tools/gendocs.py --all` and `python3 tools/gendocs.py --check`.

---

## 4. Extension Phases (Future Roadmap)

Once Phase 70 is live and we have collected hands-on UX experience on the Tab5 and LCD-7B, natural follow-on extensions include:
1. **Touch-Enabled Chess (`user/chess`)**:
   - Add touch square picking (tap piece $\rightarrow$ highlight legal destination squares $\rightarrow$ tap target square) and a `ugui` action bar (`[New Game]`, `[Undo]`, `[Hint]`, `[Flip Board]`), adding Chess to the Launcher's Touch Applications grid.
2. **Interactive CAS Plotter & Demos Viewer**:
   - A touch wrapper around named canvases allowing users to launch and step through visual demonstrations (Lorenz attractor, Cellular Automata, Function Plots) with touch zoom/pan controls.
3. **On-Screen Keypad / Mini-Inspector (Optional)**:
   - A compact numeric/symbol pop-up pad in `ugui` for entering parameters (e.g., custom plot ranges or Wi-Fi PINs) without needing a full QWERTY virtual keyboard.

---

## 5. Verification Checklist

- [ ] `ninja -C build/esp32p4-tab5`, `ninja -C build/esp32p4-lcd7b`, and `ninja -C build/rp2350-terminal` build cleanly with zero warnings, zero FPU instructions, and zero `racescan.py` violations.
- [ ] On `esp32p4-tab5`, window decorations (menu bar, window titlebars, Macintosh close boxes, status icons, minimap) render at $2\times$ HiDPI scale (`CHROME_SCALE = 2`) with anti-aliased typography.
- [ ] On `rp2350-terminal`, `CHROME_SCALE = 1` preserves 100% pixel-for-pixel compatibility.
- [ ] Booting `esp32p4-tab5` without a USB keyboard (or unplugging the keyboard) automatically opens and focuses the Touch Launcher on the Ribbon.
- [ ] From the Launcher, the user can adjust Volume, Brightness, and Orientation, open Markdown/Gemtext documents into the Reader, swipe left/right between the Launcher and open Reader windows, and navigate Reader history/TOC via touch buttons.
- [ ] Launching `moonbase` from the Launcher enters the game cleanly, and tapping `[EXIT]` in the `moonbase` HUD returns to the Launcher on the Ribbon.
- [ ] `python3 tests/runner.py`, `python3 tests/hw/test_esp32p4.py --board tab5`, and `python3 tools/gendocs.py --check` pass completely.
