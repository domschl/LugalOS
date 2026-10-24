# LugalOS Target Hardware & Board Reference Guides

[Documentation](../../README.md) > [Administrator Manual](../index.md) > **Board Reference Guides**

---

This directory contains in-depth hardware reference documents, peripheral register mappings, unbricking procedures, and operational notes for specific LugalOS hardware platforms.

---

## Supported Board Guides

1. **[ESP32-P4-WIFI6-Touch-LCD-7B Guide](esp32p4_lcd7b.md)** (`esp32p4-lcd7b`)
   * 7" 1024×600 MIPI-DSI workstation terminal (silicon revision v3.2).
   * DW-GDMA scanout, 32 MB DDR PSRAM tuning, GT911 touch, DWC2 OTG USB host, and SDIO high-speed link to ESP32-C6.

2. **[M5Stack Tab5 Guide](esp32p4_tab5.md)** (`esp32p4-tab5`)
   * 5" 720×1280 portable tablet (silicon revision v1.3).
   * Dual PI4IOE expanders, ST7121 TDDI display & touch, BMI270 IMU, INA226 fuel gauge, PMP dynamic allocation, and dual-orientation compositor.

3. **[ESP32-C6-Zero & C6-MINI Guide](esp32c6.md)** (`esp32c6`, `esp32c6-mini`)
   * Low-power 802.11ax Wi-Fi co-processor and autonomous wireless node.
   * Dual-image XIP execution, LP_AON breadcrumbs, LLP radio role, download mode reset bit, and OTA updates over 9P.

4. **[RP2350 Hardware Guide](rp2350.md)** (`rp2350-terminal`, `rp2350-chess`, `rp2350-wifi`)
   * Raspberry Pi Pico 2 and Waveshare RP2350-LCD-7.
   * Dual CDC-ACM USB serial architecture (console + 9P), 1200-baud touch reset, and `picotool` flashing.

---

## General Rules for Hardware Access

* **Dynamic Port Identification:** Linux device node numbers (`/dev/ttyACM*`, `/dev/ttyUSB*`) depend entirely on kernel enumeration order. Never assume fixed port names; always inspect `/dev/serial/by-id/` or configure `~/.config/lugalos/p4-ports.env` / `c6-ports.env`.
* **Modem Control Protection:** Opening serial ports with naive tools (`screen`, `cu`, raw `pyserial`) asserts DTR/RTS and resets ESP32 targets. **Always communicate via [`tools/p4run.py`](file:///home/dsc/Source/gith/domschl/lugalos/tools/p4run.py) or [`tools/c6run.py`](file:///home/dsc/Source/gith/domschl/lugalos/tools/c6run.py)**.

---

**Navigation:** [← Administrator Manual](../index.md) | [Documentation Index](../../README.md)
