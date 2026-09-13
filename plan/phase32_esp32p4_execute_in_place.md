# Phase 32 — The P4's code moves to flash, and stops costing RAM

**Status: PLANNED, 2026-09-12. Blocks `plan/phase28_esp32p4_ethernet.md` at
Z4.** Phase 28 is complete through Z3 and cannot continue: the P4's heap is at
exactly its 128 KB floor, margin +0, and the next line of code anywhere in
this kernel fails the assert in `linker/esp32p4.ld`. Phase 28 §Z3a records how
that happened and lists the options; this is the one chosen, and it is the one
that removes the problem rather than deferring it.

**Milestone letter: `U`.** A–G, H–N, P–T and V–Z are spoken for across
`plan/`, leaving O and U, and `O` is unusable in a terminal beside a zero.
**`U1`..`U6` have nothing to do with U-mode** — an unfortunate collision in a
tree where U-mode is discussed constantly, and worth stating once here rather
than being re-derived by every reader. The letter pool is now effectively
exhausted; phase 29 will need a scheme rather than a letter.

## 0. The measurement, and why it is decisive

The P4 image today, from `riscv64-elf-size` on `build/esp32p4/lugalos.elf`:

| Section | Bytes | Where | Costs heap? |
|---|---|---|---|
| `.text` | 186 200 | L2MEM | **yes** |
| `.rodata` | 68 904 | L2MEM | **yes** |
| `.utext` | 4 096 | L2MEM | yes |
| `.data` | 372 | L2MEM | yes |
| `.bss` | 154 176 | LOWRAM, NOLOAD | **no** |

The heap is `_heap_end − _kernel_end`, and `_kernel_end` follows the loadable
sections. So on this board **code costs heap and buffers do not** — the
reverse of the intuition every RP2350 milestone in this tree has built, where
`.text` lives in flash and costs no RAM at all, and where
`rp2350_memory_budget`'s lesson is that `.bss` growth steals from palloc.

Moving `.text` and `.rodata` into flash-mapped address space frees
**255 104 bytes, 249 KB**. The heap goes from 128 KB to roughly 375 KB — not
an optimisation but a change of category, and enough that no foreseeable
milestone has to think about it again.

`linker/esp32p4.ld` predicted this in E2 and named the fix in its own header:

> On RP2350 the 227 KB of `.text` lives in flash and costs no RAM at all. Here
> it is RAM-resident because the image is RAM-*loaded*, which is E2's whole
> boot story. E6 boots this image from flash instead, at which point the text
> goes back to being executed in place and this script gets most of that
> 227 KB back.

E6 did not do it, and said so plainly — *"it needs a second-stage bootloader
and buys nothing any current milestone wants"*. Phase 28 Z4 wants it.

## 1. What execute-in-place means on this chip

Three facts, and the first two are smaller than they sound.

**The window.** Flash maps into `0x40000000`–`0x44000000`, 64 MB, with
instruction and data space unified (`soc/soc.h`: `SOC_IROM_LOW` and
`SOC_DROM_LOW` are the same address). So `.text` and `.rodata` share one
mapped region and there is no I/D split to maintain.

**The mapping is a ROM call, not a new driver.** `Cache_FLASH_MMU_Set` at
`0x4fc00518` (IDF `esp32p4.rom.ld`):

```c
int Cache_FLASH_MMU_Set(uint32_t sensitive, uint32_t vaddr, uint32_t paddr,
                        uint32_t psize, uint32_t num, uint32_t fixed);
```

64 KB pages, `num` of them, returning 0 or an error code for misalignment or
an out-of-range vaddr. This is the same choice `drivers/flash_esp32p4.c` and
`arch/riscv/common/trap.c` already make for flash access and L2 cache
configuration, for the reasons phase 27 gives: these routines are what
Espressif's own bootloader uses, and the alternative is new code whose failure
mode is a corrupted flash.

**The L2 cache stops being free.** `esp32p4_l2_cache_shrink()` cuts it to
128 KB to buy RAM (E7), which was pure gain while nothing was cached —
the L2 caches *external* memory and there was none in use. Once `.text` is
fetched over SPI, L2 cache size becomes a performance parameter, and 128 KB
against 186 KB of text means the working set does not fit. **This is a
trade-off this phase creates and must measure**, not a detail. It is also why
U5 exists.

## 2. The hazard that makes this a phase and not a patch

**Writing flash while executing from it.** Phase 27 §3.6 saw this coming:

> `/flash0` on the P4 means writing to the same SPI flash the CPU is executing
> from through a two-level cache. RP2350 has the identical hazard and
> `drivers/flash_rp2350.c` already solves it; the P4 version differs in the
> mechanics ... and not in the shape. Precedent exists; the registers do not,
> yet.

Today there is no hazard at all, because nothing executes from flash. After
U1 everything does, and an erase or program cycle makes the MSPI unavailable:
any instruction fetched from flash during it returns garbage, and the failure
is a jump into nothing rather than an error code.

So every path that writes flash — `drivers/flash_esp32p4.c`, its callers in
`drivers/flashdisk.c`, and **everything they call, transitively, including
the interrupt handlers that might fire** — has to be RAM-resident with the
cache disabled for the duration. That transitive closure is the work, and
getting it wrong corrupts a filesystem rather than printing a wrong number.

This is the reason U3 is its own milestone and is sequenced before U4.

## 3. Milestones

### U0 — The target memory map, decided and written down

No code. Decide and record: which sections move, which cannot, where the OS
image lives in flash, and what the heap figure becomes.

`.utext` **does not move.** It is one page, self-aligned, granted to U-mode by
a single PMP NAPOT entry (`kernel/board.c`'s `board_text_region()`). Whether
PMP can grant a flash-mapped range is a question this phase does not need to
answer, and 4 KB is not worth answering it for.

`cmake/flash_layout_esp32p4.cmake` gains an OS-image segment, and the shape of
the map changes with it.

**Decided 2026-09-12: LugalOS takes the whole 16 MB. The Waveshare factory
demo is retired on purpose.** That file's existing comment requires exactly
this to be a decision rather than a side effect — *"goes below 0x00D10000 only
by first retiring the factory image on purpose"* — and it now is one. The
backup at `~/gith/esp/p4nano-factory-flash/` stops being a safety net that
constrains the layout and becomes what it should have been all along: an
archive, restorable with `esptool write_flash` if anyone ever wants the demo
back.

What that buys is not space — 255 KB would have fitted in the old margin
several times over — but *coherence*. A map laid out around an image nobody
runs would have the OS image at `0x00E80000` for no reason a reader could
reconstruct, with a 13 MB hole above address zero labelled NEVER TOUCHED. U0
lays the map out as if the board were ours, because it is:

    0x00000000  second-stage bootloader (U4)
    ...         OS image (.text + .rodata)
    ...         flash filesystem
    ...         spare, contiguous and at the top where growth is cheap

with the same 64 KB alignment rule and the same "these numbers are the only
definition" property the file already has.

**Done when:** the layout is in the board and flash-layout files with the
arithmetic shown, and the predicted heap figure is written down so U5 can be
checked against a number chosen in advance rather than one discovered after.

---

**DONE, 2026-09-12.**

**Recovery first, before anything that could need it.** `p4run.py
--reset-test` on this host:

```
run      (RTS pulse)          reset: True  rst:0x1 (POWERON),boot:0x30f (SPI_FAST_FLASH_BOOT)
download (RTS + DTR strap)    reset: True  rst:0x1 (POWERON),boot:0x307 (DOWNLOAD(USB/UART0/SPI))
```

Download mode is reached through ESP_EN and the strapping pin, so it does not
depend on flash contents: **whatever U4 writes, the board comes back.** The
factory backup was re-verified against its SHA256SUMS at the same time
(`p4nano-factory-16mb.bin: OK`), and it records the board's factory MAC
`80:f1:b2:d2:f0:53` — which phase 28's Z5 can check its eFuse read against.

**The ROM's offset is 0x2000**, not a choice:
`esp_rom/esp32p4/esp_rom_caps.h` says `ESP_ROM_BOOTLOADER_OFFSET_FLASH
(0x2000)` "determined by the ROM bootloader", and
`bootloader/Kconfig.projbuild` adds "It's not configurable in ESP-IDF". The
retired factory image agrees: its partition table sat at 0x8000 with nvs at
0x9000, leaving exactly 0x2000..0x8000. **That offset is inside what the
factory image occupied, which is why retiring it was not optional once U4 was
in scope.**

**The map**, now in `cmake/flash_layout_esp32p4.cmake` with the reasoning
attached:

```
0x00000000  reserved, below the ROM's offset                     8 KB
0x00002000  second-stage bootloader (U4)                        56 KB
0x00010000  OS image: .text + .rodata, executed in place         1 MB   (4.1x present need)
0x00110000  flash filesystem /flash0                           512 KB
0x00190000  spare, contiguous                                 14.4 MB
```

64 KB alignment throughout is a *hardware* requirement here and not only the
house style: `Cache_FLASH_MMU_Set` refuses a physical address not aligned to
its 64 KB page size.

**The filesystem moved and was reflashed**, so the tree is not left in a state
where `/flash0` is broken: `uv run tools/p4flash.py` wrote it to 0x00110000,
"Hash of data verified", and `ls /flash0` lists its contents from the new
address.

**`tools/p4flash.py`'s guard was rewritten rather than removed.** It refused
the new address — correctly, it was protecting the factory image. Its floor is
now `WRITABLE_FLOOR = 0x00110000` and protects what actually must not be
damaged by a filesystem write: the bootloader and the XIP OS image. It stays a
duplicated constant rather than reading the layout, for its original reason —
this script writes flash with no kernel guard in front of it, so a layout edit
alone must not be able to widen what it will destroy.

**The prediction U5 is checked against:**

| | bytes | |
|---|---|---|
| `.text` + `.rodata`, moving to flash | 255 104 | 249.1 KB |
| `.utext` + `.data`, staying in RAM | 4 468 | `.utext` is PMP-granted, §U0 |
| heap now | 131 072 | 128.0 KB, floor exactly |
| **heap after, before the stub** | **385 024** | **376.0 KB** |

So **U5 should find between 368 and 376 KB**, the gap being the stub and any
rounding; a figure outside that range means something did not move and is a
bug rather than a disappointment.

`.bss` is 154 176 B (150.6 KB) and lives in LOWRAM's 252 KB throughout,
unaffected either way. Worth noting for later: after U1 the binding constraint
on `.bss` is LOWRAM, not the heap, and there is about 85 KB of headroom there
once the 16 KB boot stack is counted.

### U1 — The linker script, and an image that has two halves

`.text` and `.rodata` link at `0x40000000 + offset`; `.data`, `.bss`, the
stacks and `.utext` stay where they are. The build emits two artifacts: the
flash-resident half, and the RAM-resident half that still arrives by
`load-ram`.

**Done when:** the ELF's section addresses are in the two ranges and nothing
else changed; the board still boots by `load-ram` *without* the flash half
being used, because U1 changes where things are linked and U2 is what makes
the board actually fetch them. (If that is not separable, say so and merge the
two — but try, because a failure that spans both is much harder to read.)

---

**DONE, 2026-09-13 — and the "still boots by load-ram" clause was wrong.**

It is not separable in that direction, and the reason is worth keeping rather
than quietly dropping: once `.text` links at `0x40000000`, `load-ram` has
nowhere to put it. `esptool` writes segments to their link addresses and jumps
to the entry point, and both are now in a window that is flash-mapped rather
than RAM. So an image built by U1 *cannot* be started until U2's stub maps it.

The milestones stayed separable in the way that actually mattered, though —
**U1 is verified statically, U2 by booting**, so the two have different
symptoms:

| | |
|---|---|
| `.text` | `0x40000000`, 186 200 B |
| `.rodata` | `0x4002d758`, 68 904 B — contiguous with `.text` |
| `.utext` | `0x4ff40000`, 4 096 B — stays in RAM, §U0 |
| `.data` | `0x4ff41000`, 372 B — stays, and `_sidata == _data_start` still holds |
| `.bss` | `0x4ff00200`, LOWRAM, unchanged |
| `_start` | `0x40000000` — the bottom of the window, so the stub's target is a fixed address |
| `_xip_pages` | **4** — computed by the linker, not by the stub |

**The heap is 385 024 B = 376.0 KB, against U0's prediction of 376.0 KB.**
Exact, which is the point of having predicted it: a figure in that range means
everything that should have moved did.

Four things learned:

* **`objcopy -O binary` produced a 267 MB file.** With sections 255 MB apart
  it spans from the lowest to the highest and pads the gap. Not a size
  annoyance but a wrong artifact, so the P4 now extracts the two halves by
  section — `lugalos-xip.bin` (255 104 B, exactly `_xip_end − _xip_start`) and
  `lugalos-ram.bin` (4 468 B, exactly `.utext` + `.data`). Both were checked
  byte-for-byte against the ELF at the start of `.text` and across the
  `.text`/`.rodata` boundary, and the flash offset is `vaddr − 0x40000000`
  with no adjustment.
* **We inherit no 0x20 offset from ESP-IDF.** Its linker script maps flash at
  `0x40000020`, and the reason is its own image format: a 0x18 file header
  plus an 0x08 segment header, offset so that the MMU's real constraint
  (`paddr % 64KB == vaddr % 64KB`) holds. U2 maps a raw binary, so both ends
  are 64 KB-aligned and the constraint holds trivially. Copying the 0x20
  would have been a magic number with a false explanation.
* **The ASSERTs were inverted, not deleted.** The old one checked `.text` was
  *above* `0x4ff40000`; the useful check now is that it is *inside* the flash
  window, because a `.text` that quietly fell back to RAM would boot perfectly
  under `load-ram` and silently undo the entire phase. Two more were added:
  the XIP origin must be 64 KB-aligned, and the flash half must fit
  `LUGALOS_P4_OSIMAGE_SIZE`, which keeps the linker script and the flash map
  agreeing across two files.
* **The P4 hardware suite cannot run between U1 and U2**, and now says so
  instead of failing obscurely: it stops at "refusing to load lugalos.elf:
  its entry point is 0x40000000, which is not L2MEM". That is the expected
  state of this milestone, not a regression, and U2 restores it.
* **The un-loadable image hung with no output at all.** `elf2image` builds it
  happily (entry `0x40000000`, six segments), `load-ram` writes into unmapped
  space and jumps there. `tools/p4run.py` now refuses any ELF whose entry is
  outside L2MEM and says why — a guard that stays correct after U2, since what
  `load-ram` delivers from then on is the stub, whose entry *is* in L2MEM.

### U2 — The stub: map flash, jump, and prove code is coming from it

A small RAM-resident loader that calls `Cache_FLASH_MMU_Set` for the OS
image's flash pages, enables the cache, and jumps to `_start`.

Delivery stays `load-ram` for this milestone — the stub and `.data` arrive
from the host exactly as today, and only `.text`/`.rodata` come from flash.
That is deliberate: it isolates "does XIP work" from "can the ROM boot us",
which are different questions with different failure modes, and it means U2
can fail without leaving the board unbootable.

**Done when:** the board boots and runs the existing shell with `.text`
fetched over SPI; `riscv64-elf-nm` shows the kernel's symbols at
`0x4000_0000`+; and the heap reports the figure U0 predicted. The P4 hardware
suite passes unchanged — every test in it is now also a test that XIP works.

### U3 — Writing flash while running from it

The hazard in §2. Identify the transitive closure of the flash-write path,
move it to a RAM-resident section, disable the cache across the operation, and
make the constraint structural rather than remembered — a section attribute
and, if it can be managed, a build-time check that nothing in that closure
landed in flash.

**Done when:** `/flash0` survives a write-heavy exercise with `.text` in
flash — the existing `test_flash0_mounted` plus something that actually
rewrites blocks — and the reasoning for *why* the closure is complete is
written down, not just asserted. A reviewer should be able to check the
argument rather than trust the result.

### U4 — Boot with no host at all

**In scope, 2026-09-12.** The ROM reads a second-stage image from flash and
jumps to it. U2's stub becomes that image, so the board powers on into
LugalOS with nothing attached.

Phase 28 does not strictly need this; **phase 29 does**, and that is the
reason it is in rather than deferred. A stratum-1 NTP server that requires a
laptop to boot is not a server, and every later appliance persona on this
board inherits the same requirement. Doing it here, while the boot path is
already open and freshly understood, is cheaper than reopening it later.

It remains the riskiest milestone, so it is sequenced last among the ones
phase 28 waits on, and U0–U3 are independently useful if it stalls.

Unknowns to settle first, listed because they are genuinely not yet known:
the ROM's expected second-stage offset on this part, its image format and
checksum, and whether a non-IDF image is acceptable to it. The answers are in
the ROM's boot code and in IDF's bootloader image format; U4 starts by
reading, not by writing.

**The recovery path is the real safety requirement**, and it replaces the
factory image as the thing that must not be lost. The board must remain
recoverable by `esptool` download mode (the CH343P's DTR/RTS reach ESP_EN and
the strapping pin — `tools/p4run.py` already drives them), so a bad
second-stage image is a reflash and not a brick. **That path is tested before
the first flash write that could need it**, not after.

**Done when:** power-on with no USB host reaches a shell on the console; the
download-mode recovery path has been exercised deliberately, from a
deliberately broken image, and worked.

### U5 — Reclaim, and the L2 cache trade

Raise the heap floor in `linker/esp32p4.ld` to reflect reality, and revisit
`esp32p4_l2_cache_shrink()`: 128 KB of L2 against 186 KB of text is a working
set that does not fit, and the RAM the shrink buys is no longer scarce. The
right size is now an empirical question — **measure it when we are there,
do not reason about it now** (2026-09-12, agreed).

**Done when:** the floor is raised with its new figure and the reason;
a measurement (the existing chess or `perft` workload is the obvious
instrument, being CPU-bound and already calibrated on this board) says what
the L2 size is worth; and E7's `esp32p4_l2_cache_shrink()` comment is updated
so it no longer describes a cost-free change.

### U6 — Documents

`plan/phase27_esp32p4_bringup.md` E6's "What E6 did not do" gets an
outcome. `linker/esp32p4.ld`'s header stops predicting this and starts
describing it. Phase 28 §Z3a records which option was taken and what the heap
became. The `esp32p4-l2-cache-steals-ram` memory needs the second half of its
story.

## 4. What could go wrong, stated in advance

* **XIP is slower than L2MEM, and the shell is the wrong instrument to notice
  with.** A human at a console cannot tell. Phase 24 and phase 29 care about
  microseconds, so U5's measurement matters more than it looks.
* **U3's closure is easy to get wrong and hard to see wrong.** A path that is
  *usually* not taken during a flash write — an interrupt handler, a `printk`
  under an unusual condition — fails rarely and corrupts when it does. Phase
  31's work means `printk()` is a ring append and cannot block, which helps;
  it does not make the code RAM-resident.
* **Recovery, not the factory image, is what must not be lost.** The factory
  demo is being retired deliberately (U0), and the backup at
  `~/gith/esp/p4nano-factory-flash/` is an archive. What would actually hurt
  is a second-stage image that neither boots nor leaves the board in download
  mode. Hence U4's ordering and its explicit "test the recovery path first"
  requirement.

## 5. Explicitly not in this phase

* **No PSRAM.** Still phase 27 §7's "not now", and the heap after U5 makes it
  less interesting rather than more.
* **No change to the RP2350 or QEMU boot paths.** This is one board's boot
  story. `linker/esp32p4.ld` and the P4 preset are the blast radius.
* **No revisiting `.text` size.** Phase 28 Z3a listed "trim `.text`
  elsewhere" as an alternative to this phase, not a companion to it. With
  249 KB recovered there is no reason to go looking, and doing both would
  muddy U5's measurement.
