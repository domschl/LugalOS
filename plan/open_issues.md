# Open issues

Things that are known, reproducible, and *deliberately* not fixed yet.

This list exists so that "we know about that" is written down somewhere
rather than living in one person's memory or scattered through commit
messages. It is not a backlog of planned work -- planned work lives in the
phase documents. An entry here is something small, rare, or triggered by
an unusual action, kept visible so that whoever meets it next recognises
it as known rather than spending an afternoon rediscovering it.

Each entry says what it is, how to trigger it, why it is parked, and what
fixing it would probably involve. When one gets fixed, delete it -- the
phase doc and the commit carry the history.

---

## SD card removed while mounted hangs the board

**Trigger:** physically pull the SD card out of a running board.
Reproduced once, 2026-08-31, on the `rp2350-wifi` persona while
disconnecting the card to rule it out of a wireless bring-up problem. The
console froze hard enough that flash.py's 1200-baud touch could not
recover it; only a power cycle did.

**Why it is parked:** an unconventional action. The card is expected to be
present from boot to power-off, and every persona treats it that way. A
board that boots *without* a card is handled fine -- the driver simply
finds nothing, which is how the same session continued afterwards. The
failure needs the card to disappear mid-operation, which no normal use
does.

**Likely cause:** `drivers/spisd_rp2350.c` waiting on a response from a
card that is no longer there, in a loop that has no bound -- the same
shape of bug as the one that hung the board during R5 bring-up, before
every polling loop in `drivers/cyw43_rp2350.c` was given a timeout.

**Fix, when it is worth it:** bound the SD driver's wait loops the way the
CYW43 driver's now are, and fail the operation instead of spinning. Worth
doing as part of any broader pass over blocking waits (see
[[standardized_interrupt_polling]] in the design notes -- the rule that
every polling wait should consult the same shared check applies here).

---

## ENC28J60 clears MACON1.MARXEN / ECON1.RXEN during idle

**Trigger:** leave the gateway persona idle with the ENC28J60 attached;
within seconds one or both bits clear on their own.

**Why it is parked:** worked around rather than explained, by explicit
decision on 2026-08-31. `enc_poll_locked()` notices either bit cleared and
redoes `enc_mac_phy_init()` plus the MAC address, rate-limited with an
escalation to a full hardware reset. The workaround holds under sustained
traffic (0% loss), and the reinit rate tracks transmit load rather than
idle time. `ntruchsess/arduino_uip#167` converged on the same workaround
independently, on genuine Microchip silicon rather than these clones.

**What was ruled out:** a dedicated AMS1117 regulator, three capacitor
values (220uF, 100uF -- measurably worse -- and 470uF), and SPI clock in
both directions. None eliminated it.

**Fix, when it is worth it:** a soldered rebuild instead of jumper wires is
the next plausible experiment. Only worth doing if this ever causes an
operational problem; full account in
`plan/phase19_ip_stack_and_ethernet.md` under R4.

---

## host/fuse-p9 is unverified on macOS

**Trigger:** run `tests/hw/test_gateway.py` on macOS; `fuse_mount` fails
with `mount_macfuse: the file system is not available`.

**Why it is parked:** not a code problem. On Apple Silicon under the
default "Full Security" boot policy, activating macFUSE's system extension
requires Recovery Mode -> Startup Security Utility -> Reduced Security.
That is a real security tradeoff on a developer's own machine, and the
decision was to document it rather than push it through. The code is
portable and demonstrably reaches the real macFUSE mount call.

**Fix, when it is worth it:** nothing to fix in this tree. If someone runs
the suite on Linux, or accepts the Recovery Mode change, the test should
pass as-is. Install notes and the full account are in
`host/fuse-p9/README.md`.
