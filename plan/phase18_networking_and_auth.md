# Phase 18 — Reachable over a network, and safe to be

**Status: planned 2026-08-24.** Supersedes phase 14's 14c (security/
authentication) and 14d (real networking), which were three sentences of
background written before phases 15, 16 and 17 landed.

**Scope, deliberately narrow.** One networking technology: the **W5500**
Ethernet module (WIZnet W5500 + HanRun HR961160C magjack) on a **dedicated
gateway persona** running on a bare Pico 2 (RP2350, non-W). Everything else
that could be called networking — the RP2350W's CYW43 wireless (binary blobs),
the K210, the ESP32-P4 — is **out of scope and stays out**, to be picked up in
a later phase on its own terms (phase 14's 14e).

---

## 0. What phase 14 got right, and the one thing that is now wrong

14c's sequencing argument stands and gets sharper: **auth before network, not
after.** Phase 14a made remote code execution a *feature* — drop a `.lisp`
file into a watched directory and it runs (14a, "Remote-triggered Lisp
execution"). Over a physically attached cable that is a convenience. The
moment the same 9P server answers on a LAN, it is a remote shell for anyone
who can open a socket. `fs/9p.c`'s `p9_handle_tattach()` accepts any attach
from anyone today, and `P9_TAUTH` is defined in the enum and handled nowhere.

**What is now wrong is 14d's testing strategy.** It says: prototype 9P-over-IP
against QEMU's virtio-net first, then W5500. The instinct — test the protocol
layer without hardware — is usually right and is wrong here:

> **The W5500 contains the TCP/IP stack in silicon.** It terminates TCP,
> handles ARP, ICMP and retransmission itself, and hands us a byte stream over
> SPI. Going virtio-net first would mean writing a software TCP/IP stack
> *specifically to have something to prototype against* — on the one target
> whose RAM budget is enforced at link time — to exercise a layer the real
> hardware will never run.

So this phase inverts it. The seam is a **byte stream**, the W5500 provides
one, and `p9_link_t` (`fs/include/fs/p9_link.h`) is already the abstraction
for "9P over some stream". No IP stack is written, and the protocol layer
above the stream keeps being tested on QEMU over the links that already exist.

If a software IP stack is ever wanted (raw sockets, UDP, a wireless part with
no offload), it becomes its own phase with its own justification — not a
testing scaffold smuggled in under this one.

### And the TCP path is still testable on QEMU — without an IP stack

The obvious objection to §0 is that dropping virtio-net drops the ability to
test any of this in CI. It does not, and the reason is worth stating because
it decides how N2 and N6 are verified.

**QEMU's chardev backend speaks TCP.** `tests/runner.py` already wires the
guest's virtio-console 9P link to a *unix* socket (`-chardev socket,...`);
the same chardev takes `host=127.0.0.1,port=N,server=on` instead. The guest
side is unchanged — it still sees a byte stream on virtio-console, which is
exactly the shape the W5500 will hand it — while the **host** side becomes a
real TCP socket that `host/p9lib` can connect to with the same
`connect_tcp()` the gateway will need.

So the *entire host-side network path* — `connect_tcp()`, `fuse-p9` over TCP,
and every case of the auth exchange in §6 — runs in the QEMU suite, on both
targets, with no hardware and no IP stack. `p9lib`'s transport layer already
has a socket-shaped path (its unix-socket connector), so the addition is
small.

**What this does not cover, and hardware must:** the W5500's own TCP
semantics — partial sends when the socket buffer fills, `RECV` chunking,
a peer that resets mid-frame, link-down and reconnect. A localhost TCP socket
never does any of those things badly enough to catch a bug. That is the same
split phase 17 used for DCF-77 (decoder on QEMU, radio on hardware) and it is
the honest boundary here too.

## 1. Threat model, stated up front

**What is being defended:** the 9P namespace this project exports — the SD
card, `/flash0`, `/proc`, `/dev`, and the watched directory that *runs Lisp
programs by design*.

**Against whom:** anything else on the same LAN. Not a nation state, not
someone with an oscilloscope on the SPI bus, not someone who can unplug the
board and read its flash.

**What is explicitly NOT defended:**

* **Confidentiality.** 9P frames go over the wire in clear. Auth proves *who
  is attaching*; it does not hide what they then read. Encryption is a
  separate, larger question (and a much worse fit for a 2 KB msize).
* **The UART downlinks.** A cable between two boards on the same desk is
  treated as trusted physical wiring. Auth lives at the network edge.
* **Physical access.** The pre-shared key sits in flash; anyone holding the
  board has it.

Saying this here is the point: an auth mechanism whose limits are not written
down gets credited with properties it does not have.

## 2. The hardware, as far as it is known

**Module:** the board is a **USR-ES1** (or one of its many clones): a WIZnet
**W5500** behind a HanRun **HR961160C** RJ45 magjack. The part number on the
metal shell is the *jack*, not the module — the vendor's own manual shows an
`HR961160A` on the same PCB, so the suffix is a magjack variant and not a
different board.

Hardwired TCP/IP: ARP, ICMP, IGMP, TCP, UDP, PPPoE; 8 independent sockets;
32 KB of internal socket buffer; 10/100 MAC and PHY with auto-negotiation.
SPI mode 0 or 3, up to 80 MHz. PCB 23 × 25 mm.

**The pinout, from the vendor manual** (USR-ES1 Ethernet Module Manual §3.2),
two 1×6 headers flanking the jack, **top side view**:

```
        ┌──────────────────────────┐
  1-1   │ GND                 GND  │  2-1
  1-2   │ GND               +3.3V  │  2-2      RJ45 (HR961160x)
  1-3   │ MOSI              +3.3V  │  2-3      between the two rows
  1-4   │ SCLK           NC/PWDN   │  2-4
  1-5   │ SCSn               RSTn  │  2-5
  1-6   │ INTn               MISO  │  2-6
        └──────────────────────────┘
```

**Confirmed against the physical module** (user, 2026-08-24): the pinout is
printed on the board itself and matches the table above, and **`2-4` is `NC`**
— which settles the manual's own diagram/table disagreement (its diagram
labels that pin `PWDN`). Leave it unconnected.

Two more things worth carrying into the wiring:

* **Power.** The vendor asks for "+3.3V, current not less than 200 mA"; the
  Pico 2's 3V3 rail is good for ~500 mA (user, 2026-08-24), so there is
  headroom — but it is worth knowing what spends it: ~200 mA for the module
  with the link up, ~50-100 mA for the RP2350 itself, and an SD card that can
  pull 100 mA in bursts while writing. Worst case lands around 400 mA, inside
  the budget but not by a wide margin. If this board ever behaves strangely
  under simultaneous network *and* card load, that is the first thing to
  measure rather than the last.
* **Reset timing**: the manual says hold `RSTn` low ≥2 µs and wait ≥150 ms
  after release (its text says "W5200" — inherited from an older module's
  documentation, one of several such slips). The W5500 datasheet asks for
  ≥500 µs low and ~1 ms for PLL lock. Be generous: 1 ms low, 150 ms settle
  before the first register access. It costs nothing at boot and it is the
  kind of margin that is invisible when right and maddening when wrong.

I/O is 5 V tolerant, which is irrelevant here — both ends are 3V3 — but worth
knowing before anyone reaches for a level shifter.

### Building it properly: what a soldered gateway wants

The bring-up rig is jumper wires, and `net bustest` has shown that is
electrically fine at 12.5 MHz (see "The bus is not the problem" below). The
first item below, however, *is* about fixing a fault: it is the one that made
this hardware work at all, and the rest is about turning a rig into an
appliance. Recorded here because it is much easier to do while the iron is
already hot than to retrofit.

* **Decoupling at the module, and it is not a nicety.** **220 µF across the
  power rail and 100 nF directly at the W5500's Vcc-GND pins.** An earlier
  draft of this list said "100 nF plus a 10 µF bulk", which was right in kind
  and an order of magnitude short in value -- and that gap cost this phase
  days. Without it the PHY will not negotiate reliably, will not reach 100BT,
  and will hold a link that carries no frames while every register in the chip
  reads correct. With it: 100BASE-TX full duplex in 2.0 s. See "Resolved: it
  was the module's power" below for the full before/after.

* **Short leads on SCK, MOSI, MISO and CS** -- under ~5 cm if the layout
  allows -- with a **ground return running alongside them** rather than a
  single ground taken from somewhere else on the board. A fast edge needs a
  return path next to it; giving it a long one is what turns a clean bus into
  a marginal one at higher speeds.
* **Series resistors, 22-33 Ω, on SCK and MOSI at the Pico end.** The cheapest
  possible insurance against reflections on an unterminated stub, and the
  single change most likely to keep 12.5 MHz clean on a real board.
* **Use both `+3.3V` pins (2-2, 2-3) and at least two grounds (1-1, 1-2,
  2-1).** At that current, one thin conductor per rail is a measurable drop,
  and the module's own header offers the parallel paths for free.
* **Bring out the UART1 downlink as a 3-pin header** -- GP8 (TX), GP9 (RX),
  GND -- so §4's cable is a plug rather than a soldering job later.
* **Keep SPI1/SD physically away from SPI0/W5500** where the layout allows;
  they are independent buses and there is no reason to run them together.
* **Power budget, from §2**: ~200 mA for the module, ~50-100 mA for the
  RP2350, and an SD card that bursts to ~100 mA while writing, against a 3V3
  rail good for ~500 mA. It fits, without a wide margin -- so if a finished
  board ever misbehaves under simultaneous network *and* card load, measure
  the rail before suspecting software. **This turned out to be the single most
  useful sentence in this plan, and it was written before the fault appeared
  and then ignored for days.**

## 3. The gateway persona

`cmake/board-rp2350-gateway.cmake` + a `rp2350-gateway` preset, on a bare
Pico 2. A third persona alongside chess and clock, and cheap now precisely
because phases 7 and 11 made "which hardware this board has" a per-board table
rather than a fork.

Built **out**: `ST7735`, `TM1638`, `CHESS`, `PICO_CLOCK_GREEN`, `DCF77`. No
display, no keypad, no radio.

Built **in**: UART0 (console), UART1 (the downlink, §4), USB CDC (console +
the ACM1 9P link that already exists), the flash filesystem `/flash0`, the 9P
server, the new W5500 driver — **and the SD card**.

### Why the SD card is in (user's question, and the answer is yes)

RP2350 has two SPI controllers and the W5500 needs one. Spending the other on
the SD card the chess persona already drives costs **~3.2 KB of static RAM**
(`drivers/spisd_rp2350.c` is 3189 bytes in the chess baseline; `fs/fat32.c`
carries no `.bss` of its own) on a persona that has no display buffer, no
chess engine and no font tables to pay for. That is affordable in a way it
would not be anywhere else.

What it buys is more than storage:

* **The gateway stops being a router and becomes a file server.** A LAN-
  attached node whose namespace is empty is a demo; one with a real
  filesystem behind it is the thing this project has been building toward
  since phase 5. It is also, bluntly, the most natural use of a box whose
  only jobs are "be on the network" and "hold files".
* **It gives the keys somewhere to live** — see §6, and the user's own
  follow-up question, which the SD card is what makes answerable.
* **It gives the hardware tests something to do.** `ls`, `cat`, `mkdir`, write,
  re-read over Ethernet against a real FAT32 volume exercises far more of the
  9P server than reading `/proc`.

The wiring is the chess persona's, unchanged and already proven: SPI1 on
GP10-13.

**Pins.** No conflicts between the three groups — SPI0 (16-19 + 17/20/21),
SPI1 (10-13) and UART1 (4/5) are disjoint, and all are valid function
assignments for those controllers on RP2350:

| function | GPIO | note |
|---|---|---|
| `SPI0 SCK` | 18 | the W5500 bus; same pins the chess persona gives ST7735 |
| `SPI0 MOSI` | 19 | → module `1-3 MOSI` |
| `SPI0 MISO` | 16 | → module `2-6 MISO`; ST7735 is write-only and never wired one |
| `W5500 CSn` | 17 | → module `1-5 SCSn` |
| `W5500 RSTn` | 20 | → module `2-5 RSTn`; 1 ms low, 150 ms settle |
| `W5500 INTn` | 21 | → module `1-6 INTn`; input, pull-up; polled first (N4) |
| `SPI1 SCK/MOSI/MISO/CS` | 10 / 11 / 12 / 13 | the SD card, chess persona's wiring |
| `UART1 TX/RX` | 8 / 9 | the downlink to a board — see §4 |
| `3V3` / `GND` | — | module `2-2`/`2-3` and `1-1`/`1-2`/`2-1`; see the 200 mA note in §2 |

The GPIO column is this project's choice; the module column is confirmed
against the board's own silkscreen (§2).

### The gateway's own address, and where that lives

The W5500 holds its network identity in registers the host MCU writes at
init — `SHAR` (MAC), `SIPR` (IP), `SUBR` (netmask), `GAR` (gateway). It has no
persistent storage of its own and **no DHCP in hardware**: DHCP is a protocol,
implemented in software over a UDP socket, not a feature of the chip. So
something on this side has to decide the address, and the question is what.

**The MAC is derived, not configured.** RP2350 has a unique ID (the flash
chip's, read through the bootrom), so the gateway builds a stable
locally-administered MAC from it: first octet `0x02` (locally administered,
unicast), the rest from the ID. Unique per board, stable across reboots and
reflashes, and nothing to set up. A config file may still override it, for the
case where a network insists on a particular MAC.

**The address comes from the SD card, as Lisp, using the boot path that
already exists.** `/sd0/system/etc/usr_init.lisp` is already loaded at boot
when present (it is how a persona picks its program today), so the gateway's
network config is a line in it:

```lisp
(net-config "192.168.1.50" "255.255.255.0" "192.168.1.1")
```

This is deliberately *not* a new file format with a new parser. The project's
configuration idiom is already "Lisp evaluated at boot from the card", the
loader already exists, and a `(net-status)` primitive alongside `(dcf-status)`
is the shape this codebase already uses for "what is this peripheral doing".
The cost of the alternative — a `key=value` file — is a parser, an error
story, and a second convention, to express three dotted quads.

**Board-file fallback, and failing loudly.** The board file may carry a
compiled-in default; if it does not, and the card has no `net-config`, then
**the link stays down and says so** — on the console and in `/proc/net`.
A network device that silently picks a hardcoded address on a network it knows
nothing about causes address conflicts that are somebody else's afternoon.
Unconfigured-and-noisy is the better failure: the board is still reachable
over USB, which is exactly what the console is for.

**Why static rather than DHCP, for now.** This box is a *server* — the whole
point is that a host can mount it — and servers want a stable address. DHCP
would give one only via a reservation, which is router configuration anyway.
Against that, a DHCP client is a UDP socket plus a real state machine
(DISCOVER/OFFER/REQUEST/ACK, option parsing, T1/T2 lease renewal, and a
policy for what to do when the lease cannot be renewed) — several hundred
lines whose failure modes all present as "the board is not on the network".
Deferred, and listed in §8 rather than left implied.

**One note on where this sits relative to the keys.** The network config and
the key list both live under `/sd0/system/etc/`, but they have opposite
exposure requirements: the config is harmless — arguably useful — to read
remotely, while `auth/` must never leave the board (§6). Only the `auth/`
subtree is refused; the rest of the tree stays a normal, servable filesystem.

## 4. How the gateway connects to the existing boards

This is the question the phase has to answer concretely, so: **UART, with SLIP
framing, using the link this project already built and tested.**

`link_uart_slip` (A3a, `drivers/uart_net.c`) and the `p9serve` / `p9share`
shell commands exist and are covered by the QEMU suite today ("9P Server
Reachable Over UART/SLIP Link", "9P + Console Coexist On One UART"). Nothing
new is needed on the *board* side at all — which is the whole reason the
gateway is a separate box rather than an expansion of the chess persona.

**The cable** (three wires, and the third one matters):

```
   gateway (Pico 2)                     board (chess or clock)
   UART1 TX  GP8  ───────────────────►  UART0 RX  GP1
   UART1 RX  GP9  ◄───────────────────  UART0 TX  GP0
   GND            ─────────────────────  GND
```

**GP8/GP9, not the GP4/GP5 this section first proposed.** Both are valid
UART1 pins, but GP4/GP5 are I2C0's, and `drivers/i2c_rtc.c` is built for every
RP2350 persona -- it configures those pads at boot whether or not a clock chip
is fitted. Two drivers claiming the same pads is a conflict that shows up only
as one of them mysteriously not working.

Both are 3V3 parts, so this is a direct connection with no level shifting.
Common ground is not optional: without it the two UARTs have no shared
reference and the link either fails outright or, worse, works intermittently.

**On the board side, `p9share`, not `p9serve`.** `p9serve` takes the whole
UART and never returns; `p9share` demultiplexes 9P frames and console bytes on
the same wire (A3b), so one cable gives the gateway a namespace *and* leaves
an interactive console reachable. On the clock persona, whose console is
already bound to `uart`, that is the difference between a debuggable appliance
and a mute one.

**Why UART and not something faster.** USB is not available (both boards are
USB *devices*; neither is a host). SPI would need one of the boards to be a
slave and neither driver is. I²C is slower than the UART for this. And the
UART path is the one with tests behind it. The cost is throughput, and the
plan is honest about it: at 115200 baud a 2 KB msize round trip is ~350 ms of
wire time, which is fine for `ls`, `cat` of a config file, or dropping a Lisp
program into a watched directory, and poor for pulling a PGN archive.

**Two things the downlink needed that did not exist**, both now built as
`drivers/uart1_link_rp2350.c` (N5):

1. **A second UART instance.** `drivers/uart_rp2350.c` is UART0-only, and
   generalising it was the wrong move: that file is *the console*, carrying a
   channel endpoint, a U-mode task, the p9share demux, ACCESSCTRL grants and a
   heartbeat LED, all of which exist because the console is shared and
   contended. This wire is neither. It got its own small driver instead —
   init, polled byte I/O, and a `p9_link_t`. The framing is genuinely shared:
   `slip_feed()` is now exported from `drivers/uart_net.c`, because a SLIP
   escape state machine written twice is one that differs twice.
2. **A configurable baud rate.** `uart_rp2350.c` carries
   `REG(UARTIBRD) = 81; REG(UARTFBRD) = 24;` with a comment naming 115200 —
   correct, and silently wrong the day anyone wants a faster downlink. The new
   driver computes the divisor from `CONFIG_UART1_BAUD`: the PL011 wants
   `clk_peri / (16 * baud)` in 6.6 fixed point, which is exactly
   `(4 * clk_peri) / baud` as an integer.


**More than one board** is deliberately not solved here. RP2350 has two
hardware UARTs and the gateway spends both. A second downlink means PIO
UARTs, a multi-drop bus, or daisy-chaining boards — each a real design
question, none of them needed to prove the phase. One gateway, one downlink,
one board at a time.

## 5. The shape: the gateway is a node, not a proxy

The gateway does **not** relay bytes between TCP and UART. It mounts the
board's namespace and re-exports its own:

```
   host (Linux/macOS)
     │  9P over TCP:564, authenticated
     ▼
   gateway  /                     ← its own namespace
            /proc  /flash0  /dev
            /chess ──── 9P over UART/SLIP ────► chess board's namespace
     (later /clock ─── a second downlink, out of scope)
```

`vfs_mount_remote()` + `p9_remote_mount_*()` (A5) already do exactly this, and
`(mount-remote "chess" "uartslip")` already exists as a Lisp primitive
resolving a link by device-registry name. So the gateway's whole "routing"
behaviour is one mount call at boot, and a host that mounts the gateway sees
the board's files inside it.

That reuse is the payoff from phase 5's design work, and it is the reason a
byte relay would be the wrong shape: a relay can serve exactly one board and
composes with nothing, while a mount composes by construction.

**The extension point for the W5500 is equally small.** A new
`p9_link_t` backend registered in `kernel/board.c` as a `DEV_KIND_P9LINK`
device with `DEV_F_BACKGROUND_9P` — the same three lines `usbnet` and
`uartslip` already are. Everything downstream (the server task, background
polling, `mount-remote`, `/proc/devices`) then works unchanged.

## 6. Authentication (14c)

**9P-native, via `Tauth`/afid.** Not a link-layer wrapper below 9P: the afid
mechanism exists in the protocol precisely for this, it keeps every existing
message and tool working, and it leaves the door open to real 9P clients. The
tradeoff, stated: a link-layer wrapper would also protect `Tversion`, which
`Tauth` does not. `Tversion` carries no secrets and resets connection state
that an unauthenticated peer cannot then use, so that is an acceptable
exposure — but it is an exposure, not an oversight.

**The exchange**, which needs no new message types and mirrors how Plan 9's
own auth files work (read a challenge, write a response):

```
  C → Tauth  afid, uname, aname
  S → Rauth  qid                     (afid is now an auth file)
  C → Tread  afid, 0, 32   ← 32-byte server nonce
  C → Twrite afid, 0, 32   → HMAC-SHA256(psk, nonce ‖ uname ‖ aname)
  S           verifies, marks the afid authenticated
  C → Tattach fid, afid, uname, aname
  S           accepts only if that afid is authenticated and matches
```

**Policy is per link, and fails closed on the network.** A link carries a flag
saying whether attaches on it must be authenticated. The W5500 link sets it;
the loopback channel and (by default) the UART/USB cable links do not, because
authenticating a cable you already trust only adds a way to lock yourself out.
A link that requires auth and has **no key configured refuses every attach** —
the failure mode of a misconfigured gateway must be "nobody gets in", never
"everybody does".

**Keys, plural — one per identity.** `Tauth` carries a `uname`, so the natural
shape is a lookup *by user*, not one shared secret for the world:

```
/sd0/system/etc/auth/keys      one line per identity, "uname hexkey"
```

That gives per-user revocation (delete a line), lets the gateway know *who* is
attached rather than only *that* someone valid is, and costs nothing over a
single key — the lookup is a linear scan of a handful of lines. It is the
direct consequence of putting the SD card on this persona (§3): before that
there was nowhere sensible to keep a list.

Search order: the SD keys file, then `/flash0/system/etc/p9key` as a
single-key fallback for a gateway with no card, then **nothing — and a link
that requires auth with no keys refuses every attach.**

**The keys file must not be servable, and that is a server-side rule.** The
gateway exports `/sd0` over the very network the keys defend; without this,
any authenticated user simply reads everyone else's secret and the whole
mechanism collapses into "one shared key, awkwardly spelled". `fs/9p.c` (not
the host tools — the client is the attacker here) refuses to walk to or open
anything under `/sd0/system/etc/auth/`, the same way `fs/vfs_server.c` already
validates names under `/dev/`. Writing that down as a milestone deliverable
rather than an implementation detail, because it is the kind of thing that
looks obvious in a design document and gets forgotten in a diff.

**Authentication is not authorization.** Every authenticated identity gets the
same namespace with the same rights; `uname` selects a key and then does
nothing else. Per-user views are a real and much larger question (9P's own
answer is per-fid ownership, which this server has no notion of), and this
phase does not open it. Stated here so nobody reads "multiple keys" as
"multiple permission levels".

**The nonce, and the part most likely to be got wrong.** A challenge-response
is only as good as its nonce: repeat one and a captured response replays
forever. RP2350's ROSC has a random-bit register, and it is the obvious
source — but "obvious source" is exactly the phrasing that precedes a bad
entropy bug, so **N1 measures it** (a bit-bias and run-length test over a few
hundred kilobits, reported by a shell command) rather than trusting it, and
mixes in the timer and a per-boot counter regardless.

**No crypto exists in this tree today**, which makes HMAC-SHA256 a real
milestone rather than a footnote — see N1.

## 7. Milestones

**N0 — Threat model and decision record.** §1 and §6 of this document,
reviewed and agreed before code. The one decision still genuinely open is the
key-distribution story if this ever grows past one gateway.

**N1 — SHA-256 + HMAC, and an entropy check.** Target-independent C, built on
every target, verified against the RFC 4231 test vectors by a
`hmacselftest` shell command wired into `tests/runner.py` — the same shape as
`dcf77selftest` and `clockuiselftest`, and for the same reason: the fiddly
part must not need hardware to debug. Plus `randtest`, reporting bias and run
lengths from the ROSC source. *Verify:* vectors pass on both QEMU targets;
`.bss` cost measured against the size baselines.

**N2 — The auth gate, firmware and host together.** `Tauth`/afid handling in
`fs/9p.c`, per-link policy, fail-closed-without-key, and the same exchange
implemented in `host/p9lib` and `host/fuse-p9`. Both halves in one milestone
deliberately: an auth mechanism half-deployed is an outage.
Also here, not later: **the server-side refusal to serve the keys file** (§6).
*Verify:* QEMU tests over the loopback and virtio links covering the happy
path, a wrong key, a replayed response, an attach with no prior `Tauth` on a
link that requires it, an unauthenticated attach on a link that does not, and
a walk/open of the keys path being refused. **Run over a TCP chardev too**
(§0), which is what makes `connect_tcp()` and the host half of the exchange
CI-covered rather than hardware-only. Existing 261 tests keep passing
untouched — that is the regression that matters, since every one of them
attaches without auth.

**N3 — The gateway persona.** Board file, preset, SD card on SPI1, size
baseline (`tools/sizereport-rp2350-gateway.json` — recorded *before* the
W5500 driver exists, so its cost is visible as a diff), boots to a shell over
USB with a mounted `/sd0` and nothing else attached. *Verify:* on hardware, `/proc/config` and `/proc/devices`
report what the board actually is.

**N4 — The W5500 driver.** SPI0 register access, reset and PHY bring-up, MAC
derived from the RP2350's unique flash ID, address from `(net-config ...)` on
the card with a board-file fallback and a loud unconfigured state (§3), socket
0 as a TCP server on port **564**, exposed as a `p9_link_t`. Plus `(net-status)`
and a `net` shell command. Polled `INTn` first; interrupt-driven only if measurement says it
is needed. A `/proc/net` reporting link state, IP, socket state and counters,
because "is it plugged in" must be answerable without a logic analyser.
*Verify:* on hardware — link LED, ping the gateway (the W5500 answers ICMP
itself), then a raw TCP connect from the host.
**A U-mode conversion of this driver is in scope**, following the pattern
phase 17b just paid for: its own `.w5500text` page, its own state region, and
grants for SPI0 + SIO. **And its ACCESSCTRL entries — SPI0 and the GPIO mask
— set from M-mode before the task exists.** Phase 17b lost an afternoon to
exactly that, on a peripheral (TIMER0) nobody had needed before; SPI0 will be
the same story if it is not written down here, which it now is.

**N5 — The downlink.** UART1 as a second instance, configurable baud, gateway
mounts the board at `/chess` (or `/clock`) at boot, host sees it through the
gateway's namespace. *Verify:* on hardware, with the board running `p9share`
so the console stays usable on the same wire; `cat` a file on the board's SD
card from the host, through two hops and one auth gate.

**N6 — Host tooling against real Ethernet.** `connect_tcp()` and the auth
exchange land in N2 (they are CI-testable there); what is left here is
everything that needs a wire: `fuse-p9` mounting the gateway over TCP, and
hardware tests in `tests/hw/`. *Verify:* `ls`, `cat`, `mkdir`, a write and a
re-read against the gateway's **own SD card**, then the same through the
gateway to the board's namespace, all over Ethernet, plus the failure modes a
localhost socket cannot produce — pull the cable mid-transfer, fill the
socket buffer, reconnect.

**N7 — Documentation.** README's persona list and a networking section; the
wiring diagram from §4; the auth setup, including how to generate and install
a key and what happens when you get it wrong.

## 8. Explicitly not in this phase

* **Wireless (RP2350W/CYW43), K210, ESP32-P4** — phase 14's 14e, later, on
  their own terms. The blob question alone deserves its own discussion.
* **A software TCP/IP stack.** See §0.
* **DHCP.** The W5500 has no hardware DHCP — it is a software protocol over a
  UDP socket, several hundred lines with lease renewal, and every one of its
  failure modes looks like "the board is not on the network". The gateway
  takes a static address (§3). A server wants a stable address anyway.
* **Encryption.** See §1.
* **Authorization.** Multiple keys identify *who*; they do not gate *what*.
  See §6.
* **More than one downlink.** See §4.
* **Auth on the chess/clock personas' own links.** The mechanism is shared
  code and they inherit it the moment a link asks for it; nothing in this
  phase turns it on for a cable.

## 9. Risks, and what each would look like

* **RAM on the gateway.** The W5500's 32 KB of buffering is *on the W5500*,
  which is most of why this part was chosen — but the link still needs an
  msize frame buffer, a U-mode driver needs a granted state region, and the SD
  card costs ~3.2 KB before any of that. None of it is large; all of it is on
  the same budget as the heap. Guard with the N3 baseline, recorded before the
  driver exists rather than after.
* **msize 2048 over a 100 Mbit wire.** Fine for correctness, poor for
  throughput: every read is a round trip. Raising it for the gateway persona
  alone is a one-line board-file change *if* the buffers it sizes are not on
  the same budget as something else — worth checking before promising it.
* **The 8-fid server table** (`P9_MAX_FIDS`) with a mount, a host and a FUSE
  client all holding fids at once. FUSE mounts are fid-hungry; `host/p9lib`'s
  own comment already records a session getting wedged by fid exhaustion after
  a timeout. This is the most likely place the phase runs into a wall that is
  not about networking at all.
* **Two hops of blocking I/O.** `p9_remote_*` waits forever by design
  (matching `virtio_blk`'s convention). Gateway → board over a cable that gets
  unplugged means a host request that never returns. A timeout policy for the
  *downlink* — not for local calls — is probably required, and would be the
  first place this design's "a mount is just a link" simplicity gets tested.
* **Entropy.** See §6. If the ROSC measurement comes back poor, the nonce
  needs another source and that is better known in N1 than in N4.


---

## N4 as it stands, 2026-08-24 — what works, and the one thing that does not

**Working and verified on hardware** (a bare Pico 2 + USR-ES1 module on an
isolated switch, gateway `192.168.77.2/24`, laptop `192.168.77.1/24`):

* **The bus, first try.** `VERSIONR 0x04` on the first flash -- wiring, power
  and ACCESSCTRL all correct, the last of those because phase 17b's lesson was
  applied in advance rather than rediscovered (SPI0 and its GPIOs get their
  Non-secure grants at init).
* **The PHY, after a real finding.** Auto-negotiation never links on this
  module and switch -- not in three seconds, not in forty, with the switch
  showing no light on that port. Forced 10BT half-duplex links in ~2.5 s.
  `net watch` ruled out the alternative first: 8 s with zero PHYCFGR changes
  and zero bad VERSIONR reads, so the chip is not browning out and reverting
  its own configuration. The driver now tries auto and falls back, and says
  which mode took. 10 Mbit is not a constraint here -- a 2 KB msize over SPI
  through a copying kernel is nowhere near it.
* **ICMP.** The W5500 answers ping itself: 3/3, 0.36-0.88 ms.
* **The socket.** LISTEN on 564, and `/proc/devices` lists `w5500net` as a
  p9link with `p9auth` reporting it **REQUIRED** while every cable link is
  not, and "Keys configured: NO" until one is -- the fail-closed state, on
  real hardware.
* **9P over Ethernet, including the gate.** `Tversion` negotiating msize 2048,
  an unauthenticated attach refused with *"attach: this link requires
  authentication (no afid)"*, and -- with a console key installed -- an
  authenticated attach followed by reading `/proc/version` over the wire. The
  N2 gate has now been exercised over a real network, which is what it was
  built for.

**Not working: the data path is not yet stable.** A session runs (one reached
61 frames in, 59 out) and then the chip stops answering ARP and ICMP while
continuing to report link UP, socket LISTEN and sane pointers over the same
SPI bus that is reporting it. Three things are known about it:

1. **SPI clock matters.** At 12.5 MHz -- the rate the SD card runs at on this
   same board -- the chip wedges quickly and no 9P session completes. At
   1.25 MHz a full session runs and ping still answers afterwards. That is
   consistent with signal integrity through 20 cm of jumper wire to a module
   with no series termination, and inconsistent with a pure logic bug.
2. **The read pointer runs away.** 21 KB read for 61 frames, and 13229
   single-byte resync discards: `Sn_RX_RSR` is at least sometimes read as
   more than actually arrived, after which `Sn_RX_RD` advances past the data
   and every subsequent read returns stale buffer. Now detected (RSR larger
   than the socket buffer closes the connection rather than believing it),
   which converts silent corruption into a dropped session -- but does not
   address why RSR was wrong.
3. **Access is serialised.** `p9srv` polls the link while the shell's `net`
   touches the same registers, and the kernel preempts at 100 Hz; the driver
   now holds a lock across whole operations, not single transfers, because
   the pointer arithmetic around `Sn_TX_WR` is only correct if nothing moves
   it in between. That fixed one class of failure (ping surviving the poller)
   and did not fix this one.

**The next experiment, and it is a bisection rather than another guess:** from
a cold boot, with the link registered but nothing else touching it, drive one
9P operation at a time and read the counters after each -- `net` now reports
frames in/out, resync discards, command timeouts and RX overruns, plus live
RSR/RD/TX_FSR, which is enough to see which operation first moves a pointer it
should not. If the trigger is a specific message rather than elapsed traffic,
that will show it in one run. If nothing is found there, the wiring itself is
next: shorter leads, and a scope on MISO at 12.5 MHz.

The driver is committed in this state deliberately. Everything above the data
path is proven, and the remaining fault is characterised well enough that the
next session starts from evidence rather than from the beginning.


### The bus is not the problem — measured, 2026-08-24

The wiring was the leading suspect: the chip worked and then stopped answering
ARP, and dropping SPI from 12.5 MHz to 1.25 MHz appeared to help. Before any
soldering iron came out, `net bustest` asked the bus directly -- no sockets,
no pointers, no networking:

```
  speed       read err   GAR(4,common)   PORT(2,sock)   SHAR(6,common)
  12.50 MHz          0               0              0              0
   6.25 MHz          0               0              0              0
   2.50 MHz          0               0              0              0
   1.25 MHz          0               0              0              0
```

20000 iterations per row. **The bus is clean at every speed this driver could
want.** Two conclusions, one of them uncomfortable:

* Jumper wiring is not the fault, and a soldered build -- worth doing for its
  own sake -- would not have fixed this.
* The 1.25 MHz "improvement" was a red herring. A slower clock changes the
  timing of a logic bug exactly as convincingly as it changes electrical
  margin, and the workaround was on its way to becoming a permanent constant
  with a plausible comment attached. It is reverted; the measurement is what
  should have come first.

Worth keeping: the first version of that test failed 100% at every speed,
because it used a closed socket's `Sn_DHAR` as scratch and a closed socket
does not keep it. A test that fails everywhere is testing itself.

### What the measurement then bought

With writes proven reliable, a chip that stops answering ARP is a chip that
was *told* to -- and `PHYCFGR`'s reset was being asserted **after** SHAR and
SIPR were written, in both `w5500_init()` and `w5500_set_address()`. On this
part that reset takes more with it than the PHY. Reordered so the PHY comes up
first, and the difference on one session is stark:

| | before | after |
|---|---|---|
| bytes in | 21415 | 2666 |
| frames | 61 in / 59 out | 28 in / 28 out |
| resync discards | 13229 | 1 |
| ping afterwards | dead | alive |

`rd16_stable()` also no longer returns a value it could not confirm: eight
disagreeing reads of `Sn_RX_RSR` now defer to the next poll rather than hand
back a torn number that would advance `Sn_RX_RD` past the data.

**Still open, and now intermittent rather than constant:** some runs come up
and serve, some come up reporting link UP, socket LISTEN, clean counters and
a bus that passes `bustest` -- and answer nothing. The suspect named here was
`w5500_set_address()`'s reset-and-reconfigure path. It has since been ruled
out; see below.


### Ruling out the driver — 2026-08-24, later

Three suspects were eliminated in one sitting, each by making the driver
report something it had only ever assumed.

**1. `net` was reading the driver's own variables, not the chip.** Every
"reports healthy" diagnosis in this phase was made against numbers that came
from C constants -- the port from `W5500_9P_PORT`, the address from `g_ip` --
which say what the driver *meant* to configure and cannot show a register that
silently did not take. `net` now reads `SIPR`, `Sn_MR`, `Sn_PORT` and both
buffer sizes back from the part and flags any disagreement. In the dead state
they all agree: SIPR 192.168.77.2, port 564, TCP, 8/8 KB. The configuration is
real.

**2. The reset was never verified.** `w5500_hw_reset()` pulsed RSTn and hoped.
It now checks (SIPR must read 0.0.0.0 afterwards) and falls back to the
datasheet's software reset, MR bit 7, which depends on no board wiring at all.
`net` reports which one took. In the dead state it reports **`RSTn pin`**: the
chip really is being fully reset on every `(net-config ...)`, and it comes up
dead anyway. So the bad state is not state the chip is holding, and it is not
anything this driver wrote.

**3. Reordering made it worse, not better.** Configuring the MAC with the link
down and bringing the PHY up last -- which reads like the more careful order --
measured 0/8 alive. Reverted. What that run *did* establish is more useful than
the ordering question: **the dead state persists across many consecutive
verified chip resets and then clears on its own** (3/5 alive in a later run on
unchanged firmware). Nothing inside the chip survives eight resets. Whatever is
sticky is outside it.

**Where that leaves it: the Ethernet side, not the SPI side.** `net watch 30`
in the dead state: zero PHYCFGR changes, zero bad VERSIONR reads -- the chip is
electrically steady, so this is not the digital core browning out. And the one
piece of physical evidence has been sitting in the notes since bring-up: the
**switch's port LEDs were dark** while the W5500 reported link UP.

That is explainable, and it is the thing to chase next. Auto-negotiation has
never linked on this module and switch; the driver falls back to forced 10BT
half-duplex, and **with auto-negotiation disabled a PHY reports link from
received energy alone.** PHYCFGR's link bit then reads UP whether or not the
far end ever agreed a mode. A switch port still auto-negotiating may
parallel-detect its way to 10BASE-T and carry traffic -- or may not, and the
register reads identically either way. That is exactly the observed pattern:
intermittent, indifferent to chip resets, invisible from inside. `net` now says
so in the forced-mode line rather than presenting the link bit as fact.

**The next experiment is physical, and it is one cable.** Connect the W5500
straight to the laptop's Ethernet port with no switch in between (any modern
NIC does auto-MDIX, so a patch cable is fine) and see whether
auto-negotiation links. If it does, the switch port is the fault. If it does
not, it is the cable or the module's magjack -- and only then is a soldered
build worth the effort, with the module's 3V3 supply and a bulk capacitor at
the module as the things worth doing differently. Note that "the bus is clean,
so soldering will not help" -- said earlier in this phase -- was about SPI, and
says nothing about the Ethernet side or the PHY's supply.


### The direct-connection experiment — 2026-08-24, and it exonerates the driver

The W5500 was connected straight to the laptop's NIC, no switch. Two things
came out of it, and the second one settles the phase.

**Auto-negotiation works, and the old timeout could never have seen it.**
`ethtool` on the laptop reports the link partner advertising 10baseT half and
full and 100baseT half and full, auto-negotiation **Yes**, settling on
**10 Mb/s full duplex**. That is the negotiation result read from the far end,
which is the only place it can be read honestly -- and it is a *better* link
than the forced 10BT half this driver had been falling back to. What it needs
is time: measured between 6 s and 25 s after a PHY reset, against three
seconds of patience. A gigabit NIC stepping all the way down to 10BT is in no
hurry. `w5500_phy_bring_up()` now takes a `patient` flag: 2.5 s at boot, where
nothing needs a cable to configure a MAC and a gateway must come up either
way, and 15 s for an explicit `(net-config ...)` or `net phy retry`, where the
user is asking for the link.

A correction to the entry above, which claimed forced mode "never carried a
single packet": it did. On the switch, forced 10BT half carried ICMP and
authenticated 9P sessions over TCP. The fallback is not a placebo and it
stays. What is true is narrower and still worth the warning `net` now prints:
with negotiation disabled a PHY reports link from received energy alone, so
PHYCFGR's link bit reads UP whether or not the far end agreed anything.

**And with all of that fixed, the module still passes no frames.** Final
state, every number verified rather than assumed:

| | |
|---|---|
| chip | VERSIONR 0x04, `net watch 40`: 0 PHYCFGR changes, 0 bad reads |
| reset | verified -- SIPR reads 0.0.0.0 after the RSTn pulse (`last reset: RSTn pin`) |
| registers, read back from the chip | SIPR 192.168.77.2, SHAR 02:4c:47:00:00:01, MR 0x00, Sn_MR 0x01 TCP, bufs 8/8 KB, port 564, LISTEN |
| link, both ends | UP, 10 Mb/s **full duplex**, auto-negotiated |
| laptop -> W5500 | ARP requests transmitted |
| W5500 -> laptop | **zero frames received. Not one.** |

Every register the driver writes has been read back out of the part and
matches. The reset is confirmed to happen. The link is genuinely negotiated in
both directions -- which means the W5500's transmitter works at least well
enough to send FLP bursts the laptop decodes. And no Ethernet frame ever
arrives.

There is nothing left in software to blame, and the sequence of wrong turns is
worth recording because each was a plausible reading of real evidence: the SPI
bus (`bustest`: clean at every clock rate), the socket buffer map, the read
pointer, `w5500_set_address()`'s reset path, the PHY/MAC ordering (measured
both ways), the remote mount, directory-sized replies. All eliminated, several
by making the driver report something it had only been assuming -- which is the
one durable lesson here. `net` reading the driver's own `g_ip` instead of the
chip's `SIPR` is what let "reports healthy" mean nothing for a whole phase.

**What is left is the module: its PHY, its magjack, or its supply.** The
digital core is demonstrably steady, so this is not the whole part browning
out -- but the PHY is the largest and burstiest current draw on that 3V3 rail,
and it is fed through jumper wire with no bulk capacitance at the module. An
analog side that misbehaves while the digital side stays happy is exactly the
split observed. In order of cost: a bulk capacitor (100 uF electrolytic plus
100 nF ceramic) right at the module's 3V3/GND pins; a different Ethernet
cable; a separate 3V3 supply for the module; a second W5500 module. The
soldered build is now worth doing -- and the thing to get right in it is the
module's power and ground return, not the SPI routing, which was measured
clean at 12.5 MHz.


### Resolved: it was the module's power — 2026-08-24

**220 uF across the power rail, and 100 nF directly at the W5500's Vcc-GND
pins.** That was the whole fault, and it invalidates most of what the two
sections above concluded about this hardware. The two capacitors do different
jobs and both are wanted: the bulk electrolytic holds the rail up through the
PHY's current bursts, and the ceramic, close enough to the pins to matter,
handles the fast edges the electrolytic is too slow for.

**Before and after, same firmware, same cable:**

| | before | after |
|---|---|---|
| negotiated link | 10BT, or nothing, or a forced link bit that lied | **100BASE-TX full duplex** |
| time to negotiate | 6-25 s, often never | **2.0 s** |
| magjack LEDs | red only; switch port dark | link and activity, lit and blinking |
| ping | 0/4 | 4/4, 0.31-0.67 ms |
| 9P over TCP | connection reset on any directory read | 25 frames in / 25 out, **0 resync discards, 0 timeouts** |

Every earlier belief about this PHY was a symptom of the supply: "this module
cannot auto-negotiate", "its 100BT path is unusable", "negotiation against a
gigabit NIC is just slow", and the long hunt for a fault downstream of a chip
that reported link UP, socket LISTEN, verified registers and a clean bus while
answering nothing. One cause, and not one of the guesses.

The tell, in hindsight, was the split that kept showing up in the
measurements: the digital core was always steady -- `net watch`, 40 s, zero
PHYCFGR changes and zero bad VERSIONR reads -- while the analog side
misbehaved. The core has decoupling on the module. The PHY is the largest and
burstiest draw on that rail and was fed through jumper wire with no bulk
capacitance near it. A supply that sags only under PHY activity looks exactly
like a chip that is fine and a network that is not, which is precisely how it
presented and precisely why it survived every software fix.

**For the soldered build:** these two capacitors are not optional, and they
are the most important components in the layout -- 220 uF on the rail, 100 nF
as close to the W5500's Vcc and GND as the board allows. The SPI routing
was never the problem -- `bustest` measured it clean at every clock rate.

**What was worth keeping from the wrong turns.** The instinct that produced
the fix was not a better guess; it was making the driver report facts instead
of intentions. `net` reading the driver's own `g_ip` rather than the chip's
`SIPR` is what let "reports healthy" mean nothing for a whole phase. Once
`net` read SIPR, SHAR, MR, Sn_MR, the buffer map and the port back out of the
part -- and once the reset verified itself -- software was exonerated with
evidence rather than argument, and the only place left to look was the one
place that turned out to be wrong.


## N6 as it stands — done, and it found the last real bug

`tests/hw/test_gateway.py`, the gateway persona's sibling to
`test_rp2350.py`: same conventions (each test returns `(name, ok, detail)`,
everything *skips* rather than fails when nothing answers), but nothing in it
can run on QEMU or over a localhost socket. **16 / 16 on hardware** (15 plus one that needs hands).

```
  [PASS] icmp / the chip's own stack -- rtt 0.197/0.393/0.590 ms
  [PASS] auth: unauthenticated attach refused
  [PASS] auth: wrong key refused
  [PASS] auth: correct key attaches
  [PASS] directory reads (multi-entry Rread) -- / has 6 entries, /proc 10
  [PASS] sd0: create, write, read back, remove
  [PASS] sd0: mkdir, populate, remove
  [PASS] multi-frame transfer (> msize, both directions) -- 8192 B each way, 62.6 KB/s
  [PASS] reconnect x5 (socket returns to LISTEN)
  [PASS] abrupt disconnect, then reconnect
  [PASS] pipelined requests fill the chip's TX buffer -- 12 sent, 12 correct, none lost
  [SKIP] cable pulled mid-session, then restored -- needs hands (--interactive)
  [PASS] two hops: /chess through the gateway
  [PASS] two hops: write to /chess/sd0 and read it back
  [PASS] lugal9pfuse over TCP (ls, cat, write) -- sd0 write round-trip OK
  [PASS] driver counters clean after the suite
```

The pipelined test is the one a localhost socket genuinely cannot stand in
for. The kernel's loopback buffer is large and elastic; the W5500's is 8 KB of
on-chip RAM with a hardware write pointer. Twelve requests sent with nothing
read drives `w5500_send_locked()` into its `free_space == 0` branch -- the
path that exists for "the peer has stopped reading" and that would corrupt the
stream rather than stall it if the pointer arithmetic were wrong. It also
makes the server field a pipelined peer, which its own comment calls legal
even though this client normally is not: several requests arrive in one
segment and have to be split by length prefix rather than by arrival.

The cable-pull test is opt-in behind `--interactive` because it needs hands.
Worth having as a test rather than a note: link loss is the one event where
the chip's state and the driver's diverge silently, and the recovery path is
new.

Two more things worth naming. The **write** direction had never been exercised
on this transport — everything before N6 read — and it works: files and now
directories on the gateway's own card, through FUSE, and **on the far board's
card through both hops**. That last one is the first operation in this tree
where a single Twrite crosses two transports and an auth gate, with the
gateway acting as 9P server to the host and 9P client to the board inside one
request. And the last test reads the driver's counters
after everything above: resync discards, command timeouts, RX overruns, all
zero. That is the test most likely to catch a regression, because every
transport fault in this phase appeared there before it appeared as a failed
operation.

### The bug it found: buffers reallocated under a running MAC

Writing the suite meant running `net phy` as an ordinary diagnostic, and that
exposed two faults that had been hiding behind the power problem.

**`net phy` could kill the link it was investigating.** A PHY reset puts the
chip back before "configure" in the datasheet's configure-then-open order, so
running `net phy auto` left a board with link UP, socket reporting LISTEN,
every register readable, and no ARP, no ICMP, no TCP — recoverable only by a
full `(net-config ...)`. A command whose purpose is to investigate a dead link
must not be able to create one. It re-applies the identity and re-opens the
socket now.

**And `(net-config ...)` was poisoning itself, in the way this driver had
already written down.** `w5500_socket_memory()` carries a note from earlier in
the phase: reallocating the socket buffer map underneath a running MAC takes
the whole networking block down, ICMP included. The bring-up was doing exactly
that — `hw_reset()`, then `phy_bring_up()` which waits for the link and starts
the MAC, and only *then* `socket_memory()`. That is why `net phy auto`, which
never touches the buffer map, reliably repaired what `(net-config ...)` had
just broken; the asymmetry was the clue. Moved to immediately after the reset,
while the chip is still quiet:

| | before | after |
|---|---|---|
| alive after `(net-config ...)` | intermittent | **6 / 6** |

A driver whose own comments record a hazard, at a call site that violates it,
is worth more attention than a new theory. This one was written down weeks
before it was tripped over.

**Also added:** the MAC is now re-configured on every link-up transition, not
only during bring-up (`net` reports the count). Negotiation is not a bounded
operation, so a link that settles after any fixed window used to leave a live
link and an unconfigured MAC. Now the link event drives it, which also covers
a cable being plugged back in.


### Still open after N6: a rare dead MAC while the link stays up

Not everything is fixed, and the remaining fault should not be buried under
fourteen passing tests.

Once during N6's development, after a clean full run -- 30 connections, 514
frames, zero resync discards, zero timeouts, zero overruns -- the board went
unreachable while idle. The laptop still saw a 100 Mb full-duplex link. The
board still reported link UP, socket LISTEN, and every register correct. No
link-up transition was counted, so from the chip's point of view nothing had
happened. `net phy auto` restored it in two seconds.

What is now known about it:

* It is **much rarer since the capacitors**, and rarer still since the buffer
  map moved out from under the running MAC. It has not recurred across two
  back-to-back full suite runs.
* **Load does not reproduce it.** The suite hammers the link -- pipelined
  requests, 8 KB transfers, abrupt drops, five reconnects -- and the board is
  reachable after every run.
* The one occurrence followed roughly ten minutes of **idle**, which is the
  next thing to characterise: leave the board untouched and poll, rather than
  looking for it under traffic where it has never appeared.
* It is now **cheap to recover from**: `net phy auto` re-applies the identity
  and re-opens the socket, so the repair is one command and two seconds
  rather than a full `(net-config ...)`.

Deliberately **not** fixed with a watchdog. The obvious one -- "link up, socket
LISTEN, nothing accepted for N minutes, therefore reset" -- describes a
perfectly healthy idle gateway exactly as well as it describes this fault, and
a driver that resets its own network because nobody called is worse than one
that occasionally needs a command. A watchdog is worth building when there is
a signal that distinguishes the two, and there is not one yet.


### N5 end to end, over Ethernet — verified

From the laptop, one authenticated TCP session to the gateway, reading both
boards:

```
authenticated to the gateway over Ethernet (192.168.77.2:564)
  ls /            -> ['flash0', 'sd0', 'proc', 'dev', 'srv', 'chess']
  ls /chess       -> ['flash0', 'sd0', 'proc', 'dev', 'srv']
  ls /chess/proc  -> ['ps', 'meminfo', 'version', 'df', ...]
  /proc/version        (gateway) -> LugalOS v0.13.1
  /chess/proc/version  (chess)   -> LugalOS v0.13.1
  /chess/proc/ps       (chess)   -> the chess board's own task table
```

And the proof that `/chess` is genuinely the other board rather than a loop
back through the gateway -- `/proc/config` from each, over the same
connection:

| | gateway | `/chess` |
|---|---|---|
| `ENABLE_CHESS` | 0 | **1** |
| W5500 pin config | present | absent |

Host -> Ethernet -> auth gate -> gateway -> UART1 -> chess board, one 9P
namespace, one `mount-remote` call, no proxy.


## N5 as it stands, 2026-08-24 — the two-hop namespace works

**Wiring** (crossed, plus a common ground):

```
gateway GP8  (UART1 TX) ---> chess GP1 (UART0 RX)
gateway GP9  (UART1 RX) <--- chess GP0 (UART0 TX)
gateway GND  ------------- chess GND
```

**On the chess board**, move the console off the wire the 9P server wants:

```
(console-bind "usb")     ; console to USB-CDC
klog detach console      ; kernel log off the UART too
p9serve                  ; UART0 becomes the 9P wire; does not return
```

**On the gateway**, one call:

```
lsh> (mount-remote "chess" "uart1")
=> #t
```

and the board is in the namespace:

```
lsh> ls /
chess       Remote 9P Namespace (uart1)   active

lsh> ls /chess
flash0  sd0  proc  dev  srv          <- the chess board's own mounts

lsh> cat /chess/proc/version
LugalOS v0.13.1 (Bare-Metal RISC-V Lisp Machine)
```

That is phase 5's design paying off: the gateway is a node whose namespace
happens to contain another board's, and mounting it took one call and no new
protocol. `p9_route_frame()`'s type-parity routing is what makes the 9P server
and the 9P client coexist on one task without a proxy.

**Over Ethernet, the host reaches the same namespace through the gateway** --
`Tauth`/`Tattach` with the pre-shared key, then `/proc/version` read over TCP,
all verified. What has *not* been demonstrated end to end is a file read on the
far side of both hops from the host, because directory reads over TCP hit the
N4 instability above before the walk completes. The hops are each proven; the
composition waits on N4's physical-layer fault.

**One wart, worth a line of code later:** `(mount-remote "chess" "uart1")` on a
name that is already mounted returns `#f` from `mount_alloc()` failing, which
reads as "the mount failed" when it means "it is already there". It cost this
session twenty minutes of chasing a mount that had in fact succeeded.
