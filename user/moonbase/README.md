# Moonbase Lambda — 2D Isometric Lunar Colony Simulation

*Moonbase Lambda* is a freestanding, retro 2D isometric lunar colony simulation and city-builder designed for **LugalOS** running on ESP32-P4 hardware targets (ESP32-P4-WIFI6-Touch-LCD-7B and M5Stack Tab5), as well as a native Linux desktop runner powered by SDL3.

---

## 1. Gameplay Overview (Act I: Survival & Bootstrapping)

You touch down at the lunar South Pole with an initial expedition lander and survival equipment. Your goal is to establish a permanent, self-sustaining outpost:

- **Day/Night Diurnal Cycle**: The Sun follows an accelerated lunar day/night cycle. Solar panels generate power during the daytime; cryogenic fuel cell batteries must store sufficient charge to survive the lunar night.
- **Power Grid**: Structures must be connected to power generators via power conduits or direct adjacency. The power grid is simulated using a breadth-first search graph solver.
- **Life Support**: Habitat domes support colonist life support. Oxygen extractors harvest $O_2$ from regolith and ice pockets. If power or oxygen fails, life support degrades.
- **Construction & Regolith**: Sintering plants quarry lunar regolith dust into structural materials used to build new facilities.
- **Base Management**: Use the Inspector tool to inspect buildings, view power/efficiency metrics, toggle standby mode to conserve energy, or recycle structures for materials.

### Structure Catalog

| Structure | Footprint | Cost | Description |
| :--- | :---: | :---: | :--- |
| **Expedition Lander** | 2×2 | — | Initial colony command module, radio beacon, and emergency backup battery. |
| **Solar Array** | 1×1 | 25 kg | Photovoltaic generation during daylight (10 kW). |
| **Battery Bank** | 1×1 | 40 kg | Cryogenic fuel cell battery storing up to 500 kWh for night operations. |
| **Habitat Dome** | 2×2 | 100 kg | Pressurized living quarters for up to 4 colonists; requires continuous power & $O_2$. |
| **Regolith Sinterer** | 1×1 | 50 kg | Excavates surface dust and sinters construction regolith blocks (1 kg/s). |
| **O2 Extractor** | 1×1 | 60 kg | Electrolytic extraction plant generating oxygen for the base (0.2 kg/s). |
| **Power Conduit** | 1×1 | 5 kg | Transmits electrical current between distant base facilities. |

---

## 2. Controls & Interaction

Moonbase Lambda is designed to be played seamlessly using a mouse, keyboard, or capacitive touchscreen:

### Touch & Mouse
- **Pan / Scroll Viewport**: Touch-drag or click-and-drag across the lunar surface.
- **Select Build Tool**: Tap any tool icon in the bottom ribbon (Solar, Battery, Habitat, Sinterer, O2, Conduit, Recycle, or Inspect).
- **Place Building**: Tap on any valid terrain tile. Tool automatically deselects upon successful placement.
- **Inspect Building / Tile**: Tap in Inspect mode (Button 0) to open the building inspector drawer.
- **Minimap Radar**: Tap anywhere on the radar (bottom right) for instant camera navigation.
- **Fast Terrain Toggle**: Tap the `TEX:ON` / `TEX:FAST` button above the radar to toggle between textured and flat-shaded terrain.

### Keyboard Controls
- **Arrow Keys** / **W, A, S, D**: Pan camera across the lunar surface.
- **Space**: Recenter camera on the Expedition Lander.
- **1 – 6**: Select building from build catalog.
- **R**: Toggle Recycle / Demolish tool.
- **0**: Cancel active tool / return to Inspect mode.
- **T**: Toggle terrain texturing mode (Textured vs. Fast Retro).
- **ESC** / **Q**: Cancel tool, close inspector, or exit cleanly to the LugalOS shell (`lsh`).

---

## 3. Architecture & Engine

- **Freestanding C**: Built with `-nostdlib -ffreestanding`. Zero external dependencies or POSIX requirements.
- **Zero Floating-Point**: Entirely fixed-point math (`fixed32_t`, 16.16) for all projections, elevation relief, resource rates, and solar trigonometry. Safe for RISC-V M-mode (`mstatus.FS = 0`).
- **Software RGB565 Rasterizer**: Direct 16-bit rendering with isometric block extrusion, depth-sorted tile blitting, font typography, and dirty double-buffering.
- **Macro-Crater Terrain Texturing**: Flash-compressed 4-bit packed texture unpacking at boot into a 32 KB RGB565 buffer in PSRAM (`BULK_BSS`), mapped seamlessly across tiles without repeating patterns.
- **Dual Hardware Targets**:
  - **ESP32-P4-WIFI6-Touch-LCD-7B**: Native 1024×600 fullscreen canvas.
  - **M5Stack Tab5**: 1280×720 physical presentation with 2× integer HiDPI scaling (640×360 virtual resolution).

---

## 4. Building & Running

### Host Simulator (Linux / SDL3)
```bash
# Compile and run unit tests with AddressSanitizer
make test

# Run interactive host simulator
./bin/moonbase_host

# Run simulator matching Tab5 display profile
./bin/moonbase_host --tab5
```

### Bare-Metal LugalOS Targets
```bash
# Build for ESP32-P4-LCD-7B
ninja -C build/esp32p4-lcd7b
python3 tools/p4flash.py --board lcd7b --build build/esp32p4-lcd7b --only boot,os --verify

# Build for M5Stack Tab5
ninja -C build/esp32p4-tab5
python3 tools/p4flash.py --board tab5 --build build/esp32p4-tab5 --only boot,os --verify

# Launch interactive game on hardware via LugalOS shell
lsh> moonbase
```
