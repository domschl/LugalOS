# LugalOS Consoles, Serial Topologies & Modem Control

Connecting to LugalOS hardware requires understanding host USB serial enumeration, TTY naming, and board-specific modem signal lines.

---

## 1. Dynamic Port Numbers & Device By-ID Rules

> [!IMPORTANT]
> Linux device node numbers (`/dev/ttyACM0`, `/dev/ttyUSB0`) depend entirely on the order hardware is plugged into USB ports or enumerated by the host kernel.
> Never assume fixed port indices! Always identify interfaces using `/dev/serial/by-id/` symlinks.

```bash
# List all connected LugalOS serial interfaces
ls -l /dev/serial/by-id/
```

---

## 2. Hardware Port Topologies

### 1. RP2350 Targets (Pico 2, RP2350-LCD-7)
LugalOS exposes **two CDC-ACM USB serial interfaces**:
* **Interface 0 (`if00`)**: Interactive `lsh` shell & Lisp console at **115200 baud**.
  * Symlink: `/dev/serial/by-id/usb-LugalOS_*_Dual_CDC_ACM_*-if00`
* **Interface 2 (`if02`)**: Dedicated 9P filesystem and network link (`link_usb_cdc`).
  * Symlink: `/dev/serial/by-id/usb-LugalOS_*_Dual_CDC_ACM_*-if02`

Connecting to the console:
```bash
picocom -b 115200 /dev/serial/by-id/usb-LugalOS_*_Dual_CDC_ACM_*-if00
```

### 2. ESP32-P4 Targets (M5Stack Tab5, ESP32-P4-NANO & LCD-7B)
* **High Baud Rate**: LugalOS runs the ESP32-P4 UART0 / USB-Serial/JTAG console at **921,600 baud** (`CONFIG_UART0_BAUD`) to handle rapid graphics and microkernel debug logging.
* **Modem Control Lines**:
  * `RTS` controls `ESP_EN` (hardware chip reset).
  * `DTR` controls `GPIO35/BOOT` (download boot mode).
* **Reset Danger**: Opening the port with naive tools (such as raw `screen` or bare `pyserial`) asserts RTS/DTR and holds the chip in continuous reset.

**Always communicate via `tools/p4run.py`**:
```bash
# Connect to M5Stack Tab5 5" Portable Tablet
python3 tools/p4run.py --board tab5 --run

# Connect to ESP32-P4 7" LCD Terminal
python3 tools/p4run.py --board lcd7b --run

# Connect to ESP32-P4 NANO
python3 tools/p4run.py --board nano --run
```
When multiple P4 boards are attached simultaneously, configure explicit port mappings in `~/.config/lugalos/p4-ports.env`.

### 3. ESP32-C6 Targets (ESP32-C6-Zero)
Uses native USB-Serial/JTAG (`303a:1001`):
* Opening the port with DTR low resets the chip.
* **Always communicate via `tools/c6run.py`**:
```bash
python3 tools/c6run.py --run
```
