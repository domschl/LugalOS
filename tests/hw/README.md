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
the current `build/rp2350/lugalos.uf2` (see the root `README.md`'s
"Running on Hardware" section) and connected over USB. The `p9share` test
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
`build/rp2350/lugalos.uf2` to the mounted volume. If the board is running
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
