# LugalOS Shell & Command Environment

[Documentation](../README.md) > [User Manual](index.md) > **Shell & Command Environment**

---

The interactive shell (`lsh`) is the primary user interface for LugalOS across interactive display terminals and serial consoles. Built around a Plan 9-inspired philosophy, LugalOS exposes system metrics, hardware devices, and IPC channels as hierarchical file namespaces.

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
| `fsck [/sd0\|/ram0]` | Read-only FAT32 check: every directory and cluster chain walked; reports cross-linked clusters, chains running into free entries, files whose size does not match their chain, and lost clusters (in use, reached from nowhere). Changes nothing. | `fsck` |
| `format <vol> [label]` / `mkfs.vfat <vol> [label]` | Format a volume as FAT32 (destroys data). `/sd0` gets a card's layout, as `mkfs.vfat` or the SD Association's formatter make it: an MBR with one FAT32 partition from 4 MB, 32 KB clusters, the data area on a 4 MB boundary, FSInfo and backup boot sector; `label` up to 11 characters (default `LUGALOS`). About 20 s for 32 GB. `/ram0` gets a bare volume. | `mkfs.vfat /sd0 MYCARD` |

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
* `power log` / `power log auto on|off`: The automatic log, on from boot on boards with a battery while LugalOS is in development (`CONFIG_POWER_LOG_AUTO`). One CSV file a day in `/sd0/log/` (`PYYMMDD.CSV`, local date; `PNODATE.CSV` until the clock is set) with `utc_s,uptime_s,mv,ma,mw,soc_pm,chg_stat,why`: a line a minute (`t`), and one at once when the state or the charger's status changes (`s`), the current moves by more than 100 mA (`i`), the voltage by more than 50 mV (`v`) or the charge by 1 % (`q`); `b` marks a boot. `power log` alone shows both logs.
* `power charge [off|slow|fast]`: Shows or sets the charger. `off` stops charging, `slow` and `fast` are the Tab5 charger's two modes (M5Stack's 500 mA and 1 A). The setting is stored and applied at every boot. The default is `fast`, which is how the board starts.
* `power limit [N|off]`: Stops charging at N % and resumes below N-5 %, for a tablet that stays on its charger. `off` (or 100) charges fully.
* `power off`: Switches the board off through its power supervisor. LugalOS does the same by itself when the battery stays below the cut-off voltage (6.2 V on the Tab5) for 30 s while discharging, after stopping a running `power log`. Below 6.6 V the menu bar shows `!` after the charge level.
* `cat /dev/power`: The same state as `key=value` lines (`mv`, `ma`, `mw`, `state`, `soc_pm` in 0.1 %, `minutes`, `charge`, `held`, `limit`, `chg_stat`), readable over 9P. Writing `charge MODE`, `limit N|off` or `off` to it does what the commands do.
* The menu bar shows the charge level left of the clock, with `+` while charging.

The charge level blends two estimates. One is the pack voltage, corrected for the voltage drop under load and read off a Li-ion discharge curve. The other counts the current in and out against the pack's capacity. The count keeps the number steady from second to second, and the voltage corrects it over several minutes. ([sensors](sensors.md))

### Motion (IMU)
On a board with a BMI270 (the M5Stack Tab5, or a module at 0x69):
* `imu`: One reading: acceleration in g, rotation in deg/s, and pitch and roll in degrees.
* `imu watch [s]`: Five readings a second for `s` seconds (default 10); `q` stops.
* `imu reload`: Uploads the chip's configuration again (a diagnostic).
* `orient [auto|off]`: Turns the display with the board. In `auto` the screen follows within about half a second once the board is held in a new position, and keeps its orientation while the board lies flat or is being turned. All four orientations are used (landscape, portrait, and both upside down). The setting is stored and applied at boot; the default is `off`. Turning the display by hand (`lcd orient`, Super+O, the reader's `o`) pauses `auto` until `orient auto`.

The BMI270 measures nothing until Bosch's configuration microcode (8 KB, BSD-3 licence, `drivers/bmi270_config.c`) has been uploaded to it. That takes about a second after boot, on the sensor task. The readings also appear in `sensor` and `/proc/sensor/bmi270`, not on MQTT.

### Firmware Updates
* `ota`: The update state as a table: the board and its flash layout, the running image's version, each slot's build id and place in flash (marked `RUNNING`, `active`, `pending`), where the loader or partition table, `/flash0`, the boot records and the identity sector are, and whether the trial confirmed. On a P4 with a C6 mounted at `/c6`, the C6's table follows, with the LLP version each end speaks and whether both run the same build. `ota` is read-only; `cat /dev/ota` has the same data as `key=value` lines. From Lisp: `(ota-info)` ([lisp](../dev/on_device/lisp_scripting.md)).
* `cat /dev/ota`: The node's slots, boot record and update state. Commands (`begin`, `commit`, `switch`, `reboot`, ...) are written to it over 9P, usually by `tools/lota.py` ([updates](../admin/updates.md)).
* `sdio ota [path]` (ESP32-P4): Installs LugalOS on a C6 still running the factory esp-hosted-mcu, over SDIO ([updates](../admin/updates.md)).
* `otakey`: The release key update images must be signed with (its key id), or `no release key`. `otakey set <128 hex>` stores the public key printed by `tools/lota.py keygen` or `pubkey`: from then on the node takes only images signed with it. `otakey clear` removes it. `otakey allow-unsigned` lets the next update image begun within 10 minutes be unsigned, once. A console-only command: an `ea` tool call cannot run it ([updates](../admin/updates.md#signed-images)).

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

LugalOS features a ring buffer kernel logging subsystem ([logging](../dev/kernel/logging.md)):
* `dmesg`: Displays kernel diagnostic ring messages. Background logs are silenced at the interactive prompt to keep typing clean.
* `dmesg -c`: Dumps and clears the kernel log ring.
* `dmesg -f`: Live follow mode (streams new kernel logs until `q` or `Ctrl-C`).
* `cat /proc/kmsg`: Reads current kernel logs from the synthetic VFS.
* `klog`: Displays kernel log statistics, total emitted bytes, and attached output sinks.
* `klog detach console`: Detaches console sink permanently.
* `klog attach console`: Re-enables unconditional kernel log echoing to the console.
* `lockcheck [pairs]`: The lock checker since boot: faults (blocking under a spinlock, console writes from a driver's serve callback, wait cycles), the lock order it has learned (`pairs` lists it) and any inversions, and how long `console_lock` was held: a histogram, the longest hold and who held it, and the last holds of 100 ms or more.
* `chanqtest`: Selftest of the channel layer's waiting callers: turns served in arrival order, a back-to-back client handing over to a waiter, deadlines, and refusals. Prints `CHANQ_SELFTEST_OK` or `CHANQ_SELFTEST_FAIL`.
* `stress [s] [norot] [only=PATHS]` / `stress bg [s] ...`: Load test of the paths tasks share (default 60 s): worker tasks pick at random among formatting, files on `/ram0` and `/sd0`, the local 9P server, channel calls into a busy endpoint, log bursts, shell commands and (on the ESP32-P4) cache maintenance on scratch buffers of their own, and check every result; on the Tab5 the panel is turned every 7 s as well (`norot` leaves it). `only=` limits the workers to some paths, comma-separated: `fmt`, `ram`, `sd`, `p9`, `chan`, `log`, `shell`, `cache` (e.g. `stress 600 only=sd,p9`). Reports iterations and errors per path, and the lock faults, lock-order inversions, I2C failures, log gaps and full stacks seen meanwhile; prints `STRESS_OK` or `STRESS_FAIL`. `bg` runs it in the background and prints the report in the terminal that started it. One run at a time. On the P4 boards, `tools/stress.py --board BOARD --minutes N` adds terminals, typing, 9P over TCP and fast sensor sampling around it (`--norot`, `--only`, and `--bare` for no terminals and no typing).

### Storage, Time & Network
* `sdinfo`: The SD card and its controller, re-read on every run (an empty slot is probed again, so a card inserted since boot is found): card identity, bus width and clock, host status, counters for data-path anomalies, and block 0 with its boot signature (separates a blank card from a broken data path).
* `sdbench [kb]`: Sequential SD read throughput; `sdbench w` measures writes. `fsbench <dir>` is the filesystem write/read/verify benchmark; `edbench [kb] [dir]` times the editor's buffer operations.
* `tz [POSIX-TZ]`: Shows the time zone and whether summer time is in force; with a rule (`tz CET-1CEST,M3.5.0,M10.5.0/3`, `tz UTC0`) sets it.
* `ntp [server-ip]`: Sets the clock over NTP; without an argument it asks the gateway.
* `peers [...]`: The LLP peer table. `p9serve`: Runs the local 9P server on the console link (see the networking guide).
* `setup [start | stop | reset-owner | secret TEXT | net A.B.C.D/N | net off]`: Whether this node is claimed (an `admin` grant exists), and the setup window that lets its owner claim it over the network (15 minutes, closed after five wrong secrets). `start` and `stop` open and close the window; `secret TEXT` stores the setup secret (8-32 printable characters; `tools/provision.py --setup-secret-generate` makes one and prints a label and a Wi-Fi QR payload); `reset-owner` wipes the owner part of the identity (name, Wi-Fi, grants) and leaves the node unclaimed with its uid, device key and secret intact. `net` names the one network the claim door opens on (a Pico 2 W's setup access point sets it itself). A build without `CONFIG_NETWORK_CLAIM` says so and offers no window. Console only: the network cannot reset an owner. On a board with a button (ESP32-P4 LCD-7B: BOOT; Pico 2 and 2 W: BOOTSEL; C6-Zero: BOOT) holding it 5 s does `setup start` and holding it 15 s does `reset-owner`. The ESP32-P4-NANO has none: its BOOT pin is also the Ethernet MAC's TXD1.
* `peers add NAME KEYHEX [ANAME] [ro|rw|admin]`: also takes `admin`, the grant that may change the owner part over 9P.
* `board`: Which board this is, and which software: `preset` (the build directory: `rp2350-wifi`, `esp32p4`), `board` (the board file), `hardware` (e.g. Waveshare ESP32-P4-NANO), `chip` (with its revision where the chip reports one), `version`, `build`, `tainted`, `name` and `uid`. The same text is `/proc/board`, which anyone may read without a key, and the console banner and boot log carry it as one line.
* `httpd [status | start [PORT] | stop | write on|off]`: The admin web server (default port 80): the admin page at `/` and 9P over a WebSocket at `/9p`, authenticated with a granted key exactly like 9P over TCP. A web session that offers the protected-session extension (the admin page and `p9lib` do; see [protected 9P sessions](../dev/protocols/protected_9p.md)) may write, with every frame tagged against alteration and replay. One that does not is read-only unless `httpd write on` says otherwise (allowing plain writes is allowing a hostile network to forge them). On a board with HTTPS (the ESP32-P4 boards) and a certificate from the owner's CA (`tls`, [HTTPS certificates](../admin/tls.md)), the same server speaks TLS 1.3 on port 443: there an `admin` peer may write, plain HTTP only reads, and the page on port 80 redirects to HTTPS; `status` shows `https:`. The page itself is part of the firmware, so it always matches the node (a client that does not take gzip gets `/flash0/www/admin.html` or `/sd0/www/admin.html` instead, if there is one). Not started at boot, except on a claimed node that serves its page by itself (a Pico 2 W after its setup, a C6-Zero or an ESP32-P4 board once it has an address); at most two sessions (one on the RP2350), with buffers taken when it starts and returned when it stops. `host/p9lib` speaks it as `connect_ws()`.
* `tls [status | keygen | rekey | clear | csr | cert]` (boards with `CONFIG_TLS_SERVER`: the ESP32-P4 boards and QEMU): The node's HTTPS key and certificate. `status` (also `/dev/tls`) gives the key's fingerprint, the certificate's subject, issuer and dates, whether the two match, and the names a request would carry; `keygen` makes a key when there is none (refused without hardware entropy), `rekey` replaces it (and drops the certificate), `clear` deletes both; `csr` prints the certificate request (`/dev/tlscsr`), `cert` the installed certificate (`/dev/tlscert`, which also takes a new one, admin only). The owner's CA, `host/ca`, does all of this over 9P: see the [HTTPS certificates guide](../admin/tls.md).
* `wifi ap [start SSID PASSPHRASE [CHANNEL] | stop]` (Pico 2 W): The radio as a WPA2 access point at `192.168.4.1/24`, with an address server and a name server for its clients, for trying the radio by hand. A node runs its own setup access point (see the identity guide); do not start this one while that is up.
* `wifi stats` (Pico 2 W): The CYW43439's health: the receive ring, frames over the bus each way, faults the bus latched (FIFO overflow and underflow, command and data errors), frames the receive path dropped and why, transmit-credit waits, and the chip's own packet counts where its firmware has them. `wifi probe`, `wifi join SSID PSKHEX`, `wifi leave`, `wifi recover`, `wifi trace on|off` bring the radio up by hand, join and leave, and debug it.

### Randomness & Cryptography
* `randtest [bits]`: Measures the hardware entropy source and names it: on the RP2350 the TRNG (output bits, the time one 192-bit sample takes, and its health-test counts since boot) and the ring oscillator; on the ESP32-P4 and ESP32-C6 the RNG register's words. Prints `RANDTEST_OK` or `RANDTEST_WEAK`. On a board whose source is off or failed (QEMU, a C6 with its radio down, a P4 whose ADC noise source did not start, an RP2350 TRNG that failed its health tests), key generation and signing refuse.
* `cryptotest`: Known-answer tests of the primitives behind signed updates, TLS and protected 9P sessions: SHA-1, HKDF-SHA-256, X25519, ChaCha20-Poly1305 and P-256 ECDSA/ECDH, against their standards' vectors, plus tamper checks. Key generation and signing are skipped (and said to be) without hardware entropy. Prints `CRYPTO_SELFTEST_OK` or `CRYPTO_SELFTEST_FAIL`.
* `cryptotest bench`: The time of each operation on this chip, in microseconds.
* `hmacselftest`: SHA-256 and HMAC-SHA-256 against FIPS 180-4 and RFC 4231.
* `privselftest`: Checks that an `ea` tool call cannot run privileged commands or Lisp primitives, nor write under `system/etc/`, and that the same calls work outside one. Prints `PRIV_SELFTEST_OK` or `PRIV_SELFTEST_FAIL`.

### Hardware Bring-up & Diagnostics
These are bring-up aids for developers. They read hardware state, and some change it.
* `peek ADDR [COUNT]` / `poke ADDR VALUE`: Read words from, or write a word to, a physical address (hex, `0x` optional). M-mode and unrestricted: a stray poke can hang the chip.
* `psram [test [MB]|bench]` (ESP32-P4): PSRAM size, speed and tuning window; `test` runs a pattern test, `bench` measures bandwidth.
* `smpinfo`, `clocks` (ESP32-P4, RP2350): second-hart state and the measured clock tree. `cpufreq MHz` (ESP32-P4 only) changes the CPU clock under live drivers: re-check measurements from a clean boot. `xipcycle` (RP2350) exercises the flash/XIP park handshake without writing; `flashinfo` (ESP32-P4) reports the flash chip.
* `intrdump`, `clicdump` (ESP32-P4): Interrupt controller state. `pmpinfo` reports usable PMP regions and granularity; `pmpdump` dumps every PMP register.
* `i2cdiag` (ESP32-P4): I2C controller diagnostics; `emac scan`: Ethernet PHY addresses found on the MDIO bus.
* `kbd [desc|on|off]`, `kbdlog [s]`: The USB keyboard host: state, descriptors, switching it off to leave the port to `usbprobe`, and a log of reports for `s` seconds (default 30).
* `usbprobe`, `usbprobe loop`, `boardprobe`, `uart1pins`, `uart1test [ms]` (RP2350): PIO-USB and UART1 pin and loopback probes for board bring-up.
* `led R G B` / `led off` (ESP32-C6): The on-board RGB LED, 0-255 per channel.
* `ramscreen W H`: Resizes the RAM screen (boards without a panel), e.g. `ramscreen 1024 600` to test layouts under QEMU. The screen built so far is dropped.
* `vterm`: Lists the virtual terminals. `radioosi` (ESP32-C6): Self-test of the radio blob's OS interface.
* `priostress`, `smpload`, `pinall`, `flashpark`: Scheduler and SMP test loads (equal-priority fairness, background load on every hart, pinning every task to hart 0, the flash-write park handshake).

---

## 7. Built-in Applications & Languages

From `lsh`, you can immediately invoke native applications:
* `lisp`: Starts the interactive Scheme/Lisp REPL environment ([Lisp scripting](cas.md)).
* `e [file]`: Opens the full-screen Emacs editor ([editors](editors.md)).
* `ed [file]`: Opens the teletype line editor ([editors](editors.md)).
* `chess`: Launches the chess computer UI ([chess](chess.md)).
* `moonbase`: Launches Moonbase Lambda, a 2D isometric lunar city-builder simulation.
* `ceres` (or `scroller`): Launches Ceres Alpha, a 2D horizontal arcade scroller set in the subterranean cavern network of dwarf planet Ceres. Controls: Arrow keys or `W`/`A`/`S`/`D` (thrust), `Space` (laser), `B` / `K` / `Enter` (bomb), `P` (pause), `R` (retry), `Q` / `Esc` (quit).
* `zmachine [story]`: Launches the Z-machine Version 3 interactive fiction interpreter with canvas auto-mapping ([interactive fiction](zmachine.md)).
* `cc <src> <dst>`: Compiles a C11 source file on-device to a native RISC-V ELF binary using `chibicc` ([C programming](../dev/on_device/c_programming.md)).
* `exec <elf>`: Loads and executes a RISC-V ELF binary confined to a U-mode PMP memory domain.

---

## 8. On-Device Help Commands

* `help`: Displays a quick command reference categorized by topic.
* `help test`: Displays internal hardware verification and self-test commands.
* `help <topic>` or `man <topic>`: Opens the interactive topic viewer (e.g. `man editors`, `man ribbon`, `man cas`).

---

**Navigation:** [<- User Manual](index.md) | [Documentation Index](../README.md)
