# LugalOS Hardware Presets & Targets

[Documentation](index.md) > [Administrator Manual](admin.md) > **Hardware Presets & Targets**

---

LugalOS supports a range of distinct hardware presets across RISC-V microcontrollers and virtual machines. Each preset configures the target architecture, clock trees, peripheral drivers, and memory layouts via CMake presets (`CMakePresets.json`).

---

## 1. Presets Comparison Matrix

| Preset Name | Architecture | SoC / Board | Clock | RAM & Cache | Display / UI | Network & Storage | Primary Role |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`rp2350-terminal`** | RV32 Hazard3 | Waveshare RP2350-LCD-7 | 150 MHz | 520 KB SRAM + 8 MB PSRAM | 7" 800x480 RGB LCD, GT911 Touch | SD/SPI, Dual CDC USB | Workstation Terminal |
| **`esp32p4-lcd7b`** | RV32 Dual-Core | ESP32-P4-WIFI6-LCD-7B (rev v3.2) | 400 MHz | 768 KB L2MEM + 32 MB PSRAM | 7" 1024x600 MIPI-DSI, GT911 Touch | SDMMC, USB Host, Wi-Fi 6 | High-Perf Ribbon Workstation |
| **`esp32p4-tab5`** | RV32 Dual-Core | M5Stack Tab5 (rev v1.3) | 360-400 MHz | 768 KB L2MEM + 32 MB PSRAM | 5" 720x1280 MIPI-DSI, ST7121 Touch | SDMMC, USB Host, Dual Expander | 5" Portable Tablet / Reader |
| **`esp32p4`** | RV32 Dual-Core | ESP32-P4-NANO (rev v1.3) | 360-400 MHz | 768 KB L2MEM + 32 MB PSRAM | Serial Console (921600 baud) | RMII Ethernet, SDMMC | Microkernel Compute Node |
| **`esp32c6`** | RV32IMAC | Waveshare ESP32-C6-Zero | 160 MHz | 512 KB SRAM, 8 MB Flash | Serial / JTAG Console | 802.11ax Wi-Fi, Flashfs | Low-Power Wireless Node |
| **`esp32c6-release`** | RV32IMAC | Waveshare ESP32-C6-Zero | 160 MHz | 512 KB SRAM, 8 MB Flash | Serial / JTAG Console | 802.11ax Wi-Fi, Flashfs | The same node deployed: release image, `i2c` folded |
| **`rp2350-chess`** | RV32 Hazard3 | Raspberry Pi Pico 2 | 150 MHz | 520 KB SRAM | 1.8" ST7735 TFT, TM1638 Keypad | SD/SPI card | Dedicated Chess Computer |
| **`rp2350-clock`** | RV32 Hazard3 | Waveshare Pico-Clock-Green | 150 MHz | 520 KB SRAM | SM16106 LED Matrix | DCF77 Radio, RTC | Precision Network Time Node |
| **`rp2350-gateway`** | RV32 Hazard3 | Raspberry Pi Pico 2 | 150 MHz | 520 KB SRAM | Serial Console | ENC28J60 Ethernet, USB 9P | Network Gateway Node |
| **`rp2350-wifi`** | RV32 Hazard3 | Raspberry Pi Pico 2 W | 150 MHz | 520 KB SRAM | Serial Console | CYW43439 Wi-Fi (802.11n) | Wireless IoT Node |
| **`rv32`** | RV32 NOMMU | QEMU `virt` machine | Emulated | 128 MB RAM | VirtIO Console | VirtIO Blk, VirtIO Net | 32-bit Microkernel CI Target |
| **`rv64`** | RV64 Sv39 MMU| QEMU `virt` machine | Emulated | 128 MB RAM | VirtIO Console | VirtIO Blk, VirtIO Net | 64-bit Virtual Memory Target |
| **`rv64-smp`** | RV64 SMP | QEMU `virt` (2 harts) | Emulated | 128 MB RAM | VirtIO Console | VirtIO Blk, VirtIO Net | Multicore SMP CI Target |
| **`rv32-fold`** | RV32 NOMMU | QEMU `virt` machine | Emulated | 128 MB RAM | VirtIO Console | VirtIO Blk, VirtIO Net | `rv32` with `uart`, `blk` and `i2c` folded: the folded twin |

---

## 2. Boards and Profiles

A preset is a **board** and a **profile** (`cmake/lugalos_layers.cmake`):

* The **board** (`cmake/board-<name>.cmake`) is what the hardware is: addresses, pins, memory sizes, what the hardware can do (`CONFIG_NETWORK_CLAIM`: it has a setup access point), and which drivers its hardware needs (`LUGALOS_ENABLE_SPISD`, `LUGALOS_ENABLE_SDMMC`, ...). Its name is the board id an update image is checked against.
* The **profile** (`cmake/profile-<name>.cmake`) is what the node is for: which software it carries (`LUGALOS_ENABLE_CC`, `LUGALOS_ENABLE_CHESS`, ...) and the policy and sizes that follow (Lisp pools, TLS, the RAM disk, the buddy arena, `CONFIG_KLOG_RING_KB`, `CONFIG_SHELL_HISTORY`, `CONFIG_MAX_VTERMS`).

A key in the wrong layer stops the configure. A command-line `-DLUGALOS_ENABLE_...` does not override a layer; another profile does.

| Profile | Presets | Carries |
| :--- | :--- | :--- |
| `workstation` | `esp32p4`, `esp32p4-lcd7b`, `esp32p4-tab5`, `rp2350-terminal` | Everything: CAS, chess (512 KB table), games, `cc`, `ed`, HTTPS admin, 65536 Lisp nodes in PSRAM |
| `sensor` | `esp32c6`, `rp2350-wifi`, `rp2350-sensor` | Sensors, MQTT, 9P, the admin page once claimed; no compiler, editor or games. Sized for a node without a screen: a 4 KB log ring, two lines of shell history, four terminals, no RAM disk (`init.lisp` reads the profile from `/proc/config`) |
| `radio` | `esp32c6-mini` | The Wi-Fi modem beside a P4 (LLP over SDIO), sized as `sensor` |
| `chess` | `rp2350-chess`, `rp2350-smp` | The ST7735/TM1638 chess computer |
| `clock` | `rp2350-clock` | The LED clock: DCF-77, NTP server |
| `gateway` | `rp2350-gateway` | Ethernet and the UART1 downlink |
| `qemu` | `rv32`, `rv64`, `rv64-smp` | Everything, for the automated suite |

Besides the two layers a preset sets `LUGALOS_ENABLE_SMP`, and the build sets `LUGALOS_DEV`: ON by default, a **dev image** with the self-tests and demo commands the test suites drive; `cmake --preset esp32c6 -DLUGALOS_DEV=OFF` builds a **release image** without them. `/proc/config` names the profile and whether the image is a dev build.

A preset may also **fold** driver endpoints, `LUGALOS_FOLD` (a comma-separated list such as `i2c` or `uart,blk,i2c`). A folded driver has no task of its own: its requests are served in the caller's context, which saves the driver's stack (one page for the C6's `i2c`). Callers and drivers do not change. `esp32c6-release` folds `i2c` and turns `LUGALOS_DEV` off; `esp32c6` is the same node unfolded, with its test commands. `/proc/config` lists what is folded (`FOLD=`). Which endpoints may fold, and why, is [ADR 008](008_folded_endpoints.md).

## 3. Display Density Profiles & Typography

LugalOS supports display-density-aware typography to ensure optimal legibility across varying panel sizes:

| Density Profile | Target Panels & DPI | Monospace Terminal Cell | Reader Typography | Typical Screen Geometries |
| :--- | :--- | :--- | :--- | :--- |
| **Standard Density** | `rp2350-terminal` (133 DPI), `esp32p4-lcd7b` (170 DPI) | $8 \times 16$ px (Spleen) | 15 px Proportional Serif (18 px line height) | $98 \times 27$ cells (800x480), $126 \times 34$ cells (1024x600) |
| **HiDPI Handheld** | `esp32p4-tab5` (294 DPI) | $16 \times 32$ px (Noto Sans Mono) | 32 px Anti-Aliased Noto Serif (38 px line height) | Landscape: $78 \times 21$ cells (1280x720) / Portrait: $43 \times 38$ cells (720x1280) |

All font tables are stored in Flash `.rodata` for $O(1)$ direct access with zero dynamic SRAM consumption.

---

## 4. Building Presets

To build any preset using CMake and Ninja:

```bash
# Example: Build for ESP32-P4 7" LCD Terminal
cmake --preset esp32p4-lcd7b
ninja -C build/esp32p4-lcd7b

# Example: Build for M5Stack Tab5 5" Portable Tablet
cmake --preset esp32p4-tab5
ninja -C build/esp32p4-tab5

# Example: Build for RP2350 7" Terminal
cmake --preset rp2350-terminal
ninja -C build/rp2350-terminal

# Example: Build for ESP32-C6-Zero
cmake --preset esp32c6
ninja -C build/esp32c6

# Example: Build and test on QEMU RV64
cmake --preset rv64
ninja -C build/rv64
ninja -C build/rv64 run
```
See the [flashing guide](flashing.md) for instructions on programming images onto physical hardware.

---

**Navigation:** [<- Administrator Manual](admin.md) | [Flashing Guide ->](flashing.md) | [Documentation Index](index.md)
