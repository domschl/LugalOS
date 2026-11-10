# LugalOS Storage & Filesystem Architecture

[Documentation](../README.md) > [Administrator Manual](index.md) > **Storage & Filesystems**

---

LugalOS implements a unified Virtual Filesystem (VFS) layer supporting multiple FAT32 storage backends, synthetic system filesystems (`/proc/`, `/dev/`, `/srv/`), and in-memory RAM disks.

---

## 1. Storage Backends & Volumes

| Mount Point | Backend Driver | Type | Typical Size | Description |
| :--- | :--- | :--- | :--- | :--- |
| **`/sd0/`** | SPI SD (RP2350) / SDMMC (ESP32-P4) / VirtIO (QEMU) | Physical FAT32 | 4 GB - 32 GB | Main persistent storage for binaries, CAS, games, and help |
| **`/flash0/`** | On-chip Flash (`flashfs`) | Embedded FAT32 | 512 KB - 2 MB | Read-only ROM disk containing baseline system files |
| **`/ram0/`** | DRAM / PSRAM block buffer | Volatile FAT32 | 1 MB - 16 MB | Fast scratch volume for compilation and benchmarks |
| **`/proc/`** | Kernel synthetic driver | Synthetic text | N/A | Process status (`/proc/ps`), memory (`/proc/meminfo`), disk (`/proc/df`) |
| **`/dev/`** | Device driver registry | Synthetic stream | N/A | Character and block endpoints (`/dev/uart`, `/dev/rtc`) |
| **`/srv/`** | IPC channel namespace | Channel services | N/A | Named microkernel message endpoints (`/srv/console`) |

---

## 2. MicroSD Card Preparation & Partitioning

LugalOS requires MicroSD cards formatted as **FAT32 (MBR)**:

### Formatting on Host (Linux)
```bash
# Partition as single MBR partition (type 0x0C / FAT32 LBA)
sudo fdisk /dev/sdX

# Format with FAT32
sudo mkfs.vfat -F 32 -n LUGALOS /dev/sdX1
```

### Formatting on Device
LugalOS can format connected cards directly from the shell:
```text
lsh> format /sd0
```
> [!CAUTION]
> `format` completely erases existing partition contents.

### File Names
FAT volumes (`/sd0`, `/flash0`, `/ram0`, `/nvme0`) keep **long file names** (VFAT), as a PC writes them:
* Up to 255 characters, UTF-8 in LugalOS, stored as UTF-16. Case is kept; matching ignores the case of ASCII and Latin-1 letters (`Notes.md` and `NOTES.MD` are one file).
* A name that is an 8.3 name in one case (`readme.txt`, `README.TXT`) is stored as an 8.3 entry only. Any other name gets a long-name entry and a unique short alias (`LUGALO~1.MD`), which opens the same file.
* Not allowed in names: `" * / : < > ? \ |`, control characters, and a trailing dot or space.
* A path of 128 bytes at most on the boards without PSRAM (Pico 2, RP2350 presets but the LCD-7, C6), 256 on the others. Over 9P (`p9sync.py`) the same limits hold.
* Characters outside Latin-1 are stored exactly; the console's fonts may show them as a replacement glyph.

Names on the card are **lowercase and full length** (the card image in `tools/sd_root` is; programs write lowercase: `/sd0/system/settings`, `/sd0/log/pYYMMDD.csv`, `/sd0/chess/games/`). The one exception is `vf2_uEnv.txt`, the name StarFive's U-Boot looks for.

Cards written by LugalOS before long names hold every name cut to 8.3 in capitals (`INIT.LIS` for `init.lisp`), and those are not found under their long names. Rebuild such a card (next section).

### Rebuilding a Card
The steps, each board's way of reaching it, and what to put back. `tools/p9sync.py` pushes the card image (`build/PRESET/sd_root`) and, with `--pull DIR`, copies a card's files here -- run it again to resume one cut short.

1. **Back up** what is yours: `tools/p9sync.py TRANSPORT --dst /sd0 --pull ~/card-backup/BOARD` (saved games in `chess/`, `ceres.dat`, `games/*.sav`, `system/history.lisp`, `system/settings`, screenshots, logs).
2. **Format** on the board: `format /sd0`. It sizes the volume from the card (the RP2350's SPI driver reads the card's CSD; before it took every card for 1 GiB) and takes from seconds to a minute.
3. **Sync** the image: `tools/p9sync.py TRANSPORT --src build/PRESET/sd_root --dst /sd0`.
4. **Put back** your files, renamed to lowercase where they were not (`CHESS/GAMES/GAME-001.PGN` to `chess/games/game-001.pgn`): a directory laid out as the card is, synced with `--force`.

| Board | Transport (`TRANSPORT`) | Afterwards |
| :--- | :--- | :--- |
| RP2350 (LCD-7, Pico 2) | `--serial /dev/serial/by-id/...-if02` (its own USB port) | The Pico 2 chess board: `/sd0/system/etc/usr_init.lisp` with the one line `(chess)`, which boots it into the chess app. |
| ESP32-P4 LCD-7B | `--p4 lcd7b` (its console wire, `p9share`), with `LUGALOS_P4_LCD7B_PORT` set or `~/.config/lugalos/p4-ports.env` | -- |
| M5Stack Tab5 | `--p4 tab5` (its USB console), or `--tcp IP --key HEX` over Wi-Fi (`net` shows the address; `p9key HEX` on the console sets a key for this boot) | -- |
| VisionFive 2 | `--tcp IP --key HEX` over Ethernet (type `p9key` slowly on its serial console: its UART drops bytes typed fast); `--vf2` (serial) for small files | `tools/vf2run.py --enable-autoboot --save-sd --kernel build/visionfive2/lugalos.bin`: the MBR's partition alias, `vf2_uEnv.txt` and `lugalos.bin` U-Boot boots from. Check with `tools/vf2run.py --method sd`. |

After `p9share`, a tool that ends its session leaves the wire in 9P mode: `p9share off` on the console before the next one, if it does not answer.

### Recounting Free Clusters (`df -r`)
If an unexpected reset leaves FAT32 `FSInfo` out of sync:
```text
lsh> df -r
```
This forces a full traversal of the File Allocation Table, verifying free cluster counts.

---

## 3. Directory Layout on `/sd0`

A standard LugalOS installation adheres to the following directory hierarchy:

```
/sd0/
|-- system/
|   |-- bin/          # Executable RISC-V ELF binaries (uhello.elf, etc.)
|   |-- etc/          # System configuration & boot scripts (init.lisp, stdlib.lisp)
|   \-- lib/          # Modular Lisp libraries (chart.lisp, matrix.lisp, seq.lisp)
|-- cas/              # Computer Algebra System scripts (simplify.lisp, solve.lisp...)
|-- help/             # Plain-text on-device help files generated by tools/gendocs.py
|-- CHESS/            # Opening book (BOOK.BIN) and saved game histories
|-- demos/            # Interactive demos (ca.lisp, lorenz.lisp)
\-- screenshots/      # PBM screen captures generated via Cmd+Shift+3
```

---

## 4. Embedded Flash ROM Disk (`flashfs`)

On headless appliances or boards without an SD card, LugalOS boots from `/flash0/`:
* Generated at build time by packing [`tools/sd_root/`](file:///home/dsc/Source/gith/domschl/lugalos/tools/sd_root) via `tools/create_flash_fs.py`.
* On RP2350, it is compiled into `build/rp2350/flashfs.uf2` and flashed to an independent flash offset.
* On ESP32-P4 and ESP32-C6, it is mapped into the XIP flash address space.

---

**Navigation:** [<- Identity Guide](identity.md) | [Administrator Manual](index.md) | [Updates Guide ->](updates.md) | [Documentation Index](../README.md)
