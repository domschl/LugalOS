# Phase 64 — Moonbase Lambda: 2D Isometric Lunar City-Builder

**Status**: Act I beta delivered; review findings (§5.1-5.7) and hardening milestones 64.9-64.15 open  
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

### 5.1 Review of the Act I Beta (2026-10-24)

Static review of `user/moonbase/` plus two host experiments (picking probe, idle-base simulation). Hardware was not exercised; P4 items are from code reading and need measuring (see 64.14). Severity: **H** = breaks play or hides content, **M** = visible defect or inconsistency, **L** = polish. Line numbers refer to the tree at review time.

Observed during the review:
- `make test` passes (ASan/UBSan). It covers fixed-point math, the blitter and a short smoke path only. There are no tests for picking, placement rules, the power solver or game end.
- Picking probe: a cursor at the exact centre of tile (10,10) maps to (10,10). One pixel up-left already maps to (9,9). Only the lower-right quarter of each diamond picks correctly.
- Idle-base simulation (the starting base, no player action): O2 and water run out at about t = 250 s, life support reaches 0%, and the crew is dead by about t = 270 s (game day 2). The game never ends, and the HUD reports "ALL CREW LOST" for the rest of the session.

### 5.2 General Bugs

| ID | Sev | Finding | Where |
| :--- | :-: | :--- | :--- |
| 5.2.1 | H | **Tile picking is offset by half a tile.** `moon_screen_to_grid()` floors the inverse isometric transform, but `moon_grid_to_screen()` returns the tile centre. The inverse needs +0.5 (round, not floor). Hover, placement, demolish and inspect all hit the neighbouring tile in three quarters of the diamond. Tile elevation is ignored by the inverse as well, so clicks on raised terrain land on the wrong tile. | `moon_grid.c:204-220` |
| 5.2.2 | H | **`warning_crew_lost` is never cleared.** It is set when a colonist dies and is never reset, so the "[CASUALTY]" banner is permanent and hides the O2, power and cold banners (the banners are an if/else chain). | `moon_resources.c:307`, `moon_game.c:817` |
| 5.2.3 | H | **Inspector hit-testing uses hard-coded geometry.** The close button is tested at `ip_x + 260 - 22`, but the narrow layout is 220 px wide (Tab5 at 2x). The button misses its drawn position, and a tap there can also fall into the RECYCLE or STANDBY row test. The panel outer bounds (280/230) disagree with the panel width (260/220). | `moon_game.c:327-339` |
| 5.2.4 | M | **Click-through on the HUD.** Clicks at y < 32 (top bar, notification badge, banner) reach the map and select or place on a tile under the HUD. The ghost label is not clipped either. | `moon_game.c` (POINTER_UP map branch) |
| 5.2.5 | M | **A drag that starts on UI pans the camera.** POINTER_DOWN always arms the drag, so pressing on the minimap, the ribbon or the inspector and moving pans the map. Right and middle mouse buttons also pan, and right-button release after a drag still cancels the active tool. | `moon_game.c:231-247, 410-422` |
| 5.2.6 | M | **Camera clamp is not derived from map or screen size.** Fixed bounds (±1200, 100..2000) make the left and right map corners unreachable (the iso extent is ±2016, and the screen only reaches ±1712 at 1024 px). The minimap click clamps silently to the same box. | `moon_game.c:84-89` |
| 5.2.7 | M | **Soft-lock on regolith.** The only income is the Sinterer (15 kg, 20 kW). Start stock is 60 kg. A player who builds a Habitat (50 kg) first cannot afford a Sinterer, and nothing else produces regolith. The only way out is recycling at 50%. | `moon_resources.c:30`, `moon_game.c:98` |
| 5.2.8 | M | **Stress logic punishes a working grid.** `warning_power` fires when the battery is under 20% even without a brownout. That counts as "stressed", so life support decays 4%/s and crew start dying while power is still being delivered. | `moon_resources.c:227-296` |
| 5.2.9 | M | **Battery over capacity after recycle.** The deficit branch never clamps `battery_stored_kwh` to the new capacity. Recycling a battery leaves stored energy above capacity until the next surplus tick. The brownout flag is also not recomputed immediately. | `moon_resources.c:211-232` |
| 5.2.10 | M | **Power grid propagation.** The BFS energises any structure adjacent to a powered one, so conduits are never required for adjacent buildings. A Standby structure still conducts power to its neighbours. This conflicts with the README ("connected via power conduits"). | `moon_resources.c:39-118` |
| 5.2.11 | M | **Large stack frames in the solver.** `solve_power_grid` keeps a 4 KB visited array and an 8 KB queue on the stack and is called from every tick and every UI action. U-mode task stack size on the P4 must be checked; an overflow would be silent. | `moon_resources.c:62-67` |
| 5.2.12 | M | **P4 keyboard map is incomplete.** `8`, `9` and `E` are not translated (the SDL map has them). Unknown keys still generate an event. Held-key scrolling does not exist on the P4 because the console gives no key-up events; panning is 24 px per key repeat. | `platform_p4.c:313-373` |
| 5.2.13 | L | **`moon_game_init()` assumes zeroed state.** `demolish_tool_active`, `has_selection`, `notify_*` and the structure selection are not reset. This works only because the game object is static. A restart (64.12) needs a full reset. `moon_game_shutdown()` only clears `running`. | `moon_game.c:21-72` |
| 5.2.14 | L | **Unused or dead state.** `MOON_STATE_INIT/LANDING/PAUSED/GAME_OVER` are never entered. `rate_o2` and `rate_water` of the Lander and Habitat are unused. `MOON_KEY_7..9`, `E` and `Z` are mapped but unused. `moon_platform_log()` is a no-op on the P4. | `moon_game.h`, `moon_structures.c` |
| 5.2.15 | L | **Docs drift.** `README.md` disagrees with the code on costs (25/40/100/50/60/5 kg vs 10/20/50/15/25/2), the Lander footprint (2x2 vs 1x1), Habitat crew (4 vs 8), Battery capacity (500 vs 250 kWh) and the Solar rating (10 vs 30 kW). The plan header says "Complete". Comments say a "120 s day" while `MOON_LUNAR_DAY_TICKS` is 180 s. | `README.md`, plan header |
| 5.2.16 | L | **Notification and crisis banner overlap.** Both are drawn at the same y (38 or 26), and the fixed banner widths (320-430 px) can be narrower than the text. | `moon_game.c:812-842` |

### 5.3 Performance on P4 Platforms

No frame-time numbers exist yet. The risks below follow from the code and need a measured baseline first (64.14).

| ID | Sev | Finding | Where |
| :--- | :-: | :--- | :--- |
| 5.3.1 | H | **Everything is redrawn every frame, even when idle.** The loop re-renders the whole terrain, the HUD and all structures, and presents (plus the 2x upscale and cache writeback on the Tab5) every 16 ms. Nothing changes between sim ticks except the clock and the HUD numbers. A dirty flag (camera, input, sim tick) would cut the idle load to near zero. | `platform_p4.c:407-430`, `moon_game_render()` |
| 5.3.2 | H | **Per-pixel procedural texturing.** `sample_terrain_pixel()` runs a multiply-xor hash (`micro_noise`) plus a texture fetch and per-terrain branching for every terrain pixel. Terrain covers most of the screen, so this is several hundred thousand pixels per frame. The texture is read from PSRAM (BULK_BSS), and cache misses are expensive. Static tile art could be cached (e.g. one pre-rendered 64x32 bitmap per terrain variant, or a scrolled terrain layer) or replaced by a precomputed, tileable lookup. | `moon_blitter.c` (`moon_blit_iso_tile_textured`, `sample_terrain_pixel`) |
| 5.3.3 | M | **All 4096 tiles are visited every frame.** Culling is a per-tile test inside the diagonal loop; a visible-range computation from the camera would visit about 300-500 tiles. | `moon_grid.c:244-264` |
| 5.3.4 | M | **Tab5: 640x360 render plus a full 2x upscale into PSRAM each frame.** It writes 1280x720x2 B (1.8 MB) and flushes the cache over the whole frame, even when nothing changed. Only damaged rows need to be scaled and written back. | `platform_p4.c:261-298` |
| 5.3.5 | M | **1x path allocates a second full-frame buffer** (1.2 MB) and swaps canvas backing under `console_lock()`. If the allocation fails, the fallback single-buffers silently (visible tearing), and on the Tab5 it also drops to 1x, so the UI becomes half its intended physical size. | `platform_p4.c:168-180` |
| 5.3.6 | M | **Text is drawn pixel by pixel** (8x16 glyph bitmap, `moon_blit_char`), one `moon_blit_text_fmt` per HUD item. The HUD and inspector are about 40 string draws per frame. A per-glyph row blit would cost far less. | `moon_blitter.c`, `moon_game.c` |
| 5.3.7 | M | **Sim tick catch-up loop is unbounded** (`while (last_tick_ms >= 250)`). After a stall (SD or Wi-Fi traffic, a debugger) the P4 would run many sim ticks in one frame, each with a BFS. Clamp `delta_ms` per frame (e.g. 100 ms). | `moon_game.c:455-459` |
| 5.3.8 | L | **Fixed 16 ms sleep, no frame pacing.** `task_sleep_ms(16)` added to the render time gives uneven frame times and wastes the margin. The loop should sleep for the remainder of the frame, and the FPS and frame time should be shown in a debug overlay. | `platform_p4.c:429` |
| 5.3.9 | L | **BFS runs on every sim tick (4 Hz) and every build or recycle action**, though the grid changes only on those actions. It should run only when the structure set or the active flags change. | `moon_resources.c:146` |
| 5.3.10 | L | **Culling thresholds are magic numbers.** The structure cull uses +30 and TILE_W margins, which are wrong for portrait or other presets. | `moon_structures.c:433` |

### 5.4 Input Consistency (Touch / Mouse / Keyboard)

| ID | Sev | Finding | Where |
| :--- | :-: | :--- | :--- |
| 5.4.1 | H | **The keyboard cannot play the game.** There is no map cursor, so keyboard players cannot place, inspect, recycle, toggle standby or press any UI button. Keys only select a tool and pan the camera. | `moon_game.c:134-229` |
| 5.4.2 | H | **Touch cannot quit, pause or restart.** Quitting needs ESC or Q on a keyboard or Ctrl-C. On a tablet without a keyboard the only exit is a reset. There is no menu button, and no touch equivalent of "cancel tool" except the 0:INSPECT button. | `platform_p4.c:407`, `moon_game.c` |
| 5.4.3 | H | **Touch placement is immediate and blind.** POINTER_UP places at the finger position, and the ghost and validity label are never shown beforehand (no hover on touch). The result is mis-placements that can only be undone for 50% of the cost. A touch flow needs a preview (tap to position, then confirm) or drag-to-position. The picking bug (5.2.1) makes this worse. | `moon_game.c:394-395` |
| 5.4.4 | M | **Touch targets are too small.** At the 7B's 170 DPI a 32 px ribbon button is 4.8 mm, the minimap 64 px (9.5 mm), the TEX toggle 16 px (2.4 mm), the inspector close button 18 px (2.7 mm) and the inspector buttons 26 px (3.9 mm). The common guideline is 7-9 mm. On the Tab5 (about 294 DPI, 2x) a virtual 32 px is about 5.5 mm. | `moon_game.c` (ribbon, inspector) |
| 5.4.5 | M | **Hover state is wrong on touch and after leaving the window.** `hover_valid` and the highlighted tile remain after POINTER_UP on touch and are never cleared when the mouse leaves the window. The last tapped tile stays highlighted. | `moon_game.c:249-265` |
| 5.4.6 | M | **Mouse wheel pans instead of zooming**, and there is no continuous keyboard scroll or edge scroll. Camera control differs between mouse (drag, wheel), touch (drag) and keyboard (24 px steps). | `moon_game.c:433-437` |
| 5.4.7 | M | **Cancel semantics differ per device.** Right-click, ESC, Q and 0 all cancel, but Q and ESC quit the whole game when nothing is active, with no confirmation. A stray ESC while the inspector is closed leaves the game. The README tells keyboard users that Q is "cancel/exit". | `moon_game.c:136-161` |
| 5.4.8 | M | **Layout is duplicated in render and hit-test code** (ribbon, inspector, minimap and TEX button geometry appear twice, with different constants). This is the root cause of 5.2.3 and will keep producing mismatches. Both paths should use one shared rectangle table. | `moon_game.c` |
| 5.4.9 | L | **No pause, no speed control.** `MOON_STATE_PAUSED` exists but is unreachable. The sim cannot be stopped while the player thinks, which matters with the 45 s game days. | `moon_game.h` |
| 5.4.10 | L | **Touch drag has no inertia, and the drag threshold (6 px) is fixed.** It is a different physical distance on the 7B and the Tab5 and should be defined in mm. | `moon_game.c:254, 416` |
| 5.4.11 | M | **Day/night rhythm is only readable from a percentage.** The HUD shows `D01 SUN:34%`, so the player cannot see how long the night lasts or when to have the batteries full (requested 2026-10-24). | `moon_game.c` (HUD) |

### 5.5 Graphics and Texturing Consistency

| ID | Sev | Finding | Where |
| :--- | :-: | :--- | :--- |
| 5.5.1 | H | **Structures are drawn in table-slot order, not depth order.** `moon_structure_render_all()` iterates `list[1..127]`, so a building placed later but standing further back is drawn on top of one in front of it. Slot reuse after demolition makes it random. Tall sprites (48-80 px) overlap tiles behind them, so this is very visible. | `moon_structures.c:412` |
| 5.5.2 | H | **Status badges ("OFF", "!PWR") are drawn inline with their building** and are therefore overdrawn by every structure that is drawn later, e.g. the "OFF" of a building is hidden by a building behind it. Badges, the battery LED bar and the conduit junction box must be drawn in a separate overlay pass after all sprites, or at least depth-sorted with them. | `moon_structures.c:464-486` |
| 5.5.3 | M | **Terrain and structures are separate passes.** All terrain is drawn first, then all structures. A structure standing behind a raised crater rim (+16 px) is drawn over the rim instead of being hidden by it, and the ghost, the selection frame and the hover highlight each use a different order. A single back-to-front pass per diagonal (tile, then structure, then overlays) is needed. | `moon_game.c:640-704` |
| 5.5.4 | M | **Selection frame is drawn before structures**, so the sprite covers the lower half of the cyan frame and the brackets. The 2x2 Habitat uses ad-hoc offsets (`+HALF_W`, `+HALF_H/2`) that do not match the footprint centre. | `moon_game.c:645-684` |
| 5.5.5 | M | **Ghost is a flat coloured diamond.** It shows neither the sprite nor the footprint height, and it covers the neighbouring structures. A semi-transparent sprite preview (or at least an outline plus the sprite at 50%) would match what will be built. | `moon_structures.c:490-536` |
| 5.5.6 | M | **Picking uses the ground tile, not the sprite.** Clicking the upper part of a tall building selects the tile behind it (the sprite covers it visually). Needs a sprite-aware pick (test the front-most structure whose sprite rectangle contains the point). | `moon_game.c:242, 398` |
| 5.5.7 | M | **Conduits are not drawn with the terrain depth.** They are drawn in slot order, so a conduit can overdraw the base of a building in front of it. Cable ends stop 16 px short in x direction at building edges and do not reach the neighbour's connector. Conduits do not visually join diagonal runs. | `moon_structures.c:372-403` |
| 5.5.8 | M | **Elevation handling is inconsistent.** Structures use the tile's elevation, but terrain picking ignores it (5.2.1). The Habitat uses the elevation of its origin tile only, although its four tiles can differ. | `moon_grid.c`, `moon_structures.c:419-430` |
| 5.5.9 | L | **Minimap shows no structures** and its camera box is an approximation (`width / TILE_W` tiles). A structure layer (one pixel per tile) and an exact viewport rectangle would help navigation. | `moon_grid.c:364-378` |
| 5.5.10 | L | **Flat ("FAST") mode looks very different from textured mode** (colours, hover colour, cliff rim) and is the default fallback if the P4 turns out to be too slow for textures (5.3.2). The two modes should share a palette and the tile border logic. | `moon_grid.c:285-315` |
| 5.5.11 | L | **No day/night visual.** Terrain, sprites and the sky look identical at noon and in the lunar night. A tint (or shadow direction) would make the key game mechanic visible without reading the HUD. | `moon_game.c` |
| 5.5.12 | L | **Portrait (720x1280) on the host is not supported by the game layout.** HUD, inspector and ribbon use the compact path (width < 800) but are not laid out for a tall screen. The P4 forces landscape, so this is relevant only if portrait is added later (plan Tab5 reader). | `moon_game.c` |

### 5.6 Game End (Restart / Quit)

| ID | Sev | Finding | Where |
| :--- | :-: | :--- | :--- |
| 5.6.1 | H | **There is no game over.** When `population == 0` a red banner is drawn and the simulation keeps running; `MOON_STATE_GAME_OVER` is never set. The player has no way to restart, only to quit with ESC or Q. | `moon_game.c:814`, `moon_game.h` |
| 5.6.2 | H | **No restart or new game.** A restart means leaving to `lsh` and running `moonbase` again. `moon_game_init()` cannot be called a second time safely (5.2.13). The seed is hard-coded (19690720) so every run has the same map. | `moon_game.c:32` |
| 5.6.3 | H | **No exit path for touch-only use** (see 5.4.2) and no confirmation on exit (5.4.7). | |
| 5.6.4 | M | **No victory or objective end.** The Act I goal (self-sustaining base) is never checked, so the game has no way to finish, and no "Act I complete" summary or hand-off to Act II exists. | plan §2 |
| 5.6.5 | M | **No end-of-game summary.** Days survived, peak population, resources produced and cause of failure are not recorded or shown. | |
| 5.6.6 | L | **Exit on the P4 is only partly verified.** `moon_platform_shutdown()` restores the canvas and the terminal, but only exit by Q/ESC is documented as tested. Exit by touch menu (64.11) and Ctrl-C need a hardware check, including the buffer release order (`s_render_fb` freed, touch queue not drained). | `platform_p4.c:198-238` |
| 5.6.7 | L | **The game state is not saved.** Leaving the game discards the base. | |

### 5.7 Act I Feature Completeness

Measured against plan §2 (Act I core loops) and §1.1 (resource chains):

| ID | Sev | Planned | State | Gap |
| :--- | :-: | :--- | :--- | :--- |
| 5.7.1 | H | Solar arrays and battery banking for the 14-day cycle | Done, with the day compressed to 180 s | Balance not verified (5.7.8) |
| 5.7.2 | H | **Regolith quarrying and sintering** for shielding and foundation pads | Sinterer produces regolith at a fixed rate, no excavation | No quarry or mining output per tile (`resource_val` is displayed but unused); sintered blocks and foundation pads are not modelled (`MOON_TILE_FLAG_FOUNDATION` marks the Lander tile only); the Habitat has no shielding requirement |
| 5.7.3 | H | **Ice harvesting and electrolysis** | The O2 Extractor only needs a nearby Ice or Mare tile and gives fixed O2 and water | No ice stock, no depletion, no output depending on the deposit; water and O2 are one machine; "electrolysis" (water to O2) is not a process |
| 5.7.4 | M | First pressurized habitat | Done (needs power only) | No pressure or airlock state; no O2 or water draw by the Habitat itself; crew growth ignores housing quality |
| 5.7.5 | M | Propellant and economy (fuel, LOX, exports) | Missing | Plan 64.5 lists Fuel; there is no fuel resource, no landing pad and no trade, even as a stub for Act II |
| 5.7.6 | M | **Objectives, tutorial and onboarding** | Missing | The player starts with 60 kg, a 4 minute O2 and water reserve and no guidance; 5.7.8 shows a passive base dies at game day 2 |
| 5.7.7 | M | Landing sequence (`MOON_STATE_LANDING`) | Missing | The game starts "landed"; an intro with the lander touching down and a short briefing is only a state in the enum |
| 5.7.8 | H | **Balance and difficulty** | Unverified | Idle base dies in about 4.5 minutes; a base with Lander, Habitat, Sinterer and Extractor draws about 66 kW against 10 kW of RTG at night. The sim moves 1 kWh per kW per second, so a 90 s night needs about 5000 kWh, i.e. about 20 batteries (400 kg regolith at 20 kg each, more than the 250 kg storage cap). Needs a balance pass, and a check that this is not an unintended consequence of the "120x" factor in the comment at `moon_resources.c:207`. No headless balance test exists. |
| 5.7.9 | M | Colony events and fail states | Partial | Casualties exist; no events (meteor, dust storm, equipment failure), `health` is always 100, no repair |
| 5.7.10 | L | Build catalog | 6 buildings + conduit | No "storage" building for regolith (cap 250 fixed), no "radiator" or heater for the cold warning (`warning_cold` is a flag only), no demolish refund for partly-built, no upgrade |
| 5.7.11 | L | Feedback | Text HUD only | No sound, no icons on the resource bar, no trend arrows (production vs consumption), no resource history |

### 5.8 New Milestones

Order is by dependency: correctness first (everything else builds on picking and layout), then render order, input, game flow, content and last performance tuning. 64.9 and 64.10 are independent of each other.

| Milestone | Status | Key Deliverables | Verification Criteria |
| :--- | :--- | :--- | :--- |
| **64.9** | **DONE** | **Correctness hotfixes.** Fix picking (5.2.1, round and elevation-aware); clear `warning_crew_lost` (5.2.2) and make the banners independent (5.2.16); split `warning_power` from the stress rule (5.2.8); clamp stored energy on capacity change (5.2.9); camera clamp from map and screen size (5.2.6); stop click-through and drags that start on UI (5.2.4, 5.2.5); bounded sim catch-up (5.3.7); move BFS buffers off the stack and run the solver only on change (5.2.11, 5.3.9); P4 key map `8 9 E` (5.2.12); regolith soft-lock guard (5.2.7: trickle income or a free recycle of the last structure's cost) | New unit tests: picking round-trip for all four quadrants of a tile and on raised tiles; the power solver (islands, standby, conduit); the stress rule; camera reaches all four map corners. All pass under ASan/UBSan; `make test` runs them |
| **64.10** | **DONE** | **Unified depth-sorted render pass.** One back-to-front pass per diagonal: terrain tile, then structure sprite (key `gx+gy`, footprint front corner for 2x2); separate overlay pass for badges, battery LED and conduit joints (5.5.1-5.5.3, 5.5.7); selection frame and ghost after sprites with a sprite preview (5.5.4, 5.5.5); sprite-aware picking (5.5.6); structure layer and exact viewport box in the minimap (5.5.9); common palette for TEX and FAST (5.5.10); optional day/night tint (5.5.11) | Host screenshot tests (`--dump-ppm`) of a fixed scene with overlapping buildings, an "OFF" badge behind a taller neighbour and a rim in front of a building; pixel checks that badge pixels survive; clicking a sprite's upper half selects that building |
| **64.11** | **DONE (host); hardware check open** | **Input and layout unification.** One shared UI rectangle table used by render and hit-test (5.4.8, 5.2.3); keyboard map cursor (arrows move the cursor, Enter places or selects, Tab cycles the ribbon, Backspace cancels) and continuous scroll; touch two-step placement with a visible ghost and a confirm button (5.4.3); touch-size targets (at least 44 px on the 7B, scaled on the Tab5; 5.4.4); on-screen menu button with Pause, Resume, Restart, Quit (5.4.2); hover cleared on touch and on window leave (5.4.5); cancel semantics identical on all devices, with quit only through the menu or a confirm (5.4.7); wheel zoom or documented pan (5.4.6); pause key `P` (Space stays "recentre"; the HUD shows PAUSED and the sim, clock and tint freeze) and 1x/2x/4x speed with keys `-`/`+` and on-screen buttons (5.4.9); **lunar clock widget** in the HUD (5.4.11): a 24 h Earth-equivalent dial in which one lunar day/night cycle maps onto 24 "base hours" (the game day is 180 s, so 7.5 s = 1 h), sun arc or day/night segments coloured like the tint, hour hand, and countdown to the next sunrise/sunset, so the player can plan battery stock | Clock: the hour maps to the cycle tick and the sunrise/sunset countdown matches `moon_resources`; pause freezes `sim_ticks` and the clock. Scripted event-injection tests (no SDL) drive a full build, inspect and recycle sequence by (a) mouse events, (b) touch events, (c) keyboard events and compare the final game state; on the 7B and Tab5 a person completes the same sequence by touch only |
| **64.11b** | **DONE (host); hardware check open** | **Select-first interaction** (replaces the tool-first / confirm-bar parts of 64.11 after a review on the P4 boards). A selected tile always exists; build, standby and recycle buttons act on it; build buttons show whether they could act; a ghost of a held or hovered build button; a double press arms a brush for repetition; ten-second undo; keys move the selection and the view glides to it | Same sequence by mouse, touch and keyboard (select, build, standby, undo, recycle) ends on identical state; button validity; touch peek; brush; undo window; Shift jump, Tab, Space; all under ASan |
| **64.12** | **TODO** | **Game flow: states, restart, end.** Real state machine (title or landing, playing, paused, game over, victory); `moon_game_init()` made re-entrant with a full reset and a new seed per run (5.2.13, 5.6.2); game-over overlay with Restart and Quit and a summary of days, peak crew and cause (5.6.1, 5.6.5); victory check and Act I summary (5.6.4); exit confirmation; P4 shutdown order fixed so touch hook, queue and buffers are released in a safe order (5.6.6); optional save of the base to `/sd0` (5.6.7, may slip to Act II) | Test: force population to 0 and see GAME_OVER, Restart gives a clean base, Quit returns to `lsh`; ASan run of 50 restarts shows no leak; P4: exit by menu, by Q and by Ctrl-C all return a clean text console |
| **64.13** | **TODO** | **Act I content and balance.** Objective list with a short tutorial path (5.7.6); landing intro (5.7.7); regolith quarrying from tile `resource_val` and sintered blocks or foundation pads (5.7.2); an ice stock with depletion and a separate electrolysis step (5.7.3); a Habitat that draws O2 and water and needs shielding (5.7.4); fuel resource and a landing pad stub for Act II (5.7.5); events and building health (5.7.9); regolith storage and heater buildings (5.7.10); balance pass of power, O2, water and costs using a headless simulation (5.7.8) | `tools`-less headless test plays a scripted "good" opening and survives 5 game days and a "do nothing" opening that loses in a bounded time; a balance table is committed in the plan; README and the plan catalog match the code (5.2.15) |
| **64.14** | **TODO** | **P4 performance.** Frame-time and FPS overlay and per-phase timers (terrain, structures, HUD, present) (5.3.8); dirty-flag rendering so an idle game renders at about 0 fps and present only on change (5.3.1); a visible-tile range instead of 4096 iterations (5.3.3); a cached terrain tile atlas or layer in place of per-pixel hashing (5.3.2); damaged-row upscale and writeback for the Tab5 (5.3.4); fixed fallback buffer handling (5.3.5); glyph row blits (5.3.6); remainder-of-frame sleep (5.3.8) | Measured on the LCD-7B and the Tab5 and recorded in this plan: median and p95 frame time while panning, with 3 and with 30 structures, textured and flat; target 30 fps minimum while panning and under 5% CPU while idle; no visual regression against the 64.10 reference screenshots |
| **64.15** | **TODO** | **Test and documentation closure.** Event fuzzing under ASan; a hardware test script for the board (`tests/hw`, following `test_esp32p4.py`), skipping when no P4 is attached; README and §6 rewritten to the real controls and numbers; plan status set to Complete only when 64.9-64.14 are done | `make test` and the hardware test pass; the plan header, README and the in-game help agree |

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
- Undo (`Z`, or the `Z:UNDO` button): one step, `MOON_UNDO_WINDOW_MS`; undoing a build refunds the full cost, undoing a recycle returns the building for the refunded half.
- Keys: arrows and WASD move the selection (Shift 8 tiles, `ev->mods`; on the P4 console a capital letter or a Shift-arrow), the view eases toward it (`glide_*`, any drag, wheel or minimap click cancels it); Space centres the view on the selection and then on the lander; Tab selects the next building.
- Sprite picking applies to selecting and the recycle brush; placement (a build brush) still targets the ground tile, so the tile behind a building can be built on.
- Tests: select-first moves, build on the selection, rejected builds, Shift jump, Tab, Enter, Space; standby, recycle and undo with its window; brush (double press, Enter, click, switching, end); button validity, hover preview and touch peek; the three-device sequence; touch tap/drag; menu and cancel equivalence.

### 5.9 Long-Term Features for Acts II/III (requested 2026-10-24; not scheduled)

**A. Operation of a growing base (automation or operators).** From about 15 buildings, toggling Standby by hand (and watching the battery at night) is micro-management. Two complementary designs, to be decided at the start of Act II:
1. *Operator crew (Act II).* Operators are a settler class imported from Earth (arrive with starships, need housing, O2, water, morale; cost upkeep). A building with a *control post* (or a Control Centre structure with a radius or capacity of N buildings) can only be switched or repaired while an operator is assigned; unattended buildings keep their last state but cannot be re-planned, and fail more often. This makes staffing a spending decision rather than a chore.
2. *Rules (Act III, the Lisp Cybernetics Terminal).* Declarative rules evaluated at the sim tick, first as a small in-game list ("IF battery < 20% THEN standby group `industry`; IF battery > 60% THEN activate it"), then as Lisp on the on-device interpreter (§2 Act III) with a safe API (`(power-flux)`, `(battery-pct)`, `(standby group)`, `(is-night?)`). Needs: building groups/tags, priority classes (life support never shed), hysteresis so rules do not flap, a rule log that shows what fired, and a cap on rule cost per tick (the kernel has no FP and a small heap).
Prerequisites in earlier milestones: a stable `moon_structure_t.id` and an `active` state flag with a reason (manual / rule / brownout), `revision` bumping (64.9) and a pause/speed control (64.11).

**B. Sprite animation and state icons.** Replace the text badges ("OFF", "!PWR") with a small icon set and add idle-versus-working animation: Solar (panels track the sun, glint), Battery (charge pulse), Sinterer (glow, steam puffs), O2 Extractor (rotating fan), Habitat (lit windows at night), conduits (current flow dots). Icons: standby (pause glyph), no power (bolt with slash), working (gear), damaged (wrench), staffed/unstaffed (operator glyph, once §5.9A exists). Design constraints: sprite sheets are Flash `.rodata`, one 3-4 frame strip per building, animation time from the sim clock (so pause stops it), drawn only when the building is on screen; with dirty-flag rendering (64.14) an animation forces redraw, so make it opt-out (`animations off`) and keep the idle target of ~0 fps when everything is in standby. Icons must also work at the Tab5's 2x scale and at 44 px touch size.

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
