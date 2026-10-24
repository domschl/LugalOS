# Moonbase Lambda — 2D Isometric Lunar Colony Simulation

*Moonbase Lambda* is a freestanding, retro 2D isometric lunar colony simulation and city-builder designed for **LugalOS** running on ESP32-P4 hardware targets (ESP32-P4-WIFI6-Touch-LCD-7B and M5Stack Tab5), as well as a native Linux desktop runner powered by SDL3.

---

## 1. Gameplay Overview (Act I: Survival & Bootstrapping)

You touch down at the lunar South Pole with an initial expedition lander and survival equipment. Your goal is to establish a permanent, self-sustaining outpost:

- **Day/Night Diurnal Cycle**: The Sun follows an accelerated lunar day/night cycle. Solar panels generate power during the daytime; cryogenic fuel cell batteries must store sufficient charge to survive the lunar night.
- **Power Grid**: Structures are powered when they touch a powered structure or a power conduit. The grid is solved by a breadth-first search from the Lander. The night lasts as long as the day, so solar must produce about twice the base's load on average to refill the batteries that carry it through the night.
- **Life Support**: Each colonist uses 1 kg of oxygen and 2 L of water every 30 s. Oxygen Extractors, built beside polar ice or basalt mare, make both. Low reserves are only warnings; the crew suffers when a tank is empty or the grid blacks out.
- **Construction & Regolith**: Sintering plants bake lunar regolith into building material. If nothing produces regolith the Lander scoops up to 15 kg so the base can always afford a Sinterer.
- **Base Management**: Select a building to inspect it, switch it to standby to save power, or recycle it for half its cost; `Z` undoes the last build or recycle.
- **Objectives**: A line at the bottom left walks you through the first hours (Extractor, solar, Sinterer, a second battery, Habitat, a crew of 8, Act I). `O` hides or shows it. Objectives stay done once reached.
- **Title page**: The game opens on a page where you pick the level (`1` Cadet, `2` Engineer, `3` Commander, `Q` quits). The map is the one chosen for this launch.
- **Difficulty**: Three levels (Cadet, Engineer, Commander) set the starting stocks, how far power goes, Sinterer output and how much the crew uses. Pick one on the title page or from the menu's `R:NEW GAME`. The levels are tuned with headless runs (`make balance`).

### Structure Catalog

| Structure | Footprint | Cost | Power | Notes |
| :--- | :---: | :---: | :---: | :--- |
| **Expedition Lander** | 1×1 | — | +10 kW (RTG), uses 5 | Housing for 4 colonists, 50 kWh emergency battery. Cannot be recycled. |
| **Solar Array** | 1×1 | 10 kg | up to +30 kW by day | Output follows the sun's height. |
| **Battery Bank** | 1×1 | 20 kg | −1 kW | Stores 250 kWh for the night. |
| **Habitat Dome** | 2×2 | 50 kg | −15 kW | Housing for 8 more colonists. |
| **Regolith Sinterer** | 1×1 | 15 kg | −20 kW | 30 kg of regolith per minute while powered (Engineer). |
| **Oxygen Extractor** | 1×1 | 25 kg | −25 kW | +20 kg O2 and +30 L water per minute; must stand beside ice or mare. |
| **Power Conduit** | 1×1 | 2 kg | 0 | Carries power between facilities. |

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
- **Pause, speed, menu**: The three buttons at the right of the ribbon. The menu (`ESC`) has Resume (`ESC`), the terrain mode (`T`), Restart (`R`) and Quit (`Q`); Restart and Quit ask for confirmation (`Y` / `N`). Every button shows its key in its label, in the ribbon, the inspector (`R` recycle, `E` standby, `Enter` or `Backspace` close) and the menu.

### Keyboard Controls
- **Arrow keys / W, A, S, D**: Move the selection to the adjacent tile in the direction they point, with the map rotated by 45 degrees: **Up** goes up-left, **Right** up-right, **Down** down-right, **Left** down-left (Shift: 8 tiles). Every tile is reachable, the opposite key undoes a step, and two different keys make a straight line on screen (Up then Right goes straight up). The view stays still until the selection nears the edge of the map area, then glides to centre it.
- **1 – 6**: Build on the selected tile; **R**: recycle it; **E**: toggle standby. Press a build key twice for a brush, **Enter** applies the brush to the selected tile.
- **Z**: Undo the last build or recycle. **Tab**: jump to the next building. **Enter**: open or close the inspector (when no brush is armed).
- **Space**: Bring the view to the selection; again, to the Expedition Lander.
- **O**: Hide or show the objective line. **P**: Pause / resume. **-** / **+**: simulation speed 1x, 2x, 4x. **T**: textured or fast terrain.
- **Backspace / 0**: End the brush or close the inspector.
- **ESC** / **Q**: The same, and with nothing to cancel it opens the menu. Quitting is only possible through the menu's confirmation (or Ctrl-C on the P4).

### How a run ends
- **Game over**: when the last colonist dies, the sim stops and an overlay shows the day reached, the peak crew, the number of buildings, the cause (no oxygen, no water or a power blackout) and the map seed. `R` or its button restarts on a new map, `Q` or its button quits to the shell.
- **Act I complete**: from day 4, with at least 6 crew, oxygen and water produced at least as fast as they are used, and no blackout for a whole lunar day, the base counts as self-sustaining. The overlay offers `C:CONTINUE` (or ESC), `R:RESTART` and `Q:QUIT`; it appears once per run.
- **Maps**: every run has its own seed, shown in the menu and in both overlays. The host takes `--seed=N` to replay a map; the P4 picks a random seed at every launch. A restart derives the next seed from the last. The landing apron and a polar ice deposit within reach are guaranteed for every seed.

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
