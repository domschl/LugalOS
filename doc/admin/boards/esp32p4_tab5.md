# M5Stack Tab5 Hardware Reference Guide

[Documentation](../../README.md) > [Administrator Manual](../index.md) > [Board Reference Guides](index.md) > **M5Stack Tab5 Guide**

---

This document provides in-depth hardware specifications, peripheral register details, power management procedures, and operational notes for the **M5Stack Tab5** 5" portable tablet (`esp32p4-tab5`).

---

## 1. Port Topology & Modem Signals

* **Controller:** Single USB-C cable carries native **USB-Serial/JTAG** (VID:PID `303a:1001` on `GPIO24`/`GPIO25`).
* **Console Mirroring:** LugalOS mirrors console TX and RX to both USB-Serial/JTAG and hardware UART0 (`GPIO37`/`GPIO38` on the bottom M5-Bus connector).
* **Detection:** `tools/p4run.py --board tab5` and `tools/p4flash.py --board tab5` auto-detect the JTAG port by symlink.
* **Silicon Revision:** Revision **v1.3** (same as ESP32-P4-NANO).
* **Reset Note:** Software `reboot` is not supported on ESP32-P4; restart using `tools/p4run.py --board tab5 --run`.

---

## 2. Build, Flash & Console Workflows

### Building Firmware
```bash
ninja -C build/esp32p4-tab5
```

### Flashing Firmware
```bash
python3 tools/p4flash.py --board tab5 --build build/esp32p4-tab5 --only boot,os --verify
```

### Running Interactive Console
```bash
python3 tools/p4run.py --board tab5 --run
```

### Automated Hardware-in-the-Loop Testing
```bash
cd tests/hw && uv run test_esp32p4.py --board tab5 --build ../../build/esp32p4-tab5
```
Runs 46 silicon verification checks covering power management, expanders, battery monitoring, IMU orientation, touch, and SDIO Wi-Fi.

---

## 3. Power Architecture & Battery Fuel Gauge

### Dual PI4IOE5V6408 I2C Expanders
Two expanders reside on `SYS_I2C` (`GPIO31` SDA / `GPIO32` SCL) at `0x43` and `0x44`:
* Power rail sequencing, hardware reset lines for LCD and touch.
* USB 5V VBUS enable: Expander `U7` (`0x43`) bit 3.
* Charger control: Expander `0x44` controls IP2326 (Pin 7 = `CHG_EN`, Pin 5 = `nQC`).

### Battery Monitor (TI INA226)
* Sits at I2C address `0x41` across a 5 mOhm shunt resistor on the internal 2S battery pack.
* LugalOS inverts current polarity so that positive current represents charging.
* Commands:
  * `power`: Displays battery voltage, current, power draw, and charge state.
  * `cat /dev/power`: Raw machine-readable power telemetry.
  * `power charge off|slow|fast`: Configures IP2326 charging speed.
  * `power limit N|off`: Stores charge limit in the board's identity store.
  > [!CAUTION]
  > Never execute `power off` on a remotely accessed board; it pulses `PWROFF` (P4) and completely shuts off the device.

---

## 4. Inertial Measurement Unit (Bosch BMI270) & Auto-Rotation

* **Address:** `0x68` on `SYS_I2C`.
  > [!IMPORTANT]
  > `0x68` is the **BMI270 IMU**, NOT a DS3231 RTC! The Tab5's onboard RTC is the **RX8130CE at `0x32`**.
* **Microcode Initialization:** Bosch's proprietary microcode must be uploaded at boot (`drivers/bmi270_config.c`, taking ~1.3 seconds during the first sensor read).
* **Pacing Rule:** The I2C kernel task serves one request at a time without queueing. High-frequency callers must yield between transfers to prevent starving other tasks.
* **Screen Rotation:**
  * Supports four orientations: 0 deg, 90 deg, 180 deg, and 270 deg (`lcd orient [0|90|180|270]`).
  * Automatic rotation via IMU: `orient auto|off` (sensor mappings: $+X \to 270^\circ$, $-X \to 90^\circ$, $+Y \to 0^\circ$, $-Y \to 180^\circ$).
* **Diagnostics:** `imu`, `imu watch`.

---

## 5. Display Subsystem & Dual-Orientation Compositor

* **Panel:** 5" 720x1280 MIPI-DSI ST7121 TDDI (Touch and Display Driver Integration).
* **Interface:** 2-lane MIPI-DSI @ 900 Mbps, DPI clock 70 MHz.
* **Scanout:** Double-buffered RGB565 PSRAM framebuffer scanned out at 58 Hz.
* **Backlight:** Active-high LEDC PWM on `GPIO22`.
* **Capacitive Touch:** Integrated ST7121 at `0x55`, polled every 20 ms with coordinate remapping according to active display orientation.
  > [!NOTE]
  > Address `0x28` is a secondary internal address of the ST7123 TDDI controller. Never probe or write to `0x28`.
* **Dual-Orientation Compositor Modes:**
  1. **Landscape Normal LugalOS GUI (`1280x720`, 160x45 character cells):** Optimized for multi-window Ribbon terminal operations with hardware blitting.
  2. **Portrait Full-Screen Gemtext Reader (`720x1280`, 90x80 character cells):** Optimized for long-form reading, documentation, and Spartan hypertext browsing. Press `'o'` in `reader` to switch orientations dynamically.

---

## 6. Storage & Peripherals

* **MicroSD Card:** 4-bit SDMMC Slot 0 (`GPIO39`-`GPIO44`) mounted at `/sd0/` at 20 MHz.
* **USB Host:** DWC2 OTG HS on Type-A port; 5V VBUS enabled via PI4IOE expander.
* **Grove Port A:** Second independent I2C bus (`I2C1`, `GPIO53` SDA / `GPIO54` SCL; `i2c scan 1`) for connecting external sensors.
* **Real-Time Clock:** Epson RX8130CE at `0x32` with coin-cell battery backup. Read at boot and synchronized via `date` or network NTP.

---

## 7. SDIO Wi-Fi Co-Processor (ESP32-C6)

The Tab5 is a self-contained appliance containing an internal ESP32-C6 connected over SDIO:
* **Hardware Interconnect:** SDMMC Slot 1 (`CLK=12`, `CMD=13`, `D0-D3=11/10/9/8`), with C6 reset line on `GPIO15` (active low, `CONFIG_C6_CHIP_PU_GPIO`).
* **Factory Bootloader & PMP Domains:**
  * The Tab5 factory ESP-IDF bootloader locks Physical Memory Protection (PMP) entries 0-2 and 15 (`PMP_L`).
  * The LugalOS microkernel dynamically probes locked entries at boot and allocates user domains exclusively in unlocked slots 3-14 without triggering traps.
* **Over-the-Air Updates over 9P:**
  Once the C6 is running LugalOS (for factory-fresh boards, see Section 8 first), updates to the internal C6 are performed through the P4 host over 9P/SDIO without opening the physical enclosure:
  ```bash
  uv run tools/lota.py push build/esp32c6-mini --p4 tab5 --prefix /c6 --reboot --wait
  ```
* **Boot-Time Network Configuration:**
  At boot, the Tab5 brings up LLP host mode, discovers the C6 over SDIO, mounts `/c6`, automatically pushes Wi-Fi credentials from `/sd0/etc/wlan.env` or the internal identity store to `/c6/dev/wlan`, registers network device `wlan0`, and launches the background DHCP client (`dhcpc`) to acquire an IP address.

---

## 8. Fresh Installation & First-Time Provisioning

A brand-new Tab5 ships with the factory ESP-IDF image on the P4 and `esp-hosted-mcu` on the internal C6. Installing LugalOS for the first time requires flashing all P4 segments, provisioning an identity key, and migrating the C6 from `esp-hosted-mcu` to LugalOS:

1. **Identify the USB-Serial/JTAG port** (if not yet registered in `~/.config/lugalos/p4-ports.env`, pass `--port` explicitly instead of `--board tab5`):
   ```bash
   ls /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*
   ```
2. **Build both cores:**
   ```bash
   ninja -C build/esp32p4-tab5 && ninja -C build/esp32c6-mini
   ```
3. **Flash the P4 (`boot`, `rec`, `os`, `fs`):**
   Omitting `--only` writes the permanent bootloader, boot records, slot-A OS image, and `/flash0` filesystem:
   ```bash
   python3 tools/p4flash.py --port /dev/serial/by-id/TAB5_PORT --build build/esp32p4-tab5 --verify
   ```
4. **Boot and provision identity:**
   At first boot, `idstore` initializes the identity sector with the derived name (`esp32p4-tab5-MAC4`) and silicon UID. Generate a device key in `lsh>`:
   ```bash
   python3 tools/p4run.py --port /dev/serial/by-id/TAB5_PORT --run \
     --cmd "identity key --generate" --cmd "identity"
   ```
   (Optionally set a custom name with `identity name <name>`.)
5. **Migrate the internal C6 from `esp-hosted-mcu` to LugalOS:**
   Until the C6 speaks LLP, `/c6` is not mounted and `lota.py` cannot reach it. Copy `build/esp32c6-mini/lugalos-c6-app.bin` to `/sd0/lugalos-c6-app.bin` (on the MicroSD card or via 9P `p9share`), then run `sdio ota` in `lsh>`:
   ```text
   lsh> sdio ota
   lsh> rm /sd0/lugalos-c6-app.bin
   ```
   Once `sdio ota` finishes (~14 s), the C6 reboots into LugalOS, LLP comes up, `/c6` mounts automatically, and future C6 updates can use `tools/lota.py push`.

---

**Navigation:** [<- Board Reference Guides](index.md) | [Administrator Manual](../index.md) | [Documentation Index](../../README.md)
