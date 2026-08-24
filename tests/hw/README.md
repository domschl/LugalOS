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

`test_gateway.py` is the sibling suite for the **gateway persona**, and it
talks to the board over a network rather than a cable:

```sh
uv run test_gateway.py --key 000102030405060708090a0b0c0d0e0f --console /dev/ttyACM0
```

It skips everything, rather than failing, when nothing answers at the address
— so it is safe to run speculatively. The board needs, for this boot:

```
lsh> (net-config "192.168.77.2" "255.255.255.0")
lsh> p9key 000102030405060708090a0b0c0d0e0f
lsh> (mount-remote "chess" "uart1")      # only for the two-hop test
```

`--console` is optional and adds one test: it reads the W5500 driver's own
counters afterwards and fails if any resync discard, command timeout or RX
overrun accumulated during the run. That is the test most likely to catch a
regression, because every transport fault in phase 18 showed up there before
it showed up as a failed operation.

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
link. Check `net` on the console first: it reads every relevant register back
out of the chip, so `link`, `chip regs` and `last reset` are facts rather than
the driver's intentions. A `DISAGREES` on the `chip regs` line means a
register did not take, which is a different problem from a dead link.
