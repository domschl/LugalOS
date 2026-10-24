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

A tile is always **selected** (the gold frame; the minimap shows it as a white dot). Build, standby and recycle act on the selected tile; a build button is dimmed when it could not act there, and the line at the bottom left says what is under the selection (or under the mouse).

### Touch & Mouse
- **Select**: Tap or click a tile or a building (its sprite counts). A building also opens the inspector.
- **Build / recycle / standby**: Press a ribbon button: `1:SOLAR` ... `6:CABLE` build on the selected tile, `R:RECYCLE` removes the selected building for 50%, `E:STANDBY` switches it. Hovering a build button with the mouse, or holding it on a touchscreen, shows a ghost of what it would build; slide off the button to cancel.
- **Undo**: The `Z:UNDO` button above the ribbon (ten seconds) reverses the last build (full refund) or recycle.
- **Brush**: Press the same build button twice (or double-tap it) to keep it armed: every tile you then tap or click gets that building (cables!). Press the button again, right-click or ESC to end it. `R` twice arms a recycle brush.
- **Pan**: Drag on the map (touch, left or middle button); the mouse wheel pans too. The minimap jumps to a place.
- **Pause, speed, menu**: The three buttons at the right of the ribbon. The menu has Resume, the terrain mode (textured or fast), Restart and Quit; Restart and Quit ask for confirmation.

### Keyboard Controls
- **Arrow keys / W, A, S, D**: Move the selected tile by half a tile width in a screen direction (Shift: 8 steps). Up goes up-left, then up-right, and so on, so the line is straight on screen and every tile can be reached; the opposite key undoes a step. The view stays still until the selection nears the edge of the map area, then glides to centre it.
- **1 – 6**: Build on the selected tile; **R**: recycle it; **E**: toggle standby. Press a build key twice for a brush, **Enter** applies the brush to the selected tile.
- **Z**: Undo the last build or recycle. **Tab**: jump to the next building. **Enter**: open or close the inspector (when no brush is armed).
- **Space**: Bring the view to the selection; again, to the Expedition Lander.
- **P**: Pause / resume. **-** / **+**: simulation speed 1x, 2x, 4x. **T**: textured or fast terrain.
- **Backspace / 0**: End the brush or close the inspector.
- **ESC** / **Q**: The same, and with nothing to cancel it opens the menu. Quitting is only possible through the menu's confirmation (or Ctrl-C on the P4).

### Day/night clock
The dial at the top left is a 24 h clock for the lunar cycle (one cycle is 180 s of game time): dawn is 06:00, zenith 12:00, dusk 18:00 and the middle of the night 00:00. The text beside it shows the lunar day, the time and the time until the next dusk or dawn.

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
