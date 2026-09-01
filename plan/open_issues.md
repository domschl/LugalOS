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

## `wifi join` reports success when the PSK is wrong

**Trigger:** `wifi join` (either form) with a PSK that is not the network's.
The driver prints `cyw43: joined, bssid ...` and `wifi: joined`, and
`net` shows `link up` -- while nothing works: `rx 0 frames`, no ARP, and the
host cannot ping the board. Found 2026-09-01 while verifying I7b's stored
credentials with a deliberately fake all-zero PSK, which "joined".

**Root cause, and it is a design gap rather than a slip.**
`cyw43_join_wpa2()` decides it has associated when the firmware returns a
non-zero BSSID from `IOCTL_CMD_GET_BSSID`. Its comment says "we have a BSSID
is not a guess" -- but a BSSID appears at **802.11 association**, which
happens *before* and independently of the WPA2 four-way EAPOL handshake. With
a wrong PSK the association succeeds, the handshake fails, and the BSSID is
briefly real. So the check is answering an earlier question than the one it
is being asked.

R5 anticipated the shape of this without noticing the consequence: it set the
firmware's event mask to "none" because "association state is polled rather
than decoded" (plan/phase19_ip_stack_and_ethernet.md, R5). Polling is what
makes this reachable.

**Why it is parked:** the honest fix is to decode the firmware's own link
events (`WLC_E_PSK_SUP` / `WLC_E_LINK`) rather than infer carrier from a
BSSID, which means turning the event channel back on and demultiplexing it --
exactly the work R5 deferred, and a milestone rather than a patch. A cheap
improvement worth considering first: after a BSSID appears, keep polling for
a second or so and require it to *persist*, since a failed handshake is
followed by a deauth. That is a heuristic, and should be labelled one.

**Impact:** `wifi join` cannot be trusted to report failure, so a wrong or
stale stored credential looks like a working network until the first packet
does not arrive.

**`net`'s frame counters are the reliable signal today, and that is measured
rather than assumed** -- the same board, same code path, same "joined"
message, twice: with the fake PSK, `link up` and `rx 0 frames` with the host
unable to ping it; with the operator's real PSK, `rx 19 frames` within
seconds and 8/8 pings at 0% loss. A genuine association starts absorbing
background LAN broadcast immediately, so `rx` still reading 0 a few seconds
after a "successful" join means it did not work.

---

## No identity store on RP2350, so `wifi join` needs its credentials typed

**Trigger:** `wifi join` with no arguments on a real RP2350 board. It
reports that nothing is stored, because `identity_store_device()` is only
provided by the QEMU virtio backend -- on hardware it is still the weak
`NULL`.

**Why it is parked:** this is phase 21's **I7**, a milestone in its own
right with its own flash-layout decisions. Folding it into phase 19's R5
would blur two pieces of work. `wifi join <ssid> <psk-hex>` covers the gap
meanwhile, and takes the derived PSK rather than a passphrase, so the
credential rule (I6) still holds.

**CLOSED 2026-09-01 by I7b.** `identity_store_device()` is implemented on
RP2350 (OTP CHIPID for the uid, the reserved flash sector for the record),
and `wifi join` with no arguments reads its SSID and PSK from the record --
`cyw43: joining "DOSC"...` with nothing typed. See
plan/phase21_identity_and_authentication.md's I7b entry.

---

## Rapid 9P reconnects are refused (2 slots, 2 s TIME_WAIT)

**Trigger:** open and close 9P/TCP sessions in a tight loop. The third
connect is refused, and `net` shows accepts alongside a rising reset
count.

**Why it is parked:** it is the stack behaving as designed, not a fault.
`net/tcp.c` keeps `TCP_MAX_CONNS = 2` and holds a closed connection in
TIME_WAIT for `TCP_TIME_WAIT_MS = 2000`, both deliberate and both
commented where they are defined -- two slots is one more than any board
here needs concurrently, and a shortened TIME_WAIT is already a
LAN-pragmatic choice over the RFC's 2*MSL. A client that reconnects
faster than that is asking for more than the board offers.

It is listed only because it *presents* as a transport failure:
`ConnectionRefusedError` from a client loop looks like the radio or the
driver dropping connections, and cost a round of investigation on exactly
that misreading during R5. `tests/hw/test_wifi.py` now paces past
TIME_WAIT and says why.

**Fix, when it is worth it:** raising `TCP_MAX_CONNS` costs a connection
table entry each and is trivial; the question is whether any real
workload wants it, and so far none does.

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
