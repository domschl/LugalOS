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
