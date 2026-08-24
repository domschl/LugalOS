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
| `UART1 TX/RX` | 4 / 5 | the downlink to a board — see §4 |
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
   UART1 TX  GP4  ───────────────────►  UART0 RX  GP1
   UART1 RX  GP5  ◄───────────────────  UART0 TX  GP0
   GND            ─────────────────────  GND
```

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

**Two things the downlink needs that do not exist yet**, both small and both
named here so they are not discovered later:

1. **A second UART instance.** `drivers/uart_rp2350.c` is UART0-only —
   `CONFIG_UART0_BASE` and friends are singular, and the driver's task,
   channel endpoint and demux all assume one instance. The gateway needs UART0
   (its own console) *and* UART1 (the downlink) at once.
2. **A configurable baud rate.** The divisors are hardcoded for 115200
   (`uart_rp2350.c`, "Configure Baud Rate for 150MHz clk_peri"). The downlink
   should run much faster — 1 Mbaud is comfortable for a 15 cm cable between
   two boards on a desk — so the divisor becomes a per-board config value with
   the 115200 console as its default.

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
