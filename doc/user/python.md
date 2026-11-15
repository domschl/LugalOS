# MicroPython on LugalOS (`mpy` / `python`)

[Documentation](../README.md) > [User Manual](index.md) > **MicroPython (`mpy`)**

---

LugalOS includes **MicroPython v1.24.1** (`mpy.elf`), running as an isolated user-mode (`U-mode`) ELF program with software 32-bit floating-point, `math`, `json`, `uasyncio`, `urequests` (HTTP and HTTPS), `mip`, and native bindings for LugalOS sensors, canvases, UI widgets, and hardware buses.

---

## 1. Quick Start

Run Python from the `lsh>` shell using either `mpy` or `python` (they invoke the same `/sd0/system/bin/mpy.elf` or `/flash0/system/bin/mpy.elf` binary):

```text
# Interactive REPL (Ctrl-D to exit)
lsh> python

# Evaluate an expression or statement (-c)
lsh> mpy -c print(1+2,1/2)
3 0.5

# Run a script file
lsh> mpy /sd0/demos/sensor_log.py

# Run a frozen or installed module by name (-m)
lsh> mpy -m mip install pkg
```

> [!NOTE]
> The `lsh>` shell splits command lines on spaces. For one-liners with spaces inside `-c`, wrap the code in `exec("...\\x20...")` or put your script in a `.py` file on `/sd0/` or `/ram0/`.

---

## 2. Included Standard & Frozen Modules

* **Core & Math:** `sys`, `os`, `time`, `gc`, `math`, `cmath` (not enabled), `json`, `re`, `struct`, `binascii`, `hashlib` (`sha256`), `random`, `collections`, `io`.
* **Async & Networking:** `uasyncio` (frozen), `asyncio`, `select`, `socket`, `urequests` (frozen, supports HTTP & HTTPS via LugalOS TLS 1.3), `mip` (frozen package installer).
* **LugalOS Hardware & UI:** `lugal` (`lugal.sensor`, `lugal.canvas`, `lugal.ui`, `lugal.chan`, `lugal.http_request`), `machine` (`machine.I2C`, `machine.Pin`).

---

## 3. Built-in Demos

Two ready-to-run Python demos ship on `/sd0/demos/`:

* **Async Sensor Logger (`/sd0/demos/sensor_log.py`):**
  Uses `uasyncio`, `lugal.sensor`, and `json` to sample all detected environmental sensors (or a simulated reading when none are attached) and append JSON Lines to `/nvme0/sensor_log.jsonl` (VisionFive 2) or `/sd0/demos/sensor_log.jsonl`.
* **Live Canvas Chart (`/sd0/demos/canvas_chart.py`):**
  Opens a graphical canvas via `lugal.canvas` and plots live sensor telemetry on display-equipped boards.

---

## 4. Module Search Path & Installing Packages

MicroPython searches `sys.path` in order:

1. `.frozen` (built-in frozen modules: `uasyncio`, `urequests`, `mip`)
2. `/nvme0/lib/python` (VisionFive 2 NVMe SSD)
3. `/sd0/lib/python` (MicroSD card)
4. `/flash0/lib/python` (on-board flash)

On networked boards, install pure-Python packages directly to `/sd0/lib/python` (or `/nvme0/lib/python` if mounted) with `mip`:

```text
lsh> mpy -m mip install pkg-or-url
```

---

## 5. See Also

* [Python Scripting & `lugal.*` API Reference](../dev/on_device/python_scripting.md) -- full API documentation for `lugal.sensor`, `lugal.canvas`, `lugal.ui`, `lugal.chan`, `machine`, and `socket`.
* [HTTPS Certificates & Root Trust Stores](../admin/tls.md) -- how `/sd0/system/etc/ssl/roots.bin` (public Mozilla roots) relates to `host/ca` (`/dev/tlsca`, private CA).
* [Shell & Command Environment](shell.md) -- running ELF programs and backgrounding tasks.
