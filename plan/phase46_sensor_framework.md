# Phase 46 — A Unified I2C Sensor Framework

**Status: active. Milestones 46.1, 46.2, 46.3, 46.4, 46.5 concluded with silicon verification. Written 2026-10-06.**
Succeeds `plan/phase26_mqtt_and_environment_sensors.md` (concluded) and stands beside `plan/phase45_esp32c6.md` (planned).

**Milestone scheme: `46.1`, `46.2`, `46.3`, …**

---

## 0. Motivation & The Third Implementation Rule

LugalOS's core driver design rule ([`drivers/README.md`](file:///home/dsc/Source/gith/domschl/lugalos/drivers/README.md#L10-L26)) mandates:
> *"Extract at the third implementation, never at the second. Two copies is evidence that something recurs. Three is a design."*

Until now, LugalOS supported exactly one environmental sensor: the BMP280 / BME280 ([`drivers/bme280.c`](file:///home/dsc/Source/gith/domschl/lugalos/drivers/bme280.c)). That driver was deliberately kept ad-hoc: it managed its own I2C transactions, maintained its own internal cache, launched its own sampler task, directly fed `/proc/sensors` in [`fs/vfs_server.c`](file:///home/dsc/Source/gith/domschl/lugalos/fs/vfs_server.c#L1290), and was hard-wired into the shell's `sensor` command ([`kernel/shell.c`](file:///home/dsc/Source/gith/domschl/lugalos/kernel/shell.c#L1260)) and the MQTT daemon ([`net/include/net/mqttd.h`](file:///home/dsc/Source/gith/domschl/lugalos/net/include/net/mqttd.h)).

We now expand the environmental measurement suite to six sensors:
1. **BME280** (Baseline: Temperature, Pressure, Humidity)
2. **BME680** (Temperature, Pressure, Humidity, MOX Gas Resistance — **strictly without proprietary binary blobs**)
3. **TSL2561** (Dual-channel Ambient Light / Lux)
4. **TSL2591** (High Dynamic Range Light / Lux)
5. **CCS811** (MOX Air Quality: eCO2 and TVOC)
6. **SGP30** (Multi-pixel Gas Sensor: eCO2, TVOC, raw signals)

Going from 1 sensor to 6 sensors crosses the threshold from a single driver to a formal **Category D (Device-class contract)**. Doing so without a structured framework would replicate register-pumping loops, sampling tasks, staleness timers, EMA filters, and `/proc` formats six times across the codebase.

### Official Datasheets Reference
Component datasheets for all six parts are stored in the host repository tree under:
`~/gith/sensors/` or `~/Source/gith/sensors/`
* BME280: `bst-bme280-ds002.pdf`
* BME680: `bst-bme680-ds001.pdf`
* CCS811: `CCS811_Datasheet-DS000459.pdf`
* SGP30: `Sensirion_Gas_Sensors_Datasheet_SGP30.pdf`
* TSL2561: `TSL2561.pdf`
* TSL2591: `TSL25911_Datasheet_EN_v1.pdf`


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

### 46.6 Light Sensors: TSL2561 and TSL2591
* Implement `drivers/tsl2561.c` and `drivers/tsl2591.c`.
* Implement gain/integration time control and piecewise integer Lux calculations.

### 46.7 MOX Air Quality Sensor: CCS811
* Implement `drivers/ccs811.c`: active-low `/WAKE` handling, `APP_START` boot transition, Mode 3 pulse measurement, eCO2/TVOC registers.

### 46.8 Sensirion Multi-Pixel Gas Sensor: SGP30
* Implement `drivers/sgp30.c`: 16-bit commands, CRC-8 validation, eCO2 and TVOC extraction.

### 46.9 Cross-Sensor Compensation & Lisp Bindings
* Enable Sensor Hub cross-compensation ($T$ and $H$ feed into CCS811 `ENV_DATA` and SGP30 `0x2061`).
* Expose `sensor-read` and `sensor-list` primitives to the Lisp environment.

### 46.10 Verification, QEMU Regression & Hardware Soak
* Silicon verification on RP2350 and ESP32-P4 boards with connected sensors.
* 24-hour stability soak run on an `rp2350-sensor` node publishing all active channels over MQTT.
