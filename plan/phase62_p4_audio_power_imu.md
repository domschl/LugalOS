# Phase 62 — ESP32-P4 Terminals: Audio, Battery & Charging, IMU

**Status: planned. Written 2026-10-18.**
Targets: `esp32p4-tab5` (all parts), `esp32p4-lcd7b` and `esp32p4` (NANO) (audio).
Builds on phase 46 (sensor framework), phase 47 (LCD-7B), phase 56 (Tab5). Feeds phase 58
(AI agent / LLM): the microphone and speaker are the agent's voice path.

**Milestone scheme: `62.1`, `62.2`, … in three parts:**
- **Part A (62.1–62.6):** Tab5 platform: I2C ownership, RTC, battery, charging, IMU, orientation.
- **Part B (62.7–62.11):** audio on all three P4 boards: I2S, codecs, `/dev/audio`, files, tones.
- **Part C (62.12–62.14):** streaming groundwork: WebSocket client, stream protocol, host reference server.

Part A comes first. It is smaller, and 62.1 fixes a bug that is live today (§0.1).

---

## 0. Findings That Shape the Plan

### 0.1 The Tab5's IMU is detected as an RTC (live bug)

Measured on the Tab5, build 953:
```
lsh> rtc
RTC Model: DS1307 (at 0x68)
RTC Time (UTC): 2000-00-00 00:21:24
[    0.104] [I2C RTC] DS1307 at 0x68 answered, but stored time is out of range.
lsh> date
2026-08-05 15:03:21 CEST          <- actual date 2026-10-18
```
0x68 on the Tab5 is the **BMI270**, not a DS1307. `i2c_rtc_identify_part()` falls through to
"DS1307" for any part at 0x68 that is not a DS3231, and its probe writes register 0x11 of
whatever answers. A `date` set would write the BMI270's registers 0x00–0x06. The Tab5's real RTC,
the **RX8130CE at 0x32** (with a backup battery, `BT1`), is not used, so the clock is wrong after
every power cycle until NTP runs.

### 0.2 Board-soldered parts collide with the hub's sensor addresses

| Addr | Board part | Phase-46 / RTC probe that also uses it | Boards |
| :--- | :--- | :--- | :--- |
| 0x18 | ES8311 codec | MCP9808 (0x18–0x1F) | NANO, LCD-7B |
| 0x40 | ES7210 mic ADC | HDC1080 | LCD-7B, Tab5 |
| 0x68 | BMI270 IMU | DS1307/DS3231 RTC | Tab5 |

Today the ID checks reject the codecs, but the probes still write registers on parts that are
soldered down. Once the codecs are running, a `sensor init` must not touch them. External sensors
belong on the Tab5's **Grove port A** (second I2C bus, GPIO53 SDA / GPIO54 SCL), not on SYS_I2C.

### 0.3 Audio hardware: every P4 board has a speaker path and a microphone

| | NANO | LCD-7B | Tab5 |
| :--- | :--- | :--- | :--- |
| DAC / output | ES8311 (0x18) | ES8311 (0x18) | ES8388 (0x10), speaker + 3.5 mm headphone |
| ADC / mics | ES8311's ADC, 1 analog mic | ES7210 (0x40), 2 mics (MIC1/2) | ES7210 (0x40), 4 inputs (below) |
| PA enable | GPIO53 (NS4150B) | GPIO53 | PI4IOE1 (0x43) P1 `SPK_EN` (NS4150B) |
| I2S MCLK/BCLK/WS | 13 / 12 / 10 | 13 / 12 / 10 | 30 / 27 / 29 |
| I2S DOUT / DIN | 9 / 11 | 9 / 11 | 26 / 28 |
| Headphone detect | — | — | PI4IOE1 P7 `HP_DET` |

Sources: NANO and 7B schematics (`~/Source/gith/esp/datasheet/`), the 7B board file, M5Unified
`_speaker_enabled_cb_tab5` / `_microphone_enabled_cb_tab5`.

**ES7210 inputs on the Tab5** (read from the schematic layout; confirm in 62.10):
MIC1/MIC2 are the two onboard MEMS mics (`AMIC1`, `AMIC2`). **MIC3 is `AEC_P`, the ES8388's
output fed back as an echo-cancellation reference.** MIC4 is `HPMIC`, the headset mic. In TDM mode
the ES7210 puts all four on one data line (GPIO28). M5Unified uses only MIC1/2 in stereo.

The ES8388 (output) and ES7210 (input) share one I2S0 bus (MCLK, BCLK, WS): it runs full duplex,
TX and RX on the same clocks.

### 0.4 P4 I2S facts (TRM ch. 49)

- Three I2S controllers support standard, TDM (Philips / MSB / PCM) and PDM TX/RX modes.
- Data moves through **GDMA-AHB**, which LugalOS has no driver for yet. The DW-GDMA used for DSI
  scanout is the AXI one.
- An LP I2S (ch. 50) has a hardware voice-activity detector that can wake the chip. It is for PDM
  mics on LP pins, which none of these boards have. It is noted for later, not planned.

### 0.5 Tab5 power path (schematic + M5Unified `Power_Class.inl`)

- Battery: 2S Li-Po (`SYS_BAT_2S`). The user observed **8.23 V full** and **6.0 V empty**, where the
  supervisor forces a shutdown.
- **INA226 (0x41)**, with a 5 mΩ shunt on `SYS_BAT_2S`:
  - Bus voltage LSB is 1.25 mV. Shunt LSB is 2.5 µV, which is 0.5 mA through 5 mΩ.
  - Charge current reads negative. M5Unified inverts it so that + means charging.
  - M5's configuration: 16 averages, 1.1 ms conversions, shunt and bus continuous.
- **IP2326** 2S charger, controlled through PI4IOE2 (0x44):
  - P7 `CHG_EN` (1 = charge).
  - P5 `nCHG_QC_EN` (0 = "QC"). M5Unified's `setChargeCurrent()` maps 0 → off, 500 mA → CHG_EN=1
    and QC off, 1000 mA → CHG_EN=1 and QC on.
  - P6 `CHG_STAT` is an input (charging status).
  - **LugalOS already boots with `OUT_SET = 0x89`, which is QC on.** By M5's mapping that is the
    1 A mode. This has not been measured; 62.4 checks it with the INA226.
- **Power off:** 10 pulses of 50 ms on PI4IOE2 P4 `PWROFF_PULSE` go to the `PMS150G` supervisor.
- The INA226 `ALERT` pin drives a 2N7002 (Q12). Where Q12 leads has not been traced yet.
- The BMI270's INT1/INT2 do not appear to reach a P4 GPIO, so the IMU is polled.

### 0.6 An unidentified device at 0x28 on SYS_I2C

M5Stack documents SYS_I2C (G31 SDA / G32 SCL) as: ES8388 0x10, GT911 0x14, RX8130CE 0x32,
SC2356 0x36, ES7210 0x40, INA226 0x41, PI4IOE5V6408 0x43 and 0x44, ST7123 0x55, BMI270 0x68.
GT911 and ST7123 are alternatives (display variants); this unit has the ST7123.

`i2c scan` on this unit finds 10 devices: the nine documented ones it carries, plus **0x28, which
M5Stack does not document**. 62.1 identifies it (read-only) before any probe touches it.

---

## 1. Design

### 1.1 Board-owned I2C addresses

Each board file declares the I2C addresses that belong to soldered-down parts:
```cmake
set(CONFIG_I2C_BOARD_ADDRS "0x10,0x28,0x32,0x36,0x40,0x41,0x43,0x44,0x55,0x68")   # Tab5 SYS_I2C
```
`gen_config` turns this into a sorted `.rodata` table. `i2c_bus_addr_is_board(bus, addr)` lets the
sensor hub, the DS-series RTC probe and `sensor init` skip those addresses. `i2c scan` prints them
with their owner's name, for example `68 bmi270`. The RTC driver is chosen per board
(`CONFIG_RTC_RX8130` on the Tab5) instead of being found by probing 0x68.

### 1.2 Sensor framework extensions (phase 46 contract, extended, not forked)

New channels in `sensor_chan_t`, all fixed-point:

| Channel | Unit (int32) | Source |
| :--- | :--- | :--- |
| `SENSOR_CHAN_VOLTAGE` | 1 mV | INA226 bus |
| `SENSOR_CHAN_CURRENT` | 1 mA, signed, + = charging | INA226 shunt |
| `SENSOR_CHAN_POWER` | 1 mW, signed | V × I |
| `SENSOR_CHAN_BATT_SOC` | 0.01 % | battery model (`SENSOR_TIER_INFERRED`) |
| `SENSOR_CHAN_ACCEL_X/Y/Z` | 1 mg | BMI270 |
| `SENSOR_CHAN_GYRO_X/Y/Z` | 0.1 °/s | BMI270 |

Three changes to the hub:
1. **A sampling period per device.** The hub runs one 60 s loop today. A device gets an optional
   `period_ms`: the INA226 at 1 s, the BMI270 at 100 ms while auto-orientation is on, otherwise at
   the default. The hub task sleeps until the earliest deadline. The I2C cost is small, about 1 ms
   per BMI270 read at 400 kHz.
2. **A `SENSOR_DEV_INTERNAL` flag.** Board-internal parts (INA226, BMI270) never become the
   "primary" source of an ambient channel. The BMI270's die temperature must not replace a BME280's
   room temperature. Their channels still appear in `/proc/sensors` and on MQTT, named by
   device (`ina226.voltage`, `bmi270.accel_x`).
3. **Probing the Grove bus (bus 1) on the Tab5**, so a BME280 on port A appears as it does on the
   7B's PH2.0 header.

`/proc/sensors`, `sensor`, MQTT (`mqttd` sources) and the Lisp readers pick the new channels up
through the existing paths. No consumer gets a special case.

### 1.3 Battery model (pure integer)

- **Voltage-based SoC with IR compensation.** `V_ocv = V_bus + I × R_int`. `R_int` is a board
  constant (start at 150 mΩ for the pack) that 62.3 refines from logs. `V_ocv` per cell is
  looked up in a 12-point OCV table (4.20 V → 100 %, …, 3.00 V → 0 %) with linear interpolation.
  The endpoints are set to the observed 8.23 V and 6.00 V (`CONFIG_BATT_FULL_MV`, `CONFIG_BATT_EMPTY_MV`).
- **Coulomb counting on top.** The 1 s current samples are integrated into mAh. The count is
  re-anchored to the OCV estimate after |I| < 30 mA for 60 s, and when charging ends (CHG_STAT
  drops while on USB). The learned capacity is stored in the identity record, so the estimate
  improves over a few cycles.
- **Smoothing.** The SoC shown is an EMA of the estimate and moves by at most 1 % per 30 s, so
  backlight changes do not make the number jump.
- **Calibration tool.** `power log /sd0/batt.csv` records t, V, I and CHG_STAT once a second.
  `tools/batt_fit.py` fits the OCV table and `R_int` from one full discharge and charge run.

### 1.4 Charging and power control

| Mode | CHG_EN | nCHG_QC_EN | Expected |
| :--- | :--- | :--- | :--- |
| `off` | 0 | 1 | no charging (battery storage, measurements) |
| `slow` | 1 | 1 | ~500 mA (M5: "500 mA") |
| `fast` | 1 | 0 | ~1 A (M5: "1000 mA"; LugalOS's boot state today) |

- The mode is persisted in the identity record. The default stays `fast` until 62.4's
  measurement says otherwise.
- **Charge limit (suggested extension).** For a tablet that lives on its dock: with
  `power limit 80`, LugalOS clears CHG_EN above 80 % and sets it again below 75 %. This is
  software hysteresis on the model's SoC, and is off by default.
- **Low-battery shutdown.** Below 6.6 V (under 10 %), the menu bar warns. Below 6.2 V for 30 s
  continuously, LugalOS logs it, flushes and unmounts `/sd0`, then sends `PWROFF_PULSE`.
  Shutting down this way is safer than letting the supervisor cut power at 6.0 V in the middle of a
  FAT write.
- `power off` sends the same pulse train on request.

### 1.5 IMU and auto-orientation

- **BMI270 driver.** Chip ID 0x24, accelerometer at ±4 g and 100 Hz ODR, gyroscope at
  ±1000 °/s. Data registers 0x0C–0x17.
- **Open question (decide in 62.5): Bosch's 8 KB configuration blob.** The datasheet's
  initialisation sequence uploads it to the chip; M5Unified ships it as `BMI270_config.inl`
  (BSD-3, from Bosch's SensorAPI). Phase 46 ruled out proprietary blobs for the BME680. The blob
  drives the on-chip feature engine (any-motion, step counter, orientation), which this phase does
  not need. **Step one is to test whether raw accelerometer and gyro data stream without the
  upload.** If they do, there is no blob. If they don't, the user decides.
- **Orientation in the kernel, not on the chip.** From the gravity vector (integers only):
  - Landscape when |ax| > |ay| × 1.73 (30° hysteresis, squared comparisons, no sqrt); portrait
    for the mirror-image condition.
  - The sign picks the 0° or 180° variant.
  - Nothing changes while the device lies flat (|az| > 0.8 g) or is moving (gyro magnitude above a
    threshold).
  - A new orientation must hold for 600 ms before it is applied.
- **Mapping onto the compositor.** Each state maps to `lcd orient`. The axis mapping from IMU to
  panel is measured on the device and kept in the board file (`CONFIG_IMU_AXIS_MAP`), next to
  `CONFIG_TOUCH_MIRROR_*`. If the compositor has no 180° variants yet, landscape and inverted
  landscape both map to landscape until it does.
- **Control:**
  - `orient auto|off|lock`. `lock` freezes the current orientation.
  - The setting is persisted, and the default is **off**. Auto is an opt-in, as asked.
  - The reader's `o` key still toggles by hand, which sets `lock`.

### 1.6 Audio architecture

```
  shell: play / rec / beep / tone / vol      Lisp: (audio-play) (beep) (sfx …)      9P clients
                      │                                   │                          │
                      ▼                                   ▼                          ▼
       /dev/audio (PCM r/w)   /dev/audioctl (text)   /dev/volume        ← Plan 9's own names
                      │
          kernel/audio.c: stream table, mixer (N play streams, saturating Q15),
          capture fan-out, rate/format check, PSRAM ring buffers, audio task
                      │
       ┌──────────────┴───────────────┐
       ▼                              ▼
  drivers/i2s_esp32p4.c         codec contract (audio_codec_ops_t)
  TDM TX/RX, full duplex,       es8311.c   (NANO, 7B: DAC + ADC)
  fractional MCLK divider       es8388.c   (Tab5: DAC, HP/speaker routing)
       │                        es7210.c   (7B, Tab5: 2/4-channel ADC, TDM)
       ▼
  drivers/gdma_ahb_esp32p4.c   (new: channel alloc, circular descriptor lists, EOF interrupt)
```

- **A codec contract from the start.** There are three codecs, and the third-implementation rule
  is satisfied. `audio_codec_ops_t` has `init`, `set_rate`, `power(dir, on)`, `set_volume`,
  `set_gain`, `mute` and `route` (speaker or headphone; mic, headset or reference). The board file
  names which codec serves output and which serves input.
- **Formats.** S16LE inside the kernel. Rates are 8, 16, 22.05, 24, 32, 44.1 and 48 kHz, with
  mono or stereo at the device file. MCLK = 256·fs comes from the I2S fractional divider off the
  160 MHz PLL. The TRM's divider formula is checked against IDF for the jitter it gives at 44.1 kHz.
- **DMA buffers in internal SRAM** (4 × 10 ms of descriptors). The rings behind them live in
  PSRAM, and the audio task copies between the two. This avoids GDMA-AHB reading PSRAM through the
  cache, the coherence lesson from `lcd wbtest`. At 48 kHz stereo, 10 ms is 1920 bytes per buffer.
- **The mixer.** Each play stream has its own volume. The sum saturates, then the master volume
  applies. Games' sound effects and the assistant's speech play together without either owning the
  device.
- **Capture fan-out.** Several readers (a file recorder and a network stream) can read the same
  capture. Each reader picks its channels:
  - `mic` (default): MIC1/2 averaged to mono, or stereo.
  - `ref`: the Tab5's AEC loopback.
  - `headset`
  - `all4`: raw TDM.
- **Headphones (Tab5).** `HP_DET` switches ES8388 routing and `SPK_EN` automatically, and
  `audioctl` shows the result.
- **Echo cancellation is left to the backend.** A full AEC is too heavy for this phase, and
  Espressif's AFE is a binary blob. The stream can carry the mic and the `ref` channel together,
  and the server cancels the echo (speexdsp or WebRTC APM). Without a reference (NANO, 7B unless
  its MIC3 is wired the same way), capture is ducked while playback runs.
- **Device files.** `/dev/audio`, `/dev/audioctl` and `/dev/volume`, after Plan 9.
  - Writing PCM to `/dev/audio` plays it; reading records.
  - `audioctl` takes lines like `rate 16000`, `chans 1`, `in mic|ref|headset|all4`, `gain 24`,
    `out speaker|headphone|auto`.
  - Over 9P a host can play to or record from any board with no new protocol. This is the first,
    already-working form of "streaming".

### 1.7 Streaming groundwork

The aim is a defined, tested path from the microphone to a backend and back, without committing
to a particular ASR or TTS engine.

- **Transport: WebSocket plus JSON** (AGENTS.md's rule for bidirectional real-time traffic).
  `net/websocket.c` is a minimal RFC 6455 client over `net/tcp.c`: HTTP Upgrade, masked
  client frames, text and binary frames, ping/pong, close. The phase-58 agent can reuse it.
- **Protocol, `doc/dev/protocols/audio_stream.md`:**
  ```
  → {"type":"start","rate":16000,"format":"s16le","chans":1,"ref":false,"id":"<node>"}
  → binary: 20 ms PCM frames                         ← {"type":"partial","text":"…"}
  → {"type":"stop"}                                  ← {"type":"final","text":"…"}
  ← {"type":"play","rate":22050,"format":"s16le","chans":1}
  ← binary: PCM to play                              ← {"type":"play_end"}
  ```
  With `"ref":true`, the device sends two channels (mic, ref). An optional `"format":"ima-adpcm"`
  (4:1, integer, a few dozen lines) is held back unless bandwidth calls for it. 16 kHz mono PCM is
  256 kbit/s, small next to what LLP carries.
- **Start and stop.**
  - First, push-to-talk: a key, a touch on a menu-bar button, or `stream start|stop`.
  - Then an energy VAD: integer RMS over 20 ms frames, a noise floor that tracks the room, and an
    attack/release hysteresis. It decides when to stop, not when to wake. No wake word in this phase.
- **Host reference server** (`host/audio/`, Python + `uv`, typed for `pyrefly`). It records
  incoming streams to WAV, plays back a WAV as the "answer", and logs the JSON. A hook lets a user
  plug in whisper.cpp or piper; none is bundled.
- **Encryption.** The stream is cleartext on the LAN until phase 52 Part B brings TLS. The
  doc says so, and the client keeps the TCP and TLS seam open.

---

## 2. Milestones

### Part A — Tab5 Platform

**62.1 I2C ownership, RX8130CE RTC, Grove bus, the 0x28 device**
- Add `CONFIG_I2C_BOARD_ADDRS` to all three P4 board files, `i2c_bus_addr_is_board()`, and owner
  names in `i2c scan`.
- Make the sensor hub, `sensor init` and the DS-series probe skip board addresses.
- Identify 0x28: register dumps, then the schematic, with no blind writes.
- `drivers/rx8130.c` (BCD time registers, VLF "voltage low" flag treated like the DS3231's OSF).
  Sync from it at boot, set it from `date` and NTP. `rtc` shows "RX8130CE (at 0x32)".
- Bring up the second P4 I2C controller on GPIO53/54 (Grove port A, `EXT5V_EN` on), as bus 1,
  and have the hub probe it.
- Done when the Tab5 keeps the right time across a power cycle without network, `rtc` no longer
  claims a DS1307, and a BME280 on Grove shows in `sensor`.

**Done (2026-10-18):**
- Board files list their parts (`CONFIG_I2C0_PARTS`, generator-checked ascending).
- `i2c_bus_part()` and `i2c_probe_begin/end()`. The hub's driver inits and the DS probe run
  in probe windows.
- `i2c scan` names the board parts and now goes through the bus task instead of driving the
  controller under it.
- The ESP I2C driver keeps state per controller. I2C1 is on GPIO53/54 on the Tab5, and its
  scan of the empty port is clean.
- The RX8130CE is in `drivers/i2c_rtc.c` (`CONFIG_RTC_RX8130`). STOP is held while writing and
  VLF is cleared afterwards. A year before 2024 counts as "never set". Set, chip reset and
  restored at boot, verified.
- 0x28 could not be identified. It ACKs and reads 0xff from any register. SYS_I2C also reaches
  the M5-Bus and the expansion header, so it may not be on the board. It is reserved as
  `undocumented`.
- Tests: Tab5 46/46 and NANO 46/46.
- Open:
  - The LCD-7B was not attached, so it is unverified.
  - A BME280 on the Tab5's Grove port has not been tried.
  - `reboot` is not supported on the P4 (noted, not in scope).

**62.2 Sensor framework: new channels, per-device period, internal flag**
- Add the channels in §1.2, their descriptors and formatting, MQTT names, and the `/proc/sensors` layout.
- Per-device `period_ms`, and a hub task that sleeps to the earliest deadline.
- `SENSOR_DEV_INTERNAL` and per-device naming.
- Host tests: channel formatting and scheduler deadline order. `sensor_derived_selftest` covers
  the new math.

**62.3 INA226 driver and battery model**
- `drivers/ina226.c` as a sensor driver: manufacturer ID 0x5449 and die ID 0x2260 check,
  configuration as in §0.5, calibration register for 0.5 mA LSB, voltage, current and power.
- `drivers/battery.c`: OCV table, IR compensation, coulomb counter, EMA and rate limit, a
  `BATT_SOC` channel. Golden vectors run on host and QEMU.
- `power` command:
  ```
  lsh> power
  battery   7.86 V   -412 mA (discharging)   3.24 W   SoC 71 %   ~2 h 50 min
  charger   fast (CHG_EN=1, QC=on)  status: not charging   USB: present
  ```
  `power log FILE [s]` records for calibration. `/dev/power` reads the same text and takes
  writes of `charge …`, `limit …` and `off`, for 9P and `lota`-style tools.
- A battery percentage and charge marker in the menu bar, next to HH:MM.
- One full discharge and charge log, then `tools/batt_fit.py` replaces the starting OCV table
  and `R_int`.

**62.4 Charging control and power off**
- `power charge off|slow|fast`, persisted; CHG_STAT readback.
- **Measure** the charge current in `slow` and `fast` with the INA226 at a fixed SoC. Record
  the result in this plan and in the board file, and settle the default.
- `power limit N|off` (hysteresis, §1.4).
- `power off`; low-battery warning and shutdown with `/sd0` flushed (§1.4).
- Trace the INA226 `ALERT` → Q12 path. If it is a hardware undervoltage cut, set the INA226's
  alert limit to match §1.4's thresholds instead of fighting it.

**62.5 BMI270 driver**
- Chip ID, soft reset, power configuration, accelerometer and gyro configuration. Test raw data
  without the config blob first (§1.5) and **stop for a user decision** if the blob is needed.
- Register as an internal sensor (accel and gyro channels).
- `imu` command: one reading, or `imu watch [s]` for a live view with derived pitch and roll,
  using an integer atan2 to 1°.

**62.6 Auto-orientation**
- `kernel/orient.c` state machine (§1.5), driven by the hub's 100 ms BMI270 period while enabled.
- Measure `CONFIG_IMU_AXIS_MAP` on the device.
- `orient auto|off|lock`, persisted, default off. Wire the reader's `o` key to `lock`.
- Host test: the state machine against recorded gravity traces (tilts, flat, shakes, slow turns).

### Part B — Audio (NANO, LCD-7B, Tab5)

**62.7 GDMA-AHB driver**
- `drivers/gdma_ahb_esp32p4.c`: channel claim and peripheral selection, circular descriptor
  rings, EOF interrupt through the CLIC, error counters.
- Self-test: memory-to-memory transfer (`dma test`).

**62.8 I2S0 driver, TDM, full duplex**
- `drivers/i2s_esp32p4.c`: pin routing through the matrix from the board file, MCLK fractional
  divider, standard and TDM slot layouts, TX and RX on shared clocks, start, stop and underrun
  handling (play silence, count it).
- `i2s` diagnostics: rate, actual MCLK, underrun and overrun counters, a register dump for IDF
  diffs.
- Check: an internal loopback of the I2S TX→RX bit pattern, before any codec is involved.

**62.9 Codec contract and output: ES8311, ES8388, PA, headphones**
- `drivers/include/drivers/audio_codec.h`, `es8311.c` (DAC path), `es8388.c` (DAC, speaker
  and headphone outputs, click-free power up and down as in M5Unified's sequence).
- PA enable (GPIO53, `SPK_EN`), switched only around playback so the speaker does not hiss
  when idle. `HP_DET` auto-routing on the Tab5.
- `beep [hz] [ms]` and `tone hz [s]` from an integer sine (a quarter-wave table plus
  interpolation).
- Done when `beep` works on all three boards.

**62.10 Input: ES7210, ES8311 ADC**
- `es7210.c`: 2-channel (7B) and 4-channel TDM (Tab5), per-mic gain, mic bias.
  `es8311.c` gets its ADC path (NANO).
- Confirm the Tab5's MIC3 = AEC reference and MIC4 = headset by recording each slot while a tone
  plays. Read the 7B's MIC3/4 wiring from its schematic.
- `rec` levels meter: `audio level [s]` prints integer RMS and peak dBFS per channel.

**62.11 Audio core, device files, WAV, Lisp**
- `kernel/audio.c`: streams, mixer, capture fan-out, PSRAM rings, audio task (§1.6).
- `/dev/audio`, `/dev/audioctl`, `/dev/volume` in the VFS. Playing and recording over 9P from
  the host (`host/p9lib`) is tested.
- WAV (RIFF PCM16) reader and writer, with only the needed chunks parsed and others skipped.
  Rates are converted to the device rate by a linear-interpolation resampler, which is enough for
  speech and effects and is noted as such.
- Shell commands:
  - `play FILE.wav`
  - `rec FILE.wav [s] [rate] [in]`
  - `vol [0-100]`
  - `audio` (status)
  - `play`/`rec` run as background tasks; Ctrl-C stops them.
- Lisp primitives:
  - `(audio-play path)`, `(audio-stop)`, `(beep hz ms)`, `(volume n)`.
  - Chiptune voices for games: `(sfx voice wave hz ms vol)`. `wave` is square, triangle, saw,
    noise, or a 12-step envelope. These are integer oscillators in the mixer.
  - Example: chess plays a move click, z-machine plays `@sound_effect` 1/2 (high/low beep).

### Part C — Streaming Groundwork

**62.12 WebSocket client**
- `net/websocket.c` (§1.7): upgrade handshake with SHA-1 for `Sec-WebSocket-Accept`. The
  SHA-1 is shared with whatever phase 52 Part B brings. Also masking, fragmentation on receive,
  ping/pong, close codes.
- `ws` diagnostic command: `ws connect URL`, `ws send TEXT`, `ws close`.
- QEMU test against a host echo server through the existing slirp networking.

**62.13 Audio stream protocol and client**
- Write `doc/dev/protocols/audio_stream.md`.
- `kernel/audio_stream.c`: a capture reader → WebSocket binary frames, and received `play` frames →
  a mixer stream.
- `stream start URL [rate] [ref]`, `stream stop`, `stream` (status: frames, drops, round trip).
- Push-to-talk binding (key and menu-bar touch button), then the energy VAD for the end of speech.
- An `audio_stream_*` Lisp interface for the phase-58 agent to call.

**62.14 Host reference server**
- `host/audio/`: a WebSocket server (Python, `uv`, typed) that records WAV per session, answers
  with a configured WAV or a TTS hook, and offers ASR hooks with no engine bundled.
- If `"ref":true`, it can run speexdsp AEC on the two channels.
- End to end: a Tab5 records a spoken sentence, the server stores it, and the board plays the
  server's answer through the speaker while capture keeps running.

---

## 3. Testing

**Host and QEMU (`tests/runner.py`):**
- INA226 conversions, battery model vectors (charge, discharge, re-anchoring), integer atan2,
  the orientation state machine on recorded traces.
- WAV reader and writer round trips, resampler, mixer saturation, sine table accuracy, VAD on
  recorded frames.
- WebSocket framing (masking, fragmentation, control frames).

**Hardware (`tests/hw/test_esp32p4.py`), each check skipping where a board lacks the part:**
- Tab5:
  - `rtc` reports RX8130CE.
  - `i2c scan` names the board's parts.
  - `power` gives a voltage between 6.0 and 8.4 V and a current reading.
  - `power charge` changes CHG_EN (read back from the expander).
  - `imu` gravity magnitude is 1 g ± 10 %.
- **Audio, all P4 boards, with no microphone in the loop:** the Tab5 plays a tone and records the
  `ref` slot. A Goertzel detector (integer) checks the frequency and level. This is a
  deterministic, electrical loopback.
- **Acoustic loopback** (speaker to onboard mic, all three boards): the same check with a lower
  level threshold. Marked as needing a quiet room.
- `/dev/audio` over 9P: a host writes a WAV, then records 1 s and checks the length and that the
  audio is not silence.
- WebSocket and stream test against `host/audio` on the test host. The firewall needs a rule, as
  the MQTT test does.

## 4. Documentation

- `doc/user/shell.md`: `power`, `orient`, `imu`, `play`, `rec`, `beep`, `tone`, `vol`, `audio`,
  `stream`, `ws`.
- `doc/user/sensors.md`: the new channels, internal devices, the Grove bus on the Tab5.
- New `doc/user/audio.md` (playback, recording, device files, playing from a host over 9P) and a
  battery and charging section in `doc/admin/` (calibration, the charge limit, low-battery
  shutdown).
- `doc/dev/on_device/lisp_scripting.md`: the audio and sfx primitives.
- `doc/dev/protocols/audio_stream.md`.
- AGENTS.md §3.5 (Tab5): RTC, power, IMU, audio facts. §3.4 (7B): audio pins and the ES7210.
- `python3 tools/gendocs.py --all`, then `--check` before every commit.

## 5. Decisions for the User

1. **BMI270 config blob**: only relevant if raw data needs it (62.5 tests this first). Options are
   to accept the BSD-3 Bosch blob in flash `.rodata`, or to have no IMU.
2. **Auto-orientation default**: off is proposed.
3. **Charge default**: `fast` (today's state) until 62.4 measures; whether the charge limit
   should default to on for a docked tablet.
4. **Stream transport**: WebSocket is proposed (§1.7). Alternatives are raw TCP, which is simpler
   but needs a custom framing, or 9P only, which already works through `/dev/audio` but is awkward
   for a cloud backend.
5. **Compression**: PCM only, with IMA-ADPCM held back. Opus is a separate phase if a cloud
   backend over thin links requires it.

## 6. Out of Scope (Catalogued)

- **SC2356 camera (MIPI-CSI)**: its own phase, since the CSI receiver, ISP and frame buffers in
  PSRAM are a project the size of 47.4/47.5.
- **RS-485 (SIT3088, GPIO20/21/34)**: a small item (UART1 plus a DIR GPIO), possibly with a
  Modbus RTU master. It could join a later phase or be slotted after 62.6 if wanted.
- **Antenna switch** (PI4IOE1 P0, internal or external): a one-line `wlan antenna int|ext` that
  can ride along with 62.4 if desired.
- MP3 or Opus decoding, wake word, on-device AEC, the LP I2S VAD.
- Sleep and wake (light sleep with touch or IMU wake): it depends on a P4 power-management
  phase that does not exist yet.
