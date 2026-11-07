# StarFive VisionFive 2 Hardware Reference Guide

[Documentation](index.md) > [Administrator Manual](admin.md) > [Board Reference Guides](boards.md) > **VisionFive 2 Guide**

---

This document provides hardware specifications, memory and interrupt topologies, and RAM/SD loading procedures for the **StarFive VisionFive 2 v1.3B** single-board computer (`visionfive2`), LugalOS's first 64-bit hardware platform.

---

## 1. SoC & Asymmetric Hart Topology (StarFive JH7110)

The VisionFive 2 is powered by the **StarFive JH7110** SoC with 8 GiB LPDDR4 (`0x40000000` - `0x2_3FFFFFFF`). Its CPU complex (`SiFive U74-MC`) contains **5 physical harts** with an asymmetric core layout:

| Physical Hart ID | Core Microarchitecture | ISA & Privilege | LugalOS Role & Scheduler Slot |
| :--- | :--- | :--- | :--- |
| **Hart 0** | SiFive S7 Monitor Core | RV64IMAC (M-mode only, no S-mode, no MMU) | Parked permanently in OpenSBI (`g_harts` ignores Hart 0) |
| **Hart 1** | SiFive U74 App Core 0 | RV64GC (M/S/U, Sv39 MMU, I$/D$) | Primary S-mode boot hart -> `g_harts[0]` (`tp = &g_harts[0]`) |
| **Hart 2** | SiFive U74 App Core 1 | RV64GC (M/S/U, Sv39 MMU, I$/D$) | Secondary SMP worker -> `g_harts[1]` (`tp = &g_harts[1]`) |
| **Hart 3** | SiFive U74 App Core 2 | RV64GC (M/S/U, Sv39 MMU, I$/D$) | Secondary SMP worker -> `g_harts[2]` (`tp = &g_harts[2]`) |
| **Hart 4** | SiFive U74 App Core 3 | RV64GC (M/S/U, Sv39 MMU, I$/D$) | Secondary SMP worker -> `g_harts[3]` (`g_harts[0..3]` total) |

* **Hardware Hart ID vs Logical Slot:** On the JH7110, physical `hartid` (`1..4`, stored in `g_harts[slot].hart_id` and passed in `a0` by OpenSBI) differs from the 0-based scheduler index (`slot = hartid - 1`). All SBI HSM (`0x48534D`) and SBI IPI (`0x735049`) calls translate logical slots through `hart_hw_id(slot)`.
* **Cache Coherency & FPU Policy:** All 4 U74 cores share a hardware-coherent 2 MiB L2 cache with `aclint-mswi` IPIs. LugalOS compiles the kernel with `-march=rv64imac_zba_zbb_zicsr_zifencei -mabi=lp64` (`sstatus.FS = 0`, zero floating-point instructions in S-mode).

---

## 2. Brick-Proof Boot Policy (Never Touch QSPI Flash)

> [!CAUTION]
> **Never modify or erase the onboard 16 MiB QSPI NOR flash (`gd25lq128`).**
> The onboard QSPI flash contains the factory `U-Boot SPL`, `OpenSBI v1.2`, and `U-Boot 2021.10`. LugalOS never writes to QSPI flash under any circumstances.

1. At power-on or reset, QSPI flash boots `SPL -> OpenSBI v1.2 -> U-Boot 2021.10` (`StarFive #` prompt on UART0).
2. `tools/vf2run.py` loads `build/visionfive2/lugalos.bin` into DRAM at `0x40200000` (above OpenSBI's protected `0x40000000 - 0x4007FFFF` firmware region) via 1.5 Mbps YMODEM (`loady` + `unzip`), TFTP (`tftpboot`), or local microSD (`fatload mmc 1:1 0x40200000 lugalos.bin`).
3. U-Boot launches LugalOS in S-mode via its standard Linux RISC-V `Image` boot command:
   ```text
   booti 0x40200000 - ${fdtcontroladdr}
   ```
   OpenSBI jumps to `_start` (`arch/riscv/common/entry.S`) in S-mode with `a0 = boot_hartid` (`1..4`), `a1 = dtb_pa`, and `satp = 0`.

---

## 3. Memory Map, PLIC Contexts, Timer & PMIC Reboot

### Physical & Sv39 Virtual Address Map
* **OpenSBI Reserved:** `0x40000000 - 0x401FFFFF` (2 MiB, unmapped in S-mode Sv39 page tables so null/low-DRAM dereferences fault immediately).
* **Kernel Image & Heap:** `0x40200000 - 0x48200000` (128 MiB identity-mapped RAM window: `.text`, `.rodata`, `.utext`, `.data`, `.bss`, per-hart stacks, 32 MiB page pool, and 64 MiB bulk zone).
* **DTB Snapshot:** `0xFF000000 - 0xFFFFFFFF` mapped `PTE_R` so U-Boot's relocated FDT (`~0xFFFC56A0`) can be inspected cleanly.

### Asymmetric PLIC S-Mode Contexts (`0x0C000000`)
Because Hart 0 (S7 monitor core) has only an M-mode PLIC context (`Context 0`) and no S-mode context, the S-mode PLIC contexts for U74 Harts `1..4` are:
* **Hart `h` (`1..4`)M-mode context:** `2 * h - 1` (`1, 3, 5, 7`)
* **Hart `h` (`1..4`) S-mode context:** `2 * h` (`2, 4, 6, 8`)
  * Priority threshold register: `0x0C000000 + 0x200000 + 0x1000 * (2 * h)`
  * Claim/complete register: `0x0C000000 + 0x200004 + 0x1000 * (2 * h)`
  * Enable bitmap base: `0x0C000000 + 0x002000 + 0x80 * (2 * h)`

### Console UART0 (`0x10000000`), 4 MHz Timer & PMIC Cold Reboot
* **UART0 (`dw-apb-uart` at `0x10000000`, PLIC IRQ 32):** Uses 32-bit register spacing (`reg-shift = 2`, `reg-io-width = 4`, `CONFIG_UART_REG_SHIFT = 2`). Default console rate is **115,200 baud** (`LUGALOS_VF2_PORT` or `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`).
* **Timer (`aclint-mtimer` at 4,000,000 Hz):** Read directly via `rdtime` in S-mode (`1 us = 4 ticks`) and armed via SBI Timer extension (`EID = 0x54494D45`, `FID = 0`).
* **Unattended Cold Reboot (`i2c5@12050000` + AXP15060 PMIC):** OpenSBI's `pm-reset` handler reboots the board by writing to the AXP15060 PMIC (`0x36` on `i2c5@12050000`), but U-Boot disables `i2c5` clocks before `booti`. `sbi_system_reset()` (`arch/riscv/common/sbi.c`) enables `clk_i2c5_apb` (`0x1302024C` bit 31) and deasserts `rstn_i2c5_apb` (`0x13020308` bit 12) in `SYS_CRG` before issuing the `SRST` ecall, ensuring `reboot` always performs a clean cold reset back to U-Boot.

---

## 4. MicroSD Controller (`sdio1@16020000`) & `/sd0/` VFS

The microSD slot is wired to `sdio1@16020000` (Synopsys DesignWare Mobile Storage Host Controller 2.90a, `VERID 0x5342290a`), driven by `drivers/sdmmc_jh7110.c`:
* **Clocking & Bus Width:** `SYS_CRG` `SDIO1_CLK_SDCARD` (`0x13020178`) supplies a 198 MHz CIU clock (`axi_cfg0 / 2`); `CMD6` switches the card to High Speed mode at **49.5 MHz** (`CLKDIV = 2`, 4-bit bus via `ACMD6`).
* **Transfers:** Deterministic 32-bit FIFO PIO at `SD_DATA` (`0x16020200`) with `CMD17`/`CMD18` (auto-stop) and `CMD24`, serialized through the `sdblk` driver task (`~2.25 MB/s`).
* **Diagnostics:** Run `sdinfo`, `sdbench 256`, `blkstats`, or `df` from `lsh`.

---

## 5. Gigabit Ethernet (`gmac1@16040000`) & Native IP Services

The VisionFive 2 features dual Gigabit Ethernet ports. LugalOS natively drives the primary LAN port (`gmac1@16040000`, Synopsys DesignWare EQOS v5.20 MAC with Motorcomm YT8531C RGMII-ID PHY at PHY address 1):

* **DMA Descriptors & SiFive L2 Cache Flushing:**
  * Uses 64-byte aligned descriptor rings (16 TX, 16 RX) in `.bss`.
  * Because the JH7110 EQOS DMA bus master does not participate in hardware cache coherency with the U74 L1/L2 caches, descriptors and packet buffers are flushed to DDR before kicking DMA doorbells (`DMA_CH0_TXDESC_TAIL_POINTER` / `DMA_CH0_RXDESC_TAIL_POINTER`), and invalidated before CPU reads, via the SiFive L2 Cache Controller `Flush64` register (`0x02010200`, `writeq(addr, 0x02010200)`).
* **Network Services & Daemons:**
  * `eth0`: Native network interface, auto-negotiates 1000BASE-T full-duplex.
  * **DHCP Client:** Automatically acquires IPv4 configuration at boot (`dhcp_start()`).
  * **Admin Web Server (`httpd`):** Serves the LugalOS admin web app and WebSocket 9P client on port 80 (`http://BOARD_IP/`).
  * **Spartan Server (`spartand`):** Serves Gemtext capsules from `/sd0/pub/` on port 300.
  * **Plan 9 File Server (`tcp_p9`):** Listens on port 564, allowing high-speed file synchronization and live filesystem mounts.

---

## 6. Unattended Headless SD Auto-Boot (`vf2_uEnv.txt`)

To boot LugalOS automatically on power-on without a UART cable and without touching the onboard QSPI flash:

1. Factory QSPI U-Boot evaluates `load_vf2_env` (`fatload mmc 1:3 ${scriptaddr} vf2_uEnv.txt`) on MMC device 1, partition 3.
2. LugalOS tools configure an MBR partition 3 alias pointing to the FAT32 partition (`mmc dev 1; mmc read 0x44000000 0 1; cp.b 0x440001be 0x440001de 0x10; mmc write 0x44000000 0 1`).
3. `/sd0/vf2_uEnv.txt` defines:
   ```text
   boot2=echo Booting LugalOS from SD...; fatload mmc 1:3 0x40200000 lugalos.bin; booti 0x40200000 - ${fdtcontroladdr}
   ```
4. On power-on, after the 2-second countdown, U-Boot loads `vf2_uEnv.txt` from partition 3, runs `boot2`, and loads `/sd0/lugalos.bin` directly into DRAM, booting LugalOS to the `lsh>` shell and starting network services in under 4 seconds.
5. To enable autoboot with a single command:
   ```bash
   ./tools/vf2run.py --enable-autoboot --save-sd --kernel build/visionfive2/lugalos.bin
   ```

---

## 7. Build, Boot, Storage Sync & HIL Testing Workflows

### Building the Kernel
```bash
cmake --preset visionfive2
ninja -C build/visionfive2
```

### Loading & Running via `tools/vf2run.py`
```bash
# Upload build/visionfive2/lugalos.bin via 1.5 Mbps YMODEM, save to /sd0/lugalos.bin, and run commands
./tools/vf2run.py --kernel build/visionfive2 --save-sd --cmd "uname; ps; df" --reboot

# Instant boot (<100 ms) from /sd0/lugalos.bin on the microSD card
./tools/vf2run.py --method sd --cmd "sdinfo; sdbench 256; smptest" --reboot

# Configure unattended SD auto-boot and persist kernel
./tools/vf2run.py --enable-autoboot --save-sd --kernel build/visionfive2/lugalos.bin
```

### Synchronizing Files over 9P (`tools/p9sync.py`)
While LugalOS is running at the `lsh>` prompt:
```bash
# Gigabit Ethernet TCP 9P sync (~1 second for entire SD image)
python3 tools/p9sync.py --tcp BOARD_IP --key KEY_HEX --src build/visionfive2/sd_root --dst /sd0

# Fallback UART SLIP sync
uv run --project host/p9lib python3 tools/p9sync.py \
    --vf2 \
    --src build/visionfive2/sd_root \
    --dst /sd0
```

### Hardware-in-the-Loop Test Suite
```bash
python3 tests/hw/test_visionfive2.py
```

---

**Navigation:** [<- Board Reference Guides](boards.md) | [Administrator Manual](admin.md) | [Documentation Index](index.md)
