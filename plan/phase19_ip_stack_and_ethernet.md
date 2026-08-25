# Phase 19 — An IP stack of our own, and two wires to carry it

**Status: R0 done 2026-08-25; R1 in progress.** Succeeds
`plan/phase18_networking_and_auth.md`,
which is concluded: everything it built *above* a byte stream is kept, and the
one thing below the stream -- the W5500 -- is cancelled and removed here (R0).

**Scope.** Write the network layer this project has so far avoided: ARP, IPv4,
ICMP, UDP and a server-side TCP, sized for the RP2350 and tested on QEMU
before any hardware exists. Then give it two frame sources: an **ENC28J60**
(SPI, MAC-only, documented, ordered) and the **CYW43439** on an RP2350W
(wireless, blob, already on hand). Phase 18's unfinished N7 (documentation)
lands here.

**Out of scope, explicitly:** the ESP32-P4 port (phase 20), the K210, IPv6,
DHCP, encryption, and a full Plan 9 `/net` socket filesystem. See §7.

---

## 0. Why the virtio-net question flipped

Phase 18 §0 refused to prototype 9P-over-IP against QEMU's virtio-net, and the
reason was specific and good:

> The W5500 contains the TCP/IP stack in silicon. Going virtio-net first would
> mean writing a software TCP/IP stack *specifically to have something to
> prototype against* -- on the one target whose RAM budget is enforced at link
> time -- to exercise a layer the real hardware will never run.

Every word of that depended on one premise: that the target terminates TCP
itself. That premise is gone with the part. **Every candidate that remains --
ENC28J60, CYW43439, the ESP32-P4's on-die EMAC -- hands over Ethernet frames
and nothing more.** A software stack is therefore no longer a scaffold; it is
the product, and virtio-net is not one abstraction level below the real
hardware but exactly level with it.

So the conclusion inverts while the reasoning stands, which is the good kind of
plan change: nothing written in phase 18 has to be called wrong for this phase
to be right.

One consequence is worth stating early, because it is what makes the schedule
tolerable: **the stack is the part of this work that does not depend on any
hardware.** It can be written, tested and RAM-budgeted in CI on both QEMU
targets while parts are in the post, and every wire that arrives afterwards is
a driver rather than a project.

## 1. The blob question, settled once so it stops being a fork in the road

The argument that started this phase was: *the W5500 is also a blob -- it holds
a closed TCP/IP implementation -- so the difference between it and the CYW43439
is merely the size of the blob.* That is half right, and the half that is wrong
is the half that decides the roadmap.

| | W5500 (cancelled) | CYW43439 (RP2350W) | ENC28J60 / ESP32-P4 EMAC |
|---|---|---|---|
| Where the closed code runs | its own die | its own die | nowhere |
| Who ships the firmware | WIZnet, once, in the package | **we do** -- ~230 KB in our flash, uploaded by us at boot | -- |
| In our build and our licence story | no | **yes** | no |
| What it hands us | a **TCP byte stream** | **Ethernet frames** | **Ethernet frames** |
| What is left for us to implement | nothing | the entire IP stack | the entire IP stack |

Two things fall out of that table.

**The distinction that matters is not blob size, it is what is left to
implement.** A part whose closed firmware ends at the MAC layer leaves the
network to us; a part whose closed firmware ends at TCP does not. That is the
axis this project's design ideal actually lives on -- "bare metal" here has
always meant *we wrote the thing*, not *no silicon anywhere contains
microcode*. Every chip in the tree has a mask ROM, RP2350 included.

**The CYW43439 is not a shortcut, it is strictly more work than the W5500 was.**
It is a *fullmac* part: the blob does 802.11 association and hands us Ethernet
frames, so choosing it means blob **and** a software IP stack. Anyone reaching
for a Pico 2 W expecting the W5500's "TCP arrives on SPI" bargain will not get
it. This is the single most useful fact in this section.

And once the stack is ours, the blob question stops being a fork in the road
and becomes a per-part footnote: each wire is a `netif` under the same stack,
and the honest thing to do is *label* each one (§5's blob accounting) rather
than let the choice of part decide whether the project has a network.

### Reverse-engineering the CYW43439: no, and the reason is not effort

The undocumented part is not the MAC -- joining an open AP is a few hundred
lines of well-published 802.11 state machine. It is the **PHY**: radio
register maps, calibration tables, and the regulatory-limit data the CLM blob
carries. Broadcom/Infineon has never documented it, no project has produced
open firmware for a Broadcom fullmac part, and the closest thing that exists in
the world -- `open-ath9k-htc-firmware` for the AR9271 -- is open only because
Atheros published enough to make it possible, and still needs a full *softmac*
802.11 stack on the host side plus USB host on ours. It is an order of
magnitude beyond writing TCP, for a worse result. Closed.

### So the ideal survives, stated plainly

After this phase the sentence the project can honestly say is: **LugalOS
implements its own network stack, and speaks it over a wire whose driver it
also wrote.** On the ENC28J60 that is true down to the magnetics. On the
RP2350W it is true down to the 802.11 MAC, with the radio behind a blob that is
named, sized and licensed in the README. Both are worth having; only one of
them needs an asterisk, and it gets one.

## 2. What gets built, and how small it has to be

**In:**

* **Ethernet II framing**, one MAC address per netif, broadcast + own-address
  filtering (in the MAC where the part can do it, in software where it cannot).
* **ARP** -- request, reply, a fixed 8-entry cache with timeouts, and gratuitous
  ARP on address change. No proxy ARP.
* **IPv4** -- unicast send/receive, header checksum, TTL. **No fragmentation
  and no reassembly**: an incoming fragmented datagram is counted and dropped,
  and we never emit one (MSS is clamped so we do not have to).
* **ICMP** -- echo request/reply, and destination-unreachable *emitted* for a
  closed port. `ping` answering is the first thing anyone tries and it is worth
  the fifty lines.
* **UDP** -- send and receive on a bound port. Small, and it is what a later NTP
  client (R6) or a sensor persona needs.
* **TCP, server side** -- passive open, one listener, up to **two** simultaneous
  connections, a fixed receive window of one MSS, retransmission with
  exponential backoff, correct FIN and RST handling. Active open (`connect()`)
  is R3b and may slip without blocking anything.

**Out, deliberately** -- each of these is a decision, not an omission: IPv6,
DHCP (phase 18 §8 argued it away and the argument is unchanged: a server wants
a stable address, and a client's failure modes all present as "the board is not
on the network"), TCP options beyond MSS, window scaling, SACK, Nagle, delayed
ACK, out-of-order segment reassembly, routing or forwarding between netifs,
TLS.

**Out-of-order is the important one.** A segment that arrives out of order is
dropped and not acknowledged; the peer retransmits. This costs throughput on a
lossy link and saves a reassembly queue, which is both the largest RAM line item
and the largest bug surface in a small TCP. On a LAN with a 2 KB msize it is the
right trade. Written down here so that a future throughput complaint is
recognised as this decision rather than as a mystery.

### The RAM budget, decided now rather than discovered at link time

The gateway persona is an RP2350 with the budget enforced at link time
(`plan/phase15_memory_reclamation.md`), so the stack gets a number before it
gets code:

| | budget |
|---|---|
| stack state (ARP cache, netif, socket table) | ≤ 2 KB `.bss` |
| per-netif RX buffer | 1 × 1518 B |
| per-netif TX buffer | 1 × 1518 B |
| per-TCP-connection buffers (2 conns) | 2 × (1 MSS in + 1 MSS out) ≈ 4 × 1460 B |
| **total** | **≤ 12 KB** |

That is roughly four RP2350 pages, against a persona that has no framebuffer,
no chess engine and no font tables. **R2 reports the real number** and the
number is a milestone exit criterion, not a footnote -- discovering it at R4 on
hardware is how phase 15's lessons get re-learned the expensive way.

## 3. The two seams, and why phase 18's work survives its hardware

```
   9P server  (fs/9p.c, unchanged)
        |
   p9_link_t  <-- an accepted TCP connection IS one of these
        |            (auth_required already lives here -- phase 18 N2)
   net/tcp.c, net/ip.c, net/arp.c, net/udp.c, net/icmp.c   <-- THIS PHASE
        |
   netif_t    <-- frame in, frame out, MAC, link state
        |
   drivers/virtio_net.c | drivers/enc28j60.c | drivers/cyw43.c
```

**Below: `netif_t`.** Deliberately the same shape as `p9_link_t`
(`fs/include/fs/p9_link.h`): a name, a non-blocking `poll()`, `send_frame()`,
`recv_frame()`, an opaque `ctx`, plus a MAC address and a link-up flag. The
project already has this idiom and every reviewer already knows how to read it.
The only new obligation is that a netif carries *frames*, not 9P messages.

**Above: an accepted TCP connection is a `p9_link_t`.** This is what makes
phase 18 a sunk cost of days rather than of weeks: the auth gate, the
`auth_required` flag, `p9_link_register_background()`, the `dev_*` registry
entry and the whole host side (`connect_tcp()`, `Session(key=...)`, `fuse-p9
--tcp`) plug into the new stack unchanged. `dev_w5500net` becomes `dev_tcpnet`
and the rest of `kernel/board.c` does not notice.

**Where the stack runs: as a task, in its own domain.** Every driver in this
tree is a U-mode task confined by PMP or Sv39 (phase 12 M5), and a network
stack -- the single largest attack surface the system will ever have, parsing
attacker-controlled bytes -- is the last place to make an exception. It gets its
own domain and talks to the 9P server over the existing channel IPC.

*The risk in that, named up front:* a copy-always IPC boundary on the per-frame
path could cost more than it is worth. **R2 measures it.** If a frame round trip
through channels is unaffordable, the fallback is a stack task that owns the
sockets and hands the 9P server a *stream* (which is exactly what `p9_link_t`
already is) rather than individual frames -- keeping the isolation and moving
the boundary up one layer. That fallback is the expected outcome and the design
should not fight it.

**Configuration is phase 18's, unchanged.** `(net-config "192.168.1.50"
"255.255.255.0" "192.168.1.1")` in `/sd0/system/etc/usr_init.lisp`, the MAC
derived from RP2350's unique ID, and an unconfigured link that stays down and
says so loudly rather than guessing an address (phase 18 §3). The Lisp
primitives keep their names and point at the new stack.

### The Plan 9 answer, and why only half of it lands here

Plan 9 exposes the network as files -- `/net/tcp/0/ctl`, `data`, `listen` -- and
for a 9P-native system this is obviously the right long-run shape: it would make
sockets reachable from Lisp, from the shell, and from a host over the existing
9P mount, with no new protocol.

It does **not** land in this phase, and the reason is sequencing rather than
doubt: writing a TCP state machine and designing a filesystem interface to it at
the same time means every bug is ambiguous between the two. What lands here is
**read-only status as files** -- `/net/ipifc/0/status`, per-netif and per-socket
counters -- which is enough to debug the stack with `cat` and is a strict subset
of the eventual interface. Promoting `/net` to a writable socket interface is a
later phase, with a user of its own to justify it.

## 4. Testing: two QEMU backends doing two different jobs

**(a) A raw-frame peer, for the protocol.** `-netdev socket,udp=...` (or
`-netdev dgram`) hands raw Ethernet frames to a plain host UDP socket. A ~200
line Python peer in `tests/` on the other end of it can craft *anything*: an ARP
probe, a truncated IP header, a bad checksum, a SYN followed by an immediate
RST, a segment out of order, a retransmission, a window-filling burst. No root,
no tap device, no bridge, and every failure is reproducible byte for byte.
This is the testbed a hand-written TCP needs and it is the main reason this
phase is affordable.

**(b) Slirp with a forwarded port, for the whole path.** `-netdev
user,hostfwd=tcp::PORT-:564` gives the guest a virtual LAN (the fixed
10.0.2.15/24 with a 10.0.2.2 gateway -- static, which suits the no-DHCP rule)
and gives the host a real TCP port into it. `tests/runner.py` already drives a
TCP-chardev 9P session with auth (its N2 test); the change is only that the
guest now speaks TCP itself instead of receiving a stream someone else
terminated. `lugal9p` and `fuse-p9 --tcp` then run against QEMU exactly as they
run against hardware.

**What QEMU still cannot test, and hardware must:** link up and down, a cable
pulled mid-transfer, PHY negotiation, half-duplex collisions, real frame loss,
and the ENC28J60's own errata. That is the same split phase 17 used for DCF-77
and phase 18 used for the W5500, and it is still the honest boundary.

**One acceptance criterion applies to every netif, and it is a scar:** after the
functional tests pass, the link is left **idle for fifteen minutes** and must
still work. Phase 18's W5500 passed 16/16 tests and then died at 180 seconds.
No wire is called done in this project again without sitting still first.

## 5. The parts

### ENC28J60 -- the first wire (ordered, ~3 days)

Microchip, SPI, 10BASE-T half duplex, 8 KB internal buffer, MAC + PHY, **no
TCP anywhere in it**. Slower than the W5500 by a factor of ten on paper and
irrelevant in practice: at a 2 KB msize the binding constraint is the SPI bus
and the 9P round trip, not the wire.

Its reputation is for errata, and that is *the point of choosing it*: the
ENC28J60's problems are **published by the manufacturer**, in a numbered errata
sheet, with workarounds. The W5500 clones' problem was undocumented, unowned
and unfixable. Known-and-documented beats unknown-and-cheap, and the ones to
design in from the first commit rather than rediscover are the receive-buffer
wrap address rule, the transmit-logic hang that wants a per-packet TX reset, and
the fact that the part has no auto-negotiation, no full duplex and no auto-MDIX
(so: a straight cable to a switch, and the duplex fixed on both ends or left
half).

**It drops onto the gateway persona's existing pin map.** SPI0 on GP16-19 with
reset on GP20 and interrupt on GP21 -- the same pins, the same board file, the
same "**not** GP16 for the heartbeat" note. The rename is `CONFIG_W5500_*` to
`CONFIG_ETH_*`, and nothing about the persona changes.

**Power and decoupling, before the first byte is flashed.** The ENC28J60 pulls
~180 mA with the link up, which is the same order as the W5500 and lands in the
same 500 mA budget with the same modest margin. Phase 18's most expensive lesson
was that the right decoupling (**220 µF bulk plus 100 nF at the chip's own
Vcc-GND pins**) was written into the plan and then not fitted for days. **It gets
fitted first this time**, along with short SPI leads and a ground return
alongside them. If the boards that arrive carry their own regulator and bulk
capacitance, note it and keep the 100 nF anyway.

### CYW43439 on the RP2350W -- the second wire (on hand)

A gSPI link, ~230 KB of firmware streamed from flash at boot, an ioctl-shaped
control protocol, and a join to an AP. Worth having: the parts are already in
the drawer, and a wireless sensor persona is a real future application in a way
that a second Ethernet part is not. One pleasant surprise: **WPA2 is free**,
because the join ioctl takes the passphrase and the firmware does the four-way
handshake and the crypto -- so "open network only" is a limitation this phase
does not have to accept.

**On the proposed ordering, and a recommendation to change it.** The suggestion
was to build CYW43 as a *stop-gap* while the ENC28J60 boards are in the post.
Two reasons not to:

1. **It is not a three-day job.** gSPI bring-up, firmware upload, and an ioctl
   protocol against an undocumented-but-reverse-engineered interface is the
   larger of the two drivers, not the smaller. It would still be in progress
   when the parts arrive.
2. **It is the worst possible first consumer of a brand-new IP stack.** Bringing
   up an untested stack over an untested wireless driver means every failure has
   two candidate homes. The ENC28J60 -- a wire, a documented register map, no
   firmware, no association state -- is the frame source that makes "is the
   stack right?" a question with a clean answer.

**So: ENC28J60 first (R4), CYW43 second (R5)**, where it is a driver problem
only and a bug is unambiguously in the driver. The phase keeps it either way;
this is a sequencing change, not a scope cut.

**And a trigger, so a delivery cannot stall the phase:** if the ENC28J60 boards
are late or dead on arrival, **R4 and R5 swap** and the CYW43 becomes the first
netif. The stack (R1-R3) is done by then regardless, which is the whole point of
building it first.

**Blob accounting, in the README, not in a footnote.** Where the firmware comes
from, what it is licensed under and whether we may redistribute it, how large it
is, and the plain statement that it is the only non-source artifact in the
image. A project that makes a point of being bare metal owes its readers that
paragraph, and owes it in the same place it makes the claim.

## 6. Milestones

**R0 -- Remove the W5500.** Delete `drivers/w5500_rp2350.c`,
`drivers/include/drivers/w5500.h`, the `net *` shell commands in `kernel/shell.c`
(`report`, `phy`, `bustest`, `txtest`, `rxtest`, `debug`, `watch`), the
`w5500_init()` call in `kernel/main.c`, `dev_w5500net` in `kernel/board.c`, the
`CONFIG_W5500_*` block in `cmake/board-rp2350-gateway.cmake` and
`cmake/gen_config.cmake`, the source entry in `CMakeLists.txt`, the `/proc`
lines in `fs/vfs_server.c`, the W5500 entry in
`tools/sizereport-rp2350-gateway.json`, and the Ethernet-dependent tests in
`tests/hw/test_gateway.py`. **Keep** everything phase 18's STATUS section lists
as kept -- the auth gate, the persona, the downlink, the host side. The Lisp
`(net-config)`/`(net-status)` primitives keep their names and are stubbed to
report "no interface" until R2 wires them to the stack.
*Verify:* all five presets build (`rv32-nommu`, `rv64-mmu`, `rp2350-chess`,
`rp2350-clock`, `rp2350-gateway`); QEMU suite green at its full count; `tests/hw/` green minus the removed
Ethernet tests, with the removals listed in the commit message rather than
silently dropped.

**Done, 2026-08-25** (`125ad96`). Five presets build, QEMU suite **269/269**
including the N2 auth gate over TCP on both targets, gateway static RAM
**-2152 bytes** and its heap 93 -> 94 pages with the baseline regenerated. Two
deviations from the list above, both deliberate and both argued in the commit:
`tests/hw/test_gateway.py` is **kept** (only `test_driver_counters` was
actually about the W5500; the other fifteen tests are what R4 has to make green
again, and rewriting them would be waste), and `(net-config)`/`(net-status)`
are kept **unguarded** rather than RP2350-only, because R1 and R2 need them on
the QEMU targets too.

**And a standing rule adopted here** (`f422a80`), because it changes how every
later milestone is finished: **a clean build produces no output.** Ten
pre-existing compiler diagnostics and one linker diagnostic were fixed rather
than inherited -- most were typing noise, one was real (a DCF-77 selftest
printing an uninitialised date on failure), and one was a genuinely wrong image
(an RWX `PT_LOAD` on the clock persona, from `.binary_info` being emitted
writable and folded in with the driver text pages). A warning nobody acts on
teaches the build to ignore its own diagnostics; every milestone below counts
as finished only when its targets still build silently.

**R1 -- `netif_t` and the virtio-net driver.** The seam plus the first
implementation of it: virtio-mmio device, two virtqueues, borrowing the ring
handling that `drivers/virtio_blk.c` and `drivers/virtio_console.c` already
prove. Plus the raw-frame Python peer from §4(a).
*Verify:* on both QEMU targets, the guest emits a frame the Python peer receives
byte-exact, and receives one the peer sends; counters in `/net/ipifc/0/status`
agree with what the peer counted.

**R2 -- ARP, IPv4, ICMP, UDP, and the RAM number.** The stack task, its domain,
its `/net` status files, `(net-config)` wired through.
*Verify:* the peer's ARP request is answered and its cache entry ages out; a
crafted ping is echoed; a UDP datagram round-trips; a fragmented datagram is
counted and dropped rather than mishandled; a truncated header does not fault.
**And:** the measured `.bss` and heap cost against §2's 12 KB budget, plus the
per-frame IPC cost against §3's fallback -- both reported in the milestone, not
deferred.

**R3 -- TCP, passive open, and 9P over it.** The state machine, retransmission,
FIN and RST.
*Verify:* the peer drives handshake, in-order data, out-of-order data (dropped,
retransmitted, accepted), a peer RST mid-stream, a window fill, and a half-close.
Then the end-to-end path over slirp: `lugal9p` and `fuse-p9 --tcp` complete a
full authenticated 9P session against the guest, on both targets, in CI.
*(R3b, may slip: active open, so a node can mount a peer over TCP.)*

**R4 -- ENC28J60 on the gateway persona.** Driver as a netif, the errata
workarounds of §5 designed in, the decoupling fitted before power-on.
*Verify:* a LAN host pings the gateway; `tests/hw/test_gateway.py` restored to
green over real Ethernet, including the cable-pull and buffer-fill cases R0
removed; then **fifteen minutes idle** and still working.

**R5 -- CYW43439 on an RP2350W.** gSPI, firmware upload, join (WPA2), netif.
*Verify:* joins a real AP, answers a ping, completes an authenticated 9P session
over TCP, survives the same fifteen-minute idle soak. Plus the blob accounting
paragraph in the README.

**R6 -- optional: an NTP client.** UDP is already there from R2; NTP is ~150
lines and it is the first thing that makes the network useful to a *persona*
rather than to a host -- the phase 17 clock could set its own time. Marked
optional so it cannot delay R7.

**R7 -- Documentation** (inherits phase 18's N7). README's implementation-status
section and a networking section that says what the stack does and does not do
(§2's "out" list, verbatim -- an unstated limit gets credited as a feature); the
gateway wiring; the auth setup, key generation and what a wrong key looks like;
and the blob paragraph from R5.

## 7. Explicitly not in this phase

* **The ESP32-P4 port** -- phase 20. It is the blob-free-est destination
  available (Ethernet MAC on-die, external PHY over MDIO, full public TRM,
  RISC-V) and precisely for that reason it is a *port*, not a networking
  feature: ROM boot image format, flash, PSRAM, caches, a new interrupt
  controller. Sequenced after this phase so that when the port lands, the
  network it needs is already written and tested. Note that the board's *Wi-Fi*
  is a separate ESP32-C6 over SDIO, which is blob-shaped again -- the P4 is only
  blob-free over the wire.
* **The K210** -- unscheduled. It has no networking at all, which is what has
  kept it at the bottom of the list since phase 14.
* **Reverse-engineering the CYW43439** -- see §1.
* **A writable `/net` socket filesystem** -- see §3. Status files only.
* **IPv6, DHCP, TLS, routing, fragmentation, SACK/window scaling** -- see §2.
* **Confidentiality on the wire.** Phase 18 §1's threat model is inherited
  unchanged: auth proves who attaches, and does not hide what they then read.

## 8. Risks, and what each would look like

* **A hand-written TCP is the largest correctness surface in this project's
  history.** It parses attacker-controlled bytes, it has a state machine with
  eleven states, and its bugs present as "the transfer hangs sometimes". The
  mitigations are all structural and all in the plan already: the packet-level
  Python peer (§4a), one MSS of window, no reassembly, two connections, and the
  stack in its own memory domain so a bug in it is contained rather than
  systemic.
* **The RAM budget is missed.** Looks like: the gateway persona fails to link,
  or `palloc`'s free-page baseline collapses. Caught at R2 by making the number
  an exit criterion; the release valve is one connection instead of two.
* **The per-frame IPC cost is unaffordable.** Looks like: throughput an order of
  magnitude below the wire. Caught at R2, and §3 already names the fallback.
* **The ENC28J60 boards are late, dead, or erratic.** Looks like phase 18 again.
  Mitigated by §5's swap trigger (CYW43 becomes the first netif) and by the fact
  that R1-R3 need no hardware at all.
* **The CYW43 driver stalls on an undocumented control protocol.** Looks like:
  the firmware uploads and the join never completes. It is R5, after everything
  that matters is already working over a wire, so it can be timeboxed and
  deferred to a later phase without taking the network with it.
* **Scope creep toward "a real TCP".** Looks like: someone adds SACK. §2's
  "out" list exists to be pointed at.
