# LugalOS Shell & Command Environment

The interactive shell (`lsh`) is the primary user interface for LugalOS across interactive display terminals and serial consoles. Built around a Plan 9–inspired philosophy, LugalOS exposes system metrics, hardware devices, and IPC channels as hierarchical file namespaces.

---

## 1. Shell Basics & Path Resolution

When you start LugalOS, `lsh` displays the interactive prompt:

```text
lsh> 
```

### Search Path & Binary Execution
To execute a program:
* **Direct Path**: Entering a path containing a slash (e.g. `/sd0/system/bin/hello.elf` or `/ram0/myprog.elf`) runs the executable directly.
* **Search Path**: Entering a bare command name (e.g. `uhello`) searches through the volumes listed in `/proc/path` (typically `/sd0/system/bin/` then `/flash0/system/bin/`).
* **Built-in Commands**: Built-in shell commands are resolved immediately without filesystem access.

---

## 2. Filesystem & Navigation Commands

LugalOS supports multiple mounted FAT32 volumes:
* `/flash0/`: Embedded Flash ROM/XIP filesystem (read-only or flash-backed).
* `/sd0/`: Physical MicroSD card (read/write).
* `/ram0/`: In-memory RAM disk (read/write, volatile).
* `/proc/`: Synthetic process and system information filesystem.
* `/dev/`: Hardware device interfaces (`/dev/uart`, `/dev/rtc`, `/dev/sensors`).
* `/srv/`: Named message channels and service endpoints.

### Common File Operations
| Command | Description | Example |
| :--- | :--- | :--- |
| `ls [path]` | List files and directories | `ls /sd0` |
| `cat <path>` | Display file contents to console | `cat /sd0/hello.c` |
| `more <path>` | View file contents with terminal-height paging | `more /sd0/cas/simplify.lisp` |
| `touch <file>` | Create an empty file | `touch /sd0/test.txt` |
| `mkdir <path>` | Create a new directory | `mkdir /sd0/myproject` |
| `rmdir <path>` | Remove an empty directory | `rmdir /sd0/myproject` |
| `cp <src> <dst>` | Copy a file | `cp /flash0/init.lisp /sd0/init.lisp` |
| `rm <file>` | Delete a file | `rm /sd0/test.txt` |
| `df [-r]` | Show filesystem disk usage (`-r` recounts FAT) | `df` |
| `format <vol>` | Format a volume as FAT32 (destroys data) | `format /ram0` |

---

## 3. System & Process Monitoring

### Process & Resource Inspection
* `ps` (alias for `cat /proc/ps`): Lists running tasks, their priority tier, current CPU core (Hart), execution state, and memory domain isolation status (`PMP`, `Sv39`, or `-`).
* `top`: Interactive real-time process, CPU, and memory monitor. Press `q` to exit.
* `meminfo` (alias for `cat /proc/meminfo`): Shows total, used, and free page frames in the physical allocator (`palloc`), including bulk PSRAM pools.
* `uname`: Prints the OS build name, release version, and hardware architecture target.

### Real-Time Clock & Timestamps
* `date`: Displays current date and time.
* `date YYYY-MM-DD HH:MM:SS`: Sets the system time (local time) and the battery-backed hardware RTC: a DS1307/DS3231 module, or the board's own RX8130CE on the M5Stack Tab5.
* `rtc`: Reports the RTC chip and its address, oscillator health, and temperature readback (DS3231 only). At boot the kernel clock is set from the RTC unless the chip reports a power loss or holds a year before 2024 (a clock nobody set).

### Power & Battery
On a board with a battery (the M5Stack Tab5):
* `power`: Pack voltage, current (positive while charging), power, the estimated charge level, and the time to empty or full.
* `power log FILE [s]`: Writes one CSV line a second (`t_s,mv,ma,mw,soc_pm`) to FILE, for `s` seconds or until `power log stop`. Used to measure a full discharge for calibrating the charge estimate.
* `power charge [off|slow|fast]`: Shows or sets the charger. `off` stops charging, `slow` and `fast` are the Tab5 charger's two modes (M5Stack's 500 mA and 1 A). The setting is stored and applied at every boot. The default is `fast`, which is how the board starts.
* `power limit [N|off]`: Stops charging at N % and resumes below N−5 %, for a tablet that stays on its charger. `off` (or 100) charges fully.
* `power off`: Switches the board off through its power supervisor. LugalOS does the same by itself when the battery stays below the cut-off voltage (6.2 V on the Tab5) for 30 s while discharging, after stopping a running `power log`. Below 6.6 V the menu bar shows `!` after the charge level.
* `cat /dev/power`: The same state as `key=value` lines (`mv`, `ma`, `mw`, `state`, `soc_pm` in 0.1 %, `minutes`, `charge`, `held`, `limit`, `chg_stat`), readable over 9P. Writing `charge MODE`, `limit N|off` or `off` to it does what the commands do.
* The menu bar shows the charge level left of the clock, with `+` while charging.

The charge level blends two estimates. One is the pack voltage, corrected for the voltage drop under load and read off a Li-ion discharge curve. The other counts the current in and out against the pack's capacity. The count keeps the number steady from second to second, and the voltage corrects it over several minutes. ([sensors](doc/user/sensors.md))

### Motion (IMU)
On a board with a BMI270 (the M5Stack Tab5, or a module at 0x69):
* `imu`: One reading: acceleration in g, rotation in deg/s, and pitch and roll in degrees.
* `imu watch [s]`: Five readings a second for `s` seconds (default 10); `q` stops.
* `imu reload`: Uploads the chip's configuration again (a diagnostic).
* `orient [auto|off]`: Turns the display with the board. In `auto` the screen follows within about half a second once the board is held in a new position, and keeps its orientation while the board lies flat or is being turned. All four orientations are used (landscape, portrait, and both upside down). The setting is stored and applied at boot; the default is `off`. Turning the display by hand (`lcd orient`, Super+O, the reader's `o`) pauses `auto` until `orient auto`.

The BMI270 measures nothing until Bosch's configuration microcode (8 KB, BSD-3 licence, `drivers/bmi270_config.c`) has been uploaded to it. That takes about a second after boot, on the sensor task. The readings also appear in `sensor` and `/proc/sensor/bmi270`, not on MQTT.

### Firmware Updates
* `cat /dev/ota`: The node's slots, boot record and update state. Commands (`begin`, `commit`, `switch`, `reboot`, ...) are written to it over 9P, usually by `tools/lota.py` ([updates](doc/admin/updates.md)).
* `sdio ota [path]` (ESP32-P4): Installs LugalOS on a C6 still running the factory esp-hosted-mcu, over SDIO ([updates](doc/admin/updates.md)).

---

## 4. Shell History & Command Expansion

LugalOS maintains persistent, bounded command history preserved across reboots and power cycles in `/sd0/system/history.lisp`:
* **Interactive Navigation**: Use `Up-Arrow` and `Down-Arrow` at the shell prompt to recall previously entered commands across boot cycles (last 10 entries retained).
* `history`: Displays numbered command history entries.
* `history clear`: Clears in-memory and on-disk history log.
* `!n`: Re-executes command number $n$ from history (e.g. `!1`).
* `!!`: Re-executes the most recent command.

---

## 5. Shell Pipelines & Text Filters

LugalOS supports Unix-style composability via the pipe operator (`|`). Commands run in sequential buffered stages:

```text
lsh> help | more
lsh> dmesg | grep -i wlan
lsh> cat /proc/meminfo | grep Heap
lsh> ps | wc -l
```

### Core Text Utilities
| Command | Description | Example |
| :--- | :--- | :--- |
| `grep [-i] [-v] <pat> [f]` | Filter lines matching pattern (`-i` case-insensitive, `-v` invert) | `dmesg \| grep klogd` |
| `head [-n N] [file]` | Emit first $N$ lines (default: 10) | `cat /sd0/log.txt \| head -n 5` |
| `tail [-n N] [file]` | Emit last $N$ lines (default: 10) | `dmesg \| tail -n 15` |
| `wc [-l\|-w\|-c] [file]` | Count lines (`-l`), words (`-w`), or bytes (`-c`) | `ls /sd0 \| wc -l` |
| `echo [text]` | Output text to console or pipeline stream | `echo "hello world" \| wc -w` |
| `more [file]` | View file or pipeline stream with terminal paging | `help \| more` |

---

## 6. Kernel Logging & Diagnostics

LugalOS features a ring buffer kernel logging subsystem ([logging](doc/dev/kernel/logging.md)):
* `dmesg`: Displays kernel diagnostic ring messages. Background logs are silenced at the interactive prompt to keep typing clean.
* `dmesg -c`: Dumps and clears the kernel log ring.
* `dmesg -f`: Live follow mode (streams new kernel logs until `q` or `Ctrl-C`).
* `cat /proc/kmsg`: Reads current kernel logs from the synthetic VFS.
* `klog`: Displays kernel log statistics, total emitted bytes, and attached output sinks.
* `klog detach console`: Detaches console sink permanently.
* `klog attach console`: Re-enables unconditional kernel log echoing to the console.
* `lockcheck [pairs]`: The lock checker since boot: faults (blocking under a spinlock, console writes from a driver's serve callback, wait cycles), the lock order it has learned (`pairs` lists it) and any inversions, and how long `console_lock` was held: a histogram, the longest hold and who held it, and the last holds of 100 ms or more.
* `chanqtest`: Selftest of the channel layer's waiting callers: turns served in arrival order, a back-to-back client handing over to a waiter, deadlines, and refusals. Prints `CHANQ_SELFTEST_OK` or `CHANQ_SELFTEST_FAIL`.

---

## 7. Built-in Applications & Languages

From `lsh`, you can immediately invoke native applications:
* `lisp`: Starts the interactive Scheme/Lisp REPL environment ([Lisp scripting](doc/user/cas.md)).
* `e [file]`: Opens the full-screen Emacs editor ([editors](doc/user/editors.md)).
* `ed [file]`: Opens the teletype line editor ([editors](doc/user/editors.md)).
* `chess`: Launches the chess computer UI ([chess](doc/user/chess.md)).
* `zmachine [story]`: Launches the Z-machine Version 3 interactive fiction interpreter with canvas auto-mapping ([interactive fiction](doc/user/zmachine.md)).
* `cc <src> <dst>`: Compiles a C11 source file on-device to a native RISC-V ELF binary using `chibicc` ([C programming](doc/dev/on_device/c_programming.md)).
* `exec <elf>`: Loads and executes a RISC-V ELF binary confined to a U-mode PMP memory domain.

---

## 6. On-Device Help Commands

* `help`: Displays a quick command reference categorized by topic.
* `help test`: Displays internal hardware verification and self-test commands.
* `help <topic>` or `man <topic>`: Opens the interactive topic viewer (e.g. `man editors`, `man ribbon`, `man cas`).
