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


## Where the 2026-09-14/15 stability campaign is written up

Phase 28 (ESP32-P4 Ethernet) paused after Z3 to clear the intermittents rather
than carry them into work that needs a trustworthy suite.
**`plan/phase33_stability_hunt.md`** is the account: seven defects fixed, with
the evidence that identified each, what remains open and what it is bounded at,
and the four confident diagnoses that turned out wrong and why.

Several entries below were **rewritten or withdrawn** during it. Two are worth
knowing about before reading them:

* the `exec` wild jump was **not** in the embedded FAT32 image -- that came
  from nearest-symbol arithmetic against a build that had since moved;
* the console-splice fix has **no measured support**; the `I3` numbers once
  credited to it belong to the identity-store bug, which is fixed separately.

Phase 28 resumes at **Z4 -- frames on the wire**.

## FIXED: the P4's EMAC lost ~1% of frames because we drove the RMII pads too hard

**Closed 2026-09-16, phase 28 Z5.** Kept rather than deleted because the
*shape* of this one is worth having on file: the bug was a single line with a
confident comment, and it survived a long hunt through the descriptor path
because the comment read like a justification.

**The cause.** `pad_iomux()` forced `FUN_DRV = 3` -- maximum drive -- on every
RMII pad, explained in the code as "strongest drive; RMII runs at 50 MHz".
That is an assumption, and it was never checked against anything. ESP-IDF
configures the same pads (`emac_esp_iomux_init()`, `esp_eth_mac_esp_gpio.c`)
by setting the IOMUX function and the pull mode and **never touching drive
strength**, leaving the pad default of 2. Maximum drive into short
unterminated board traces buys edge rate at the cost of overshoot and
ringing, which at 50 MHz arrives as corrupted bits.

**The evidence, at matched sample sizes:**

| path | forced drv=3 | default drv=2 |
|---|---|---|
| MAC-internal loopback (never touches a pad) | 3500/3500 | 3500/3500 |
| PHY loopback (RMII pins + IP101G) | 3494/3500, **1 corrupt** | **3500/3500** |
| the wire, 1000 echoes @ 20/s | 991/1000, 983/1000 | **1000/1000** x2 |
| the wire, 1000 echoes @ 10/s | ~494/500 equivalent | **1000/1000** |

Three thousand echoes across three runs with no loss, against a bug that
previously showed ~1% flat.

**How it was localised, which is the reusable part.** Two loopback modes
bisect the physical path: MAC-internal never leaves the MAC, PHY loopback
(BMCR bit 14) goes out through the real transmit path and the RMII pins and
is turned around inside the PHY. The first stayed perfect while the second
lost *and corrupted* frames -- and corruption with every error counter clean
(TDES0 no error, peer NIC no CRC/length/fragment errors) is a signal-integrity
signature rather than a logic one. `emac loopback` / `emac loopback phy`
remain in the driver for exactly this.

**What was checked and found correct**, so nobody re-treads it: the RMII clock
configuration matches IDF's `emac_ll_clock_enable_rmii_input` exactly (source
selects, the three enables, `hp_pad_emac_txrx_clk_en`, `pad_emac_ref_clk_en=0`
for a clock-input board); the divider values 1 and 19 match IDF's
`50MHz/25MHz-1` and `50MHz/2.5MHz-1`; the field positions `[7:0]` and `[17:10]`
match the generated header; `hw_ver1` and `hw_ver3` agree on every field this
driver touches (this board is **rev v1.3**, i.e. hw_ver1); and the pin
assignment is identical to IDF's own CI config for P4 + IP101
(`MDC=31, MDIO=52, RST=51, addr=1`). `ESP32P4_SELECTS_REV_LESS_V3` changes
nothing in the EMAC path but a PTP pin.

Two smaller alignments came with the fix: RMII pads are now explicitly
floated (IDF's `GPIO_FLOATING`; an RMII line is driven at both ends and a
stray pull fights the driver), and `FILTER_EN` -- a pin filter that discards
pulses shorter than two clock cycles -- is cleared rather than left to
whatever the bootloader left behind.

**Three false trails, recorded so they are not re-run.** The periodic station-
address write (removing it made loss *worse*); transmit store-and-forward plus
a deeper transmit ring (an apparent tenfold improvement that was 200-700
packet samples separating 1.3% from 1.5% -- noise, and larger runs reversed
the ordering, so the driver ships IDF's configuration); and the descriptor
path generally, which MAC-internal loopback had already exonerated at
3500/3500 before any of that was tried.

**Two real bugs found on the way, both fixed, neither the cause:** the Z4
receive race (`emac_rx_peek()` returns NULL for both "nothing ready" and
"errored", and the poll loop re-read the descriptor to tell them apart, so a
frame arriving in that window was scored a hardware error and discarded --
3-15% depending on rate), and the loopback diagnostics sharing descriptor
rings with the live netif (`netsrv` consumed 65 of one 700-frame run;
`g_diag_owns_rings` now makes the netif yield).

## Pressure is published as station pressure, not reduced to sea level

**Trigger:** subscribe to a sensor node's `pressure` topic and compare it with
any weather service. The board at 520 m publishes `963.69`; every app and
dashboard for the same place and moment says about `1025`. Both are correct
and they are not the same quantity.

`drivers/bme280.c` publishes what the part measures -- **station pressure**,
the actual air pressure where the sensor sits. What is conventionally shown,
and what "1013.25 hPa" means, is pressure **reduced to sea level** (QNH; *auf
Normalnull*, NN). The difference is about 60 hPa at 520 m, which is far larger
than any weather variation, so an unreduced reading does not look like a
slightly wrong number -- it looks like a permanent deep storm, and it cannot
be compared with anything.

**Why it is parked:** the reduction needs a fact the board does not have. It
is not a calibration constant or a property of the part; it is **where the
sensor was installed**, which only the person installing it knows. Publishing
a reduced pressure without that number would mean inventing it, and inventing
it is worse than publishing the honest raw quantity -- which is at least
correct, labelled, and convertible by anyone who does know the altitude.

**Fix, when it is worth it:**

1. An **installation altitude in metres**, configurable and persistent. The
   identity record is the right home, beside the broker and the address: it is
   per-board, it survives reflashing, and altitude is exactly the kind of
   per-board fact I9 put the address there for. `mqttcfg` would gain it, or it
   would get its own small setter.
2. The reduction itself, which is the barometric formula

       p_0 = p * (1 - (0.0065 * h) / (T + 0.0065 * h + 273.15)) ** -5.257

   using the measured temperature, so it tracks the day rather than assuming a
   standard atmosphere. **This is the part with a real cost**: that exponent
   needs `exp`/`log`, and this kernel has no floating point and no math
   library. Either a fixed-point exponential, or -- probably better for a
   range of 0-3000 m -- a small interpolation table, since the correction is
   smooth and one part in a thousand is far below the sensor's own accuracy.
3. A decision about **what to publish**. Publishing only the reduced value
   loses the honest measurement; publishing only the raw one keeps the problem.
   Two topics (`pressure` and `pressure_msl`) costs one more source slot and
   lets a subscriber choose, which is probably right -- and is why this is a
   design note rather than a one-line change.

Until then, the published `pressure` is station pressure and the README's
example values are station pressure. Anyone comparing them with a forecast
needs to reduce them first.

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

## The WLAN credential test flakes about one run in six

**Trigger:** run the full QEMU suite repeatedly. "WLAN Credential:
Host<->Device Format Agreement, Round Trip, Never Printed (I6)" occasionally
fails with `the new credential's fingerprint does not match`, immediately
after the board reported `wlan: credential installed`. It passes in isolation
every time.

**Why it is parked:** it is a harness timing fault, not a device fault, and the
evidence is that the *device* said it stored the credential. `send_and_expect`
accumulates output and matches a regex against everything read so far
(QemuSession's own docstring records an earlier family of races in exactly this
area), so a fingerprint line left over from the check *before* the write can
satisfy -- or fail -- the pattern meant for the one after it.

**Measured, 2026-09-02:** seven full runs across two trees, one failure. It was
first seen before the change that was initially suspected of causing it, and
three runs of that tree with the change reverted were clean by luck rather
than by fix -- which is how a one-in-six flake looks when you sample it three
times, and worth remembering before blaming the next change that coincides
with it.

**Fix, when it is worth it:** have that test drain to a unique marker before
reading back, rather than matching against an accumulated buffer -- the same
shape the runner already uses elsewhere.

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

---

## `cc` searches only /ram0 for a relative `#include`

**Trigger:** compile a source on `/sd0` (or `/flash0`) whose header sits
beside it: `#include "myhdr.h"` from `/sd0/prog.c` does not find
`/sd0/myhdr.h`.

**Why it is parked:** `user/chibicc/preprocess.c` builds exactly two
candidate paths for a relative header -- `/ram0/<name>` and
`/ram0/include/<name>` -- and an absolute `#include "/sd0/myhdr.h"` works
because it takes a different branch. So there is a way to express every
case; it is the *conventional* one (look next to the including file) that
is missing. Found 2026-09-03 while fixing the read length in that same
function, which had been bounded by `sizeof(char *)` and so had never
delivered more than 3 bytes of any header -- with that broken, nothing had
ever exercised the search path hard enough to notice its shape.

**Fix, when it is worth it:** derive the directory of the file currently
being preprocessed and try that first, then the existing `/ram0`
fallbacks. The awkward part is that `preprocess_internal()` is handed a
buffer rather than a path, so the including file's directory has to be
threaded down to it (or kept in a small include-stack alongside `depth`,
which already exists for the recursion guard).

---

## `K3` checks pin values from a table, not from the build

**Trigger:** none today -- the test passes on both personas. This is about
how it knows what to expect.

**Why it is parked:** `test_config_pins` carries a hardcoded table of pin
values, now split into a universal block plus per-feature blocks gated on
`ENABLE_SPISD` / `ENABLE_ST7735` / `ENABLE_TM1638` read from
`/proc/config`. That fixed the failure (the table was the chess persona's
map, so ten correctly-absent keys read as wrong pins on rp2350-clock), but
the table is still a hand-maintained second copy of
`cmake/board-*.cmake`, and a persona whose pins nobody adds here is
checked against nothing. `LED_EXT_GPIO` already had to drop to a presence
check for exactly that reason -- it is 16 on chess and 9 on the clock
board, which has no plain onboard LED at all.

**Fix, when it is worth it:** every build directory already generates
`lugalos_config.h` (`cmake/gen_config.cmake`) with the real
`#define CONFIG_*` values, so the test could compare `/proc/config`
against the header for the build the board was flashed from and drop the
table entirely -- checking every key automatically, on any persona,
present and future. The awkward part is knowing *which* build directory:
`rp2350.local_build_id()` already has this problem and solves it by being
told, and its own docstring records a false mismatch from guessing wrong
(2026-09-01). Matching `/proc/buildid` against each `build/rp2350*/`
would settle it. Deferred 2026-09-03 because only a clock board was
attached, and a rewrite that cannot be run against a chess persona is
worse than a narrower change that can.

---

## A trailing slash breaks path resolution below a mount root

**Trigger:**

```
ls /flash0/system/        ->  ls: path 'system/' not found
ls /flash0/system         ->  lists BIN, ETC correctly
ls /flash0/               ->  lists correctly (it is the mount root)
```

So the trailing slash is harmless at a mount root and fatal one level
down. Confirmed on RP2350 hardware, `rp2350-chess`, 2026-09-03.

**Why it is parked:** it is a usability bug, not a correctness one --
nothing in the tree writes a path that way, and the failure is a clean
"not found" rather than a wrong answer. It was found because a new
hardware-suite preflight probe used `ls /flash0/system/bin/` and reported
a stale filesystem on a board that was correctly flashed, which is the
more expensive shape of this: a diagnostic that fires on healthy input.
The probe now omits the slash.

**Fix, when it is worth it:** `kernel/path.c` / `vfs_resolve()` should
strip a trailing separator from the relative part before lookup, the way
it evidently already does for the mount-root case. Worth checking whether
the same asymmetry affects `cat`, `cp` and the 9P walk, which take the
same resolver -- the shell's `ls` is only where it happened to surface.
---
## C6/C7's exact heap comparison is disturbed by background allocation

**Trigger:** run `tests/hw/test_rp2350.py` against a networked persona
(`rp2350-clock` on a Pico 2 W) in the first minute after a flash, while the
WLAN supervisor is still retrying its join. Seen once in four runs on
2026-09-03; three settled runs either side were clean.

**Why it is parked:** the test reads `Pages Used` before and after a
compile and requires them to be *equal* -- which is exactly the right
assertion for what it checks (that chibicc's arena comes back), and exactly
what any concurrent allocation elsewhere in the system breaks. The cause is
consistent with the WLAN join path allocating between the two readings, but
that was inferred from timing rather than demonstrated, so it is written
down as an observation and not a diagnosis.

**Fix, when it is worth it:** not by loosening the equality -- a tolerance
band would blunt the one thing it detects. Better either to take both
readings while the system is demonstrably quiescent (the suite already
knows how to wait for a settled console), or to have the test compare only
the pages attributable to the compile, which `/proc/meminfo`'s peak figure
already distinguishes. Whichever, it should be a deliberate change to a
leak-detection test rather than a drive-by loosening.

---

## `sizereport` cannot see initialised statics on RP2350

**Trigger:** add an initialised static (`static thing_t x = { ... };` with
any non-zero field) to any file in an RP2350 build. It costs exactly as
much RAM as the equivalent zero-initialised object and
`cmake --build build/rp2350 --target sizecheck` does not count a byte of
it.

**Why:** `tools/sizereport.py` sums `nm` symbols typed `b` or `d`. On
RP2350 initialised data lands in `.data`, which `linker/rp2350.ld` places
inside an executable PT_LOAD (it shares the segment with `.ramfunc` --
there is a comment in CMakeLists.txt about that PT_LOAD being genuinely
RWX). `nm` therefore types those symbols `t`, and the filter drops them.
Measured 2026-09-03: in RP2350's RAM window there are 329 `b` symbols,
68 `t`, and **zero** `d`. The RV32 QEMU build, for contrast, has 59 `d`.

Found the expensive way. S2 first wrote its two locks as
`static ylock_t g_pump_lock = YLOCK_INIT;`, and `sizecheck` reported a
24-byte *saving* on a like-for-like replacement of two 12-byte objects --
the memory had not gone anywhere, it had become invisible. Rewritten to
rely on all-zero being a valid free lock, they report `+0`, which is the
truth.

**Why it matters more than 24 bytes:** on RP2350 `.bss` and the heap are
the same memory, which is the entire reason this guard exists. A blind
spot in it is a way to spend heap without anyone being told -- and it
rewards exactly the habit (a self-documenting initialiser) that a reviewer
would otherwise encourage.

**Fix, when it is worth it:** count symbols by *section* rather than by
`nm` type -- `readelf -sW` gives a section index per symbol, so `.data`
can be counted wherever the linker has put it and `.ramfunc`'s actual code
can still be excluded deliberately rather than by accident. Until then the
convention is: on RP2350, prefer zero-initialised statics, and treat any
size *drop* on a change that added state as a measurement bug until proven
otherwise.

---

## CLOSED: the P4's console was its only 9P wire — now it has Ethernet

**Closed 2026-09-16 by phase 28 (Z5/Z7), and closed the way this entry said
it would be**, which is the reason it is kept rather than deleted.

**What it used to say.** The P4 has no USB device stack, so its single
`/dev/ttyACM*` is the CH343P bridge rather than the P4 talking, and console
and 9P shared UART0 through `p9share`'s SLIP demux. Giving the board a
downlink to a gateway the way `drivers/uart1_link_rp2350.c` describes needed
either a USB device stack (phase-sized) or a `uart1_link_esp32p4.c` that could
not be tested without a gateway physically wired to it. It was parked with:

> *"phase 28 brings up Ethernet on this board, at which point the P4 has a
> netif and the whole question changes shape — a node on the LAN needs no
> downlink cable. That is the reason to wait rather than to pick one of the
> two now."*

**What happened.** Exactly that. `eth0` registers at boot from eFuse identity,
and 9P runs over TCP on port 564: `tests/hw/test_esp32p4.py`'s `test_lan_node`
attaches from the laptop, lists `/proc` and reads `/proc/kmsg` over the wire.
The board is reachable as a node rather than as the far end of somebody's
serial cable, so neither of the two expensive options is needed for the
purpose that motivated them.

**What is genuinely still absent**, stated so this does not read as more than
it is: the P4 still has no USB device stack, so it still presents one ACM port
and that port is still the bridge chip. Out-of-band console access of the kind
[[hw_out_of_band_9p_channel]] describes for the RP2350 — `/proc/kmsg` when the
console itself is wedged — now exists over Ethernet instead, which is better
when the network is up and useless when it is not. A board whose console has
hung *and* whose link is down is still only reachable by a power cycle.

**The transferable part:** parking an issue with a named future event that
changes its shape, rather than with a fix, worked here. The event arrived, the
question dissolved instead of being answered, and no time went into either of
the two designs that would have been obsolete on arrival.

---

## `mqttd` has no file-backed source, so a gateway cannot publish a mounted namespace

**Trigger:** mount a sensor node's namespace on a gateway
(`(mount-remote "p4" "uart1")`) and try to publish a value out of it.
Nothing does.

**Why it is parked:** `mqttd_add_source()` (net/mqttd.h) takes a *function*.
Every source in the tree is compiled in beside the driver that produces it --
`drivers/bme280.c`'s three, and `mqttd fake`'s synthetic one. A gateway
republishing another node's readings needs a source that opens a path on a
mount, reads a `key=value` block and returns one field, and nothing in the
tree does that yet.

Found in phase 27's E7, where it is the last piece between a P4 that measures
and a broker that hears about it. Independent of the wiring issue above: it is
the same missing piece whether the gateway is an RP2350 or a QEMU guest.

**Fix, when it is worth it:** a `mqttd_add_file_source(name, path, field,
rule)` whose sample function does a 9P read through the VFS like any other
file. The rate limiting, the will and the reconnect machinery above it all
work unchanged -- Q5 built them against a source that is just a function
returning a number, which is exactly what this would be.

---

## Two hand-rolled yielding locks are outside the wait-for graph

**Trigger:** a cycle or a re-entrant call through `drivers/cyw43_rp2350.c`'s
`g_bus_busy` or `drivers/enc28j60_rp2350.c`'s `g_busy`. Nothing detects it;
the board stops.

**Why it is parked:** both are `ylock_t` in everything but type — a flag and a
`sched_yield()` loop waiting for it. Because they record no owning task they
contribute no edge to phase 31's wait-for graph, so a cycle through one is
invisible in exactly the way lock cycles were invisible to the channel-only
graph before Y3.

It has already cost something: `net/stack.c`'s comment records a re-entrant
call through `net_set_address()`'s gratuitous ARP taking the bus lock a frame
below already held, and "the board deadlocked on `wifi probe` with the console
gone. It cost a physical BOOTSEL to recover."

**Fix, when it is worth it:** make them `ylock_t`. That is smaller than it
sounds and strictly better than widening the graph to understand bespoke
flags: a `ylock_t` is re-entrant for its owner, which is precisely the failure
above, and it joins the graph for free. Not done in phase 31 Y4 because it
changes two drivers on a board that was not attached at the time, and
`plan/phase30_driver_framework.md` opens both files anyway.

*(2026-09-11: phase 31 Y5 does not touch this. Y5 removes printk ownership
from the graph; these two are the opposite problem — blocking resources that
were never in it. If anything Y5 raises their relative weight, since after it
they are the only blocking resources in the tree that contribute no edge.)*


---

## `exec` of a loaded ELF hangs or fails — on **every** target, not one

**Consolidated 2026-09-13 from three entries that were the same bug.** They
were recorded separately because each was seen once, on a different platform,
with a different-looking symptom. A 12-run soak caught three hung guests at
once and the logs make the pattern plain:

```
lsh> exec /flash0/system/bin/uhello.elf              <- rv32, then nothing
     exec .../uisolate.elf -> Created task #7 'uprog' <- rv64, then nothing
lsh> exec .../uisolate.elf                            <- rv64, then nothing
```

and on the ESP32-P4 the same operation fails deterministically with
`'…uhello.elf' was terminated before it could exit`.

**It is not chibicc's code generation.** That was the natural suspicion, since
the P4 failure appeared the moment chibicc was enabled there. Every binary
above is **host-built** (`uhello.elf`, `uisolate.elf` from `tests/`), and a
chibicc-built binary fails identically. Compilation is fine; running is not.

**What the wait does.** `elf_load_and_run_argv()` spins
`while (sched_task_state(pid) != TASK_DEAD && spins < 2000000) sched_yield();`
so the shell is not permanently wedged — but two million yields is far past
any test's timeout, and from outside it is indistinguishable from a hang. The
child is created (`Created task #7 'uprog'`) and then never reaches
`TASK_DEAD`, with no fault reported.

**Not reproducible in isolation**, which is the main obstacle. Tried and
failed to provoke it: 30 consecutive `exec`s of `uhello`, 25 of `uisolate`
(the one that faults on purpose, and the binary in two of the three hung
logs), and 12 cycles of `lockselftest`-then-`exec` to recreate the klog-burst
context the hung logs show immediately beforehand. All clean. It needs
accumulated suite state that a tight loop does not have.

**Caught with a cause code, 2026-09-13.** A 16-run soak produced a guest log
that names it:

```
lsh> exec /flash0/system/bin/uisolate.elf
[Trap Exception] Cause: 0x2, epc=0x8007ecec, tval=0x7070615f, inst=0x7070615f
[Trap Register Dump] a0=0x8023b050, a1=0x8023b058, sp=0x802d0fd0, ra=0x8007ece8
```

Cause 0x2 is **illegal instruction**, and the instruction word is ASCII:
`0x7070615f` is the bytes `5f 61 70 70`, `_app`. The CPU was executing text.

Where: `.text` in that build spans `0x80000000`–`0x8006a050` and `.rodata`
runs to `0x801027d0`, so **`epc` is inside `.rodata`**. `ra` is `epc-4`, so it
was already executing there and simply stepped forward.

**Which object, corrected 2026-09-14.** This entry used to say the epc was
inside `g_flash_fs_start`, the embedded FAT32 image, on the strength of
`nm`'s nearest preceding symbol. That attribution does not hold up and should
not be built on:

* the byte sequence `5f 61 70 70` (`_app`) does **not occur anywhere** in the
  512 KB blob — checked exhaustively — and the blob bytes around the offset
  the epc would correspond to are all zero, which cannot decode to the
  reported `inst`;
* the only occurrence in the *loaded* image is inside the `.rodata` string
  literal `"ring_append"`. String literals have no symbols, so `nm` reports
  whatever global happens to sit below them — here the blob.

The build has since shifted and the old address can no longer be resolved
against it, so "a run of `.rodata` string literals" is the better reading but
not proof. What is certain is unchanged and is the part that matters: the CPU
was executing `.rodata` in kernel mode. **Rather than resolve addresses across
builds again, the fatal handler now prints `[Trap Where]`,** classifying `epc`
and `ra` as IN or OUTSIDE `.text` against symbols from the running build.

**Why the captures are truncated -- answered 2026-09-15, and the previous
answer here was wrong.** This entry said the truncation was
`uart_flush_critical()` taking `g_tx_batch_lock` unconditionally, so a hart
that died holding it silenced the other hart's dump. That deadlock is real and
the fix for it stands, **but it is not what truncates these captures.**

`soak14/run25` shows the actual mechanism. One line of the dump printed, the
second did not, and QEMU's guest-error channel then logged

```
get_physical_address: reserved bits set in PTE: addr: 0x802ca000 pte: 0xffffffffffffffff
```

**2,820,123 times** -- one address, one bad entry, repeated identically. The
first of those lines comes *after* the `[Trap Exception]` line, not before: so
the PTE damage is not the cause of the fault being reported, it is what the
**fatal handler itself** hits while reporting it. The handler faults, re-enters
itself, faults again, forever. That run took 795 s against a 178 s norm.

**(a) and (b) are NOT the same event -- an earlier revision of this entry said
they were, on the strength of `run 23` in the same soak, and that was wrong.**
Checked run by run afterwards:

| run | section | trap? | PTE errors |
|---|---|---|---|
| soak14/run25 | RV64 SMP | **yes** | 2,820,123 |
| soak14/run23 | RV64 MMU | no | 0 |
| soak15/run34 | RV32 NOMMU | no | 0 |

Only `run25` is the wild jump. The `NO RESULT` runs land in three *different*
sections and carry no guest fault of any kind -- no trap, no PTE error, no
sched marker. They are a separate problem and are tracked as one below.

`trap.c` already warned about this shape -- "a second fault inside the handler
reporting the first" -- and nothing enforced it. **Fixed** with a per-hart
one-shot guard at the top of the fatal path: a re-entry halts immediately
without printing, since printing is the thing being protected against. Per
hart rather than global, so the other hart can still report its own unrelated
fault. That keeps whatever reached the wire and keeps the run short enough to
still be a test result. The same shape is recorded in
`drivers/uart_16550.c`, where the RX-overrun notifier was moved off
`printk_critical()` because it "hung three suite runs" — but the panic path
cannot be moved off it. `uart_flush_critical()` now uses a bounded
`spin_trylock_irqsave()` (kernel/lock.c) and drains its own per-hart slot
regardless: interleaved characters in a panic beat a panic nobody sees.

**The hand-off guard now actually guards — 2026-09-14.**
`sched_check_incoming()` (kernel/sched.c) checks the `ra` that `ctx_switch()`
is about to restore, and is called at both switch sites. It only ever tested
for `ra == 0`, the phase 27 E4 failure; its own comment said a real text-range
test "would need a symbol all four linker scripts define -- worth doing on its
own". An `ra` pointing into `.rodata` is non-zero, so this exact bug walked
straight through it. All four scripts now define `_ktext_lo`/`_ktext_hi` from
the location counter inside `.text`, and ASSERT that `task_trampoline`,
`sched_yield` and `task_exit` fall inside that window — so the guard's premise
is checked by the linker rather than assumed. A corrupt hand-off now halts
with the scheduler table intact instead of jumping and destroying the
evidence.

**Measured across 104 soak runs, 2026-09-14, and the entry above needs
splitting.** Three soaks since the console-flush fix (24 + 40 + 40 runs) hit
the `NO RESULT` symptom **once**, against 2 in the 42 runs before it: 1.0% vs
4.8%, and P(<=1 in 104 | rate unchanged) = 3.7%. So the rate did fall. It is
not gone.

**The one occurrence carried no kernel fault at all** -- no `[Sched BUG]`, no
`[Trap Exception]`, no `[Trap Where]`, no stall diagnostic, with every one of
those instruments compiled in and armed. That matters, because it means this
symptom and the captured wild jump are **two different failures** that this
entry has been treating as one:

* **(a) a kernel wild jump during `exec`** -- captured exactly once, with a
  real trap dump and an `epc` in `.rodata`. Still unexplained, still not
  reproduced since the instruments went in.
* **(b) the RV64-SMP section going slow or silent** -- `NO RESULT`, no fault,
  the log stopping at the section header. Both `soak6/run3` and
  `soak10/run32` are this. A clean suite is **182 s**; run 32 hit the 600 s
  wall, so that section alone took over 400 s against its usual ~40 s. That is
  a cascade of sub-test timeouts, not a wedged machine -- which is exactly why
  12 clean standalone runs of the same section proved nothing about it.

**One hypothesis for (b) eliminated with evidence.** "A leaked QEMU from an
earlier section spinning at 100% CPU would make the last section crawl, and no
kernel instrument would ever see it." Leftover QEMU processes were counted
before the sweep on all 40 runs of the next soak: **zero, every run**.

**What the longer budget bought.** Raising the outer timeout from 600 s to
1200 s lets a slow section finish and name its sub-test instead of dying
anonymously. It immediately produced one: `SMP: restricted domains were
actually activated on hart 1 (X2)` failing with `domains_hart0: 3` and
**`domains_hart1: 0`** -- a counter read before the other hart had incremented
it. Worth chasing on its own; it is the first named failure ever seen inside
this section.

**150 more runs, 2026-09-15: still not caught, and the rate is now bounded.**
`soak16` ran 150 suites with every instrument armed: **0 traps, 0 NO RESULT**.
With `soak15`'s 60 that is **0 in 210**.

Stated carefully, because this arithmetic has been got wrong once already:
there has been exactly **one** instrumented capture of this fault, ever
(`soak14/run25`). `soak14/run23` was counted as a second when `soak16` was
sized, and it is not one -- it carries no trap markers at all. The rate
estimate was therefore 1/40, not 2/40, and the soak was sized against a number
twice too large.

0 in 210 puts the **95% upper bound at 1.4% per run**. It does **not** mean
fixed: nothing changed since `soak14` that could plausibly cure it, because
the only kernel change in between was the fatal-path recursion guard, and that
is strictly *post*-fault -- it changes what happens after the trap, not whether
the trap happens. With one observation in total, "fixed" and "rare" cannot be
told apart, and asserting either would be a guess.

**What did change and is worth keeping:** QEMU's own diagnostics now go to a
file (`-D`) rather than into the guest's console stream, summarised per session
and deduplicated with counts. That is what stops the next occurrence from
burying its own evidence the way `run25` did under 2.8 million identical lines.

**Earlier negative results, kept:** 120 `exec`s of `uisolate.elf` under
`smpload` on two harts, and 12 consecutive runs of the whole RV64-SMP section,
were all clean.

So this is not "a task that never finishes". It is a **wild jump in kernel
context**, and the spin loop merely reports the aftermath: the child never
reaches `TASK_DEAD` because the kernel fell over while handling it.

**Two things that look like clues and are not.** `entry 0x0` in the loader's
line for `uisolate.elf` is normal -- that image has one segment with its entry
at offset 0, confirmed against a healthy run. And the `exec` diagnostic added
for this did not fire, correctly: the kernel faults long before the wait loop
reaches its threshold.

**Where to look first, narrowed:** `uisolate.elf` is the program whose entire
purpose is to take a domain fault, and it is the binary in three of the four
captured hangs. So the suspect path is **the kernel's handling of a U-mode
fault**, not `exec` itself -- and the ESP32-P4's deterministic "terminated
before it could exit" is very likely the same path reaching a different end.
A jump into `.rodata` from that path means a corrupted return address or a
function pointer built from something that is not a function.

**Superseded question:** why a created task never reaches `TASK_DEAD` and
never faults. Two of three hung guests were running `uisolate.elf`, whose
whole purpose is to take a domain fault, so the fault path is the first
suspect — and the P4's "terminated before it could exit" is the *same* path
reaching a different end. A diagnostic in the spin loop that reports the
child's actual `sched_task_state()` after N yields would say more than any
amount of reading.

## `exec` on the ESP32-P4 -- **FIXED 2026-09-15**: L1 is writeback, `fence.i` is not enough

**Root cause.** The ELF loader writes a program through the data path and then
jumps to it, closing the gap with `fence rw,rw; fence.i`. On this chip that is
insufficient: internal memory is reached *through* L1
(`SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE`) and **L1 data is writeback**, so the
loader's stores can still be dirty in the D-cache when the fetch of those same
addresses misses I-cache, goes to L2/RAM, and reads whatever was there before.
`fence.i` invalidates instruction cache; it does not write back data cache.

**What made it diagnosable at last.** Every earlier attempt saw only "the task
died and printed nothing". Two fixes from the previous days changed that:
`_inst_lo` was `_ram_start` while `.text` runs from flash, so the P4's fatal
handler had been printing `inst=0x00000000` for essentially every fault; and
the U-mode fault path now reports cause/epc/tval. The first run with both in
place printed the answer immediately.

**The evidence that settled it** -- two consecutive `exec`s of the *same*
binary, from a fresh boot each time:

```
[Trap] User task faulted: cause 2, epc=0x4ff5c020, addr=0x269c790b
[Trap] User task faulted: cause 7, epc=0x4ff5c01c, addr=0x1000
```

Same program, same load address, **different fault each time**. A wrong entry
offset or a bad image fails identically; varying garbage at the entry is stale
memory. That also corrects this entry's old "deterministic" label: it failed
every time, but not in the same way.

**Fix:** `esp32p4_icache_sync()` in `arch/riscv/common/xip_esp32p4.c`, called
from the loader beside the existing `fence.i`. Writes back **first**
(`Cache_WriteBack_All`, ROM 0x4fc00414) so the loader's bytes reach memory,
then invalidates the **instruction** cache only
(`Cache_Invalidate_All(CACHE_MAP_L1_ICACHE_0)`). Order is not symmetric:
phase 32 U2 established that invalidating L1 D discards dirty lines rather
than writing them back, which threw away the live stack. Both ROM addresses
come from esp-idf's own `esp32p4.rom.ld`, and the invalidate address there
matches the constant this file already used -- which is what makes the pair
verified rather than inferred.

**Verified on hardware:** `UPROG_TEXT_OK` and `UPROG_DATA_OK` both print, and
`tests/hw/test_esp32p4.py` is **17/17**, EMAC tests included.

**Consequence for the other entries:** this was *not* the QEMU wild jump.
QEMU emulates no caches, so `fence.i` is sufficient there and this fix changes
nothing for it. Folding the two together was wrong, and un-folding them was
right; the QEMU wild jump remains open.

### Original report (kept)

**Un-folded 2026-09-15.** The heading used to say "folded into the entry
above", which contradicted this entry's own closing paragraph ("Related but
distinct ... recording them apart keeps that a question"). The body was right
and the heading was wrong: a **deterministic** failure on one board is very
unlikely to be the same defect as a 2%-per-run race on QEMU, and the same
over-merging has now been caught twice elsewhere in this file.

**It is also the easiest of the family to solve now, and was undiagnosable
before.** Everything this entry says is missing -- "no fault is reported", "a
U-mode fault should have printed something" -- was partly an artifact of the
handler, and every piece has since been fixed:

* `linker/esp32p4.ld`'s `_inst_lo` was `_ram_start` while `.text` executes from
  flash, so the fatal handler printed `inst=0x00000000` for **every**
  flash-resident `epc` -- which since phase 32 is nearly all of them. Fixed
  2026-09-14; the P4 can now read back the faulting instruction at all.
* `[Trap Where]` now says whether `epc` and `ra` are inside `.text`, decided
  against the running build.
* A per-hart guard stops the fatal handler faulting recursively and eating its
  own report.
* `sched_check_incoming()` now validates the incoming `sp` against RAM and the
  parked `ra` against `.text`, instead of only testing `ra == 0`.

So the single highest-value experiment available is one `exec` on the board:
deterministic, one run, and the instrument that was missing is now present.

**Observed** 2026-09-13, immediately after phase 32 enabled chibicc there.
`cc` compiles cleanly; running the result does not:

```
lsh> exec /flash0/system/bin/uhello.elf
[Sched] Created task #6 'uprog' (8 KB)
[Sched] Task #6 'uprog' exited
[ELF] '/flash0/system/bin/uhello.elf' was terminated before it could exit
=> -1
```

The task is created and dies before printing anything. No fault is reported.
The same happens for a chibicc-built binary and for the prebuilt `uhello.elf`,
so it is not about which program.

**This path has never been tested on this board**, which is why it is being
recorded rather than attributed. `tests/hw/test_esp32p4.py`'s U-mode case runs
`umodetest` -- the built-in `umode_probe`, which enters U-mode without loading
anything -- and no test on this board has ever `exec`'d an ELF. So whether
phase 32 broke it or it never worked here is **open**, and the honest answer
is that nobody knows yet.

**What has been ruled out:** `.utext` moved from `0x4ff40000` to `0x4ff41000`
when `.boot` took the first page, which was the obvious phase-32 suspect.
It is still 4 KB-aligned, which is what `board_text_region()`'s single PMP
NAPOT entry needs, so that is not it.

**Where to look first:** `sched_task_exited_cleanly()` is what reports the
failure (arch/riscv/common/elf.c:833), so the question is what the task did
instead of calling `SYS_UEXIT`. A U-mode fault should have printed something;
that it did not is itself a clue. Compare against the RP2350, where the same
`exec` path works, and against a QEMU target where it is covered by the suite.

**Related but distinct:** the entry below is an `exec` hanging on
`rv32-nommu` under QEMU. Same verb, different platform and different symptom
(a spinning guest rather than a task that exits). They may share a cause;
recording them apart keeps that a question.

## An intermittent hang in `rv32-nommu`, executing a chibicc-built binary

**Observed** 2026-09-13, during phase 32 U5. The suite stopped progressing at
the RV32 target with 171 passes and no failures. One `qemu-system-riscv32` sat
at 99.7% CPU for ten minutes; the guest's console showed the compile had
succeeded and the *exec* had not returned:

```
cc /sd0/hello.c /sd0/hello.elf
[    4.414] [chibicc] Build clean: generated 196-byte RISC-V ELF binary at '/sd0/hello.elf'
=> #t
lsh> exec /sd0/hello.elf            <- and nothing, ever
```

The runner's own per-test timeouts are 4-5 s, so it was not waiting on an
expect; it was blocked on a guest that never stopped running. **It did not
reproduce**: the next clean run was 363/363 in 181 s.

**Not phase 32, and this was checked rather than argued.** That phase's only
shared-file change is a `#if defined(CONFIG_BOARD_ESP32P4)` block in
`entry.S`. Building `rv32-nommu` from the commit before the phase
(`55dfdb8`) in a worktree and comparing gives a **byte-identical
`.text.entry`** -- 448 bytes, same SHA-256 -- so the guard holds and the
target's boot code is unchanged. (Comparing whole object files is misleading
here: `-ggdb` embeds absolute source paths, so a worktree build differs for
that reason alone. Compare sections.)

**Where to look first:** `exec` on RV32 NOMMU loads an ELF that chibicc has
just written through the VFS. Candidates are the loader reading a file whose
last block has not reached the device, and the U-mode entry itself spinning
on something that never arrives. A spinning guest rather than a faulting one
means it is a loop, not a trap.

**Relationship to the two entries below is unknown.** Three distinct shapes
have now been seen on this tree -- a truncated single test, an SMP wait-for
cycle, and this hang -- and they may or may not share a cause. Recording them
separately keeps that an open question instead of an assumption.

## `lock_selftest`'s cycle case hangs on two harts — **FIXED 2026-09-13**

Root cause: the wait-for edge was a *sample*, not a state. A ylock waiter
dropped its edge at the bottom of every loop iteration and re-added it at the
top of the next, so it blinked — and `chan_call()`, which consults that graph
to decide whether to refuse a call that would close a cycle, could sample in
the gap, see nothing, and block. On one hart the gap only executes while the
caller is descheduled, which is why it never appeared there.

Fixed in `kernel/lock.c` by not dropping the edge across the yield. Verified
with a negative control, 12 runs of `lockselftest` on two harts each way:
**0 hangs with the fix, 2 without**. A following 12-run full-suite soak had
**zero hangs** (the soak that found it had three).

What it does *not* explain is the entry below, which is still open — and the
same soak produced the first hard evidence of that one, so the two were never
the same problem.



**Measured** 2026-09-13, a 12-run soak of `tests/runner.py`: **3 of 12 runs
hung**, and the three logs are **byte-identical** — 344 passes, then
`[Target: RV64 SMP -- two harts]` and nothing more. The other nine were clean
363/363 at 182–183 s. So the location is deterministic and only the
occurrence is random.

The first thing that target runs is `lockselftest`, and that is where it
stops.

**An earlier version of this entry got the diagnosis wrong, and the mistake is
worth keeping.** It read the `[Lock BUG] … closes a wait-for cycle` output as
phase 31's checker catching a real deadlock in ordinary kernel code. It is
not: `lock_selftest` **provokes** those messages deliberately, and a task
named `cyclewait` exists for exactly that purpose. Most of that output is the
test passing. Reading a log without reading the test that produced it turned
expected output into a false alarm.

**What is actually wrong** is narrower and still real. The cycle case
(kernel/lock.c §6) takes `g_cycle_lock`, spawns `cyclewait` to block on it,
checks that a `chan_call()` into that task is *refused*, then releases and
waits for the waiter to finish. On the failing runs the log fills instead with

```
[Lock BUG] hart 0: ylock_acquire() -- task 7 waiting for task 0 closes a wait-for cycle.
[Lock BUG]   Nothing can refuse this one; it will hang. Fault 3, 4, 5, 6, 7, 8 …
```

repeating from **both** harts with the fault counter climbing. Note which
primitive: `ylock_acquire()`, whose message says outright that it *cannot*
refuse — unlike the `chan_call()` the test is checking, which can and does.
So this is not the case under test firing; it is the waiter re-entering
acquisition after the release and finding a cycle nothing can break.

**Why two harts and not one:** on a single hart the main task and the waiter
interleave only at yields, and the release is observed before the waiter runs
again. On two they are genuinely concurrent, and the window between
`ylock_release()` and the waiter completing is real time rather than a
scheduling point. That file already carries a comment about this exact
fragility — the edge-sampling loop was widened from 200 bare yields to
`task_sleep_ms()` because it "failed about one run in three on two harts"
after Y5c. The same shape, one step later in the same test.

**Where to look first:** whether `g_cycle_done` and the waiter's exit are
being observed in the right order, and whether the `for … 400 sched_yield()`
after the release is the same kind of fragile sampling the loop above it was
fixed for. A `sched_yield()` count is a measure of this hart's run queue, not
of the other hart's progress.

**Not phase 32**, checked rather than assumed: that phase's shared-file change
is `#if defined(CONFIG_BOARD_ESP32P4)`-guarded, and `rv32-nommu` built from
before it has a byte-identical `.text.entry`.

## An intermittent suite failure, ~1 run in 5, since logging went asynchronous

**Trigger:** run `tests/runner.py` repeatedly. Roughly one run in five fails a
single test, and it is a *different* test each time -- `Network Autoconfig`,
`Identity Toolset`, `MQTT Subscribe` have all been seen. The failure shape is
always the same: the command's output is *truncated* in the captured log, so
the test's expect times out with the first lines present and the rest missing.

**Measured** 2026-09-11 across five consecutive runs after Y5f: 362, 363, 363,
363, 363.

**Why it is parked rather than fixed:** it is not specific to any one
milestone. It appeared during Y5c/Y5d as the async console ordering was being
worked out, got much rarer when the drain moved inside `console_lock()`
(Y5d) -- which removed the window where a writer could interpose between a
drain and a write -- and did not go away entirely. A *different single test
each run* is the signature of a race in something shared, and the shared thing
here is the console.

**Caught in the act, 2026-09-13**, which the entry below had asked for. A
12-run soak produced a failing run whose console shows a single `cprintf()`
line cut in two with a klog record in the wound:

```
line 746:  LOCK_SELFTES] record 1463, filling the ring with no consumer running
line 749:  T_OK (16/16)
```

That is `LOCK_SELFTEST_OK (16/16)` split across two flushes. Every check in
that run passed **16/16**; the suite reported a failure purely because the
token it greps for no longer existed as a contiguous string. So the failure
mode is now known precisely: **not lost output, not a hung kernel — a spliced
line.**

**One hypothesis has been eliminated with evidence, which narrows it.** The
obvious suspect was the per-hart TX batch in `drivers/uart_16550.c`: a task
migrating harts mid-string would leave half its line in the batch of the hart
it started on. A probe that counted exactly that — bytes found in another
hart's batch belonging to the current context — **never fired once in six
runs**, so tasks are not migrating mid-string and that is not the cause. The
probe was backed out rather than shipped; a fix for a mechanism that does not
occur is worse than no fix.

**What that leaves.** `cprintf()` holds `console_lock` across the whole string
and `klog_drain()` takes the same (re-entrant) ylock, so the two logical
writers *are* serialised. Yet a klog record landed inside a locked string. So
either a writer reaches `uart_putc()` without that lock, or the splice happens
below the lock entirely. Note `cprintf()` calls `console_flush()` *after*
`console_unlock()` (kernel/printk.c) — the window the per-hart batch split was
introduced to close for two cores, still open for two tasks on one hart.

**The `uart_putc()` enumeration was done, 2026-09-13, and the answer is not
there.** Every writer that could produce klog text — `klog_drain()` and the
boot fan-out — takes `console_lock`, which `cprintf()` holds across the whole
string. The unlocked callers (`uart_net`'s SLIP framing, `/dev/uart` writes,
`ed`, `lisp`) do not emit log records, and `printk_debug()` uses a different
path entirely.

**Three hypotheses have now been eliminated with evidence**, which is worth
more than the guesses were:

* *A task migrating harts mid-string, splitting its line across two per-hart
  TX batches.* A probe counting exactly that — bytes found in another hart's
  batch belonging to the current context — never fired in six runs. The
  speculative fix built on it was backed out rather than shipped.
* *One context appending into another's partially-written batch.* A second
  probe, counting that, never fired either. **Note how this one nearly became
  a false positive:** the first version of the probe called
  `printk_critical()` while holding `g_tx_batch_lock`, and that reaches
  `uart_flush_critical()`, which takes the same non-reentrant spinlock. Three
  suite runs hung, and the hangs were read as "the probe fired and deadlocked
  before reporting". They were the probe being wrong, not the condition being
  present. A diagnostic that can hang is a diagnostic that lies.
* *Racy expects in the suite.* Real, and fixed — see the commit for the eight
  sites — but **not this**. They are a separate failure that was hiding in the
  same statistic.

**What is left, stated narrowly.** Two artifacts survive scrutiny:

1. `LOCK_SELFTES] record 1463, …` then `T_OK (16/16)` — a `cprintf()` line cut
   in two with a klog record in the wound, while `console_lock` was held.
2. A run where `identity provision` was sent and **never echoed at all**, so
   the shell either lost the input or stalled; the refusal it should have
   printed appears nowhere in the log.

Those may be one problem or two. The second is the more interesting, because
a lost *echo* is not a console-output race at all — it points at the input
path or at the shell not running, and nothing so far has looked there.

**Rate, across soaks of twelve runs each** (small samples; the direction is
worth more than the numbers): 9/12 clean before any fix, 10/12 after the
`lock_selftest` fix, 11/12 after the expect fixes. Candidates: a drain that starts after the test's command
has begun printing; the per-hart TX batch flushing at a different moment than
the drain; or klogd's 50 ms idle wake landing mid-command.

**A mechanism found by reading, 2026-09-14 -- not yet confirmed on hardware,
and deliberately labelled as such.** `console_puts()` (kernel/console.c) calls
`klog_drain()` on every invocation. The Y5c comment above it explains the
drain as a sync point "at the start of a whole write, never between two
characters of one" -- and that is true of `console_putc()`, which is where the
drain used to be and where it was splicing "UMODE_OK" between the O and the K.
But `console_puts()` is not a message boundary. It is a string-emitting
primitive, and its three callers call it mid-message:

* **`kernel/line_editor.c` -- 30 call sites**, and a single prompt redraw runs
  through a dozen of them (`"\033[?25l"`, `"\r"`, the prompt, `"\033[K"`,
  cursor moves, `"\033[?25h"`). So a drain can fire, and *block*, between any
  two escape sequences of one redraw. This is the path that **echoes typed
  input**, which makes it the first place to look for both surviving
  artifacts: a spliced echo, and an `identity provision` that was never echoed
  at all.
* **`cprintf()`'s format engine** uses `ps()` for exactly one thing -- the
  timestamp prefix, emitted as `"["`, the width padding, then `"] "`. So a
  whole klog record can be flushed **between the `[` and the `]` of a
  timestamp**. That is the shape of artifact 1: `…] record 1463, …` appearing
  inside another token, with the bracket that precedes it belonging to a
  different message than the text that follows it.
* **`SYS_PRINT`** (arch/riscv/common/trap.c) -- a whole syscall, and the one
  caller for which the drain is in the right place.

This is consistent with everything the eliminations left standing: the two
logical writers *are* serialised by `console_lock`, and the splice still
happens, because the drain is being invoked from **inside** the locked region
by the very function that is emitting it. No writer reaches `uart_putc()`
without the lock -- which is what the enumeration found, correctly. The lock
was never the gap.

**Fixed, and measured -- 2026-09-14.** The drain moved out of
`console_puts()` into a named `console_sync()`, called at the boundaries that
really are whole writes: `cprintf()` already drained right after
`console_lock()` and so needed nothing, `SYS_PRINT` gained an explicit call
(its own comment records that sync point being worth 353/363 -> 361/363), and
both `redraw_line()` and `redraw_box()` now take `console_lock()` across the
whole redraw -- never across `console_getc()`, which waits for a human.

That last part is the substantive half. **kernel/line_editor.c never took
`console_lock` at all**, for any of its thirty writes, while klogd's 50 ms
drain does. Two writers, one of them unlocked, were never serialised against
each other at any point in this file's history.

It survived the way it could most obviously have failed: Y5d records that
locking in this area broke the two-hart boot *deterministically*, and the
verification run was 363/363 including that target.

**The measurement that was claimed for this, and is withdrawn.** An earlier
revision of this entry credited the fix with taking the `I3` test from 2/24 to
0/80, P = 0.09%, on the grounds that `I3` checks `log.count(raw_key) != 1` and
so is "literally a spliced-echo detector". **That attribution was wrong and
should not be repeated.** `I3` has several checks, and the kept logs show it
has never once failed on the raw-key count: every captured failure, before the
fix and after, is the *rename* step -- "rename did not report success",
"identity name: the device write failed", "/proc/node did not answer after
rename". `soak8/run11`, from before the change, is byte-for-byte the same
failure as `soak12/run7` from after it.

Across four soaks `I3` runs 2/24, 0/40, 0/40, 3/40 -- 5 in 144, consistent
with one unchanged rate of about 3.5%, and two zero-soaks in a row at that
rate has probability ~6%, which is unremarkable. So there is **no measured
support for this fix at all.** It rests on its mechanism: the drain did fire
from inside a partially-emitted message, and kernel/line_editor.c did write
the console thirty times without ever taking the lock klogd takes. Both are
true independently of what the soaks show, and neither was measurable through
`I3`, which was never affected by them.

See the `I3` entry below for what that test is actually failing on.

**What it did not fix, stated plainly.** The overall flake rate barely moved
(12.5% -> 10%), because a *different* failure now dominates it: see the MQTT
entry below. Lumping those in with the splice was a mistake of this entry's
earlier revisions, and the numbers separate them cleanly -- MQTT flakes ran
1/24, 3/40, 5/40 across the same three soaks, with no trend.

**What would settle it:** capture a failing run's raw console bytes (the
runner keeps them) and compare the interleaving against the ring's own order
via `/proc/kmsg`, which is authoritative. If the ring order is right and the
console order is not, it is a sync-point gap; if the ring order is wrong, it
is something else entirely.

## The P4's one objcopy warning is documented, not outstanding

**Checked 2026-09-14 and closed the same day, recorded so it is not
re-opened a third time.** Every `esp32p4` link ends with

```
riscv64-elf-objcopy: lugalos.elf: warning: empty loadable segment detected at
vaddr=0x40000000, is this intentional?
```

It looks like the one violation of `memory/zero_warning_policy.md`, and it is
not an oversight: **`CMakeLists.txt` already explains it in full**, at the
`lugalos-ram.elf` step. Removing `.text` and `.rodata` empties the `PT_LOAD`
that covered them and objcopy drops it, which is precisely the intent — the
ROM must not be handed a segment in the flash-mapped window. There is no flag
meaning "yes, intentional"; `-j` keep-only warns about three segments instead
of one; and filtering the text would hide the day it names a different
address. So it stands, explained, as the one place the rule is knowingly bent.

Verified rather than taken on trust: `readelf -lW lugalos-ram.elf` lists four
`PT_LOAD`s, all at `0x4ff…`, and **no segment at `0x40000000`** — the safety
property the whole arrangement exists to produce.


## The P4's fatal handler could not read back a faulting instruction — FIXED 2026-09-14

`linker/esp32p4.ld` defined `_inst_lo/_inst_hi` — the window the fatal trap
handler may safely read an instruction from — as `_ram_start.._ram_end`, over
a comment reading "all code is RAM-resident here". Phase 32 moved `.text` and
`.rodata` into the flash XIP window and made that false without anyone
noticing. Every fault whose `epc` was in flash — which, since phase 32, is
nearly all of them — failed the range test, so `inst=0x00000000` was printed
for it: indistinguishable from a genuine zero word, and on the one target
whose faults are hardest to reproduce.

`_inst_lo` is now `_xip_start`, spanning flash and RAM together exactly as
`linker/rp2350.ld` already did for the same reason.

## MQTT tests flake ~8% of runs: the broker is a one-shot listener

**Now the dominant intermittent**, once the echo splice was fixed: 9 failures
across 104 soak runs on 2026-09-14 (1/24, 3/40, 5/40 -- no trend), spread over
`MQTT Client: CONNECT…`, `MQTT Appliance: Announce…` and `MQTT Config In The
Identity Record…`. A failing run's captured log is always the same shape:

```
mqtt connect 10.0.2.2:40607
mqtt: connecting to 10.0.2.2:40607...
mqtt: the broker refused the connection, or is unreachable
```

**This looks like a test-instrument limitation rather than a kernel bug**, and
the code says why. `tests/mqttbroker.py` binds with `listen(1)` and its
`_serve()` thread calls `accept()` **exactly once** -- there is no loop back to
it, and `keep_listening` defaults to `False`. So the fixture can serve one
connection, ever. If anything consumes that single accept before the CONNECT
the test is waiting on -- a client-side retry, a torn-down probe connection --
the real attempt lands in the backlog and is never accepted, and the guest
waits out its timeout.

Note the guest's own message cannot distinguish the two cases: "refused the
connection, or is unreachable" is printed for a TCP RST *and* for a connection
that opens but never produces a CONNACK. So the log above is consistent with
both "nothing was listening" and "the listener was already used up", and the
message is not evidence for either.

**Confirmed and fixed, 2026-09-14 -- reproduced outside QEMU first.** A
twelve-line harness against `MqttBroker` alone shows it exactly:

```
connection 1: CONNACK 20020000
connection 2: NO CONNACK -> TimeoutError   <-- the flake
accepted: 1
```

The second connection **opens** -- there is no reset -- and then waits forever
for a CONNACK that no accept will ever produce. That is why the guest's
"refused the connection, or is unreachable" was so misleading: the truthful
half of that message is "unreachable", and nothing was refused.

The fix is in `tests/mqttbroker.py`: `_serve()` now loops over `accept()`
until stopped instead of calling it once, `keep_listening` is honoured (it was
stored in `__init__` and read by nothing, so the parameter had never worked)
and now defaults to `True`, the backlog went from 1 to 8, and `self.connections`
counts accepted connections so a future failure can say whether the client
reconnected at all. The same harness now reports `accepted: 3` for three
successive connections.

Nothing needed the old behaviour: `stall_after` and `split_headers` have no
callers, and the tests that *want* a dead broker call `close()` and then build
a fresh one on the same port.

**Measured, and it did not eliminate the flake.** 1 failure in the 40 runs
after, against 9 in the 104 before -- 2.5% vs 8.7%, but P(<=1 in 40 | rate
unchanged) = 12.6%, so the soak on its own proves nothing. The *mechanism* is
proven (reproduced and fixed outside QEMU, above); what is not proven is that
it was the only mechanism, and the residual failure has the same shape. The
failure message now reports `accepting.connections` / `refusing.connections`,
which distinguishes the two halves of the guest's ambiguous message: non-zero
means the broker accepted something and the fault is above the socket, zero
means nothing ever arrived.

**It cost 50 s a run, and the cause was not what it looked like.** Suite time
went 182 s -> 232 s, and reverting *only* `tests/mqttbroker.py` put it back to
182.46 s. The X2 retry added in the same batch was ruled out first: a probe
showed `domains_hart1` non-zero after a single `exec` in 10 runs of 10, so that
loop almost never takes a second pass.

It was not guest work at all -- it was `close()`. Timed directly, with no QEMU
involved:

```
keep_listening=True   close() took 5.000s  thread_alive=True
```

**Closing a socket from another thread does not wake a thread blocked in
`accept()` on Linux**, and `close()` ends in `join(timeout=5.0)`. That cost was
invisible while `_serve()` exited after one connection -- a served broker had
no thread left to join -- and became five seconds per broker the moment the
loop sent it back to `accept()`. With about fourteen broker instances in a
suite run (7 creation sites, exercised by both the rv32 and rv64 sections),
that is the whole 50 s.

`recv()` has the same property, and that case mattered more: the suite closes
its brokers in a `finally` while the guest is usually **still connected**, so
the thread is parked in `_serve_conn()`'s recv rather than in accept. That path
cost the full 5 s even before the accept loop existed.

Fixed by giving both sockets a 0.25 s timeout, so the loops get to look at
`self._stop` promptly; both already treat `socket.timeout` as "keep going".
Measured after, across the four cases that matter:

```
idle             close() 0.201s  alive=False  connections=0
served+closed    close() 0.201s  alive=False  connections=1
live connection  close() 0.201s  alive=False  connections=1
3 successive     close() 0.000s  alive=False  connections=3
```

Since the pre-existing 182 s baseline also paid the live-connection stall, the
suite should now come in *below* it. Not yet confirmed end-to-end: the fix
landed while a soak was mid-flight.

## SMP X2 reads `domains_hart1: 0` about one run in forty

**First seen 2026-09-14**, and only visible because the soak's outer timeout
was raised from 600 s to 1200 s -- at 600 s this section was being killed
before it could report anything (see the `exec` entry's (b)).

```
  [FAIL] SMP: restricted domains were actually activated on hart 1 (X2)
    domains_hart0: 3
    domains_hart1: 0
```

Hart 0 activated three restricted domains; hart 1 reported none.

**It is not a missing synchronisation primitive**, checked rather than
assumed:

* `g_domain_activations[hart_id()]++` -- each hart increments **its own**
  slot, so no update can be lost however the two interleave;
* those increments happen inside context switches that take and release
  `g_sched_lock`, so they are fenced, and the read happens seconds later;
  stale visibility cannot turn a non-zero into a zero across that gap;
* the counter only moves `if (d)` -- for a task that actually *has* a domain.
  Driver tasks are pinned to hart 0 and most kernel tasks carry no domain, so
  at this point in the test the only domain-carrying unpinned task is `uprog`.

And the run that failed had **`SMP: an out-of-domain store still faults` PASS**
immediately above it. The isolation worked; only the evidence that it happened
*on hart 1* was missing. The test's own comment says `uprog` is "left unpinned
on purpose -- this is the case where the scheduler chooses", so a run where the
scheduler kept it on hart 0 leaves the counter at zero with nothing wrong.

**Fixed in the test, 2026-09-14:** the check now retries, re-`exec`ing
`uisolate.elf` up to six times and passing as soon as hart 1's count moves.
That proves what §1 actually claims -- the secondary does install restricted
domains -- where a single attempt only sampled whether it did so this time.

`g_domain_activations` was also made `volatile`, because a word one hart
writes and another reads is a data race in C's model even when every hart owns
its index. That is hygiene and is documented at the declaration as **not** the
cause of this failure, so it is not mistaken for one later.

## `identity name` intermittently fails to persist a rename (I3, ~3.5%)

**The real cause of the `I3` flake**, separated 2026-09-14 from the console
splice it had been wrongly filed under. Rate across four soaks: 2/24, 0/40,
0/40, 3/40 -- 5 in 144, no trend, and present on both `rv32-nommu` and
`rv64-mmu`. It predates every console change made this session:
`soak8/run11` and `soak12/run7` are the same failure.

Three captured shapes, all of them the rename step:

```
rename did not report success:   identity name toolset-test        (no reply at all)
identity name: the device write failed
/proc/node did not answer after rename
```

The middle one names it: `node_identity_rename_persistent()` got non-zero from
`identity_store_write()` (kernel/identity.c). That function has three ways to
fail and one of them is already excluded:

* `scratch_acquire()` of 8 KB -- **not this**, it prints "[Identity] not
  enough free heap to rewrite the record" and no failing log contains it;
* `rc != 0` from the `idstore_writer_add_field()` sequence -- the record is
  small here (uid + name) and nowhere near the 4 KB limit, so unlikely;
* `idstore_writer_commit()`, whose whole body is
  `dev->write_blocks(dev, w->buf, 0, IDSTORE_BLOCKS)` -- **the remaining
  suspect**, i.e. the write to the identity block device itself.

**A correlation that looked strong and is an artifact**, recorded so it is not
rediscovered: `[Sched] Task #5 'mqttd' exited` appears in the captured window
of every failing run and in no passing run. That is not a signal -- the runner
only prints guest output for tests that *fail*, so any guest line can only
ever be seen in a failure.

## ROOT CAUSE FOUND, 2026-09-14: the identity block device has no mutual exclusion

The diagnostic above was added, and the first soak with it caught the failure
on run 15:

```
[    0.122] [Identity] existing record on 'virtio_blk_id0' unreadable (corrupt,
or the device read failed); rewriting from the patch alone -- any uid, key or
grants it held are being dropped
```

No `write_blocks(...) returned` line anywhere in that run, so **the write was
never the problem** -- the *read* was, which is why three soaks of staring at
the write path found nothing.

**What it actually breaks, and why the test message was misleading.** The
failing step in run 15 is not the rename at all, it is the second `identity
provision`, which is supposed to be refused:

```c
if (idstore_read(dev, &rec) == IDSTORE_VALID && !force) return NODE_ID_ERR_POPULATED;
```

A read that returns `IDSTORE_CORRUPT` walks straight through that guard and
re-provisions a populated store. The same substitution explains the other two
shapes: a rename whose `idstore_read()` fails rewrites the record *from the
patch alone*, silently dropping the uid, the device key and the grants.

**The mechanism, in `drivers/virtio_blk_id.c`.** Everything the transfer needs
is a single shared static -- one request header, one status byte, one
three-descriptor chain, one `last_used_idx` -- and the completion wait is

```c
while (g_id_vq_mem.used.idx == g_id_last_used_idx) { sched_yield(); }
```

There is **no lock in the file at all**. The yield does not merely permit a
second caller, it invites one: the arriving caller overwrites the header, the
descriptors and the status byte while the first is parked, both then observe a
`used.idx` that moved once, and the first returns the second's status or
reports success over a buffer nothing filled. The device is reached indirectly
through `identity_store_device()` "from anything that touches the record" --
the file's own comments say so twice -- and several of those callers run in the
same early-boot moment: network autoconfig, the MQTT config read,
`node_identity_init()`, and the shell. `t=0.122 s` is exactly that moment.

**Fixed** by serialising `virtio_blk_id_transfer()` on a `ylock` -- a ylock and
not a spinlock because the critical section contains a yield, which is
precisely what a spinlock may not span. A compiler barrier was also added
after the completion wait, before the status byte and the device-written
buffer are read; the file already uses that idiom on the submit side.

**Not yet re-measured against the 5-in-144 baseline** -- the fix was written
while a soak was still running on the previous binaries, and rebuilding mid-run
would have mixed provenance.

## `NO RESULT` runs are a host-side stall, not a guest hang (the old "(b)")

**Separated from the `exec` wild jump 2026-09-15**, after the recursion guard
made it possible to tell the two apart. Four occurrences across ~250 soak runs,
in **three different target sections** (RV64 SMP, RV64 MMU, RV32 NOMMU), and
apart from `soak14/run25` -- which is the wild jump and belongs to the other
entry -- none of them carries a guest fault: no `[Trap …]`, no `[Sched BUG]`,
no PTE error, nothing.

`soak15/run34` is the informative one, because the runner diagnosed itself:

```
[Retry] rv32: 1 failure(s) with no fault marker (possible QEMU host-stdio
stall, not a guest crash) -- retrying the whole run from a fresh boot
(attempt 2/3) before treating as real.
```

`tests/runner.py` already carries that mechanism and the reasoning behind it:
QEMU's `-nographic` chardev sets `O_NONBLOCK` on its console fd and is
documented not to always retry a `write()` returning `EAGAIN`. Its comment is
careful to say this is "no longer the *known* cause of anything, kept in case
it is ever the cause of something new" -- and it now looks like it is.

**The harness turns a recoverable flake into a total loss.** The retry re-runs
the *whole architecture* from a fresh boot, up to three attempts, and the outer
soak timeout is 1200 s against a 178 s norm. `run34` spent the entire budget
inside those attempts and reported nothing, so a mechanism built to absorb the
stall produced a `NO RESULT` instead.

**Open question, and it is the one that matters:** the retry only fires when
*no* failing log mentions a fault marker, which is exactly what a genuine
silent guest hang also looks like -- the runner's own comment says so. So
"host-stdio stall" is the leading explanation, not a proven one. Worth doing:
record per-attempt timings so a stalled attempt can be told from a slow one,
and check whether the stall correlates with a particular test rather than a
particular section.



## Creating a new file on the ESP32-P4's /flash0 fails, though rewrites work

**Found 2026-09-17 while running 34.11's write soak**
(`plan/phase34_esp32p4_pll_bringup.md`). Not a phase-34 defect and explicitly
not caused by the second core — the control below is the point of this entry.

`write /flash0/newname.txt something` returns `#f`. Rewriting a file that
already exists returns `#t` and the content reads back correctly.

**It is not the second core.** Checked both ways from a fresh boot:

| | create new file | rewrite existing |
|---|---|---|
| `harts online = 1` | `#f` | `#t`, content verified |
| `harts online = 2` | `#f` | `#t`, content verified |

Identical single-core, so 34.11's flash-park protocol is not implicated, and
neither is anything else phase 34 changed.

**It is not space.** `/proc/df` reports `/flash0` at **15 %** — 870 of 1024
512-byte blocks free.

**Leading suspicion: the root directory is out of entries.** `ls /flash0`
shows a modest file count, but a FAT32 root directory is a fixed number of
entries and long filenames consume several apiece. Worth confirming before
acting on it. A second candidate is that the create path takes a different
route through `vfs_write()` than the overwrite path and fails somewhere that
returns no diagnostic — the absence of *any* message beyond `#f` is itself a
finding, since the flash driver is talkative about refusals.

**Worth doing:** make the create path say why it failed before guessing at
the cause. Everything above was inferred from a boolean.

## The shell's `write` exhausts the Lisp string pool after ~60 calls

Same session, same soak. Write 61 of 120 failed with

```
[Lisp Error] String pool exhausted! Further strings/symbols will alias.
```

`kernel/shell.c`'s `write` command builds a Lisp call and evaluates it
(`write "path" "text"`), so each invocation interns two strings that are
never reclaimed. Harmless interactively -- nobody types 60 writes -- and it
is a real limit for any scripted use of the shell, which is exactly what
`tests/hw/` does.

The filesystem was unaffected: the soak's next read-back checkpoint passed,
and all six checkpoints across 120 rewrites read back correctly.
