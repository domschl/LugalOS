# Ceres Alpha -- Subterranean Arcade Scroller

[Documentation](../README.md) > [User Manual](index.md) > **Ceres Alpha**

---

**Ceres Alpha** is a fast-paced 2D horizontal arcade scroller developed natively for LugalOS. Set in the subterranean cavern network and industrial mining facilities of dwarf planet Ceres, players pilot an advanced interceptor craft through treacherous geological formations, abducting alien drones, surface-to-air missile batteries, and fortified laser barriers to destroy the alien Reactor Core.

---

## 1. Hardware Targets & Display Presets

Ceres Alpha runs natively on ESP32-P4 hardware with hardware-accelerated 2D blitting and on host Linux workstations via SDL3:

* **M5Stack Tab5 (`esp32p4-tab5`)**:
  * 5" 720x1280 MIPI-DSI panel running in landscape mode.
  * Rendered at 640x360 virtual arcade resolution and integer scaled 2x to 1280x720 via the Pixel Processing Accelerator (PPA) Synchronous Resampling Module (SRM).
  * High-performance 60 FPS gameplay with minimal CPU overhead.
* **Waveshare ESP32-P4-LCD-7B (`esp32p4-lcd7b`)**:
  * 7" 1024x600 MIPI-DSI display running in 1x native resolution centered on the wide screen.
* **Host Simulator (`user/ceres/bin/ceres_host`)**:
  * SDL3 windowed host simulator for development, testing, and automated visual regression checks.
  * Supports `--tab5`, `--7b`, and `--native` resolution modes with SDL3 real-time chiptune audio output.

---

## 2. Flight Controls & Game Mechanics

### Pilot Controls

| Action | Primary Key | Alternative Key | Notes |
| :--- | :--- | :--- | :--- |
| **Thrust Up / Climb** | `Up Arrow` | `W` | Climbs toward cavern ceiling |
| **Thrust Down / Dive** | `Down Arrow` | `S` | Dives toward cavern bedrock |
| **Retro Brake** | `Left Arrow` | `A` | Decelerates forward screen position |
| **Forward Boost** | `Right Arrow` | `D` | Boosts forward across the screen |
| **Twin-Pulse Lasers** | `Space` | -- | Rapid twin forward lasers; destroys aerial craft and SAM rockets |
| **Gravity Bombs** | `B` | `K` / `Enter` | Parabolic falling bombs (max 2 concurrent); destroys ground installations |
| **Pause Game** | `P` | -- | Freezes action and displays pause banner |
| **Restart / Retry** | `R` | -- | Restarts mission immediately |
| **Quit to Shell** | `Q` | `Escape` | Exits cleanly back to `lsh>` prompt |

> [!TIP]
> **Keyboard Hold State-Machine**:
> LugalOS serial and USB consoles automatically manage key-down autorelease timers. Holding an arrow key or `WASD` maintains continuous, smooth flight without stuttering on key-repeat boundaries. Pressing an opposite direction instantly cancels the existing hold.

### Fuel Management & Flame-Out Glide
* Flying steadily consumes fuel at ~12 units/sec (1000 units = 100.0% tank).
* When fuel drops below 200 units, the HUD flashes a red `[LOW FUEL]` alert accompanied by warning beeps.
* If fuel reaches 0, the engine cuts out into an unpowered **Flame-Out Glide**: thrusters extinguish, the ship cannot climb, and gravity gently pulls the craft downward toward the bedrock floor.
* Bombing red **Fuel Silos** replenishes +25% fuel instantly. Rescuing stranded astronauts provides an emergency fuel top-up.

---

## 3. Mission Objectives & 6-Sector Campaign Pipeline

The campaign spans six procedurally generated sectors with increasing difficulty and distinct environmental hazards:

1. **Sector 1: Mountain Approach**
   * High-speed flight over rugged basalt peaks and rolling terrain.
   * SAM silos, radar dishes, and fuel silos introduce fundamental bombing skills.
2. **Sector 2: Crystal Cavern**
   * Claustrophobic subterranean cavern with jagged stalactites hanging from the ceiling and stalagmites rising from the floor.
   * Requires careful altitude control to avoid deadly rock collisions.
3. **Sector 3: Asteroid Trench**
   * Low-ceilinged rocky trench populated by inert tumbling meteors and diving alien fighters.
4. **Sector 4: Fortified Base**
   * Industrial alien mining complex guarded by high-voltage pulsating **Laser Barriers**.
   * Active force fields vaporize approaching craft and block forward laser fire.
   * Bombing the armored **Power Generator** pylon collapses the linked barrier, flashing `FORCE FIELD DOWN!` on the HUD.
5. **Sector 5: The Labyrinth**
   * Tight, undulating corridors with steep vertical shafts requiring aggressive climbing and retro-braking.
6. **Sector 6: Reactor Stronghold & Climax**
   * Heavy facility corridor leading into the grand alien reactor containment hall.
   * Deep within sits the vibrating **Alien Reactor Core** (3 Hit Points).
   * Bombing or blasting the Core triggers the **Base Meltdown Sequence**:
     * 3-second emergency countdown with seismic screen tremors (`screen_shake`) and strobing siren lights.
     * Successfully surviving the escape rush awards $+10,000$ points bonus plus astronaut survivor multipliers, refuels the craft, and rolls over into **Loop 2** with enhanced enemy aggression.

---

## 4. Search & Rescue Operations

Throughout the caverns, human astronaut personnel are stranded or imprisoned in cryo-bunkers:

* **Cryo-Bunker Breach**: Armored cryo-bunkers deflect laser fire. Delivering a precision bomb hit breaches the dome, liberating the survivor inside onto the bedrock floor.
* **Ventral Tractor Scoop**: Flying at low altitude directly above a stranded survivor activates the ship's ventral tractor beam, beaming the astronaut up into the rescue bay (+500 points, +200 fuel).
* **Alien Harvester Abductors**: Yellow abductor drones actively hunt ground survivors, descending with tractor beams to winch victims skyward.
* **Heroic Mid-Air Parachute Rescue**: Destroying an abductor while it is winching a survivor releases the astronaut into a gentle parachute descent. Scooping the falling parachutist mid-air awards a **HEROIC RESCUE** $+1000$ bonus!

---

## 5. Chiptune Audio Synthesizer

Ceres Alpha features a procedural audio synthesizer implemented entirely with pure fixed-point and integer arithmetic (zero floating-point math, fully safe for RISC-V M-mode `mstatus.FS = 0`):

* **Pulse Wave Voices**:
  * Dual-channel pulse wave generator with selectable duty cycles (50%, 25%, 12.5%).
  * Synthesizes rapid laser pew frequency sweeps, low-fuel alert beeps, and the meltdown siren.
* **Triangle Wave Voice**:
  * Smooth triangular waveform synthesizing the descending bomb drop whistle and sub-bass explosion rumbles.
* **16-bit Galois LFSR Noise Voice**:
  * Procedural pseudo-random noise generator with dynamic integer low-pass filtering.
  * Produces realistic explosion debris rumbles, rocket ignition blasts, and electric barrier arc discharges.
* **Arpeggiated Chime Channel**:
  * Sequenced 4-note and 5-note major arpeggios for rescue celebrations and mission completion fanfares.

On host systems, audio streams in real-time at 22,050 Hz via SDL3 audio device streams.

---

## 6. High-Score & Statistics Persistence

Ceres Alpha automatically persists high scores and career records to non-volatile storage:

* **Storage Paths**:
  1. `/sd0/ceres.dat` (MicroSD storage volume, preferred)
  2. `/flash0/ceres.dat` (Internal Flash storage fallback)
  3. `ceres.dat` (Local working directory on host simulator)
* **Persisted Data**:
  * Career high score (`hi_score`)
  * Total astronauts rescued across all games (`total_rescued`)
  * Total sorties flown (`games_played`)
  * Deepest sector reached (`highest_sector`)
  * Rotating 32-bit checksum verifying data integrity against corruption.

---

## 7. Launching & Testing

From the LugalOS command shell:
```text
lsh> ceres
```

For automated performance benchmarking or hardware-in-the-loop testing, specify the desired frame count:
```text
lsh> ceres 90
```
This runs Ceres for exactly 90 frames, reports average FPS and final score, releases all session locks, and returns cleanly to `lsh>`.
