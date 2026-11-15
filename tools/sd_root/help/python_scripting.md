# Python Scripting & `lugal.*` API Reference

[Documentation](index.md) > [System Programmer Manual](dev.md) > **Python Scripting (`mpy`)**

---

This reference documents the LugalOS MicroPython runtime (`user/mpy/`), its U-mode execution model, and the native `lugal`, `machine`, and `socket` modules. For a user introduction, see [MicroPython on LugalOS](python.md).

---

## 1. Execution Model & Soft-Float ABI

`mpy.elf` is a separately linked user-space ELF binary running in **U-mode** under the target board's memory domain (`PMP` on NOMMU targets, `Sv39` page tables on VisionFive 2 and `rv64`).

* **Software Floating-Point:** LugalOS kernels boot with `mstatus.FS = 0` (hardware FPU disabled) and do not save/restore `f0`-`f31` on context switches. `mpy.elf` uses `MICROPY_FLOAT_IMPL_FLOAT` with integer-ABI soft-float (`-mabi=ilp32` / `-mabi=lp64`), vendored MIT `libm`, and `-lgcc` helpers. Standard Python `1/2`, `math.*`, `json` floats, and `time.sleep()` work identically across FPU-less microcontrollers and 64-bit workstations.
* **GC Heap & Stack:** `mpy.elf` reserves a 96 KB static GC heap (`MPY_GC_HEAP_BYTES`).

---

## 2. `lugal` Module Reference

```python
import lugal
```

### `lugal.sensor` -- Environmental Sensors

* `lugal.sensor.list() -> list[str]` -- returns the names of active sensors registered with `sensor_hub` (e.g. `['bme280', 'ina226']`).
* `lugal.sensor.read(name_or_kind) -> dict` -- reads the latest sample by device name or sensor kind; returns a dict of integer/scaled fields (e.g. `{'pressure': 95390, 'humidity': 3667}`).
* `lugal.sensor.read_dev(name) -> dict` -- reads a specific named sensor device.
* `lugal.sensor.age(name) -> int` -- returns sample age in milliseconds.

### `lugal.canvas` -- Graphical Canvases

* `lugal.canvas.open(title, width, height) -> int` -- opens a window canvas in the Ribbon / workstation compositor and returns its canvas handle `cid`.
* `lugal.canvas.close(cid)` -- closes the canvas window.
* `lugal.canvas.size(cid) -> (w, h)` -- queries canvas dimensions.
* `lugal.canvas.rgb(r, g, b) -> int` -- packs 8-bit RGB into a color value.
* `lugal.canvas.fill(cid, color)` -- clears the canvas.
* `lugal.canvas.pixel(cid, x, y, color)`
* `lugal.canvas.rect(cid, x, y, w, h, color)` -- filled rectangle.
* `lugal.canvas.frame(cid, x, y, w, h, color)` -- 1 px rectangle outline.
* `lugal.canvas.line(cid, x0, y0, x1, y1, color)`
* `lugal.canvas.circle(cid, cx, cy, r, color)`
* `lugal.canvas.text(cid, x, y, str, color[, bg])`
* `lugal.canvas.swap(cid)` -- presents the back buffer to the display.

### `lugal.ui` -- Immediate-Mode `ugui` Windows

* `lugal.ui.open(title, w, h) -> int` / `lugal.ui.close(wid)` / `lugal.ui.size(wid) -> (w, h)`
* `lugal.ui.begin(wid)` / `lugal.ui.end(wid)` / `lugal.ui.fill(wid, color)`
* Widgets (return interaction state / updated value):
  * `lugal.ui.label(wid, x, y, text)`
  * `lugal.ui.button(wid, x, y, w, h, text) -> bool`
  * `lugal.ui.checkbox(wid, x, y, text, checked) -> bool`
  * `lugal.ui.toggle(wid, x, y, text, on) -> bool`
  * `lugal.ui.slider(wid, x, y, w, min, max, val) -> int`
  * `lugal.ui.progress(wid, x, y, w, val, max)`
  * `lugal.ui.textfield(wid, x, y, w, id, text) -> str`
  * `lugal.ui.section(wid, x, y, w, title)`
  * `lugal.ui.disable(wid, flag)`
* `lugal.ui.event(wid)` / `lugal.ui.text(wid, id)` / `lugal.ui.set_text(wid, id, s)`

### `lugal.chan` -- Raw Kernel IPC Channels

* `lugal.chan.call(endpoint, req_bytes[, resp_max]) -> bytes` -- sends a synchronous copy-always request to a named kernel/driver channel (e.g. `"uart"`, `"i2c"`, `"net"`).

### `lugal.http_request` -- HTTP / HTTPS Client

* `lugal.http_request(method, url[, headers[, body]]) -> (status, headers_str, body_bytes)` -- delegates to the kernel `"net"` channel (`http_request()`), supporting TLS 1.3 HTTPS verified against `/sd0/system/etc/ssl/roots.bin` (82 trusted roots, RSA-PSS & ECDSA P-256). Used internally by the frozen `urequests` module.

---

## 3. `machine` and `socket` Modules

* `machine.I2C(bus)`: `scan()`, `writeto(addr, buf)`, `readfrom(addr, nbytes)`, `readfrom_mem(addr, memaddr, nbytes)`, `writeto_mem(addr, memaddr, buf)`.
* `machine.Pin(id, mode)`: `machine.Pin.IN`, `machine.Pin.OUT`, `pin.value([v])`.
* `socket`: TCP/UDP sockets bridged over the kernel `"net"` channel (`AF_INET`, `SOCK_STREAM`, `SOCK_DGRAM`, `getaddrinfo()`, `connect()`, `send()`, `recv()`, `close()`, `settimeout()`).
