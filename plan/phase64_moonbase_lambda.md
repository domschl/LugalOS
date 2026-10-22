# Phase 64 — Moonbase Lambda: 2D Isometric Lunar City-Builder

**Status**: In Progress  
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

---

## 2. Architecture & Directory Structure

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

## 3. Platform Hardware Abstraction Layer (HAL)

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

## 4. Milestones & Phased Execution

| Milestone | Status | Key Deliverables | Verification Criteria |
| :--- | :--- | :--- | :--- |
| **64.1** | **IN PROGRESS** | Phase plan, project tree, Platform HAL (`moon_platform.h`), Host SDL3 runner, `Makefile` with ASan/UBSan | Clean compile, SDL3 window opens at 1024×600 and 1280×720, exits cleanly on ESC/close, ASan clean |
| **64.2** | Planned | Fixed-point math (`moon_fixed.h`), 16-bit RGB565 blitter (`moon_blitter.c`), dirty rects, procedural glyph rendering | Flat shading, color fills, sprite blits with transparency key, 60 FPS verified |
| **64.3** | Planned | Isometric projection math, screen-to-grid raycasting/picking, procedural lunar terrain generation (regolith, craters, elevation) | Smooth camera pan/drag, correct tile highlighting under cursor |
| **64.4** | Planned | Structure catalog (Lander, Solar Array, Battery, Habitat, Regolith Sinterer, O2 Plant), footprint & placement rules | Validated placement, collision detection, road/cable network connections |
| **64.5** | Planned | Resource simulation loop (Energy, Regolith, Oxygen, Water, Fuel), Lunar Day/Night cycle, life support stress | Discrete tick updates, battery discharge during night, resource starvation warnings |
| **64.6** | Planned | UI HUD (top resource bar, build menu ribbon, inspector pane), touch controls, minimap overview | Responsive UI, touch-friendly tap/drag gestures, full settlement gameplay loop |
| **64.7** | Planned | LugalOS bare-metal integration (`platform_p4.c`), CMake integration into LugalOS build system, PSRAM canvas binding | Runs on ESP32-P4-LCD-7B and Tab5 bare metal with native touch and double buffering |
