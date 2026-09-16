# The hardware seams of LugalOS

**Maintained inventory. G0, `plan/phase30_driver_framework.md`.**

Every place this tree crosses from portable code into something a particular
chip does. One row per seam: what declares it, what implements it, and what
the ESP32-P4 bringup (phase 27) cost it — because a seam that survived a
second platform unchanged is a seam with evidence behind it, and that is the
only kind worth trusting.

The companion document is `drivers/README.md`, which states the categories
below and the rule for when to extract a shared implementation. Read that one
before writing a driver; read this one before adding a *board*.

**How to maintain it:** a new backend adds an entry to the Implementations
column, never a row. A new row means a new seam, which is a design decision
and belongs in a phase plan first. If a row ever names a file that does not
exist, this document is wrong — fix it in the same commit that moved the file.

---

## 1. Controller seams (category B)

The questions every platform must answer. Narrow on purpose, and all of them
predate phase 27.

| Seam | Declared in | Implementations | What the ESP32-P4 cost it |
|---|---|---|---|
| Which IRQ fired | `kernel/include/kernel/devirq.h` | `kernel/devirq.c`, dispatched from `arch/riscv/common/trap.c` | Gained a return value on `devirq_dispatch()` in E3 — "unhandled" means something different on a level-triggered CLIC than on a claim/complete PLIC. **No caller changed.** |
| Trap entry, interrupt enable | `arch/riscv/include/arch/trap.h` | `arch/riscv/common/trap.c`, four arms: `CONFIG_BOARD_RP2350` (Hazard3), `CONFIG_BOARD_ESP32P4` (CLIC), `CONFIG_MODE_S` (rv64 PLIC), else (rv32 PLIC) | One new `#elif` arm, ~200 lines, plus `arch/riscv/include/arch/esp32p4_intr.h`. Contract unchanged. |
| What time is it | `kernel/include/kernel/time.h`, `kernel/include/kernel/ticker.h` | `kernel/time.c`, `kernel/ticker.c`, same four-arm shape | One arm each. The P4 tick runs off the CLINT (E4). |
| How memory maps | `arch/riscv/include/arch/vmm.h` | `arch/riscv/rv32_nommu/vmm.c`, `arch/riscv/rv64_mmu/vmm.c` | **Nothing.** `rv32_nommu/` was reused verbatim. |
| How a task is isolated | `arch/riscv/include/arch/pmp.h`, `arch/riscv/include/arch/umode.h`, `kernel/include/kernel/mem_domain.h` | `arch/riscv/common/pmp_probe.c`, `umode.c`, `umode.S`, `mem_domain.c` | **Untouched.** The P4's PMP is a standard one. |
| Board facts the linker knows | the four scripts in `linker/` | `qemu-rv32.ld`, `qemu-rv64.ld`, `rp2350.ld`, `esp32p4.ld` | See §3 — this seam is newer than the others and phase 27 is what produced it. |

## 2. Device-class contracts (category D)

What a *kind* of device promises, independent of which chip provides it.
Already abstracted, and they held.

| Contract | Declared in | Implementations |
|---|---|---|
| Block device | `drivers/include/drivers/block.h` | `virtio_blk.c`, `spisd_rp2350.c`, `flashdisk.c`, `ramdisk.c`, `idstore_rp2350.c`, `virtio_blk_id.c` |
| Network interface | `net/include/net/netif.h` (`net/netif.c`) | `virtio_net.c`, `enc28j60_rp2350.c`, `cyw43_rp2350.c`, `uart_net.c`, `loopback_net.c`, `emac_esp32p4.c` |
| Console | `kernel/include/kernel/console.h` | `uart_16550.c`, `uart_rp2350.c`, `uart_esp32p4.c`, `usb_cdc.c`, `virtio_console.c` |
| Device registry (`/dev`) | `kernel/include/kernel/device.h` | one, `kernel/device.c` — the registry itself is the abstraction |
| Channels / endpoints | `kernel/include/kernel/chan.h` | one, `kernel/chan.c` |

`netif_register()` has taken ENC28J60, CYW43439 and virtio without changing,
and phase 28 plugs the P4's EMAC into it unchanged. Nothing is owed here.

**The prediction held (2026-09-16, phase 28 Z5/Z7).** `net/include/net/netif.h`
is byte-for-byte unmodified across the whole of phase 28, and the P4's EMAC --
the first *on-die* MAC this tree has met, as against three parts on a bus --
registered through it without an argument. `kernel/board.c` needed one
`DEV_KIND_NETIF` entry shaped exactly like the ENC28J60's, and everything
above the seam (ARP, IP, TCP, the 9P server) worked on the first wire without
being told a new chip existed.

Two details are worth keeping, because they are where the seam could
plausibly have failed and did not:

* **`netif_register()` filling `.mac` from the node identity when a driver
  leaves it zeroed** is what let the EMAC start out on a derived address and
  later switch to the eFuse one (Z5) with no change at this layer.
* **The `poll()`-must-not-block rule** turned out to *determine the driver's
  whole shape* -- it is why the EMAC has no ISR and is not a driver task
  (phase 28 Z6). A contract that decides a driver's structure rather than
  merely accepting it is doing more work than a header usually does, and that
  is an argument for category D as a whole.

The one thing phase 28 *did* need from this layer was nothing at all, which
is the outcome this section predicted.

## 3. The linker-symbol contract

Board layout facts, stated by the board's own script instead of by an `#if`
in shared code. Established by G1 (2026-09-11) after phase 27 showed the
alternative: two `CONFIG_BOARD_ESP32P4` arms in files that no board should
have to edit, both correct, both in the wrong place.

Every script in `linker/` defines all of these. A new board satisfies the
contract or fails to link, which is the point.

| Symbol | Means | Read by |
|---|---|---|
| `_ram_start`, `_ram_end` | the writable region | `kernel/meminfo.c`, `kernel/palloc.c` |
| `_bss_start`, `_bss_end`, `_data_start`, `_data_end`, `_sidata` | the image's own sections | `arch/riscv/common/entry.S`, `kernel/meminfo.c` |
| `_kernel_end`, `_heap_end` | the heap's bounds | `kernel/palloc.c` |
| `_stack_bottom`, `_stack_top` | the boot stack | `kernel/meminfo.c` |
| `_image_bytes` | **how much RAM the resident image occupies.** A size, not an address — and computed by the script, because layout is what a linker script knows and C does not. The P4's image is not one contiguous run from the bottom of RAM; three others are. | `kernel/meminfo.c` |
| `_inst_lo`, `_inst_hi` | the window the fatal-trap handler may read a faulting instruction back from without risking a second fault inside the handler reporting the first. RAM on three targets; flash *and* RAM on RP2350, which runs `.text` in place and `.ramfunc` from SRAM. | `arch/riscv/common/trap.c` |
| `_utext_start` | the U-mode text page granted to driver domains | `arch/riscv/common/mem_domain.c` and every U-mode driver |

These assignments live **below** each script's memory-map block, not beside
`_kernel_end`: they read `_ram_start`/`_ram_end`/`_flash_start`, and a linker
script assignment sees only symbols already assigned above it. The first draft
of G1 had them above and happened to resolve correctly, which is not the same
as being correct.

Per-board extras are legitimate and stay per-board: `rp2350.ld` alone defines
`_blktext_start`, `_st7735text_start`, `_clocktext_start`, `_usbtext_start`
(one `.utext` section per U-mode driver domain) and the `__scratch_x/y` and
`__ramfunc` pairs the RP2350 boot path needs.

## 4. Bus arbitration (category E)

Added by phase 30 itself, 2026-09-11. Only exists where a bus is 1:n.

| Seam | Declared in | Implementations |
|---|---|---|
| I2C transfer | `drivers/include/drivers/i2c_bus.h` | `drivers/i2c_bus.c`, three controller arms: RP2350 (Synopsys DW_apb_i2c), ESP32-P4 (Espressif command list), and a stub for targets with no controller |

Devices on the bus — `drivers/i2c_rtc.c` (DS3231/DS1307), `drivers/at24c32.c`
(EEPROM), `drivers/bme280.c` (sensor) — call `i2c_xfer()` and own no
registers. Before the split the bus lived inside the RTC driver and the EEPROM
driver had its own copy of the RP2350 controller registers.

**SPI has no such seam, deliberately.** Every SPI controller on every persona
here drives exactly one device, by board design (`cmake/board-rp2350.cmake`
says so on both SPI0 and SPI1). A bus abstraction there would be arbitration
for contention the board files go out of their way to prevent.

## 5. What is deliberately not a seam

Register-level access to a specific peripheral (category A). `UART_CONF0_SYNC`
on the P4, IO_MUX `MCU_SEL`, `clicintattr`, `UART0_IMSC` on the RP2350.

Never shared, across chips or across vendors. The failure mode of sharing
these is not a merge conflict, it is a plausible-looking wrong value: phase 27
wrote `UART0_SYS_CLK_EN` and `UART0_APB_CLK_EN` as bits 24 and 25, reasoned
from where UART0 sits in the peripheral list, and they are actually 18 and 7,
in two different registers that order their fields differently. There was no
position to infer. A HAL that made "the UART clock gate" a portable concept
would have made that error *easier* to write.

## 6. The standing conclusion

Phase 27 §0 set out to falsify the seams by building a second platform on
them. The result is the two tables above: category B took one `#elif` arm per
row and one refinement with evidence behind it; category D took nothing at
all. What leaked was not the arch seams but the layer above them — the
driver-as-task pattern, which is category C, has no board-specific content,
and is what `plan/phase30_driver_framework.md` is for.

**Adding a HAL now would be adding a second one, on top of a first one that
passed its test.**
