# LugalOS UI Framework: Views, Window Events, Input and `ugui`

The ribbon (`drivers/screen.c`, `drivers/ribbon.c`) shows three kinds of window: **terminals**, **canvases** (a program's pixels: chess, moonbase, a Lisp plot) and **views** (the kernel's own windows: the reader, the launcher). This page is how a view is written, what a window is told, where input goes, and the toolkit (`ugui`) views and programs draw with. The user's side is [the ribbon manual](../../user/ribbon.md).

The contracts are the header comments of `drivers/include/drivers/view.h`, `kernel/include/kernel/event.h`, `kernel/include/kernel/console.h` and `drivers/include/drivers/ugui.h`. This page is the overview.

---

## 1. Views

A view is a window type number, a table of operations and a file of its own. The screen and the P4's compositor handle every view's window alike: they draw, size, free and hit-test it, and take its pixels through its ops. `drivers/viewtest.c` (`viewtest`, dev builds) is the smallest one, and the test suite checks with it that a new view needs no line in `drivers/screen.c` or `drivers/dsi_esp32p4.c`.

```c
typedef struct {
    const char *name;
    bool (*active)(uint8_t inst);                    /* that instance open */
    void (*event)(uint8_t inst, const win_event_t *ev);
    bool (*captures)(uint8_t inst, int x, int y);     /* a finger here is the view's: NULL never */
    void (*resize)(uint8_t inst, int w, int h);       /* the content is w x h now */
    void (*close)(uint8_t inst);                      /* its window went */
    uint8_t *(*surface)(uint8_t inst, uint32_t *stride, int *w, int *h);  /* P4: its GRAY8 surface, or NULL */
    uint32_t (*seq)(uint8_t inst);                    /* moves at each drawing */
} view_ops_t;
```

**Adding a view:**
1. A type number in `ribbon_win_type_t` (`drivers/include/drivers/ribbon.h`). Every type from `RIBBON_WIN_READER` up is a view (`ribbon_is_view()`).
2. Its ops, and their entry in `view_for_window()` (`drivers/view.c`).
3. Its window: screen request `'v' type flags title` opens it (`SCREEN_VIEW_SINGLETON`: focus the type's window if there is one). The reply gives the backing slot (`inst`) and the content size. The view draws into the slot's 1-bpp backing (`lcdterm_get_reader_backing()`, `ramscreen_get_reader_backing()` on QEMU's RAM screen) and, on the P4, may keep a GRAY8 surface the compositor reads directly; request `'W' slot` shows the backing in its window.

**One task draws every view** (the views task, `view_start()`). Events, resizes and closes are queued for it in order (`view_post()`); the shell's commands on views run on it and are waited for (`view_call()`, `view_sync()`). A view's state therefore has one owner (`single task: views`) and needs no lock of its own against a second drawer. Boards without a panel have no task; the calls run inline there. A view's animation (a fling) stops when something is queued (`view_pending()`).

**Closing.** A view's close box, `Cmd+W` or its own `q` closes the window; the console calls its `close()` op. A canvas window whose program reads its events is asked instead (`WIN_EV_CLOSE_REQUEST`); a program that does not answer in 200 ms gets the not-responding alert (`drivers/winclose.c`).

---

## 2. What a Window Is Told

Every window gets `win_event_t`s (`kernel/include/kernel/event.h`): a view through its `event()` op, a canvas program from its window's ring (`window_poll()`; a ring per window, in PSRAM, panel boards only). Coordinates are the window's content coordinates.

| Event | Meaning |
| :--- | :--- |
| `POINTER_DOWN` / `POINTER_UP` with `WIN_EVF_PRESS` | a finger went down / lifted or moved off: show the control under it pressed, then let go |
| `POINTER_UP` with `WIN_EVF_TAP` | a tap or a left click at (x, y): act. `button` 2: a right click or a long press, the context menu |
| `POINTER_MOVE` | a captured finger's point; `button` 0: the mouse hovering (-1, -1: it left) |
| `DRAG`, `DRAG_END` | a vertical drag (dy since the last), and its end (dy over its last `key` ms: a fling when fast) |
| `SCROLL` | a vertical swipe; with `WIN_EVF_WHEEL` a mouse wheel, dy in notches |
| `KEY_DOWN` | a decoded key (`key`, `mods`, `kernel/keyseq.h`); the system's Cmd shortcuts are taken out before |
| `FOCUS_GAINED`, `FOCUS_LOST`, `SUSPEND`, `RESUME` | the keyboard focus; off the screen (scrolled away, the panel blanked) and back |
| `RESIZE` | the content is w x h now (after `ORIENT`, a new width) |
| `CLOSE_REQUEST` | the close box or `Cmd+W`, for a program that closes its window itself |
| `INPUT_MODE` | what is attached, `key` a mask of `WIN_INPUT_KEYBOARD`, `_POINTER`, `_TOUCH` |
| `ORIENT`, `POWER` | the display turned (`key` its rotation); the battery's charge at each 10 % step (`WIN_POWER_CHARGING`, `_LOW`) |
| `TILT` | with `WIN_CLAIM_TILT`: gravity in the screen's frame (mg), at most 10 Hz while focused |

A new window is told the system's state when it appears, so a program knows the input mode, orientation and power from the start. State events and close requests are never dropped from a full ring; the oldest event goes instead.

**Claims** (`window_claim()`): `WIN_CLAIM_POINTER` gives a canvas the raw pointer stream and no gestures while it has the focus (moonbase); from a mouse its events carry `WIN_EVF_MOUSE`, every button and the wheel. Edge swipes (within 3 mm of the screen's side) stay the window manager's even then, so a full-screen game can always be left. `eventtest` (dev builds) shows what a window is told.

---

## 3. Input

```
touch (GT911, ST7123) --contacts--> gesture recogniser (mm thresholds) --+
USB mouse / xHCI mouse --motion--> kernel/pointer.c --console_pointer()--+--> console --> the window's events
USB / UART keyboard --bytes--> keyseq --------------------------------+
attach / detach, rotation, battery -------------------------------------+
```

- **Touch** (`drivers/gesture.c`): a tap moves under 5 mm, a swipe crosses 10 mm, a long press is held 500 ms; the board's DPI makes millimetres pixels. A mostly vertical stroke is a drag, followed by the focused view. `ts tap X Y`, `ts swipe DIR` inject.
- **Pointer** (`console_pointer(x, y, buttons)`, `console_wheel(dx, dy)`): absolute screen pixels and `WIN_BUTTON_*`. Hover goes to the view under the pointer; the left button is a finger (a click when released within 6 px, else a drag); the right button's release is the context menu; the vertical wheel scrolls the window *under* the pointer, the horizontal one moves along the ribbon. `input.natural_scroll` turns both wheel axes round. Drivers feed relative motion to `kernel/pointer.c` (`pointer_relative()`: `input.pointer_speed`, clamped to the screen in its orientation). `ms move|click|drag|wheel` inject.
- **Devices**: drivers outside the `usbkbd` stack report theirs with `console_input_present()`; touch is the probe's answer at run time (`touch_found()`), not the build's.
- **The on-screen keyboard** is a service: a terminal gets it in TOUCH mode with no keyboard; a view asks for it for a focused text field (`console_osk_for_view(type, inst, want)`), and its window gets shorter above it while it is out. Its keys come to the view as `KEY_DOWN`, to a terminal as bytes for the terminal it is drawn in.
- **The cursor**: the P4 compositor draws the arrow last in each frame; the RP2350-LCD-7 splices it into its scan-out; the VisionFive 2 draws it in `kernel/cursor.c`. A finger hides it (`console_pointer_hide()`).

---

## 4. `ugui`

`ugui` (`drivers/ugui.c`) is an immediate-mode toolkit: a draw pass draws every widget and registers its hit-box in a `ugui_ctx_t`; input is tested against the boxes of the last pass. Drawing and hit-testing share one rectangle, so they cannot disagree.

```c
ugui_begin(&ctx);
ugui_button(&surf, &ctx, r, "OK", ID_OK, false);
...
ugui_end(&surf, &ctx);                 /* the focus ring and the hover outline */
/* later, a tap at (x, y): */
uint16_t id = ugui_hit_test(&ctx, x, y, &index);
```

- **Surfaces** (`ugui_surface_t`): a 1-bpp canvas, a GRAY8 buffer, or both; `scale` is `CHROME_SCALE` (2 on the Tab5).
- **Feedback and focus**: `ugui_arm()` / `ugui_disarm()` (pressed while a finger is on it), `ugui_hover()` (a mouse: the widget as drawn), `ugui_nav()` (Tab, arrows to the nearest widget that way, Enter and Space activate, Esc back; the ring shows with the first key), `ugui_disable_next()` (dimmed: no feedback, skipped by the focus).
- **Touch targets**: a widget drawn smaller than 7 mm is touched over an area that big (`ugui_target_px()`); what is drawn under the finger wins. None without touch. Sizes in millimetres: `ugui_dpi()`, `ugui_mm_px()` (the DPI from the display backend at run time: the panel's, an HDMI monitor's EDID).
- **Widgets**: button, toolbar button, card (`ugui_card_img()` with an icon), stepper, segmented control, list row, virtual list (`ugui_vlist()`: only the rows that show), scrollbar (draggable with `ugui_add_drag_hit()`), toggle, checkbox, slider, progress, tab bar, path bar, text field, dialog and alert, action sheet, icon grid (`ugui_icon_grid()`), images (`ugui_image4()`: 4-bit gray, dithered on 1-bpp).
- **Text fields** (`ugui_text_key()`): a caret, selection by keys, Cmd+C / X / V / A on the system clipboard (`kernel/clipboard.c`).
- **Layout**: rectangle cuts (`ugui_cut_top()`, `_left()`, ...), so one description fits landscape and portrait.
- `uguiselftest` (dev builds, in the test suite) checks the toolkit; `launcher gallery` shows every widget.

---

## 5. Applications, Icons and Settings

- **Registry** (`kernel/apps.c`): applications (id, title, command line, icon, the window a single-instance one runs in, `APP_*` capabilities) and file associations (extension to application), two sorted `.rodata` tables searched by binary search. `app_for_path()`, `open <path>`, `apps`, `apps check`.
- **Icons** (`drivers/icons.c`): 64 x 64 and 32 x 32 4-bit gray images generated from `assets/icons/*.png` by `tools/gen_icons.py` into `drivers/icons_data.c` (sorted `.rodata`); `icon_find()`, `icon_image()` picks the size. `icons check`.
- **Settings** (`kernel/settings.c`): typed keys in a sorted `.rodata` table with board defaults (`audio.volume`, `display.brightness`, `display.orient`, `input.natural_scroll`, `input.pointer_speed`, `launcher.face`, `ui.click_sound`, `ui.osk_preview`, `ui.text_size`). Setting one applies it at once and saves `/sd0/system/SETTINGS` (or `/flash0`) two seconds after the last change. `settings_get()` / `settings_set()`; `settings`, `settings check`.
