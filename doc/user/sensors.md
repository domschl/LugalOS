# LugalOS Environmental Sensors & MQTT Telemetry

[Documentation](../README.md) > [User Manual](index.md) > **Environmental Sensors & MQTT Telemetry**

---

LugalOS features a comprehensive sensor subsystem capable of running autonomous environmental monitoring nodes. A node reads physical environmental metrics over I2C, applies filtering and publish rules, and transmits telemetry to standard MQTT 3.1.1 brokers over Wi-Fi or Ethernet.

---

## 1. Supported Sensors & I2C Bus

LugalOS includes built-in drivers for a wide range of I2C environmental and atmospheric sensors:
* **Temperature & Humidity**: Bosch BME280, BMP280, Texas Instruments HDC1080, TMP117, Microchip MCP9808.
* **Air Quality & Gas**: Bosch BME680 (VOC), Sensirion SGP30 (eCO2/TVOC), ScioSense CCS811, Winsen MH-Z19B (CO2), Sensirion SCD4x (SCD40/41/43, photoacoustic CO2 at 0x62; the hub prefers it over eCO2, takes its pressure compensation from the pressure sensor, and keeps its own temperature and humidity out of the fused channels).
* **Magnetic Field**: PNI RM3100 (I2C 0x20-0x23, the hub's first choice: no offset to drift) and MEMSIC MMC5983MA (0x30; SET/RESET offset removal on every sample), both for watching the earth's field; Infineon TLV493D-A1B6 (+/-130 mT, 0x5E; sleeps in power-down between samples). Channels `mag_x/y/z` in nT, not published over MQTT. The RM3100 and MMC5983 are sampled every second, each a mean of 8 measurements (RM3100, 400 cycle counts, 6.7 nT per LSB) or 4 SET/RESET pairs (MMC5983, 6.1 nT per LSB); with several present `sensor` shows each under its own name and the channel itself comes from the RM3100. Keep them away from steel, the USB cable and anything that draws changing current: a field of a few uT from a wire is a thousand times a storm. An RM3100 address 0x20-0x23 is also a PCF8574's, so the probe writes to one there.
* **Ambient Light**: AMS/TAOS TSL2561, TSL2591.

### Discovering Connected Sensors
`i2c scan` lists every address that answers on each bus, then the parts soldered to the board on that bus:
```text
lsh> i2c scan

I2C Bus 0 Scan (GPIO7 SDA / GPIO8 SCL):
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
10: -- -- -- -- -- -- -- -- 18 -- -- -- -- -- -- --
...
70: -- -- -- -- -- -- 76 --
Found 4 I2C device(s) on bus 0.
Board parts: 18 es8311
```
Sensors are searched for at their usual addresses, but never at a board part's address. Some parts share an address with a sensor: the ES8311 codec sits at an MCP9808 address (0x18), the ES7210 microphone ADC at the HDC1080's (0x40), and the Tab5's BMI270 at the DS3231's (0x68). Probing them would write their registers, so a sensor with the same address cannot be used on that bus.

**M5Stack Tab5:** connect external sensors to **Grove port A** (bus 1, GPIO53 SDA / GPIO54 SCL), not to the internal bus. The sensor hub searches both buses. The port's 5 V supply is switched on at boot. Sensors are searched for at boot; one plugged in later is found by `sensor init`.

### Board Sensor Wiring

| Board / Preset | Bus | Pins | Notes |
| :--- | :--- | :--- | :--- |
| **RP2350 (`rp2350-sensor`, `rp2350-wifi`, `rp2350-chess`, `rp2350-gateway`)** | I2C0 | `GP4` (SDA, pin 6), `GP5` (SCL, pin 7) | `3V3` (pin 36), `GND` (pin 38); internal pull-ups enabled |
| **RP2350 (`rp2350-sensor` MH-Z19B CO2)** | UART1 | `GP8` (TX), `GP9` (RX) | 9600 baud (cross TX/RX to sensor) |
| **RP2350-LCD-7 (`rp2350-terminal`) / Clock (`rp2350-clock`)** | I2C1 | `GP6` (SDA), `GP7` (SCL) | Shared with touch / RTC |
| **ESP32-C6-Zero (`esp32c6`)** | I2C0 | `GPIO0` (SDA), `GPIO1` (SCL) | 100 kHz |
| **M5Stack Tab5 (`esp32p4-tab5`)** | I2C1 (Grove A) | `GPIO53` (SDA), `GPIO54` (SCL) | 5 V supplied on Grove A; avoid `I2C0` internal bus |
| **ESP32-P4-LCD-7B / NANO (`esp32p4-lcd7b`, `esp32p4`)** | I2C0 | `GPIO7` (SDA), `GPIO8` (SCL) | Avoid addresses of soldered board parts (see `i2c scan`) |

For complete board pinouts, see the [RP2350 Guide](../admin/boards/rp2350.md#7-pinouts--hardware-wiring) and [ESP32-C6 Guide](../admin/boards/esp32c6.md#7-pinouts--external-wiring).

### Sampling Period
`sensor period N` samples the environmental sensors every N seconds, from the next round on; `sensor period` alone shows the current period. It lasts until the next boot.

`sensor lux` takes a light reading now (TSL2591, else TSL2561) instead of showing the last sample.

### Power and Motion Channels
Board parts report through the same channels as the environmental sensors: in `sensor`, in `/proc/sensor/<part>`, and (except motion) on MQTT as `sensor/<part>/<channel>`:

| Channel | Unit | `/proc/sensor` key | Source |
| :--- | :--- | :--- | :--- |
| `voltage` | V | `voltage_mv` | INA226 bus voltage |
| `current` | A, + while charging | `current_ma` | INA226 shunt |
| `power` | W | `power_mw` | voltage x current |
| `battery` | % | `battery_pm` (0.1 %) | charge estimate (`power`) |
| `accel_x/y/z` | g | `accel_x_mg` ... | IMU (not published) |
| `gyro_x/y/z` | deg/s | `gyro_x_ddps` ... | IMU (not published) |

A board part may be sampled on its own period. The INA226 and the magnetometers are read once a second, environmental sensors every 60 s. An INA226 module on a Grove or header bus is found too, and is taken to have the usual 0.1 Ohm shunt.

### Magnetometers: Earth's Field and Space Weather
Three 3-axis magnetometers are supported. Together they can watch the slow and fast changes of the earth's field, which a geomagnetic storm disturbs:

| Sensor | Address | Resolution | Noise (datasheet) | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **PNI RM3100** | `0x20`-`0x23` | 6.7 nT/LSB (400 cycle counts) | 15 nT at 200 cycle counts | Magneto-inductive: no offset to drift. The hub's first choice. Reads at most +/-800 uT. |
| **MEMSIC MMC5983MA** | `0x30` | 6.1 nT/LSB (18 bit) | 0.4 mG = 40 nT RMS | AMR bridge; every sample is SET/RESET pairs that cancel the offset and its temperature drift. Also reports a die temperature (diagnostic only, +/-3 degC). +/-8 G. |
| **Infineon TLV493D-A1B6** | `0x5E` | 98 uT/LSB | | Wide range (+/-130 mT) for magnets, far too coarse for the earth's field. |

All three use the channels `mag_x`, `mag_y` and `mag_z`, in **nT** (the earth's field of 48 uT reads about `48000`), published in `/proc/sensor/<part>` as `mag_x_nt` ... and not over MQTT. The RM3100 and MMC5983 are sampled every second: the RM3100 as the mean of 8 measurements (about 110 ms), the MMC5983 as the mean of 4 SET/RESET pairs (about 80 ms). With several present, `sensor` lists each by name and the unqualified channel (`(sensor-read 'mag-x)`) comes from the RM3100, then the MMC5983, then the TLV493D. Axes are the chip's own and the two chips are not aligned with each other, so compare their total field, not their components.

For scale: the earth's field is 25-65 uT. A quiet day moves it by 20-50 nT; a minor storm by 50-100 nT, a major one by several hundred. The sensors' noise is a few tens of nT per sample, so averaging over minutes and a clean setup, below, decide what can be seen. Absolute accuracy is a few percent (gain and axis alignment); the sensors measure *change* well, absolute field only roughly.

#### RM3100 Wiring (I2C mode)
The breakout, from the sensor folder's `RM3100-pinout.txt`:

| Breakout pin | Connect to | Why |
| :--- | :--- | :--- |
| 8 I2C/SPI | 3.3V | The chip's I2CEN pin: high selects I2C, low selects SPI |
| 2 MOSI/SDA | Host SDA (shared bus; C6-Zero: GPIO0) | I2C data |
| 4 SCK/SCL | Host SCL (shared bus; C6-Zero: GPIO1) | I2C clock, at most 1 MHz |
| 1 CS/A0 | GND (or 3.3V) | Address bit 0 (SA0) |
| 3 MISO/SA1 | GND (or 3.3V) | Address bit 1 (SA1) |
| 5 3.3V | 3.3V | The chip takes 2.0-3.6 V, so not 5 V |
| 6 GND | GND | |
| 7 DRDY | Optional: a free GPIO | Goes high when a measurement is ready. The driver polls the STATUS register (0x34) instead, so it can stay unconnected. |

The driver looks at all four addresses `0x20`-`0x23`, so either choice of SA0 and SA1 works (both low is `0x20`). Those are also the addresses of a PCF8574 I/O expander (an HD44780 LCD backpack with its address jumpers closed): the probe writes to whatever answers there, so do not put one on the same bus.

#### MMC5983MA Wiring
Power it from 2.8-3.6 V (3.3 V) and connect SDA and SCL to the shared bus; the address is fixed at `0x30`. Breakouts differ in their pinout and in whether they carry the I2C pull-ups and the SET/RESET capacitor the chip needs on its CAP pin, so follow the breakout's own documentation. Strapping a breakout to I2C may be needed (it also speaks SPI).

#### Setting Up for Clean Measurements
A development board on a desk sees none of the earth's small variations: the magnetometer reads its own board, cable and desk. Measured on a C6-Zero with the sensors a few centimetres from the radio, the RM3100 showed about +/-100 nT of scatter with occasional 1-2 uT spikes (probably Wi-Fi transmit bursts) and the MMC5983 about +/-40 nT. A storm of 50-100 nT is not resolvable that way. For real measurements:

* **Distance is the only shield.** A static or slow field cannot be shielded without also shielding the quantity being measured (mu-metal would hide the earth's field from the sensor), and aluminium or copper do nothing against it. Fields from a current loop fall with the cube of the distance: put the sensors on a cable or boom of 0.5-1 m or more from the microcontroller, its radio and antenna, the USB cable, the RGB pixel, the power supply and anything that switches current. Keep the radio quiet if you can (Wi-Fi off, or a longer MQTT interval) or move the sensors away.
* **Twist the supply and signal leads.** Supply and ground, and SDA with SCL, as twisted pairs (or one twisted-pair cable) so the currents in them cancel; keep the I2C cable short (under 1 m at 100 kHz) and use a linear supply or a battery rather than a switching adapter for the sensor board.
* **No steel, nickel or magnets near the sensors.** Use brass, nylon or aluminium screws and standoffs. Check headers (nickel plating is magnetic), USB and SMA connectors, breadboard springs, shielding cans, a speaker and the SD card socket. A wristwatch, phone or tool moved nearby is plainly visible as a step: use that as a test.
* **Mount it rigidly and keep it still.** A tilt of 1 mrad (0.06 degrees) moves a component of a 50 uT field by up to 50 nT: more than a minor storm. Fix the sensor to something that does not flex, vibrate or settle, and keep it out of draughts, sun and heaters (thermal movement tilts it and the MMC5983 and RM3100 both drift a little with temperature).
* **Mind the site.** Outdoors or on a ground floor in a weatherproof non-magnetic box, away from steel structure, rebar, parked cars, lifts, electric railways and trams (their DC return currents move the field by uT), and mains cables.
* **Use both sensors.** Mount the RM3100 and MMC5983 a metre or so apart. A change both see together is the earth's; a change only one sees is a local source nearby it.

#### Analysis Tips
* Compare the **total field** `sqrt(x^2 + y^2 + z^2)`: to first order a small tilt does not change it, so it separates a real change from the sensor moving. Per-axis changes show the direction of the disturbance.
* Subtract a slow baseline and look at 1-5 minute means. Storms and substorms last tens of minutes to hours; the daily variation is smooth and repeats; a step or a sawtooth is a local source.
* Keep a log with timestamps (`mqttd` does not carry these channels; read `/proc/sensor/rm3100` or `(sensor-read 'rm3100)` and log from a script) and compare it against a public reference: the planetary K index (Kp) from GFZ Potsdam or NOAA SWPC, and the data of the nearest INTERMAGNET observatory.
* The sensors are not calibrated against a reference: gain is good to a few percent. Do not compare absolute values between sensors or against published fields, only changes.

### Reading Sensor Telemetry
```text
lsh> sensor
BME280 [0x76]:
  Temperature: 21.45  deg C
  Humidity:    48.20 %
  Pressure:    1013.25 hPa
```
Sensors can also be queried programmatically via `/proc`:
```text
lsh> cat /proc/sensors
```

---

## 2. Autonomous MQTT 3.1.1 Telemetry Client

LugalOS incorporates a native, zero-dependency MQTT 3.1.1 client running directly atop the microkernel TCP/IP stack.

### Manual MQTT Operations
```text
# Connect to broker
lsh> mqtt connect 192.168.1.100:1883 [username] [password]

# The same over TLS (port 8883 unless given): the broker's certificate must come from
# the CA on /dev/tlsca and name its address -- see the HTTPS certificates guide
lsh> mqtt connect tls://192.168.1.100

# Publish a single message
lsh> mqtt pub lugalos/nodes/livingroom/temp "21.45"

# Subscribe to incoming topics
lsh> mqtt sub lugalos/control/#

# Disconnect
lsh> mqtt disconnect
```

### The Autonomous Telemetry Daemon (`mqttd`)
Rather than polling on a fixed timer, `mqttd` operates using intelligent publish rules:
1. **Exponential Smoothing**: Raw ADC/sensor samples pass through an exponential moving average filter to eliminate sensor jitter.
2. **Delta Thresholding**: A metric is published immediately if it changes by more than `delta` from the previous published value.
3. **Rate Limiting (`min_interval`)**: Prevents bus flooding during rapid temperature fluctuations.
4. **Heartbeat (`max_interval`)**: Even if values remain static, the node emits periodic reports so the broker knows the node is healthy.
5. **Last Will and Testament (LWT)**: Configured on connection; if a node loses power or radio link, the broker immediately marks it offline.

### Setting Telemetry Rules
```text
# Rule syntax: mqttd rule <metric_id> <min_interval_s> <max_interval_s> <delta>
lsh> mqttd rule 0 5 300 0.2
```
*Rule above: Publish metric 0 (temperature) at least every 300 seconds, at most every 5 seconds, or immediately whenever temperature changes by $\ge 0.2^\circ\text{C}$.*

---

## 3. Appliance Zero-Touch Provisioning

An environmental node can be provisioned once so that it boots, joins Wi-Fi, and publishes sensor telemetry with zero user interaction:

1. Configure Wi-Fi credentials in the identity record:
   ```text
   lsh> wlan my-network my-hex-psk
   ```
2. Configure the target MQTT broker in the identity record:
   ```text
   lsh> mqttcfg 192.168.1.100:1883 10
   ```
   or, to a broker with a certificate from your LugalOS CA, `mqttcfg tls://192.168.1.100 10` ([MQTT over TLS](../admin/tls.md#4-the-node-as-a-client-mqtt-over-tls)).
3. Reboot the board. The kernel automatically:
   * Connects to the wireless access point.
   * Probes the I2C sensors.
   * Connects to the MQTT broker and registers its Last Will.
   * Starts streaming sensor readings according to configured delta rules.

---

## 4. Hardware Datasheet References

Official component datasheets for all supported environmental and light sensors are stored on the host system under `~/gith/sensors/` or `~/Source/gith/sensors/`:
* **BME280:** `bst-bme280-ds002.pdf`
* **BME680:** `bst-bme680-ds001.pdf`
* **CCS811:** `CCS811_Datasheet-DS000459.pdf`
* **HDC1080:** `hdc1080.pdf`
* **MCP9808:** `MCP9808.pdf`
* **MMC5983MA:** `MMC5983MA_RevA_4-3-19.pdf`
* **MH-Z19B:** `mh-z19b-co2-ver1_0.pdf`
* **MiCS-6814:** `MiCS-6814_Datasheet.pdf` & `Multichannel gas sensor 1.0.pdf`
* **RM3100:** `RM3100-RM2100-Sensor-Suite-User-Manual-V11.0.pdf`
* **SCD4x:** `CD_DS_SCD4x_Datasheet_D1.pdf`
* **SGP30:** `Sensirion_Gas_Sensors_Datasheet_SGP30.pdf`
* **TLV493D:** `infineon-tlv493d-a1b6-datasheet-en.pdf`
* **TMP117:** `tmp117.pdf`
* **TSL2561:** `TSL2561.pdf`
* **TSL2591:** `TSL25911_Datasheet_EN_v1.pdf`

---

**Navigation:** [<- User Manual](index.md) | [Documentation Index](../README.md)
