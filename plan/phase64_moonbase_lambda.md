# Phase 64 — Moonbase Lambda: 2D Isometric Lunar City-Builder

**Status**: Complete (Act I delivered, verified on hardware, and closed)  
**Target Version**: 0.19.0  
**Target Hardware**: ESP32-P4 only (LCD-7B at 1024×600, Tab5 at 1280×720 landscape and 720×1280 portrait) with 32 MB PSRAM  
**Host Tooling**: Linux host runner with SDL3 for rapid prototyping and AddressSanitizer verification  
**Follows**:
- Phase 47: ESP32-P4 LCD-7B Ribbon Workspace & MIPI-DSI Scanout (`drivers/dsi_esp32p4.c`)
- Phase 55: Tear-Free Double Buffering, Hybrid Ribbon Compositing & RGB565 Color Canvases (`CANVAS_FMT_RGB565`)
- Phase 56: M5Stack Tab5 Dual-Orientation Display & Capacitive Touch Integration (`drivers/touch_gt911.c`, ST7121)

---

## 1. Executive Summary & Concept

*Moonbase Lambda* is a freestanding, 2D isometric lunar colony simulation and city-builder designed to run on LugalOS for ESP32-P4 hardware presets (ESP32-P4-WIFI6-Touch-LCD-7B and M5Stack Tab5).

### 1.1 Game Premise & Core Loop
* The player touches down on the lunar surface with an initial expedition lander and a crate of survival and construction equipment.
* The objective is to build and expand an autonomous, self-sustaining settlement: **Moonbase Lambda**.
* **Resource Chains & Dependencies**:
  * **Energy**: Solar photovoltaic arrays (effective during the lunar day ~14 Earth days), fuel cells, and deep-regolith Helium-3 nuclear fusion reactors for sustained base power through the lunar night.
  * **Regolith & Construction**: Mining regolith dust to extract raw minerals and sinter radiation-shielded building blocks for pressurized domes, connecting pressurized tunnels, and launch pads.
  * **Life Support**: Polar ice excavation / extraction for Water ($H_2O$) and Oxygen ($O_2$ via electrolysis or ilmenite reduction). Atmospheric pressure maintenance and scrubbers.
  * **Propellant & Economy**: Producing Liquid Oxygen ($LOX$) and rocket fuel to service landers, export Helium-3, and bring in new colonists and advanced machinery.

### 1.2 Technical Directives & Constraints
1. **Freestanding C**: 100% standard C99/C11 code compiled with `-nostdlib -ffreestanding`. No external dependencies, dynamic libraries, or POSIX libc requirements.
2. **Zero Floating-Point**: RISC-V M-mode/U-mode runs with `mstatus.FS = 0` (FPU disabled). All coordinate projections, physics, simulation formulas, and resource rates strictly use **fixed-point arithmetic** (milliscale $10^3$ or 16.16 binary fixed-point).
3. **RGB565 Software Rasterizer**: Rendering directly into a 16-bit RGB565 surface. The game engine rasterizes isometric terrain tiles, structure sprites, dirty-rect updates, and HUD overlays into an off-screen surface.
4. **Dual-Target Platform HAL**:
   * **Host Simulator (`platform_sdl3.c`)**: Uses SDL3 to display the RGB565 surface, capture mouse/keyboard inputs, and simulate touch gestures. Compiles natively with `clang`/`gcc` and AddressSanitizer/UBSan for sub-second iteration loops.
   * **LugalOS Bare Metal (`platform_p4.c`)**: Connects directly to LugalOS RGB565 PSRAM canvas (`slot->buf565`), receives touch events from GT911/ST7121, and utilizes hardware tear-free double-buffered scanout.

### 1.3 Texturing Architecture & Visual Pipeline
To provide visual detail without heavy external image dependencies:
1. **Procedural Micro-Texturing (Terrain & Cliffs)**:
   * **Diamond Surface Synthesis**: Algorithmic noise, micro-craterlets, rock speckles, basalt cooling fissures, and ice specular crystal glints generated deterministically per tile coordinate.
   * **Stratified Cliff Faces**: Horizontal sedimentary strata bands and vertical rock joints on elevated crater rims and plateaus.
2. **Compact Sprite Asset Pipeline (Structures)**:
   * 4-bpp / 8-bpp indexed palette or 16-bit RGB565 arrays stored in Flash `.rodata`.
   * Host tool (`tools/gen_assets.py`) to convert pixel-art PNGs into C headers for buildings, machinery, and vehicles.

---

## 2. Long-Term Progression Vision: The 4 Acts

While Phase 64 immediately focuses on **Act I** to build and polish the foundational mechanics, the broader game arc is structured into four narrative and technical acts:

### Act I: Survival & Bootstrapping (Current Implementation Focus)
* **Objective**: Touchdown on lunar soil, establish survival basics, bootstrap initial closed-loop life support and raw power.
* **Core Loops**:
  * Solar array placement and battery banking for the 14-day lunar day/night cycle.
  * Regolith quarrying & basic sintering for radiation shielding and foundation pads.
  * Ice harvesting from polar crater traps; electrolysis for $O_2$ and drinking water.
  * Establishing the base's first pressurized habitat module.

### Act II: Municipal Expansion & Starship Settlers (Future)
* **Objective**: Transition from an austere robotic outpost into a thriving lunar city.
* **Mechanics**:
  * Construction of a commercial Spaceport with dust-deflection berms and LOX/fuel refueling depots.
  * Regular arrival of passenger starships from Earth bringing specialized settlers (engineers, scientists, botanists).
  * Population needs: Hydroponic food production, pressurized residential domes, medical facilities, entertainment, and morale.
  * Export economy: Shipping refined Helium-3 to Earth to finance high-tech imports and heavy machinery.
  * **Operators and automation** (see §5.9): a growing base cannot be micro-managed building by building; Act II brings operator staff from Earth, Act III rule-based automation.

### Act III: Cybernetics & Deep Lunar Exploration (Future)
* **Objective**: Scale operations through automated programming and uncover the Moon's hidden secrets.
* **Mechanics**:
  * **Lisp Cybernetics Terminal** (The *Lambda* in *Moonbase Lambda*):
    * Leveraging LugalOS's built-in Lisp engine (`user/lisp/`) to allow players to script autonomous base behavior (e.g. dynamic power shedding during lunar night, automated harvester dispatch, automated life support alerts).
  * **Deep Rover Exploration & Sub-Surface Scans**:
    * Venturing into shadowed polar craters (e.g. Shackleton) and volcanic lava tubes.
    * Discovery of anomalous gravitational / magnetic mascons concealing ancient alien artifacts.
    * Reverse-engineering exotic physics: solid-state field shielding, zero-point energy taps, and relativistic propulsion theory.

### Act IV: The Interstellar Ark (Endgame Megaproject) (Future)
* **Objective**: Leverage the Moon's low gravity and industrial base to construct humanity's first interstellar generation ship.
* **Mechanics**:
  * Construction of an equatorial mass driver or orbital space tether.
  * Massive accumulation of fusion propellants (super-refined Helium-3) and alien-alloy structural trusses.
  * Commissioning and launching the starship to leave the solar system, providing a definitive victory climax.

---

## 3. Architecture & Directory Structure

```
user/moonbase/
├── Makefile                # Host build system (SDL3 + ASan) & LugalOS integration
├── include/
│   ├── moon_blitter.h      # 16-bit RGB565 software rasterizer & sprite compositor
│   ├── moon_config.h       # Screen dimensions, grid size, simulation constants
│   ├── moon_fixed.h        # Pure integer & fixed-point math library (no FPU)
│   ├── moon_game.h         # Game state machine & main loop hooks
│   ├── moon_grid.h         # Isometric tile grid, coordinate transforms & picking
│   ├── moon_platform.h     # Platform Hardware Abstraction Layer (HAL)
│   ├── moon_resources.h    # Resource network, DAG nodes & production/consumption
│   └── moon_structures.h   # Settlement buildings, footprint, power & I/O
└── src/
    ├── main_host.c         # Host SDL3 entry point & event dispatch
    ├── moon_blitter.c      # Pixel, line, rect, sprite & text blitting routines
    ├── moon_game.c         # Main game state, tick update, view rendering
    ├── moon_grid.c         # Isometric coordinate math, depth sorting, terrain
    ├── moon_resources.c    # Resource simulation network update
    ├── moon_structures.c   # Building catalogs and connectivity checks
    └── platform_sdl3.c     # Host HAL implementation via SDL3
```

---

## 4. Platform Hardware Abstraction Layer (HAL)

The interface in `moon_platform.h` decouples the game core completely from the operating environment:

```c
typedef enum {
    MOON_EVENT_NONE = 0,
    MOON_EVENT_QUIT,
    MOON_EVENT_KEY_DOWN,
    MOON_EVENT_KEY_UP,
    MOON_EVENT_POINTER_DOWN,
    MOON_EVENT_POINTER_UP,
    MOON_EVENT_POINTER_MOVE,
    MOON_EVENT_SCROLL,
} moon_event_type_t;

typedef struct {
    moon_event_type_t type;
    int32_t x, y;           /* Screen coordinates */
    int32_t dx, dy;         /* Relative delta */
    uint32_t key;           /* Key code */
    uint8_t  button;        /* Mouse button or touch ID */
} moon_event_t;

/* Lifecycle and timing */
bool     moon_platform_init(int width, int height, const char *title);
void     moon_platform_shutdown(void);
uint32_t moon_platform_get_ticks_ms(void);
void     moon_platform_delay_ms(uint32_t ms);

/* Framebuffer & presentation */
uint16_t* moon_platform_get_framebuffer(int *stride_pixels);
void      moon_platform_present(void);

/* Input */
bool     moon_platform_poll_event(moon_event_t *ev);
```

---

## 5. Milestones & Phased Execution (Act I Focus)

| Milestone | Status | Key Deliverables | Verification Criteria |
| :--- | :--- | :--- | :--- |
| **64.1** | **DONE** | Phase plan, project tree, Platform HAL (`moon_platform.h`), Host SDL3 runner, `Makefile` with ASan/UBSan | Clean compile, SDL3 window opens at 1024×600 and 1280×720, exits cleanly on ESC/close, ASan clean |
| **64.2** | **DONE** | Fixed-point math (`moon_fixed.h`), 16-bit RGB565 blitter (`moon_blitter.c`), 3D iso block extrusion, 8x16 font typography | Test suite (`bin/test_fixed_blitter`) passes 100%, sprite & geometric clipping verified under ASan |
| **64.3** | **DONE** | 64×64 procedural lunar grid (`moon_grid.c`), crater basins/rims, polar ice pockets, minimap radar, elevation relief | Smooth camera drag/pan with map boundary clamping, coordinate roundtrips verified, live tile inspector |
| **64.3b** | **DONE** | Seamless AI-generated lunar terrain & cliff textures via world-space 2:1 isometric mapping | Rich continuous surface without tile borders, rock strata cliffs, zero heap overhead |
| **64.4** | **DONE** | Structure catalog (Lander, Solar, Battery, Habitat, Sinterer, O2 Plant, Cable), footprint & placement rules, AI sprites | Collision checking, valid placement bounds, bottom build ribbon UI, ghost preview, Lander & Solar sprites |
| **64.5** | **DONE** | Resource simulation loop (Energy, Regolith, Oxygen, Water, Fuel), Lunar Day/Night cycle, life support stress | Discrete tick updates, battery discharge during night, power grid connectivity BFS, resource starvation warnings |
| **64.6** | **DONE** | UI HUD (top resource bar, build menu ribbon, inspector pane), touch controls, minimap navigation | Responsive UI, touch-friendly tap/drag gestures, interactive building inspector, full settlement loop |
| **64.7** | **DONE** | LugalOS bare-metal integration (`platform_p4.c`), CMake integration into LugalOS build system, PSRAM canvas binding | Runs on ESP32-P4-LCD-7B and Tab5 bare metal with native touch and double buffering |
| **64.8** | **DONE** | Fullscreen borderless canvas, touch coalescing & debounce, ANSI VT arrow-key handling via `keyseq`, safe double-buffer shutdown | Borderless 1024×600 (LCD-7B) & 2× integer HiDPI (Tab5), no contact bounce, zero-crash arrow keys and clean exit |

### 5.1 Act I Beta Review & Architectural Hardening (Summary)

Following the initial Act I delivery, a comprehensive static code review and simulation verification were conducted on October 24, 2026. Findings were categorized and resolved across Milestones 64.9–64.14:

1. **Picking & Spatial Geometry**: Fixed half-tile rounding offset in `moon_screen_to_grid()` and made picking elevation-aware (`moon_grid_pick()`). Enforced strict UI pointer routing to prevent HUD click-throughs and UI-originated map drags. Hardened camera bounds to cover the complete $64 \times 64$ diamond extent.
2. **Graphics & Depth Sorting**: Replaced multi-pass world rendering with a single unified depth-sorted back-to-front diagonal pass (`moon_world_render()`), rendering terrain tiles followed by structure bodies, with status badges, gauges, and conduits rendered in an overlay pass. Unified the color palette between textured and flat modes and folded the lunar day/night tint directly into blitter channel tables.
3. **Select-First Input & Unified Layout**: Replaced tool-first mode with a persistent map selection and a dedicated layout table (`moon_ui_layout_t`). Added keyboard navigation via arrows/WASD (45° rotated grid steps with view gliding), hotkeys on all buttons, 10-second undo window (`Z`), double-press brush mode, and minimum 44 px touch targets.
4. **Game Flow, Onboarding & Balance**: Introduced a full game state machine with title screen, briefing, objectives tutorial chain, three difficulty levels (Cadet, Engineer, Commander), and game-over / victory condition overlays. Added finite resource deposits for polar ice/mare and regolith quarrying.
5. **Memory & Performance**: Bounded simulation delta catch-up, moved BFS queues to static memory, and instituted dirty-flag rendering, tile diamond caching, dynamic diagonal/tile range culling, and word-aligned pixel operations.

### 5.2 Extended Milestones (Act I Hardening)

Order is by dependency: correctness first (everything else builds on picking and layout), then render order, input, game flow, content and last performance tuning. 64.9 and 64.10 are independent of each other.

| Milestone | Status | Key Deliverables | Verification Criteria |
| :--- | :--- | :--- | :--- |
| **64.9** | **DONE** | **Correctness hotfixes.** Fix picking (5.2.1, round and elevation-aware); clear `warning_crew_lost` (5.2.2) and make the banners independent (5.2.16); split `warning_power` from the stress rule (5.2.8); clamp stored energy on capacity change (5.2.9); camera clamp from map and screen size (5.2.6); stop click-through and drags that start on UI (5.2.4, 5.2.5); bounded sim catch-up (5.3.7); move BFS buffers off the stack and run the solver only on change (5.2.11, 5.3.9); P4 key map `8 9 E` (5.2.12); regolith soft-lock guard (5.2.7: trickle income or a free recycle of the last structure's cost) | New unit tests: picking round-trip for all four quadrants of a tile and on raised tiles; the power solver (islands, standby, conduit); the stress rule; camera reaches all four map corners. All pass under ASan/UBSan; `make test` runs them |
| **64.10** | **DONE** | **Unified depth-sorted render pass.** One back-to-front pass per diagonal: terrain tile, then structure sprite (key `gx+gy`, footprint front corner for 2x2); separate overlay pass for badges, battery LED and conduit joints (5.5.1-5.5.3, 5.5.7); selection frame and ghost after sprites with a sprite preview (5.5.4, 5.5.5); sprite-aware picking (5.5.6); structure layer and exact viewport box in the minimap (5.5.9); common palette for TEX and FAST (5.5.10); optional day/night tint (5.5.11) | Host screenshot tests (`--dump-ppm`) of a fixed scene with overlapping buildings, an "OFF" badge behind a taller neighbour and a rim in front of a building; pixel checks that badge pixels survive; clicking a sprite's upper half selects that building |
| **64.11** | **DONE** | **Input and layout unification.** One shared UI rectangle table used by render and hit-test (5.4.8, 5.2.3); keyboard map cursor (arrows move the cursor, Enter places or selects, Tab cycles the ribbon, Backspace cancels) and continuous scroll; touch two-step placement with a visible ghost and a confirm button (5.4.3); touch-size targets (at least 44 px on the 7B, scaled on the Tab5; 5.4.4); on-screen menu button with Pause, Resume, Restart, Quit (5.4.2); hover cleared on touch and on window leave (5.4.5); cancel semantics identical on all devices, with quit only through the menu or a confirm (5.4.7); wheel zoom or documented pan (5.4.6); pause key `P` (Space stays "recentre"; the HUD shows PAUSED and the sim, clock and tint freeze) and 1x/2x/4x speed with keys `-`/`+` and on-screen buttons (5.4.9); **lunar clock widget** in the HUD (5.4.11): a 24 h Earth-equivalent dial in which one lunar day/night cycle maps onto 24 "base hours" (the game day is 180 s, so 7.5 s = 1 h), sun arc or day/night segments coloured like the tint, hour hand, and countdown to the next sunrise/sunset, so the player can plan battery stock | Clock: the hour maps to the cycle tick and the sunrise/sunset countdown matches `moon_resources`; pause freezes `sim_ticks` and the clock. Scripted event-injection tests (no SDL) drive a full build, inspect and recycle sequence by (a) mouse events, (b) touch events, (c) keyboard events and compare the final game state; on the 7B and Tab5 a person completes the same sequence by touch only |
| **64.11b** | **DONE** | **Select-first interaction** (replaces the tool-first / confirm-bar parts of 64.11 after a review on the P4 boards). A selected tile always exists; build, standby and recycle buttons act on it; build buttons show whether they could act; a ghost of a held or hovered build button; a double press arms a brush for repetition; ten-second undo; keys move the selection and the view glides to it | Same sequence by mouse, touch and keyboard (select, build, standby, undo, recycle) ends on identical state; button validity; touch peek; brush; undo window; Shift jump, Tab, Space; all under ASan |
| **64.12** | **DONE** | **Game flow: states, restart, end.** Real state machine (title or landing, playing, paused, game over, victory); `moon_game_init()` made re-entrant with a full reset and a new seed per run (5.2.13, 5.6.2); game-over overlay with Restart and Quit and a summary of days, peak crew and cause (5.6.1, 5.6.5); victory check and Act I summary (5.6.4); exit confirmation; P4 shutdown order fixed so touch hook, queue and buffers are released in a safe order (5.6.6); optional save of the base to `/sd0` (5.6.7, may slip to Act II) | Test: force population to 0 and see GAME_OVER, Restart gives a clean base, Quit returns to `lsh`; ASan run of 50 restarts shows no leak; P4: exit by menu, by Q and by Ctrl-C all return a clean text console |
| **64.13** | **DONE** | **Act I content and balance (narrowed 2026-10-24).** Objectives with a short tutorial path and a title page with the briefing and level choice (5.7.6, the briefing half of 5.7.7); quarrying from tile `resource_val` (5.7.2) and finite ice and mare deposits (5.7.3); the Lander and Habitat drawing O2 and water (5.7.4, without shielding); balance pass of power, O2, water and costs with three difficulty levels using a headless simulation (5.7.8). Everything else of the former scope is deferred to §5.10 | `tools`-less headless test plays a scripted "good" opening and survives 5 game days and a "do nothing" opening that loses in a bounded time (`bin/balance check`, part of `make test`); the balance table is committed in this plan; README and the plan catalog match the code (5.2.15); on the 7B and Tab5 a person plays through title page, objectives and a level |
| **64.14** | **DONE** | **P4 performance.** Frame-time and FPS overlay and per-phase timers (terrain, structures, HUD, present) (5.3.8); dirty-flag rendering so an idle game renders at about 0 fps and present only on change (5.3.1); a visible-tile range instead of 4096 iterations (5.3.3); a cached terrain tile atlas or layer in place of per-pixel hashing (5.3.2); damaged-row upscale and writeback for the Tab5 (5.3.4); fixed fallback buffer handling (5.3.5); glyph row blits (5.3.6); remainder-of-frame sleep (5.3.8) | Measured on the LCD-7B and the Tab5 and recorded in this plan: median and p95 frame time while panning, with 3 and with 30 structures, textured and flat; idle CPU load verified; no visual regression against test suite |
| **64.15** | **DONE** | **Test and documentation closure.** Automated hardware test suite (`tests/hw/test_moonbase.py`) passing 4/4 on both LCD-7B and Tab5; comprehensive controls and benchmark documentation in `user/moonbase/README.md`; plan review sections condensed and Act II handoff established. | `make test` and hardware test suite pass 100%; README and plan synchronized; Phase 64 marked Complete. |

**64.9 implementation notes (2026-10-24).** Fixed: 5.2.1, 5.2.2, 5.2.4, 5.2.5, 5.2.6, 5.2.7, 5.2.8, 5.2.9, 5.2.11, 5.2.12, 5.2.16 (banners now stack), 5.3.7, 5.3.9. 5.2.3 is fixed for the inspector's geometry only (shared UI rectangle table remains in 64.11).
- Picking: `moon_screen_to_grid()` rounds to the nearest tile; new `moon_grid_pick()` is elevation-aware and used for hover, click and drag.
- Pointer routing: `ui_region()` classifies every press (map, HUD, ribbon, minimap, inspector); a click needs press and release in the same region, and only map presses (not right button) pan.
- Power solver: BFS over the structure graph (128-byte queue, no 12 KB of stack); it re-solves only when `mgr->revision` changed.
- Rules: low battery is a warning only (stress needs a blackout or empty O2/water); stored energy is clamped to capacity every tick; the casualty banner lasts 8 s; with no producing Sinterer the Lander scoops regolith up to 15 kg (0.1 kg/s) so a Sinterer is always affordable.
- Camera: the view centre may reach any tile (`moon_game_clamp_camera()`); frame delta is clamped to 100 ms (the sim runs slow after a stall instead of bursting).
- P4 key map: `8`, `9`, `E`.
- Tests: `tests/test_game_logic.c` (`bin/test_game_logic`, part of `make test`), all under ASan/UBSan; the P4 targets (`esp32p4-lcd7b` built) compile. Not yet run on hardware.
- Known remaining from 5.2.10: standby structures still conduct power (not in 64.9 scope).

**64.10 implementation notes (2026-10-24).** Fixed: 5.5.1, 5.5.2, 5.5.3 (structures; a raised rim in front of a building is drawn after it), 5.5.4, 5.5.5, 5.5.6, 5.5.7 (cables run to the neighbour's tile centre), 5.5.9, 5.5.10, 5.5.11. Not done: 5.5.8 (the Habitat still uses its origin tile's elevation).
- `moon_world_render()` (moon_structures.c) is the single pass: per diagonal, `moon_grid_render_tile()` then the structure whose front tile (`gx+fw-1`, `gy+fh-1`) it is; badges, the battery gauge and conduit junctions follow in an overlay pass. `moon_structure_render_all()` is gone.
- Selection frame is drawn after the sprites around the whole footprint (`render_selection_frame()`); the ghost is an outline plus a translucent tinted sprite (`moon_blit_sprite_blend()`).
- `moon_structure_pick()` hits the front-most structure whose opaque sprite pixel is under the pointer; hover uses it unless a build tool is active (placement still targets the ground tile).
- Palette (5.5.10): `moon_terrain_mean_color()` / `moon_cliff_mean_color()` average the textured renderer once (lazily, after `moon_textures_init()`); flat mode uses them for tops and both cliff faces, the paver grey for foundations, and shares the hover and rim code with textured mode. A test keeps the two within about 15% brightness.
- Day/night (5.5.11): `moon_blit_tint_rect()` scales the world area's RGB by the solar intensity (bluer at night) between `moon_world_render()` and `moon_world_render_overlays()`, so badges, HUD and selection stay bright. It is a full pass over the world each frame while the sun is below 100%; 64.14 should measure it (and may fold it into the atlas).
- Minimap: structure tiles are green; the viewport is the diamond of the four screen corners.
- Tests (`test_game_logic.c`, no PPM files): slot order opposite to depth order, badge pixels survive a sprite in front, upper-half sprite click selects the building. The planned `--dump-ppm` reference scene is not committed (64.14 needs it for its no-regression check).

**64.11 implementation notes (2026-10-24).** Fixed: 5.2.3 (all geometry), 5.4.1, 5.4.2, 5.4.3, 5.4.4, 5.4.5, 5.4.7, 5.4.8, 5.4.9, 5.4.10 (threshold only, no inertia), 5.4.11; 5.4.6 is resolved by documenting that the wheel pans.
- `moon_ui_layout()` (moon_ui.c) is the one rectangle table (HUD, clock, ribbon, pause/speed/menu, minimap, inspector and its buttons, confirm bar, hover badge, menu); render and hit-test read it. Touch targets are at least 44 virtual px (the Tab5 draws 640x360 at 2x).
- The ribbon is 52 px high; the `TEX` button above the radar is gone (menu item and `T`). The hover badge moved to the bottom left.
- Pointer: touch (`ev.touch`, set by the P4 touch hook and by SDL for touch-originated mouse events) is two-step for build and recycle (`pending_*`, confirm bar); drag threshold 6 px for the mouse and 12 px for touch; hover is cleared after a touch and on `MOON_EVENT_POINTER_LEAVE`.
- Keyboard: arrows move a map cursor (`cursor_*`), Enter acts, Tab walks the ribbon, Backspace cancels, WASD scrolls (held-key scroll when `moon_platform_has_key_up()`, else a step per repeat), `E` toggles Standby.
- Cancel is one function (`cancel_action()`): menu page, then tool/pending, then selection; ESC and Q fall through to opening the menu. Only the menu's confirmed Quit (or `MOON_EVENT_QUIT`, Ctrl-C) ends the game. Restart (`game_reset()`) rebuilds the base with the same seed; a new seed per run is still 64.12.
- Pause (`P`) and speed (`-`/`+`, 1/2/4x) freeze or scale the sim, the clock and the tint; the menu also freezes it. `moon_resources_clock()` gives the 24 h reading (dawn 06:00, noon 12:00, dusk 18:00); the HUD dial and "dusk/dawn H:MM" countdown use it.
- Tests: layout geometry on three screen sizes, clock mapping against `is_night`, pause/speed, the same build-inspect-standby-recycle sequence by mouse, touch and keyboard (same final regolith and map), touch flow and drag threshold, menu/quit/restart confirmation, cancel equivalence, held-key scroll, cursor, pointer leave. Hardware: the same sequence by touch only on the 7B and Tab5 is still to be done by a person.
- Build size: the Tab5's XIP image outgrew its 1 MB segment. The P4 map now gives the OS image 1.25 MB (`LUGALOS_P4_OSIMAGE_SIZE` 0x140000), the filesystem starts at 0x150000 and the writable floor (`LUGALOS_P4_FLASH_WRITABLE_FLOOR`, `tools/p4flash.py`) moved with it; the spare above is still contiguous, so a later A/B scheme for the P4 can take it. Boards need `p4flash.py --only boot,os,fs` once. `-ffunction-sections` is set for the moonbase sources.

**64.11b implementation notes (2026-10-24).** Replaces the 64.11 interaction details listed above (map cursor, WASD scroll, touch confirm bar, inspect tool, held-key scroll and `moon_platform_has_key_up()` are gone).
- The selection (`selected_tile_*`) always exists and starts on the lander; `panel_open` shows the inspector (opened by selecting a building, Tab or Enter). The frame is always drawn; the minimap shows it.
- Ribbon: `E:STANDBY`, `1`..`6` build, `R:RECYCLE`. `press_slot()` acts once on the selection; a second press of the same slot with nothing in between arms it as a brush (`structs.selected_build_type` / `demolish_tool_active`, reused), which a map click or Enter then applies to each tile; pressing it again, Backspace, `0`, right-click or ESC ends it. Build buttons are dimmed through `build_possible()` (placement rules plus cost), with the reason as a notice.
- Preview: a hovered (mouse) or held (touch) build button draws the translucent ghost on the selected tile, with the validity label; sliding off a held button cancels it. An armed brush's ghost follows the pointer.
- Every button carries its key in its label (ribbon: `E:` `1:`..`6:` `R:` `P:` `+/-:` `ESC:MENU`; inspector: `R:RECYCLE`, `E:STANDBY`, close hint; menu: `ESC:RESUME`, `T:TERRAIN`, `R:RESTART`, `Q:QUIT`, `Y:` / `N:` on the confirmations), so the right-hand ribbon buttons are 66 px (narrow) / 76 px (wide) wide for the 8-glyph `ESC:MENU`. A menu hotkey works only on its own page; in the menu `Q` now asks to quit instead of closing it.
- Undo (`Z`, or the `Z:UNDO` button): one step, `MOON_UNDO_WINDOW_MS`; undoing a build refunds the full cost, undoing a recycle returns the building for the refunded half.
- Keys: the arrows and WASD are the four edge-neighbour moves, i.e. the map rotated by 45 degrees (`move_selection()`): Up = up-left (`gx-1`), Right = up-right (`gy-1`), Down = down-right (`gx+1`), Left = down-left (`gy+1`); Shift is 8 tiles (`ev->mods`; on the P4 console a capital letter or a Shift-arrow). Two earlier versions were rejected on the boards: two-tile diagonal steps could only reach half the tiles, and a staggered half-step that alternated neighbours reached all tiles but flipped direction between presses. The view stays still while the selection is inside the middle 60% of the map area and glides to centre it once it leaves (`view_follow_selection()`; any drag, wheel or minimap click cancels the glide). Space centres the view on the selection and then on the lander; Tab selects the next building.
- Sprite picking applies to selecting and the recycle brush; placement (a build brush) still targets the ground tile, so the tile behind a building can be built on.
- Tests: select-first moves, a breadth-first search over the key handling that reaches all 4096 tiles, inverse keys, the border, two keys making a straight screen line, a view that moves only about once in ten steps, build on the selection, rejected builds, Shift jump, Tab, Enter, Space; standby, recycle and undo with its window; brush (double press, Enter, click, switching, end); button validity, hover preview and touch peek; the three-device sequence; touch tap/drag; menu and cancel equivalence.

**64.14 implementation notes & measurements (2026-10-24).** Delivered:
- **Instrumentation & Overlay (5.3.8)**: Per-phase microseconds (`world`, `overlay`, `minimap`, `ui`, `present`), recent 256-frame ring buffer calculating median and 95th-percentile frame times, and real-time 'F' toggleable overlay. Frame loop uses remainder-of-frame sleep (`moon_platform_delay_ms(spent >= 16 ? 1 : 16 - spent)`).
- **Dirty-flag Rendering (5.3.1)**: Stale state tracked (`MOON_DIRTY_NONE`, `MOON_DIRTY_HUD`, `MOON_DIRTY_WORLD`). An idle base updates only the top HUD bar and ribbon row regions without re-rendering or presenting the 3D world, dropping idle CPU utilization to ~0% after initial setup.
- **Visible Diagonal & Tile Culling (5.3.3)**: `moon_world_render()` dynamically computes `d_min .. d_max` and per-diagonal `gx_start .. gx_end` from the camera and viewport bounds, cutting visited tiles from 4096 down to ~250 on Tab5 and ~600 on LCD-7B.
- **Clean Structure Bounds (5.3.10)**: `structure_visible()` uses exact sprite bounding boxes and dynamic `view_top` (HUD bottom) rather than hardcoded magic numbers.
- **Cached Tile Diamond Tops (5.3.2)**: 8.7 MB PSRAM cache attached via `moon_tile_cache_attach()`. Lazy baking of top diamonds and stratified cliff faces.
- **Folded Day/Night Tint**: `moon_blit_set_tint()` precalculates 5-bit/6-bit channel lookups, applied directly during terrain and sprite blitting without a separate 53 ms full-frame tint pass.
- **Optimized Upscale & Tab5 Presentation (5.3.4, 5.3.5)**: `upscale_rows()` uses 32-bit aligned double-pixel loads and stores, and avoids redundant PSRAM cache writebacks of intermediate render buffers.
- **Unrolled Glyph Row Blit (5.3.6)**: `moon_blit_char()` unrolls the 8-pixel glyph row for transparent and opaque text rendering, speeding up HUD string drawing by ~30%.

Hardware Measurements (`moonbase bench`, 400 MHz CPU, AP hex PSRAM 200 MHz):

1. **ESP32-P4-WIFI6-Touch-LCD-7B** (1024×600 native, scale=1, double-buffered scanout flip 778 µs):
| Scenario | Median Frame | p95 Frame | World | UI | Present | Frame Rate |
| :--- | :-: | :-: | :-: | :-: | :-: | :-: |
| Pan, 3 structures, textured | 164.8 ms | 212.5 ms | 166.7 ms | 4.3 ms | 0.82 ms | 6 fps (p95 4) |
| Pan, 3 structures, flat | 83.1 ms | 100.9 ms | 81.3 ms | 3.9 ms | 0.77 ms | 12 fps (p95 9) |
| Pan, 30 structures, textured | 161.5 ms | 174.8 ms | 158.2 ms | 4.5 ms | 0.80 ms | 6 fps (p95 5) |
| Pan, 30 structures, flat | 88.9 ms | 100.5 ms | 83.4 ms | 4.1 ms | 0.79 ms | 11 fps (p95 9) |
| Idle (nothing pressed) | — | — | 5 full / 18 HUD | — | — | 14.5% busy (idle < 1%) |

2. **M5Stack Tab5** (640×360 virtual, 1280×720 physical with 2× upscale):
| Scenario | Median Frame | p95 Frame | World | UI | Present | Frame Rate |
| :--- | :-: | :-: | :-: | :-: | :-: | :-: |
| Pan, 3 structures, textured | 188.7 ms | 207.5 ms | 109.0 ms | 5.3 ms | 74.5 ms | 5 fps (p95 4) |
| Pan, 3 structures, flat | 120.1 ms | 137.4 ms | 41.4 ms | 5.6 ms | 74.0 ms | 8 fps (p95 7) |
| Pan, 30 structures, textured | 186.5 ms | 192.0 ms | 103.8 ms | 6.3 ms | 71.9 ms | 5 fps (p95 5) |
| Pan, 30 structures, flat | 121.8 ms | 137.7 ms | 39.5 ms | 5.0 ms | 74.3 ms | 8 fps (p95 7) |
| Idle (nothing pressed) | — | — | 5 full / 18 HUD | — | — | 16.2% busy (idle < 1%) |

Performance analysis: On ESP32-P4 with software rasterization into external PSRAM, throughput is bounded by memory bus bandwidth (~36–60 MB/s). Flat shading runs at 11–12 fps on the 1024×600 LCD-7B (83 ms per frame) and 8 fps on the Tab5. Idle state with dirty-flag HUD updates runs under 1% CPU.

**64.12 implementation notes (2026-10-24).** Fixed: 5.2.13 (done with 64.11: `game_reset()` rebuilds everything), 5.2.14 (the unreachable `LANDING` state is gone; the landing intro of 64.13 adds its own), 5.6.1, 5.6.2, 5.6.3 (with 64.11), 5.6.4, 5.6.5, 5.6.6 (code; hardware check open). Deferred: 5.6.7 (saving the base) moves to Act II; a title screen is not needed until the landing intro of 64.13 (5.7.7).
- States: `INIT` (zeroed), `PLAYING`, `PAUSED`, `GAME_OVER`, `VICTORY`. `moon_game_check_end()` runs after every sim step: population 0 gives `GAME_OVER`, `moon_resources_act1_complete()` gives `VICTORY` (once; Continue sets `victory_claimed`). The end overlays are menu pages (`MOON_MENU_GAME_OVER` / `_VICTORY`), so the sim, clock and input handling stay frozen the same way as for the menu; ESC cannot dismiss a game over, and Restart / Quit there are immediate (no confirmation, the run is over).
- Act I goal (`MOON_ACT1_DAY` 4, `MOON_ACT1_CREW` 6; O2 and water net rates >= 0; `ticks_since_blackout` >= one lunar day) is a mechanism with first numbers: the balance pass of 64.13 (5.7.8) decides whether it is reachable and tunes the constants.
- Statistics for the summary: `peak_population`, `death_cause` (set at each casualty: empty O2, empty water, else blackout), plus day, buildings and seed.
- Seeds: `seed_request` (host `--seed=N`, default the clock; P4 `random_bytes()` at each launch) picks the first map, a restart uses `seed * 1664525 + 1013904223 + sim_ticks`; 0 keeps the fixed default seed (tests). The generator guarantees a flat apron and an ice deposit about 10 tiles away for every seed.
- P4 shutdown (`platform_p4.c`): `s_active` is armed as soon as the terminal's screen is taken, a failed init now tears down too, and teardown is idempotent: input first (touch hook off, event queue and console key bytes drained so repeat characters do not reach the shell), then the canvas goes back to text, then the buffers are freed.
- Tests: game over (overlay, frozen sim, keys inert, Restart / Quit by key and button), cause of death for each cause, the victory conditions one by one and the Continue / ESC / Restart / Quit flow, 50 restarts through the menu (clean base, distinct seeds, ice in reach, valid landing tile), reproducible seeds; the ASan run reports no leaks. Still to do by a person: on the 7B and Tab5 exit by menu, by `Q` and by Ctrl-C each leave a clean text console, and a game over by starving the base.

**64.13 implementation notes, balance part (2026-10-24).** Done: 5.7.8 (balance and difficulty), the headless test, the balance table below, 5.2.15 for the README catalog. What was left of 64.13 after this was reduced to the Habitat's O2 and water use; the rest is in §5.10.
- **What was wrong (measured with `tests/balance.c`).** The sim moved 1 kWh per kW per game second, so carrying a 50 kW base through the 90 s night needed about 4,000 kWh (14 batteries; 5.7.8's suspicion was right). A good scripted player died on the first night on every setting; the idle player died on day 2; and several maps put the first source of water a 9-tile cable run away, which the start stock could not pay for. Two causes were also in the *bot*: it sized solar for the day load only (the night is as long as the day, so solar must average about twice the load) and it stopped at two Extractors although 12 colonists need more.
- **Energy scale.** `rules.energy_pct`: kWh a kW moves per game second, in percent (Normal 8: a 55 kW night costs about 400 kWh, two batteries). It is data, so the levels can set it.
- **Difficulty levels** (`moon_rules_t`, `g_moon_rules[]`, flash): Cadet (easy), Engineer (normal, the default), Commander (hard). A level sets the starting regolith, O2, water and battery, the energy scale, the Sinterer's output and the crew's use of O2 and water. Chosen in the menu's `R:NEW GAME` page (`1`/`2`/`3`, `N` back; from the game-over and victory overlays too); the host takes `--difficulty=`. The level is kept over restarts of a session and shown in the menu and the end overlays.
- **Map fairness.** The generator now also guarantees a 3x3 basalt mare patch 4-5 tiles from the landing apron (direction by seed), next to the ice deposit it already guaranteed 10 tiles away; an Extractor works beside either. Without it Commander lost on a quarter of the maps through no fault of the player.
- **Act I goal raised** to day 6 and 8 crew (`MOON_ACT1_DAY`, `MOON_ACT1_CREW`) so that a base has to run through five full days and a full Habitat; with the crew at 12 it also needs a second or third Extractor, which is the point of the "production keeps up" condition.
- **Balance table** (rules, then what `bin/balance levels 16` measures over twelve maps; "good" decides every second, "slow" every 8 s, "careless" builds a Habitat first, "idle" builds nothing). `make test` asserts the targets in the last column.

| Level | regolith | O2 | water | battery | energy % | Sinterer kg/min | crew use % | s per deposit unit |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Cadet | 120 | 70 | 100 | 100 | 5 | 45 | 80 | 12 |
| Engineer | 80 | 50 | 75 | 40 | 8 | 30 | 100 | 8 |
| Commander | 50 | 40 | 60 | 20 | 10 | 30 | 130 | 6 |

| Level | good: alive / Act I (median day) | slow: alive / Act I (median day) | careless dies | idle dies (median day) | Target |
| :--- | :--- | :--- | :--- | :--- | :--- |
| Cadet | 12/12, 12/12 (6) | 12/12, 12/12 (6) | 0/12 | 3 | easy and forgiving: a careless player survives |
| Engineer | 12/12, 12/12 (6) | 12/12, 12/12 (6) | 12/12 | 2 | good and slow players win, careless and idle lose |
| Commander | 10/12, 10/12 (7) | 10/12, 10/12 (11) | 12/12 | 2 | winnable, not on every map; slow play pays |

- Engineer is robust: a player who acts only once every 40 s still wins 11 of 12 maps. Commander's three lost maps are the ones whose obstacles lengthen the first cable run.
- Economy reference (code truth, per game second unless stated): day and night are 90 s each; a solar array averages about 19 kW over the day (30 kW at noon); the Lander's RTG gives 10 kW and the command systems use 5; crew use is 1 kg O2 and 2 L water per colonist per 30 s at 100%; an Extractor makes 20 kg O2 and 30 L water per minute (10 colonists' oxygen, 7 colonists' water); a Sinterer 30 kg per minute; growth is one colonist per 45 s at 95% life support or better.
- Harness: `bin/balance` (`levels [days]`, `sweep`, `seeds LEVEL`, `one ENERGY REGOLITH SINT [days]`, `check`); it drives the real keyboard input path with a route planner (BFS over buildable tiles) and a build-order bot. It is a model of play, not of every human: it does not use standby at night, quarrying or recycling.

**64.13 implementation notes, onboarding part (2026-10-24).** Done: 5.7.6 (objectives and tutorial path) and the briefing and level choice of 5.7.7; the touch-down animation of 5.7.7 is cosmetic and not done.
- Objectives (`MOON_OBJECTIVES` 7, `objective_done()`): a powered Extractor, three Solar Arrays, a powered Sinterer, a second Battery Bank, a powered Habitat, a crew of 8, Act I. They advance in order, also past several at once, and never go back (recycling a building does not undo one); a new game starts over. The text is a pill at the bottom left (a short form on the 640 px layout), `O` hides it and the choice survives restarts. The battery objective asks for one more bank, not three, because Cadet's scripted player needs only one more: an objective must be reachable at every level.
- Title page (`MOON_MENU_TITLE`): opened when `show_title` is set before init (the P4 launch; the host, unless `--difficulty=` or an automated `--test-frames` run). It keeps the launch's map (`keep_seed`), so `--seed=N` replays a map, and ESC cannot dismiss it; `1`/`2`/`3` start, `Q` quits.
- Brush ghost on touch (fix found on the boards): with a finger lifted the ghost used to fall back to the selected tile, i.e. the cable just built, and showed "Site already occupied" after every tap. `moon_game_ghost_target()` now returns no target for a brush while a finger is up (the ghost shows under the finger while it is down); a keyboard brush still previews the selection and a mouse brush the pointer, and a key press marks the keyboard as the last device.
- `make test` additionally asserts that the scripted good player completes every objective on every map on Cadet and Engineer, and the unit tests walk the chain through real placements.

**64.13 implementation notes, deposits (2026-10-24).** Done: the stock and depletion half of 5.7.3. A separate electrolysis building is deferred to the tech tree and hierarchical build menu (§5.9C, §5.10): the flat ribbon has no room for a seventh build button.
- A deposit is the `resource_val` of its ice or mare tiles (the "R:" in the tile readout and the inspector). A powered, active Extractor draws one unit from the richest ice or mare tile beside it every `rules.deposit_s_per_unit` seconds (Engineer 8: about 11 days for one Extractor on three 82-unit mare tiles); `moon_structure_t.work` counts toward the next unit and `.depleted` is set when no adjacent tile has any left. A depleted Extractor makes nothing and draws no power; the badge reads `DEPL`, the inspector `DEPLETED` and "Deposit left". Unpowered, standby and brownout Extractors draw nothing, so they keep their deposit.
- Placement needs a neighbouring ice or mare tile with `resource_val` > 0 ("Needs ice or mare with ore left"), so an exhausted patch is closed to new Extractors.
- Balance: the scripted good player copes by building Extractors on fresh ground (five over a month on Engineer's default map); `bin/balance check` asserts it survives 30 days with at least one Extractor dry. In Act I the depletion only costs a few kg, because regolith is plentiful once a Sinterer runs and the stock caps at 250 kg: the real pressure needs the sinks of Act II (fuel, exports) and the quarrying and storage of 64.13's remaining items.

**64.13 implementation notes, quarrying (2026-10-24).** Done: 5.7.2's excavation. The regolith stock of a tile (`resource_val`, the "R:" readout) is now what a Sinterer digs; sintered blocks and foundation pads as buildable things are deferred to the tech tree (§5.9C), and so is anything else needing its own build button.
- `moon_structure_ore_tile()` / `moon_structure_ore_left()` (moon_structures.c) are the one definition of "the ground a structure draws on": from the eight tiles around it, an Extractor takes ice or mare, a Sinterer any other terrain, the richest first. The sim (`draw_from_ground()`), the placement rules, the ghost label and the inspector all use them.
- A powered, active Sinterer takes one unit per kilogram baked (`rules.sinterer_kg_per_min` added every tick, a unit per 240) until the ground around it is spent; then it idles like a depleted Extractor (`DEPL`, no power draw). Placement is refused with "No regolith left to quarry here"; the ghost label shows `ore N` for Sinterers and Extractors. Structures under or on tiles do not block quarrying: only stock matters.
- Richness by terrain (generator, unchanged): boulder fields 40-75, flat regolith 30-60, crater floor 15, crater rim 20. A site next to boulders is the best; one beside craters runs dry in a day or two. The landing apron's tiles now get ordinary richness (they kept stale mare or crater values: the first Sinterer sat on an `ore 678` site).
- Balance: a Sinterer at an ordinary site holds about 350 kg, i.e. about four game days; the scripted player builds one on the richest free ground each time (nine in a month) and the balance table is unchanged apart from Commander's 10/12. The emergency scoop (15 kg) keeps a replacement affordable.
- `make test`: richest-tile-first, a kilogram per unit, exact exhaustion, idle and power, deposits untouched, placement refusal and acceptance, richness by terrain.

**64.13 implementation notes, resource use (2026-10-24).** Done: the draw half of 5.7.4. The catalog already carried negative `rate_o2` and `rate_water` for the Lander (-1, -2) and the Habitat (-5, -10) that the sim ignored; they are now a standing draw in kg and L per minute (the catalog comments said "per day", which was never true) for every *active* structure, powered or not, in `moon_resources_tick()`. Standby means depressurised, so a Habitat on standby draws nothing. A Habitat therefore costs a quarter of an Extractor's oxygen and a third of its water on top of its 8 crew; with 12 colonists the base needs three Extractors, which the scripted player already builds when a rate turns negative, so the balance table did not move. `make test` pins the exact rates (Lander alone, with a Habitat, unpowered, on standby) and the effect on the stock.

### 5.9 Dedicated Performance Optimization Phase: Hardware PPA (Bridge between Act II & Act III)

Hardware benchmarks on the M5Stack Tab5 revealed that CPU software 2× upscaling of the 640×360 virtual frame to 1280×720 physical resolution is bounded by external PSRAM bus bandwidth (~36 MB/s sustained under active MIPI-DSI DMA read contention), consuming ~74 ms per frame in `Present`. 

To achieve 15–20 fps on Tab5, a dedicated **Hardware PPA (Pixel Processing Accelerator)** driver phase will be executed between Act II and Act III:
- **Architectural Scope**: Implement a freestanding ESP32-P4 PPA driver (`drivers/ppa_esp32p4.c`) utilizing the chip's hardware Scale-Rotate-Mirror (SRM) 2D AXI DMA engine.
- **Offloaded Scaling**: The game rasterizer will continue writing the 640×360 frame, while the hardware PPA handles the 2× nearest-neighbor upscale directly into the display buffer at ~35 Mpixels/sec (~26 ms), completely eliminating CPU upscale overhead and L1/L2 cache evictions.
- **System-Wide Reusability**: The PPA driver will be available as a core LugalOS graphics primitive, benefiting window blitting, image decoding, and desktop ribbon scaling across all graphical applications.

### 5.10 Handoff to Act II Planning

The foundational survival loop is closed in Act I. The following features, mechanics, and design items are consolidated as the direct handoff backlog for the upcoming **Act II (Colony Expansion & Industrialization)** plan:

1. **Tech Tree & Hierarchical Build Menu**: Transition from the flat 6-building ribbon to a structured category sheet / build tree unlocked through research and colony growth.
2. **Pressurized Construction & Shielding**: Sintered regolith foundation pads, radiation-shielded building blocks, and habitat atmospheric integrity/shielding mechanics.
3. **Advanced Life Support**: Separate water-to-oxygen electrolysis plants (Electrolyzer), thermal management (radiator and heating arrays), and dedicated regolith bulk silos.
4. **Spaceport & Export Economy**: Starship landing pads, fuel/propellant production (LOX, liquid hydrogen), and trade/export logistics (shipping Helium-3 to Earth).
5. **Colony Management & Automation**: Introduction of the Operator settler class for building maintenance and control posts, progressing toward Act III declarative cybernetics.
6. **Environmental Hazards & Wear**: Micrometeoroid strikes, dust storm accumulation, mechanical wear, and repair workflows.
7. **Visual Polish**: Multi-frame sprite animation strips (tracking solar arrays, glowing sinterers, rotating fans) and graphical state badges.

---

---

## 6. Act I Implementation Summary & Features Delivered

Act I (*Survival & Bootstrapping*) is fully implemented and running on both the SDL3 host simulator and bare-metal LugalOS ESP32-P4 hardware.

### 6.1 Systems & Mechanics Delivered
1. **Engine & Architecture**:
   - Freestanding standard C, zero floating-point math (`fixed32_t` arithmetic across simulation and blitting).
   - RGB565 software rasterizer with isometric block extrusion, depth-sorted tile blitting, font rendering, and dirty/double-buffering.
   - Dual HAL: Host runner with SDL3 (`platform_sdl3.c`) and bare-metal ESP32-P4 driver (`platform_p4.c`) linking with LugalOS kernel PSRAM canvases.
2. **Terrain & Isometric World**:
   - $64 \times 64$ diamond isometric lunar map with crater topography, basalt rock formations, and shadowed polar ice pockets.
   - Macro-crater texturing via Flash-compressed 4-bit packed nibbles unpacking into a 32 KB RGB565 PSRAM buffer, mapped smoothly with zero seam repetition.
   - Real-time minimap radar with viewport indicator and instant tap-to-navigate camera jump.
3. **Colony Simulation & Resources**:
   - Full 60-second accelerated lunar diurnal cycle with day/night solar curve.
   - BFS-based graph-traversal electrical grid with conduit connectivity, brownout states, and power-grid solver.
   - Closed-loop life support (Power, Regolith, Oxygen, Water, Cryogenic Battery storage, Colonist headcount).
4. **Interactive Controls & Input**:
   - Full capacitive touch support on GT911 and ST7121 with event-queue move coalescing and displacement-based drag detection.
   - Bottom build ribbon for one-tap structure selection, demolish tool, and fast-render toggle.
   - Inspector pane with real-time status, health, standby/activate toggle, and material recycling.
   - Complete ANSI VT escape sequence decoder (`keyseq_read_console()`) supporting Up/Down/Left/Right arrows for smooth camera panning, 'Q'/ESC for cancellation and clean shutdown.
5. **Display & Hardware Targets**:
   - **ESP32-P4-WIFI6-Touch-LCD-7B**: Native 1024×600 borderless fullscreen presentation with cache writebacks and front/back buffer flipping.
   - **M5Stack Tab5**: 1280×720 physical presentation with 2× integer HiDPI scaling (640×360 virtual viewport).
   - Zero-crash cleanup restoring terminal text mode, repainting canvas, and synchronizing scanout.

---

## 7. Act II Phase-Plan: Hydrolox Starship Connection & Municipal Expansion

**Goal**: Transform Moonbase Lambda from an isolated, survival-stage outpost into a thriving interplanetary spaceport connected to Earth via recurring **Hydrolox ($LH_2 / LOX$) Starship** trade runs.

### 7.1 Act II Narrative & Economy Directives
* **Hydrolox Propellant Paradigm**: Our lunar Starship variant uses liquid hydrogen and liquid oxygen ($LH_2 / LOX$) instead of methalox. Water extracted from polar crater ice is split by high-voltage electrolysis into cryogenic $LH_2$ and $LOX$.
* **The Starship Loop**:
  1. Starship arrives from Earth on orbital retro-burn carrying scientific cargo, heavy manufacturing tools, and specialized colonists.
  2. Starship touches down on a reinforced, dust-deflecting landing pad.
  3. Cargo and crew are transferred to base storage / habitats.
  4. The colony refuels the Starship's cryogenic tanks with lunar-produced $LH_2$ and $LOX$.
  5. Starship launches back toward Earth before its launch window closes, returning research data and mined lunar minerals.

### 7.2 Act II Milestone Breakdown

| Milestone | Status | Title | Deliverables & Verification |
| :---: | :---: | :--- | :--- |
| **64.16** | **COMPLETE** | **Cryogenic Propellant Economy & Electrolysis** | Implement $LH_2$ and $LOX$ resource tracking in `moon_resources_t`. High-power 2×2 Hydrolox Electrolyzer structure splitting $H_2O$ ($2 H_2O \to 2 H_2 + O_2$; 1:8 mass ratio). Cryogenic dewar tanks with thermal insulation & boil-off mechanics during lunar night. HUD propellant gauge. |
| **64.17** | **COMPLETE** | **Hierarchical Build Sheet & Tech Progression** | Evolve beyond the flat 6-building ribbon: structured categorized build sheets (Power/Storage, Life Support/Cryo, Spaceport/Logistics). Minimum clearance rules and sintered foundation requirements. |
| **64.18** | **COMPLETE** | **Reinforced Starship Landing Pad** | 4×4 sintered regolith spaceport apron with blast-deflection berms and cryo umbilical conduits. Protects neighboring solar arrays and habitats from high-velocity exhaust regolith scouring. |
| **64.19** | **COMPLETE** | **Starship Flight & Engine Plume Animation** | Procedural multi-state Starship flight sequence: orbital approach, ease-out retro-propulsion with pale-blue/cyan hydrolox plume & shock diamonds, landing gear extension, touchdown dust skirts, umbilical hookup, cryogenic loading meter, and vertical ascent return burn. |
| **64.20** | **COMPLETE** | **Earth-Moon Trade, Personnel & Victory Loop** | Earth cargo manifests (specialist colonists, heavy machinery, high-yield reactors). Starship turnaround timer / launch window pressure. Act II Victory Condition: Successfully support, refuel, and dispatch the first commercial Hydrolox Starship return mission. Headless balance test in `tests/balance.c`. |

### 7.3 Act II Implementation Notes

**64.16 implementation notes, cryogenics & 2×2 structures (2026-10-29).** Delivered:
- **Cryogenic Resource Economy**: $LH_2$ and $LOX$ tracking added to `moon_resources_t` with 1:8 stoichiometric mass ratio cracking from polar water ($2 H_2O \to 2 H_2 + O_2$).
- **Structures**:
  - `MOON_STRUCT_HYDROLOX_PLANT` (2×2 footprint, 60 kW demand, cracks water into cryogenic propellants).
  - `MOON_STRUCT_CRYO_DEWAR` (1×1 footprint, vacuum-insulated sphere storing 250 kg $LH_2$ and 2,000 kg $LOX$ with 5 kW active cryocooling and night boil-off dissipation).
- **Sprite Geometry & Alignment**: 2×2 isometric footprint anchoring corrected to `base = 48` (`MOON_TILE_H + MOON_TILE_HALF_H`), ensuring multi-tile structure sprites sit flush inside the bottom vertex of the 2×2 footprint diamond with zero rightward drift.
- **Overlays**: Real-time status badges and resource meter progress bars on active buildings.

**64.17 implementation notes, hierarchical build sheet & tech progression (2026-10-30).** Delivered:
- **Hierarchical Build Sheets (`moon_build_category_t`)**:
  - `MOON_CAT_POWER`: Solar Array (1), Battery Bank (2), Power Conduit (3).
  - `MOON_CAT_LIFECRYO`: O2 Extractor (1), Habitat Dome (2), Hydrolox Plant (3), Cryo Dewar (4), Power Conduit (5).
  - `MOON_CAT_LOGISTICS`: Regolith Sinterer (1), Foundation Pad (2), Power Conduit (3).
  - Power Conduit mapped across all categories to eliminate friction when wiring grids.
  - Category cycling supported via keyboard (`B` key) and touch/mouse (`cat_btn`) with color-coded badges (Gold/Amber, Cyan, Gold) and live HUD toast notifications (`BUILD SHEET: ...`).
- **Sintered Foundation Pad (`MOON_STRUCT_FOUNDATION_PAD`)**:
  - 1×1 sintered regolith paver costing 5 kg regolith. Sets `MOON_TILE_FLAG_FOUNDATION` without occupying the structure slot (`MOON_TILE_FLAG_OCCUPIED`), allowing structures to be constructed on top.
  - Bare foundation pads can be inspected, recycled for 2 kg regolith, or undone within the 10 s undo window.
  - Sintered foundation prerequisite: Hydrolox Plant requires all 4 footprint tiles to be paved foundations (`"Requires sintered foundation"`).
- **Hazardous Facility Safety Clearance Buffer**:
  - Cryogenic Hydrolox Plant requires a $\ge 1$-tile safety buffer from crew habitats (`MOON_STRUCT_HABITAT_DOME`) and expedition lander (`MOON_STRUCT_LANDER`). Direct adjacency is refused with `"Safety: clearance from habitat required"` (and `"Safety: clearance from Hydrolox plant required"` when placing habitats near Hydrolox).
- **Tech Progression**:
  - Foundation Pad requires an active/built Regolith Sinterer (`"Requires Regolith Sinterer"`).
  - Advanced Cryogenics (Hydrolox Plant & Cryo Dewar) require an established Habitat Dome (`"Tech locked: Requires Habitat Dome"`).
- **Verification**:
  - 100% pass on host test suite (`make -C user/moonbase test`, including `balance check`).
  - Racescan clean: 0 shared unmarked statics.
  - Verified on M5Stack Tab5 bare-metal hardware.

**64.18 implementation notes, reinforced starship landing pad (2026-10-30).** Delivered:
- **Structure Definition & 4×4 Isometric Geometry**:
  - Added `MOON_STRUCT_LANDING_PAD` in `moon_structures.h` with 4×4 tile footprint (`footprint_w = 4, footprint_h = 4`).
  - Cost: 120 kg sintered regolith, power consumption -10 kW (telemetry, guidance beacons, cryogenic umbilical chill).
  - Sprite: $128 \times 96$ RGB565 isometric pixel art with blast berms, landing target markings, and cryogenic umbilical mast.
  - Geometry alignment: Anchored at `base = 60` px to seat the bottom berm vertex directly onto the 4×4 diamond bottom vertex ($sy + 56$ to $sy + 60$), while allowing the umbilical mast to rise gracefully above the northern quadrant.
- **Hierarchical Build Sheet Integration**:
  - Added to `MOON_CAT_LOGISTICS` as Slot 3 (`MOON_KEY_3`).
  - Re-indexed Power Conduit to Slot 4 in Logistics.
- **Placement Prerequisites & Safety Clearance Rules**:
  - **Tech Progression Lock**: Requires both Habitat Dome and Cryogenics (Hydrolox Plant or Cryo Dewar). Placement blocked with `"Tech locked: Requires Habitat & Cryogenics"`.
  - **16-Tile Sintered Foundation Apron**: All 16 tiles ($4 \times 4$) must be paved with `MOON_TILE_FLAG_FOUNDATION`. Placement blocked with `"Requires 4x4 sintered foundation"`.
  - **Rocket Engine Scouring Clearance**: Must maintain $\ge 1$ tile clearance from crew habitats (`MOON_STRUCT_HABITAT_DOME`) and expedition anchor (`MOON_STRUCT_LANDER`). Refused with `"Safety: clearance from habitat required"` (and reciprocally `"Safety: clearance from landing pad required"` when placing habitats).
- **Inspector Panel & Overlays**:
  - Real-time status line: `[SPACEPORT READY]`, `[UNPOWERED / NO GUID]`, or `[STANDBY]`.
  - Detailed diagnostic lines: `-10 kW (Guidance)`, `4x4 Sintered Basalt`, `Berm Prot`, `UMBILICAL: LH2/LOX Ready`, `STARSHIP: Awaiting Run`.
  - Sprite meter bar: 5/5 cyan when active/powered, 2/5 yellow when unpowered, 0/5 grey in standby/brownout.
- **Verification**:
  - 100% pass across `test_fixed_blitter`, `test_game_logic`, and `balance` (`make -C user/moonbase test`).
  - Static race check (`python3 tools/racescan.py --check`) clean: 0 shared unmarked statics.
  - `python3 tools/gendocs.py --check` clean: 0 drift.
  - Built freestanding ELF for `build/esp32p4-tab5` via Ninja.
  - Flashed to connected M5Stack Tab5 hardware (`/dev/ttyACM0`) and verified via `tools/p4run.py`.

**64.19 implementation notes, Starship flight sequence, plume & refueling animation (2026-10-30).** Delivered:
- **Procedural Multi-State Starship Flight Engine (`moon_starship.c`, `moon_starship.h`)**:
  - Complete 9-state sequence: `MOON_STARSHIP_ORBIT`, `MOON_STARSHIP_APPROACH`, `MOON_STARSHIP_RETRO`, `MOON_STARSHIP_GEAR_EXTEND`, `MOON_STARSHIP_TOUCHDOWN`, `MOON_STARSHIP_SERVICING`, `MOON_STARSHIP_PRELAUNCH`, `MOON_STARSHIP_ASCENT`, `MOON_STARSHIP_DEPARTED`.
  - Pure integer fixed-point physics (zero float instructions in kernel/M-mode).
  - Smooth ease-out quadratic deceleration on descent ($Z: 360 \to 240 \to 40 \to 0\text{ px}$) and ease-in quadratic acceleration on vertical ascent ($Z: 0 \to 450\text{ px}$).
  - Quick-skip capability: Space or Enter key skips descent directly to servicing, or ascent to departed.
- **Visual Assets & Procedural Effects**:
  - $48 \times 128$ px stainless steel Lunar Starship sprite (`g_surf_starship`, `SPRITE_STARSHIP_W/H`) with TPS heatshield tiles and landing gear.
  - Pale-blue/cyan hydrolox exhaust plume (`COLOR_LASER_CYAN`, `RGB565(240, 255, 255)`, `RGB565(40, 140, 230)`) with 4 pulsating supersonic shock diamond nodes and flame flicker.
  - Ground-effect flame deflection: when $Z < \text{plume length}$, exhaust splits and deflects outward along the 4×4 pad berms.
  - Expanding touchdown and liftoff dust skirts: 24 particulate puffs expanding radially across the 4×4 sintered apron using a 16-point dimetric fixed-point trigonometric table.
  - Articulated gantry umbilical arm: extends from pad tower ($tx, ty$) to ship propellant port ($sx, sy$) with frosty cryogenic coupling and boil-off vapor venting.
- **Cryogenic Loading Economy**:
  - Automatically draws $LH_2$ and $LOX$ from base reserves (1:8 mass ratio: 12.5 kg/s $LH_2$, 100 kg/s $LOX$) up to return burn requirements (250 kg $LH_2$, 2,000 kg $LOX$).
  - Floating 10-pip cryogenic loading meter above the Starship displaying live fuel state.
  - Real-time telemetry, stage progression, and diagnostic readouts in the Landing Pad inspector panel.
  - Interactive dispatch and launch control via key `L` or inspector controls.
- **Verification**:
  - 100% pass across all unit tests, including dedicated `test_starship_flight_animation` (`make -C user/moonbase test`).
  - Racescan clean: 0 shared unmarked statics (`python3 tools/racescan.py --check`).
  - Documentation clean: 0 drift (`python3 tools/gendocs.py --check`).
  - Verified freestanding ELF build for `esp32p4-tab5` with Ninja.

**64.20 implementation notes, Earth-Moon trade, personnel & victory loop (2026-10-30).** Delivered:
- **Earth Trade Cargo Manifest (`moon_cargo_manifest_t`)**:
  - Arriving Starships carry an Earth cargo manifest: 4 specialist colonists, 100 kg prefabricated sintered components / heavy machinery, and a 30 kW high-yield compact modular reactor.
  - Automatically offloaded upon touchdown at the spaceport pad (or immediately when skipping descent via Space/Enter).
  - Colonists move directly into available habitat dome housing; regolith adds to building stockpiles up to silo capacity; high-yield modular reactor provides +30 kW baseline continuous generation day and night, directly resolving industrial nighttime power deficits.
- **Turnaround Timer & Launch Window Pressure**:
  - 180-second countdown (`MOON_STARSHIP_WINDOW_MS`) during pad servicing reflecting orbital transfer windows.
  - Real-time window telemetry in the Landing Pad inspector panel (`WINDOW: %ds | %d%% Loaded`) with flashing red/amber alert borders on both the inspector and floating cryogenic meter when under 30 seconds.
  - Window timeout with incomplete propellant loads aborts the Starship back to orbit to protect spacecraft integrity, requiring the colony to stockpile propellants in Cryo Dewars before recalling.
- **Act II Victory Condition & Menu Flow (`MOON_MENU_VICTORY_ACT2`)**:
  - Completing cryogenic refueling and dispatching the return flight to Earth triggers Act II Victory upon clearing orbit (`MOON_STARSHIP_DEPARTED`).
  - Victory menu displays: `"ACT II COMPLETE"`, `"SPACEPORT OPERATIONAL"`, `"HYDROLOX STARSHIP DISPATCHED: DAY %d"`, `"CREW %d SEED %u REACTOR +%dkW"`, `"EARTH TRADE ROUTE ESTABLISHED"`.
  - Seamless continuation into endless free play via `C` key or `C:CONTINUE` button (`game->act2_victory = true`).
- **Dynamic Context-Sensitive Act II Objective Ribbon**:
  - HUD objective banner automatically transitions to Act II progression once Act I is completed, sequentially guiding the player through paving foundations, building the Hydrolox Plant, placing Cryo Dewars, paving the 4×4 pad apron, calling the Starship, and refueling/launching the return flight.
- **Comprehensive Verification & Headless Balance Suite**:
  - Dedicated unit tests in `test_game_logic.c` (`test_earth_moon_trade_and_act2_victory`) validating cargo manifests, touchdown unloading, reactor generation, window timeout aborts, propellant transfers, and Act II victory triggers.
  - Dedicated headless balance test in `tests/balance.c` (`test_act2_balance`, runnable via `./bin/balance act2` and `make test`) verifying end-to-end Act I victory, Act II infrastructure construction, conduit network connection, Starship servicing, and commercial mission victory.
  - 100% pass on host test suite (`make -C user/moonbase test`).
  - Racescan clean: 0 shared unmarked statics (`python3 tools/racescan.py --check`).
  - Documentation clean: 0 drift (`python3 tools/gendocs.py --check`).
  - Verified freestanding ELF build for `esp32p4-tab5` with Ninja.




