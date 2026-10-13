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
* `date YYYY-MM-DD HH:MM:SS`: Sets system time and synchronizes the battery-backed hardware RTC (DS1307/DS3231).
* `rtc`: Reports RTC chip status, oscillator health, and temperature sensor readback.

---

## 4. Kernel Logging & Diagnostics

LugalOS features a ring buffer kernel logging subsystem ([logging](doc/dev/kernel/logging.md)):
* `cat /proc/kmsg`: Reads current kernel logs.
* `klog`: Displays kernel log statistics, total emitted bytes, and attached output sinks.
* `klog detach console`: Detaches the interactive console from kernel log broadcasts (silences background driver logs during interactive typing).
* `klog attach console`: Re-enables kernel log echoing to the interactive console.

---

## 5. Built-in Applications & Languages

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
