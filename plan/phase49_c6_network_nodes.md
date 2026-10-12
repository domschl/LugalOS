# Phase 49 — The C6 as the P4's network node: one channel, frames and 9P

**Status: planned, not started. Written 2026-10-12; §1's verdict and all of
§2's decisions confirmed by the owner the same day. The NANO goes first.** Expands phase 45's
"Expansion" milestones 45.12–45.17 (`plan/phase45_esp32c6.md` §5–§7), which
were deliberately left as sketches until the stand-alone C6 worked. It works
(45.1–45.11 done: WPA2-PSK, DHCP, authenticated 9P over Wi-Fi, MQTT sensor
persona, watchdog, a 30-minute unattended soak). This phase turns the sketches
into a plan, and starts by answering a question phase 45 did not ask: **is 9P
alone enough as the coupling between the P4 and its C6?** (§1).

**Milestone scheme: `49.1`, `49.2`, …** Each ends with a repeatable
verification and a commit subject starting `49.N:`. Phase 45's numbers map as
in §9.

**Scope, in the order it will be built.**

1. **The analysis** (§1) and the decisions it leads to (§2).
2. **One channel protocol** — the *LugalOS link protocol* (LLP) — carrying
   Ethernet frames *and* 9P over whatever wire a pair of chips has, specified
   and tested without any C6 hardware first (49.1–49.2).
3. **The C6 as a NIC**, proven on the C6-Zero over a serial wire before any
   SDIO exists (49.3).
4. **SDIO on both sides**: the P4's SDIO host (slot 1), brought up first
   against the C6's *factory* ESP-Hosted firmware as a known-good reference;
   LugalOS on the C6-MINI; the C6's SDIO slave (49.4–49.6).
5. **Scenario 1 — the C6 provides networking** for the P4-NANO and the
   P4-LCD-7B (`wlan0` on the P4; 49.7).
6. **Scenario 2 — full 9P integration** between the two nodes on a board, and
   updating the C6 through it (49.8).
7. **The measurement that checks §1** (49.9), then the RP2350-terminal with a
   C6-Zero over UART (49.10).

---

## 1. Analysis: would 9P alone be enough?

### 1.1 What "the C6 provides networking" has to mean in this tree

Concretely, a networked P4 today (the NANO over its RMII Ethernet) does all of
this, and a P4 on Wi-Fi must too — the LCD-7B has *no* Ethernet, so for it the
C6 is the only network:

| # | Service | Direction | Code |
|---|---|---|---|
| a | Authenticated 9P server on TCP/564 (`fuse-p9`, `p9sync.py`, other nodes) | inbound | `net/tcp.c` listener → `fs/9p.c` |
| b | MQTT sensor publishing (the 7B carries a BME280; phase 26/46 persona) | outbound TCP | `net/mqtt.c`, `mqttd.c` over `tcp_stream_*` |
| c | NTP client (and the NTP server, `CONFIG_ENABLE_NTP_SERVER`) | outbound/inbound UDP | `net/ntp.c`, `ntp_server.c` |
| d | DHCP client, ARP, ICMP echo | link-local | `net/dhcp.c`, `arp.c`, `icmp.c` |
| e | 9P *client* to other nodes (`p9-remote-cat`, Lisp's `tcp_connect`) | outbound TCP | `user/lisp/lisp.c` |
| f | What comes next: AGENTS.md names WebSockets + JSON as the project's web architecture — an HTTP/WebSocket server on the P4, which has the RAM (32 MB PSRAM) for it | inbound TCP | not yet |

And "full 9P integration" means the two dies on one board see each other's
namespaces, and the LAN sees both.

### 1.2 The physical facts that bound every option

* **The only P4↔C6 path is SDIO** (phase 27 §7, confirmed from the schematic,
  same on the LCD-7B per phase 47): C6 IO18–23 ↔ P4 GPIO14–19, C6 `CHIP_PU` on
  P4 GPIO54; on the NANO also P4 GPIO6 ↔ C6 IO2 (free for a handshake). The C6's
  UART0 and USB pair go to a header, not to the P4. **9P needs a transport too:
  "9P only" still means an SDIO driver pair and a framing on it.** The choice
  is *what rides on the channel*, not whether there is one.
* **SDIO capacity is not the bottleneck.** C6 slave: SDIO 2.0, 4-bit, 0–50 MHz,
  DMA, 512 B blocks, an interrupt vector each way, shared registers (C6
  datasheet §4.2.1.8). The P4's slot 1 reaches GPIO14–19 only through the GPIO
  matrix (slot 0, the microSD, is IO_MUX-only: `drivers/sdmmc_esp32p4.c`), so
  plan for ~20–40 MHz: **≈ 10–20 MB/s raw.** Even 1-bit at 20 MHz is 2.5 MB/s.
* **LugalOS's TCP is the bottleneck, by two orders of magnitude.** Receive
  window = the 9P msize (`P9_MAX_MSIZE` 4096 off the RP2350; `net/tcp.c`
  `rcv_window()`), `TCP_MAX_CONNS 2`, no RTT estimator, and 9P is one request
  outstanding at a time. Measured: 62.6 KB/s 9P over the ENC28J60 (phase 18),
  ~20 KB/s through `fuse-p9` over the RP2350W's Wi-Fi (phase 19). Nothing in
  this phase is fast enough for the channel to matter; *where TCP runs* matters.
* **A Wi-Fi station has exactly one MAC.** In 802.11 three-address mode the AP
  accepts only frames whose source is the associated station's address; there
  is no bridging of a second MAC behind a STA (that needs four-address/WDS,
  which we do not do). Whatever runs IP for the P4 must use **the C6's station
  MAC**.
* **The C6 is small and tainted.** 512 KB SRAM; with radio + sensors + MQTT
  joined, **60 KB heap free** (45.11). It runs the Espressif blob and says so
  (`tainted: blob(espressif-wifi)`, phase 45 §2), whose stated intent is that
  "a peer may refuse to hand key material to a tainted node".
* **The 9P server answers synchronously.** `p9_server_process()` turns one
  request into one reply before taking the next (one `p9srv` task; the local
  channel endpoint is the same function). A request that would *wait* — a read
  on a socket's data file until bytes arrive — stalls every other client of that
  server. Deferred replies are not supported today.

### 1.3 The candidates

**A — 9P only.** The C6 is the board's IP node; the P4 has no IP stack on the
Wi-Fi path. Three sub-variants, increasing in what they deliver:

* **A1, namespace proxy (inbound only).** The C6 mounts the P4's namespace over
  the channel (`vfs_mount_remote()` on a channel-backed `p9_link_t`, exactly what
  A5/B1 already do for remote and local mounts) at `/p4`. A host attaches to the
  C6 and walks into `/p4/sd0/...`. Almost no new code.
* **A2, plus a socket filesystem on the C6** (`/net/tcp/clone`, `ctl`, `data`,
  `/net/udp`, Plan 9 style) so the P4's MQTT, NTP and 9P client (b, c, e) reach
  out through files.
* **A3, plus a stream relay**: the C6 splices TCP/564 byte-for-byte onto a 9P
  session on the channel, so the host authenticates to the *P4*, not to the C6.

**B — Ethernet frames only.** The C6 is a Wi-Fi NIC (phase 45's D2): it copies
frames between the blob's rings and the channel. The P4 runs DHCP, ARP, TCP,
9P, MQTT, NTP itself with the C6's MAC, as `wlan0` — a second `netif_t` beside
the NANO's `eth0` (`NETIF_MAX` is 2, phase 19: "the second slot is what makes a
gateway with both a wire and a radio expressible").

**C — frames for IP, 9P for everything else, on the same channel.** B's data
path, plus 9P messages as a second payload type: the P4 mounts the C6 at `/c6`
(radio status, scan, credentials, the C6's own `/proc`, its flash for updates),
and the C6 can mount the P4 if it needs to. Phase 45 §6's bespoke `CTRL`
command set is replaced by 9P files.

### 1.4 Comparison

| Criterion | A1 | A2 (+ socket FS) | B / C (frames) |
|---|---|---|---|
| (a) host → P4 files | yes, through the C6's 9P server and a second 9P hop | yes | yes, the P4's own server |
| (b) MQTT from the P4 | no | yes, new code on the C6 | yes, unchanged |
| (c) NTP, (d) DHCP/ping from the P4 | no (the C6 could export its time) | partly (UDP files) | yes, unchanged |
| (e) P4 as 9P client to peers | no | yes | yes, unchanged |
| (f) future web server on the P4 | no | only through a listen/accept file API | yes |
| Throughput today | ≈ same (TCP-bound), plus a store-and-forward 9P hop on a 160 MHz core | ≈ same, plus a copy through 9P per socket read/write | ≈ same, plus one frame copy |
| Throughput tomorrow | **capped by the C6's RAM**: window and connection count live in 60 KB | same cap | **set by the P4**: windows and msize can grow into PSRAM; the C6 forwards frames whatever the window |
| TCP connections | the C6's two, shared with its own | the C6's two, shared | the P4's own; the C6 needs none |
| C6 RAM | +2 msize buffers + fids | + per-socket buffers + server state; already the tightest budget in the tree | a few frame slots (the rings exist: `kernel/radio_netif_c6.c`); the C6's own TCP/MQTT can be dropped in the modem persona |
| New code | small | **large and awkward**: a writable socket FS *and* deferred replies in the 9P server (§1.2), on the smallest node | a frame bridge on the C6, a `netif` driver on the P4 — both seams exist |
| Who authenticates the host | **the tainted C6** (it must hold the peer keys, or the P4 must trust whatever the C6 forwards) | same | **the P4**; peer keys never leave it (A3 also fixes this, at the cost of a relay) |
| Node identity on the LAN | the C6; the P4 is a subtree | the C6 | the P4; the C6 is a subtree (`/c6`) |
| A C6 restart (`radio rejoin`, watchdog) | kills every session | kills every session | sessions are on the P4 and retransmit through a short outage (45.8's rejoin) |
| NANO with Ethernet *and* Wi-Fi | two stacks on two chips | same | one stack, two `netif`s |

### 1.5 Verdict

**9P alone is enough for scenario 2 (namespace integration) and for the inbound
file path, at about today's speed. It is not enough as the network path.**
Making it so (A2) means writing a socket filesystem and deferred 9P replies on
the chip with the least RAM, puts the tainted die in the authentication path,
gives the P4 no address of its own, ties every session's life to the C6, and
fixes the P4's network capacity at the C6's 60 KB forever — while the
alternative costs one frame copy, uses seams that exist on both sides
(`netif_t`, the frame rings), and spends < 1 % of the SDIO bus.

So: **C.** One channel, two payloads. Frames carry IP for the P4; 9P carries
control, status and the two namespaces. The 9P half is not an afterthought — it
replaces phase 45 §6's whole `CTRL`/`EVENT` design with files, and it is the
part that makes "the C6 is part of the P4" true. A1 comes for free with it (the
C6 mounting `/p4` is the same call in the other direction), which is what lets
49.9 *measure* the two paths side by side instead of trusting this table.

**What would reverse the verdict:** a measurement in 49.9 showing the frame path
far slower than the A1 proxy (e.g. U-mode radio ISR latency under bidirectional
load, phase 45 §9), or the C6 unable to sustain the frame rate. Then A1 for
files plus moving MQTT/NTP onto the C6 is the fallback — not A2.

---

## 2. Decisions (confirmed 2026-10-12)

| # | Question | Proposal |
|---|---|---|
| D1 | Network path | **Ethernet frames (C).** 9P rides on the same channel as a second payload type (§1.5). Supersedes phase 45 §8's "9P networking / `/net` — a later layer": `/net` as a socket FS is *not* built. |
| D2 | Control plane | **9P files on the C6, not a command set.** `/c6/wlan/{ctl,status,scan,mac}`, `/c6/proc/*`. The channel protocol keeps only what 9P cannot do: version/reset handshake, flow control, link state. |
| D3 | Address and MAC | **The P4 owns the board's IP address, using the C6's station MAC.** In the modem persona the C6 has no IP on Wi-Fi; it is reached as `/c6` through the P4. (Two IPs on one MAC with distinct DHCP client-ids is possible but depends on the router's lease keying; not attempted.) |
| D4 | One C6 image, two roles | **Persona chosen at run time:** a host's `HELLO` on the channel makes the C6 a NIC; no host within a few seconds of boot and it is phase 45's stand-alone node (own stack, own IP, own MQTT). Same kernel; presets differ only by board (§5.2). |
| D5 | Credentials | **The P4's identity record holds the WLAN SSID/PSK** (the `wlan` command, as on the C6-Zero and RP2350W), and writes the derived PSK to `/c6/wlan/ctl` at each link-up. The C6 holds it in RAM — it must, it runs the supplicant. **9P peer keys never reach the C6.** |
| D6 | Flashing the C6-MINI | **Once through its header** (user action, §5.3); after that **over 9P from the P4**, A/B slots, the stage 2 never rewritten remotely (49.8). |
| D7 | SDIO bring-up order | **Host first, against the factory ESP-Hosted slave** still on the C6 (phase 47 D6 left the 7B's untouched): a known-good peer isolates host-driver bugs from slave-driver bugs (the "trace the reference first" rule). One board keeps its factory C6 until our slave enumerates. |
| D8 | ESP-Hosted compatibility | **Still no** (phase 45 D3). It is a reference for 49.4 only. |
| D9 | Name | The protocol is **LLP** (LugalOS link protocol), not "modem protocol": it links two LugalOS nodes, one of which happens to have a radio. |

---

## 3. The channel protocol (LLP v1) — what 49.1 specifies

Properties carried from phase 45 §6, revised by §1:

* **Channel-agnostic framing, channel-specific flow control.**
  A frame is `u8 type, u8 flags, u16 len, payload` (little-endian, as 9P).
  Types:

  | Type | Payload | Notes |
  |---|---|---|
  | `LINK` | `HELLO {version, role, mtu, max_frames, mac}`, `RESET`, `CREDIT n`, `STATE {link_up, rssi}` | link management only |
  | `ETH` | one Ethernet frame, ≤ 1514 B, no FCS (`NETIF_FRAME_MAX`) | the data path |
  | `P9` | one 9P message, T or R, either direction | demuxed by `fs/p9_link.c`'s existing T/R parity rule |

* **SDIO** (P4 ↔ C6-MINI): the slave's hardware does the flow control —
  receive-buffer tokens (the host may write only as many packets as the C6 has
  loaded buffers), a packet-length register for host reads, an 8-bit interrupt
  vector each way, and shared registers that carry link state without a frame.
  Several LLP frames are packed per CMD53 transfer, 4-byte aligned, padded to
  the block (512 B); the bus CRC covers integrity.
* **UART** (C6-Zero ↔ RP2350 or a PC): SLIP framing (`drivers/uart_net.c`'s
  `slip_feed()`, as `p9share` and the UART1 downlink use) plus a CRC-16 per
  frame, and explicit `CREDIT` frames.
* **Versioned and resettable.** `HELLO` carries a major/minor; a major mismatch
  is refused loudly, on both sides, with both versions printed (the two dies are
  separate builds and will be flashed apart). Either side may send `RESET`; the
  P4 can also hard-reset the C6 (GPIO54), and the C6 must treat a host that
  vanished mid-transfer as normal.
* **Priorities:** `LINK` and `P9` ahead of `ETH` in each direction, so a full
  frame queue cannot starve control.
* **Not in v1:** multiple logical 9P sessions per channel (one is enough: a
  board has one P4 and one C6), compression, encryption (the wire is a PCB
  trace).

## 4. Shape of the code

```
 P4 (untainted)                                   C6 (tainted)
 ───────────────                                  ───────────────
 net/ (DHCP,TCP,9P,MQTT,NTP)                      radio domain (U-mode, blob)
   │ netif_t "wlan0"                                 │ RX/TX frame rings (45.7)
 drivers/llp_netif.c ─┐                            ┌─ kernel/llp_bridge.c   (modem persona: rings ⇄ ETH frames)
 fs/p9_link (/c6) ────┤                            ├─ fs/p9_link (/p4 optional, 49.9)
                      ▼                            ▼
              kernel/llp.c  (framing, HELLO, credits, priorities — shared, host-tested)
                      │                            │
 drivers/sdio_host_esp32p4.c (slot 1)   drivers/sdio_slave_esp32c6.c
                      └──────── SDIO 4-bit, GPIO14–19 ─┘
```

* `kernel/llp.c` is target-independent and built everywhere; its UART and
  virtio-console bindings make it testable in QEMU and on the host
  (`tests/host/llp_host.c` under ASan/UBSan, as `kobj_host` and friends).
* The P4's SD/MMC controller has two slots and one command path: the microSD
  driver becomes a *slot client* of a shared controller with a lock, rather
  than a second driver poking the same registers.
* On the C6, the SDIO slave is a bus master the APM **can** confine (unlike the
  Wi-Fi MAC, phase 45 §4.5): its DMA is fenced to the channel's buffers, so a
  host bug cannot scribble on the C6's kernel.

## 5. Board facts to establish (not yet known)

### 5.1 Already known

* NANO and LCD-7B: C6-MINI-1(-N4) on SDIO GPIO14–19, `CHIP_PU` on GPIO54
  (phases 27, 47). NANO: GPIO6 ↔ C6 IO2. Both C6s run factory ESP-Hosted today.
* The C6-MINI-1-N4 has **4 MB flash**, not the Zero's 8 MB: the C6 flash map
  (`cmake/flash_layout_esp32c6.cmake`, identity at `0x7F0000`) does not fit
  as-is.

* **NANO header P2** (read from a rendering of the schematic, 2026-10-12; this
  corrects phase 27 §7's "pins 19 and 20"): C6_U0RXD on **20**, C6_U0TXD on
  **22**, C6_IO9 (the boot strap) on **24**, C6_IO12/IO13 — the C6's **USB D−/D+**
  — on **21/23**, P4 GPIO54 = C6_CHIP_PU (via R54 0 Ω) on **13**, P4 GPIO6 = C6_IO2
  on 12; GND on 3, 4, 7, 10, 18, 25, 26. On the module: EN has 10 kΩ to 3V3 and
  1 µF to GND (R13/C6); IO8 has 10 kΩ to 3V3 (R25), so the second strap is
  already right for download mode.
* **LCD-7B:** the C6's UART0 and IO9 are on header **H4 "C6-UART"**; CHIP_PU from
  P4 GPIO54 (R33 0 Ω), C6_IO2 from P4 GPIO6 (R31) (`cmake/board-esp32p4-lcd7b.cmake`).
  Whether H4 also carries the USB pair: to read.
* **Nothing in the kernel drives GPIO54 today** (no reference in `drivers/`,
  `kernel/`, `arch/`), so the pin floats as an input and the module's pull-up
  holds the C6 running. From 49.4 the P4 drives it **open-drain only** (pull low
  or release): an adapter or a finger on the same net can then never fight a
  pin driven high.

### 5.2 To read or measure in 49.5

* **Is the C6's USB-Serial/JTAG reachable through P2 21/23** (wiring B in
  §5.3), with the factory firmware and with ours? If yes, the C6-MINI has the
  C6-Zero's console, `c6run.py`/`c6flash.py` work unchanged, and download mode
  needs no IO9 wire (the USB-JTAG peripheral resets the chip into it).
* **Can the P4 alone ever put the C6 into download mode?** It drives `CHIP_PU`
  but not IO9 (strapping: IO9 low = joint download, which includes *SDIO
  download boot*, C6 datasheet Table 3-3). It cannot: a broken C6 stage 2 needs
  the header again, which is why D6 never rewrites stage 2 remotely.
* **Flash map for 4 MB:** stage 2 128 KB, two OS slots sized from the measured
  XIP image (text + rodata + the blob's ~374 KB), identity at the top, `/flash0`
  shrunk or dropped on this board. Board file `esp32c6-mini` (4 MB, console on
  whatever 5.2 finds, no WS2812, no I2C sensors, SDIO slave on).
* **Cold vs warm.** `CHIP_PU` from the P4 is a chip reset; PMU/LP registers
  survive it (AGENTS.md §3.3). Only a board power cycle tests a cold C6.

### 5.3 Wiring for flashing the NANO's C6 (the NANO goes first)

Header P2, 2×13 at 2.54 mm, odd pins on one row:

```
            P2
           ┌──────┐
  VCC_5V  1│ o  o │ 2  ESP_LDO_VO4
     GND  3│ o  o │ 4  GND
 ESP_3V3  5│ o  o │ 6  GPIO0
     GND  7│ o  o │ 8  GPIO1
   GPIO3  9│ o  o │10  GND
   GPIO2 11│ o  o │12  GPIO6  ── C6_IO2
  GPIO54 13│ o  o │14  GPIO53         GPIO54 ══ C6_CHIP_PU (EN)
  GPIO47 15│ o  o │16  GPIO48
  GPIO46 17│ o  o │18  GND
  GPIO45 19│ o  o │20  C6_U0RXD  ◄── adapter TXD
 C6_IO12 21│ o  o │22  C6_U0TXD  ──► adapter RXD
 C6_IO13 23│ o  o │24  C6_IO9    (boot strap: low at reset = download)
     GND 25│ o  o │26  GND       ──── adapter GND
           └──────┘
 C6_IO12 = C6 USB D−, C6_IO13 = C6 USB D+
```

**Wiring A — the UART adapter (connected 2026-10-12; enough for the first
flash and the factory backup).** 3.3 V logic, adapter VCC left unconnected
(the board is powered over its own USB).

| Adapter | P2 pin | Net |
|---|---|---|
| TXD | 20 | C6_U0RXD |
| RXD | 22 | C6_U0TXD |
| GND | 26 | GND |
| — (jumper wire, only while entering download mode) | 24 ↔ 26 | C6_IO9 → GND |
| — (touch briefly to GND, e.g. pin 25, to reset) | 13 | GPIO54 / C6_CHIP_PU |

To enter download mode: jumper 24↔26 in, touch 13 to GND for a moment (or
power-cycle the board), and the ROM waits at 115200 for esptool. Take the
jumper out and reset again to run what is in flash. Without the jumper, a reset
prints the factory firmware's boot log on this adapter at 115200, which checks
the C6→adapter direction before esptool is involved. On this host the adapter
is the **second CP2102 and has the same USB serial number (`0001`) as the
NANO's console CP2102**, so their `/dev/serial/by-id` links collide: address
both by `/dev/serial/by-path/` (hub port 4.2 = the P4 console, answers `lsh>`
at 921600; hub port 4.4 = the C6's UART0).

**Wiring B — the C6's own USB (recommended for the 49.5–49.8 development
loop).** A USB cable cut open, or a USB-A breakout:

| USB wire | P2 pin | Net |
|---|---|---|
| D− (white) | 21 | C6_IO12 |
| D+ (green) | 23 | C6_IO13 |
| GND (black) | 25 | GND |
| VBUS (red) | — | not connected, insulated |

Keep the wires under ~15 cm and twist D+/D−: this is 12 Mbit/s full-speed USB.
The C6 then appears as `usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00`,
the serial number is the MAC so it cannot collide with the C6-Zero's, and
`tools/c6run.py` / `c6flash.py` reset it into download mode by themselves:
no IO9 jumper and no reset wire. Wiring A stays useful alongside it, as a second
console (UART0) and as the serial wire for LLP reference tests.

**Wiring C (optional, if the adapter has a DTR pin):** adapter DTR → P2 24
(IO9). The host then sets the strap, and the reset comes from the P4 (GPIO54,
open-drain, a `c6 reset` shell command in 49.4), so the UART path is automatic
without USB.

**Order of work on the NANO:**
1. Wiring A. With the jumper in and a reset, `esptool --chip esp32c6 --port
   <by-path 4.4> --before no_reset chip_id`: both directions, MAC, flash size
   (expect 4 MB).
2. Back up the factory flash: `read_flash 0 0x400000` into
   `~/Source/gith/esp/p4nano-c6-factory-flash/`, next to the P4's
   `p4nano-factory-flash/`. Phase 45 §7.4 says no backup is *required*. It is
   still worth a minute, because D7 uses the factory slave as the SDIO reference
   and the backup lets the NANO return to it at any time.
3. Remove the jumper, reset, check the factory boot log. **49.4 runs against
   this factory C6.** Only then 49.5 writes LugalOS (wiring B if it works,
   otherwise A).

**Done 2026-10-12, steps 1 and 2 (wiring A):**
* The ROM banner: `ESP-ROM:esp32c6-20220919`, `rst:0x1 (POWERON),boot:0x4
  (DOWNLOAD(USB/UART0/SDIO_FEI_FEO))`, `waiting for download`, so USB, UART0
  and SDIO download are all enabled on this part. `chip-id`: **ESP32-C6FH4
  (QFN32) rev v0.2, 4 MB embedded flash**, 40 MHz crystal, base MAC
  `58:e6:c5:d8:d4:3c`.
* **Factory flash backed up** to `~/Source/gith/esp/p4nano-c6-factory-flash/`
  (README there: sha256, a restore command). It is ESP-Hosted-MCU
  `network_adapter` `release/ng-v1.0.2-330-g83efce6` (built 2024-10-18) in
  `ota_0`, with `ota_1` erased. The boot loader's and the app's own checksum and
  SHA-256 validate offline (`esptool image-info`). An on-chip `verify-flash`
  could not reconnect, because the stub stays at the read's 460800 baud and a
  new connection starts at 115200: verify in the same esptool call, or after a
  reset.
* What cost time: TX/RX were crossed the wrong way round (the first attempts
  were silent in both directions). The board was reset by a power cycle of the
  NANO's own USB. **A reset through pin 13 (GPIO54 → R54 → EN) is still
  unproven**, so confirm it before 49.4 relies on it.

---

## 6. Milestones

### Protocol, without hardware

* **49.1 LLP v1: spec and reference.** §3 written out as `doc/llp.md` (frame
  layouts, state machines for `HELLO`/`RESET`/credits, versioning rules);
  `kernel/llp.c` with a host harness (`tests/host/llp_host.c`, ASan/UBSan:
  framing, SLIP + CRC, credit exhaustion and recovery, a `RESET` mid-frame, a
  version mismatch refused, frames interleaved with 9P under priority) —
  mutation-tested like the other host harnesses; and a Python reference
  (`host/llp/`, `uv`, `pyrefly`-clean) that speaks LLP over a serial port.
  *Done when:* the harness and the Python reference agree on a shared vector
  file of frames, both directions.

* **49.2 Two QEMU nodes, one channel.** A "modem" node (rv32, virtio-net as its
  stand-in radio) and a "host" node (no NIC) joined by virtio-console carrying
  LLP: `kernel/llp_bridge.c` on the first, `drivers/llp_netif.c` (`wlan0`) on the
  second. The host node gets a DHCP lease from QEMU's user network *through* the
  modem node, answers 9P on 564 from the outside, and mounts the modem at `/c6`.
  Runs in `tests/runner.py`. *Done when:* the multi-node test passes 20 runs in a
  row (the phase-5 lesson: one-in-three races only show up on repetition), and
  the modem node holds no TCP state for the host's traffic.

### The C6 as a NIC, on the C6-Zero

* **49.3 C6-Zero modem persona over serial** (= 45.13). Persona selection (D4),
  `llp_bridge.c` on the real rings, `/c6/wlan/*` files, the STA MAC handed over in
  `HELLO`. First wire: the C6-Zero's USB-Serial/JTAG with the console and LLP
  demultiplexed (as `p9share` does), the Python reference as the host — it takes
  a DHCP lease with the C6's MAC and pings the gateway; with privileges, an
  optional TAP bridge gives the PC a `wlan` interface through the C6. *Done
  when:* `test_esp32c6.py` gains a modem check (lease + ping through the bridge),
  the stand-alone checks still pass with no host attached, and the C6's heap in
  the modem persona is recorded against the 60 KB of 45.11.

### SDIO

* **49.4 P4 SDIO host, against the factory slave** (= 45.14). Slot 1 on
  GPIO14–19 via the GPIO matrix, the controller shared with the microSD slot
  (§4), GPIO54 reset (open-drain, §5.1; `c6 reset`). NANO first, its C6
  still factory and backed up (§5.3). CMD0/CMD5/CMD3/CMD7, CCCR/FBR/CIS through CMD52, 4-bit
  bus, CMD53 byte and block mode, the card interrupt on DAT1. Reference: IDF's
  ESP-Hosted host built for the P4 (toolchain in `~/.espressif`), register and
  bus traces diffed against ours. `sdio` shell command: card info, CIS, a raw
  CMD53 read of the slave's registers. *Done when:* the factory C6 enumerates
  from cold and warm reset on both boards, the microSD still works with both
  slots in use (`sdbench` unchanged), and CMD53 throughput at the highest stable
  clock is recorded.

* **49.5 LugalOS on the C6-MINI.** The board facts of §5.2; the
  `esp32c6-mini` preset and 4 MB flash map; first flash through header P2
  (§5.3: wiring B with `c6flash.py` if the USB pair works, else wiring A with
  the IO9 jumper). Two small pieces of kernel work come with it. First, the C6 kernel's only console
  is the USB-Serial/JTAG (`drivers/usbjtag_esp32c6.c`): with wiring A alone,
  a UART0 console driver (GPIO16/17; probably the P4's `uart_esp32p4.c` IP) is
  needed first. Second, `reboot download` sets `LP_AON_FORCE_DOWNLOAD_BOOT`
  (`LP_AON_SYS_CFG_REG` bit 30) and resets, so a running C6 enters the ROM's
  download mode without the IO9 jumper, which is then only for recovery.
  The radio, the join and
  the stand-alone persona as on the C6-Zero, with the module's antenna. *Done
  when:* `test_esp32c6.py` passes on the NANO's C6 (its non-applicable checks —
  LED, I2C sensors — skipping), and the LCD-7B's C6 is still factory.

* **49.6 C6 SDIO slave + LLP over SDIO** (= 45.15). Packet mode, DMA
  descriptors, receive tokens as LLP's credits, interrupts both ways, link state
  in a shared register; the APM fence on the slave's DMA (§4). A loopback mode
  (the C6 echoes `ETH` frames) for `sdio bench` from the P4. *Done when:* echo
  runs 10 minutes both ways with no lost or corrupted frame, a `RESET` and a
  GPIO54 hard reset mid-stream both recover without a P4 reboot, and throughput
  is ≥ 5 MB/s each way (two orders above what the stack needs; the margin is the
  point).

### Scenario 1 — networking for the P4 boards

* **49.7 `wlan0` on the P4** (= 45.16). `llp_netif.c` over SDIO; credentials
  from the P4's identity record (D5); DHCP with the C6's MAC (D3); on the NANO,
  `eth0` and `wlan0` side by side with a stated rule for which carries the
  default route. Everything in §1.1 (a)–(e) over Wi-Fi: authenticated 9P from the
  host, `fuse-p9`, MQTT from the 7B's BME280, NTP. **`p9sync.py` to the LCD-7B
  over Wi-Fi** replaces the console `p9share` path for `/sd0` updates. *Done
  when:* `test_esp32p4.py` gains Wi-Fi checks (join, lease, ping, authenticated
  9P read, MQTT message seen by the broker) passing on both boards, the 30-minute
  soak of 45.11 repeated on the 7B (no rejoin, no reset), and a `radio rejoin` on
  the C6 does not drop an open 9P session on the P4.

### Scenario 2 — full 9P integration

* **49.8 `/c6` in the P4's namespace, and updates through it.** The C6's
  namespace mounted at `/c6` over the channel's `P9` type: `/c6/wlan/*`,
  `/c6/proc/*` (its `taint` line included), `/c6/flash/os-a|os-b`. The LAN sees
  `/c6` through the P4's server (the host authenticates to the P4; the C6 trusts
  the channel as a cable, `P9_AUTH_NOT_REQUIRED`, which is the existing policy
  for attached links). **C6 update:** the P4 writes an image into the inactive
  slot, the C6 checks its SHA-256 (`kernel/sha256.c`) before marking it, the P4
  resets it via GPIO54, and the C6's stage 2 falls back to the old slot if the
  new one does not `HELLO` within N seconds. A host tool (`tools/c6update.py`, or
  a `--c6` mode of `p9sync.py`) wraps it. *Done when:* a C6 image goes from the
  build directory to the NANO's C6 over Wi-Fi-then-SDIO with no header attached,
  and a deliberately broken image is rolled back by itself.

### Checking the verdict

* **49.9 Frames versus 9P-only, measured.** On one board: the C6 mounts the
  P4 at `/p4` (A1 — the same `vfs_mount_remote()` in the other direction), so a
  host can reach the same file both ways — through `wlan0` and the P4's own
  server, and through the C6's server and the channel. Measure file read/write
  throughput and round-trip latency for both, the C6's heap and CPU load in each,
  and session survival across a C6 restart. Record in §8. *Done when:* the
  numbers are in this plan and §1.5 is either confirmed or rewritten.

### The RP2350 terminal

* **49.10 C6-Zero as a Wi-Fi add-on for the RP2350-terminal** (= 45.17). LLP
  over SLIP + CRC on the RP2350's UART1 (`drivers/uart1_link_rp2350.c`'s pins,
  today 115200 baud). Bandwidth first: the highest baud both PL011 and the C6's
  UART hold without loss over the actual wiring. The RP2350's stack and its 2 KB
  msize stay as they are; `wlan0` reuses `llp_netif.c`. *Done when:*
  `test_rp2350.py` gains a Wi-Fi check through the C6-Zero, and the terminal's
  suite and `lcdterm_shots.py` are unchanged.

## 7. Risks

| Risk | Why | What we do |
|---|---|---|
| **Slot 1 over the GPIO matrix is slow or marginal** | Slot 0 is IO_MUX-only; the matrix adds delay at 40 MHz | Start at 400 kHz → 20 MHz, step up, sample-edge tuning; the need is < 1 MB/s, so a modest clock is acceptable |
| **One controller, two slots** | The microSD and the C6 share the command path and DMA | Controller + slot clients with a lock (§4); `sdbench` before/after as the regression |
| **U-mode radio ISR latency under load** | Phase 45 §9: the radio's interrupts go through a kernel stub and a U-mode thread; a NIC moves more frames than a sensor node | Measured in 49.7/49.9; fallback is M-mode handlers for that path only (phase 45's stated fallback) |
| **C6 RAM in the modem persona** | Rings, SDIO buffers, the 9P server, the radio's 64 KB heap | The modem persona drops the C6's own TCP/MQTT/NTP load; measured in 49.3 |
| **First flash needs the header** | The P4 does not drive IO9 | One user action per board (49.5); never rewrite stage 2 remotely (D6) |
| **Version skew between the dies** | Separate builds, flashed apart | `HELLO` major/minor, loud refusal (§3); 49.8 makes updating the C6 a routine step |
| **Warm-reset hides cold-boot bugs** | `CHIP_PU` keeps PMU/LP state (phase 45 §4.9) | Every hardware milestone ends with a board power cycle (ask the user) |
| **The factory C6 is needed as a reference and is expendable** | D7 vs phase 45 §7.4 | Keep the 7B's C6 factory until 49.6 passes; no backup is required |
| **STA MAC ownership** | Only one MAC on Wi-Fi (§1.2) | D3; the P4 learns the MAC from `HELLO` and refuses `wlan0` without it |
| **Two CP2102s with the same serial number** | The NANO's console and the C6's UART adapter both report `0001`, so `/dev/serial/by-id` (and `~/.config/lugalos/p4-ports.env`'s NANO entry) can point at either | Address both by `/dev/serial/by-path/` (§5.3); prefer wiring B for the C6, whose serial is its MAC |

## 8. Measurements (filled in as milestones finish)

| What | Value | Milestone |
|---|---|---|
| CMD53 throughput, factory slave | — | 49.4 |
| LLP loopback over SDIO, each way | — | 49.6 |
| C6 heap free, modem persona | — | 49.3 / 49.7 |
| 9P file read host → P4: via `wlan0` / via C6 `/p4` proxy | — / — | 49.9 |
| Session survives `radio rejoin`: frames / proxy | — / — | 49.9 |
| UART1 LLP throughput, RP2350 ↔ C6-Zero | — | 49.10 |

## 9. Mapping from phase 45

| Phase 45 | Here | Change |
|---|---|---|
| 45.12 modem protocol spec + Python reference | 49.1 (+ 49.2 in QEMU) | `CTRL`/`EVENT` replaced by 9P files and `LINK` frames (D2) |
| 45.13 C6 as a modem over UART | 49.3 | the C6-Zero's USB console first; UART1 to the RP2350 in 49.10 |
| 45.14 P4 SDIO host | 49.4 | against the factory slave first (D7) |
| 45.15 C6 SDIO slave | 49.5 (C6-MINI bring-up, new) + 49.6 | |
| 45.16 P4-NANO networked through its C6 | 49.7 (both P4 boards) + 49.8 (9P integration, C6 updates) | |
| 45.17 C6-Zero for the RP2350-terminal | 49.10 | |
| — | 49.9 | new: the measurement behind §1 |

## 10. Explicitly not in this phase

* A socket filesystem (`/net`) on any node (§1.5).
* IP forwarding or NAT between `eth0` and `wlan0` on the NANO.
* ESP-Hosted compatibility (D8); returning a C6 to factory firmware.
* BLE, 802.15.4, SoftAP, WPA3, low-power modes (phase 45 §8 stands).
* The P4's USB-Serial/JTAG as a 9P link (deferred in 47.13; 49.7 makes it less
  needed still).
* TLS.

## 11. References

* `plan/phase45_esp32c6.md` — the C6 node, the radio domain, frame rings (§4.3–§4.9), the sketches this phase replaces (§5–§7)
* `plan/phase27_esp32p4_bringup.md` §7 — the P4↔C6 wiring; `plan/phase47_esp32p4_lcd7b_ribbon.md` §9 — the 7B's C6
* `plan/phase5_distributed_design.md` Rule 2 ("9P *is* the service IPC"), A5 mounts; `fs/p9_link.c` (T/R demux), `fs/vfs_server.c` (`vfs_mount_remote()`)
* `net/include/net/netif.h`, `kernel/radio_netif_c6.c`, `drivers/cyw43_rp2350.c` — the `netif` seam and its templates
* `drivers/sdmmc_esp32p4.c` — the P4's SD/MMC controller (slot 0)
* C6 datasheet §3.1 (boot modes), §4.2.1.8 and TRM chapter "SDIO Slave Controller"; P4 TRM ch. 57 (SD/MMC host): `~/Source/gith/esp/datasheet/`
* IDF: `components/sdmmc/sdmmc_io.c` (host CMD52/53), `components/esp_driver_sdio/` (slave); ESP-Hosted as a bus reference only
