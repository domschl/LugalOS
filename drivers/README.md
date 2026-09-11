# Writing a driver for LugalOS

**Read this before adding hardware support.** G0,
`plan/phase30_driver_framework.md`. The companion document is
`plan/hardware_seams.md`, the maintained inventory of every seam this tree
already has and what the ESP32-P4 bringup proved about each.

---

## The rule

**Extract at the third implementation, never at the second.**

Two copies is evidence that something recurs. Three is a design.

The corollary matters as much: **at the second implementation, do nothing but
note it.** When phase 27 added a second UART driver, extracting then would
have meant changing two drivers that worked, on two boards, to make room for
a third that had never run. Refusing was right. When E3 added the third, the
shape was obvious and the extraction was mechanical.

This rule is not a style preference. It is what would have caught the I2C bus
living inside `i2c_rtc.c` — by the time `at24c32.c` had its own copy of the
RP2350 controller registers, that was the third implementation and nobody
counted.

## Which kind of thing are you writing?

Hardware support in this tree falls into five categories. They want opposite
treatment, so decide which one you are in before you write anything.

### A. Registers of a specific peripheral — **never share**

`UART_CONF0_SYNC`, IO_MUX `MCU_SEL`, `clicintattr`, `UART0_IMSC`.

Different silicon, different registers. The failure mode of sharing is not a
merge conflict, it is a plausible-looking wrong value that costs a hardware
session to find.

Per-board files, per-board provenance, **recorded in the file**. Phase 27's
record: `UART0_SYS_CLK_EN` and `UART0_APB_CLK_EN` were written as bits 24 and
25, reasoned from where UART0 sits in the peripheral list, and are actually 18
and 7 — in two different registers that order their fields differently. There
was no position to infer.

> **Verify every bit mask against the datasheet or TRM page, and say in a
> comment where you got it.** Never infer a bit position from a plausible
> pattern. If the document is a PDF, render the page and count the bits.

### B. Controller seams — **already abstracted, extend the same way**

Which IRQ fired, what time is it, how memory maps, how a task is isolated.
About six of them, listed in `plan/hardware_seams.md` §1.

These are stable because they answer questions every platform must answer. Add
an `#elif` arm; do not add a seam. If you think you need a new one, that is a
design decision and belongs in a phase plan first.

### C. The driver-as-task pattern — **the actual shared code**

The serve loop, endpoint registration, the retry wrapper, the liveness check,
U-mode domain construction, the ISR→`task_unblock()` waiter slots.

Zero board-specific content, and currently nine near-identical copies of the
serve loop and seven of the U-mode domain. This is what
`plan/phase30_driver_framework.md` is extracting into a shared
`driver_task.c` (G2 — not written yet). Until it lands, copy the closest
existing driver and say in a comment which one you copied.

### D. Device-class contracts — **already abstracted, just implement one**

`include/drivers/block.h`, `net/include/net/netif.h`, the console binding,
the `/dev` registry. Implement the contract; do not widen it to fit your
device. `netif_register()` has taken
ENC28J60, CYW43439 and virtio unchanged, which is the evidence that it is the
right width.

### E. Bus arbitration — **only when the bus is 1:n**

**The number of devices on the bus decides the shape.**

* **1:1 — build nothing.** The device driver owns its controller. That is
  category A (its registers) plus category C (its task), and there is no
  shared resource to extract. Every SPI controller on every persona here
  drives exactly one device, by board design.
* **1:n — exactly one owner, one generic transfer, and the bus does not live
  inside any device's driver.** See `drivers/i2c_bus.c`: three controller
  arms behind `i2c_xfer()`, with `i2c_rtc.c`, `at24c32.c` and `bme280.c` as
  devices that own no registers at all.

## Things that will bite you

* **`-fno-jump-tables` is per translation unit, and required for any file with
  U-mode (`UATTR`) code.** A `switch` — or an `if`/`else` chain GCC's
  switch-conversion pass reconstructs into one — compiles to a jump table in
  ordinary `.rodata`, outside the `.utext` page the U-mode domain grants, and
  the indirect jump through it takes a load access fault. Add your file to the
  `set_source_files_properties(... -fno-jump-tables)` list in `CMakeLists.txt`.
  Found on real hardware, not in review.
* **`printk()` blocks.** Never call it mid-context-switch, mid-exit, under
  `g_sched_lock`, or in an ISR. `printk_critical()` exists for those.
* **A blocking call under a `spinlock_t` is a kernel bug, and now a detected
  one.** Spinlocks are checked leaves: `kernel/lock.c` faults if you block
  while holding one. If you need a lock you can hold across a block, that is
  `ylock_t`, and it participates in the wait-for graph. See
  `plan/phase31_concurrency_hierarchy.md`.
* **Fix every compiler and linker diagnostic, including pre-existing ones.**
  The tree builds ten presets with zero warnings and that is load-bearing: one
  tolerated warning hides the next real one.
* **A new board must satisfy the linker-symbol contract** in
  `plan/hardware_seams.md` §3, or it will not link. That is intentional.

## Before you claim it works

`tests/runner.py` is the QEMU suite and it must stay green. Hardware claims
need hardware: `tests/hw/test_rp2350.py` and `tests/hw/test_esp32p4.py`, and
`tests/hw/README.md` records the failure signatures already understood — read
it before debugging, because one of them is *"scan works and everything else
fails"*, and that is a bad ground, not your driver.
