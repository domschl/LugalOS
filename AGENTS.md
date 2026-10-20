# AGENTS.md — LugalOS AI Developer & Agent Guide

This document is the reference guide for AI pair programmers, autonomous agents, and subagents working in the LugalOS repository. It describes the environment, hardware targets, documentation resources, toolchains, build presets, hardware flashing protocols, 9P filesystem synchronization, and testing procedures.

---

## 1. System & Environment

* **Host Environment:** Linux (x86_64).
* **Repository & Resource Roots:**
  * LugalOS and external platform SDKs may reside under either `~/gith/` or `~/Source/gith/`:
    * Repository Root: `~/gith/domschl/lugalos` or `~/Source/gith/domschl/lugalos`
    * Pico Resources: `~/gith/pico` or `~/Source/gith/pico`
    * ESP Resources: `~/gith/esp` or `~/Source/gith/esp`
  * Agents should dynamically check both roots when resolving file paths.
* **Compilers & Toolchains:**
  * 32-bit RISC-V: `riscv32-unknown-elf-gcc`
  * 64-bit RISC-V: `riscv64-unknown-elf-gcc`
  * Build System: `cmake` with `ninja` generator.
  * Python: Python 3.11+, managed via `uv`.
    * Default type checker: `pyrefly`.
    * Type hints: Always use modern typing syntax (`| None`, `list`, `dict`; never legacy `Optional`, `List`, `Dict`).
* **Critical Architectural Rules:**
  * **Freestanding C:** Built with `-nostdlib -ffreestanding`.
  * **Zero Floating-Point in Kernel / M-Mode:** The RISC-V boot code intentionally boots with `mstatus.FS = 0` (FPU disabled). Hardware float instructions or compiler soft-float `libm` calls will trigger **Illegal Instruction** traps. All mathematical routines must use pure fixed-point (e.g. scale $10^6$ for transcendental functions) or exact integer/bignum/rational arithmetic.
  * **Zero SRAM Overhead:** Optional features, symbols, and builtins must be stored in Flash `.rodata` tables sorted in ASCII order for $O(\log N)$ `bsearch()`.
  * **Web Architecture:** Simple HTML, vanilla JavaScript, and CSS. Avoid third-party web frameworks and heavy dependencies. For bidirectional real-time communication, prefer WebSockets + JSON.

---

## 2. External Hardware Resources & Documentation

Reference documentation and vendor SDKs are located in `~/gith/` or `~/Source/gith/`:

### RP2350 (Raspberry Pi Pico 2)
* **Path:** `~/gith/pico/` or `~/Source/gith/pico/`
* **Datasheets & Hardware Docs:** `${PICO_ROOT}/datasheet/`
  * Contains RP2350 datasheet, architecture details, Hazard3 dual-core RISC-V specs, and pinouts.
* **Official SDK:** `${PICO_ROOT}/pico-sdk/`
* **Flashing Tool (`picotool`):**
  * Binary path: `${PICO_ROOT}/pico-sdk-tools/picotool/picotool` (or discoverable via `find ~/gith/pico ~/Source/gith/pico -name picotool -type f`).
  * Use this binary to query or flash connected RP2350 boards via USB.

### ESP32-P4 (Espressif ESP32-P4-NANO)
* **Path:** `~/gith/esp/` or `~/Source/gith/esp/`
* **Datasheets & Hardware Docs:** `${ESP_ROOT}/datasheet/`
  * Technical reference manual, memory map, peripheral registers, and pinouts.
* **ESP-IDF & Platform:** `${ESP_ROOT}/esp-idf/`, `${ESP_ROOT}/ESP32-P4-Platform/`
* **Flashing Tools:** `${ESP_ROOT}/esptool/`
* **Reference Factory Flash Backup:** `${ESP_ROOT}/p4nano-factory-flash/`

### Environmental Sensors Datasheets
* **Path:** `~/gith/sensors/` or `~/Source/gith/sensors/`
  * Contains official component datasheets for all supported environmental and light sensors:
    * BME280: `bst-bme280-ds002.pdf`
    * BME680: `bst-bme680-ds001.pdf`
    * CCS811: `CCS811_Datasheet-DS000459.pdf`
    * HDC1080: `hdc1080.pdf`
    * MCP9808: `MCP9808.pdf`
    * MH-Z19B: `mh-z19b-co2-ver1_0.pdf`
    * MiCS-6814: `MiCS-6814_Datasheet.pdf` & `'Multichannel gas sensor 1.0.pdf'`
    * SGP30: `Sensirion_Gas_Sensors_Datasheet_SGP30.pdf`
    * TMP117: `tmp117.pdf`
    * TSL2561: `TSL2561.pdf`
    * TSL2591: `TSL25911_Datasheet_EN_v1.pdf`


---

## 3. Connected Hardware Targets & TTY Port Mappings

> [!IMPORTANT]
> **Dynamic Port Numbers & Plug-in Order:**
> Linux device node numbers (`/dev/ttyACM*`, `/dev/ttyUSB*`) **depend entirely on the order hardware is plugged in or enumerated by the kernel**.
> Agents must **never assume fixed port indices** (e.g. assuming `/dev/ttyACM0` is always a specific interface).
> Always identify ports by inspecting `/dev/serial/by-id/` or using the auto-detection logic in `tools/p4run.py`, `tools/p4flash.py`, and `tools/p9sync.py`.

### Port Topologies

#### 1. RP2350 Hardware (e.g. `rp2350-terminal`, `rp2350-chess`)
RP2350 devices running LugalOS expose **two CDC-ACM USB serial ports**:
* **Interface 0 (`if00`):** Interactive `lsh` shell & Lisp REPL console (115200 baud).
  * Symlink: `/dev/serial/by-id/usb-LugalOS_*_Dual_CDC_ACM_*-if00`
* **Interface 2 (`if02`):** USB CDC network / 9P file server link (`link_usb_cdc`).
  * Symlink: `/dev/serial/by-id/usb-LugalOS_*_Dual_CDC_ACM_*-if02`

#### 2. ESP32-P4-NANO (`esp32p4`)
ESP32-P4-NANO connects via **two separate serial bridges**:
* **CP2102 Bridge (`/dev/ttyUSB*`):** Connected to ESP32-P4 UART0 (interactive console, **921600 baud** since 47.7; `tools/p4run.py` `BAUD`).
  * Symlink: `/dev/serial/by-id/usb-Silicon_Labs_CP2102_*-port0`
* **CH34x Bridge (`/dev/ttyACM*`):** Connected to hardware control lines:
  * `RTS` controls `ESP_EN` (hardware chip reset).
  * `DTR` controls `GPIO35/BOOT` (download boot mode).
  * Symlink: `/dev/serial/by-id/usb-1a86_USB_Single_Serial_*-if00`
  > [!WARNING]
  > Opening the CP2102 or CH34x adapter with naive terminal utilities (such as `cu`, `screen`, or raw serial open without careful modem control) can inadvertently assert RTS/DTR and hold the ESP32-P4 in permanent reset.
  > **ALWAYS use `tools/p4run.py` or `tools/p4flash.py`** to communicate with or flash the ESP32-P4.

#### 3. ESP32-C6-Zero (`esp32c6`, phase 45)
One USB-C cable, one port: the chip's native USB-Serial/JTAG (VID:PID `303a:1001`),
console, loading and reset all on it.
* Symlink: `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00`
* **Always go through `tools/c6run.py` / `tools/c6flash.py`**, which open the port with
  DTR high / RTS low — opening with DTR low resets the chip (plan/phase45 §45.1).
* The kernel is **two images**: `.text`/`.rodata` execute in place from flash at
  `0x42000000`, the rest lives in SRAM. The map is `build/esp32c6/flash.manifest`
  (from `cmake/flash_layout_esp32c6.cmake`), never typed on a command line.
* Development loop (nothing written but the flash half, and only when it changed):
  ```bash
  ninja -C build/esp32c6
  tools/c6run.py --kernel build/esp32c6 --cmd "preempttest" --cmd-wait 8
  ```
* Leave it running by itself (the loader at `0x0`, the boot records erased, the build in slot A,
  verified, then reset): `tools/c6flash.py build/esp32c6`. After that, new builds go over 9P
  into the other slot, boot on trial and confirm or fall back by themselves
  (`tools/lota.py push`, doc/admin/updates.md); the loader prints `[loader] slot X, BUILD` on UART0.
* Test it: `cd tests/hw && uv run test_esp32c6.py --port <C6-Zero>` (loads the kernel itself;
  `--no-load` tests what is running). 21 checks (with WLAN credentials in the identity record the
  board joins by itself at boot and the radio-start tests watch that instead); skips when no board
  is attached. The last check (49.3) arms LLP's radio role and stays in it until reset. Wi-Fi checks read
  `~/.config/lugalos/wifi.env` (never print it; only the derived 64-hex PSK goes to the board).
* The C6-MINI beside the P4 on the NANO (49.5): preset `esp32c6-mini` (4 MB map:
  `cmake/flash_layout_esp32c6_mini.cmake`), console on the C6's own USB through header P2
  pins 21/23. Pick boards by name: `--board zero|nano` on `c6run.py`, `c6flash.py` and
  `test_esp32c6.py` (from `~/.config/lugalos/c6-ports.env`); e.g.
  `tools/c6flash.py build/esp32c6-mini --board nano`,
  `cd tests/hw && uv run test_esp32c6.py --board nano --build ../../build/esp32c6-mini`.
* The C6's console is also copied to its UART0 (115200; NANO header P2 pin 22): when the USB console
  is silent (a hang, early boot), that copy still shows what the kernel says. Silent-boot breadcrumbs
  in LP_AON (read with esptool `read-mem`): 0x600B1000 stage, 0x600B1004 ticks, 0x600B1008/0x600B100C
  last trap cause/pc, 0x600B1014/0x600B1018 ra/task at the last tick.
* `reboot` restarts a C6; `reboot download` restarts it into the ROM's download mode (no BOOT
  strap needed; then esptool `--before no-reset`). That sets bit 30 of LP_AON_SYS_CFG
  (0x600B1034), which survives every reset but a power cycle or CHIP_PU: a plain `reboot` and
  `c6flash.py` clear it; by hand, esptool `write-mem 0x600B1034 0 0x40000000` with its default
  reset before and after. An esptool session opened with `--before no-reset` restarts the chip
  into download mode again whatever the bit says (DTR stays asserted: the USB-JTAG's BOOT strap).
* LLP radio role (49.3): `llp radio` at the C6's console shares the USB port between the console
  and LLP frames; `cd host/llp && uv run --extra serial --extra p9 llp host --port <C6-Zero> --arm
  --dhcp --ping 3 --cat /proc/version` plays the host. With the NANO's C6 also on USB (P2 21/23),
  two Espressif ports exist: always pass `--port`.
* I2C on the C6-Zero: GPIO0 = SDA, GPIO1 = SCL (100 kHz); `i2c scan`, `sensor`, `/proc/sensors`.
* As the RP2350 terminal's Wi-Fi (49.10): the C6-Zero on the terminal's header H7 (UART0,
  crossed TX/RX, 921600 baud, LLP), both ends starting at boot; a C6 that boots with no USB
  host arms LLP on its UART0 by itself. Two supplies: the C6 on a USB charger, only GND/TX/RX
  to H7 (H7's 3V3 cannot carry the C6's transmit bursts); doc/admin/networking.md §5.
* The hardware watchdog (MWDT0, 30 s) is armed at boot; `wdt` shows it and the last reset's
  cause, `wdt test` stops feeding it (the board resets -- only when that is the point).
* A cold chip and a warm one differ: PMU/LP registers survive a chip reset, so radio
  bring-up changes must be checked after a USB power cycle (ask the user to unplug ~10 s).
* A known-good reference exists: ESP-IDF's toolchain is installed (`~/.espressif`); its
  `examples/wifi/getting_started/station` builds for the board, for register diffs.
* The top 16 KB of SRAM (`0x4087c000`..) is the **ROM's data** — never hand it out; the
  ROM's Wi-Fi code keeps its state there.
* Quick standalone checks (no kernel): `tools/build_minimal_esp32c6.sh run`,
  `tools/build_umode_probe_esp32c6.sh run`.

#### 4. ESP32-P4-WIFI6-Touch-LCD-7B (`esp32p4-lcd7b`, phase 47)
One CH343P ("USB TO UART" Type-C) carries **both** UART0 (GPIO37/38) and reset
(RTS → ESP_EN, DTR → GPIO35). There is no CP2102, so `tools/p4run.py` picks it
for both roles by itself.
* Symlink: `/dev/serial/by-id/usb-1a86_USB_Single_Serial_*-if00`
* **Opening this port resets the chip** unless the tty was left at B0, because
  Linux raises DTR/RTS on open. `tools/p4run.py` parks the port at B0 whenever
  it closes it, so always go through `p4run.py` / `p4flash.py`. Any other
  program (picocom, a bare pyserial open) resets the board.
* Silicon **revision v3.2** (the NANO is v1.3): L2MEM's cache and ROM data
  sit at the opposite ends, and the CLIC is the standard one. A build for
  the wrong revision halts at boot with `[Chip] ... Halted.` See plan/phase47 §6.
* **With both P4 boards attached, always pass `--board nano|lcd7b`.** Two
  CH343Ps can only be told apart by serial number, so the mapping lives in
  `~/.config/lugalos/p4-ports.env` (`LUGALOS_P4_<BOARD>_PORT` /
  `_RESET_PORT`). Without `--board`, detection refuses rather than reset the
  wrong board.
* Build `ninja -C build/esp32p4-lcd7b`; flash with
  `tools/p4flash.py --board lcd7b --build build/esp32p4-lcd7b --only boot,os --verify`;
  console with `tools/p4run.py --board lcd7b --run --cmd "..."`.
* Runs at 400 MHz (v3 ladder 100/200/400) with the core on the external DC-DC.
* Console (both P4 boards): UART0 at **921600 baud** (`CONFIG_UART0_BAUD`, matched by
  `tools/p4run.py`'s `BAUD`). The ROM's first boot lines are at 115200 and arrive garbled.
* 32 MB PSRAM at 0x48000000 (both P4 boards, `drivers/psram_esp32p4.c`), 200 MHz
  hex DDR via IDF's DQS timing tuning at every boot (`CONFIG_PSRAM_SPEED_MHZ`;
  falls back to 20 and logs it if no window is found): `psram` (shows the tuning
  window), `psram test [MB]`, `psram bench`. BULK_BSS and the bulk
  page zone live there. Register constants come from the generated
  `drivers/include/drivers/esp32p4_psram_regs.h` (`tools/gen/p4_regs.py`), which
  refuses to generate if hw_ver1 and hw_ver3 disagree.
  I2C on GPIO7/8 (PH2.0 header): ES8311 0x18, ES7210 0x40, GT911 0x5D;
  `i2c rd ADDR [REG..] N` reads registers raw (e.g. `i2c rd 5d 81 40 4` → "911").
  A DS3231 RTC module (0x68, with its AT24C32 EEPROM at 0x57) sits on the 7B's
  PH2.0 I2C header too (2026-10-11): the kernel syncs from it at boot, and the
  menu bar shows HH:MM once the time is valid. A fresh module has OSF set and
  is ignored ("time is unverified") until `date YYYY-MM-DD HH:MM:SS` (local
  time) sets it, which also clears OSF.
* Display (47.4, `drivers/dsi_esp32p4.c`): EK79007 over 2-lane MIPI-DSI at
  1 Gbps. It comes up on the first `lcd pattern [bars|hbars|ber|off]`, not at
  boot. `lcd` reports the link, `lcd bl on|off` the backlight (GPIO32,
  active low), `lcd cmd CC [PP..]` sends a DCS write. DCS reads return 0
  (open, plan §6.4).
  The panel terminal is `drivers/lcdterm.c` (portable, 47.6); a panel driver
  only describes its 1-bpp frame to it (`lcdterm_backend_t`). Window widths
  come from the screen width (`RIBBON_COLS_*(w)` in `drivers/ribbon.h`, D7).
  On targets without a panel, `ramscreen W H` sets the RAM screen's size
  (QEMU tests layouts at 1024x600 with it).
  Since 47.7 the LCD-7B boots into the shell on its panel (126x34; console
  device `lcd`, tee to UART0, input still UART0); `lcd colours FG BG` (RGB565
  hex; a coloured pair switches the frame to RGB565), `lcd repaint` (after a
  test pattern), `lcd termbench`, `lcd tee uart|off`. The terminal draws from
  the kernel there (no U-mode lcdterm task on the P4 yet); a 16 ms `lcdflush`
  task converts changed 1-bpp rows into the frame.
  Frame buffer (47.5): `lcd fb [gray|rgb565]` starts a 1024×600 frame in
  PSRAM (GRAY8 = 600 KB, the chosen format), scanned out by the DW-GDMA at
  60 Hz and re-armed by its interrupt (CLIC 18). `lcd test
  checker|grid|text|ramp`, `lcd verify` (exact readback), `lcd fps`,
  `lcd bench`, `lcd wbtest` (cache coherence, needs eyes). Drawing must end
  with `dsi_lcd_fb_flush()`. Font glyph rows are LSB-first (bit 0 = left).
* Backlight (47.11): `lcd backlight [0-100]` (LEDC PWM on GPIO32, inverted;
  no argument reads the duty back), `lcd bl on|off`. Both panels:
  `lcd blank [min|Ns|off]` turns the backlight off after that long without
  input (default off). A USB-keyboard key that wakes it is swallowed; serial
  input wakes it and is kept.
* Touch (47.12, both the LCD-7B and the RP2350 touch terminal): GT911 at 0x5D,
  polled (`drivers/touch_gt911.c`). Tap a window to focus it, tap the menu
  bar's minimap to jump, swipe sideways to move the ribbon. `ts` shows it,
  `ts log [s]` prints presses and gestures. Orientation is in the board file
  (`CONFIG_TOUCH_MIRROR_X/_Y`; the 7B's are both set).
* USB host (47.9, `drivers/usbhost_esp32p4.c`): the Type-A port is the OTG HS
  core (DWC2 4.30a), polled, slave mode, one transaction per channel; it is
  a *port* for the shared keyboard host `drivers/usbkbd_host.c` (also the
  RP2350's, over PIO-USB). `usb` shows the controller, the port and the last
  device's descriptors; `usb regs` dumps registers for an IDF diff; `kbd`,
  `kbdlog [s]` as on the RP2350. Devices behind a high-speed hub are reached
  with split transactions through its TT (the owner's keyboard has a built-in
  USB 2.0 hub; it has a battery, and unplugging it browns the board out).
* Flash runs at IDF's 80 MHz QIO (stage 2, `xip_esp32p4.c`). Never call the
  ROM's `spi_flash_attach()` from the running kernel: it drops XIP to
  10 MHz single-line, and cache misses then hit the CPU's DBUS timeout
  (imprecise load access faults; plan §6.5). On an access fault, the
  `[Trap Cache]` line names the reason ("core timeout raw" bit 4 = DBUS
  timeout).
* SDIO toward the C6 (49.4): slot 1 of the SD/MMC host (GPIO14-19 via the matrix; C6 reset GPIO54,
  open-drain). `sdio probe [khz-hex]` (e.g. `sdio probe 2710` = 10 MHz), `sdio`, `sdio rd52 fn addr`,
  `sdio rd53 fn addr len`, `sdio bench [kb fn addr len]` (all hex), `sdio reset` (GPIO54), `sdio resync` (LLP's
  channel reset). Both boards run it at 40 MHz in SDIO high speed (the microSD stays at 20 MHz on its own divider
  of the shared 40 MHz base); the C6 raises the card interrupt and the P4's LLP pump waits for it.
  `llp soak [s] [size]` is the loopback bench as a background task (C6: `llp loop on`; results in `dmesg`).
  The C6's SDIO slave hardware answers whatever the C6 runs. Flash the P4 with both halves
  (`--only boot,os`): an `os`-only flash pairs a new XIP half with an old RAM half and traps at boot.
  With several CP2102s attached (the NANO console, the 7B's C6 on H4, the NANO's C6 UART0) their by-id
  links collide (all serial "0001"), so `p4-ports.env` and `c6-ports.env` name them by USB path
  (/dev/serial/by-path/...); check with a newline at 115200/921600 when the hub wiring changes.
  The 7B's other Type-C is its P4's USB-Serial/JTAG (flashable, but not the LugalOS console).
* The 7B's C6 runs LugalOS (`esp32c6-mini`; factory image backed up in
  `~/Source/gith/esp/p4lcd7b-c6-factory-flash/`). Its only console is UART0 on header H4
  "C6-UART" (SH1.0: 1 IO9, 2 GND, 3 U0RXD, 4 U0TXD) through a CP2102 at 115200:
  `tools/c6run.py --board lcd7b` / `c6flash.py --board lcd7b` (`c6-ports.env`). Update it over
  9P through the P4: `uv run tools/lota.py push build/esp32c6-mini --p4 lcd7b --prefix /c6
  --reboot --wait`. Through the header (the loader, or a C6 that does not boot): `reboot download`
  at its console, then `c6flash.py` (`--before` reset is harmless, the adapter has no DTR/RTS
  wired); afterwards `sdio reset` on the P4 boots it. A dead C6 needs
  H4 pin 1 to pin 2 and an `sdio reset`. That CP2102 reports serial 0001 like the NANO's console.
* LLP over SDIO starts at boot on the 7B (C6: radio role; P4: host role, no Ethernet on this
  board): link up, `wlan0`, `/c6` mounted, and the P4's `wlan` credentials handed to
  `/c6/dev/wlan`. `llp` shows both ends; `llp loop on` (C6) + `llp bench` (P4) measures the channel.
* Test: `cd tests/hw && uv run test_esp32p4.py --board lcd7b --build ../../build/esp32p4-lcd7b`
  (40/40 since 49.7's Wi-Fi and rejoin checks, which need this host's firewall to let the test's
  MQTT broker take a connection; 47.13: backlight, blanking, USB keyboard, touch, RTC and the
  ribbon's hotkeys included, each skipping where a board lacks it; EMAC tests
  skip; a BME280 sits on the PH2.0 I2C header at 0x76). NANO: `uv run
  test_esp32p4.py --board nano` (40/40, the 7B's skipping).

#### 5. M5Stack Tab5 (`esp32p4-tab5`, phase 56)
5" portable tablet with 720×1280 native portrait IPS display, capacitive touch, microSD, USB host,
32 MB PSRAM and 16 MB flash.
* Console & Flashing: single USB-C cable carries native **USB-Serial/JTAG** (VID:PID `303a:1001`,
  `GPIO24/25`). The kernel mirrors console TX/RX to both USB-Serial/JTAG and UART0 (`GPIO37/38` on M5-Bus).
  `tools/p4run.py --board tab5` and `tools/p4flash.py --board tab5` auto-detect the JTAG port by symlink.
* Dual `PI4IOE5V6408` I2C expanders at `0x43` and `0x44` on `SYS_I2C` (`GPIO31/32`): power sequencing,
  hardware reset for LCD/Touch, and USB 5V VBUS enable (`U7` bit 3).
* I2C parts on `SYS_I2C` are declared in the board file (`CONFIG_I2C0_PARTS`); `i2c scan` names them and
  sensor/RTC probes skip them (`i2c_probe_begin`, drivers/i2c_bus.h). 0x68 is the **BMI270**, not an RTC.
  0x28 is a second address of the ST7123 touch/display chip (gone while TP_RST is low); leave it alone.
  Holding TP_RST low also blanks the panel (TDDI: one chip); reset the board afterwards.
* RTC: the board's **RX8130CE at 0x32** (`CONFIG_RTC_RX8130`, backup cell), read at boot and set by `date`/NTP.
  `reboot` is not supported on the P4; reset with `tools/p4run.py --board tab5 --run`.
* Battery: 2S pack on the INA226 (0x41, 5 mOhm, sign inverted so + = charging); `power`, `cat /dev/power`,
  `power log FILE [s]`; charge estimate in drivers/battery.c (board facts `CONFIG_BATT_*`).
  Charger (IP2326 via 0x44: P7 CHG_EN, P5 nQC): `power charge off|slow|fast`, `power limit N|off`
  (stored in the identity record); `power off` pulses PWROFF (P4) and switches the board off --
  never run it on a board you need to keep talking to. CHG_STAT (P6) reads 1 whether charging or not.
* IMU: BMI270 at 0x68, needs Bosch's microcode uploaded (drivers/bmi270_config.c, ~1.3 s, first read
  on the sampler task); `imu`, `imu watch`. The i2c task serves one caller at a time and queues nobody:
  a client that issues long transfers back to back must pause between them or it starves the others.
* Display rotation: all four of 0/90/180/270 (`lcd orient`); `orient auto|off` turns it with the IMU
  (kernel/orient.c, mapping `CONFIG_IMU_ROT_*` measured: +x 270, -x 90, +y 0, -y 180). ksnprintf() is
  re-entrant now (it was one global context, and two tasks formatting at once corrupted a stack).
* Grove port A is a second I2C bus (I2C1, GPIO53 SDA / GPIO54 SCL, `i2c scan 1`); external sensors go there.
* Display (`drivers/dsi_esp32p4.c`): 5" 720×1280 MIPI-DSI ST7121 TDDI, 2-lane @ 900 Mbps, DPI clock
  70 MHz, double-buffered RGB565 PSRAM scanout at 58 Hz. Backlight active-high LEDC PWM on `GPIO22`.
* Dual-Orientation Compositor: supports **Landscape Normal LugalOS GUI (`1280×720`, 160×45 cells)**
  with 90°/270° hardware blitting, and **Portrait Full-Screen Gemtext Reader (`720×1280`, 90×80 cells)**.
  Dynamic orientation toggle via `lcd orient [portrait|landscape]` or `'o'` in `reader`.
* Capacitive Touch: ST7121 at `0x55` (and GT911 at `0x5D`), polled every 20 ms with orientation-aware
  coordinate remapping.
* MicroSD: 4-bit SDMMC Slot 0 (`GPIO39–44`) mounted on `/sd0/` at 20 MHz.
* USB Host: DWC2 OTG HS on Type-A port with `USB5V_EN` via PI4IOE2.
* SDIO Wi-Fi Co-Processor (ESP32-C6, phase 52): connected via SDMMC Slot 1 (CLK 12, CMD 13, D0–D3 11/10/9/8); C6 reset on GPIO15 (active low, `CONFIG_C6_CHIP_PU_GPIO`; M5Stack's esp-hosted config uses the same pin).
  * Closed appliance without external C6 serial/JTAG headers. Boots LugalOS via factory ESP-IDF bootloader from `ota_1` partition (`layout=idf`).
  * Bootloader locks PMP entries 0–2 and 15 (`PMP_L`); LugalOS kernel probes locked entries at boot (`arch/riscv/common/mem_domain.c`) and dynamically allocates user domains in unlocked slots 3–14 without trap.
  * Over-the-air updates for the C6 are performed over 9P/SDIO without opening the case:
    ```bash
    uv run tools/lota.py push build/esp32c6-mini --p4 tab5 --prefix /c6 --reboot --wait
    ```
  * Boot-time Wi-Fi & DHCP: P4 boots into LLP host role, discovers C6 on SDIO, mounts `/c6`, automatically pushes credentials from `/sd0/etc/wlan.env` or identity record to `/c6/dev/wlan`, registers `wlan0`, and arms DHCP client (`dhcpc`) to acquire network address automatically.
* Build & Flash:
  ```bash
  ninja -C build/esp32p4-tab5
  python3 tools/p4flash.py --board tab5 --build build/esp32p4-tab5 --only boot,os --verify
  python3 tools/p4run.py --board tab5 --run
  ```
* Test: `cd tests/hw && uv run test_esp32p4.py --board tab5 --build ../../build/esp32p4-tab5` (46/46 passed).

---

## 4. Hardware Presets & Agent Requests

LugalOS supports distinct hardware presets. If a task requires testing against a specific board, the agent can request the user to attach or plug in hardware conforming to the required preset:

| Preset Name | Target Architecture | Description |
| :--- | :--- | :--- |
| `rp2350-terminal` | RP2350 Hazard3 RV32 | Waveshare RP2350-LCD-7 (7" LCD terminal, PSRAM, USB console, SD card) |
| `rp2350-chess` | RP2350 Hazard3 RV32 | Pico 2 with ST7735 LCD, TM1638 keypad, SD card, chess engine |
| `esp32p4` | ESP32-P4 RV32 | Waveshare ESP32-P4-NANO (dual-core RISC-V @ 360–400 MHz, RMII Ethernet, SDMMC, writable flash) |
| `esp32p4-lcd7b` | ESP32-P4 RV32 (rev v3.2) | Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (7" 1024×600 MIPI-DSI, GT911 touch, USB-A host, SDMMC, 32 MB PSRAM, C6 on SDIO; phase 47) |
| `esp32p4-tab5` | ESP32-P4 RV32 (rev v1.3) | M5Stack Tab5 (5" 720×1280 MIPI-DSI ST7121 touch, USB-A host, SDMMC, 32 MB PSRAM, dual PI4IOE expanders; phase 56) |
| `rp2350-clock` | RP2350 LED Clock | Waveshare Pico-Clock-Green (SM16106 LED matrix + DCF77 radio) |
| `rp2350-gateway` | RP2350 Gateway | Network gateway persona (ENC28J60 Ethernet / USB 9P / UART1 downlink) |
| `rp2350-wifi` | RP2350 CYW43439 | Raspberry Pi Pico 2 W with CYW43439 Wi-Fi |
| `esp32c6` | ESP32-C6 RV32 | Waveshare ESP32-C6-Zero (RV32IMAC @ 160 MHz, 512 KB SRAM, 8 MB flash, USB-Serial/JTAG console) |
| `rv32` | RISC-V 32-bit NOMMU | QEMU virt machine (32-bit microkernel test target) |
| `rv64` | RISC-V 64-bit Sv39 MMU | QEMU virt machine (64-bit virtual memory target) |
| `rv64-smp` | RISC-V 64-bit SMP | Dual-core SMP virtual machine |

To build any target:
```bash
ninja -C build/<preset>
```

---

## 5. Flashing & Storage Synchronization Procedures

### Flashing RP2350 Targets (e.g. `rp2350-terminal`)
1. **Build the Preset:**
   ```bash
   ninja -C build/rp2350-terminal
   ```
   > [!CAUTION]
   > Always flash `build/rp2350-terminal/lugalos.uf2` for the terminal board. Flashing the generic `rp2350` build will leave the LCD uninitialized (blank/black).

2. **Reboot to BOOTSEL via 1200-baud Touch:**
   LugalOS USB CDC stack implements the 1200-baud touch protocol on the console port (`if00`). Opening and immediately closing the console port at 1200 baud arms a deferred bootrom reset directly into BOOTSEL mode:
   ```python
   import serial
   with serial.Serial("/dev/ttyACM1", 1200):
       pass
   ```
3. **Flash using `picotool`:**
   ```bash
   /home/dsc/gith/pico/pico-sdk-tools/picotool/picotool load -f build/rp2350-terminal/lugalos.uf2 -x
   ```
4. **Manual BOOTSEL Fallback:**
   If the running firmware is unresponsive or in a fault state, ask the user for a manual BOOTSEL button press, or copy `build/rp2350-terminal/lugalos.uf2` to the mounted mass-storage volume (`RPI-RP2`).

### Updating the MicroSD Card via 9P (`tools/p9sync.py`)
To update files on the board's microSD card (`/sd0`) without removing the physical card or interrupting console sessions, use the LugalOS 9P synchronization utility:
```bash
uv run --project host/p9lib python3 tools/p9sync.py \
    --serial /dev/ttyACM2 \
    --src build/rp2350-terminal/sd_root \
    --dst /sd0
```
* **ESP32-P4 boards** (one console wire, no 9P port): `tools/p9sync.py --p4 lcd7b
  --src build/esp32p4-lcd7b/sd_root` (or `--p4 nano`) puts 9P on the console with
  `p9share`, syncs over SLIP and turns it off again, opening the port only the way
  `p4run.py` does (a plain pyserial open or close resets the LCD-7B).
* **Preservation Policy:** `tools/p9sync.py` is additive and idempotent. It creates missing directories (e.g. `/sd0/cas/`), updates files that differ in size, and **strictly preserves user files** (such as `/sd0/CHESS/` games and `/sd0/SYSTEM/HISTORY.LIS`).

### Flashing & Running ESP32-P4-NANO
1. **Build the Preset:**
   ```bash
   ninja -C build/esp32p4
   ```
2. **Flash Second-Stage Bootloader and XIP OS Image:**
   ```bash
   python3 tools/p4flash.py --only boot,os --verify
   ```
3. **Run Interactive Console & Monitor:**
   ```bash
   python3 tools/p4run.py
   ```

---

## 6. Testing Framework & Scripts

### Quick Standalone Unit Tests
* **Phase 43.1 Math Foundations:**
  ```bash
  python3 tests/test_lisp_cas.py
  ```
  Verifies exact `expt`, `^`, `isqrt`, `to-decimal`, `symbol<?`, `string<?`, `math-sin`, `math-cos`, etc.
* **Phase 43.2 CAS Simplifier:**
  ```bash
  python3 tests/test_cas_simplify.py
  ```
  Verifies canonical AST forms, like-term collection, exact constant folding, identities, radicals, and ordering in pure Lisp.

### Comprehensive Test Runner
* **Full Automated Test Suite:**
  ```bash
  python3 tests/runner.py
  ```
  Runs host harnesses (ASan/UBSan FAT32, cc, 9P, IP) and automated QEMU regression tests across RV64, RV32, networking, filesystem, and Lisp engine.

### Hardware-in-the-Loop Test Suites
Located in `tests/hw/` (safe to run speculatively; skips rather than fails when hardware is absent):
* `tests/hw/test_rp2350.py`: Silicon verification suite for RP2350 peripherals, memory pools, and console.
* `tests/hw/test_esp32p4.py`: Comprehensive silicon test suite for ESP32-P4 CLIC, PMP, I2C, and SDMMC.
* `tests/hw/test_wifi.py`: CYW43 Wi-Fi and network interface testing.
* `tests/hw/lcdterm_shots.py` (47.6): the panel terminal's regression check on the
  RP2350 terminal -- a fixed shell scenario, 8 PBM screenshots fetched over 9P;
  `--out DIR` records, `--compare OLDDIR` diffs pixel by pixel (clock masked).
  Record before a change to the terminal/screen/ribbon code, compare after.
  `/sd0` keeps 8.3 names only -- long names sharing a prefix collide.
  47.8: `--scenario ribbon` plays phase 44's multi-terminal / multi-canvas
  checks (two terminals, rule 30 + rule 90, chess, canvas close/reopen, moves,
  Lorenz, a CAS plot) with the keyboard's own Cmd sequences over the console,
  checks the chess board survives navigation pixel for pixel and that exits
  leave one window; `--board lcd7b` runs either scenario on the LCD-7B
  (needs `/sd0/demos` and `/sd0/cas`: sync them with `p9sync.py --p4 lcd7b`).
* From a serial line the Cmd hotkeys are `ESC [ 1 ; 9 C/D` (Cmd+Right/Left,
  `;13` adds Ctrl) and `ESC [ cp ; 9 u` (Cmd+character, e.g. Cmd+1 is
  `ESC [ 49 ; 9 u`) -- kernel/keyseq.h.
* With an ESP32-P4-NANO attached, run `test_rp2350.py --no-uart`: its CP2102 is
  the NANO's console, not an RP2350 UART dongle.

---

## 7. Lisp & CAS Architectural Notes

* **Interpreter Dialect:** LugalOS Lisp is a compact Scheme-like Lisp.
  * Supported Special Forms: `quote`, `quasiquote`, `unquote`, `unquote-splicing`, `lambda`, `if`, `cond`, `let`, `let*`, `while`, `and`, `or`, `define`, `set!`.
  * **Recursion:** Use named `let` (`(let loop ((x 0)) ... (loop (+ x 1)))`) for tail-recursive loops. `letrec` is not built-in.
  * **File I/O:** `prim_load` and `prim_read_file` use `vfs_stat` to allocate scratch buffers dynamically according to the file size.
* **Computer Algebra System (`cas/`):**
  * Stored in `cas/` in the repository root.
  * Automatically staged by CMake into `SD_STAGE/cas` and embedded in `lugalos_sd.img` and `flashfs.bin` as `/sd0/cas` and `/flash0/cas`.
  * Core entry point: `(load "/sd0/cas/simplify.lisp")` followed by `(simplify expr)`.
  * For full documentation, see [`doc/user/cas.md`](file:///home/dsc/Source/gith/domschl/lugalos/doc/user/cas.md).

---

## 8. Documentation System & Code Commentary Rules

LugalOS maintains a unified documentation architecture under `doc/` and strict rules for source code comments. Agents modifying or creating code must adhere to these policies:

### Detailed Topic Documentation Pointers
* **User Manual:** [`doc/user/`](file:///home/dsc/Source/gith/domschl/lugalos/doc/user/) — Shell commands, editors (`e`, `ed`), ribbon window manager, chess engine, CAS, sensor telemetry.
* **Administrator Manual:** [`doc/admin/`](file:///home/dsc/Source/gith/domschl/lugalos/doc/admin/) — Hardware presets, flashing protocols, serial console baud rates, 9P networking, authentication, FAT32 storage.
* **Developer Manual:** [`doc/dev/`](file:///home/dsc/Source/gith/domschl/lugalos/doc/dev/) — On-device C & Lisp programming, microkernel IPC, memory domains, scheduler & SMP lock hand-off, tokenized dictionary logging, testing guide.
* **Architecture Decision Records (ADRs):** [`doc/history/adr/`](file:///home/dsc/Source/gith/domschl/lugalos/doc/history/adr/) — Durable architectural decisions.

### Source Code Commentary Standard
Source code comments must have a high signal-to-noise ratio:
1. **What to Include in Comments:**
   * **Invariants & Contracts:** Preconditions, postconditions, pointer nullability, and buffer ownership (who allocates and who frees).
   * **Hardware Quirks & Errata:** Silicon errata, register timing delays, revision differences (e.g. ESP32-P4 v1.3 vs v3.2).
   * **Concurrency & Memory Isolation:** Which lock guards what, why a lock is held across context switches, PMP granularity and alignment rules.
   * **Ownership of static storage:** every writable static or global reached from more than one task or handler carries a verdict comment, on its line or in the comment just above (one comment covers a run of declarations up to a blank line): `single task: <who>`, `guarded by: <lock>`, `interrupt-shared: <protocol>`, or `accepted: <why>`. `tools/racescan.py --build build/<preset>` lists what is unmarked (`--why CONTEXT FUNC` shows how a context reaches a function), and `tests/runner.py` fails while anything is (`racescan.py --check` on rv32, rv64-smp, Tab5, RP2350 terminal and C6); a handler path that may block is cut off with `if (devirq_in_handler()) return;` (plan/phase63). Native shell commands do not hold the Lisp lock: a tool whose state is one set takes a `session_claim_t` (kernel/lock.h) so a second terminal is refused rather than sharing it.
   * **Driver tasks and waits:** call a busy endpoint with `chan_call_wait()` (kernel/chan.h: a FIFO turn with a deadline, `CHAN_WAIT_MS`), never a hand-written retry loop; a facade touches the hardware itself only when `driver_task_alive()` is false -- a failed call with the task alive is an error, not a fallback. A timed poll reads the clock *before* the condition (`time_poll_until_us()`, kernel/time.h), or a task preempted past the deadline fails on a stale reading. `i2cstats` counts I2C failures by cause; `chanqtest` checks the queue.
   * **Interrupt-shared state:** `irq_save()` keeps out the handler only on the calling hart. A word that a handler and a task (or tasks on two harts) both read-modify-write is guarded by a spinlock taken on both sides (`spin_lock_irqsave()` in the handler too), or on the RP2350 written through its atomic set/clear aliases (+0x2000/+0x3000). A handler holding such a lock wakes tasks only after releasing it (a spinlock is a leaf). Boards run one hart until `smpstart`; rv64-smp always runs two.
   * **Shared system registers and lock order:** a read-modify-write of a word that holds other drivers' bits (the P4's HP_SYS_CLKRST/LP_CLKRST clock gates and resets) goes through `sysreg_update()` (kernel/sysreg.h); on the RP2350 use the +0x2000/+0x3000 aliases. ylocks are taken in one order -- the kernel learns it and reports an inversion; `lockcheck [pairs]` shows the order, any inversions and `console_lock` hold times (holds over 100 ms need a reason in a comment). On the P4 panel, frame-side state is under `g_frame_lock`, always taken after `console_lock` (`frame_begin()`).
   * **Non-Obvious Algorithms:** Mathematical derivations, pure fixed-point scaling rationale.
2. **What NEVER to Include in Comments:**
   * **No Milestone or Phase References:** Never write `/* M3, plan/phase12... */` or `/* Phase 48 */`.
   * **No Historical Narratives / Archaeology:** Never document obsolete shims ("Replaces the pre-B2 shim which did nothing...").
   * **No Changelogs in Code:** Avoid "Fixed bug where X happened". Use git commit messages for history.
   * **No Dead Code:** Delete obsolete or commented-out code.

### Documentation as Code (Agent PR Rules)
* Whenever adding or altering shell commands, update [`doc/user/shell.md`](file:///home/dsc/Source/gith/domschl/lugalos/doc/user/shell.md).
* Whenever adding or modifying Lisp primitives, update [`doc/dev/on_device/lisp_scripting.md`](file:///home/dsc/Source/gith/domschl/lugalos/doc/dev/on_device/lisp_scripting.md).
* Whenever updating documentation in `doc/`, run `python3 tools/gendocs.py --all` to regenerate on-device plain-text help files in `tools/sd_root/help/`.
* Run `python3 tools/gendocs.py --check` before committing to verify zero documentation drift.

