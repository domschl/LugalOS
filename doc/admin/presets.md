# LugalOS Hardware Presets & Targets

LugalOS supports a range of distinct hardware presets across RISC-V microcontrollers and virtual machines. Each preset configures the target architecture, clock trees, peripheral drivers, and memory layouts via CMake presets (`CMakePresets.json`).

---

## 1. Presets Comparison Matrix

| Preset Name | Architecture | SoC / Board | Clock | RAM & Cache | Display / UI | Network & Storage | Primary Role |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`rp2350-terminal`** | RV32 Hazard3 | Waveshare RP2350-LCD-7 | 150 MHz | 520 KB SRAM + 8 MB PSRAM | 7" 800×480 RGB LCD, GT911 Touch | SD/SPI, Dual CDC USB | Workstation Terminal |
| **`esp32p4-lcd7b`** | RV32 Dual-Core | ESP32-P4-WIFI6-LCD-7B (rev v3.2) | 400 MHz | 768 KB L2MEM + 32 MB PSRAM | 7" 1024×600 MIPI-DSI, GT911 Touch | SDMMC, USB Host, Wi-Fi 6 | High-Perf Ribbon Workstation |
| **`esp32p4-tab5`** | RV32 Dual-Core | M5Stack Tab5 (rev v1.3) | 360–400 MHz | 768 KB L2MEM + 32 MB PSRAM | 5" 720×1280 MIPI-DSI, ST7121 Touch | SDMMC, USB Host, Dual Expander | 5" Portable Tablet / Reader |
| **`esp32p4`** | RV32 Dual-Core | ESP32-P4-NANO (rev v1.3) | 360–400 MHz | 768 KB L2MEM + 32 MB PSRAM | Serial Console (921600 baud) | RMII Ethernet, SDMMC | Microkernel Compute Node |
| **`esp32c6`** | RV32IMAC | Waveshare ESP32-C6-Zero | 160 MHz | 512 KB SRAM, 8 MB Flash | Serial / JTAG Console | 802.11ax Wi-Fi, Flashfs | Low-Power Wireless Node |
| **`rp2350-chess`** | RV32 Hazard3 | Raspberry Pi Pico 2 | 150 MHz | 520 KB SRAM | 1.8" ST7735 TFT, TM1638 Keypad | SD/SPI card | Dedicated Chess Computer |
| **`rp2350-clock`** | RV32 Hazard3 | Waveshare Pico-Clock-Green | 150 MHz | 520 KB SRAM | SM16106 LED Matrix | DCF77 Radio, RTC | Precision Network Time Node |
| **`rp2350-gateway`** | RV32 Hazard3 | Raspberry Pi Pico 2 | 150 MHz | 520 KB SRAM | Serial Console | ENC28J60 Ethernet, USB 9P | Network Gateway Node |
| **`rp2350-wifi`** | RV32 Hazard3 | Raspberry Pi Pico 2 W | 150 MHz | 520 KB SRAM | Serial Console | CYW43439 Wi-Fi (802.11n) | Wireless IoT Node |
| **`rv32`** | RV32 NOMMU | QEMU `virt` machine | Emulated | 128 MB RAM | VirtIO Console | VirtIO Blk, VirtIO Net | 32-bit Microkernel CI Target |
| **`rv64`** | RV64 Sv39 MMU| QEMU `virt` machine | Emulated | 128 MB RAM | VirtIO Console | VirtIO Blk, VirtIO Net | 64-bit Virtual Memory Target |
| **`rv64-smp`** | RV64 SMP | QEMU `virt` (2 harts) | Emulated | 128 MB RAM | VirtIO Console | VirtIO Blk, VirtIO Net | Multicore SMP CI Target |

---

## 2. Hardware Profiles: Full vs Minimal

LugalOS operates with two memory resource profiles:
1. **Full Profile (Boards with PSRAM)**:
   * `rp2350-terminal`, `esp32p4-lcd7b`, `esp32p4-tab5`, `esp32p4`, and QEMU.
   * Allocates bulk pools (`BULK_BSS`) in PSRAM for 16k–64k Lisp nodes, graphical framebuffers, and full standard libraries.
2. **Minimal Profile (Boards without PSRAM)**:
   * `esp32c6`, `rp2350-chess`, `rp2350-clock`, `rp2350-wifi`.
   * Strict `.bss` budgeting: 1,024 Lisp node pool, lean boot libraries, on-demand module loading via `(require 'pkg)`. Zero SRAM impact on physical heap allocator (`palloc`).

---

## 3. Building Presets

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
