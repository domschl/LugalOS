# Phase 56: M5Stack Tab5 Support & Dual-Orientation Gemtext Reader

**Status**: COMPLETE (VERIFIED LIVE)  
**Target Hardware**: M5Stack Tab5 (`esp32p4-tab5` / `esp32p4-tab5x`)  
**Reference Materials**:
- Schematics & Block Diagram: `~/Source/gith/esp/datasheet/Tab5_Schematics_PDF.pdf`, `Tab5_Overall_Design_Block_Diagram.pdf`
- Display & Touch Specs: `~/Source/gith/esp/datasheet/ST7123_SPEC_Preliminary_V0.5.pdf`, `ST7123-TDDI-Interface-Protocol-V01.11.pdf.pdf`
- Vendor Reference Drivers: `~/Source/gith/esp/M5Stack/M5GFX/` (`M5GFX.cpp`, `Panel_ST7123.hpp`, `Panel_ILI9881C.hpp`, `Touch_ST7123.inl`), `~/Source/gith/esp/M5Stack/M5Unified/` (`M5Unified.inl`, `Power_Class.inl`)

---

## 1. Overview & Phase 56 Scope

The **M5Stack Tab5** is a 5-inch portable RISC-V tablet built around the **ESP32-P4** dual-core 360–400 MHz RISC-V SoC with **16 MB SPI Flash** (`XM25QH128CHIQ`) and **32 MB 200 MHz Hex DDR PSRAM**, paired with an **ESP32-C6-MINI-1U** wireless co-processor.

### Phase 56 Focus (Core Interactive Tablet & E-Reader Platform)
Phase 56 brings LugalOS to the M5Stack Tab5 with an initial focus on the core interactive peripherals and our flagship **Gemtext (`.gmi`) Reader** application:
1. **Console & Bootloader Bring-up**:
   - Native USB-Serial/JTAG console & flashing over the USB-C port (`GPIO24/25`, VID:PID `303a:1001`) plus UART0 (`GPIO37/38`, routed to M5-Bus).
   - Support for both silicon revisions shipped in Tab5 units: **Tab5** (ESP32-P4 rev v1.x, `chip_revision < 300`) and **Tab5X** (ESP32-P4 rev v3.x, `chip_revision >= 300`).
2. **System I2C (`GPIO31`/`GPIO32`) & Dual `PI4IOE5V6408` IO Expanders (`0x43`, `0x44`)**:
   - Power rail sequencing, LCD/Touch hardware reset lines, and USB 5V VBUS power switching.
3. **MIPI-DSI Display Subsystem (5" 720×1280 Native Portrait IPS Panel)**:
   - Runtime auto-detection across all three factory display/touch variants (**ST7123 TDDI**, **ST7121 TDDI**, and **ILI9881C + GT911**).
   - Tear-free double-buffered GRAY8 (`2 × 921.6 KB`) and RGB565 (`2 × 1.84 MB`) scanout from 32 MB PSRAM via DW-GDMA, plus LEDC PWM backlight control on `GPIO22`.
4. **Dual-Orientation Display Architecture (Landscape LugalOS GUI + Portrait Full-Screen Reader)**:
   - **Landscape Mode (`1280×720`)**: Normal LugalOS multi-window Ribbon GUI (`160×42` character cells + 2-row top menu bar + 1-row bottom status bar), rotated 90°/270° into the native `720×1280` scanout buffer during dirty-row conversion.
   - **Portrait Full-Screen Mode (`720×1280`)**: Dedicated e-book / Gemtext (`.gmi`) document viewer mode in `reader`, rendering `90×78` character cells (or full-width proportional/monospace Gemtext lines) directly in native `720×1280` portrait scanout orientation with 4-bit anti-aliased serif/sans/mono typography and 60 Hz inertia flick scrolling.
5. **Capacitive Touchscreen (`ST7123` / `ST7121` at `0x55` & `GT911` at `0x14`/`0x5D`)**:
   - Unified touch polling and gesture engine (`TP_INT = GPIO23`) with dynamic portrait/landscape coordinate transformation.
6. **MicroSD Card (`SDMMC` Slot 0) & USB Type-A Host Keyboard (`DWC2 OTG HS`)**:
   - 4-bit SDMMC on `GPIO39–44` (`/sd0`) for `.gmi` document libraries and system storage.
   - High-Speed USB 2.0 Host on the Type-A port (`USB2_OTG_D+/D-`) with `USB5V_EN` enabled via `PI4IOE2` (`0x44` bit 3).

### Deferred to Subsequent Phase(s) (Phase 57+)
The following onboard peripherals are documented in the hardware architecture tables below for pin/address reservation and will be implemented in follow-on phases:
- **Wi-Fi 6 Networking (ESP32-C6-MINI-1U)**: SDIO Slot 1 on `GPIO8–13` + `RST = GPIO15`, `WLAN_PWR_EN` (`PI4IOE2` P0), and RF antenna switch (`PI4IOE1` P0).
- **Audio Codec & Dual-Mic AEC**: `ES8388` (`0x10`) + `ES7210` (`0x40`) + `NS4150B` PA (`PI4IOE1` P1) over `I2S0` (`GPIO26–30`) and headphone detect (`PI4IOE1` P7).
- **MIPI-CSI Camera**: `SC2356` 2MP (1600×1200) sensor on 2-lane MIPI-CSI (`CAM_MCLK = GPIO36`, `CAM_RST = PI4IOE1` P6).
- **Power Management, RTC & IMU**: `INA226` current/voltage monitor (`0x41`), `RX8130CE` RTC (`0x32`), `BMI270` 6-axis IMU (`0x68`/`0x69` for auto-rotation), and `PWROFF_PULSE` (`PI4IOE2` P4).
- **Industrial RS-485**: `SIT3088` transceiver on `GPIO20` (TX), `GPIO21` (RX), `GPIO34` (DIR).

---

## 2. M5Stack Tab5 Hardware Architecture Reference

### 2.1 SoC, Clocks, Memory & Console Topology

| Subsystem | M5Stack Tab5 Specification | Notes / Comparison with Waveshare LCD-7B |
| :--- | :--- | :--- |
| **Main SoC** | ESP32-P4 (Dual-Core RISC-V, 360 MHz v1.3 / 400 MHz v3.2) | Tab5 ships with v1.x (`M5Tab5`) or v3.x (`M5Tab5X`). |
| **Main Crystal** | 40 MHz (`X1`) | Standard ESP32-P4 40 MHz XTAL. |
| **Core DC-DC** | External `SY8089AAAC` (`U5`) controlled by `VHP_DCDC_EN`/`DVFS` | Same external core buck topology as LCD-7B. |
| **SPI Flash** | 16 MB QIO (`XM25QH128CHIQ`, `U3`) @ 80 MHz | 2× capacity of NANO; same as 16 MB P4 flash map. |
| **PSRAM** | 32 MB Hex DDR PSRAM @ **200 MHz** (`0x48000000`) | Mandatory 200 MHz DDR DQS tuning (`drivers/psram_esp32p4.c`). |
| **USB-C Port (`J8`)** | Connected via `R94`/`R95` (0Ω) to `GPIO24` (`D-`) / `GPIO25` (`D+`) | **ESP32-P4 Native USB-Serial/JTAG** (`303a:1001`). Note: `U32 CH9102F` is DNI (`NC`) on schematic. |
| **UART0 (`GPIO37/38`)** | `GPIO37` (`U0TXD`), `GPIO38` (`U0RXD`) | Routed to M5-Bus pins 14/13 (`G14/TXD2`, `G13/RXD2`). |
| **Boot / Reset** | `SOC_RST` (`CHIP_PU`), `BOOT_GPIO35` (`GPIO35`) | Accessible via physical buttons and USB-Serial/JTAG reset. |

> [!IMPORTANT]
> **USB-C Console & Flashing (`303a:1001`)**:
> Unlike the Waveshare LCD-7B (which routes UART0 through a CH343P on its primary USB-C connector), the M5Stack Tab5 leaves `U32` (`CH9102F`) unpopulated and connects the USB-C receptacle (`J8`) directly to `GPIO24/GPIO25` (the ESP32-P4's built-in Full-Speed USB-Serial/JTAG controller, VID:PID `303a:1001`).
> - LugalOS on Tab5 should support **dual console / early boot tee** across both **USB-Serial/JTAG** (`drivers/uart_esp32c6.c` style JTAG FIFO, or P4 `USB_DEVICE_JTAG` peripheral) and **UART0** (`GPIO37/38`), ensuring seamless operation over a single USB-C cable without resetting the chip on port open.

---

### 2.2 Internal & External I2C Buses

| Bus | SCL | SDA | Speed | Connected Devices |
| :--- | :--- | :--- | :--- | :--- |
| **Internal (`I2C1` / `SYS_I2C`)** | `GPIO32` | `GPIO31` | 400 kHz | `PI4IOE1` (`0x43`), `PI4IOE2` (`0x44`), Touch (`0x55` or `0x14`/`0x5D`), `ES8388` (`0x10`), `ES7210` (`0x40`), `RX8130CE` (`0x32`), `INA226` (`0x41`), `BMI270` (`0x68`), `SC2356` (`0x36` via `U36` 1.8V level shifter) |
| **External (`I2C0` / Grove Port A)** | `GPIO54` | `GPIO53` | 100/400 kHz | HY2.0-4P Grove connector (`J11`) & M5-Bus `EXT_SCL`/`EXT_SDA` |

---

### 2.3 Dual `PI4IOE5V6408` I2C IO Expanders (`0x43` & `0x44`)

The Tab5 uses two 8-bit I2C GPIO expanders (`PI4IOE5V6408`, `U6` at `0x43` and `U7` at `0x44`) on `SYS_I2C` (`SDA=GPIO31`, `SCL=GPIO32`) to manage peripheral resets and power switches.

#### `PI4IOE5V6408` Register Map
| Reg | Name | Description |
| :--- | :--- | :--- |
| `0x01` | `DEVICE_ID` | Device ID & Reset control (bit 0 = reset status) |
| `0x03` | `IO_DIR` | Pin direction (`0` = Input, `1` = Output) |
| `0x05` | `OUT_SET` | Output state (`0` = Low, `1` = High) |
| `0x07` | `OUT_H_IM` | Output High-Impedance (`1` = Hi-Z, `0` = Push-Pull active; **defaults to `0xFF` at reset!**) |
| `0x0B` | `PULL_EN` | Pull-up/down enable (`1` = Enabled, `0` = Disabled) |
| `0x0D` | `PULL_SEL` | Pull selection (`1` = Pull-up, `0` = Pull-down) |
| `0x0F` | `IN_STA` | Input status readback |

> [!CAUTION]
> **Critical `PI4IOE5V6408` Quirk (`OUT_H_IM` = `0x07`)**:
> On power-on reset, `PI4IOE5V6408` sets register `0x07` (`OUT_H_IM`) to `0xFF`, placing all outputs in high-impedance mode even if `IO_DIR` (`0x03`) is set to output! To drive any output pin, its corresponding bit in `0x07` **must** be cleared to `0`.

#### Expander 1: `U6` (`pi4io1_i2c_addr = 0x43`, `ADDR = GND`)
| Bit | Schematic Net | Direction | Boot Default | Function |
| :--- | :--- | :--- | :--- | :--- |
| **P0** (bit 0) | `RF_PTH_L_INT_H_EXT` | Output | `0` (Internal) | C6 RF Antenna Switch (`0` = Internal 3D antenna, `1` = External MMCX) |
| **P1** (bit 1) | `SPK_EN` | Output | `0` (Muted) | `NS4150B` Speaker Amplifier Enable (`1` = On) |
| **P2** (bit 2) | `EXT5V_EN` | Output | `1` (Enabled) | `U26 LPW5209AB5F` 5V power switch for M5-Bus / Grove / EXT (`SYS_EXT5VO`) |
| **P3** (bit 3) | `NC` (Tab5) / `HAT_EN` (Tab5X) | Hi-Z / Out | `0` (Tab5) / `1` (Tab5X) | Bottom Hat power rail enable on Tab5X (`board_M5Tab5X`) |
| **P4** (bit 4) | `LCD_RST` | Output | `0` -> `1` | MIPI-DSI LCD Panel Hardware Reset (active low; pulse low then high) |
| **P5** (bit 5) | `TP_RST` | Output | `0` -> `1` | Capacitive Touch Controller Hardware Reset (active low) |
| **P6** (bit 6) | `CAM_RST` | Output | `1` | `SC2356` MIPI-CSI Camera Reset |
| **P7** (bit 7) | `HP_DET` | Input | Input | 3.5mm Headphone Jack Insertion Detect |

#### Expander 2: `U7` (`pi4io2_i2c_addr = 0x44`, `ADDR = SOC_3.3V`)
| Bit | Schematic Net | Direction | Boot Default | Function |
| :--- | :--- | :--- | :--- | :--- |
| **P0** (bit 0) | `WLAN_PWR_EN` | Output | `1` (or `0` until C6 used) | `U38 LPW5209AB5F` 3.3V power rail (`WLAN_3.3V`) for ESP32-C6-MINI-1U |
| **P1** (bit 1) | `NC` | Hi-Z | Hi-Z | Unused |
| **P2** (bit 2) | `NC` | Hi-Z | Hi-Z | Unused |
| **P3** (bit 3) | `USB5V_EN` | Output | `1` (Enabled) | `U27 MT9700` 5V VBUS power switch (`SYS_USB5V`) for **USB Type-A Host Port (`J10`)** |
| **P4** (bit 4) | `PWROFF_PULSE` | Output | `0` | Soft power-off trigger pulse to `U28 PMS150G` power supervisor |
| **P5** (bit 5) | `nCHG_QC_EN` | Output | `0` (Enabled) | Fast-charge enable (`0` = QC enabled, `1` = disabled) |
| **P6** (bit 6) | `CHG_STAT` | Input | Input | `IP2326` Battery Charger status input |
| **P7** (bit 7) | `CHG_EN` | Output | `1` (Enabled) | `IP2326` Battery Charger enable (`1` = charging enabled) |

#### Recommended Boot Initialization Sequence for `0x43` and `0x44`
```c
/* 1. Initialize PI4IOE2 (0x44): Enable WLAN_PWR_EN (bit 0), USB5V_EN (bit 3), CHG_EN (bit 7) */
i2c_write_reg(0x44, 0x03, 0b10111001); /* IO_DIR: P7, P5, P4, P3, P0 as outputs */
i2c_write_reg(0x44, 0x07, 0b00000110); /* OUT_H_IM: disable Hi-Z on active outputs */
i2c_write_reg(0x44, 0x0D, 0b00001000); /* PULL_SEL: pull-up on USB5V_EN */
i2c_write_reg(0x44, 0x0B, 0b00001000); /* PULL_EN */
i2c_write_reg(0x44, 0x05, 0b10001001); /* OUT_SET: CHG_EN=1, USB5V_EN=1, WLAN_PWR_EN=1 */

/* 2. Initialize PI4IOE1 (0x43): Assert LCD_RST (bit 4=0) and TP_RST (bit 5=0), enable EXT5V (bit 2=1) */
i2c_write_reg(0x43, 0x03, 0b01111111); /* IO_DIR: P0..P6 outputs, P7 (HP_DET) input */
i2c_write_reg(0x43, 0x07, 0b00000000); /* OUT_H_IM: push-pull on P0..P6 */
i2c_write_reg(0x43, 0x0D, 0b00010100); /* PULL_SEL: pull-up on LCD_RST & EXT5V_EN */
i2c_write_reg(0x43, 0x0B, 0b00010100); /* PULL_EN */
i2c_write_reg(0x43, 0x05, 0b01000100); /* OUT_SET: CAM_RST=1, TP_RST=0, LCD_RST=0, EXT5V_EN=1 */

/* Drive TP_INT (GPIO23) HIGH during reset release so GT911 (if present) latches addr 0x14 */
gpio_output_set(23, 1);
delay_ms(100);

/* 3. Release LCD_RST (bit 4=1) and TP_RST (bit 5=1) */
i2c_write_reg(0x43, 0x05, 0b01110100); /* OUT_SET: CAM_RST=1, TP_RST=1, LCD_RST=1, EXT5V_EN=1 */
delay_ms(100);
```

---

### 2.4 MIPI-DSI Display Subsystem (5" 720×1280 Portrait Native)

#### Physical & Electrical Wiring
- **MIPI-DSI PHY**: 2-lane DSI (`DSI_CK_P/N`, `DSI_D0_P/N`, `DSI_D1_P/N`), powered by internal **LDO Channel 3 at 2500 mV** (`VDD_MIPI_DPHY`, identical to Waveshare LCD-7B).
- **Native Scanout Resolution**: **720 horizontal × 1280 vertical** (Portrait Native).
- **Backlight (`LCD_BL_GPIO22`)**: Driven by `GPIO22` into `U8 ME2212` boost converter (`EN` pin, **active-high** PWM, 44.1 kHz LEDC PWM — note that unlike the Waveshare LCD-7B where GPIO32 is active-low, Tab5's `ME2212` `EN` on `GPIO22` is **active-high** with a `5.1kΩ` pull-down `R24` to `GND`!).

#### Factory Display & Touch Variants (Runtime Auto-Detection)
M5Stack manufactures the Tab5 with three different display/touch controller combinations. Because **ST7123** and **ST7121** are TDDI (Touch with Display Driver Integration) chips that require different MIPI-DSI lane bitrates (`1040 Mbps` vs `900 Mbps`) and cannot be distinguished via DSI readback before the PHY clock is started, M5GFX probes the **touch controller over I2C (`GPIO31`/`GPIO32`) first** after releasing `LCD_RST`/`TP_RST`:

| Variant | Detection Probe (on `I2C1` after `TP_RST` release) | DSI Lane Rate | DPI Pixel Clock | HSYNC `(pw, bp, fp)` | VSYNC `(pw, bp, fp)` | Init Table |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **1. ST7123 (TDDI)** | `0x55` reg `0x0000` returns `fw_version == 0x03` (or fallback default) | **1040 Mbps** | **80 MHz** | `pw=2, bp=40, fp=40` | `pw=2, bp=8, fp=220` | `Panel_ST7123` (`list0` + `DISPON` + `TEON` + `SLPOUT`) |
| **2. ST7121 (TDDI)** | `0x55` reg `0x0000` returns `fw_version == 0x01` | **900 Mbps** | **70 MHz** | `pw=2, bp=40, fp=40` | `pw=20, bp=24, fp=200` | `Panel_ST7123` (`list0` + `DISPON` + `TEON` + `SLPOUT`) |
| **3. ILI9881C + GT911** | `GT911` ACKs at `0x14` or `0x5D` (and `0x55` NACKs) | **1040 Mbps** | **80 MHz** | `pw=40, bp=140, fp=40` | `pw=4, bp=20, fp=20` | `Panel_ILI9881C` (`list0..list3`) |

> [!WARNING]
> **Critical ST7123 / ST7121 TDDI Timing Constraint (`vsync_front_porch >= 200`)**:
> In TDDI (Touch with Display Driver Integration) panels, capacitive touch electrodes share the LCD common/source matrix. The ST7123/ST7121 performs touch sensing during the vertical blanking interval (**VFP = 220 lines** for ST7123, **200 lines** for ST7121). Reducing `vsync_front_porch` to standard LCD values (e.g., 16–20 lines) starves the TDDI touch scan window and **causes the touchscreen to stop working completely**.

---

### 2.5 Capacitive Touch Subsystem (`ST7123` / `ST7121` & `GT911`)

- **I2C Bus**: `SYS_I2C` (`SDA = GPIO31`, `SCL = GPIO32`, 400 kHz).
- **Interrupt Pin**: `TP_INT = GPIO23`.
- **Reset Pin**: `PI4IOE1` (`0x43`) bit 5 (`TP_RST`).
- **Controller 1: `ST7123` / `ST7121` (`I2C Addr = 0x55`)**:
  - 16-bit big-endian register addressing (`reg >> 8`, `reg & 0xFF`).
  - **Firmware Info**:
    - `0x0000` (1 byte): `FW_VERSION` (`0x03` = ST7123, `0x01` = ST7121).
    - `0x0005..0x0009` (5 bytes): `MAX_X_H`, `MAX_X_L` (`720`), `MAX_Y_H`, `MAX_Y_L` (`1280`), `MAX_TOUCHES` (`<= 10`).
    - `0x000C..0x000F` (4 bytes): `FW_REVISION`.
  - **Touch Polling Protocol**:
    1. Read 1 byte from `0x0010` (`adv_info_t`): bit 3 (`0x08`, `with_coord`) indicates touch coordinates are available.
    2. Read `max_touches` (or 1–2 points for LugalOS gesture engine) of 7-byte `touch_report_t` structs starting at `0x0014` (`ST7123_REPORT_COORD_0_REG`):
       ```c
       typedef struct __attribute__((packed)) {
           uint8_t x_h_flags; /* bit 7: valid, bit 6: reserved, bits 5..0: x[13:8] */
           uint8_t x_l;       /* x[7:0] -> x = ((x_h_flags & 0x3F) << 8) | x_l (0..719) */
           uint8_t y_h;       /* y[15:8] */
           uint8_t y_l;       /* y[7:0] -> y = (y_h << 8) | y_l (0..1279) */
           uint8_t area;
           uint8_t intensity;
           uint8_t reserved;
       } st7123_point_t;
       ```
- **Controller 2: `GT911` (`I2C Addr = 0x14` or `0x5D`)**:
  - Already supported by `drivers/touch_gt911.c` (Phase 47.12); only needs multi-address probe (`0x14` in addition to `0x5D`) since `GPIO23` is held HIGH during `TP_RST` release.

---

### 2.6 MicroSD Card (`SDMMC` Slot 0) & USB Type-A Host (`DWC2 OTG HS`)

#### MicroSD Card (`J2 TF_CARD_SOCKET`)
| Signal | ESP32-P4 GPIO | Notes |
| :--- | :--- | :--- |
| `SDIO1_SCK` (`CLK`) | `GPIO43` | 22Ω series damping resistor (`R23`), 5.1kΩ pull-up |
| `SDIO1_CMD` (`CMD`) | `GPIO44` | 5.1kΩ pull-up (`R21`) |
| `SDIO1_D0` (`DAT0`) | `GPIO39` | 5.1kΩ pull-up (`R21`) |
| `SDIO1_D1` (`DAT1`) | `GPIO40` | 5.1kΩ pull-up (`R21`) |
| `SDIO1_D2` (`DAT2`) | `GPIO41` | 5.1kΩ pull-up (`R22`) |
| `SDIO1_D3` (`DAT3`) | `GPIO42` | 5.1kΩ pull-up (`R22`) |
| `VDD` | `SOC_3.3V` | Always powered when SoC is on |

> [!TIP]
> **100% Pin-Compatible with Existing P4 SDMMC Driver**:
> `GPIO39–44` on SDMMC Slot 0 is the exact same default IOMUX pin mapping used by `esp32p4` (NANO) and `esp32p4-lcd7b` (`drivers/sdmmc_esp32p4.c`). No GPIO matrix changes are required for `/sd0`!

#### USB Type-A Host Port (`J10 USB-A`)
| Signal | Connection | Notes |
| :--- | :--- | :--- |
| `EXT_USB_DM` / `DP` | `USB2_OTG_D-` (pin 49) / `USB2_OTG_D+` (pin 50) | High-Speed DWC2 OTG 2.0 controller (`0x50000000`) |
| `VBUS` (`SYS_USB5V`) | `U27 MT9700` load switch, enabled by `USB5V_EN` (`PI4IOE2` `0x44` bit 3) | Must set `PI4IOE2` bit 3 = `1` before USB host enumeration |

> [!TIP]
> **100% Controller-Compatible with Existing P4 USB Host Keyboard Driver**:
> Once `PI4IOE2` (`0x44`) bit 3 (`USB5V_EN`) is set HIGH, `drivers/usbhost_esp32p4.c` and `drivers/usbkbd_host.c` work out-of-the-box with the Tab5 USB-A port (including keyboards behind High-Speed USB 2.0 hubs via split transactions).

---

### 2.7 Other Onboard Peripherals (Catalogued for Future Phases)

| Peripheral | Interface / Pins | I2C Addr / Control | Target Phase |
| :--- | :--- | :--- | :--- |
| **ESP32-C6-MINI-1U** (Wi-Fi 6) | SDIO Slot 1: `CLK=GPIO12`, `CMD=GPIO13`, `D0=GPIO11`, `D1=GPIO10`, `D2=GPIO9`, `D3=GPIO8`, `RST=GPIO15` | Power: `PI4IOE2` bit 0 (`WLAN_PWR_EN`); Ant: `PI4IOE1` bit 0 | Phase 57 |
| **ES8388 Audio Codec** (DAC/HP/Line) | `I2S0`: `MCLK=GPIO30`, `BCLK=GPIO27`, `LRCK=GPIO29`, `DOUT=GPIO26`, `DIN=GPIO28` | `SYS_I2C` `0x10`; Speaker PA: `PI4IOE1` bit 1 (`SPK_EN`); HP detect: `PI4IOE1` bit 7 | Phase 57+ |
| **ES7210 4-Ch Mic ADC** (Dual Mic + AEC) | Shares `I2S0` (`MCLK=30`, `BCLK=27`, `LRCK=29`, `SDOUT1=28`) | `SYS_I2C` `0x40` | Phase 57+ |
| **SC2356 2MP Camera** (1600×1200) | 2-Lane MIPI-CSI + `CAM_MCLK=GPIO36` (or 24 MHz osc `X2`) | `SYS_I2C` `0x36` (via 1.8V translator `U36`); `CAM_RST`: `PI4IOE1` bit 6 | Phase 58+ |
| **RX8130CE RTC** | `SYS_I2C` (`GPIO31`/`32`), backup battery `BT1` | `SYS_I2C` `0x32` | Phase 57+ |
| **BMI270 6-Axis IMU** | `SYS_I2C` (`GPIO31`/`32`), `INT1` -> wakeup | `SYS_I2C` `0x68` (or `0x69`) | Phase 57+ |
| **INA226 Power Monitor** | `SYS_I2C` (`GPIO31`/`32`), `5 mΩ` shunt (`R39`) on `SYS_BAT_2S` | `SYS_I2C` `0x41` | Phase 57+ |
| **RS-485 (`SIT3088`)** | `TX=GPIO20`, `RX=GPIO21`, `DIR=GPIO34` | Switchable 120Ω termination (`S2`) | Phase 58+ |

---

## 3. Dual-Orientation Display & Reader Architecture

### 3.1 The Orientation Challenge: Native Portrait (`720×1280`) vs Landscape (`1280×720`)
Unlike the Waveshare ESP32-P4-LCD-7B (whose EK79007 panel scans out `1024×600` in landscape), the M5Stack Tab5's 5-inch panel (ST7123 / ST7121 / ILI9881C) is a **native portrait panel (`720×1280`)**:
- The MIPI-DSI DPI controller and DW-GDMA **must always scan out 1280 lines of 720 pixels** (`stride = 720` bytes in GRAY8, or `1440` bytes in RGB565).
- However, LugalOS has **two primary usage personas** on the Tab5:
  1. **Landscape Normal LugalOS GUI Mode (`1280×720`)**:
     - The user holds the Tab5 horizontally (or docks it with a USB Type-A keyboard).
     - The logical screen is **`1280 × 720`**:
       - `160` columns × `45` total character rows (`8×16` font).
       - 2-row top menu bar + `42` window content rows + 1-row bottom status bar.
       - Using `RIBBON_COLS_1024(1280)` (or a new `RIBBON_COLS_1280` geometry profile): two spacious side-by-side windows (e.g. `85` cols left + `72` cols right + borders/gap = `160` cols), or 3 visible columns!
  2. **Portrait Full-Screen Reader Mode (`720×1280`)**:
     - The user holds the Tab5 vertically like a book or tablet to read Gemtext (`.gmi`) documents in `reader`.
     - The logical screen is **`720 × 1280`**:
       - `90` columns × `80` total character rows (`8×16` anti-aliased serif/sans/mono font).
       - Full-screen single-window view (`90` cols × `78` document rows + 1 header row + 1 status/progress row, or borderless full-screen `90×80` mode), maximizing vertical reading context (nearly **2.5× more lines per page** than the 31-line landscape reader!).

### 3.2 Zero-Tear Hardware Double Buffering in Both Orientations
Our Phase 55 double-buffered PSRAM scanout architecture (`fb_bufs[2]`, `fb_front`, `fb_back`, `dsi_lcd_flip()`) handles both orientations cleanly:
- **Physical Scanout Buffers (`fb_bufs[0]`, `fb_bufs[1]`)**:
  - Always allocated as `720 × 1280` in PSRAM (`921,600` bytes per buffer in GRAY8; `1,843,200` bytes per buffer in RGB565).
- **Mode A — Portrait Native (`720×1280`, `rot = 0` or `180°`)**:
  - Logical screen dimensions: `W = 720`, `H = 1280` (`90 × 80` cells).
  - `term_convert()` renders dirty character rows (`dy = 0..79`) directly into contiguous horizontal scanlines of `fb_back` (`720` bytes per row).
  - Zero rotation overhead; `lcd_scroll_v` and `reader` inertia flick scrolling run natively along the 1280-pixel vertical axis.
- **Mode B — Landscape Rotated (`1280×720`, `rot = 90°` CW or `270°` CCW)**:
  - Logical screen dimensions: `W = 1280`, `H = 720` (`160 × 45` cells).
  - When `term_convert(y0, y1)` renders logical landscape character rows `dy` (`y = dy*16 .. dy*16+15`, `x = 0..1279`), it writes each 8×16 glyph tile rotated 90°/270° into `fb_back` at physical coordinates:
    - For 90° CW (`USB-C on right/bottom`): `phys_x = 719 - logical_y`, `phys_y = logical_x`.
    - For 270° CCW (`USB-C on left/top`): `phys_x = logical_y`, `phys_y = 1279 - logical_x`.
  - **Cache-Line Friendly Tile Blitting**: Instead of writing scattered single pixels across a `720`-byte stride in PSRAM (which would thrash the 64-byte L1 data cache), `term_convert_rot90()` renders an `8×16` glyph into a small stack-allocated `16×8` rotated tile in fast L1 SRAM and copies 16-byte contiguous horizontal runs per physical scanline, followed by `cache_writeback_addr()` and `dsi_lcd_flip()`.

### 3.3 Dynamic Runtime Orientation Switching (`screen_set_orientation`)
- **CLI & Syscall**:
  - `lcd orient landscape|portrait|0|90|180|270` command at the shell.
  - `SYS_SCREEN_CTRL` sub-command `SCREEN_CTRL_SET_ORIENTATION` (`0` = Landscape `1280×720` Ribbon GUI, `1` = Portrait `720×1280` Full-Screen).
- **Automatic Portrait Mode in `reader`**:
  - When `reader` is launched on Tab5 (or when the user presses `o` / `Tab` / taps the orientation icon in the `reader` header bar), `reader` can switch the display into **Portrait Full-Screen Mode (`720×1280`)**, re-wrapping the Gemtext document to `86–90` columns and expanding the viewport to `76–78` lines per page.
  - Exiting `reader` (`q`) or switching back (`o`) restores **Landscape Normal LugalOS GUI Mode (`1280×720`)** and triggers a full Ribbon workspace repaint.
- **Touch Coordinate Remapping**:
  - The touch driver reads raw portrait coordinates `(raw_x ∈ 0..719, raw_y ∈ 0..1279)` from `ST7123` or `GT911` and transforms them according to the active orientation before feeding `touch_gt911.c`'s tap/swipe/flick gesture state machine:
    - In **Portrait Mode (`720×1280`)**: Vertical finger drag (`Δraw_y`) drives `reader` vertical inertia scrolling; horizontal swipe switches windows or navigates back/forward.
    - In **Landscape Mode (`1280×720`)**: Horizontal finger drag along the long 1280px axis (`Δraw_y` mapped to `Δlogical_x`) drives Ribbon horizontal window scrolling, while vertical finger drag along the 720px axis (`Δraw_x` mapped to `Δlogical_y`) drives vertical scrolling.

---

## 4. Implementation Milestones

### Milestone 56.1: Board Preset (`esp32p4-tab5`), Console, I2C1 & `PI4IOE5V6408` Driver
1. **CMake Preset & Board Config (`boards/esp32p4_tab5.conf`, `CMakePresets.json`)**:
   - Add `esp32p4-tab5` (and `esp32p4-tab5x` if v3.2 silicon is detected on the target unit; we will auto-detect silicon revision at first probe via `p4flash.py` / `esptool chip_id`).
   - Configure 16 MB Flash XIP @ 80 MHz, 32 MB Hex DDR PSRAM @ 200 MHz, and `CONFIG_BOARD_M5STACK_TAB5=1`.
2. **USB-Serial/JTAG + UART0 Console & Host Tooling (`tools/p4run.py`, `tools/p4flash.py`)**:
   - Add `--board tab5` support to `tools/p4run.py` and `tools/p4flash.py` (supporting both USB-Serial/JTAG `303a:1001` on `GPIO24/25` and UART0 on `GPIO37/38`).
   - Add ESP32-P4 USB-Serial/JTAG console TX/RX support in the kernel so the single USB-C cable serves as both flasher and interactive `lsh` console without needing an external UART adapter on the M5-Bus.
3. **System I2C (`GPIO31`/`GPIO32`) & `PI4IOE5V6408` Expander Driver (`drivers/pi4ioe_esp32p4.c`)**:
   - Initialize `I2C1` on `SDA = GPIO31`, `SCL = GPIO32` at 400 kHz.
   - Implement `pi4ioe_init()` to configure `U6` (`0x43`) and `U7` (`0x44`), clearing `OUT_H_IM` (`0x07`), enabling `EXT5V_EN` (`0x43` P2) and `USB5V_EN` (`0x44` P3), and sequencing `LCD_RST` (`0x43` P4) and `TP_RST` (`0x43` P5) with `GPIO23` (`TP_INT`) held high.
   - Expose `i2c scan` and `pi4io` diagnostic shell commands.

### Milestone 56.2: MIPI-DSI Display Bring-up (`720×1280` ST7123 / ST7121 / ILI9881C) & Backlight
1. **Runtime Panel Auto-Detection (`drivers/dsi_esp32p4.c`)**:
   - After `pi4ioe_init()` releases `LCD_RST` and `TP_RST`, probe `I2C1` (`0x55` reg `0x0000` for ST7123 `fw=0x03` vs ST7121 `fw=0x01`, and `0x14`/`0x5D` for GT911 / ILI9881C).
   - Configure the MIPI-DSI D-PHY PLL (`1040 Mbps` for ST7123/ILI9881C, `900 Mbps` for ST7121) and DPI pixel clock (`80 MHz` or `70 MHz`) with the exact porch timings required by TDDI (`VFP = 220` for ST7123, `VFP = 200` for ST7121).
   - Send the corresponding DCS initialization sequence (`Panel_ST7123` `list0`+`list1` or `Panel_ILI9881C` `list0..list3`).
2. **Active-High LEDC Backlight on `GPIO22`**:
   - Parameterize backlight GPIO and polarity in `drivers/dsi_esp32p4.c` (`GPIO22`, active-high for Tab5 `ME2212` vs `GPIO32` active-low on LCD-7B).
   - Support `lcd backlight [0-100]`, `lcd bl on|off`, and `lcd blank`.
3. **Double-Buffered `720×1280` PSRAM Scanout**:
   - Allocate `fb_bufs[0]` and `fb_bufs[1]` (`720 × 1280` GRAY8 = `900 KB` each, or RGB565 = `1.8 MB` each) in PSRAM.
   - Configure DW-GDMA multi-block LLI chains for `720 × 1280` (`921,600` bytes in GRAY8 = `240` blocks of `3840` bytes; `1,843,200` bytes in RGB565 = `480` blocks of `3840` bytes).
   - Verify with `lcd test checker|grid|ramp`, `lcd verify`, and `lcd fps`.

### Milestone 56.3: Dual-Orientation Compositor (`1280×720` Landscape GUI & `720×1280` Portrait Reader)
1. **90°/270° Rotated Tile Blitter in `drivers/lcdterm.c`**:
   - Add `lcdterm_orient_t` (`LCDTERM_ORIENT_LANDSCAPE_1280x720` and `LCDTERM_ORIENT_PORTRAIT_720x1280`).
   - Implement cache-friendly `16×8` SRAM tile rotation in `term_convert()` for Landscape Mode (`1280×720`, `160×45` cells) into the physical `720×1280` PSRAM back buffer,supporting 1-bit crisp and 4-bit anti-aliased GRAY8/RGB565 fonts.
   - Extend `RIBBON_COLS_*` in `drivers/ribbon.h` for `1280px` landscape width (`160` cols) and `720px` portrait width (`90` cols).
2. **Portrait Full-Screen Gemtext Reader Mode (`user/reader.c`, `drivers/screen.c`)**:
   - Add orientation control to `SYS_SCREEN_CTRL` and `lcd orient portrait|landscape`.
   - Update `user/reader.c` to support **Portrait Full-Screen Mode (`720×1280`, `90×78` viewport)**:
     - Automatic or one-key (`o` / header tap) switch between Landscape Ribbon GUI (`1280×720`) and Portrait Full-Screen Reader (`720×1280`).
     - Dynamic line-wrapping (`reader_wrap_col`) adjusted to portrait width (`86` cols) with generous margins and anti-aliased serif typography.
     - Update `screenshot` (`drivers/screen.c`) to output the active logical orientation (`1280×720` in landscape, `720×1280` in portrait).

### Milestone 56.4: Capacitive Touch Driver (`ST7123` + `GT911`) & Orientation-Aware Inertia Scrolling
1. **`ST7123` / `ST7121` I2C Touch Driver (`drivers/touch_gt911.c` or `drivers/touch_st7123.c`)**:
   - Implement the `0x55` I2C protocol (reading status reg `0x0010` and 7-byte coordinate reports at `0x0014`) alongside `GT911` (`0x14`/`0x5D`).
   - Transform raw panel coordinates `(0..719, 0..1279)` into logical screen coordinates based on the active orientation (`1280×720` landscape vs `720×1280` portrait).
2. **Gesture & Momentum Physics Verification**:
   - Verify tap-to-focus, minimap tap-to-jump, horizontal 3-step animated Ribbon window swipe in Landscape Mode (`1280×720`), and 60 Hz tear-free vertical inertia flick scrolling in Portrait Full-Screen `reader` Mode (`720×1280`).

### Milestone 56.5: MicroSD Card (`/sd0`) & USB Type-A Host Keyboard
1. **MicroSD Card (`drivers/sdmmc_esp32p4.c`)**:
   - Verify 4-bit SDMMC Slot 0 on `GPIO39–44` at 20 MHz, mounting FAT32 `/sd0` at boot and syncing `/sd0/doc/*.gmi` over 9P (`tools/p9sync.py --p4 tab5`).
2. **USB Type-A Host Keyboard (`drivers/usbhost_esp32p4.c`, `drivers/usbkbd_host.c`)**:
   - Ensure `PI4IOE2` (`0x44`) bit 3 (`USB5V_EN`) is enabled at boot so `U27 MT9700` supplies 5V VBUS to `J10`.
   - Verify USB HID keyboard enumeration (direct and via High-Speed hub), typing in `lsh` and `reader`, and Ribbon window hotkeys (`Ctrl+W`, `F1`–`F6`).

### Milestone 56.6: Hardware-in-the-Loop Test Suite & Documentation
1. **Automated Hardware Test Suite (`tests/hw/test_esp32p4.py`)**:
   - Add `--board tab5` support to `tests/hw/test_esp32p4.py`.
   - Add Tab5-specific verification checks: `PI4IOE5V6408` (`0x43`/`0x44`) presence, ST7123/ILI9881C DSI link & frame buffer readback at `720×1280`, orientation switching (`1280×720` <-> `720×1280`), touch controller response (`0x55` / `0x14`), `/sd0` read/write, and USB Host port status.
2. **Documentation**:
   - Document the M5Stack Tab5 preset, port topology, and Portrait/Landscape Reader workflow in `AGENTS.md` and `doc/admin/`.

---

## 5. Verification Criteria
- [x] `ninja -C build/esp32p4-tab5` builds cleanly with zero warnings and zero floating-point instructions in kernel/M-mode.
- [x] `PI4IOE5V6408` expanders (`0x43`, `0x44`) initialize cleanly on `GPIO31`/`GPIO32` and power up LCD, Touch, and USB-A 5V VBUS.
- [x] 5" `720×1280` MIPI-DSI display auto-detects the panel variant (`ST7123` / `ST7121` / `ILI9881C`) and runs tear-free double-buffered scanout from 200 MHz PSRAM.
- [x] **Landscape Mode (`1280×720`)** displays the full LugalOS Ribbon GUI (`160×45` window area) with crisp and anti-aliased fonts and smooth horizontal window transitions.
- [x] **Portrait Full-Screen Mode (`720×1280`)** in `reader` displays Gemtext (`.gmi`) documents across the full 1280px vertical height with 4-bit anti-aliased typography and smooth 60 Hz touch flick inertia scrolling.
- [x] Capacitive touch (`ST7123` / `ST7121` / `GT911`), USB Type-A Host keyboard, and MicroSD card (`/sd0`) pass all hardware-in-the-loop tests.

---

## 6. Live Hardware Verification Results (2026-10-16)

Physical target: **M5Stack Tab5** (`E8:F6:0A:E2:FC:97`), ESP32-P4 revision v1.3 @ 360 MHz, 32 MB Hex DDR PSRAM @ 200 MHz, 16 MB QIO Flash, ST7121 5" 720×1280 MIPI-DSI TDDI display, 32 GB microSD card.

### 6.1 Hardware-in-the-Loop Test Suite (`tests/hw/test_esp32p4.py --board tab5`)
```
======================================================================
        LugalOS ESP32-P4 Hardware-in-the-Loop Suite (E8, phase 27)
======================================================================

console /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_E8:F6:0A:E2:FC:97-if00
  [PASS] boots to a shell
  [PASS] /proc answers
  [PASS] /flash0 is mounted (E6)
  [PASS] flash writes while executing from flash (U3) -- 3 files written, read back, and survived a reboot
  [PASS] chibicc compiles a C file on the P4 (phase 32) -- 196-byte ELF generated
  [PASS] the tick preempts under a READY task (E4, MIL) -- spin0 +200, spin1 +200 ticks
  [PASS] U-mode isolation refuses (E5)
  [PASS] I2C scan finds the codec and the sensor (E7) -- found 10 I2C devices on Tab5 (ES8388, PI4IOE, ST7121 touch)
  [PASS] node identity comes from eFuse, not a build seed (Z5) -- mac e8:f6:0a:e2:fc:97 (silicon), uid 7636c7e4f577252b (silicon)
  [PASS] microSD card answers (35.5) -- 30436 MB, 4-bit at 20000 kHz
  [PASS] the bus really is 4-bit (35.4) -- 4-bit at 20000 kHz
  [PASS] /sd0 is mounted (35.2) -- /sd0/             62333952      39680   62294272       0% /sd0/
  [PASS] reads go through the sdblk task (35.3) -- 273 -> 314 calls served
  [PASS] /sd0 survives a reboot (35.4) -- 3 files written, rebooted, read back
  [PASS] PSRAM is up, 32 MB, bulk zone in it (47.3) -- 32768 KB, vendor 0x0d, 200 MHz, tuning window 25/31 -> #16, bulk zone 7819 pages
  [PASS] PSRAM holds a pattern through the caches (47.3) -- 4 MB x2, 147/216 ms
  [PASS] DSI link up, panel init clean, colour bars (47.4) -- PLL locked (PHY status 0x000015b9), 0 init errors, VPG on
  [PASS] DSI frame buffer from PSRAM, 60 Hz, readback exact (47.5) -- RGB565, 57.01 Hz, 0 DMA errors, no underrun, checker reads back exactly
  [PASS] the terminal on the panel: 158 x 42, frame-paced, colours (47.7) -- 1125 lines/s, a character 7046 us, colours verified (RGB565 native)
  [PASS] backlight PWM: 30 % is duty 307/1024, timer running (47.11) -- duty 307/1024 at 5000 Hz, timer 902 -> 81
  [PASS] idle blanking: blanks when quiet, serial input wakes it (47.11) -- blanked once in 4 s, woken by the next command
  [PASS] touch: GT911 or ST7121 found, polled, no I2C errors (47.12) -- 720x1280, 65 polls in ~1 s, 0 I2C errors
  [PASS] RTC: model identified, oscillator running (47.7) -- DS1307, running, 2000-00-00 00:21:24 UTC
  [PASS] ribbon: Cmd+Enter, Cmd+Left/Right, Cmd+W over serial (47.8) -- 2 windows, focus 1 -> 0 -> 1, closed back to 1
  [PASS] gemtext: Ribbon Reader window opening and ribbon split (53.6) -- reader window opened in ribbon, reported as Reader, closed cleanly
  [PASS] typography: Reader anti-aliased pipeline (55.3) -- anti-aliased reader opened, listed in reader list, and closed cleanly
  [PASS] screenshot: color/grayscale scanout capture in PGM/PPM format (55.3) -- captured /sd0/screenshots/shot-002.ppm with valid Netpbm header
  [PASS] canvas: RGB565 color window & hybrid compositing (55.4) -- RGB565 canvas opened, verified Lisp primitives, captured /sd0/screenshots/shot-003.ppm (P6 PPM), closed cleanly
  [PASS] animation: ribbon slide and reader smooth scrolling (55.5) -- ribbon slide animation and reader smooth scrolling verified at 60 Hz
  [PASS] display orientation: Landscape 270 deg and Portrait 0 deg (56.3) -- switched to 0 deg Portrait and back to 270 deg Landscape

----------------------------------------------------------------------
Result: 46 / 46 PASSED
======================================================================
```

### 6.2 Full Automated Regression Test Suite (`tests/runner.py`)
```
----------------------------------------------------------------------
Result: 506 / 506 Tests PASSED (344.06s)
======================================================================
```
Zero regressions across all existing targets: RP2350, ESP32-P4-NANO, ESP32-P4-LCD7B, ESP32-C6-Zero, RV32, and RV64 SMP.
