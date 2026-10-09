# Phase 46 — A Unified I2C Sensor Framework

**Status: active. Milestones 46.1–46.17 concluded with silicon verification on real hardware (RP2350 Pico 2 W + multi-sensor bench). Written 2026-10-06, updated 2026-10-09.**
Succeeds `plan/phase26_mqtt_and_environment_sensors.md` (concluded) and stands beside `plan/phase45_esp32c6.md` (planned).

**Milestone scheme: `46.1`, `46.2`, `46.3`, …**

---

## 0. Motivation & The Third Implementation Rule

LugalOS's core driver design rule ([`drivers/README.md`](file:///home/dsc/Source/gith/domschl/lugalos/drivers/README.md#L10-L26)) mandates:
> *"Extract at the third implementation, never at the second. Two copies is evidence that something recurs. Three is a design."*

Until now, LugalOS supported exactly one environmental sensor: the BMP280 / BME280 ([`drivers/bme280.c`](file:///home/dsc/Source/gith/domschl/lugalos/drivers/bme280.c)). That driver was deliberately kept ad-hoc: it managed its own I2C transactions, maintained its own internal cache, launched its own sampler task, directly fed `/proc/sensors` in [`fs/vfs_server.c`](file:///home/dsc/Source/gith/domschl/lugalos/fs/vfs_server.c#L1290), and was hard-wired into the shell's `sensor` command ([`kernel/shell.c`](file:///home/dsc/Source/gith/domschl/lugalos/kernel/shell.c#L1260)) and the MQTT daemon ([`net/include/net/mqttd.h`](file:///home/dsc/Source/gith/domschl/lugalos/net/include/net/mqttd.h)).

We now expand the environmental measurement suite to eight sensors:
1. **BME280** (Baseline: Temperature, Pressure, Humidity)
2. **BME680** (Temperature, Pressure, Humidity, MOX Gas Resistance — **strictly without proprietary binary blobs**)
3. **TSL2561** (Dual-channel Ambient Light / Lux)
4. **TSL2591** (High Dynamic Range Light / Lux)
5. **CCS811** (MOX Air Quality: eCO2 and TVOC)
6. **SGP30** (Multi-pixel Gas Sensor: eCO2, TVOC, raw signals)
7. **MiCS-6814** (Grove Multichannel Gas Sensor v1.0: CO, NO2, NH3 via I2C at 0x04)
8. **MH-Z19B** (NDIR Infrared True CO2 & chamber temperature via UART1 at 9600 baud)

Going from 1 sensor to 8 sensors crosses the threshold from a single driver to a formal **Category D (Device-class contract)**. Doing so without a structured framework would replicate register-pumping loops, sampling tasks, staleness timers, EMA filters, and `/proc` formats eight times across the codebase.

### Official Datasheets Reference
Component datasheets for all eight parts are stored in the host repository tree under:
`~/gith/sensors/` or `~/Source/gith/sensors/`
* BME280: `bst-bme280-ds002.pdf`
* BME680: `bst-bme680-ds001.pdf`
* CCS811: `CCS811_Datasheet-DS000459.pdf`
* SGP30: `Sensirion_Gas_Sensors_Datasheet_SGP30.pdf`
* TSL2561: `TSL2561.pdf`
* TSL2591: `TSL25911_Datasheet_EN_v1.pdf`
* MiCS-6814: `MiCS-6814_Datasheet.pdf` and `'Multichannel gas sensor 1.0.pdf'`
* MH-Z19B: `mh-z19b-co2-ver1_0.pdf`


---

## 1. System Invariants & Architectural Constraints

Every piece of this framework must strictly observe the following core rules of LugalOS:

1. **Freestanding C (`-nostdlib -ffreestanding`)**: No libc dependencies, no dynamic heap allocations during normal sampling loops.
2. **Zero Floating-Point in Kernel / M-Mode (`mstatus.FS = 0`)**: The RISC-V boot path boots with the hardware FPU disabled. FPU instructions or soft-float `libm` calls cause an unhandled **Illegal Instruction** trap. All calibration math, exponential moving averages, and unit conversions must execute in **pure fixed-point or exact integer arithmetic**.
3. **Category E (1:n Bus Arbitration)**: All I2C sensors share a single physical bus with RTCs ([`drivers/i2c_rtc.c`](file:///home/dsc/Source/gith/domschl/lugalos/drivers/i2c_rtc.c)) and EEPROMs ([`drivers/at24c32.c`](file:///home/dsc/Source/gith/domschl/lugalos/drivers/at24c32.c)). The underlying bus access is mediated exclusively by [`i2c_xfer()`](file:///home/dsc/Source/gith/domschl/lugalos/drivers/include/drivers/i2c_bus.h#L74-L75). No sensor driver may touch the hardware controller registers directly.
4. **Non-Blocking Cache & Staleness (`age_s`)**: The VFS / 9P server task serving `/proc/sensors` must **never** perform blocking I2C transactions. An autonomous sampler task updates a shared cache, and all readers obtain timestamped values with explicit age.
5. **Deterministic Selftests without Hardware**: Every sensor's compensation arithmetic must be pure and testable on QEMU / host using golden reference vectors (extending [`tools/bme280_reference.py`](file:///home/dsc/Source/gith/domschl/lugalos/tools/bme280_reference.py)).

---

## 2. The Three Abstraction Layers

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           APPLICATION CONSUMERS                             │
│     [ /proc/sensors ]   [ shell: sensor ]   [ mqttd ]   [ Lisp REPL ]       │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │
┌──────────────────────────────────────▼──────────────────────────────────────┐
│                  LAYER 3: SENSOR HUB & POST-PROCESSING                      │
│   drivers/sensor_hub.c                                                      │
│   • Autonomous Sampler Task (updates cache without blocking bus callers)     │
│   • Central Sensor Cache & Staleness (age_s)                                │
│   • Standalone EMA Filtering (alpha_shift) & Publish Rules (delta / rate)   │
│   • Cross-Sensor Compensation Pipeline (feeds T & H into CCS811 / SGP30)    │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │
┌──────────────────────────────────────▼──────────────────────────────────────┐
│               LAYER 2: SENSOR DRIVERS (Category D Contract)                 │
│   drivers/include/drivers/sensor.h                                          │
│   • Standard Device Contract: sensor_dev_t, sensor_ops_t                    │
│   • Standard Metric Units in Fixed-Point (int32_t, NO FLOAT)                │
│   • Drivers:                                                                │
│     - drivers/bme280.c            - drivers/bme680.c (Blob-free!)           │
│     - drivers/tsl2561.c           - drivers/tsl2591.c                       │
│     - drivers/ccs811.c            - drivers/sgp30.c                         │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │
┌──────────────────────────────────────▼──────────────────────────────────────┐
│                    LAYER 1: I2C HARDWARE & REGISTER BUS                     │
│   drivers/include/drivers/i2c_reg.h & drivers/i2c_bus.c                     │
│   • Register Helpers: read/write u8, u16 (BE/LE), buffer transfers          │
│   • Universal Bus Seam: i2c_xfer() (U-mode driver task or direct M-mode)   │
│   • SoC Hardware Controllers: RP2350 (DW_apb), ESP32-P4, ESP32-C6           │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Layer 1 Detailed Design: I2C Register Bus

Every environmental sensor communicates via a register read/write pattern (write register pointer, optionally repeat-start and read payload). Rather than open-coding buffer packing in every driver, Layer 1 introduces standard register helpers over [`i2c_xfer()`](file:///home/dsc/Source/gith/domschl/lugalos/drivers/include/drivers/i2c_bus.h#L74-L75):

```c
/* drivers/include/drivers/i2c_reg.h */
#ifndef DRIVERS_I2C_REG_H
#define DRIVERS_I2C_REG_H

#include <stdint.h>
#include <stdbool.h>

bool i2c_reg_read_u8(uint8_t addr, uint8_t reg, uint8_t *val);
bool i2c_reg_write_u8(uint8_t addr, uint8_t reg, uint8_t val);

bool i2c_reg_read_u16_be(uint8_t addr, uint8_t reg, uint16_t *val);
bool i2c_reg_read_u16_le(uint8_t addr, uint8_t reg, uint16_t *val);

bool i2c_reg_read_bytes(uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t len);
bool i2c_reg_write_bytes(uint8_t addr, uint8_t reg, const uint8_t *buf, uint32_t len);

/* Raw command write without register prefix (for sensors like SGP30 / CCS811) */
bool i2c_cmd_write(uint8_t addr, const uint8_t *cmd, uint32_t len);

#endif /* DRIVERS_I2C_REG_H */
```

---

## 4. Layer 2 Detailed Design: Sensor Contract & Drivers

### 4.1 Standardized Channels and Metric Fixed-Point Units

All quantities are scaled as 32-bit signed integers (`int32_t`):

| Channel Enum | Measurement | Unit & Scaling | Format | Example |
| :--- | :--- | :--- | :--- | :--- |
| `SENSOR_CHAN_TEMP` | Air Temperature | $0.01\ ^\circ\text{C}$ (centi-degrees) | `int32_t` | `2150` = $21.50\ ^\circ\text{C}$ |
| `SENSOR_CHAN_PRESSURE` | Barometric Pressure | $1\ \text{Pa}$ (Pascals) | `int32_t` | `101325` = $1013.25\ \text{hPa}$ |
| `SENSOR_CHAN_HUMIDITY` | Relative Humidity | $0.01\ \%\text{RH}$ (centi-percent) | `int32_t` | `5520` = $55.20\ \%\text{RH}$ |
| `SENSOR_CHAN_LUX` | Ambient Illuminance | $0.01\ \text{Lux}$ (centi-Lux) | `int32_t` | `45000` = $450.00\ \text{Lux}$ |
| `SENSOR_CHAN_ECO2` | Equivalent $\text{CO}_2$ | $1\ \text{ppm}$ (parts per million) | `int32_t` | `420` = $420\ \text{ppm}$ |
| `SENSOR_CHAN_TVOC` | Total VOC | $1\ \text{ppb}$ (parts per billion) | `int32_t` | `125` = $125\ \text{ppb}$ |
| `SENSOR_CHAN_GAS_RES` | MOX Gas Resistance | $1\ \Omega$ (Ohms) | `int32_t` | `125400` = $125.4\ \text{k}\Omega$ |
| `SENSOR_CHAN_CO` | Carbon Monoxide | $0.01\ \text{ppm}$ (centi-ppm) | `int32_t` | `1598` = $15.98\ \text{ppm}$ |
| `SENSOR_CHAN_NO2` | Nitrogen Dioxide | $0.01\ \text{ppm}$ (centi-ppm) | `int32_t` | `11` = $0.11\ \text{ppm}$ |
| `SENSOR_CHAN_NH3` | Ammonia | $0.01\ \text{ppm}$ (centi-ppm) | `int32_t` | `311` = $3.11\ \text{ppm}$ |
| `SENSOR_CHAN_CO2` | True NDIR $\text{CO}_2$ | $1\ \text{ppm}$ (parts per million) | `int32_t` | `1443` = $1443\ \text{ppm}$ |

### 4.2 The `sensor_dev_t` Contract

```c
/* drivers/include/drivers/sensor.h */
#ifndef DRIVERS_SENSOR_H
#define DRIVERS_SENSOR_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SENSOR_CHAN_TEMP = 0,
    SENSOR_CHAN_PRESSURE,
    SENSOR_CHAN_HUMIDITY,
    SENSOR_CHAN_LUX,
    SENSOR_CHAN_ECO2,
    SENSOR_CHAN_TVOC,
    SENSOR_CHAN_GAS_RES,
    SENSOR_CHAN_CO,
    SENSOR_CHAN_NO2,
    SENSOR_CHAN_NH3,
    SENSOR_CHAN_CO2,
    SENSOR_CHAN_MAX
} sensor_chan_t;

struct sensor_dev;

typedef struct sensor_ops {
    /* Probes chip ID, loads factory calibration, configures power/forced mode */
    bool (*init)(struct sensor_dev *dev);
    /* Triggers conversion, waits/yields for completion, applies math models */
    bool (*sample)(struct sensor_dev *dev);
    /* Retrieves a calibrated metric value */
    bool (*get_value)(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out_val);
    /* Diagnostic selftest running pure math against reference vectors */
    uint32_t (*selftest)(bool report);
} sensor_ops_t;

typedef struct sensor_dev {
    const char         *name;       /* e.g. "bme280", "bme680", "tsl2591" */
    uint8_t             addr;       /* Detected I2C address */
    uint32_t            chan_mask;  /* Bitmask of (1u << SENSOR_CHAN_*) */
    const sensor_ops_t *ops;
    void               *priv;       /* Driver state & factory calibration block */
} sensor_dev_t;

#endif /* DRIVERS_SENSOR_H */
```

### 4.3 Target Sensor Specifications & Strategy

#### 1. BME280 (Baseline Reference)
* **Datasheet**: `bst-bme280-ds002.pdf`
* **Address**: `0x76` / `0x77`. Chip ID: `0x60` (BME280), `0x58` (BMP280).
* **Channels**: Temp, Pressure, Humidity.
* **Refactor**: Wrap [`drivers/bme280.c`](file:///home/dsc/Source/gith/domschl/lugalos/drivers/bme280.c) into the `sensor_dev_t` contract. Keep the existing datasheet integer formulas and verification script ([`tools/bme280_reference.py`](file:///home/dsc/Source/gith/domschl/lugalos/tools/bme280_reference.py)).

#### 2. BME680 (Environmental & MOX Gas — Zero Binary Blobs)
* **Datasheet**: `bst-bme680-ds001.pdf`
* **Address**: `0x76` / `0x77`. Chip ID: `0x61`.
* **Channels**: Temp, Pressure, Humidity, Gas Resistance ($R_{\text{gas}}$).
* **The No-Blob Solution**: Bosch provides a closed-source binary library ("BSEC") to compute an arbitrary "Index of Air Quality" (IAQ 0–500) and estimate VOC baseline drift. **LugalOS rejects all binary blobs.**
  * The actual BME680 hardware measures physical gas resistance in Ohms ($\Omega$) across a heated metal-oxide plate.
  * Target heater calculation ($R_{\text{target}}$ based on heater temperature $\sim 320\ ^\circ\text{C}$ and duration $\sim 150\ \text{ms}$) is fully documented in the public datasheet.
  * Gas resistance calculation is evaluated using public 64-bit integer formulas and the datasheet lookup table (identical to the Linux kernel IIO driver `drivers/iio/chemical/bme680_core.c`).
  * Pure integer compensation script: `tools/bme680_reference.py`.

#### 3. TSL2561 (Dual-Channel Light Sensor)
* **Datasheet**: `TSL2561.pdf`
* **Address**: `0x29`, `0x39`, or `0x49`. Chip ID: `0x0A` register.
* **Channels**: Lux.
* **Mechanism**: Command register bit 7 high (`0x80 | reg`). Dual photodiodes: Channel 0 (broadband visible + IR) and Channel 1 (IR).
* **Math**: Piecewise linear integer approximation of $\text{Lux} = (a \cdot \text{CH0} - b \cdot \text{CH1}) \times \text{scale}$ across five ratio brackets.

#### 4. TSL2591 (High Dynamic Range Light Sensor)
* **Datasheet**: `TSL25911_Datasheet_EN_v1.pdf`
* **Address**: `0x29`. Chip ID: `0x50` at register `0x12`.
* **Channels**: Lux.
* **Mechanism**: Command register `0xA0 | reg`. Wide dynamic range (600,000,000:1) with programmable gain ($1\times$ to $9876\times$) and integration times (100 ms to 600 ms).
* **Math**: Counts Per Lux ($\text{CPL}$) fixed-point calculation.

#### 5. CCS811 (MOX Air Quality: eCO2 & TVOC)
* **Datasheet**: `CCS811_Datasheet-DS000459.pdf`
* **Address**: `0x5A` or `0x5B`. Hardware ID: `0x81` at register `0x20`.
* **Channels**: eCO2 (ppm), TVOC (ppb).
* **Hardware Pin Caveat**: Active-low `/WAKE` pin. If not hardwired low on the breakout board, must be driven low before I2C transactions.
* **State Machine**: Boots into bootloader; must verify `APP_VALID` status and issue `APP_START` (0xF4) before reading `ALG_RESULT_DATA` (0x02). Mode 3 (60-second low-power pulse) is selected for environmental monitoring.
* **Environmental Tuning**: Accepts ambient temperature and humidity via `ENV_DATA` (0x05) to tune internal baseline drift.

#### 6. SGP30 (Sensirion Multi-Pixel Gas Sensor)
* **Datasheet**: `Sensirion_Gas_Sensors_Datasheet_SGP30.pdf`
* **Address**: `0x58`.
* **Channels**: eCO2 (ppm), TVOC (ppb).
* **Protocol**: 16-bit commands (e.g. `0x2008` `measure_iaq`, `0x2003` `iaq_init`). Every 2 bytes of data are followed by an 8-bit CRC (`CRC-8-Dallas/Maxim`, polynomial `0x31`, init `0xFF`).
* **Environmental Tuning**: Supports absolute humidity compensation via command `0x2061`.

#### 7. MiCS-6814 (Grove Multichannel Gas Sensor v1.0)
* **Datasheet**: `MiCS-6814_Datasheet.pdf` and `'Multichannel gas sensor 1.0.pdf'`
* **Address**: `0x04` on the shared I2C bus.
* **Architecture**: The MiCS-6814 sensor contains three independent micro-machined semiconductor gas sensor elements on a heated silicon substrate (RED for reducing gases / CO, OX for oxidizing gases / NO2, NH3 for ammonia). The Grove v1.0 board features an onboard MCU (ATmega168PA/STM32) acting as an I2C slave coprocessor that drives the heaters and samples the raw ADC channels.
* **Channels**: CO ($0.01\ \text{ppm}$), NO2 ($0.01\ \text{ppm}$), NH3 ($0.01\ \text{ppm}$).
* **Protocol**: Commands include version query (`0x00`), raw ADC read (`0x01..0x06`), and preheat state.
* **Preheating**: Requires a 10-minute warm-up stabilization period (`MICS6814_WARMUP_PERIOD_S 600u`) for the heater elements to reach chemical equilibrium. Driver guards and flags readings during preheating.

#### 8. MH-Z19B (NDIR Infrared Carbon Dioxide Sensor)
* **Datasheet**: `mh-z19b-co2-ver1_0.pdf`
* **Interface**: 9600 baud 8N1 UART (PL011 UART1 on RP2350, GP8 TX / GP9 RX).
* **Power Requirement**: $4.5\text{V} - 5.5\text{V}$ DC on $V_{in}$ connected directly to Pico Pin 40 (`VBUS`, 5V USB power). Logic signals are 3.3V TTL compatible.
* **Channels**: True Physical $\text{CO}_2$ ($1\ \text{ppm}$ resolution, 0–2000 or 0–5000 ppm range) and internal optical chamber temperature ($0.01\ ^\circ\text{C}$).
* **Physical Principle**: Non-Dispersive Infrared (NDIR) optical absorption at $4.26\ \mu\text{m}$. Unlike metal-oxide sensors, it is immune to ethanol, VOCs, and reducing gases, serving as the ground-truth standard for $\text{CO}_2$.
* **Protocol**: Query command `0x86` (`0xFF 0x01 0x86 0x00 0x00 0x00 0x00 0x00 0x79`). Response format: `0xFF 0x86 HIGH LOW TEMP ... CHK`. Checksum algorithm: `(uint8_t)(~sum + 1)`.
* **Automatic Baseline Calibration (ABC)**: Supports disabling ABC (`0x79 0x00`) to prevent baseline corruption in unventilated indoor spaces.
* **Preheating**: Requires a 3-minute warm-up stabilization period (`MHZ19B_WARMUP_PERIOD_S 180u`).

### 4.4 Hardware Bench Cabling & Pinout Map (RP2350 Pico 2 W)

The current multi-sensor test bench aggregates 5 active environmental sensors across two hardware buses on the Raspberry Pi Pico 2 W (`rp2350-sensor` preset):

```
       Raspberry Pi Pico 2 W Pinout & Sensor Bus Topology
       ─────────────────────────────────────────────────
               [ USB Micro / Type-C Power & Console ]
                           ┌───────────┐
     UART0 TX (Console)──1 ┤ GP0   VBUS├ 40── MH-Z19B Vin (5V Power)
     UART0 RX (Console)──2 ┤ GP1   VSYS├ 39
  External Heartbeat LED──4 ┤ GP2    GND ├ 38── MH-Z19B & I2C Common GND
             I2C0 SDA ───6 ┤ GP4   3V3 ├ 36── I2C Sensors VCC (3.3V)
             I2C0 SCL ───7 ┤ GP5       │
        MH-Z19B TXD ────11 ┤ GP8 (TX1) │
        MH-Z19B RXD ────12 ┤ GP9 (RX1) │
                           └───────────┘

1. Shared 3.3V I2C Bus (GP4 SDA / GP5 SCL @ 100 kHz):
   • BME680:     Addr 0x76 (Temp, Pressure, Humidity, Gas Resistance)
   • SGP30:      Addr 0x58 (Multi-pixel MOX: eCO2, TVOC)
   • CCS811:     Addr 0x5a (MOX: eCO2, TVOC; /WAKE hardwired to GND)
   • MiCS-6814:  Addr 0x04 (Grove Multichannel Gas: CO, NO2, NH3)
   • TSL2591:    Addr 0x29 (when fitted on bench)

2. Dedicated 5V/3.3V UART1 Bus (GP8 TX / GP9 RX @ 9600 baud 8N1):
   • MH-Z19B:    Vin to Pin 40 (VBUS, 5V); GND to Pin 38;
                 Sensor TXD -> Pico GP9 (Pin 12, UART1 RX);
                 Sensor RXD -> Pico GP8 (Pin 11, UART1 TX).

3. Status Indication:
   • External Heartbeat LED moved to GP2 (Pin 4) to ensure GP9 is
     dedicated exclusively to UART1 RX.
```


---

## 5. Layer 3 Detailed Design: Sensor Hub & Post-Processing

### 5.1 Decoupled EMA Filtering & Publishing Rules

The filtering logic currently coupled inside [`net/mqtt.c`](file:///home/dsc/Source/gith/domschl/lugalos/net/mqtt.c) is moved to a general-purpose engine in `drivers/sensor_hub.c`:

```c
typedef struct {
    uint16_t min_interval_s;   /* Rate limit: never publish more often */
    uint16_t max_interval_s;   /* Heartbeat: always publish at least this often */
    int32_t  delta;            /* Publish when filtered value moves by >= delta */
    uint8_t  alpha_shift;      /* EMA filter weight: y += (x - y) >> alpha */
} sensor_rule_t;

typedef struct {
    int32_t  raw_val;
    int32_t  filtered_val;
    int32_t  last_published_val;
    uint64_t last_sample_ms;
    uint64_t last_published_ms;
    bool     valid;
    bool     dirty;            /* Trigger flag for publishers */
} sensor_channel_state_t;
```

### 5.2 Sensor Hub Responsibilities

1. **Autonomous Sampler Task**:
   * Runs periodically (e.g. 5–10 s configurable).
   * Iterates across all probed sensors and invokes `ops->sample()`.
   * Updates EMA filter values and staleness timers (`age_s`).
   * Evaluates publish criteria: marks `dirty = true` if `now - last_pub >= max_interval` or `abs(filtered - last_pub) >= delta`.
2. **Cross-Sensor Compensation Pipeline**:
   * If a BME280/BME680 is present alongside a CCS811 or SGP30, the Hub automatically routes ambient $T$ and $H$ into the gas sensor's compensation registers.
3. **Pluggable Multi-Consumer Architecture**:
   * **MQTT Daemon (`mqttd`)**: Replaces the hardcoded `MQTTD_MAX_SOURCES 4` table. Simply queries the Sensor Hub for `dirty` channels and publishes `<prefix>/<node>/<channel_name>`.
   * **VFS Server (`/proc/sensors`)**: Formats all active sensors and their metric values into standard key-value lines without blocking on I2C.
   * **Console (`sensor` command)**:
     * `sensor`: Displays all detected sensors and their latest readings.
     * `sensor selftest`: Executes arithmetic verification on all drivers.
   * **Lisp Engine**: Exposes `(sensor-read 'temp)` and `(sensor-list)` directly to LugalOS Lisp.

---

## 6. Milestones
 
-### 46.1 I2C Register Helpers & Seam Refactoring
+### 46.1 I2C Register Helpers & Seam Refactoring [Concluded]
 * Implemented `drivers/include/drivers/i2c_reg.h` and `drivers/i2c_reg.c`.
 * `i2c_reg_read_u8`, `i2c_reg_write_u8`, `i2c_reg_read_u16_be/le`, `i2c_reg_read_bytes`, `i2c_reg_write_bytes`.
 * Refactored `drivers/bme280.c` to Layer 1 helpers.
 
-### 46.2 The Sensor Device Contract
+### 46.2 The Sensor Device Contract [Concluded]
 * Implemented `drivers/include/drivers/sensor.h` and `drivers/sensor.c`.
 * Defined `sensor_chan_t`, standard metric scaling, and `sensor_dev_t`.
 
-### 46.3 BME280 Refactor under `sensor_dev_t`
+### 46.3 BME280 Refactor under `sensor_dev_t` [Concluded]
 * Refactored `drivers/bme280.c` to implement `sensor_dev_t` interface.
 * All regression tests (`test_bme280_compensation`) pass unchanged.
 
-### 46.4 Sensor Hub & Standalone EMA Engine
+### 46.4 Sensor Hub & Standalone EMA Engine [Concluded]
 * Implemented `drivers/include/drivers/sensor_hub.h` and `drivers/sensor_hub.c`.
 * Centralized cache, staleness age tracking (`age_s`), autonomous sampler task, and EMA filter.
 * Connected `/proc/sensors` and shell `sensor` command.
 
-### 46.5 BME680 Driver (Pure Fixed-Point, Blob-Free)
+### 46.5 BME680 Driver (Pure Fixed-Point, Blob-Free) [Concluded]
 * Implemented `drivers/bme680.c` and `drivers/include/drivers/bme680.h`.
 * Forced-mode triggering, heater profile calculation (320 °C, 150 ms), integer compensation for $T$, $P$, $H$, and $R_{\text{gas}}$.
 * Created `tools/bme680_reference.py` independent verification script.
 * Silicon verification on physical Pimoroni BME680: 26.63 °C, 960.30 hPa, 52.11 %RH, 21.46 kOhm gas resistance.

### 46.6 Light Sensors: TSL2561 and TSL2591 [Concluded]
 * Implemented `drivers/tsl2561.c` and `drivers/tsl2591.c`.
 * Implemented gain/integration time control, CPL and piecewise integer Lux calculations.
 * Reference math scripts: `tools/tsl2561_reference.py`, `tools/tsl2591_reference.py`.
 * Silicon verification on physical hardware:
   - TSL2591 at 0x29 (on BlueDot dual breakout): 128.77 Lux, verified concurrently alongside BME280 at 0x77.
   - TSL2561 at 0x39: 106.61 Lux, verified standalone with DS3231 RTC.
 * Autonomous sampling, non-blocking cache, staleness age tracking (`age_s`), and `/proc/sensors` multi-sensor reporting.
 * All selftests pass with 0 failures on real silicon and in automated regression runner.

### 46.7 MOX Air Quality Sensor: CCS811 [Concluded]
* Implemented `drivers/ccs811.c` and `drivers/include/drivers/ccs811.h`.
* Active-low `/WAKE` support, `APP_START` (0xF4) bootloader state machine transition.
* Mode 1 continuous IAQ measurements: extracts eCO2 (ppm) and TVOC (ppb) from `ALG_RESULT_DATA` (0x02).
* Implemented cross-sensor environmental compensation: Sensor Hub routes ambient $T$ & $H$ from BME280/BME680 into CCS811 `ENV_DATA` (0x05) encoded in 1/512 %RH and 1/512 °C with -25°C offset.
* Reference math script: `tools/ccs811_reference.py`.
* Silicon verification on physical hardware: CCS811 at 0x5A running concurrently alongside BME280 at 0x76:
  - `ccs811 at 0x5a: 400 ppm eCO2, 0 ppb TVOC`
  - `bme280 at 0x76: 26.20 C, 959.13 hPa, 47.16 %RH`
  - `/proc/sensors`: reports station & sea-level pressure, temperature, humidity, eCO2, TVOC, and independent staleness ages.
* All 5 driver selftests (BME280, BME680, CCS811, TSL2561, TSL2591) pass with 0 failures.

### 46.8 Sensirion Multi-Pixel Gas Sensor: SGP30 [Concluded]
* Implemented `drivers/sgp30.c` and `drivers/include/drivers/sgp30.h`.
* 16-bit big-endian command protocol, Sensirion CRC-8 (polynomial 0x31, init 0xFF) validation.
* Pure integer Taylor expansion in Q16 for absolute humidity calculation `sgp30_calc_ah_8_8` formatted as fixed-point 8.8 $g/\text{m}^3$.
* Reference math script: `tools/sgp30_reference.py`.
* Silicon verification on physical hardware: SGP30 at 0x58 running concurrently alongside BME280 at 0x76:
  - `sgp30 at 0x58: 400 ppm eCO2, 0 ppb TVOC`
  - `bme280 at 0x76: 26.11 C, 958.66 hPa, 48.48 %RH`
  - `/proc/sensors`: unified reporting of temperature, pressure, humidity, eCO2, TVOC, and independent staleness ages.
* All 6 driver selftests (BME280, BME680, CCS811, SGP30, TSL2561, TSL2591) pass with 0 failures.

### 46.9 Cross-Sensor Compensation & Lisp Bindings [Concluded]
* Sensor Hub cross-compensation pipeline: live ambient $T$ & $H$ from BME280/BME680 routed into CCS811 `ENV_DATA` (0x05) and SGP30 `0x2061` absolute humidity registers.
* Added `(sensor-list)` primitive to Lisp: returns detected sensor symbols, e.g. `(sgp30 bme280)`.
* Added `(sensor-read [chan | dev] [chan | age | filtered])` primitive to Lisp:
  - `(sensor-read)`: returns an alist of all available channels, e.g. `((temp . 2633) (pressure . 95857) (humidity . 4792) (eco2 . 400) (tvoc . 0))`.
  - `(sensor-read 'temp)`: returns ambient temperature (centi-Celsius).
  - `(sensor-read 'eco2)` / `(sensor-read 'tvoc)`: returns gas metrics directly.
  - `(sensor-read 'temp 'age)`: returns metric age in seconds.
  - `(sensor-read 'temp 'filtered)`: returns EMA filtered value.
  - `(sensor-read 'bme280 'temp)`: reads specific device channel.
* Zero SRAM overhead: registered in `.rodata` `user/lisp/builtins_table.h` in exact ASCII sorted order for $O(\log N)$ `bsearch()`.
* Automated regression test in QEMU virt verifying safe `()` and `#f` returns when no sensors are fitted.

### 46.10 Verification, QEMU Regression & Hardware Soak [Concluded]
* Silicon verification on RP2350 and ESP32-P4 boards with connected sensors.
* 24-hour stability soak run on an `rp2350-sensor` node publishing all active channels over MQTT.
* All regression tests pass with 0 failures on QEMU virt and hardware runner.

### 46.11 Grove Multichannel Gas Sensor (MiCS-6814) [Concluded]
* Implemented `drivers/mics6814.c` and `drivers/include/drivers/mics6814.h`.
* Probed at I2C address `0x04` on the shared bus.
* Implemented 10-minute warm-up stabilization countdown (`MICS6814_WARMUP_PERIOD_S 600u`).
* Extracted calibrated concentrations for Carbon Monoxide (`co`), Nitrogen Dioxide (`no2`), and Ammonia (`nh3`) in centi-ppm ($0.01\ \text{ppm}$).
* Created `tools/mics6814_reference.py` golden vector selftest.
* Verified on real silicon: `mics6814 selftest: 0 cases failed`, reporting live 15.98 ppm CO, 0.11 ppm NO2, 3.11 ppm NH3.

### 46.12 Winsen MH-Z19B NDIR CO2 Sensor via UART1 [Concluded]
* Implemented `drivers/mhz19b.c` and `drivers/include/drivers/mhz19b.h`.
* Configured dedicated PL011 UART1 on GP8 (TX) / GP9 (RX) at 9600 baud 8N1 on RP2350.
* Moved external heartbeat LED to GP2 to prevent UART1 RX pin collision.
* Connected $V_{in}$ to Pin 40 (`VBUS`, 5V USB power) for optical lamp supply.
* Implemented packet checksum `~sum + 1` validation, 0x86 read command, and ABC calibration control.
* Implemented 3-minute optical preheat countdown (`MHZ19B_WARMUP_PERIOD_S 180u`).
* Extracted true physical $\text{CO}_2$ in 1 ppm and chamber temperature in $0.01\ ^\circ\text{C}$.
* Verified on real silicon: `mhz19b selftest: 0 cases failed`, reporting live 1443 ppm true $\text{CO}_2$.

### 46.13 Standardized Decimal Scaling & Derived Metrics Pipeline [Concluded]
* Standardized `sensor_chan_desc_t` metadata table across all drivers and export sinks.
* Implemented derived Mean Sea Level Pressure ($P_{\text{msl}}$ / QNH) via fixed-point hypsometric formula using station altitude from `idstore`.
* Implemented Absolute Humidity ($AH$ in $g/\text{m}^3$) and Dew Point ($T_{\text{dew}}$ in centi-°C).
* Exposed derived channels to `/proc/sensors`, MQTT, and Lisp (`(sensor-read 'dew-point)`, `(sensor-read 'abs-humidity)`).
* Verified on real silicon: altitude 520m, station pressure 949.08 hPa -> QNH 1006.45 hPa, dew point 17.25 °C, absolute humidity 13.85 g/m³.

### 46.14 Blob-Free Open-Source BME680 IAQ & VOC Index Engine [Concluded]
* Implemented zero-float open-source IAQ index calculation from BME680 raw gas resistance $R_{\text{gas}}$, temperature, and humidity.
* Integrated humidity cross-sensitivity compensation ($40\ \%\text{RH}$ reference).
* Implemented adaptive sliding clean-air baseline tracking ($R_{\text{base}}$) with slow downward decay.
* Exposed standardized relative IAQ (0–500) to the sensor hub, `/proc/sensors`, and Lisp.
* Verified on real silicon: IAQ index dynamically reported and adapted (25 clean baseline, rising under VOC presence).

### 46.15 Multi-Sensor Fusion & Ground-Truth Cross-Calibration Model [Concluded]
* Implemented device-qualified channel addressing (`<dev>.<chan>`) alongside fused composite channels (`(sensor-read 'mhz19b 'co2)`).
* Implemented NDIR ground-truth calibration engine using MH-Z19B:
  - Solvent / VOC contamination detector: flags MOX $\text{eCO}_2$ when diverging > 1.8x from true NDIR $\text{CO}_2$ (`[WARN] MOX contaminated by VOC/solvents`).
  - Automated clean-air baseline anchoring for SGP30 and CCS811 when MH-Z19B reads outdoor baseline levels ($400 - 430\ \text{ppm}$).
* Verified on real silicon: MH-Z19B measured true 1011 ppm CO2 while CCS811 spiked to 6597 ppm eCO2 (ratio 652%), correctly flagged and disambiguated.

### 46.16 Dual-Tier Calibration Persistence Architecture (Flash vs EEPROM) [Concluded]
* Implemented dual-tier calibration storage model:
  - Tier 1: Fast, high-endurance (> 1M cycles) AT24C32 I2C EEPROM intermediate storage at `0x57`, offset `0x0F00`. Hourly and fresh-air checkpointing without NOR flash wear.
  - Tier 2: Permanent Flash `idstore` (`IDSTORE_FIELD_SENSOR_CAL`).
* Boot resolution: `sensor_hub_init()` checks EEPROM first, then Flash.
* Shell commands: `sensor cal [show | save | restore | clear | eeprom save | eeprom restore | eeprom clear]`.
* Lisp primitives: `(sensor-cal ['save | 'restore | 'clear | 'save-eeprom | 'restore-eeprom | 'clear-eeprom])`.
* Verified on real silicon: active baselines round-tripped and persisted across reboots on both AT24C32 EEPROM and Flash `idstore`.

### 46.17 TI HDC1080 Temp/Humidity Sensor & Per-Sensor Baseline Management [Concluded]
* Supported combined CCS811 + TI HDC1080 daughterboard on shared I2C bus:
  - HDC1080 probed at fixed I2C address `0x40`, verified via manufacturer ID `0x5449` ('TI') and device ID `0x1050`.
  - Implemented `drivers/hdc1080.c` and `drivers/include/drivers/hdc1080.h`.
  - Sequential 14-bit Temperature ($T_{\text{c100}} = \frac{\text{raw} \times 16500}{65536} - 4000$) and Relative Humidity ($H_{\text{rh1000}} = \frac{\text{raw} \times 100000}{65536}$) acquisition with strict zero-float integer math.
  - Implemented `hdc1080_selftest()` verifying arithmetic against TI datasheet golden vectors.
  - Exposed HDC1080 to `/proc/sensors`, `sensor` status report, and Lisp primitives `(sensor-read 'hdc1080 'temp)`, `(sensor-read 'hdc1080 'humidity)`.
* Implemented Per-Sensor Calibration Reset Protocol:
  - Added `sensor_hub_cal_clear_dev(const char *dev_name)` to selectively clear an individual sensor's baseline without wiping other devices' stored calibrations across Flash `idstore` and AT24C32 EEPROM.
  - Added `ccs811_reset()` executing the 4-byte software reset sequence (`0x11, 0xE5, 0x72, 0x8A`) to register `0xFF`, putting replaced sensors back into clean burn-in state.
  - Added shell commands: `sensor cal clear [sensor]`, `sensor cal reset [sensor]`.
  - Added Lisp primitives: `(sensor-cal 'clear '[sensor])`, `(sensor-cal 'reset '[sensor])`.
* Verified on real silicon:
  - Replaced CCS811 baseline cleared via `sensor cal reset ccs811`.
  - New CCS811 reads clean ambient baseline: $400\ \text{ppm}\ \text{eCO}_2$, $0\ \text{ppb}\ \text{TVOC}$, with $\text{MOX ratio} = 51\%$ against MH-Z19B true $\text{CO}_2$, completely clearing previous solvent contamination warnings.
  - HDC1080 actively sampled: reporting $30.98\ ^\circ\text{C}$, $47.78\ \%\text{RH}$ alongside BME680.

---

## 7. Multi-Sensor Fusion, Cross-Compensation, and Calibration Architecture

As the measurement suite expands to multiple concurrent sensors measuring overlapping physical and derived phenomena (e.g. SGP30 vs CCS811 for eCO2/TVOC, BME280 vs BME680 for T/P/RH, MH-Z19B true CO2 vs MOX eCO2), the Sensor Hub must transition from a passive multiplexer to an active **Sensor Fusion and Calibration Pipeline**.

### 7.1 Namespace & Source Attribution Disambiguation

Every metric in the hub possesses two identities:
1. **Device-Qualified Channel (`<device>.<channel>`)**:
   - Accesses the raw, un-adulterated measurement of a specific physical sensor instance.
   - Examples: `sgp30.eco2`, `ccs811.eco2`, `mhz19b.co2`, `bme680.temp`, `mhz19b.temp` (optical chamber temperature).
   - In Lisp: `(sensor-read 'sgp30 'eco2)`, `(sensor-read 'mhz19b 'co2)`.
   - In MQTT: `<node>/sensor/sgp30/eco2`, `<node>/sensor/mhz19b/co2`.
   - In `/proc/sensors`: individual sections or explicit device-prefixed keys (`sgp30_eco2_ppm`, `mhz19b_co2_ppm`).

2. **Fused Composite Channel (`<channel>`)**:
   - High-level unified view exposed to generic consumers (`(sensor-read 'co2)`, `/proc/sensors` primary lines, `<node>/sensor/co2`).
   - Evaluated by the Sensor Hub's **Fusion Arbiter** based on sensor class, priority, and validity:
     * **Carbon Dioxide (`co2`)**:
       - Priority 1: MH-Z19B (true physical NDIR optical absorption, confidence 1.0).
       - Fallback: SGP30 / CCS811 `eco2` (flagged with derived confidence attribute).
     * **Equivalent $\text{CO}_2$ (`eco2`)**:
       - SGP30 and CCS811 weighted consensus when both are within tolerance ($|\Delta| < 20\%$). If contaminated by solvent spike (detected via true NDIR $CO_2$ divergence), flagged as invalid.
     * **Ambient Temperature (`temp`)**:
       - Average of ambient meteorological sensors (`bme680`, `bme280`). Internal device chamber temperatures (like `mhz19b` incandescent optical block) are strictly excluded from ambient fusion.
     * **Barometric Pressure (`pressure`)**:
       - Primary meteorological barometric sensor (`bme680` or `bme280`).
     * **Illuminance (`lux`)**:
       - TSL2591 (high dynamic range) prioritized over TSL2561.

### 7.2 Units, Decimal Scaling, and Zero-Float Formatting

In accordance with LugalOS's strict `mstatus.FS = 0` (zero floating-point) architectural invariant, all kernel and driver calculations occur in exact integer arithmetic. To eliminate discrepancies across output sinks (shell, `/proc`, MQTT, Lisp), every channel is bound to a standardized `sensor_chan_desc_t`:

```c
typedef struct {
    sensor_chan_t chan;
    const char   *name;        /* Machine key: "temp", "pressure", "humidity", "co2" */
    const char   *unit_symbol; /* Display unit: "°C", "hPa", "%RH", "ppm", "ppb", "Ohm" */
    uint8_t       decimals;    /* Decimal places for display: 2 for 0.01, 0 for 1 */
    int32_t       scale_div;   /* Divider from raw int32_t to standard units */
} sensor_chan_desc_t;
```

* **Barometric Pressure**:
  - Raw internal representation: **Pascals** ($1\ \text{Pa} = 0.01\ \text{hPa}$).
  - Machine export (`/proc/sensors`, Lisp): `94886 Pa`.
  - Human display / MQTT: `948.86 hPa` (scaled by $100$, 2 decimal places).
* **Temperature**:
  - Raw internal: **centi-degrees** ($0.01\ ^\circ\text{C}$). E.g., `2731` = $27.31\ ^\circ\text{C}$.
* **Relative Humidity**:
  - Raw internal: **centi-%RH** ($0.01\ \%\text{RH}$). E.g., `5210` = $52.10\ \%\text{RH}$.
* **Concentrations**:
  - True $\text{CO}_2$, $\text{eCO}_2$: integer $\text{ppm}$ ($1\ \text{ppm}$).
  - $\text{TVOC}$: integer $\text{ppb}$ ($1\ \text{ppb}$).
  - Multichannel gases ($\text{CO}, \text{NO}_2, \text{NH}_3$): centi-ppm ($0.01\ \text{ppm}$). E.g., `1598` = $15.98\ \text{ppm}$.
* **Resistance**:
  - Raw internal: integer $\Omega$ ($1\ \Omega$). Human display: $\Omega$ or $\text{k}\Omega$.

### 7.3 Derived Metrics Pipeline

The Sensor Hub provides a deterministic, zero-float mathematical pipeline for derived physical and meteorological metrics:

1. **Mean Sea Level Pressure ($P_{\text{msl}}$ / QNH)**:
   - Uses the barometric formula based on the barometric lapse rate:
     $$P_{\text{msl}} = P_{\text{station}} \times \left(1 - \frac{L \cdot h}{T_0}\right)^{-\frac{g \cdot M}{R_0 \cdot L}} \approx P_{\text{station}} \times \left(1 + \frac{h}{44330.77 \cdot (1 - (P/P_0)^{0.190284})}\right)$$
   - Evaluated in pure fixed-point polynomial arithmetic using the station elevation $h$ provisioned in the node's identity record (`idstore`).
   - In clean air: $P_{\text{msl}} \approx P_{\text{station}} \times \left(1 + \frac{h}{8430}\right)$ for modest altitudes.
2. **Absolute Humidity ($AH$ in $g/\text{m}^3$)**:
   - Calculated via the Magnus-Tetens formula for saturation vapor pressure $e_s(T)$ and ideal gas law:
     $$AH = 216.7 \times \frac{\frac{RH}{100} \cdot e_s(T)}{T + 273.15}$$
   - Executed via fixed-point Taylor expansion (already proven in `sgp30_calc_ah_8_8`).
   - Automatically injected into SGP30 (`0x2061`) and CCS811 (`ENV_DATA`) for real-time MOX moisture compensation.
3. **Dew Point ($T_{\text{dew}}$ in centi-°C)**:
   - Pure integer approximation: $T_{\text{dew}} \approx T - \frac{100 - RH}{5}$.
4. **Vapor Pressure Deficit ($VPD$ in Pa)**:
   - Useful for indoor environmental quality and plant growth monitoring.

### 7.4 Open-Source, Blob-Free BME680 IAQ & VOC Index Engine

#### Research & The "No-Blob" Challenge
Bosch Sensortec requires their proprietary closed-source binary library (`BSEC`) to calculate an "Index of Air Quality" (IAQ 0–500), equivalent $\text{CO}_2$, and breath VOC equivalent. LugalOS strictly rejects binary blobs:
1. They violate freestanding kernel execution (`-nostdlib -ffreestanding`).
2. They are tied to specific toolchain ABIs and cannot run under LugalOS's `mstatus.FS = 0` trap invariant.
3. They are opaque black boxes that prevent formal verification.

#### The Physical Principle of the BME680 MOX Heater
The BME680 features a micro-machined hot plate heated to $320\ ^\circ\text{C}$ for $150\ \text{ms}$. Oxygen ions adsorb onto the tin dioxide ($SnO_2$) metal-oxide semiconductor surface. In clean air, electrons are trapped by adsorbed oxygen, yielding a high baseline electrical resistance ($R_{\text{gas\_base}} \sim 50\ \text{k}\Omega - 200\ \text{k}\Omega$). When volatile organic compounds (reducing gases such as ethanol, acetone, toluene) strike the heated plate, they react with oxygen ions, releasing electrons back into the conduction band and causing electrical resistance $R_{\text{gas}}$ to drop sharply ($5\ \text{k}\Omega - 20\ \text{k}\Omega$).

#### Open-Source Compensation & Index Architecture
Drawing from open-source research (including Sensirion's BSD-3-licensed Gas Index Algorithm and community BME680 models):
1. **Humidity Cross-Sensitivity Compensation**:
   Water molecules compete with VOCs on the heated plate. High relative humidity lowers $R_{\text{gas}}$ even in completely clean air.
   We normalize raw $R_{\text{gas}}$ to reference humidity ($40\ \%\text{RH}$):
   $$R_{\text{comp}} = R_{\text{gas}} \times \left(1 + \alpha_{\text{hum}} \cdot (RH - 40\%)\right)$$
   where $\alpha_{\text{hum}} \approx 0.002$ per centi-percent RH, computed in integer arithmetic.
2. **Adaptive Baseline Tracking ($R_{\text{base}}$)**:
   A sliding exponential maximum filter tracks clean-air events over a 24- to 72-hour window:
   - When $R_{\text{comp}} > R_{\text{base}}$, $R_{\text{base}}$ rapidly ascends toward the clean-air reading.
   - When air is polluted ($R_{\text{comp}} < R_{\text{base}}$), $R_{\text{base}}$ decays at an extremely slow time constant ($\tau \sim 48\ \text{h}$) to prevent baseline poisoning from short-term indoor VOC spikes.
3. **Relative Air Quality Score (0–500 IAQ)**:
   $$\text{Score}_{\text{gas}} = \text{clamp}_{0..500}\left( 500 - \frac{R_{\text{comp}}}{R_{\text{base}}} \times 500 \right)$$
   When air is pristine ($R_{\text{comp}} \approx R_{\text{base}}$), IAQ is $25 - 50$ (clean). When heavily polluted ($R_{\text{comp}} \ll R_{\text{base}}$), IAQ approaches $400 - 500$.

### 7.5 Ground-Truth Calibration: NDIR $\text{CO}_2$ vs MOX $\text{eCO}_2$

#### Why MOX $\text{eCO}_2$ Generates Unrealistic Values
Users frequently observe that $\text{eCO}_2$ sensors (CCS811, SGP30) report wild, unrealistic values (e.g. spiking to $65,000\ \text{ppm}$ or fluctuating erratically). The physical reason is straightforward:
* **$\text{CO}_2$ is chemically inert**: Carbon dioxide does not react catalytically on heated $SnO_2$ metal-oxide surfaces. **MOX sensors cannot detect $\text{CO}_2$ directly.**
* **The "Equivalent" Proxy Assumption**: Sensor manufacturers assume that in human-occupied rooms, human breath contains a fixed ratio of breath hydrogen ($H_2 \sim 10\ \text{ppm}$) and metabolic VOCs proportional to exhaled $\text{CO}_2$. Their internal firmware multiplies detected $H_2$/VOC signals by a fixed scalar to estimate $\text{eCO}_2$.
* **The Real-World Failure Mode**: Whenever alcohol, hand sanitizer, cleaning spray, fruit, perfume, or cooking gases are present, the MOX sensor detects massive amounts of reducing ethanol/VOC molecules. Unable to distinguish breath hydrogen from cleaning alcohol, the sensor incorrectly assumes thousands of people are exhaling in the room and reports $e\text{CO}_2 = 65,000\ \text{ppm}$.

#### Ground-Truth Calibration Model with MH-Z19B
By integrating the **Winsen MH-Z19B NDIR CO₂ sensor** on the same bench, LugalOS acquires true physical ground truth:
1. **Contamination Disambiguation (False-Positive Gate)**:
   - The Hub continuously monitors the ratio:
     $$r_{\text{valid}} = \frac{\text{eCO}_2}{\text{CO}_2^{\text{NDIR}}}$$
   - **Metabolic Respiration**: When room $\text{CO}_2$ rises due to human occupancy, both $\text{CO}_2^{\text{NDIR}}$ and $\text{eCO}_2$ rise in tandem ($0.8 \le r_{\text{valid}} \le 1.5$). $\text{eCO}_2$ is accepted as valid.
   - **Solvent / VOC Contamination**: When $\text{eCO}_2 \gg \text{CO}_2^{\text{NDIR}}$ (e.g. $r_{\text{valid}} > 2.0$), the Hub flags the MOX sensor as **chemically contaminated**. The $\text{eCO}_2$ value is suppressed or clamped, and the signal is correctly re-attributed to an elevated **$\text{TVOC}$** event.
2. **Dynamic Outdoor Baseline Calibration**:
   - Both SGP30 and CCS811 rely on automatic baseline tracking that can drift if the room is never ventilated.
   - When the MH-Z19B detects that physical $\text{CO}_2$ has dropped to ambient background ($400 - 430\ \text{ppm}$) and TVOC is low, the Hub confirms that genuine fresh outdoor air is present.
   - The Hub uses this verified fresh-air window to commit and anchor the baseline registers of the SGP30 and CCS811.
3. **Empirical Sensitivity Scaling**:
   - In clean respiration conditions, the Hub fits an empirical linear regression:
     $$\Delta \text{CO}_2 = k_{\text{mox}} \cdot \Delta S_{\text{mox}}$$
   - This tunes the individual sensor's aging curve against the physical optical standard.

### 7.6 Dual-Tier Calibration Persistence Architecture (Flash vs EEPROM)

Metal-oxide gas sensors (SGP30, CCS811, MiCS-6814, BME680) maintain running baseline resistances that drift over time and require periodic checkpointing to avoid amnesia across power cycles. However, standard microcontroller NOR Flash requires erasing an entire 4 KB sector per write and has limited endurance (~100,000 cycles). High-frequency writing to Flash would rapidly degrade silicon.

To address this, LugalOS implements a **dual-tier storage model** utilizing the connected RTC clock module (which features a DS3231 RTC paired with an AT24C32 4 KB I2C EEPROM at `0x57` on the shared I2C bus):

1. **Tier 1: AT24C32 EEPROM Fast Intermediate Storage (`0x57`, offset `0x0F00`)**:
   - **Characteristics**: Byte/page addressable, > 1,000,000 write cycle endurance, zero block-erase overhead.
   - **Role**: High-frequency running baseline checkpoints. Automatically checkpointed hourly and immediately whenever fresh outdoor air is confirmed ($r_{\text{valid}}$ verified against MH-Z19B NDIR standard).
   - **Lifespan**: Writing once per hour provides over 110 years of continuous operational lifespan.
2. **Tier 2: NOR Flash Identity Store (`IDSTORE_FIELD_SENSOR_CAL = 10`)**:
   - **Characteristics**: In-flash wear-leveled identity record storage, survives external EEPROM removal.
   - **Role**: Long-term anchor baselines committed upon explicit user action (`sensor cal save` or `(sensor-cal 'save')`).
3. **Boot Resolution Logic**:
   - On boot, `sensor_hub_init()` first queries the AT24C32 EEPROM for the most recent valid calibration blob (`sensor_cal_blob_t`).
   - If EEPROM is absent or uncalibrated, it falls back to the Flash identity store.
   - Restored baselines are immediately applied to SGP30, CCS811, MiCS-6814, and BME680 before initial measurement cycles begin.
4. **Shell & Lisp Interface**:
   - Shell:
     - `sensor cal` — displays active baselines and status in both Flash and EEPROM.
     - `sensor cal save` / `restore` / `clear` — manages persistent Flash anchor (and syncs EEPROM).
     - `sensor cal eeprom save` / `restore` / `clear` — directly manages fast EEPROM intermediate storage.
   - Lisp:
     - `(sensor-cal)` — returns an alist of active baselines and persistent flags (`saved?`, `eeprom-saved?`, `fresh-air?`).
     - `(sensor-cal 'save-eeprom)` / `(sensor-cal 'restore-eeprom)` / `(sensor-cal 'clear-eeprom)`.

### 7.7 Hierarchical Multi-Sensor State Presentation (Milestone 46.18)

When multiple environmental sensors are deployed simultaneously (e.g. BME680 + HDC1080 for temperature/humidity, MH-Z19B + CCS811 + SGP30 for carbon dioxide, TSL2561/TSL2591 for ambient light), flat metric presentation introduces ambiguity regarding value origin, calibration source, and physical vs inferred transducers.

Milestone 46.18 implements an explicit three-tier presentation hierarchy across all subsystem interfaces:
1. **Tier 1: Direct Physical Hardware (`hw`)** — Raw transducer measurements tied to a specific silicon device address.
2. **Tier 2: Inferred Mathematical Models (`inferred`)** — Values synthesized through deterministic physics formulas (Magnus dew point, August-Roche-Magnus absolute humidity, hypsometric MSL barometric pressure, BME680 IAQ heuristic).
3. **Tier 3: Fused Consensus & Arbitration (`fused`)** — Top-level system arbitration prioritizing optical ground truth (e.g. MH-Z19B NDIR over MOX eCO2) and primary environmental sensors.

#### 1. MQTT Hierarchical Topics
Topics published under `lugalos/<node>/` now explicitly identify their origin:
* **Hardware Devices**: `lugalos/<node>/sensor/<device>/<metric>`
  * `sensor/bme680/temperature`, `sensor/bme680/pressure`, `sensor/bme680/humidity`, `sensor/bme680/gas_resistance`
  * `sensor/hdc1080/temperature`, `sensor/hdc1080/humidity`
  * `sensor/ccs811/eco2`, `sensor/ccs811/tvoc`
  * `sensor/sgp30/eco2`, `sensor/sgp30/tvoc`
  * `sensor/mhz19b/co2`
  * `sensor/mics6814/co`, `sensor/mics6814/no2`, `sensor/mics6814/nh3`
  * `sensor/tsl2591/lux`, `sensor/tsl2561/lux`
* **Inferred Models**: `lugalos/<node>/sensor/inferred/<metric>`
  * `sensor/inferred/pressure_msl`
  * `sensor/inferred/abs_humidity`
  * `sensor/inferred/dew_point`
  * `sensor/inferred/iaq`
* **Fused Consensus**: `lugalos/<node>/sensor/fused/<metric>`
  * `sensor/fused/co2` (MH-Z19B optical NDIR priority, MOX eCO2 fallback)
  * `sensor/fused/temperature`, `sensor/fused/humidity`, `sensor/fused/pressure`
  * `sensor/fused/eco2`, `sensor/fused/tvoc`, `sensor/fused/lux`

#### 2. Lisp Dialect Hierarchy & Provenance Inspection
* `(sensor-read)` (0 args):
  Returns a structured 3-tier nested tree:
  ```lisp
  ((hw (bme680 (temp . 2450) (pressure . 95800) (humidity . 4520) (gas-res . 34000))
       (hdc1080 (temp . 2410) (humidity . 4600))
       (ccs811 (eco2 . 450) (tvoc . 15))
       (mics6814 (co . 120) (no2 . 15) (nh3 . 30))
       (mhz19b (co2 . 420)))
   (inferred (pressure-msl . 101325)
             (dew-point . 1230)
             (abs-humidity . 980)
             (iaq . 55))
   (fused (co2 . 420)
          (temp . 2450)
          (pressure . 95800)
          (humidity . 4520)
          (eco2 . 450)
          (tvoc . 15)))
  ```
* Tier Subtree Queries:
  * `(sensor-read 'hw)` — returns the hardware device tree.
  * `(sensor-read 'inferred)` — returns the derived metrics alist.
  * `(sensor-read 'fused)` — returns the arbitrated consensus alist.
* Device Queries:
  * `(sensor-read '<dev>)` — returns all channels for that specific device (e.g. `(sensor-read 'hdc1080)` => `((temp . 2410) (humidity . 4600))`).
  * `(sensor-read '<dev> '<chan> ['age])` — returns the numeric value (or sample age) from that device.
* Channel Provenance Inspection:
  * `(sensor-origin '<chan>)` — returns metadata describing origin tier, source device, mathematical model, and physical principle:
    ```lisp
    (sensor-origin 'co2)
    ;; => ((tier . fused) (source . mhz19b) (model . ndir-optical) (desc . "NDIR infrared optical absorption"))

    (sensor-origin 'dew-point)
    ;; => ((tier . inferred) (source . bme680) (model . magnus-tetens) (desc . "dew point temperature"))

    (sensor-origin 'temp)
    ;; => ((tier . fused) (source . bme680) (model . hardware-transducer) (desc . "bme680"))
    ```

#### 3. VFS `/proc` Subdirectory Hierarchy
* `/proc/sensors`: Maintained as a 100% backward-compatible composite flat key=value file, preventing any breakage of legacy consumers, remote 9P gateways, or `cat /proc/sensors` parsers.
* `/proc/sensor/` virtual directory:
  * Listed in `ls /proc` as `<DIR>`.
  * `ls /proc/sensor` lists `fused`, `inferred`, and all actively detected device names (`bme680`, `hdc1080`, `ccs811`, `mics6814`, `mhz19b`, etc.).
  * `/proc/sensor/fused`: Contains the current arbitrated readings (`co2_ppm`, `temperature_c100`, `humidity_rh1000`, `pressure_pa`, etc.).
  * `/proc/sensor/inferred`: Contains mathematical models (`pressure_msl_pa`, `altitude_m`, `dew_point_c100`, `abs_humidity_c100`, `iaq`, `mox_contaminated`, `fresh_air_verified`).
  * `/proc/sensor/<device>`: Contains device-specific identity, address, sample period, validity, and raw transducer measurements.
