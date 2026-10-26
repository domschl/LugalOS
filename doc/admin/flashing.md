# LugalOS Firmware Flashing Guide

[Documentation](../README.md) > [Administrator Manual](index.md) > **Firmware Flashing Guide**

---

This document describes procedures for flashing and programming LugalOS binaries across supported microcontrollers: Raspberry Pi Pico 2 (RP2350), Espressif ESP32-P4, and ESP32-C6.

---

## 1. Flashing RP2350 Targets (Pico 2 & RP2350-LCD-7)

RP2350 boards generate standard UF2 firmware files. The build outputs three images:
1. `ptable.uf2`: The A/B partition table (flash sector 0). Written once per board, and again only if `cmake/flash_layout.cmake` changes.
2. `lugalos.uf2`: The operating system kernel image.
3. `flashfs.uf2`: The `/flash0` filesystem image containing embedded utilities and libraries (absolute flash address; a board that already has one keeps it).

The kernel runs from one of two slots the bootrom treats as an A/B pair (`doc/admin/updates.md`). A board that was flashed before the partition table existed needs the **first-time install**: BOOTSEL, then copy `ptable.uf2`, wait for the drive to settle, then copy `lugalos.uf2` to the BOOTSEL drive. `/flash0` and the identity sector are in the same place as before and survive it. With `picotool`, write the table, then **reboot into BOOTSEL (`picotool reboot -u`) before loading the image**: the bootrom reads the table at boot, and an image loaded before that is treated as an absolute write at flash address 0, which overwrites the table. After that, `lugalos.uf2` copied to the drive goes to the slot that is *not* the one the bootrom would boot, and the board restarts into it; or update over the network with `tools/lota.py push` and no BOOTSEL at all.

### Method A: Automated BOOTSEL via 1200-Baud Touch
LugalOS's USB CDC stack implements the 1200-baud touch protocol on the console port (`if00`). Opening and immediately closing the port at 1200 baud arms a deferred bootrom reset directly into BOOTSEL mode:

```python
import serial
with serial.Serial("/dev/serial/by-id/usb-LugalOS_rp2350_Dual_CDC_ACM_...-if00", 1200):
    pass
```

### Method B: Programming via `picotool`
Once the board is in BOOTSEL mode (or when attached via USB while holding the physical BOOTSEL button):

```bash
# Flash the operating system kernel
picotool load -v -x build/rp2350-terminal/lugalos.uf2

# If updating embedded flash filesystem (/flash0):
picotool load -v build/rp2350-terminal/flashfs.uf2
```

`picotool` reads the partition table: `lugalos.uf2` goes to the A/B slot the bootrom would not boot, and `-x` makes a flash-update boot of it. That boot **erases the other slot's first sector** (datasheet 5.1.16), so a `picotool` load leaves one image and no fallback; it is the developer flow. Updates that keep the old image until the new one has proven itself are `tools/lota.py push` (`doc/admin/updates.md`). Without the udev rule in `tools/udev/60-rp2350-bootsel.rules` (copy it to `/etc/udev/rules.d/`), `picotool` cannot open the board.

> [!CAUTION]
> Always flash `build/rp2350-terminal/lugalos.uf2` for the 7" LCD terminal. Flashing the generic `rp2350` build will leave the LCD uninitialized (blank/black).

---

## 2. Flashing ESP32-P4 Boards (Tab5, LCD-7B & NANO)

The ESP32-P4 uses `tools/p4flash.py` to program the second-stage bootloader and the Execute-In-Place (XIP) flash image.

### Board Disambiguation (`--board`)
Always specify `--board nano`, `--board lcd7b`, or `--board tab5` when flashing, as they use different serial bridges and silicon revisions (NANO and Tab5 are rev v1.3; LCD-7B is rev v3.2):

```bash
# Flash M5Stack Tab5 5" Portable Tablet
ninja -C build/esp32p4-tab5
python3 tools/p4flash.py --board tab5 --build build/esp32p4-tab5 --only boot,os --verify

# Flash ESP32-P4 7" Touch LCD Terminal
ninja -C build/esp32p4-lcd7b
python3 tools/p4flash.py --board lcd7b --build build/esp32p4-lcd7b --only boot,os --verify

# Flash ESP32-P4 NANO Compute Node
ninja -C build/esp32p4
python3 tools/p4flash.py --board nano --build build/esp32p4 --only boot,os --verify
```

> [!WARNING]
> Do NOT open the serial port with generic tools (`screen`, `cu`) before flashing, as raising DTR/RTS can hold the ESP32-P4 in reset. `tools/p4flash.py` and `tools/p4run.py` handle modem control correctly.

---

## 3. Flashing ESP32-C6-Zero

The ESP32-C6 uses the chip's native USB-Serial/JTAG controller (`303a:1001`). The kernel consists of two parts: `.text`/`.rodata` executing in place from flash at `0x42000000`, with remaining data living in SRAM.

### Development Loop (Load to RAM & Flash)
To run a kernel without writing the loader or a slot's RAM half (the XIP half
goes to slot A's, the RAM half is loaded over USB):
```bash
ninja -C build/esp32c6
tools/c6run.py --kernel build/esp32c6 --run
```

### Permanent Flash Programming
The C6's flash holds a small permanent **loader** at `0x0` (what the boot ROM
starts), two **boot records**, and two **slots**, each with one update image
(see [Updating Nodes over 9P](updates.md)). Through the header pins (or the
C6-Zero's USB), `c6flash.py` writes the loader, erases the boot records and
puts the build into slot A:
```bash
ninja -C build/esp32c6
tools/c6flash.py build/esp32c6
tools/c6flash.py build/esp32c6 --only slot     # slot A only, the loader kept
```
The script opens the port with DTR high and RTS low (opening with DTR low resets the chip) and verifies written blocks.
After that, new builds go over 9P with `tools/lota.py push`, without the header.

---

**Navigation:** [<- Presets Matrix](presets.md) | [Administrator Manual](index.md) | [Consoles Guide ->](consoles.md) | [Documentation Index](../README.md)
