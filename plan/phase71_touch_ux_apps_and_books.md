# Phase 71 — Touch UX Foundation II, Launcher/Files/Settings & the Book Reader

**Status: PLANNED (drafted 2026-11-05, after 0.20.0; decisions in §4 confirmed by the owner 2026-11-05).**
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

---

## 1. Review of the proposed scope (corrections and additions)

| Proposal | Verdict | Correction / enhancement |
| :--- | :--- | :--- |
| Apply the games' fused graphics path to the UX | **Yes, broadened** | On the Tab5's chip (rev v1.3) the PPA SRM block cannot read GRAY8. That needs rev ≥ 3.0 (`ppa_ll_srm_is_color_mode_supported()` in esp-idf), which the 7B has but the Tab5 does not. So either the UX surfaces become RGB565, or the gray surface goes through the blend engine as A8 first (L8/CLUT is disabled in esp-idf on every revision). Decided: RGB565 (§4). The plan also removes the non-rotation costs (re-parse, double draw, synchronous animation frames), which are half the problem. **Measure first** with a UX bench, as moonbase did. |
| Vertical ribbon in portrait | **Yes, generalised** | Make the ribbon's *major axis* a property chosen by orientation, not a second window manager. Portrait shows one window, or two stacked top/bottom. This requires gesture arbitration, because a vertical swipe is also content scroll (71.2, 71.5). |
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

**Added items not in the original list:** UX benchmark and frame-time overlay (71.0); generic view-window framework (71.4); settings persistence (71.4/71.6); Latin-1 + typographic punctuation for the standard-density serif font (71.9); last-read position and "continue reading" shortcut (71.10).

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
- **Terminals (stretch, 71.1e).** Rendering terminal cells into an RGB565 surface per terminal turns a ribbon pan containing terminals into composite-only as well. Optional, decided after the 71.1 measurements.

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
  - `WIN_EV_INPUT_MODE`: keyboard attached/detached, carries the new mode (`KBD`, `TOUCH`, `BOTH`). Produced from `usbkbd` attach/detach, no longer polled in the shell.
  - `WIN_EV_ORIENT`: orientation changed (after the change). Followed by `RESIZE` with the new size.
  - `WIN_EV_TILT`: opt-in per view (`CLAIM_TILT`), at most 10 Hz, pitch/roll in milli-g from the BMI270. For a future level/game. Never delivered by default.
  - `WIN_EV_POWER`: battery level band changed (100/50/20/10 %), charger attached/detached.
  - `WIN_EV_SUSPEND`/`WIN_EV_RESUME`: the view is fully off-screen or back. Views stop timers while suspended.
  - `WIN_EV_SETTINGS`: a setting a view depends on changed (for example text size).

### 2.3 Portrait: the major-axis ribbon

- `ribbon_t` gets an `axis` field (`RIBBON_AXIS_X` | `RIBBON_AXIS_Y`). `rx0`/`rx1`/`x_view` become major-axis coordinates (renamed `m0`/`m1`/`m_view`). Window size along the major axis is counted in text rows when the axis is Y (`RIBBON_ROWS_*` next to `RIBBON_COLS_*`).
- **Portrait defaults (Tab5 720×1280):** one window per screen. `Cmd+\` (maximise toggle) switches between one-up and a **top/bottom pair** (about 18 terminal rows each at 16×32). Never side-by-side slices.
- **Navigation:** swipe on chrome or the edge band along Y (rule 1 in 2.2). `Cmd+Up/Down` in portrait, and `Cmd+Left/Right` keep working in both orientations as "previous/next window" (so muscle memory carries over). The minimap moves to a vertical strip at the right edge of the menu bar area, or stays horizontal as window order. Decided with a mock-up on the device.
- **Rotation keeps the window order and focus.** Landscape pairs become portrait stacks. A one-up portrait window stays one-up.
- The 7B and RP2350 never use `AXIS_Y`. On RP2350 the field is constant and folds away.

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

The launcher is the home view, with a tab bar for **Shortcuts | Files** (remembered in `launcher.face`). Settings leaves the launcher and becomes its own app with a shortcut tile.

**Shortcuts face**
- Icon grid of entries from `/sd0/system/SHORTCUT` (text, one per line: `type<TAB>target<TAB>label<TAB>icon`; types `app`, `doc`, `dir`, `book`). Defaults come from `/flash0` when the SD file is missing.
- Tap/Enter opens the entry. Long-press or `Menu`/`m` opens an action sheet: Open, Rename, Remove, Move left/right. Removing asks for confirmation.
- A "Continue reading" tile appears when a book has a stored position (71.10).

**Files face**
- Real directory traversal starting at a **roots** list (`/sd0`, `/flash0`, `/ram0`, mounted 9P trees). Path bar with tappable segments; `Backspace`/`←` goes up.
- Virtual list rows: icon by association, display name, size, and type badge. Display names come from metadata where available (book catalog, `# H1` of Markdown files, cached), otherwise the 8.3 name.
- Tap/Enter opens by association. Action sheet: Open, Open with…, Add to shortcuts, Info, and (with confirmation) Delete. Copy/rename are a follow-up phase unless cheap through existing VFS calls.
- Sorting: directories first, then name. A "Books" root shows the catalog as a cover grid instead of the raw directory (`/sd0/BOOKS`).

**Keyboard map (launcher and all `ugui` apps):** Tab/arrows move focus, Enter opens, Esc goes back, `Cmd+1/2` switches faces, `/` focuses search (later), `Cmd+Space` opens the launcher from anywhere (new global hotkey; `Cmd+L` as alternative if `Cmd+Space` collides with the keyboard layout).

### 2.8 Books: converter, layout, reading

**Converter `tools/epub2lmd.py`** (in a `uv` project under `host/booktools/`, typed for `pyrefly`, with tests):
- Input: a directory tree of `.epub` files. Output: `<out>/BOOKS/`, merged into `build/<preset>/sd_root/` or synced with `p9sync.py` (additive and idempotent, as for the other SD content).
- Per book a directory `B<hash6>` (8.3-safe, stable across runs: hash of the epub's `dc:identifier`, or of title+author):
  - `META` — `title=`, `author=`, `lang=`, `chapters=`, `source=` (original file name), `converter=` version.
  - `CH001.MD` … — one file per spine item, in the reader's restricted Markdown. Chapters below 2 KB are merged with the next.
  - `INDEX.MD` — the table of contents (from the epub nav/NCX) as links to `CHnnn.MD#anchor`.
  - `COVER.GIC` — cover icon (2.6), or one generated from title/author when the epub has no cover.
- `BOOKS/CATALOG` — one line per book: id, title, author, chapter count. This is the launcher's data source, so it never has to open 500 directories.
- **XHTML → Markdown mapping:** `h1-h4` → `#`…, `p` → paragraph, `em/i` → `*`, `strong/b` → `**`, `blockquote` → `>`, `ul/ol/li` → lists, `pre/code` → fences, `a href` → links rewritten to `CHnnn.MD#id`, footnotes (`epub:type="noteref"/"footnote"`) → `[^n]`, simple tables → pipe tables, `img` → `*[Image: alt]*`, `br` → line break. Everything else is reduced to text. CSS is ignored.
- **Character folding:** every character outside the target font set is mapped through a fold table (’‘ → ', “” → ", – → -, — → --, … → ..., non-breaking space → space, ligatures → letters). Remaining unknown characters become `?` and are counted in the converter's report. The target set is Latin-1 + the punctuation added in 71.9.
- DRM-protected epubs (`META-INF/encryption.xml` with content encryption) are skipped with a message. No attempt to decrypt.
- Report per book: chapters, bytes, folded/unknown characters, dropped elements.

**Reader core for large documents** (`drivers/reader_view.c`, split into `reader_layout.c` + `reader_view.c`):
- **Layout index:** one pass over the file builds an array of blocks `{file_off, y, h, kind, toc_level}` (12-16 bytes each) in PSRAM, for the current text width. A 500 KB chapter of ~5000 paragraphs needs ~80 KB. The pass reads the file in 4 KB chunks and yields between chunks. A progress bar is shown for long files, and the first screen is drawn as soon as its blocks are laid out.
- **Draw** = binary search of the first visible block, `vfs_pread` from its offset, render only blocks that intersect the viewport (plus the ring margin of 2.1). No whole-file re-parse.
- **Invalidation:** a width change (rotation, maximise, text size) rebuilds the index. The reading position is restored by byte offset.
- **Links/TOC** are collected during the index pass, not per draw (today `link_count`/`toc_count` are rebuilt on every draw).
- **Limit:** the index is bounded by available PSRAM, not by a fixed constant. A file above that limit opens with a clear message, never a crash.

**Book mode (reader opened on a book directory or a `CHnnn.MD` inside one):**
- **Paging:** page = viewport height minus one line of overlap, cut at line boundaries (no line split across pages). Page table derived from the layout index.
- **Gestures:** tap on the right third / swipe left → next page; tap on the left third / swipe right → previous page; tap in the middle third → toggle the toolbar (Back, Contents, Text size, Rotate, Close). The reader claims `SWIPE_H` in book mode only (2.2), so the ribbon still moves from chrome and edges.
- **Keys:** Space/PgDn/→ next page, PgUp/← previous page, `t`/`c` contents, `+`/`-` text size, Home/End chapter start/end, `n`/`p` next/previous chapter.
- **Chapters:** the next page at the end of a chapter opens the next `CHnnn.MD`. The status line shows the chapter title and "page x of y" for the chapter, plus a book-wide percentage estimated from chapter sizes.
- **Position:** stored per book as `chapter + byte offset` in `/sd0/BOOKS/<id>/POS`, written on page turn (debounced) and on close. Reopening continues there.
- **Rendering:** page turns are instant by default (one surface redraw + composite). A slide animation is an option in Settings, using composite-only frames (2.1).

---

## 3. Milestones

Order is by dependency: measure → compositor → input → toolkit → framework → apps. 71.8/71.9 (reader core, converter) are independent of 71.5-71.7 and can run in parallel with them.

### 71.0 — Baseline, bench and Phase 70 carry-over
- `uxbench [doc]` (a shell command on every panel board, `drivers/uxbench.c`): focus moves across terminal + reader + launcher, reader line scroll, page scroll and redraw, launcher redraw. Each action is timed from input to glass (`lcdterm_settle()`), and the converted frames' stages come from the backend (`lcdterm_frame_stats()`, P4: snapshot, 1-bpp conversion, reader, canvas, terminal cells, write-back) with median/p95.
- `lcd stats live on|off|reset` (P4): frames per second and mean/worst frame time once a second. **Deviation:** this replaces the on-glass overlay. An overlay drawn into the frame would dirty the rows it measures.
- **Deviation:** touch-to-glass latency for synthetic taps moves to 71.2, which adds `touch inject`. 71.0 measures redraw-to-glass, and the release latency is known from the code (§0.3.3).
- Record the numbers for the Tab5 (landscape and portrait) and the 7B in this plan.
- Carry-over: `uguiselftest` into `tests/runner.py` (QEMU rv32/rv64); `plan/open_issues.md` entries for the 70.6/70.7/70.8 gaps, each pointing to the 71.x milestone that closes it.
- **Acceptance:** bench numbers recorded for all three configurations; runner green.

**71.0 results (2026-11-05, build 1337.ee71a10e+, `uxbench` with the default `shell.md`, terminal + reader + launcher):**

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
- Every animation step is a **forced full frame** (`term_sync_post()` sets `g_t_force`): six per pan and three to five per scroll. Each costs ~126 ms, regardless of how little moved.
- The **1-bpp conversion** (~60 ms) and the **reader/launcher composite** (~57-70 ms) are similar in size and are paid one after the other for the same pixels. Skipping the conversion under surfaces alone would halve a frame.
- "app" (wall time outside frames) is the reader's whole-file re-parse plus `screen_repaint()` → `draw_all()`. A reader update repaints the entire 1-bpp screen (`lcdterm_repaint()` in `reader_update_instance()`), not just the window. Both are targeted by 71.1 and 71.8.
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

The 7B's frames are **5× cheaper** than the Tab5's (25 against 126 ms), which matches a third of the frame traffic plus no rotation. But its reader scroll and page are still dominated by **app time** (231 and 396 ms per action: the whole-file re-parse per animation step and the full `draw_all()`). So 71.8's layout index and the partial repaint in 71.1 matter on the 7B as much as the compositor work matters on the Tab5.

### 71.1 — Compositor fast path (Tab5 performance)
- **71.1a:** an RGB565 reader surface composited by PPA SRM, measured against today's path on one reader window (decision 1 in §4). The `ugui`/font blitters get RGB565 output (gray ink levels through a 256-entry gray→RGB565 table).
- **71.1b:** surfaces for reader and launcher in the chosen format; PPA composite for 90°/270°; tiled CPU transpose fallback (and for 180°). Delete the per-pixel scatter loops in step 2b.
- **71.1c:** the 1-bpp conversion skips all surface rectangles; `ugui` 1-bpp drawing only on demand; `lcd shot` from the frame buffer.
- **71.1d:** asynchronous animation frames for ribbon pan and reader scroll (no `lcdterm_sync_post()` per step); pan = recomposite at shifted offsets; scroll = `src_y` offset + exposed rows only.
- **71.1e (stretch, decided on 71.1d's numbers):** terminal cell surfaces.
- **Acceptance:** `uxbench` meets the 2.1 targets on the Tab5 landscape; the 7B is not slower on any row; `lcd verify` and `tests/hw/lcdterm_shots.py` pass; RP2350 pixel-identical; zero `racescan` violations; `lockcheck` shows no new pairs.

**71.1 progress (2026-11-05):**

*Done:*
- **71.1c (first half):** the 1-bpp conversion skips every unit wholly inside a reader, launcher or canvas surface (both rotated and native paths, GRAY8 included). A surface that moved or went away since a back buffer showed it is repainted there from the 1-bpp frame (`srf_track()`), and a reader there is composited again.
- **71.1d (first half):** `term_sync_post()` no longer forces a full redraw. Animation steps convert only what changed.
- **71.1a/b (measured, partly kept):** reader/launcher surfaces get an RGB565 copy on RGB565 frames, composited and rotated by the PPA. It is used only where the copy is current or the window moved (pan), because new content costs more that way (see below).
- **Correctness check:** `lcd check` and `uxbench` compare the incrementally drawn frame against a forced full redraw of the same state, pixel by pixel. Every scenario matches on the Tab5 and the 7B.

| `uxbench`, median input to glass | Tab5 71.0 → now | 7B 71.0 → now | RP2350 (first record) |
| :--- | :--- | :--- | :--- |
| Focus move / pan | 1031 → 577 ms | 265 → 215 ms | 108 ms |
| Reader line scroll | 856 → 646 ms | 332 → 282 ms | 390 ms |
| Reader page scroll | 1398 → 1065 ms | 579 → 497 ms | 469 ms |
| Reader redraw | 157 → 157 ms | 83 → 83 ms | 195 ms |
| Launcher redraw | 140 → 140 ms | 66 → 50-66 ms | 20 ms |

*Finding: the Tab5 is PSRAM-bandwidth-bound in RGB565, and the PPA does not get around it.*

| Tab5 landscape, full-width reader (~0.8 Mpx) | Cost |
| :--- | :--- |
| CPU composite, GRAY8 → RGB565, rotated, per pixel | ~70 ms |
| GRAY8 → RGB565 copy on the CPU (for the PPA) | ~48 ms |
| PPA SRM rotate of that copy into the frame (DMA alone) | ~40 ms (~80 MB/s: 1.6 MB read + 1.6 MB written) |

Moonbase's PPA pass (Phase 64) reads only 0.46 MB, a quarter-size source scaled 2×, which is why it was fast. Here source and destination are full-size. The panel's scanout of a 1.84 MB RGB565 frame at 60 Hz already reads ~110 MB/s from the same PSRAM. So every full-window change costs ≥ 40 ms on the Tab5 in RGB565, whichever engine moves the pixels. The 2.1 targets (pan ≤ 150 ms, scroll frames ≤ 16 ms) are not reachable this way for full-width windows.

*Open for decision (owner):* a **GRAY8 frame on the Tab5**, as the 7B has, halves the scanout traffic (to ~55 MB/s) and every composite (1 byte a pixel). The cost: the anti-aliased terminal pass needs a GRAY8 output, and the rotated GRAY8 composite must be a CPU tiled transpose (SRM GRAY8 needs chip rev ≥ 3.0). Games' RGB565 canvases already switch the frame to RGB565 while visible (`fb_start(FB_RGB565)` in the snapshot). This would revise decision 1 for the Tab5's *frame*. Surfaces stay GRAY8, which then suits both boards.

*Checked 2026-11-05, after the owner chose the GRAY8 frame:* **the Tab5 cannot scan out GRAY8.** On chip revisions below 3.0 the DSI bridge takes RGB888, RGB565 or YUV422 input only. esp-idf's `mipi_dsi_brg_ll_set_input_color_format()` has `LCD_COLOR_FMT_GRAY8` (`raw_type` 12) only under `CHIP_SUPPORT_MIN_REV >= 300`, and `fb_start()` forces RGB565 on the Tab5 for that reason. YUV422 is also 2 bytes a pixel, so the Tab5's scanout traffic cannot be reduced. The GRAY8-frame option above is void for the Tab5. It stays valid for the 7B (rev v3.2), whose frame already is GRAY8.

*Done after that (2026-11-05):*
- **Frame format returns after a colour canvas:** once no RGB565 canvas has been visible for 1 s (`FB_RETURN_MS`), the frame goes back to the board's own format (`g_fb_pref`: GRAY8 on the 7B, RGB565 on the Tab5). Before, the 7B stayed RGB565 after the first colour demo until reboot. `lcd` reports the own format and the switches made. Verified on the 7B: a Lisp RGB565 canvas switches to RGB565 when brought into view, and back to GRAY8 once it is off screen or closed; the glass check matches.
- **Surface edge units:** where a surface's edge is not on a multiple of 8 pixels, the 1-bpp conversion's edge unit wrote over a few of its pixels. Found by the glass check (a 3-pixel strip at the launcher's left edge after a focus move), previously hidden by the forced animation frames. A reader whose edge or partial band the conversion touched is now composited again in that pass (`srf_touch()`). The native path also converts only the dirty columns (as the rotated path already did), so typing next to a reader does not recomposite it.

- **Reader/launcher updates repaint their tile only:** canvas request `'W'` copies a reader's or the launcher's 1-bpp backing into its tile, replacing `lcdterm_repaint()` (`draw_all()` of the whole screen) after every reader update. 7B reader redraw 83 → 67 ms. A first version also sent it at every scroll-animation step. That woke the flush task mid-step, so it composited a half-drawn surface, which made the 7B's reader scrolling flicker heavily (owner's report). Removed: the steps stay synchronous.
- **Screen check:** `uxbench` also compares the 1-bpp screen against a full `draw_all()` (canvas request `'E'`). It found an unlocked writer to the screen's top rows while a reader is focused; see `plan/open_issues.md`.

- **Reader layout checkpoints (71.8's index, first form):** the full layout records, every half screen, the source line's offset, the document y, the link number and the code/math block flags. A later draw at the same width and file size starts at the checkpoint above the view and stops below it. `uxbench` checks it against a full layout at the top and deep in the document (`reader_layout_check()`): they match on both P4 boards. Tab5 line scroll 646 → 576 ms, 7B 282 → 215 ms; page Tab5 1065 → 996, 7B 497 → 431. What remains per animation step is drawing the whole surface (~50 ms on the 7B) and, on the Tab5, compositing it (~70 ms).
- **LCD-7B crash** (owner's report: a second terminal, then chess from the launcher): the scheduler bounded stacks by RAM's end while the 7B's page heap runs past it. Fixed in `kernel/sched.c`.

- **Strip scrolling for animation steps:** a scroll step moves the content below the action bar in both surfaces and lays out and draws only the rows that came into view (`reader_scroll_step()`, `reader_draw_view()` into a sub-surface), then the scrollbar. The full draw after the animation is unchanged. Steps keep an even row count and strips start on even rows, because the 1-bpp grey pattern alternates by row. `uxbench`'s strip check compares one step of 32, 7, −32, −5, ±101 px against a whole draw: all match on both P4 boards. Tab5 line scroll 576 → 472 ms, page 996 → 786; 7B 215 → 166, 431 → 331. Against the 71.0 baseline: Tab5 scroll 856 → 472, page 1398 → 786; 7B scroll 332 → 166, page 579 → 331.

**71.1 closed 2026-11-05** (owner's decision). Final `uxbench` medians, input to glass:

| | Tab5 71.0 → 71.1 | LCD-7B 71.0 → 71.1 | RP2350-LCD-7 71.0 → 71.1 |
| :--- | :--- | :--- | :--- |
| Focus move / pan | 1031 → 577 ms | 265 → 215 ms | 108 → 108 ms |
| Reader line scroll | 856 → 472 ms | 332 → 166 ms | 390 → 201 ms |
| Reader page scroll | 1398 → 786 ms | 579 → 331 ms | 469 → 284 ms |
| Reader redraw | 157 → 157 ms | 83 → 83 ms | 195 → 151 ms |
| Launcher redraw | 140 → 140 ms | 66 → 50-66 ms | 20 → 29 ms |

Done: the frame record and `uxbench`; glass, screen, layout and strip checks; covered units skipped; no forced animation frames; the PPA for moved surfaces; the frame's own format restored after colour canvases; tile-only reader/launcher updates; layout checkpoints; strip scrolling. Every check passes except the screen check's known writer (open issues).

**Targets revised for the Tab5.** Its rev v1.3 bridge scans out RGB565 only (no GRAY8), and every full-window composite costs ≥ 40 ms of PSRAM bandwidth whichever engine moves it (§2.1's options measured above). The 2.1 targets (pan ≤ 150 ms, scroll frames ≤ 16 ms) are not reachable for full-width windows there. They stay the targets for the 7B. What is left of the gap is the number of frames per action (the owner keeps today's animation steps) and the composite per frame.

**Deferred** (not scheduled; reopen with numbers): scroll as a compositor source offset (taller surface; the composite itself stays bandwidth-bound on the Tab5); `ugui`/reader 1-bpp drawing only on demand on the P4, with `lcd shot` from the frame buffer; terminal cell surfaces (71.1e); the 7B's launcher redraw, now dominated by the launcher's own drawing.

### 71.2 — Input pipeline and system events
- Raw contact reporting from both touch chips; GT911 zero-count release; recogniser with mm thresholds and `LONG_PRESS`, `DRAG_*`, throttled `POINTER_MOVE`.
- Claims and arbitration (2.2 rules 1-3); edge band and chrome start.
- Per-view event rings and routing with pointer capture; `window_poll_event()` with a window handle; port chess, moonbase, ceres; delete the canvas tap/key rings and `touch_set_hook()`.
- System events: `INPUT_MODE` from `usbkbd` attach/detach (the shell's 200 ms poll goes away), `ORIENT`+`RESIZE`, `POWER`, `SUSPEND`/`RESUME`, opt-in `TILT`.
- `touch log` shows recogniser output; `touch inject` for scripted gestures in tests.
- Edge swipe out of moonbase and ceres (2.2), with `SUSPEND`/`RESUME` in both games' P4 platform layers.
- **Acceptance:** host test of the recogniser on recorded traces from both chips (tap, long press, swipes in four directions, drag, wobble, edge swipes); on the 7B a tap reaches the view ≤ 60 ms after lift (was ≥ 250 ms); an edge swipe leaves moonbase and ceres on the Tab5 and the 7B, and a content swipe inside them still pans the game; chess unchanged in behaviour; hardware suites pass.

**71.2 progress (2026-11-05):**
- **Recogniser** (`drivers/gesture.c`, `tests/host/gesture_host.c`, 48 checks under ASan/UBSan): tap, long press and four swipes from one finger's contacts, thresholds in millimetres from the board's new `CONFIG_PANEL_DPI` (Tab5 294, 7B 170, RP2350-LCD-7 133): a tap moves under 5 mm, a swipe crosses 10 mm, a long press is held 500 ms. The touch driver feeds it and maps its gestures onto today's outputs (tap, ribbon move, scroll); long presses are counted, not yet used.
- **Lift detection:** the GT911's zero-point report and the ST7123's first empty coordinate report during a contact end it at once (measured on the Tab5: every empty report during a contact came after its last point). Before, a contact ended 80 ms (ST7123) or up to 250 ms (GT911) after its last report; that is now only the fallback for a missed lift. `ts` reports how contacts ended and the longest gap within one. The owner: the 7B's on-screen keyboard is instant, the Tab5's faster with a slight, acceptable delay; taps, swipes and drags work on both.
- **Injection:** `ts tap X Y`, `ts swipe left|right|up|down` (with the 7B crash fix).
- **Finger-following scroll:** a stroke that is mostly vertical past the tap distance is a drag. `GESTURE_DRAG` reports its motion at each report, and `console_touch_drag()` sums it until the input path takes it, so a slow frame catches up in one step. The focused reader follows the finger with strip steps. At the release (`GESTURE_DRAG_END`) the finger's motion over its last ~100 ms decides a fling (≥ 3 mm), otherwise the view stays and is drawn whole. Only the focused reader scrolls (before, a vertical swipe scrolled the active reader instance from any window); the launcher keeps its one-step scroll per stroke. Owner: perfect on the 7B, good on the Tab5 within its frame cost.
- **Edge swipe out of full-screen games:** a contact that begins within 3 mm of the left or right edge is the window manager's, never the game's touch hook (`TOUCH_EDGE_MM`, counted in `ts`). A canvas that asked for full-screen carries a flag. When the focus leaves its window (edge swipe, keyboard, minimap), the screen goes back to the ribbon with the game's window in it, and when the focus returns, full-screen again (`fullscreen_follow_focus()`). Moonbase and ceres pause while their window is in the background: nothing simulated or drawn, the sound stream stopped (an unrefilled DMA ring replayed its last second; refilled out of step it doubled voices), and on resume they continue from where they were. Owner's test on the Tab5: leaving by edge swipe, returning by swipe and minimap, sound stops and resumes cleanly.
- **Full-screen game convention (owner, 2026-11-06):** every full-screen game has the same menu button, starting with *New game* and *Exit* (later: settings, save/load); an edge swipe leaves it paused; its window brings it back. Moonbase's HUD arrows (never implemented) and `[Exit]` give way to the menu button. Done for moonbase: one MENU button at the top right on every page, opening the menu (Resume, terrain, New game, Exit) over play, the title or an ending, with Resume going back there. Ceres stays keyboard-only for now (owner: its gameplay does not suit touch); leaving and returning by swipe is enough there.
- **Two games at once:** in full-screen layout only the focused window's canvas is shown (another game's, paused, used to be composited over it), and the one audio stream has an owner (`audio_p4_release()`, under `s_audio_lock`): a resuming game takes it over, a pausing or exiting one stops it only if it still owns it. Before, a direct swipe between the games left both silent. The launcher's Moonbase card focuses a running moonbase (found by its canvas, not its generic window title).
- **Per-window event rings** (`kernel/winevent.c`, `kernel/event.h`): every canvas window has its own ring (in PSRAM, on panel boards only), for its taps (a DOWN/UP pair, `WIN_EVF_TAP`), its close request, and with `WIN_CLAIM_POINTER` the raw pointer stream of contacts that begin while it has the focus and not at the screen's edge (moves merged while unread, content coordinates). `touch_set_hook()`, the console's global tap and event rings and its close flag are gone; moonbase claims the pointer, chess reads its taps and close request, ceres its close request. Keys stay a byte ring for the focused canvas, emptied when that changes. Static RAM: panel boards −2.7 KB (moonbase's own queue and the console's rings), +2.6 KB PSRAM; the boards without a panel −700 B (phase 70's rings). `FOCUS_*` and `RESIZE` stay reserved for the view framework (71.4). Owner's test: moonbase (map drag, buttons, menu, edge swipe), chess (taps, close box), ceres (close box), two programs side by side. Known: ceres's close box is not drawn (its full-screen picture covers the title bar) but works.
- **Moonbase's own tests** (`make test` in `user/moonbase`) had not linked since phase 70 (the HUD arrows' platform calls had no host stub) and were not run by `tests/runner.py`. They link again, have a menu-button test, and the runner runs them.

### 71.3 — `ugui` v2 and touch feedback
- Items 1-14 of 2.4, with `uguiselftest` cases for each (focus order and arrow navigation, armed/disarm/activate, damage union, clip, virtual list ranges, text field editing, dialog capture).
- **OSK feedback:** armed key drawn inverted at touch-down; a key preview bubble above the key on HiDPI (2× glyph); repeat on long press for Backspace and arrows; optional click sound (`ui.click_sound`, short tone through `audio_esp32p4`); Shift/Ctrl state visibly latched.
- Reader's `rdraw_*` moved onto `ugui_surface_t`.
- **Acceptance:** `uguiselftest` green in QEMU; on the Tab5 and 7B every launcher/reader/chess control shows armed state at touch-down; the OSK shows key preview; the 7B touch targets measure ≥ 7 mm.

### 71.4 — View framework, app registry, icons, settings store
- `view_ops_t`, `RIBBON_WIN_VIEW`; reader and launcher ported; `0x80` tagging removed; close protocol with owner acknowledgement and claim release.
- App registry and associations in `.rodata`; `open <path>` shell command; `apps` shell command (list, `apps info <id>`).
- `.GIC` format, `tools/gen_icons.py`, first icon set (reader, chess, moonbase, ceres, terminal, files, settings, book, folder, Markdown, Gemtext, Lisp, C source, text, generic).
- Settings store with persistence; existing launcher settings wired to it; `settings` shell command (`settings get|set|list`).
- **Acceptance:** adding a test view needs no change in `screen.c` or `dsi_esp32p4.c`; settings survive a reboot; closing a window whose owner task hangs releases its session claim within 200 ms; docs updated (`doc/user/shell.md` for `open`, `apps`, `settings`).

### 71.4b — `ugui` for Lisp programs (added 2026-11-06, owner)
- Lisp primitives over the 71.3 widgets, in the immediate-mode model the toolkit already uses: the program owns its loop, `(ui-begin)`, one call per widget (drawn into the program's own surface, its hit-box registered: `(ui-button id "OK" x y w h)`, cards, lists, steppers, text fields...), `(ui-end)` presents; `(ui-event)` takes the next event from the window's own ring (71.2's routing), `(ui-hit ev)` names the widget it hit. No callbacks from system tasks into the program.
- The surface: a canvas window (GRAY8 or RGB565, as the board's surfaces are), so a Lisp UI app is a ribbon window like any other and gets focus, keyboard parity (71.3's focus) and the full-screen convention if it asks for full screen.
- Primitives in the builtins table (`.rodata`, sorted), documented in `doc/dev/on_device/lisp_scripting.md`; a sample app ("hello ugui": a few controls and a list) in `tools/sd_root/demos/`.
- **Acceptance:** the sample runs by touch and by keys only on the Tab5, the 7B and the RP2350-LCD-7; QEMU's RAM screen runs it in the test suite with injected events.

### 71.5 — Portrait major-axis ribbon
- `ribbon_t.axis`, renamed major-axis fields, row-based sizes; one-up and top/bottom pair in portrait; rotation preserves order and focus.
- Gesture arbitration along Y; `Cmd+Up/Down`; minimap variant chosen on the device.
- **Acceptance:** `tests/test_ribbon.py` covers both axes (layout, viewport, insert/remove, rotation round trip); on the Tab5 portrait a terminal + reader pair is usable by touch and keys; landscape behaviour unchanged.

### 71.6 — Settings app
- Own view: Display (brightness, orientation, text size), Sound (volume, click sound, test beep), Input (OSK preview, keyboard status), Power (battery, charger, INA226 readings), Network (Wi-Fi state, IP), Storage (mounted volumes, free space), About (version, board, chip revision).
- Fully keyboard-operable through the `ugui` focus.
- **Acceptance:** each setting by touch and by keys only; persistence; `WIN_EV_SETTINGS` reaches the reader (text size change relayouts it).

### 71.7 — Launcher: Shortcuts and Files
- Tab bar; Shortcuts face with the persisted list, action sheet, defaults from `/flash0`.
- Files face: roots, traversal, path bar, virtual list, association icons, metadata names (H1 cache, book catalog), open/open-with/add-to-shortcuts/info/delete-with-confirm.
- Books root as a cover grid.
- `Cmd+Space` global hotkey.
- **Acceptance:** a scripted keyboard-only session (host or QEMU with `ramscreen`) browses from `/sd0` to a Markdown file, opens it, adds it as a shortcut, removes the shortcut; the same sequence by `touch inject` on the Tab5; a directory with 500 entries scrolls at frame rate.

### 71.8 — Reader core for large documents
- Layout index, chunked build with progress, visible-block rendering, byte-offset positions, link/TOC collection during indexing.
- **Acceptance:** a 1 MB generated Markdown file opens with the first screen in ≤ 1 s on the Tab5; scrolling anywhere in it meets the 2.1 targets; memory use is reported by `reader list`; the existing reader tests (`tests/test_markdown.py`, showcase document) render identically.

### 71.9 — epub converter, catalog, fonts
- `host/booktools/` uv project: `epub2lmd.py`, fold table, catalog writer, cover → `.GIC`, report; tests on small generated epubs (checked in under `tests/books/`). The owner provides a source tree of DRM-free books for integration testing; it stays outside the repository and is referenced by a path option (`--books-src`, or `LUGALOS_BOOKS_SRC`). The converter runs over the whole tree, and its report is the 71.9 acceptance record.
- Standard-density serif and its AA variant extended to Latin-1 plus `– — ‘ ’ “ ” … •` (`tools/gen_serif_font.py`); the reader decodes UTF-8 for both font sets.
- `tools/create_sd_image.py` / `p9sync.py`: an option to include a `BOOKS` tree.
- **Acceptance:** converter tests pass under `pyrefly`; the owner's book tree converts and syncs to `/sd0/BOOKS` with `p9sync.py`; a German and an English book from it convert with zero unknown characters and render umlauts on the Tab5 and the 7B; RP2350 flash growth from the font extension is recorded and stays within budget (or the extension is P4-only).

### 71.10 — Reader book mode
- Paging, tap zones, claimed horizontal swipe, middle-tap toolbar, chapter chaining, status line, per-book position, "Continue reading" shortcut, text size from settings.
- **Acceptance:** a person reads ten chapters on the Tab5 (portrait) by touch only and on the 7B by keyboard only; position survives close, rotation and reboot; page-turn latency meets 2.1.

### 71.11 — Verification and documentation
- Hardware suites: `tests/hw/test_esp32p4.py` gains launcher-faces, settings persistence, gesture-claim, book-mode and `uxbench` regression checks (fail if a bench row gets > 20 % slower than the recorded baseline).
- `tests/hw/lcdterm_shots.py` scenarios: launcher both faces, settings, portrait pair, book page.
- Docs: `doc/user/ribbon.md` (portrait, gestures), `doc/user/reader.md` (book mode), new `doc/user/launcher.md` and `doc/user/books.md` (converter usage), `doc/user/shell.md` (`open`, `apps`, `settings`), `doc/dev/` page for the view framework, events and `ugui` v2. `python3 tools/gendocs.py --all` and `--check`.
- RAM baselines re-recorded; `sizereport` for RP2350 shows no SRAM growth.

---

## 4. Decisions (confirmed 2026-11-05)

1. **Surface format on P4: RGB565** (option A). It reuses the proven PPA SRM path with no new driver: the Tab5's rev v1.3 cannot read GRAY8 in SRM, and 32 MB PSRAM makes the memory cost irrelevant. Spike 71.1a is reduced to measuring RGB565 surfaces against today's path and tuning the tiled CPU fallback. Option B is dropped.
2. **Ribbon gestures:** a horizontal swipe anywhere moves the ribbon in views that do not claim it (terminals, canvases). Claiming views (book mode, sliders, chess drag) keep their swipes. Edge and chrome swipes always reach the window manager, **including from the full-screen games moonbase and ceres** (2.2).
3. **Books live in `/sd0/BOOKS`**, synced by `p9sync.py`, not part of the default `sd_root` image. The owner provides a DRM-free source tree for testing (71.9).
4. **Images in books: alt text only** in this phase. Images are listed as a future extension (§5).
5. **RP2350-terminal: reduced support.** It gets the view framework, the registry and the launcher (build-gated, PSRAM-backed). Portrait, books and the Latin-1 font are not included unless the flash budget allows.

## 5. Out of scope — future extensions (accepted 2026-11-05)

- **Book images and inline figures**: the converter writes epub images as gray image files next to the chapters (4-bpp, sized for the panel, cover-style dithering for 1-bpp). The reader draws them as image blocks through the `.GIC` blit from 71.3/71.4, and the layout index gets an `image` block kind with known height. Phase 71 lays the groundwork: alt text keeps the image's place in the text, the icon format and blit exist, and the converter's report counts the dropped images.
- Hyphenation; justified text.
- **Phase 72 candidate: `ugui` for C programs (chibicc).** C programs run in U-mode under memory domains, so this needs its own design: `ugui` as a user-space library compiled into the program (it is freestanding and allocation-free; the question is the font tables' visibility from U-mode), a surface mapped into the program's domain, and an event syscall for the program's window. Same immediate-mode API as 71.4b's Lisp one. Decided later whether and when (owner, 2026-11-06).
- File copy/move/rename across volumes; search across the catalog.
- Multi-touch (pinch zoom) — the ST7123 reports up to 10 points, the GT911 5; the recogniser in 71.2 reads only the first point, but its contact model is written so a second point can be added.
- Colour UI accents on RGB565 surfaces.
- SD-provided app manifests beyond the minimal form in 2.5.
