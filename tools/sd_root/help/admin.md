# LugalOS Administrator Manual

[Documentation](index.md) > **Administrator Manual**

---

The **LugalOS Administrator Manual** provides complete instructions for system provisioning, board configuration, hardware flashing, networking setup, and filesystem management across all supported RISC-V targets.

---

## Manual Chapters

1. **[Hardware Presets & Capabilities Matrix](presets.md)**
   * Complete catalog of all supported hardware presets: RP2350 (Pico 2, LCD-7, Chess, Clock, Gateway), ESP32-P4 (LCD-7B, NANO), ESP32-C6 (Zero), and QEMU (RV32, RV64, RV64-SMP).
2. **[Firmware Flashing & Toolchains](flashing.md)**
   * Toolchain prerequisites (`riscv32-unknown-elf-gcc`, `picotool`, `p4flash.py`, `c6flash.py`).
   * Bootloader stages, XIP flash layout, and UF2 generation.
3. **[Consoles, Ports & Modem Control](consoles.md)**
   * Host serial topology: dual-CDC-ACM USB ports, CP2102, CH343P.
   * Identifying interfaces via `/dev/serial/by-id/`.
   * High-speed baud rates (921600 baud on ESP32-P4) and preventing inadvertent reset via DTR/RTS modem signals.
4. **[Plan 9 Networking & Host Integration](networking.md)**
   * Serving and mounting 9P filesystems over USB CDC, SLIP, and TCP.
   * Synchronizing MicroSD cards via `tools/p9sync.py`.
   * Mounting LugalOS namespaces on Linux hosts via `fuse-p9`.
   * Wi-Fi and Ethernet network configuration.
5. **[Identity, Provisioning & Credentials](identity.md)**
   * Secure board identity storage (`idstore`).
   * Deriving Wi-Fi PSK hex secrets without storing raw passwords in plain text (`wifi.env`).
   * 9P authentication and HMAC message signing.
6. **[Storage & Filesystems](storage.md)**
   * FAT32 partition requirements on MicroSD cards.
   * Flashfs embedded ROM disk generation.
   * RAM disk sizing and formatting (`/ram0/`).
7. **[Updating Nodes over 9P](updates.md)**
   * A/B slots, a permanent loader, trial boots that confirm themselves or fall back.
   * `tools/lota.py`: building an update image and sending it over any 9P path (e.g. to a P4's C6 through `/c6`).
8. **[Target Hardware & Board Reference Guides](boards.md)**
   * Deep dive operational guides, hardware pinouts, power sequencing, and errata for [ESP32-P4-LCD-7B](esp32p4_lcd7b.md), [M5Stack Tab5](esp32p4_tab5.md), [ESP32-C6](esp32c6.md), and [RP2350](rp2350.md).

---

**Navigation:** [<- Documentation Index](index.md)
