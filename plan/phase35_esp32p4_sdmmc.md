# Phase 35 — The P4 gets a disk: the microSD slot, and with it the network-storage persona

**Status: COMPLETE and verified on the board, 2026-09-18.** A 16 GB SanDisk
card mounts at `/sd0` over a 4-bit bus at 20 MHz, reads at **1547 KB/s**, and
survives a reboot with its files intact; `tests/hw/test_esp32p4.py` is 25/25
with the five new SD cases, and the QEMU suite is 368/368.

It did not work first time, and the two things that were wrong are the whole
value of this document: the module needs a clock divider **before** it is
reset (§8), and the CPU FIFO path the TRM documents **does not exist on this
silicon** (§1.2). The second one invalidated the design this driver was
originally written to.

Written alongside the code rather than before it, which is a departure from
phases 32–34 and is explained in §6.

**Milestone scheme: `35.1`, `35.2`, … —** phase 34's numbering, for the reason
phase 34 gives (a commit subject saying `35.3` tells a reader where to look;
`V2` does not).

## 0. What was missing

The ESP32-P4-NANO has a microSD socket wired to the chip's own SD/MMC host
controller, and until this phase LugalOS did not touch it. The board's entry in
`fs/vfs_server.c` said so in as many words:

> E2, plan/phase27_esp32p4_bringup.md: no block device on this board yet, so
> /sd0 stays unmounted and `df` reports it as such.

So the P4 had 9P over TCP (phase 28), an eFuse identity (phase 21's I7b
analogue), a writable `/flash0` of 512 KB (phase 32's E6) — and nothing to
serve. It was a network node with a filesystem the size of a floppy disk.

**This is the persona, not a feature.** The RP2350 personas are separated by
pin conflicts: the clock baseboard's GP10–13 are its LED matrix, so that
persona cannot have an SD card, and `cmake/board-rp2350-clock.cmake` says so.
The P4's SD pins (GPIO39–44) conflict with nothing on this board — not the
EMAC's (28–35, 49–52), not UART0's (37/38) — so there is no version of this
board that wants the socket built out. The one `esp32p4` preset grows the card
and is re-described as the network-storage persona; a second preset would be a
second build tree to keep green for no hardware reason.

## 1. What the chip offers, and the three traps in it

The controller is a Synopsys DesignWare mobile-storage host (TRM chapter 57),
the same family as the ESP32-S3's. Three facts about *this* chip decided the
shape of `drivers/sdmmc_esp32p4.c`.

### 1.1 The pads have no power until software gives them some

`SOC_SDMMC_IO_POWER_EXTERNAL` in IDF's `soc_caps.h` is one line and it means
this: GPIO39–45 are in the **VDDPST_5** IO domain, and on the NANO that domain
is fed from the chip's own LDO channel 4 (net `ESP_LDO_VO4`, ESP32-P4 pin 84).
The same rail reaches the card's `VDD` through R14 (0R) and Q1, an AO3401
P-channel switch whose gate is GPIO45.

So a driver that configures the controller perfectly, sets every pad to
function 0 and starts the clock gets **nothing**, with no error anywhere to
read: the pins have no supply. The rail is brought up in `sdmmc_power_on()`,
which follows TRM §15.4.2.9.3 with the two interrupt-waiting steps replaced by
a delay (this kernel routes no PMU interrupt), and which is the reason this
phase needed the schematic and not only the TRM.

The switch is settled *before* the rail, deliberately. Q1 conducts when its
gate is low and R27 (10K) holds it there, so the board's default is "SD
powered"; but if anything earlier left GPIO45 driving high, the switch would
open the instant the rail came up and the symptom would be a card that never
answers CMD0 on a board whose PMU registers all read back correctly.

### 1.2 The CPU cannot do the transfers, and the TRM says it can

**This section used to argue the opposite, and the argument was good.** TRM
§57.6 documents two paths into the controller's RAM, DMA and CPU, and gives the
CPU one a FIFO **32 bits wide and 512 entries deep** — 2 KB, four times a
512-byte block. A whole block therefore fits with room to spare, so the "drain
it or lose data" pressure that normally forces DMA does not exist at this size,
while the DMA path costs descriptor rings **and a cache discipline on every
caller's buffer** — and this chip's L1 is writeback, which is exactly the class
of bug that is invisible until it is silent data corruption. PIO looked like
the smaller thing that could be right, and the driver was written that way.

**It does not work.** Measured on the bench, 2026-09-18, on a card that was
otherwise behaving perfectly — it identified, reported its capacity, and
negotiated a 4-bit bus:

| Observation | |
|---|---|
| CMD17 completed | `DATA_OVER` set, no error bit in `RINTSTS` or anywhere else |
| `STATUS.fifo_count` | **128** — exactly one block of 32-bit words, so the data did arrive |
| 128 reads of `SDHOST_BUFFIFO_REG` | the **same** word every time (`0x6d9058eb`, the block's real first word), and `fifo_count` still 128 afterwards |
| reads across the whole `0x200–0x7FF` window | identical behaviour |
| `RX_WMARK` lowered to 0, so the FIFO request is raised continuously | no change |
| `fifo_reset` | count → 0, so the control path is fine |
| read block 1 instead | head becomes `0x41615252` — `"RRaA"`, the FAT32 FSInfo signature, which is precisely what sector 1 of this card holds |

The last two rows are what make this conclusive rather than a guess: the
FIFO's **write** side tracks the card exactly, and only the read port fails to
advance. Console output between reads rules out any load merging in the core.

So §57.6's CPU path is not wired on this silicon. ESP-IDF's agreement is
retrospective evidence — its SDMMC driver has only ever used the IDMAC, on
every ESP32 part, with no FIFO-mode branch to fall back to.

**This is the same class of error the project already recorded for this chip's
UART**, where the TRM describes `TXFIFO_CNT` as an RX count. The register *bit
diagrams* have been reliable every time; the prose around them has not. The
rule that follows: on this chip, a register diagram is provenance and a
paragraph is a hypothesis.

The driver now uses the IDMAC, with one cache-line-padded descriptor and a
512-byte bounce buffer — the buffer because cache maintenance is only safe on
a line-aligned, whole-line-sized region and a `block_dev_t` caller promises
neither. `drivers/emac_esp32p4.c` had already paid for both of those lessons on
this chip.

Two register notes that cost time: `SDHOST_CTRL_REG`'s `DMA_ENABLE` (bit 5)
and `USE_INTERNAL_DMA` (bit 25) **are absent from the P4's generated
`sdmmc_reg.h`**, which stops naming CTRL fields at bit 11 — they exist only in
`sdmmc_struct.h`. And `BMOD.DE` alone is not enough; both CTRL bits are set by
IDF's own `sdmmc_ll_enable_dma()` on this part.

### 1.3 There is no card-detect switch to read

The socket has one — pin 9, `CD` — and the schematic pulls it to `SD1_VDD`
through R10 and then routes it **nowhere**. No GPIO sees it.

So "is there a card" is answered by asking the card, and the controller's own
`CDETECT` input is tied to the GPIO matrix's constant zero, because the
controller otherwise refuses every command for a card it cannot see, with no
status this driver could report. Write-protect is tied the same way (the socket
has no WP switch either), and the SDIO interrupt input to a constant one, since
it is `~(int_n | card_int | card_detect)` and a zero there would assert it
permanently.

## 2. Slot 0 has no GPIO matrix, and the board happens to match it

`SDMMC_LL_SLOT_SUPPORT_GPIO_MATRIX(0)` is 0: slot 0's six signals are IO_MUX
pads at function 0 and nothing else — CLK 43, CMD 44, D0–D3 39–42
(`soc/sdmmc_pins.h`). The Waveshare wiring is exactly that set.

This is the opposite of the EMAC's situation, where `cmake/board-esp32p4-nano
.cmake` carries a paragraph warning that the RMII pin list is a board fact that
must not be tidied. Here there is nothing to get wrong, and the board file says
that too — for the next reader, who will arrive expecting the EMAC's problem.

The one collision worth naming: **GPIO45 is slot 0's D4 pad**. That matters
only for an 8-bit bus, which an SD card does not have, so the pad is free to be
this board's power switch.

## 3. The milestones

| | What | Where |
|---|---|---|
| **35.1** | The host: LDO VO4, the pads, the clock tree, the controller reset | `drivers/sdmmc_esp32p4.c`, `cmake/board-esp32p4-nano.cmake`, `CMakeLists.txt` |
| **35.2** | The card: CMD0–CMD16, capacity from the CSD, and `/sd0` mounted from it | same, plus `fs/vfs_server.c` |
| **35.3** | The system seams: `block_dev_t`, the `sdblk` driver task, `/proc/devices` | `kernel/board.c`, `kernel/main.c` |
| **35.4** | The data path: single-block read and write over the IDMAC (the FIFO was the plan; §1.2), 4-bit at 20 MHz | `drivers/sdmmc_esp32p4.c` |
| **35.5** | `sdinfo` and `sdbench`, and the hardware tests that make the claims checkable | `kernel/shell.c`, `tests/hw/test_esp32p4.py` |

### 35.1 The host

Three controllers own the clocks, exactly as the EMAC's do and with no pattern
shared between them: the bus-clock gate is `HP_SYS_CLKRST_SOC_CLK_CTRL1` bit 14
(the EMAC's is bit 13, beside it), the module's source select, enable and
divider are in `PERI_CLK_CTRL01/02`, and the peripheral reset is bit 28 of
`LP_CLKRST_HP_SDMMC_EMAC_RST_CTRL_REG` — the same register phase 28 already
read the bit diagram of for its own half.

The host divider is not a divisor field. The hardware takes three *edge*
numbers describing one period (`SDIO_LS_CLK_EDGE_L/H/N`) plus a write-1 commit,
and division by 1 is not expressible that way at all — it has its own bit. Two
stages get the two clocks this driver uses: PLL_F160M / 2 / (2×100) = 400 kHz
for identification, and PLL_F160M / 8 = 20 MHz afterwards.

### 35.2 The card, and its real size

The RP2350's SPI driver reports a **hardcoded** capacity:

```c
.num_blocks = 2097152, // Default 1 GB capacity estimate
```

This one reads CMD9 and decodes the CSD, both versions. That is not polish:
`df` and every bounds check in `block_dev_t` are downstream of this number, and
"1 GB" on a 32 GB card is a filesystem that cannot address most of its volume
and a `df` that lies. The v1 path also converts the card's own block size down
to 512-byte units, which is a conversion and not a rounding — a v1 card with
`READ_BL_LEN` 10 reports half as many blocks of twice the size.

### 35.4 Four bits, and why the driver does not believe ACMD6

ACMD6 returns R1 with no error **whether or not D1–D3 are actually
connected**. Nothing before it has ever driven those lines: in 1-bit mode D1
and D2 do nothing at all. So a driver that trusts the response reports a 4-bit
bus and then fails on its first real read, somewhere that looks like a
filesystem problem.

`sdmmc_set_bus_width()` therefore switches, raises the clock, and **reads block
0 back**. If that fails it returns to 1 bit and tries again; if that also fails
it says so and lets the mount fail in the ordinary way. So the width in the
boot line is the width a block actually arrived over, not the width the driver
asked for — which is what makes `test_sd_bus_is_four_bit` worth failing on.

20 MHz is the ceiling without negotiation: default speed is capped at 25 MHz by
the SD physical-layer spec, and going faster means CMD6, a re-timed sampling
phase, and a card that is allowed to say no. 20 MHz is PLL_F160M/8 exactly.

## 4. What this keeps identical to the other two block drivers

The `sdblk` endpoint name, the `'R'`/`'W'` wire protocol, the four-block batch,
`blk_task_call_count()`, and the fall-back-to-direct-access rule are all
`drivers/spisd_rp2350.c`'s and `drivers/virtio_blk.c`'s, unchanged. `/proc`,
`blkstats` and the hardware tests ask the same questions of whichever of the
three a board builds, and a third dialect would have meant a third case in each
of them. `kernel/shell.c`'s `blkstats` had an `#if !defined(CONFIG_BOARD_ESP32P4)`
around it whose comment said "no block device until E6"; that is now three
drivers and no exception.

**One difference, stated rather than hidden: this task is kernel-mode.**
spisd's drops to U-mode under a PMP domain covering SIO and SPI1. This one does
not, because nothing on the P4 does yet — the board has no `.utext` region
wired up and no driver running under a domain — and `drivers/driver_task.h`'s
warning is explicit that the choice is real isolation or a kernel-mode task. A
single driver claiming isolation on a board where no test can check it would be
claiming something nobody could contradict.

## 5. What is deliberately not here

**Multi-block transfers (CMD18/CMD25).** A real speed-up available for the
asking, and now cheaper than it looked when this driver was PIO: the IDMAC
descriptor would become a small ring, and the batching seam above already hands
the driver four blocks at a time. What it adds is an auto-stop completion to
wait on and an abort path to get right. Single-block correctness first.

**High speed (CMD6) and UHS.** 25 MHz is the default-speed ceiling and this
driver stays under it. The P4 supports SDR50/SDR104 and a delay-line phase
calibration to go with them; that is a phase, not a milestone.

**Card-change detection.** There is no CD line to watch (§1.3). A card inserted
after boot is picked up by anything that probes — `sdinfo`, `ls /dev`, `mount`
— because the driver retries the card half of the probe on every ask and an
empty slot costs microseconds to discover. Automatic remounting is not
attempted.

**Interrupts.** `INTMASK` stays 0 and the driver polls `RINTSTS`, which is raw
and set regardless. The EMAC made the same call for the same reason (phase 28's
Z6): the block driver is a task that is already waiting on its endpoint, and an
interrupt would add a path without removing a poll.

## 6. Why this plan was written with the code rather than before it

Phases 32–34 each opened with a measurement that made the case. This one could
not: the question "how fast is the P4's SD card" has no answer until there is a
driver, and the case for building it at all is a sentence — the board has a
socket and the OS could not use it. Writing 2000 words of speculation first
would have been ceremony.

What the plan does carry is the part that would otherwise be lost: the three
traps in §1, each of which cost real time and none of which is visible in the
finished code without its comment.

## 7. What the first hardware run settled

All five questions this section asked before the board was attached, with the
answers it gave on 2026-09-18.

**1. Does VO4 come up, and is the switch closed?** Yes, first time. The LDO
sequence in §1.1 worked as written and never had to be bisected — which is
the return on rendering TRM Registers 15.64/15.65 and counting bits rather
than trusting a plausible-looking macro.

**2. Does the 4-bit read-back pass?** Yes, on the first card tried, and the
suite asserts it rather than accepting whatever the driver reports. The 51K
pull-ups plus the internal ones are evidently enough at 20 MHz.

**3. Is 20 MHz clean?** Yes. Across the bench work, the hardware suite and
5 MB of `sdbench` reads, not one DCRC, DRTO or FRUN was seen.

**4. How fast is it, actually? — 1547 KB/s**, and the sample sizes matter, so:
1024 KB in 661 ms and 4096 KB in 2645 ms, on separate runs, giving 1547 and
1548 KB/s. That is a steady-state rate, not a lucky read.

It is also **a sixth of what the bus could carry**: 512 bytes at 4 bits and
20 MHz is 51 µs of data, and the measured cost is 323 µs per block. So ~270 µs
per block is overhead, and the obvious suspect was ruled out rather than
assumed — replacing the per-spin `time_get_us()` in both polling loops with a
check every 64th spin moved the number by **nothing at all** (1547 → 1547), so
that change was reverted rather than kept with a comment claiming it helped.
What is left is per-command cost: CMD17's own round trip and the card's read
access time, paid once per 512 bytes.

**That is the case for multi-block (§5), now with a number attached.** One
CMD18 for four blocks would pay that cost once instead of four times. The
supporting observation: `ls /sd0` plus one `df` cost **15188** block reads,
because the free-space scan walks the FAT — at 323 µs each, that is most of
five seconds spent almost entirely on per-command overhead.

**5. Does the boot-time probe cost anything visible?** Yes, and more than
expected. `/flash0` mounts at t+0.245 s and `/sd0` at t+0.463 s, so the card
costs about **218 ms** of boot — against a total boot that phase 32 measured
at 282 ms. Ten of those milliseconds are the deliberate supply-ramp delay; the
rest is the identification sequence, every command of which runs at 400 kHz
because the SD specification requires it until the card has left idle.

No baseline was taken with the slot empty, so "218 ms" is the interval between
two log lines rather than a difference of two boots — stated that way on
purpose. It is worth revisiting if boot time ever matters here: the probe could
move behind the scheduler and let `/sd0` appear a moment after the shell
does, which is what a network-storage node actually needs.

## 8. Two things that bit, and what they cost

Recorded because neither is visible in the finished code without its comment,
and both were found on hardware rather than by reading.

**A stopped clock reads as a stuck reset.** The first run printed
`[SDMMC] controller reset never completed -- no module clock?` and meant it.
The three edge fields of the host divider reset to zero, and all-zero is *not*
divide-by-one — division by one has its own bit (`SDIO_HS_MODE`). A stopped
module clock means the controller's own reset bits are never cleared, because
nothing clocks the logic that would clear them. **The divider has to be set
before the reset, not merely before the first transfer.** IDF does exactly
this, and the ordering is invisible unless you are looking for it.

**The TRM's prose described a data path that does not exist.** §1.2 has the
measurements. The rule that comes out of it is the one this chip keeps
teaching: **a register bit diagram is provenance; a paragraph is a
hypothesis.** The project had already recorded one instance (the UART's
`TXFIFO_CNT`, described as an RX count); this is the second and the more
expensive, because the paragraph was load-bearing for a whole design.
