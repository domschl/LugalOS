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

## Clock display flickers while the radio comes up (deferred to phase 22/23)

**Trigger:** power on an `rp2350-clock` board with stored WLAN credentials.
For roughly the first 15 s the display flickers moderately, once a second.
The connected steady state is fine.

**Cause:** the CYW43439's 231 KB firmware upload is a bit-banged gSPI
transfer that takes tens of seconds of CPU, and the Pico-Clock-Green
multiplexes its display in software -- so anything that disturbs the frame
cadence is visible. `bp_write_bulk()` already yields between chunks, which is
what turned this from "strong" into "moderate" and cost the bring-up about
10 s.

**Three worse causes were found and fixed first**, and they are worth
distinguishing from this one: an unpaced join-wait spin, unthrottled idle
polling of the radio, and the supervising task spinning at normal priority.
See plan/phase19_ip_stack_and_ethernet.md's note. What remains is the upload
itself.

**Why it is parked:** the appliance's steady state is good, and the honest
fix is a second core -- phase 22 (SMP locking foundation) and phase 23
(multicore scheduling) are already planned. Pinning the display to one core
and the radio to the other removes the class rather than shaving this
instance. Doing it before then would mean either slowing the upload further
or special-casing the display in the driver, neither of which is a good
trade for 15 s once per boot.

---

## No clean way to leave a BSS before re-joining

**Trigger:** `wifi join <ssid> <psk>` (or `wifi join`) on a board that is
already associated. Setup fails before the credentials are ever tested:

    cyw43: ioctl cmd 263 failed, status 0xfffffffb
    cyw43: iovar "mfp" (4 bytes) rejected
    wifi: join failed

**Cause:** several of the join's setup iovars cannot be changed while the
firmware is associated -- `mfp` is the one that surfaces it -- and
`cyw43_join_wpa2_locked()` treats every one of them as fatal.

**Fixed for `mfp` specifically:** it advertises Management Frame Protection
*capability* -- a preference, not a requirement -- so its refusal is now
logged and stepped over rather than aborting the join. A re-join over a live
connection works, which is what made the wrong-PSK verdict testable at all
(the join now decides on the firmware's own AUTH/LINK/KEYED events).

**Still open:** there is no clean way to *leave* a BSS first. Issuing
`CYW43_IOCTL_SET_DISASSOC` (0x69) exactly as the reference does
(`cyw43_ctrl.c`'s `cyw43_wifi_leave()`: length 0, NULL buffer,
`CYW43_ITF_STA`) is answered with `0xfffffffe` (BADARG) and the firmware
stays associated. That attempt was reverted rather than left in place adding
a failing ioctl to every join. Any other setup iovar that turns out to be
refused while associated will need the same judgement `mfp` got -- or the
disassociate finally working.

**Why it is not urgent:** neither path that matters hits it. A join at boot
starts unassociated, and the supervising task's re-join happens *after* a
link loss, when the board is also unassociated -- measured recovering in ~5 s
after a deliberately broken link. It is a manual `wifi join` over a live
connection that fails, and the workaround is that the same command works once
the link is down.

**Worth fixing as:** either make the non-essential setup iovars non-fatal
(`mfp` is a capability hint, not a requirement), or find the correct way to
leave a BSS first. The second is better, since a clean leave is the honest
thing to do before joining somewhere else.

---

## The console runs away if it is written to while the clock app owns it

**Trigger:** on the `rp2350-clock` persona, send Ctrl-C to hand the console
back from the clock application, then immediately write a command. Observed
once, 2026-09-01. The shell banner appears correctly, then the session
degrades into evaluating fragments of its own output -- `Unbound symbol:
lsh:`, `=> (lsh: "command" "li")` -- repeating for as long as it is watched,
with `[Lisp Error] Node pool exhausted!` alongside. The board itself keeps
running: its network task, its radio and its 9P server were all still
answering afterwards.

**Why it is parked:** it needs a very specific gesture -- writing into the
console in the moment it is being handed back, before the line editor has
settled -- and the recovery is a reboot, which a clock being carried to a new
location gets anyway. Nothing that runs unattended does this.

**What is not established:** whether the trigger is the write racing the
hand-back, or a flood of buffered clock-app output being re-read as input.
Both are consistent with what was seen, and telling them apart needs the
gesture repeated deliberately with the console's own echo path instrumented,
which was not worth doing mid-measurement. Recorded rather than diagnosed, on
purpose -- the next person to meet it should know it is known and that the
board is not damaged.

**Fix, when it is worth it:** likely in `kernel/line_editor.c`'s handling of
input arriving while a foreground application is releasing the console, and
the "command line too long, ignored" that precedes the runaway is probably the
first symptom rather than an unrelated message.

---

## The clock board stopped answering after ~2 h of P1 logging

**Trigger:** unknown. On 2026-09-02 the `rp2350-clock` board ran the P0/P1
measurement for 1.81 h, delivering a complete and self-consistent result, and
then stopped: no more broadcasts on udp/5959 and no ICMP reply, for an hour.
It had been up roughly 2.5 h in total.

**What is known.** The last samples show nothing wrong -- reception at 6.9/7,
a frame accepted every minute, round trips of 10-14 ms, the reference at
stratum 1. Both the broadcast and the ping stopped together, which points at
the board or its radio rather than at the collector: a lost broadcast is
routine (they are unacknowledged) but a lost *ping* is not.

**What is not known, and cannot be recovered.** `/proc/kmsg` lives in RAM, so
whatever the board said as it went is gone at the next power cycle. Nothing
was attached to its console at the time -- the board is wherever the DCF
reception is, which is the whole reason it broadcasts.

**A strong candidate, found 2026-09-02 by reading the driver rather than by
reproducing anything:** the link may not have been lost at all in any sense
the board could see. `g_link_up` was set and cleared *exclusively* by the
firmware's DEAUTH and DISASSOC events, which is correct when an AP says
goodbye and says nothing about an AP that simply stops -- a power cut, a
crash, or the board carried out of range. Carrier would stay asserted,
`wifiup`'s supervisor would see a healthy link forever, and the board would be
off the network with nothing anywhere to notice. That is precisely the
observed shape: broadcasts and ICMP stopping together, permanently, with no
recovery.

**Mitigated** by making the supervisor watch liveness rather than carrier: a
receive counter that has not moved in five minutes now forces a rejoin. That
does not *prove* this was the cause -- nothing recorded at the time can -- but
it removes the mechanism, and a recurrence after it would be evidence of
something else entirely, which is worth more than the current single data
point.

**Other candidates, still untested:** a hang in a task (the board has no
watchdog), or power. The P0 instrument is a suspect by proximity rather than
by evidence -- newest code, only minute-cadence loop -- but `ntp_query()`
unbinds its port on every exit path and nothing it does accumulates.

**What to capture next time**, in this order, because the first two are lost
by a power cycle: read `/proc/kmsg` and `/proc/ps` over the out-of-band 9P
channel on the second CDC port *before* rebooting it, then `net` for the
interface counters. `tests/hw/flash.py` will now hand the console back with
Ctrl-C first (2026-09-02), so a board whose shell is merely behind the clock
application is no longer mistaken for one that has died.

**Why it is parked:** one occurrence, no reproduction, and no evidence to work
from. Logged so that a second occurrence is recognised as a pattern rather
than as a first.

---

## The clock board's DS3231 does not survive a power cut

**Trigger:** power-cycle the `rp2350-clock` board. Its DS3231 comes back with
OSF set -- the oscillator stopped while Vcc was away, so the backup supply is
not holding it up.

**Established by experiment on 2026-09-02**, not by reading the flag. OSF is
sticky and this tree never cleared it until that day, so a set flag proved
nothing: it could have been reporting an event from any time in the past, and
the board's time was demonstrably fine across a USB reconnect. What settled it
was clearing the flag (which writing the time now does), confirming it clear
-- the panel's PM lamp was out and the face was reading the DS3231 -- then
removing power and finding it set again.

The flag only became a usable diagnostic once something started clearing it.
That is worth remembering the next time a sticky status bit is read as a
verdict.

**What it is not:** proof that the cell is dead. "The backup supply did not
hold the oscillator up" covers a cell that is flat, a cell that was never
fitted (these baseboards often ship without one), a holder not making contact,
and a board-level fault on VBAT. Only a meter separates those.

**Why it is parked:** it is a hardware condition, and the software handles it
properly now. The kernel clock is not seeded from a chip whose OSF is set --
a reset DS3231 reads 2000-01-01 00:00:00 and passes every range check, so
seeding would replace a known-unset clock with a confidently wrong one. The
face falls back to the kernel clock, which NTP or the radio sets within a
minute of boot, and lights the PM lamp meanwhile. The first write after that
clears OSF, so the RTC carries the time for as long as power is on.

The visible symptom before those fixes was a panel showing a fixed 00:00 for a
day while the board's own clock was correct to the millisecond.

**Fix:** fit a working CR2032.

**The software side is verified on hardware, 2026-09-02**, which is what makes
this a parked hardware condition rather than an open bug. Observed on the
panel through a full boot: `Init` while nothing has set the clock, the Chime
lamp lit while the RTC is not vouching, then the time appearing and the lamp
going out as NTP sets the clock and the write clears OSF. With a working cell
none of that is reached -- the RTC seeds the kernel clock at boot and the time
is simply there.

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
