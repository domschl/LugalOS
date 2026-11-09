# Phase 71 — Touch UX Foundation II: Compositor, Input, Toolkit, Views and the Mouse

**Status: DONE (closed 2026-11-09).** Drafted 2026-11-05, after 0.20.0; decisions in §4 confirmed by the owner 2026-11-05; split and renumbered 2026-11-09. Every milestone 71.0-71.9 done; what was measured and decided is in each milestone's status and result.

**Continued in:** phase 76 (long file names, `ugui` for Lisp, the portrait ribbon: `plan/phase76_ux_foundation_lfn_lisp_portrait.md`), phase 77 (the launcher's faces and the Settings app: `plan/phase77_launcher_and_settings.md`), phase 78 (the book reader: `plan/phase78_book_reader.md`). The milestone mapping is at the start of §3.

**Target Hardware**:
- **Primary**: M5Stack Tab5 (`esp32p4-tab5`, 720×1280 native portrait panel, landscape by 270° rotation, `CHROME_SCALE = 2`, ST7123 touch)
- **Secondary**: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (`esp32p4-lcd7b`, 1024×600 native landscape, `CHROME_SCALE = 1`, GT911 touch)
- **Regression**: Waveshare RP2350-LCD-7 (`rp2350-terminal`, 1-bpp): builds, no SRAM growth, pixel-identical unless a milestone says otherwise.

**Follows**: Phase 70 (touch mode, `ugui`, launcher, OSK, chess touch UI), Phase 64 §5.9 (PPA SRM fused 2× + rotation for moonbase), Phase 61 (restricted Markdown reader), Phase 55 (double buffering).

---

## 0. Review of Phase 70

Phase 70 delivered a usable first touch persona: HiDPI chrome (`CHROME_SCALE`), the `ugui` toolkit, a launcher, the on-screen keyboard (OSK), a touch chess UI and the moonbase exit button. The review below separates what is solid from what was marked done but is only partly there, because several Phase 71 items depend on the gaps.

### 0.1 Solid

- `CHROME_SCALE` geometry: a single scale factor, RP2350 constants unchanged.
- `ugui` hit-box registry: drawing and hit-testing share one rectangle, no coordinate duplication (`drivers/ugui.c:23-50`).
- Keyboard hot-plug policy in `shell_idle_poll()` (`kernel/shell.c:8250`): boot without keyboard opens the launcher, unplug reopens it, plug-in hides the OSK.
- Launcher execution goes through a mailbox (`launcher_poll_exec()`), so touch never runs an app on the input path.
- Chess engine buffers moved to PSRAM.

### 0.2 Marked done in the Phase 70 checklist, but partial

| Item | What exists | What is missing |
| :--- | :--- | :--- |
| **70.6 event model** | `win_event_t` and one global SPSC ring in `kernel/console.c:255-285` | The ring is **global, not per window**. It runs next to two older rings (canvas tap ring, canvas key ring) that are fed in parallel. Only moonbase reads it, and moonbase also still installs the global `touch_set_hook()` (`user/moonbase/src/platform_p4.c:263`). Chess reads the old tap ring. No `POINTER_MOVE` reaches in-kernel views. `FOCUS_*`, `CLOSE_REQUEST` and `RESIZE` are declared but never pushed. |
| **70.7 owner reclamation** | `owner_pid` is recorded on window creation | `close_window()` reads `closed_pid` and never uses it (`drivers/screen.c:1942`). No graceful timeout, no claim release. |
| **70.2 rdraw consolidation** | `ugui` has its own surface and text routines | `reader_view.c` still draws through its private `rdraw_*` (41 call sites). Two copies of the same blitters. |
| **70.8 DPI-calibrated touch** | `TAP_PX` is 48 on HiDPI, 24 otherwise | Swipe threshold is still 1/8 of the screen (`SWIPE_DIV`), so 160 px on the Tab5 in landscape and 90 px in portrait. The thresholds are not in millimetres. The 7B (170 DPI, scale 1) gets the 133-DPI targets. |
| **70.5 tests** | `uguiselftest` runs in `tests/hw/test_esp32p4.py` | It is not in `tests/runner.py`/QEMU, so a toolkit regression is only caught with hardware attached. |
| **Launcher keyboard** | `j`/`k`/Space scroll, `r` rescans | No way to *activate* anything by keyboard. No focus. |

**Action:** Phase 70's checklist stays as is (history). Milestone 71.0 closes or re-homes each row, and `plan/open_issues.md` gets an entry for whatever is deferred.

### 0.3 Structural issues found

1. **Special-cased window kinds leak into the compositor.** Every new view adds a `RIBBON_WIN_*` type, a branch in `close_window()`, and a branch in the P4 snapshot (`launcher_get_aa_backing()` is selected by `rids[i] & 0x80u` in `drivers/dsi_esp32p4.c:1677`). Settings and Files would add two more of each. This needs a generic view interface before new apps are added (71.4).
2. **Gestures are classified only on release** (`gesture()` in `drivers/touch_gt911.c:156`). Nothing follows the finger: a vertical swipe scrolls *after* the lift, and there is no pressed state to show. A horizontal swipe **anywhere** is consumed globally as a ribbon move, which makes in-app horizontal gestures (book page turns, sliders, chess drag) impossible without a hook.
3. **Release latency on GT911**: a release is detected 250 ms after the last report (`touch_gt911.c:218`). Every tap on the 7B therefore lands at least 250 ms late, on top of the 20 ms poll. On the Tab5 (ST7123) it is 80 ms.
4. **No input-mode or system events.** Keyboard presence is polled by the shell every 200 ms and acted on there. Apps cannot learn that the keyboard went away, that the device was rotated, or that the battery is low.
5. **Settings are not persisted.** Volume, brightness and orientation go back to their defaults on reboot.
6. **Launcher state is written from the input path** with an `accepted:` race verdict (`drivers/launcher_view.c:129`). With more app views this pattern multiplies. The view framework (71.4) should give every view one owner context.
7. **FAT32 is 8.3-only.** LFN entries are skipped (`fs/fat32.c:1783`). Book titles, shortcuts and the file manager cannot rely on file names for display.
8. **The standard-density serif font is ASCII-only** (`tools/gen_serif_font.py`: `0x20..0x7E`). Noto (Tab5) has Latin-1. German or French books would lose umlauts and accents on the 7B and RP2350.

### 0.4 Why the Tab5 UX is slower than the 7B (root cause)

The games were fixed by rendering into an RGB565 canvas and letting the PPA SRM do the 2× scale and the 90° rotation in one DMA pass, with zero-copy buffer swaps and cache-residency tracking (Phase 64 §5.9: `present` went from 75.8 ms to 0.79 ms). The general UX does not take that path anywhere:

| Cost | Where | 7B (native landscape) | Tab5 (landscape = 270° rotation) |
| :--- | :--- | :--- | :--- |
| Reader/Launcher GRAY8 → frame composite | `term_draw()` step 2b, `dsi_esp32p4.c:1880-1945` | `memcpy` per row | **per pixel**, GRAY8→RGB565 on the CPU, each pixel to a different 1440-byte-strided PSRAM row: one cache-line fill per pixel column |
| 1-bpp shadow conversion under a reader window | step 2 rotated path | converted, then overwritten | converted (8×8 transpose), then **overwritten** by step 2b. The skip list covers terminals only, not readers or the launcher |
| Synchronous redraw per animation step | `screen_animate_ribbon()` (`screen.c:1376`), `reader_scroll_instance()` (`reader_view.c:347`) | cheap | `lcdterm_sync_post()` does a full rotated conversion under `console_lock`: **~170 ms each** (`dsi_esp32p4.c:2384`). A ribbon pan is 5 steps + 1 final, about **1 s**. A reader scroll is 3-5 steps. |
| Reader redraw | `reader_draw()` (`reader_view.c:1230`) | re-reads and re-lays out **the whole file** from the VFS on every draw | same, and it happens per animation step |
| `ugui`/reader double rendering | every widget draws into the 1-bpp canvas **and** the GRAY8 surface (`ugui.c:51-73`, `165-205`) | 2× draw work | 2× draw work |

Two of these costs hit both boards (reader re-parse, double rendering). The 7B hides them because its composite is a `memcpy`.

**Corrected by the 71.0 measurements (§3, 71.0 results):** rotation is *not* the main factor. With the Tab5 in portrait (no rotation) a focus move still takes 0.96 s against 1.03 s in landscape, and the reader composite 56 ms against 70 ms. The Tab5's frame is **RGB565** (needed for its anti-aliased terminal), so a full frame is 1.84 MB of PSRAM writes, about 50 ms at the ~36 MB/s that Phase 64 measured. Every forced frame pays that once for the 1-bpp conversion and again for each reader area on top of it. The 7B's GRAY8 frame is a third of that traffic. The levers are therefore, in order: no forced full frames per animation step, every pixel written once, and bulk copies by DMA (the PPA did moonbase's full 1280×720 2× + rotate in 15.7 ms). The window-surface design below delivers all three.

The fix is architectural, not a micro-optimisation: **window surfaces composited by the PPA, and scroll/pan as composite-only operations**. That is the "fused" approach from the games, generalised. Details in 71.1.

### 0.5 Cross-phase dependencies: the VisionFive 2 workstation (phases 74, 75)

Phases 72-75 port LugalOS to the StarFive VisionFive 2 (`visionfive2`, RV64, S-mode). Two of their stages build on phase 71, and are developed in parallel with it (revised 2026-11-08 against what 71.4 actually built; numbers as renumbered 2026-11-09):

- **Phase 74.5** brings a USB keyboard and a **USB mouse** over the VL805 xHCI controller (`drivers/usb_hid.c`), and 74.2/74.3 a 256 GB NVMe SSD at `/nvme0` (FAT32 through `block_dev_t`, `fs/fat32.c` untouched by phase 74).
- **Phase 75** brings 1080p/720p HDMI (DC8200), a software arrow cursor (75.3, `kernel/cursor.c`) and the ribbon as a desktop on it (75.4).

So the mouse becomes a **third input mode** next to touch and keyboard, the screen gets a resolution and a DPI that are only known at run time (EDID), and FAT32 long names (76.1) are needed on a second, much larger volume. What phase 71 provides, and what it expects in return:

| Area | Phase 71 provides (the contract) | Phases 74/75 provide |
| :--- | :--- | :--- |
| **Input devices** | `WIN_EV_INPUT_MODE` with a **bitmask** of what is attached (`WIN_INPUT_KEYBOARD`, `WIN_INPUT_POINTER`, `WIN_INPUT_TOUCH`), told to every window and view on each change and to each new window (71.4, done). Drivers outside the `usbkbd` stack report their devices with `console_input_present()` (71.4, done). | 74.5 calls `console_input_present(WIN_INPUT_KEYBOARD / WIN_INPUT_POINTER, …)` on attach and detach, and feeds keys through the existing console input path. |
| **Pointer events** | **71.7:** `console_pointer(x, y, buttons)` and `console_wheel(dx, dy)` in screen pixels, absolute; the console turns them into the existing `win_event_t`s (no new `EV_*` names): hover (`POINTER_MOVE` with `button` 0, to the window under the pointer), left press/release/click (`POINTER_DOWN`/`UP` with `WIN_EVF_PRESS`, `WIN_EVF_TAP`: the same as a finger), drag with the button held, **right click = the context menu** (`button` 2, the same action as a touch long press: the action sheet), vertical wheel = `WIN_EV_SCROLL` to the window **under the pointer**, horizontal wheel = ribbon pan. Focus follows a click. Chrome (close box, minimap, menu bar) clicks act as taps. `ms move/click/wheel` shell injection, as `ts` does for touch, so QEMU and the P4 panels test it without a mouse. | 75.3 keeps the cursor position (acceleration, clamping) and draws the cursor; it calls `console_pointer()` / `console_wheel()` with absolute positions and does no routing of its own. |
| **Widgets** | 71.7: `ugui` hover state (the widget under the pointer drawn highlighted), exact hit areas with a mouse (the 7 mm touch growth is for fingers), the focus ring rules unchanged (shown once a key is used). | -- |
| **Display backend** | The ribbon, views, window rings, the close protocol and the system events are compiled where `LCDTERM_PRESENT` is defined (drivers/lcdterm.h). Millimetre metrics (`ugui_metrics_t`, the gesture thresholds) take the DPI from the display backend at run time, with `CONFIG_PANEL_DPI` as the board default (71.6). | 75.2/75.4 implement an `lcdterm` backend for the DC8200 frame (as `dsi_esp32p4.c` is one) and add the VisionFive 2 to `LCDTERM_PRESENT`; the backend reports the DPI from the EDID's physical size. |
| **Long file names** | 76.1 in `fs/fat32.c`, for every FAT volume: `/sd0` and `/nvme0` alike, including the 64-bit build. | 74 mounts `/nvme0` with `fat32_init()` and does not change `fs/fat32.c`; it inherits long names when 76.1 lands. |

Limits phase 75 should know: the ribbon holds `RIBBON_MAX_WINDOWS` = 8 windows; a window's width in columns is a `uint8_t` (a 1920-pixel screen at 8-pixel cells is 240 columns: it fits; 2560 does not); screen replies carry coordinates as 16 bits.

---

## 1. Review of the proposed scope (corrections and additions)

| Proposal | Verdict | Correction / enhancement |
| :--- | :--- | :--- |
| Apply the games' fused graphics path to the UX | **Yes, broadened** | On the Tab5's chip (rev v1.3) the PPA SRM block cannot read GRAY8. That needs rev ≥ 3.0 (`ppa_ll_srm_is_color_mode_supported()` in esp-idf), which the 7B has but the Tab5 does not. So either the UX surfaces become RGB565, or the gray surface goes through the blend engine as A8 first (L8/CLUT is disabled in esp-idf on every revision). Decided: RGB565 (§4). The plan also removes the non-rotation costs (re-parse, double draw, synchronous animation frames), which are half the problem. **Measure first** with a UX bench, as moonbase did. |
| Vertical ribbon in portrait | **Yes, generalised** | Make the ribbon's *major axis* a property chosen by orientation, not a second window manager. Portrait shows one window, or two stacked top/bottom. This requires gesture arbitration, because a vertical swipe is also content scroll (71.2, 76.3). |
| Standard events (mode switch, tilt, …) | **Yes, plus repair** | First finish 70.6 (per-window routing, live pointer moves, close/focus/resize actually emitted, old rings and hooks retired). Then add system events. Tilt is delivered as debounced orientation *intent* plus an opt-in raw tilt stream, never as a high-rate default. |
| Toolkit review and new primitives | **Yes** | The key additions are **keyboard focus** (gives keyboard parity almost for free), **pressed/armed state**, **damage rectangles** (so compositing can be partial), **virtual lists**, text field, dialogs, long-press menus and image blits. List in 71.3. |
| Touch feedback, especially OSK | **Yes** | Feedback needs press events at touch-down, which do not exist today (§0.3.2), and a shorter release latency (§0.3.3). Add key preview on the OSK, armed state on all widgets, and an optional click sound. |
| Keyboard-only usability | **Yes, as a rule** | Make it a checked invariant: every `ugui` widget that registers a hit-box is also focusable, and a host test drives each app by keys only. Add a global launcher hotkey. |
| Settings as its own app | **Yes** | Plus **persistence** (a settings store), which is missing today. |
| App icons (gray-scale) and app↔icon binding | **Yes** | Define an **app registry** (`.rodata`, sorted, `bsearch`, per AGENTS.md) with icon reference, command, capability flags and **file associations**. Define one small 4-bpp gray icon format with 1× and 2× variants. On 1-bpp targets the icons are dithered at build time. |
| Launcher: Shortcuts face + Files face | **Yes** | Shortcuts are a persisted list of typed entries (app, document, directory, book). The file manager does real traversal with a virtual list, open-with by association, and "add to shortcuts". Deleting needs a confirm dialog. Display names come from metadata, not the 8.3 name. |
| epub → reader-markdown converter | **Yes, with constraints** | Host-side Python (`uv`, `pyrefly`, stdlib `zipfile` + `html.parser`, Pillow only for covers). Output is **one directory per book with 8.3 names** plus a catalog for titles. A typographic fold table handles characters outside the target font. Images become alt text in this phase. DRM-protected epubs are skipped with a message. |
| Covers as gray icons | **Yes** | The converter writes the cover in the same icon format as the apps, so the launcher has one code path. |
| Book-sized files | **Yes, needs a new reader core** | Layout index (block → byte offset, y, height), built once per width and cached. Rendering touches only visible blocks. Position is stored as **byte offset**, not pixels, so it survives rotation and font changes. |
| Page forward/back gestures | **Yes** | Tap zones (left/right third) plus horizontal swipe, *claimed* by the reader in book mode so the ribbon does not take it. Page turns without animation by default (e-reader style), which is also the fastest path on the Tab5. |

**Added items not in the original list:** UX benchmark and frame-time overlay (71.0); generic view-window framework (71.4); settings persistence (71.5, 77.1); Latin-1 + typographic punctuation for the standard-density serif font (78.2); last-read position and "continue reading" shortcut (78.3).

---

## 2. Design

### 2.1 Window surfaces and the P4 compositor

**Model.** Each visible view (reader, launcher, settings, files, and later terminals) owns a **surface** in PSRAM in *logical* orientation. The flush task composites the surfaces into `fb_back()`. The view draws only when its content changes. Scrolling and ribbon panning change *where* the surface is read from, not its pixels.

- **Surface format on P4: RGB565** (decision 1 in §4). The options that were weighed:
  - *Option A, RGB565 surfaces*: the PPA SRM path that already exists (`ppa_srm_rgb565()`). Twice the memory of GRAY8 (a full 1280×720 window is 1.8 MB; PSRAM is 32 MB). Text rendering writes 2 bytes per pixel. Opens the door to colour accents later.
  - *Option B, GRAY8 surfaces + PPA blend*: the TRM (chapter 39) lists GRAY for SRM and L4/L8 for blend, but esp-idf enables SRM GRAY8 only on rev ≥ 3.0 and has L8/L4 commented out on all revisions. On the Tab5 (v1.3) the remaining route is to treat the gray surface as **A8** foreground with a fixed white colour over a black background (out = gray). That gives an RGB565 scratch (unrotated), then SRM rotate. Two DMA passes, half the surface memory, keeps the existing GRAY8 font blitters, but needs a new blend-engine driver and a background source. On the 7B (v3.2) SRM could read GRAY8 directly, but the 7B does not rotate, so it does not need it.
  - *Fallback (both)*: a cache-friendly **tiled CPU transpose** (32×32 tiles, written in ascending-address order) instead of today's per-pixel scatter. Used when the PPA is busy or fails, and on the 180° path.
- **Scroll as a source offset.** A scrolling view keeps a surface taller than its viewport (viewport + one page above and below, as a row ring). Scrolling by *d* moves the composite's `src_y` and renders only the newly exposed *d* rows. Ring wrap = two PPA blocks.
- **Pan as a destination offset.** A ribbon animation step recomposites the visible surfaces at shifted `dst_x` and redraws only the chrome (menu bar, title bars, 1-bpp shadow). No view redraws during a pan.
- **Asynchronous animation frames.** Animation steps post frames to the flush task and pace on its VSYNC flip (Phase 55 double buffering). They do not call the synchronous full redraw. Only the *final* frame of a rotation stays synchronous.
- **No hidden work.** The 1-bpp → frame conversion skips the rectangles of every AA/RGB surface, not only terminals. On P4, `ugui` draws into the 1-bpp canvas only when asked (screenshot or `ramscreen` verification); `lcd shot` reads the frame buffer instead.
- **Damage rectangles.** `ugui_end()` returns the union of changed widget rectangles. The view hands it to the compositor, so a stepper tap recomposites 1-2 % of the screen instead of the whole window.
- **Terminals (stretch).** Rendering terminal cells into an RGB565 surface per terminal turns a ribbon pan containing terminals into composite-only as well. Optional, decided after the 71.1 measurements.

**Budget targets (Tab5 landscape, measured by `uxbench`; "today" from the 71.0 baseline):**

| Interaction | Today (estimate from code comments) | Target |
| :--- | :--- | :--- |
| Ribbon focus move / pan | 1031 ms (7 frames of ~126 ms, 6 of them forced) | ≤ 150 ms total, ≥ 30 fps during the move |
| Reader scroll by one line | 856 ms (4 frames of ~137 ms + 322 ms re-parse/draw) | ≤ 16 ms per frame, finger-following |
| Reader page turn (book mode) | 1398 ms (page scroll today: 6 frames + 579 ms app) | ≤ 60 ms tap-to-glass |
| Launcher/settings tap feedback | 140 ms redraw-to-glass (plus touch release latency) | armed state on glass ≤ 50 ms after touch-down |

The 7B must get no slower on any row (its baseline is in the 71.0 results: pan 265 ms, line scroll 332 ms, page 579 ms, launcher 66 ms).

### 2.2 Input pipeline and gesture arbitration

```
touch driver ──raw down/move/up──▶ gesture recogniser ──▶ router ──▶ per-view event ring
USB kbd / UART ──key bytes──▶ keyseq ─────────────────────────┘          │
mouse (75.3) ──console_pointer()/wheel──▶ router (71.7) ───────┤          │
orient / battery / usbkbd ──system events──────────────────────┘          ▼
                                                                    view->event()
```

- **The touch driver reports raw contacts** (down/move/up with timestamps). Classification moves into a recogniser that runs on the input pump. On GT911, a report whose point count is zero ends the contact immediately. The 250 ms timeout stays only as a fallback for missed reports.
- **The recogniser emits** `POINTER_DOWN`, `POINTER_MOVE` (throttled to the frame rate), `POINTER_UP`, `TAP`, `LONG_PRESS` (≥ 500 ms within the tap slop), `SWIPE` (with axis, direction, velocity), `DRAG_START`/`DRAG_END`. Thresholds are in **millimetres**: each board config gets `px_per_mm` (Tab5 ≈ 11.6, 7B ≈ 6.7, RP2350 ≈ 5.2). Tap slop is 3 mm and the swipe minimum is 8 mm.
- **Arbitration (who gets a gesture).** Each view declares claims when it opens: `CLAIM_SCROLL_V`, `CLAIM_SWIPE_H`, `CLAIM_DRAG` (any axis), and can change them at runtime (the reader claims `SWIPE_H` only in book mode). The rules, in order:
  1. A gesture that **starts on chrome** (menu bar, title bar) or within the **edge band** (3 mm from the screen edge along the ribbon's major axis) always goes to the window manager.
  2. Otherwise the view under the finger gets it if it claims that axis.
  3. Unclaimed gestures fall through to the window manager (today's behaviour, so terminals keep it).
  - **Full-screen games** (moonbase, ceres) claim every content gesture, but rule 1 still applies: an **edge swipe** leaves the game for the ribbon. There is no chrome in full screen, so the edge band is the only way out besides the in-game `[Exit]`. The game gets `SUSPEND`: it pauses its simulation and stops rendering, and it stays reachable from the minimap/launcher, where `RESUME` continues it. If keeping the game task alive in the background turns out impractical in 71.2, the edge swipe instead opens the game's own pause menu (Resume / Quit). The edge band is the same in both cases.
- **Routing.** Pointer events go to the view under the contact at `POINTER_DOWN` and stay with it until `UP` (capture). Key events go to the focused view. System events go to all views.
- **Per-view rings** replace the global ring and the canvas tap/key rings. Canvas/user-task apps (chess, moonbase, ceres) read their window's ring through `window_poll_event()`, which gets an explicit window handle. `touch_set_hook()` is deleted once moonbase and ceres are ported.
- **System events** (new `win_event_type_t` values):
  - `WIN_EV_INPUT_MODE`: what is attached, as a bitmask (`WIN_INPUT_KEYBOARD`, `WIN_INPUT_POINTER`, `WIN_INPUT_TOUCH`; revised 2026-11-08 for the VisionFive 2's mouse, §0.5). From `usbkbd` and from drivers that report devices with `console_input_present()`, no longer polled in the shell.
  - `WIN_EV_ORIENT`: orientation changed (after the change). Followed by `RESIZE` with the new size.
  - `WIN_EV_TILT`: opt-in per view (`CLAIM_TILT`), at most 10 Hz, pitch/roll in milli-g from the BMI270. For a future level/game. Never delivered by default.
  - `WIN_EV_POWER`: battery level band changed (100/50/20/10 %), charger attached/detached.
  - `WIN_EV_SUSPEND`/`WIN_EV_RESUME`: the view is fully off-screen or back. Views stop timers while suspended.
  - `WIN_EV_SETTINGS`: a setting a view depends on changed (for example text size).

### 2.3 Portrait: the major-axis ribbon

Moved to phase 76 §1.1 (`plan/phase76_ux_foundation_lfn_lisp_portrait.md`).

### 2.4 `ugui` v2

Additions, in order of need for the apps in this phase:

1. **Keyboard focus.** `ugui_ctx_t` gets `focus_id`/`focus_index` and an ordered focus list built during the draw pass (every hit-box widget is focusable unless it opts out). `ugui_nav(ctx, key)` handles Tab/Shift-Tab, arrows (2-D nearest in the arrow's direction), Enter/Space (activate), Esc (back/cancel). A focus ring is drawn in the Classic Mac style (dotted 1×/2× frame). Focus is visible only in `KBD` input mode, so touch users never see it.
2. **Armed/pressed state.** On `POINTER_DOWN` over a widget, it is *armed* (drawn inverted). Moving out disarms it, moving back re-arms it, and `UP` while armed activates it (Mac semantics). Requires the press events from 2.2.
3. **Damage tracking.** `ugui_end(ctx, &damage)` returns the union of rectangles whose state changed since the previous pass (widget id + state hash per hit-box).
4. **Clip stack.** `ugui_push_clip()`/`ugui_pop_clip()` for scroll containers.
5. **Scroll container** with finger-following drag, momentum (fixed-point decay, no FPU) and a scrollbar. Draws only the visible children.
6. **Virtual list.** `ugui_vlist(ctx, rect, count, row_h, row_cb, user)`: draws only visible rows, from a callback. Needed for directories with hundreds of entries and the book catalog.
7. **Icon grid.** Icon above label, selection, focus, long-press. Used by the Shortcuts face.
8. **Gray image blit.** 4-bpp gray and 1-bpp sources, integer scale. Used for icons and covers (format in 2.6).
9. **Text field.** Single line, caret, selection by keys, OSK integration (OSK opens when the field gets focus in `TOUCH` mode). Used for rename, new shortcut label, search.
10. **Modal dialog / alert / confirm** and **action sheet** (long-press menu). Modal = focus and pointer capture inside the dialog.
11. **Toggle switch, checkbox, slider** (drag-able, with keyboard steps), **progress bar**, **path bar** (breadcrumbs, each segment tappable), **tab bar** (the launcher's two faces).
12. **Layout helpers.** Rect-cut functions (`ugui_cut_top/left/…`, `ugui_grid()`) so one layout description adapts to landscape and portrait.
13. **Metrics.** `ugui_metrics_t` from `px_per_mm`: minimum target 7 mm, spacing, text sizes. Widgets use it instead of raw `scale`, which fixes the 7B's undersized targets.
14. **Single blitter set.** The reader's `rdraw_*` moves onto `ugui_surface_t` (finishes 70.2). The 1-bpp branch becomes optional at runtime (2.1).

`uguiselftest` grows with each item and runs in QEMU through `tests/runner.py`. Layout math also gets a host test.

### 2.5 View framework and app registry

**View interface** (`drivers/include/drivers/view.h`):

```c
typedef struct view_ops {
    const char *name;
    bool  (*open)(view_t *v, const char *arg);        /* arg: path, URL, or NULL */
    void  (*close)(view_t *v);
    void  (*event)(view_t *v, const win_event_t *ev);  /* runs on the view's owner context */
    bool  (*draw)(view_t *v, ugui_surface_t *s, ugui_rect_t *damage);
    uint8_t claims;                                     /* CLAIM_* defaults */
} view_ops_t;
```

- One window type `RIBBON_WIN_VIEW` with a `view_t *` replaces `RIBBON_WIN_READER` and `RIBBON_WIN_LAUNCHER`. `close_window()` and the compositor snapshot handle all views the same way (no `0x80` tagging).
- **Ownership.** Views run their `event`/`draw` on one context (the display service task), never on the input pump. This removes the `accepted:` race on launcher state (§0.3.6) by construction. racescan verdicts become `single task: display service`.
- **Close protocol** (finishes 70.7): `CLOSE_REQUEST` → view or owner task acknowledges → resources freed. A user-task owner that does not acknowledge in 200 ms gets its session claims released and is signalled. `owner_pid` is finally used.

**App registry** (`kernel/apps.c`, `.rodata`, sorted by id, `bsearch`):

```c
typedef struct {
    const char *id;          /* "chess", "files", "moonbase", "reader", "settings", "term" */
    const char *title;
    const char *exec;        /* shell command, or NULL for a built-in view */
    const view_ops_t *view;  /* built-in view, or NULL */
    const char *icon;        /* icon name, resolved as in 2.6 */
    uint16_t caps;           /* APP_TOUCH | APP_KBD | APP_FULLSCREEN | APP_SINGLETON */
} app_desc_t;

typedef struct { const char *ext; const char *app_id; } app_assoc_t;   /* sorted by ext */
/* ".gmi" -> reader, ".md" -> reader, ".lsp"/".c"/".txt" -> editor, ".pgn" -> chess, book dir -> reader */
```

- `open <path>` (new shell command) resolves the association. The launcher, the file manager and shortcuts all call the same resolver.
- **SD-provided apps (optional, end of phase):** `/sd0/system/apps/*.app` text manifests (id, title, exec, icon) merged after the built-ins, for Lisp programs and user ELFs. Kept in a PSRAM table, never SRAM.

**Settings store** (`kernel/settings.c`): typed keys (`display.brightness`, `audio.volume`, `display.orient`, `ui.text_size`, `ui.click_sound`, `ui.osk_preview`, `launcher.face`, …). Persisted as a small `key=value` text file `/sd0/system/SETTINGS`, with the writable flash filesystem as fallback where a board has one. Loaded at boot, written debounced (≥ 2 s after the last change). Changes emit `WIN_EV_SETTINGS`.

### 2.6 Icons

- **Format `.GIC`** (gray icon): magic, version, width, height, bpp (4), then rows of 4-bpp gray (0 = ink, 15 = paper, matching the AA font coverage convention). Two sizes per icon: 32×32 for scale 1, 64×64 for scale 2 (512 B and 2 KB). A cover icon is 48×64 / 96×128 (portrait ratio).
- **Built-in icons** are generated from PNG sources in `assets/icons/` by `tools/gen_icons.py` into `.rodata` tables (sorted by name). For the 1-bpp RP2350 the same tool emits an ordered-dither 1-bpp version.
- **Resolution order** for an icon name: `.rodata` built-in → `/sd0/system/icons/<NAME>.GIC` → a generic fallback for the file type.
- The existing 16×16 1-bpp icons in `launcher_view.c` become the fallback set.

### 2.7 Launcher: two faces

Moved to phase 77 §1 (`plan/phase77_launcher_and_settings.md`).

### 2.8 Books: converter, layout, reading

Moved to phase 78 §1 (`plan/phase78_book_reader.md`).

---

## 3. Milestones

Order is by dependency: measure → compositor → input → toolkit → framework → apps' foundations → mouse. Each milestone ends with a **Status** line: *done* (with the date it closed), *open* (with what is left), or *planned*.

**Renumbered 2026-11-09** (owner). The phase was split: what follows 71.4 in the old plan is phases 76-78. Commit messages and older notes use the old numbers:

| Old | New | Old | New |
| :--- | :--- | :--- | :--- |
| 71.0-71.3 | 71.0-71.3 | 71.4b `ugui` for Lisp | 76.2 |
| 71.4 parts 4a-5c (views, close protocol, system events) | 71.4 | 71.5 portrait ribbon | 76.3 |
| 71.4 parts 1-3 (registry, icons, settings store), the icon grid | 71.5 | 71.6 Settings app | 77.1 |
| 71.4's text fields, mm metrics, click sound | 71.6 | 71.6b FAT32 long names | 76.1 |
| 71.4p pointer input | 71.7 | 71.7 Shortcuts and Files | 77.2 |
| 71.4m USB mouse | 71.8 | 71.8-71.10 reader core, epub, book mode | 78.1-78.3 |
| 71.11, this phase's part | 71.9 | 71.4's damage tracking | 76.5 (optional) |

The plan before the split is `plan/phase71_touch_ux_apps_and_books.md` at commit d03fb415.

### 71.0 — Baseline, bench and Phase 70 carry-over
- `uxbench [doc]` (a shell command on every panel board, `drivers/uxbench.c`): focus moves across terminal + reader + launcher, reader line scroll, page scroll and redraw, launcher redraw. Each action is timed from input to glass (`lcdterm_settle()`), and the converted frames' stages come from the backend (`lcdterm_frame_stats()`, P4: snapshot, 1-bpp conversion, reader, canvas, terminal cells, write-back) with median/p95.
- `lcd stats live on|off|reset` (P4): frames per second and mean/worst frame time once a second. **Deviation:** this replaces the on-glass overlay. An overlay drawn into the frame would dirty the rows it measures.
- **Deviation:** touch-to-glass latency for synthetic taps moved to 71.2, which added `ts tap`. 71.0 measures redraw-to-glass.
- Carry-over: `uguiselftest` into `tests/runner.py` (QEMU rv32/rv64); `plan/open_issues.md` entries for the 70.6/70.7/70.8 gaps.
- **Acceptance:** bench numbers recorded for all three configurations; runner green.
- **Status: done 2026-11-05.**

**Results (build 1337.ee71a10e+, `uxbench` with the default `shell.md`, terminal + reader + launcher):**

| Tab5 (ESP32-P4 v1.3), RGB565 frame, double-buffered | Landscape (270°) | Portrait (0°) |
| :--- | :--- | :--- |
| Focus move / pan: input to glass, median (p95) | 1031 ms (1048) | 960 ms (979) |
| — frames per action; forced | 7.0; 6 | 7.0; 6 |
| — frame median: total / 1-bpp conv / reader | 126 / 60 / 58 ms | 117 / 53 / 58 ms |
| Reader line scroll (32 px) | 856 ms (908) | 856 ms (873) |
| — frames per action; app time per action | 4.0; 322 ms | 4.0; 384 ms |
| Reader page scroll | 1398 ms (1520) | 1380 ms (1398) |
| — frames per action; app time per action | 6.0; 579 ms | 6.0; 668 ms |
| Reader redraw (no scroll) | 157 ms: frame 73 (reader 69), app 84 | 175 ms: frame 60 (reader 56), app 114 |
| Launcher redraw | 140 ms: frame 72 (reader 69), app 69 | 140 ms: frame 59 (reader 55), app 77 |

Reading the table:
- Every animation step was a **forced full frame** (`term_sync_post()` set `g_t_force`): six per pan and three to five per scroll. Each costs ~126 ms, regardless of how little moved.
- The **1-bpp conversion** (~60 ms) and the **reader/launcher composite** (~57-70 ms) are similar in size and were paid one after the other for the same pixels.
- "app" (wall time outside frames) is the reader's whole-file re-parse plus `screen_repaint()` → `draw_all()`.
- Portrait differs from landscape by only 7-15 %, so the frame format and PSRAM bandwidth (§0.4) dominate, not rotation.

| LCD-7B (ESP32-P4 v3.2), GRAY8 frame, double-buffered, native landscape | |
| :--- | :--- |
| Focus move / pan: input to glass, median (p95) | 265 ms (265) |
| — frames per action; forced | 7.0; 6 |
| — frame median: total / 1-bpp conv / reader | 25 / 9 / 14 ms |
| Reader line scroll (32 px) | 332 ms (348): 4 frames, app 231 ms |
| Reader page scroll | 579 ms (597): 6 frames, app 396 ms |
| Reader redraw (no scroll) | 83 ms: frame 17 (reader 15), app 66 |
| Launcher redraw | 66 ms: frame 17 (reader 15), app 42 |

The 7B's frames are **5× cheaper** than the Tab5's (25 against 126 ms), which matches a third of the frame traffic plus no rotation. Its reader scroll and page were dominated by **app time** (the whole-file re-parse per animation step and the full `draw_all()`), so the layout index (78.1) and the partial repaint (71.1) matter on the 7B as much as the compositor work matters on the Tab5.

### 71.1 — Compositor fast path (Tab5 performance)
- RGB565 reader and launcher surfaces composited by PPA SRM, measured against the CPU path (decision 1 in §4); tiled CPU fallback.
- The 1-bpp conversion skips all surface rectangles.
- Asynchronous animation frames for ribbon pan and reader scroll; pan as recomposite at shifted offsets, scroll by exposed rows only.
- **Acceptance:** `uxbench` meets the §2.1 targets on the Tab5 landscape (revised below); the 7B is not slower on any row; `lcd verify` and `tests/hw/lcdterm_shots.py` pass; RP2350 pixel-identical; zero `racescan` violations; `lockcheck` shows no new pairs.
- **Status: done 2026-11-05** (owner's decision), targets revised for the Tab5.

**Result.** Final `uxbench` medians, input to glass:

| | Tab5 71.0 → 71.1 | LCD-7B 71.0 → 71.1 | RP2350-LCD-7 71.0 → 71.1 |
| :--- | :--- | :--- | :--- |
| Focus move / pan | 1031 → 577 ms | 265 → 215 ms | 108 → 108 ms |
| Reader line scroll | 856 → 472 ms | 332 → 166 ms | 390 → 201 ms |
| Reader page scroll | 1398 → 786 ms | 579 → 331 ms | 469 → 284 ms |
| Reader redraw | 157 → 157 ms | 83 → 83 ms | 195 → 151 ms |
| Launcher redraw | 140 → 140 ms | 66 → 50-66 ms | 20 → 29 ms |

Done:
- **Covered units skipped:** the 1-bpp conversion skips every unit wholly inside a reader, launcher or canvas surface (rotated and native paths, GRAY8 included). A surface that moved or went away since a back buffer showed it is repainted there from the 1-bpp frame (`srf_track()`). Where a surface's edge is not on a multiple of 8 pixels, a reader whose edge unit the conversion touched is composited again (`srf_touch()`). The native path converts only the dirty columns.
- **No forced animation frames:** `term_sync_post()` no longer forces a full redraw. Animation steps convert only what changed.
- **The PPA for moved surfaces:** reader/launcher surfaces get an RGB565 copy on RGB565 frames, composited and rotated by the PPA, used only where the copy is current or the window moved (pan).
- **Checks:** `lcd check` and `uxbench` compare the incrementally drawn frame against a forced full redraw, pixel by pixel (glass check); the 1-bpp screen against a full `draw_all()` (screen check, canvas request `'E'`); the layout and strip checks below. All match on the Tab5 and the 7B, except the screen check's known writer (`plan/open_issues.md`).
- **Frame format returns after a colour canvas:** once no RGB565 canvas has been visible for 1 s (`FB_RETURN_MS`), the frame goes back to the board's own format (`g_fb_pref`: GRAY8 on the 7B, RGB565 on the Tab5).
- **Tile-only updates:** canvas request `'W'` copies a reader's or the launcher's 1-bpp backing into its tile, instead of `lcdterm_repaint()` of the whole screen. Animation steps stay synchronous: a `'W'` per step woke the flush task mid-step and made the 7B's scrolling flicker.
- **Reader layout checkpoints** (78.1's index, first form): every half screen, the source line's offset, the document y, the link number and the block flags; a later draw at the same width and file size starts at the checkpoint above the view. `reader_layout_check()` compares against a full layout.
- **Strip scrolling:** a scroll step moves the content below the action bar and lays out and draws only the rows that came into view (`reader_scroll_step()`). Steps keep an even row count, because the 1-bpp grey pattern alternates by row. `uxbench`'s strip check compares steps of 32, 7, −32, −5, ±101 px against a whole draw.
- Fixed on the way: an LCD-7B crash (the scheduler bounded stacks by RAM's end while the 7B's page heap runs past it, `kernel/sched.c`).

**Finding: the Tab5 is PSRAM-bandwidth-bound in RGB565, and the PPA does not get around it.**

| Tab5 landscape, full-width reader (~0.8 Mpx) | Cost |
| :--- | :--- |
| CPU composite, GRAY8 → RGB565, rotated, per pixel | ~70 ms |
| GRAY8 → RGB565 copy on the CPU (for the PPA) | ~48 ms |
| PPA SRM rotate of that copy into the frame (DMA alone) | ~40 ms (~80 MB/s: 1.6 MB read + 1.6 MB written) |

Moonbase's PPA pass (Phase 64) reads only 0.46 MB, a quarter-size source scaled 2×, which is why it was fast. Here source and destination are full-size, and the panel's scanout of a 1.84 MB RGB565 frame at 60 Hz already reads ~110 MB/s from the same PSRAM. So every full-window change costs ≥ 40 ms on the Tab5, whichever engine moves the pixels. A GRAY8 frame on the Tab5 is not possible: on chip revisions below 3.0 the DSI bridge takes RGB888, RGB565 or YUV422 only (`mipi_dsi_brg_ll_set_input_color_format()` has GRAY8 only under `CHIP_SUPPORT_MIN_REV >= 300`).

**Targets revised for the Tab5.** The §2.1 targets (pan ≤ 150 ms, scroll frames ≤ 16 ms) are not reachable for full-width windows there; they stay the targets for the 7B. What is left of the gap is the number of frames per action (the owner keeps today's animation steps) and the composite per frame.

**Deferred** (not scheduled; reopen with numbers): scroll as a compositor source offset (taller surface; the composite itself stays bandwidth-bound on the Tab5); `ugui`/reader 1-bpp drawing only on demand on the P4, with `lcd shot` from the frame buffer; terminal cell surfaces; the 7B's launcher redraw, dominated by the launcher's own drawing.

### 71.2 — Input pipeline
- Raw contacts from both touch chips; lift detection; a recogniser with mm thresholds; finger-following scroll.
- Claims and arbitration (§2.2 rules 1-3): edge band, full-screen games.
- Per-window event rings with pointer claims; `touch_set_hook()` and the global rings removed.
- `ts` injection for scripted gestures.
- **Acceptance:** host test of the recogniser on recorded traces; on the 7B a tap reaches the view ≤ 60 ms after lift (was ≥ 250 ms); an edge swipe leaves moonbase and ceres on the Tab5 and the 7B, and a content swipe inside them still pans the game; chess unchanged in behaviour; hardware suites pass.
- **Status: done 2026-11-06** (owner's decision). The system events moved to 71.4.

**Result:**
- **Recogniser** (`drivers/gesture.c`, `tests/host/gesture_host.c`, 48 checks under ASan/UBSan): tap, long press and four swipes, thresholds in millimetres from `CONFIG_PANEL_DPI` (Tab5 294, 7B 170, RP2350-LCD-7 133): a tap moves under 5 mm, a swipe crosses 10 mm, a long press is held 500 ms.
- **Lift detection:** the GT911's zero-point report and the ST7123's first empty report during a contact end it at once; the old timeouts (80 ms ST7123, 250 ms GT911) are only the fallback. `ts` reports how contacts ended.
- **Injection:** `ts tap X Y`, `ts swipe left|right|up|down`.
- **Finger-following scroll:** a mostly vertical stroke past the tap distance is a drag (`GESTURE_DRAG`, summed by `console_touch_drag()`); at the release the last ~100 ms decide a fling (≥ 3 mm). Only the focused view scrolls.
- **Edge swipe out of full-screen games:** a contact that begins within 3 mm of the left or right edge is the window manager's (`TOUCH_EDGE_MM`). Leaving a full-screen game's window returns the ribbon; returning goes full-screen again (`fullscreen_follow_focus()`). Moonbase and ceres pause in the background, sound stopped, and resume where they were.
- **Full-screen game convention (owner, 2026-11-06):** one menu button (New game, Exit, later settings and save/load); an edge swipe leaves the game paused; its window brings it back. Done for moonbase; ceres stays keyboard-only (owner).
- **Two games at once:** only the focused window's canvas is shown in full-screen layout, and the audio stream has an owner (`audio_p4_release()`).
- **Per-window event rings** (`kernel/winevent.c`, `kernel/event.h`): every canvas window has its own ring (PSRAM, panel boards only) for taps, close requests, and with `WIN_CLAIM_POINTER` the raw pointer stream. `touch_set_hook()` and the console's global tap and event rings are gone. Static RAM: panel boards −2.7 KB, +2.6 KB PSRAM; boards without a panel −700 B.
- Moonbase's own tests (`make test` in `user/moonbase`) link again and run in `tests/runner.py`.

### 71.3 — `ugui` v2 and touch feedback
- Items of §2.4: focus, armed state, clipping, virtual list, controls, dialogs, layout helpers, metrics for touch targets, one blitter set.
- On-screen keyboard feedback: armed key at touch-down, key preview, key repeat, latched modifiers.
- **Acceptance:** `uguiselftest` green in QEMU; on the Tab5 and 7B every launcher/reader/chess control shows armed state at touch-down; the OSK shows key preview; the 7B touch targets measure ≥ 7 mm.
- **Status: done 2026-11-06** (owner's decision). The image blit and icon grid moved to 71.5, the text sizes in millimetres and the click sound to 71.6, damage tracking to 76.5.

**Result:** press feedback in the launcher, reader action bar, chess and the OSK (with key preview); keyboard focus and navigation (Tab, arrows, Enter, Esc), the launcher fully keyboard-operable; dimmed controls that cannot act; touch areas grown to 7 mm past what is drawn, what is drawn under the finger winning; no dead zones on the OSK; key repeat for Delete and the arrows; clipping and a virtual list (the launcher's document list scrolls by pixels, follows the finger, glides); toggle, checkbox, slider, progress, tab bar, path bar, text field, dialog and action sheet, all in `launcher gallery`; rect-cut layout helpers (one launcher layout for both orientations); the reader drawing through ugui's surface and primitives (pixel-identical). Fixed on the way: typing into the wrong terminal (stale reply bytes), launcher flicker (double-buffered GRAY8 surface), the document list missing `/sd0/docs`, Tab5 rotation leaving the other orientation's pixels in one frame buffer.

### 71.4 — View framework, close protocol, system events
- `view_ops_t`: the reader and the launcher behind one interface, generic in the screen and the compositor (no `0x80` tagging), drawn by one task.
- Close protocol with owner acknowledgement and the not-responding alert.
- System events: `INPUT_MODE`, `ORIENT` + `RESIZE`, `POWER`, `SUSPEND`/`RESUME`, `FOCUS_*`, opt-in `TILT`, delivered to every view and window ring.
- **Acceptance:** adding a test view needs no change in `screen.c` or `dsi_esp32p4.c`; closing a window whose owner task hangs shows the not-responding alert within 200 ms (owner, 2026-11-07: Wait or Close Window, with a bomb; the claim stays with the stuck program, because a kernel task cannot be stopped from outside safely, `drivers/winclose.h`).
- **Status: done 2026-11-09.** The acceptance check is a test of its own: `viewtest` (`drivers/viewtest.c`, dev builds) is a view of a new window type with one button, added with its type number, its ops and their entry in `view_for_window()` -- no line in `screen.c` or `dsi_esp32p4.c`. On QEMU's RAM screen (runner test) its window opens focused, taps reach it in its own coordinates and hit its button, and its close box closes it; on the Tab5 its window shows and the glass check (`lcd check`) matches a full redraw. The one change it needed elsewhere: `ribbon` listed an unknown view type as a terminal, and now names any view by its ops.

**Result** (2026-11-07, commits cc86b35c, d1d4ca15, 09c80c70, 6272ad83, e4b87c6f, d6846adf, 4891cd05):
- **Views** (`drivers/view.h`, `drivers/view.c`): `view_ops_t` (active, event, captures, resize, close, surface, seq) and `view_for_window()`. A view gets decoded `win_event_t`s: press and release, tap, captured moves, `DRAG`/`DRAG_END`/`SCROLL`, `KEY_DOWN`. The system's Cmd shortcuts are taken out in the console pump for every view.
- **Generic in the screen:** window types from `RIBBON_WIN_READER` up are views (`ribbon_is_view()`), drawn, sized, freed and hit-tested alike; screen op `'v'` opens or focuses any view type; compositor surfaces are tagged (type << 4 | instance) and read through the view's ops. **Deviation:** instead of one `RIBBON_WIN_VIEW` type, every view keeps a window type of its own in the view range.
- **One task draws every view** (the views task): events, resizes, closes and the shell's view commands are queued for it in order (`view_post()`, `view_call()`). Launcher and reader state have one owner (`single task: views`).
- **Close protocol** (`drivers/winclose.c`): a canvas window whose program reads its events gets `WIN_EV_CLOSE_REQUEST`; a program that has not read it after 200 ms, or still has its window 5 s after reading it, gets the alert "X is not responding." with a bomb (Wait, Close Window).
- **System events:** `FOCUS_GAINED`/`FOCUS_LOST` and `SUSPEND`/`RESUME` from the screen's window reports (the games pause on them; `lcdterm_is_canvas_active()` polling gone); `INPUT_MODE` as a bitmask (`WIN_INPUT_KEYBOARD`, `_POINTER`, `_TOUCH`; `console_input_present()` for drivers outside `usbkbd`); `ORIENT` then each window's `RESIZE`; `POWER` at each 10 % step with charging and low flags; `TILT` with `WIN_CLAIM_TILT` at 10 Hz. A new window is told the system's state (`sys_hello()`). The shell's 200 ms keyboard poll is gone. `eventtest`, `closetest game`.

### 71.5 — App registry, icons, icon grid, settings store
- App registry and associations in `.rodata`; `open <path>`; `apps`.
- `.GIC` icons, `tools/gen_icons.py`, a first icon set; the 4-bit image blit; the icon grid.
- Settings store with persistence; `settings` shell command.
- **Acceptance:** `apps check`, `icons check`, `settings check` in the suite; settings survive a reboot; docs for `open`, `apps`, `settings` in `doc/user/shell.md`.
- **Status: done 2026-11-09.** Registry, icons and settings tried on the Tab5 and the 7B; the icon grid works by touch, mouse and keyboard (owner, 2026-11-09).

**Result** (commits 15c2c75f, 01aeb3a7, 504fa102, d03fb415):
- **Registry** (`kernel/apps.c`): applications (id, title, command line, icon, the window a single-instance one runs in, `APP_TOUCH`/`KBD`/`FULLSCREEN`/`SINGLETON`) and file associations, two sorted `.rodata` tables; `app_for_path()`; `open <path>`, `apps`, `apps info ID`, `apps check`. The launcher's cards and documents go through it.
- **Icons:** 17 icons in `assets/icons/*.png` (Classic Mac style, `tools/draw_icons.py`), `tools/gen_icons.py` to 64 × 64 and 32 × 32 4-bit gray in `drivers/icons_data.c` (`.rodata`, sorted, 43.5 KB), `.GIC` files with `--gic`; `icon_find()`, `icons`, `icons check`; `ugui_image4()` (integer scale, transparency, a 4 × 4 ordered dither on 1-bpp) and `ugui_card_img()`.
- **Icon grid:** `ugui_icon_grid()`: icons over labels in cells, one hit id with an index per cell, so press, hover, focus (arrows to the nearest cell) and the context menu work as on other widgets; the gallery's Icons tab shows the set in it.
- **Settings store** (`kernel/settings.c`): typed settings in a sorted `.rodata` table (`audio.volume`, `display.brightness`, `display.orient`, `input.natural_scroll`, `input.pointer_speed`, `launcher.face`, `ui.click_sound`, `ui.osk_preview`, `ui.text_size`) with board defaults, applied at once, saved to `/sd0/system/SETTINGS` (or `/flash0`) two seconds after the last change; `settings`, `settings get|set|reset`, `settings check`. Owner's test: settings survive a power cycle on the Tab5 and the 7B. `WIN_EV_SETTINGS` comes with its first consumer, the Settings app (77.1).

### 71.6 — Text input and sizes: the on-screen keyboard for every field, clipboard, millimetres, click
- The on-screen keyboard as a system service for any focused text field, not a terminal's.
- The text field on the system clipboard: Cmd+C/X/V/A and a Cut/Copy/Paste menu.
- Sizes in millimetres from the display's DPI at run time.
- The click sound for on-screen keyboard keys (`ui.click_sound`).
- **Acceptance:** on the Tab5 and the 7B without a keyboard, the gallery's text field opens the on-screen keyboard in its window and takes its keys, which also click with `ui.click_sound` on; the clipboard by keys and by menu (runner test); the touch targets measure ≥ 7 mm on every panel from the run-time DPI.
- **Status: done 2026-11-09.** The click heard on the 7B and the Tab5 (owner). The editor `e` takes the system's Cmd keys beside its Emacs ones (owner's decision, 2026-11-09: the system's standard). It had Cmd+A / C / X / V since phase 37.5b; they are documented now (`doc/user/editors.md`, with its other Cmd keys) and `editselftest` checks them beside Ctrl+Y. Found on the way: the self-test freed the editor before its last case and went on using it; the free is at the end now.
- **Fixed 2026-11-09** (owner's Tab5 test):
  - **On-screen keyboard keys lost after a game:** launcher → moonbase → end moonbase → back to a terminal, and the keyboard's keys did not reach the terminal until it was tapped. A program that closes its own window moves the focus without the console hearing of it, so its active terminal was stale; the keyboard's keys now go to the terminal the keyboard is out in (the tap's reply names it). The fault predates the keyboard-for-views work.
  - **No click on the Tab5:** silent in the first test, heard after the next flash. `audiotest click [n]` (new) clicks whatever the setting says and prints what the clicks did; on the Tab5 it showed the stream started, every click played and the stream owned. The first build's silence was not explained; if it comes back, `audiotest click` tells a click that never played from one the speaker did not make.

**Result** (commit d03fb415):
- **Keyboard for views:** a view asks with `console_osk_for_view()` (the launcher whenever a text field is focused with nothing over it); where there is touch and no keyboard the keyboard comes out in that view's window (`'k'` with the view as owner, `screen_osk_rows()`), the view gets shorter (its `'R'` height, the `'W'` blit and the P4's surface rectangle stop above the keyboard), its keys reach the view as key events, and it goes when the field loses the focus, a keyboard is plugged in, or its hide key is pressed. Tried by injection on the RP2350-LCD-7 (`kbd off`).
- **Clipboard:** ugui's text field takes Cmd+C / X / V / A on `kernel/clipboard.c` (one line of it pasted); the gallery's field has Cut / Copy / Paste / Select All on a long press or a right click. `uguiselftest` and a runner test.
- **Millimetres:** the DPI comes from the display backend at run time (`lcdterm_backend_t.dpi`, `lcdterm_dpi()`: `CONFIG_PANEL_DPI` on the panels, the monitor's EDID on the VisionFive 2, 96 without one, 0 on QEMU's RAM screen); `ugui_dpi()` / `ugui_mm_px()`; the touch targets and the reader's fling distance use them.
- **Click:** `ui.click_sound` (off by default) clicks for each on-screen keyboard key through a low-latency stream (`audio_p4_start_lowlat()`: 3 × 128 samples, ~24 ms ahead, against the games' 12 × 512) that a `uiclick` task opens on the first click and closes after 4 s of quiet; a game holding the sound keeps it.

### 71.7 — Pointer input: the mouse as an input mode
- **Why:** the VisionFive 2 workstation (phases 74.5, 75.3, 75.4) has a USB mouse and no touch. What a pointer event means to windows and widgets is the console's and `ugui`'s, defined here, so phase 75 only feeds positions (§0.5).
- `console_pointer(x, y, buttons)` / `console_wheel(dx, dy)`; hover, click, right click as the context menu, drag, the wheel to the window under the pointer, the horizontal wheel along the ribbon; `ugui` hover; `ms` injection.
- **Acceptance:** in QEMU, injected hover highlights a launcher card, a click opens it, a right click opens a document's action sheet, the wheel scrolls the reader under the pointer while a terminal has the focus, and the horizontal wheel pans the ribbon; `uguiselftest` covers hover; on the 7B the same by `ms` with the touch screen still working; phase 75.3 reuses the entry points unchanged.
- **Status: done 2026-11-08** -- runner test on QEMU, `ms` on the 7B, a real mouse on the P4 panels and the RP2350 through 71.8, and on the VisionFive 2 (owner, 2026-11-09).

**Result** (commit febe9f2d):
- `console_pointer()` / `console_wheel()` (`kernel/console.h`): hover to the view under the pointer (and to a canvas that claims the pointer), the old view told the pointer left (`POINTER_MOVE` at -1, -1); the left button as a finger (press feedback, a click when released within 6 px, else a vertical drag; captured controls follow it); the right button's release as the context menu; a window claiming the pointer gets every button raw (`WIN_EVF_MOUSE`) and the wheel; the vertical wheel to the view under the pointer, the horizontal one a window along the ribbon. Two flags, no new event names: `WIN_EVF_WHEEL`, `WIN_EVF_MOUSE`. `WIN_BUTTON_*` masks for drivers.
- A touch long press is the same context menu (`console_touch_context()`).
- `ugui_hover()` (the widget as drawn: exact), outlined by `ugui_end()` / `ugui_draw_hover()`; the launcher and the reader's bar show it. The launcher: a document's action sheet (Open, Open in Editor or in Reader, Cancel); the wheel steps its list. The reader: three lines a notch.
- `ms move | click [X Y] [right|middle] | drag | wheel DY [DX]` on every panel and QEMU's RAM screen (`screen_window_at()`, shared with the panels).
- **Deviation:** exact hit areas come from the board, not the event: a board without touch has no grown areas; on a P4 panel with a mouse the 7 mm areas stay (a view cannot tell a click from a tap).

### 71.8 — A USB mouse on the P4 panels and the RP2350 terminal
- **Why:** 71.7 routes a pointer on every screen, but the P4 and the RP2350-LCD-7 read no mouse and draw no cursor. With a mouse the 7B and the Tab5 work like the HDMI workstation, and the RP2350-LCD-7 without a touch panel (owner, 2026-11-08) gets a pointer at all.
- USB HID mice in the shared host (`drivers/usbkbd_host.c`, the RP2350's in U-mode `.kbdtext`); touch found at run time (`touch_found()`); a shared kernel pointer (`kernel/pointer.c`); the cursor in the P4 compositor and the RP2350 scan-out; `cursor` on every screen.
- **Acceptance:** on the 7B and the Tab5 with a USB mouse and a keyboard on one hub: the cursor follows the mouse at the panel's frame rate in both frame formats and both Tab5 orientations; hover, click, right click, wheel and drag as 71.7's runner test checks with `ms`; touch works and hides the cursor; unplugging the mouse hides it and reports `WIN_INPUT_POINTER` gone; `usbkbdselftest` covers mouse descriptors and reports; on the RP2350-LCD-7 without touch a mouse and a keyboard on its hub operate the launcher, the reader and the terminals (no on-screen keyboard), with touch both work together; static RAM grows only by the mouse's state.
- **Status: done 2026-11-09.** Tried on the 7B, the Tab5 and the RP2350-LCD-7 (owner, 2026-11-08) and the VisionFive 2 (owner, 2026-11-09). The VisionFive 2's xHCI mouse decodes its reports as the P4's host does (2026-11-09): the report descriptor's length from the HID descriptor, the descriptor read at setup and parsed by `usbmouse_parse_report_desc()`, the boot protocol only for one without X and Y, each report decoded by `usbmouse_decode()` over the bytes the transfer moved (a 64-byte report buffer that a transfer never overruns).

**Result** (commits 8782e6f7, 4515f943, f0a7bdd8, 4c9c8f36):
- **Touch found, not built in:** `touch_found()` (the probe's answer) replaces `TOUCH_PRESENT` for `WIN_INPUT_TOUCH`, the on-screen keyboard, the boot-time launcher and the grown touch areas. A mouse arriving where there is no keyboard and no touch opens the launcher.
- **Driver:** `usb_parse_config()` finds boot mice (class 3/1/2, also beside a keyboard on a receiver); the host reads the mouse's HID report descriptor (`usbmouse_parse_report_desc()`: buttons, X, Y, wheel, report ID, fields up to 16 bits) and decodes its reports, the boot protocol only for a descriptor without X and Y (the owner's 046a:b092 reports its wheel in the report protocol only); a ring of its own (`usbmouse_event()`); `kbd` lists the mouse and its layout; `usbkbdselftest` covers a mouse's and a receiver's descriptors and reports. The RP2350's task code follows `.kbdtext`'s rules (`tools/check_umode_text.py`).
- **Pointer:** the console pump reads the ring into `kernel/pointer.c` (`input.pointer_speed`, 25..400 %; clamped to the screen in its orientation) and on to `console_pointer()` / `console_wheel()`. **Natural scrolling** (owner, 2026-11-08, the Macintosh's): `input.natural_scroll`, on by default, turns both wheel axes round in `console_wheel()`.
- **Cursor on the P4:** the compositor (`drivers/dsi_esp32p4.c`) draws the 16 × 16 arrow (`kernel/cursor_shape.h`, scaled by `CHROME_SCALE`) last in every pass, in GRAY8 and RGB565 and every rotation; where it was is drawn again from its sources, so it keeps no saved pixels. `console_pointer_shown()` / `console_pointer_hide()` (a finger, an unplugged mouse) and the backend's `pointer` hook.
- **Cursor on the RP2350-LCD-7:** its frame is scanned with no CPU, so the cursor is spliced into the scan-out: the frame DMA reads 32 strips of 15 rows through a 32-word address ring (`g_seg`), and the two strips under the cursor come from a copy with the arrow drawn in, refreshed at 50 Hz by an `lcdcursor` task.
- **Scrollbar:** the launcher's document scrollbar is a control (`ugui_add_drag_hit()`): pressed or dragged, by mouse or finger.
- **`cursor [show|hide|speed PCT]`** on every screen. The VisionFive 2 on the shared path: `xhci_take_mouse()`, `pointer_relative()`, `kernel/cursor.c` draws the shared arrow, `ms` through `console_pointer()`.
- Owner's tests (2026-11-08): the 7B and the Tab5 (cursor, wheel, scrollbar by touch), the RP2350-LCD-7 (mouse and keyboard behind its hub, cursor, hover, clicks, wheel, scrollbar, touch, the turned panel, natural scrolling).

### 71.9 — Verification and documentation
- Hardware suites: `tests/hw/test_esp32p4.py` gains settings persistence, system events, mouse (by `ms`) and `uxbench` regression checks (fail if a bench row gets > 20 % slower than the recorded 71.1 baseline).
- `tests/hw/lcdterm_shots.py` scenarios: the launcher, the gallery with the on-screen keyboard in a view, the cursor.
- Docs: `doc/user/ribbon.md` (gestures, the mouse, the keyboard in a view), a new `doc/dev/` page for the view framework, the events and `ugui` v2; `doc/user/shell.md` checked for every command this phase added (`open`, `apps`, `settings`, `ms`, `ts`, `cursor`, `eventtest`, `launcher gallery`); `python3 tools/gendocs.py --all` and `--check`.
- RAM baselines re-recorded; `sizereport` for RP2350 shows no SRAM growth beyond what 71.8 accepted.
- **Status: done 2026-11-09.** Docs: `doc/dev/kernel/ui_framework.md` (views and how to add one, window events and claims, input routing, the on-screen keyboard as a service, `ugui`, registry, icons, settings); `doc/user/ribbon.md` (the mouse on every board, the cursor, scrollbars, the key click, the keyboard's keys to its own terminal); `doc/user/shell.md` checked for this phase's commands (`viewtest` and `audiotest click` added). Hardware checks done 2026-11-09 (`tests/hw/test_esp32p4.py`, passing on the Tab5 and the 7B): `test_settings_persist` (a setting kept across a reset), `test_view_and_mouse` (`viewtest` opened focused, an `ms` click reaching it, the glass check, Cmd+W), `test_uxbench_regression` (no row 20 % slower than 71.1's record: Tab5 pan 576, scroll 472, page 768, redraw 158, launcher 157 ms; 7B 215, 166, 331, 83, 66 ms), beside the existing `test_system_events` and `test_close_protocol`. Screenshots: `lcdterm_shots.py --scenario views` (the launcher, the gallery, `viewtest`, the cursor). **RAM over the phase** (baselines at its start, 03445983, against its close): static SRAM down on every panel board -- RP2350-LCD-7 197,573 -> 196,834 B, LCD-7B 219,635 -> 216,791, Tab5 220,022 -> 217,178 -- with the same heap pages; PSRAM (`BULK_BSS`) up 51 KB on the RP2350-LCD-7 and 60 KB on the P4s (the views' state, the per-window event rings, the test view).

---

## 4. Decisions (confirmed 2026-11-05)

1. **Surface format on P4: RGB565** (option A). It reuses the proven PPA SRM path with no new driver: the Tab5's rev v1.3 cannot read GRAY8 in SRM, and 32 MB PSRAM makes the memory cost irrelevant. Option B is dropped.
2. **Ribbon gestures:** a horizontal swipe anywhere moves the ribbon in views that do not claim it (terminals, canvases). Claiming views (book mode, sliders, chess drag) keep their swipes. Edge and chrome swipes always reach the window manager, **including from the full-screen games moonbase and ceres** (§2.2).
3. **Books live in `/sd0/BOOKS`** (now phase 78).
4. **Images in books: alt text only** (now phase 78).
5. **RP2350-terminal: reduced support.** It gets the view framework, the registry and the launcher (build-gated, PSRAM-backed). Portrait, books and the Latin-1 font are not included unless the flash budget allows.

## 5. Out of scope

The future extensions accepted on 2026-11-05 moved with the work they extend: UX 2.0 chrome, on-screen keyboard diacritics, `ugui` for C, multi-touch and colour accents to phase 76 §3; file copy/move and catalog search to phase 77 §3; book images, hyphenation and justified text to phase 78 §3. SD-provided app manifests (§2.5) to phase 77 §3.
