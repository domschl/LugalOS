# RP2350 Hardware-in-the-Loop Tests

Exercises real RP2350 silicon: `link_usb_cdc` (ACM1/EP4) and `p9share` (the
UART demux), plus the actual T3 milestone from
[`plan/phase5_distributed_design.md`](../../plan/phase5_distributed_design.md)
-- RP2350 hardware talking real 9P to a live QEMU node. There is no QEMU
device model for `link_usb_cdc`, so this is the only place that code path
gets exercised at all; everything here is skipped (not failed) when no board
is attached, so it's safe to run speculatively and safe to leave out of CI.

## Setup

Requires [`uv`](https://docs.astral.sh/uv/) and a board already flashed with
the current `build/rp2350/lugalos.uf2` **and** `build/rp2350/flashfs.uf2`
(the `/flash0` filesystem is its own flash image since I7a; see the root
`README.md`'s "Two images, flashed independently") and connected over USB. The `p9share` test
also needs a CP2101/CP2102 UART adapter wired up; it's skipped otherwise.

```bash
cd tests/hw
uv sync        # pulls pyserial from the committed uv.lock -- reproducible,
                # isolated from your system Python
uv run test_rp2350.py
```

Port auto-detection distinguishes the two CDC-ACM ports (console vs.
`link_usb_cdc`'s net port) by probing with a complete, self-contained
Tversion frame and checking for an `Rversion`-shaped reply -- **never** a
bare byte or partial write. `link_usb_cdc`'s plain length-prefixed framing
has no SLIP-style resync-on-garbage mechanism (unlike `p9share`'s UART
demux), so a partial/misaligned write to the net port desyncs its frame
parser until the next real USB bus reset (unplug/replug -- closing and
reopening the tty is *not* enough, since that isn't a bus-level reset). If
`link_usb_cdc` mysteriously stops responding, that's the first thing to
suspect; a replug clears it.

Override auto-detection if it guesses wrong (e.g. more than one RP2350-like
device attached):

```bash
uv run test_rp2350.py --console /dev/tty.usbmodemXXXX1 --net /dev/tty.usbmodemXXXX3 --uart /dev/tty.usbserial-XXXX
```

## What each test proves

- **`link_usb_cdc` standalone** -- a host 9P client reads `/proc/version`
  over ACM1.
- **`p9share` standalone** -- a real SLIP-framed 9P transaction and a
  plain-text console command, back to back, over the *same* physical UART
  connection.
- **T3: RP2350 <-> QEMU** -- bridges ACM1 to a live QEMU RV64 guest's
  `virtio-console` chardev with a plain byte relay (both ends already speak
  the same length-prefixed framing, so no re-framing is needed), writes a
  uniquely-named marker file to the RP2350's own `/ram0` over the console,
  then runs `(p9-remote-cat ...)` from *inside the QEMU guest's own Lisp
  REPL* and checks the marker's content came back. A match is only possible
  if the bytes genuinely crossed real hardware.

## PMP probe (B3 prep)

`test_pmp_probe` reads this silicon's actual PMP configuration via the
kernel's `pmpinfo` shell command and prints the numbers. Decision **D2**
("PMP early, NOMMU leads") makes B3 depend on them: the implemented region
count bounds how many isolated servers a NOMMU node can host, and the RISC-V
privileged spec permits 0, 16 or 64 entries, so Hazard3's real count has to
be measured rather than assumed. QEMU's RV32 model reports 16 entries at
4-byte granularity; there is no reason to expect real silicon to match.

The test asserts only what B3 genuinely requires -- that PMP exists, and that
no entry is locked at boot (a locked entry cannot be reprogrammed until
reset). The counts themselves are reported, not compared against a hardcoded
expectation.

### Reflashing

There is no automatic path: LugalOS implements its own USB CDC stack, which
receives `SET_LINE_CODING` but ignores the baud rate, so the Arduino-style
"1200-baud touch" that reboots Pico-SDK firmware into BOOTSEL does nothing
here. Flash manually -- hold BOOTSEL while connecting, then copy
`build/rp2350/lugalos.uf2` to the mounted volume (and `flashfs.uf2` too, on
a board that has never had it -- holding BOOTSEL again in between, since
each copy reboots out of it). If the board is running
firmware older than the feature under test, `test_pmp_probe` says so
explicitly rather than failing with a confusing parse error.


## Gateway suite — over Ethernet (N6)

### Wiring the ENC28J60 (R4)

The gateway persona currently has hardware to wire against: a **HanRun V823
HR911105A** module (the common ten-pin ENC28J60 breakout with integrated
RJ45 magnetics). Its header is two rows of five, printed on the board as:

```
  row A:  CLOUT   WOL   SI    CS    VCC
  row B:  INT     SO    SCK   RESET GND
```

Connect to the `rp2350-gateway` board file's reserved SPI0 pins
(`cmake/board-rp2350-gateway.cmake`):

| module pin | signal | RP2350 pin |
|---|---|---|
| SI | SPI MOSI (into the chip) | GP19 |
| SO | SPI MISO (out of the chip) | GP16 |
| SCK | SPI clock | GP18 |
| CS | SPI chip select | GP17 |
| RESET | active-low reset | GP20 |
| INT | active-low interrupt, open-drain | GP21 |
| VCC | 3.3V | 3V3 -- **not 5V, no onboard regulator on this module** |
| GND | ground | GND |
| WOL | wake-on-LAN output | not connected -- no WOL support planned |
| CLOUT | buffered clock output | not connected -- nothing downstream needs it |

Fit the power and decoupling before the first power-on, not after --
Phase 18's W5500 lesson (plan §5) was writing this down and then not doing
it for days.

**Power: a dedicated AMS1117-3.3 regulator for the module, not the
RP2350's own 3V3 rail.** Bring-up on the shared rail produced reproducible
register corruption (see plan §R4's account) that responded, though not
completely, to more decoupling -- consistent with the ENC28J60's own
documented peak draw (up to ~180-200 mA in TX/link-up bursts) sagging a
rail sized for the RP2350 alone.

* **Input**: the RP2350 board's `VBUS` pin (5V from USB) -- an AMS1117-3.3
  needs roughly 4.5-4.8V minimum given its dropout, so VBUS is the right
  source, not the RP2350's own already-regulated 3V3 pin.
* **Ground**: common with the RP2350's GND -- not a separate return. A
  floating ground reference reproduces the same symptoms a genuinely bad
  connection does (see the wiring debugging note in plan §R4).
* **Decoupling**: bulk capacitance across the AMS1117's output, close to
  the module's own VCC/GND pins, plus 100 nF ceramic alongside it. Landed
  at **470 µF** after trying 220 µF and 100 µF; none of the three
  eliminated the remaining quirk below, so treat 470 µF as "known to work
  with the current workaround in place," not as a value verified
  sufficient on its own.

**A known, live quirk, not fully root-caused: `ECON1.RXEN` and/or
`MACON1.MARXEN` spontaneously clear during normal operation** -- on this
specific clone chip within seconds of boot, but the same failure mode is
documented on genuine ENC28J60 silicon too
(`ntruchsess/arduino_uip#167`, hours to weeks in the field). Neither the
dedicated regulator nor any capacitance value tried eliminated it, only
masked how often it needs correcting. The driver (`drivers/enc28j60_rp2350.c`)
watches for it on every poll and does a full MAC/PHY re-init when caught --
the same shape other ENC28J60 libraries converged on independently.
`net regs` reports how many times it has fired since boot
(`full MAC/PHY reinit triggered N time(s)`); a number that keeps climbing
under real traffic is expected, not a regression. Full account, including
everything ruled out before finding the actual CS-hold-time bug this
quirk survived, is in `plan/phase19_ip_stack_and_ethernet.md` under R4.

`test_gateway.py` is the sibling suite for the **gateway persona**, and it
talks to the board over a network rather than a cable:

```sh
uv run test_gateway.py --key 000102030405060708090a0b0c0d0e0f --interactive
```

It skips everything, rather than failing, when nothing answers at the address
— so it is safe to run speculatively. The board needs, for this boot:

```
lsh> (net-config "192.168.77.2" "255.255.255.0")
lsh> p9key 000102030405060708090a0b0c0d0e0f
lsh> (mount-remote "chess" "uart1")      # only for the two-hop test
```

`--interactive` opts into `test_cable_pull`, which needs hands (it prints an
instruction and waits for the cable to actually be unplugged, then plugged
back in); every other test runs without it. There is no `--console` flag on
this script — that was true of an earlier version whose driver-counters test
went with the W5500 in phase 19's R0. R4's equivalent is `net regs`
(§"If it all skips" below), read over the console separately rather than
threaded through this suite.

### What it covers that QEMU and a localhost socket cannot

| test | what it would catch |
|---|---|
| `icmp` | the chip is alive and configured, with no firmware involved |
| `auth: unauthenticated / wrong key` | the N2 gate, on the only wire that is a network |
| `directory reads` | replies too big for one segment — the first thing that broke |
| `sd0 write/read/remove` | the write direction, which nothing before N6 exercised |
| `multi-frame transfer` | 8 KB each way; a read pointer that runs away shows up here |
| `reconnect x5`, `abrupt disconnect` | the socket returning to LISTEN without being asked politely |
| `two hops` | `/chess` through the gateway, proven distinct by `/proc/config` |
| `lugal9pfuse over TCP` | the CLI as a user meets it, including `--key-file` |
| `driver counters` | anything the operations above hid |

### If it all skips

`[!] No 9P server answering` means the board has no address, no key, or no
link. Check `net` on the console first -- it reports the interface, link
state, address and per-protocol counters, all read fresh rather than
recalling what the driver last assumed. On the ENC28J60 (R4), `net regs`
goes a layer deeper: the chip's own raw `EIE`/`EIR`/`ESTAT`/`ECON1`/`ECON2`,
`EPKTCNT`, `ERXFCON`, `MACON1`, the RX ring pointers, `PHSTAT2`, and how
many times the driver's own MAC/PHY recovery has fired since boot -- see
the wiring section above for what a nonzero, climbing reinit count means
and why it is expected rather than a fault.

## ESP32-P4 (Waveshare ESP32-P4-NANO) — loading and reset

Phase 27's second silicon. Nothing here is a test yet: E1 is a standalone
bare-metal program, and the hardware suite is E8's work. This is the
procedure for getting code onto the board, written down because the port
arrangement is the part that costs an afternoon if guessed at.

**Nothing in this section writes flash.** `esptool load-ram` delivers an
image over the download protocol straight into L2MEM and jumps to it, so a
reset restores whatever is in flash and there is nothing to brick. Writing
flash starts at E6, and E6 is gated on the backup below.

```sh
tools/p4run.py --ports              # which cable is which
tools/p4run.py --reset-test         # can this host reset the board?
tools/build_minimal_esp32p4.sh run  # E1: build, image, load into RAM, listen
tools/p4run.py --probe              # is our program still running?
tools/p4run.py --run                # reset back into whatever is in flash
```

### Running the kernel (E2 onward)

```sh
cmake --preset esp32p4
cmake --build --preset esp32p4
tools/p4run.py build/esp32p4/lugalos.elf --baud 921600 --interactive
```

Three things about that last line are not obvious.

**`--baud` is the load transfer only.** The console stays at 115200
throughout, because the kernel configures UART0 itself the moment it starts
and the ROM's rate is only in force until then
(`drivers/uart_esp32p4.c`). Use it because the arithmetic is unkind
otherwise: the kernel image is ~227 KB against E1's 1.2 KB, which is about
40 seconds at 115200.

**Pass the `.elf`, never an image.** `p4run.py` runs `elf2image` itself on
every load. That is not a convenience — E1 lost most of an afternoon to
scripts that took an image path while only the build regenerated images, so
every load after the first re-delivered a stale binary and two confident
diagnoses were written down before anyone checked a timestamp.

**`--interactive` relays this terminal until Ctrl-]**; `--cmd 'ls /proc'`
(repeatable) types a line and prints what comes back instead, which is how
the shell gets exercised with nobody at the keyboard. Both work on their own
too, against a board that is already running:

```sh
tools/p4run.py --cmd 'ls /proc' --cmd 'cat /proc/meminfo'
tools/p4run.py --interactive
```

**What this kernel does not have yet**, so that a quiet answer is not
mistaken for a fault: no preemption (the CLIC is up since E3, but the CLINT
comparator behind the tick is E4), no `/flash0` or `/sd0` (E6), no network,
and no U-mode isolation (E5). `df` reports the filesystems as unmounted
because they are.

**Checking the interrupt path**, which E3 added and which the console cannot
demonstrate by working:

```sh
tools/p4run.py --cmd 'uartstats' --cmd 'cat /proc/meminfo' --cmd 'uartstats'
```

`rx_wakes` is the count that matters. It is the number of interrupts that
woke a task blocked on a read, so it has to grow between those two calls; if
it does not, the console is falling back to the `sched_yield()` polling path
underneath the ISR and looking exactly the same from here. `irqs` alone does
not answer this — a driver whose TX blocks on an interrupt and whose RX polls
reports a healthy total.

**Checking the exception path**: `trapselftest` executes an illegal
instruction under a probe and reports whether the trap vector caught it and
execution resumed. `trapselftest fatal` runs the same instruction with no
probe, which prints the full register dump and halts the board — reload to
recover, nothing is written to flash.

**Checking the tick** (E4): `clicdump`. The register block at the top is the
easy half; the two lines at the bottom are the ones that matter.

```
[CLIC] spin0 (nothing else READY): ticks +200 over 2 s, MIL seen=0, MIE now=1
[CLIC] spin1 (a READY task waiting): ticks +200 over 2 s, MIL seen=0, MIE now=1
```

Both must read `+200` (2 s at 100 Hz) with `MIL seen=0`. `spin1` reading `+0`
with `MIL seen=255` is this board's signature failure: preemption switched
tasks out of the interrupt handler, so the `mret` that lowers
`mintstatus.MIL` never ran and every interrupt on the chip is masked — with
`mstatus.MIE` still reading 1. **A dump taken at the prompt cannot see it**;
only sampling from inside a loop that never yields can, which is why those
two phases exist.

**Two things about timing on this board.** There is no PLL yet, so the CPU
runs off the 40 MHz crystal and is roughly ten times slower than the part's
400 MHz rating. `preempttest` bounds its wait at 400 M iterations of a
volatile 64-bit loop, which is about **150 seconds** here when preemption is
broken and about 100 ms when it works — so a "hang" that lasts under three
minutes is not yet evidence of anything.

And `--cmd-wait` is slept once per command *plus once for the bare Return
that precedes them*, so three commands at `--cmd-wait 200` need 800 seconds,
not 600. Getting that wrong kills the run under `timeout` and prints nothing
at all, which looks exactly like a dead board.

### Flashing the filesystem (E6)

```sh
tools/p4flash.py --verify
```

That is the whole command. It takes **no address**: the base comes from
`build/esp32p4/flashfs.addr`, which CMake generates from
`cmake/flash_layout_esp32p4.cmake` — the same definition the kernel was
compiled against. An address typed here is one that can disagree with the
kernel's, and the symptom is a filesystem that mounts as garbage rather than
an error.

**The kernel is not flashed.** Unlike the RP2350's two UF2 files, only the
filesystem lives in flash on this board; `tools/p4run.py` delivers the kernel
into RAM on every run. So the split is still "two images, flashed
independently", with one of the two never touching flash at all.

`--verify` re-checks with esptool's on-chip digest afterwards, which is not
the same as re-reading the file: a re-read can reproduce its own transfer bug
and look like agreement.

**The factory image is out of reach**, by construction rather than by care.
The Waveshare demo occupies the low 13.06 MB (read out of the verified backup
at `~/gith/esp/p4nano-factory-flash/`, not guessed); the segment starts at
14 MB. `drivers/flash_esp32p4.c` refuses every erase and program below that
floor, and `p4flash.py` carries its own copy of it because it is the one path
that can write flash without the kernel's guard in front of it. `flashtest`
on the board proves both refusals before it writes anything.

**Checking the flash path** without touching it: `flashinfo` dumps 32 bytes
from three places, one of which is the factory partition table at `0x8000`. It
must begin `aa 50` and name `nvs` — the same bytes the backup contains, so a
correct read is provable rather than plausible. `flashtest` then does an
erase/program/read-back in the last sector of the writable region, and reports
the guard result first.

### The two ports, and why names are not enough

The script needs two things from the wiring, and they are not always the
same cable: a **console** (full duplex — loading means esptool talks) and a
**reset line** (DTR/RTS reaching U6, which wires RTS to ESP_EN and DTR to
GPIO35).

Ports cannot be told apart by device name. On Linux the CH343P bridge
enumerates through `cdc_acm` as `/dev/ttyACM*` — and so does the P4's own
native USB-Serial-JTAG, the one socket that must be avoided. `p4run.py`
therefore identifies ports by USB VID:PID; `--ports` prints what it found.
Override with `--port` / `--reset-port` or `LUGALOS_P4_PORT` /
`LUGALOS_P4_RESET_PORT`.

On the development board as wired, the two roles are split, and not by
preference:

| cable | VID:PID | here | role |
|---|---|---|---|
| CH343P (on-board) | `1a86:55d3` | `/dev/ttyACM0` | reset lines, and UART0 RX |
| CP2102 (external, wired to UART0) | `10c4:ea60` | `/dev/ttyUSB0` | console, full duplex |

The CH343P's host-to-board TX path is dead on this board — esptool's own
diagnosis is *"Download mode successfully detected, but getting no sync
reply: The serial TX path seems to be down."* Its RX and its modem lines
work fine, which is why it keeps the reset job. A stock board with a working
CH343P uses that one cable for both, which is what the defaults do.

The split buys something a single cable cannot: the reset port's RX is still
UART0, so it can be held open and read *through* the load. The banner and
the `misa`/`mtvec`/`mhartid`/`mstatus` dump are printed once, in the instant
the ROM jumps to us, while esptool still owns the port it loaded over — on a
single-cable host they are simply lost.

### Reset

Linux (`cdc_acm`) carries the modem-control lines, so loading needs no
buttons: `p4run.py` drives RTS and DTR itself and the ROM confirms which
mode it chose. macOS's built-in CH34x driver does not carry them; there the
script falls back to asking for **hold BOOT, tap RESET, release BOOT**.

`--reset-test` distinguishes the two cases that matter, because only the
second is hard and only the second is what an unattended suite needs:

```
run (RTS pulse)               2812 bytes  reset: True   rst:0x1 (POWERON),boot:0x30f (SPI_FAST_FLASH_BOOT)
download (RTS + DTR strap)     128 bytes  reset: True   rst:0x1 (POWERON),boot:0x307 (DOWNLOAD(USB/UART0/SPI))
```

Read the verdict off the ROM's own `boot:` line rather than off silence. An
earlier version of this test flushed the input buffer *after* driving the
reset lines, which discarded the banner — it reported "this host cannot
reset the board" about a host that resets it perfectly.

### The GPIO toggle: no user LED on this board

The NANO has **no user LED** — `LED1` in the schematic is a hardwired 5 V
power indicator, and `LED0`/`LED3`/`LEDMOD` belong to the Ethernet PHY. E1's
toggle therefore drives **GPIO20 (header P1 pin 13)** and reads the pad back
through the input buffer, which needs no instruments:

```
gpio20  = drive 1 reads 1, drive 0 reads 0  PASS
gpio20  = released, pull-down reads 0, pull-up reads 1  PASS (reading the pad, and the pin is free)
```

The second line is the one that matters: with the output disabled and
`GPIO_OUT` left high, a `GPIO_IN` that merely mirrored `GPIO_OUT` would read 1
both times. Reading 0 then 1 proves the pad is being measured — and that
GPIO20 has nothing else on it.

GPIO20–23 are the free pins on this board (all four on P1). Avoid GPIO7/8
(I2C), GPIO34–38 (strapping; GPIO36 has a 10 kΩ pull-up, GPIO37/38 are the
console), and GPIO14–19 plus GPIO54 (the C6).

### Flash backup — a prerequisite, not a suggestion

**E6 must not begin until the factory image is safely off the board.** The
Waveshare factory demo is not obtainable again once overwritten.

Taken 2026-09-05 and kept outside the repository (16 MB of vendor binary is
not something to carry in git) at `~/gith/esp/p4nano-factory-flash/`, with a
`README.md` recording chip revision, flash part and restore command:

```sh
esptool --chip esp32p4 --port /dev/ttyUSB0 --baud 921600 \
        read-flash 0 0x1000000 p4nano-factory-16mb.bin
esptool --chip esp32p4 --port /dev/ttyUSB0 --baud 921600 \
        verify-flash 0 p4nano-factory-16mb.bin
```

Run `verify-flash` afterwards and do not skip it: it compares an on-chip
digest rather than re-reading over the same path that produced the file, so
it can catch a transfer that was quietly wrong. This one matched.
