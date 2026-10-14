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
| D6 | Flashing the C6-MINI | **Once through its header** (user action, §5.3); after that **over 9P from the P4**, A/B slots, the stage 2 never rewritten remotely (49.8). *Amended 2026-10-14:* the ROM-loaded image is the kernel's RAM half, not a loader; "never rewritten remotely" applies to a new permanent loader at `0x0`, installed once through the header, and each slot carries both halves (49.8b). |
| D7 | SDIO bring-up order | **Host first, against the factory ESP-Hosted slave** still on the C6 (phase 47 D6 left the 7B's untouched): a known-good peer isolates host-driver bugs from slave-driver bugs (the "trace the reference first" rule). One board keeps its factory C6 until our slave enumerates. |
| D8 | ESP-Hosted compatibility | **Still no** (phase 45 D3). It is a reference for 49.4 only. |
| D9 | Name | The protocol is **LLP** (LugalOS link protocol), not "modem protocol": it links two LugalOS nodes, one of which happens to have a radio. |

---

## 3. The channel protocol (LLP v1)

**Specified in `doc/llp.md` (49.1); that document is authoritative.** In
brief:

* **Frame:** `u8 type, u8 seq, u16 len (LE), payload`. Types: `LINK` (link
  management, never flow-controlled), `ETH` (one Ethernet frame, 14–1514 B, no
  FCS), `P9` (one 9P message, T or R, either direction, demuxed by
  `fs/p9_link.c`'s T/R parity rule). Payload ceiling 4096; each side announces
  its own, at least 1514.
* **LINK operations:** `HELLO` (version, role, flags, session ids, max payload,
  window, MAC), `CREDIT` (ack, window), `PROBE` (next seq), `STATE` (Wi-Fi link,
  RSSI).
* **Sessions instead of a RESET message:** each start picks a random session
  id. A peer whose id changes is a restarted peer, and everything about the old
  session is dropped in both directions. A major-version mismatch parks the link
  in MISMATCH, loudly, on both sides, until the peer restarts.
* **Flow control per channel:** SDIO has it in hardware. A UART gets LLP
  credits (cumulative `ack` + `window` ≤ 64), with `PROBE` so a lost data frame
  cannot hold its credit forever. A binding may send LINK frames ahead of queued
  data, **except PROBE**, which must not overtake data sent before it.
* **Liveness:** a keepalive after 1 s of silence, and DOWN after 3 s without a
  frame. A peer that was only cut off comes back with its session intact.
* **Serial binding:** `SLIP(header | payload | CRC-32 LE)`, the zlib/Ethernet
  CRC (not the CRC-16 sketched here before; the tree already had CRC-32).
* **SDIO binding:** frames packed per CMD53, 4-byte aligned, no LLP CRC. Fixed
  in 49.6.
* **Priorities** (LINK and P9 ahead of ETH) are a binding's queueing policy,
  not part of `llp.c`. They arrive with the first binding that has queues
  (49.2).
* **Not in v1:** several 9P sessions per channel, compression, encryption
  (the wire is a PCB trace).

## 4. Shape of the code

```
 P4 (untainted)                                   C6 (tainted)
 ───────────────                                  ───────────────
 net/ (DHCP,TCP,9P,MQTT,NTP)                      radio domain (U-mode, blob)
   │ netif_t "wlan0"                                 │ RX/TX frame rings (45.7)
 kernel/llp_node.c ───┐                            ┌─ kernel/llp_node.c     (radio role: interface ⇄ ETH frames)
 fs/p9_link (/c6) ────┤                            ├─ fs/p9_link (/p4 optional, 49.9)
                      ▼                            ▼
              kernel/llp.c  (framing, HELLO, credits, PROBE — shared, host-tested)
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
  NANO's own USB.
* **Step 3, and the pin-13 reset, confirmed:** with the jumper out, touching
  P2 13 to GND resets the C6 (`rst:0x1 (POWERON)` — EN low is a power-up to this
  chip), so **R54 is fitted** and GPIO54 can reset the C6 in 49.4. The factory
  log, as a reference for 49.4: IDF `v5.4-dev-3602-ga97a7b0962`, flash DIO
  80 MHz; `ESP-Hosted-MCU Slave FW version :: 0.0.6`, `Transport used :: SDIO
  only`, WLAN + BLE-HCI over SDIO; `SDIO_SLAVE: sending mode:
  SDIO_SLAVE_SEND_STREAM`, `RxQ[20] timing[0]` (stream mode for slave→host, 20
  receive buffers, default sampling edge). BT MAC = base + 2 (`…d4:3e`). On its
  first boot after the backup it rewrote its PHY calibration ("checksum
  failure"), so the backup's `nvs`/`phy_init` no longer match the chip
  byte for byte. That is harmless: both are regenerated.
* **Wiring B works, with the factory firmware running** (a cut USB cable on
  P2 21/23/25, 2026-10-12): the C6 enumerates as `303a:1001`,
  `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_58:E6:C5:D8:D4:3C-if00`
  (serial = the base MAC). Over it, esptool's default reset puts the C6 into download
  mode with no jumper and no pin 13 (`chip-id`, `USB mode: USB-Serial/JTAG`), and
  `--after hard-reset` returns it to the factory app. Over the same link the
  backup's boot loader + partition table (0x0–0x9000) and `ota_0` were checked
  on the chip: **`verify-flash` digests match**. So 49.5 can use `c6run.py` /
  `c6flash.py` and the existing USB console as on the C6-Zero. With the C6-Zero
  also attached, `c6run.py` refuses to guess and wants `--port` (or
  `LUGALOS_C6_PORT`). A `--board zero|nano` mapping in the style of
  `p4-ports.env` is a convenience for 49.5.

---

## 6. Milestones

### Protocol, without hardware

* **49.1 LLP v1: spec and reference — DONE 2026-10-12.** `doc/llp.md` (the
  specification); `kernel/llp.c` + `kernel/include/kernel/llp.h` (framing, the
  serial binding, the HELLO/credit/PROBE/keepalive state machine; no
  allocation, no clock, no I/O; 5.8 KB of text on rv32, built for every
  target); `host/llp/` (the Python reference, a `uv` project, `pyrefly`-clean,
  5 tests including a lossy-line soak); `tests/host/llp_vectors.txt`
  (generated by the reference); `tests/host/llp_host.c` (replays the vectors,
  fuzzes, soaks); and a runner check that the vectors are current.
  *Done when* is met: C and Python agree on 9 frame vectors, 8 receiver
  streams and 11 two-link scenarios, every emitted byte, event and delivery.
  * **The soak** (two links over a simulated 3 Mbit/s UART, bit flips, lost
    bytes, a 5 s outage, data both ways): every delivered frame intact, in
    order, never duplicated; the receive buffer never overflowed; both sides
    noticed the outage and came back by themselves. 2000 fuzzed streams under
    ASan/UBSan, nothing found.
  * **Mutation-tested:** six deliberate bugs, all caught. They were: credits
    ignoring the receiver's free space, a HELLO always answered, PROBE not
    skipping lost frames, stale frames accepted, the old SLIP overflow, and a
    CRC missing a byte. The stale-frame mutation was missed at first (a serial
    line never duplicates), which is why the `duplicates` scenario exists.
  * **Found on the way:** `slip_feed()` (now `drivers/slip.c`) handed up the
    *tail* of an oversized frame as a frame of its own, despite its comment
    saying the frame was dropped. Fixed: the whole frame is now discarded up to
    the next END. The console demux, which detected overflow by the length
    dropping to 0, now checks `SLIP_DISCARDING`. The CRC-32 moved from
    `kernel/idstore.c` to `kernel/crc32.c`, so LLP shares it.
  * **Changed from §3's sketch:** no `RESET` message (session ids do that);
    `PROBE` added; CRC-32 instead of CRC-16; frame-vs-9P priority moved to the
    bindings (49.2).

* **49.2 Two QEMU nodes, one channel — DONE 2026-10-12.** An rv32 *radio*
  (virtio-net on slirp, its stand-in Wi-Fi) and an rv64 *host* (no network
  device) joined by their virtio-consoles through a unix socket, carrying LLP's
  serial binding. `kernel/llp_node.c` is both roles in one file, not the
  `llp_bridge.c` / `llp_netif.c` pair sketched here: they share the pump, the
  queues and the 9P link, and differ only in where an Ethernet frame goes.
  * **`llp radio`**: the node's stack lets go of its interface
    (`net_stack_detach()`, new), every frame goes over the link both ways, and
    the interface's MAC goes to the host in HELLO and its link state in STATE.
  * **`llp host`**: on the first handshake the node registers `wlan0` with the
    radio's MAC, attaches its stack and starts DHCP; the lease comes from slirp
    *through* the radio (10.0.2.15).
  * **Both** serve 9P over LLP's P9 frames: the `llp` link, a device on the same
    wire as `vconsole` (whose 9P pump gives the channel up). The host mounts the
    radio with the existing `(mount-remote "c6" "llp")`.
  * **One task owns the link** (`llp`). Everyone else only queues frames for it
    under a spinlock, P9 before ETH: the binding's priority rule (§3). The
    credit window is the smaller of the two receive queues' room (8 ETH, 4 P9
    slots), because LLP's credits count frames, not types.
  * **The test** (`test_llp_two_nodes`, runner): handshake with the radio's MAC,
    `wlan0`, the lease, `/c6` mounted. Then, from outside, an authenticated 9P
    session to the host's own TCP/564 (every frame through the radio): the
    host's `/proc/cpuinfo` (S-mode, rv64), `init.lisp` and a 34 KB CAS file
    byte for byte; and through the host's `/c6`, the radio's `/proc/cpuinfo`
    (M-mode, rv32) and a 33 KB file from the radio's card, a 9P request
    crossing TCP, the bridge and LLP. The radio reports no interface of its own;
    neither side counts CRC errors, losses or full queues. **20 of 20 runs
    pass, about 1.3 s each, ~210 frames bridged per run.**
  * **What it does not test:** flow control under pressure. 9P keeps one
    request in flight and our TCP's 4 KB window about three frames, so the
    receive window never fills here ("held for credit": 0, reported in the
    result line). The host harness's soak (`llp_host.c`) is where credits run
    out and recover; a UART (49.3, 49.10) is where they will matter on a wire.

### The C6 as a NIC, on the C6-Zero

* **49.3 C6-Zero radio role over its USB port — DONE 2026-10-12** (= 45.13).
  The C6-Zero, with real Wi-Fi, is the network card of a host on the other end
  of its one USB cable: this PC, played by the Python reference.
  * **One cable, two users.** `drivers/usbjtag_esp32c6.c` gains a
    demultiplexer, armed by `llp radio` and off until then. Inbound bytes from
    one SLIP END to the next go to LLP, everything else to the console.
    Outbound, LLP's pump is the only writer to the FIFO: console text from every
    other task is queued and written *between* frames. No console byte lands in
    a frame, and nothing masks interrupts for a frame's length; the radio's
    interrupt path runs on this core. Fatal-path output stays direct.
  * **`llp_node.c` on the C6:** its radio role bridges the Wi-Fi netif exactly
    as it bridged virtio-net in 49.2. Buffers now come from pages at start,
    sized by role: the C6 has no Ethernet queues, two 9P slots in and one out,
    and a one-page task. Frames are streamed to the FIFO
    (`llp_serial_emit()`, new, checked byte for byte against the encoder by the
    host harness) instead of encoded into a 3-page buffer.
  * **The host side:** `host/llp` gains `SerialNode`, a pump thread owning the
    port, with the same console/frame split and 9P before Ethernet. It also
    gains `net.Host`, enough of an IP stack for DHCP, ARP and ping, built by
    hand. Its `P9Socket` lets `p9lib` run over LLP's P9 frames, and the
    `llp host --port … --arm --dhcp --ping N --cat PATH --cmd LINE` CLI ties
    them together. **Lesson:** the host must not send its first HELLO until
    the radio has armed LLP. The first attempt's HELLOs reached the C6's line
    editor as keystrokes, where an escape byte recalled history.
  * **On the silicon** (`test_esp32c6.py`, now **21/21**, the LLP check last
    because the role lasts until reset; the twenty stand-alone checks pass with
    no host attached): `llp radio`; the handshake carries the board's station
    MAC; STATE says Wi-Fi up; a DHCP lease **for the board's MAC** from the home
    router through the bridge (192.168.178.161); the gateway answers 3/3; and
    `/proc/version` (with its `tainted` line) and `/proc/node` read over 9P on
    the same link. No CRC errors, losses or overflows on either side.
  * **Measured:**
    * **Heap in the radio role: 8–16 KB free**, down from 36–44 KB before
      arming. LLP takes 24 KB of buffers plus a 4 KB stack. This is far below
      45.11's 60 KB, which no longer holds, because the kernel has grown since.
      It is the tightest number in this phase and 49.5 has to watch it: the
      C6-MINI runs the same kernel plus an SDIO slave.
    * **Ping to the gateway through the bridge: 37–96 ms.** For comparison,
      the same PC pinging the C6's *own* stack over Wi-Fi gets 13–104 ms
      (average 29). Wi-Fi latency dominates; the bridge adds perhaps 10–20 ms
      (polled USB). That is for 49.9 to measure properly.
  * **Not done here, deliberately:**
    * **`/c6/wlan/*` control files** move to 49.7, where a P4 first hands the
      C6 its credentials (D5). Today the C6 joins from its own identity record,
      and its whole `/proc` is already readable over LLP's 9P.
    * **Persona selection on HELLO (D4)** moves to 49.6. On the C6-Zero the
      shared port is a human's console, so LLP is armed explicitly. On the
      C6-MINI, SDIO is a dedicated channel and arming at boot is natural.
    * **The optional TAP bridge.** `net.Host` proves the frames pass without
      needing root.
  * "Held for credit" now counts episodes, not retries, in C and Python. A
    radio whose host has gone counted 28 000 in a minute.

### SDIO

* **49.4 P4 SDIO host — DONE 2026-10-12, on the NANO** (= 45.14).
  `drivers/sdio_esp32p4.c`: slot 1 of the SD/MMC host on GPIO14–19 through the
  GPIO matrix (CLK 18, CMD 19, D0–D3 14–17, from the schematic and identical to
  ESP-Hosted's P4 defaults). The C6's reset on GPIO54, open-drain only.
  * **One controller, two slots.** `drivers/sdmmc_esp32p4.c` keeps the
    controller and slot 0 (the microSD) and exports a small API
    (`drivers/sdhost_esp32p4.h`). Every use runs under a re-entrant ylock with a
    current card, which goes into each command. Each card's bits of `CLKENA`,
    `CLKSRC`, `CLKDIV`, `CTYPE` and `RST_N` are its own. Status clears spare
    `[31:16]` (the cards' SDIO interrupts). The host divider is fixed at
    160/8 = 20 MHz, which the microSD takes undivided; 400 kHz is 20/(2×25),
    and each card picks its own second stage. **The microSD is unaffected:**
    `sdbench` 1610–1617 KB/s (phase 35: 1547), and the P4 suite's SD checks
    pass with both slots in use.
  * **The bring-up:** an I/O reset (CCCR RES) first, since a card that already
    has an address ignores CMD5. Then CMD5 until ready, CMD3, CMD7; the CCCR;
    the CIS's MANFID; function 1 enabled; 512-byte blocks; 4-bit. CMD52, and
    CMD53 in byte and block mode through one IDMAC descriptor (up to 4 KB, a
    bounce buffer of its own). Shell: `sdio`, `sdio probe [khz]`, `reset`,
    `rd52`, `wr52`, `rd53`, `bench` (numbers in hex).
  * **The reference was the C6 itself, not ESP-Hosted** (D7 amended): the 7B
    was not attached, and the NANO's factory image had been replaced in 49.5.
    Two things answered as known-good Espressif slaves:
    * the ROM's download mode (`reboot download`), whose CIS says device
      **0x100d**;
    * **the SDIO slave hardware itself.** With LugalOS on the C6, CMD5, CMD52
      and the CCCR/CIS still answer (device **0x6666**, the hardware default),
      so the host can be tested whatever the C6 runs.
  * **On the silicon:** OCR `0xa0ffff00` (2 functions), CCCR rev 0x32 (SDIO
    3.0), caps 0x17, vendor **0x0092** (Espressif), function 1 ready, 4-bit.
    A CMD53 read of the CCCR agrees byte for byte with CMD52.
  * **Speed:**
    * **10 MHz works, 20 MHz does not:** commands pass at 20 MHz but no data
      phase completes. The ROM samples and drives on the falling edge
      (`SDIO_FEI_FEO`; MTMS/MTDI read low on this module, C6 datasheet
      Table 3-4) and slot 1 adds the GPIO matrix's delay.
    * **CMD53 throughput at 10 MHz, 4-bit: 1675 KB/s** in 256-byte reads (603
      KB/s in 16-byte ones: per-command cost). 2.5 MHz: 444 KB/s.
    * 49.6 sets our own slave's edges, which the C6's SDIO slave allows, and
      tunes the P4's sample phase.
  * **`test_esp32p4.py` gains `test_sdio_slot1`:** probe at 10 MHz, vendor
    0x0092, 4-bit, CMD53 = CMD52. **37/37 on the NANO.**
  * **Two bugs found on the way, fixed:**
    1. **The C6's USB console died on every cold boot** (power-on, or EN pulled
       by the P4). `drivers/usbjtag_esp32c6.c`'s "host gone" path dropped its
       byte count without committing the FIFO (`WR_DONE`). After a power-on
       reset re-enumerates the port, the boot log filled the FIFO before the
       host read it, and a FIFO full of uncommitted bytes never drains. The
       kernel ran on (Wi-Fi, DHCP, ping) with a silent console. A USB reset
       keeps the host reading, so no test ever saw it. Fixed (commit, keep
       committing while full); 3/3 cold boots answer. **This is also phase
       45's open "stall at breadcrumb 0x61"** (2026-10-06): 0x61 is where a
       *healthy* radio ends up after a reconnect (`radio_plat_phy_enable()`
       writes 0x60/0x61 and takes the calibrated path with no further
       breadcrumbs), and the silence was this console.
    2. **`lcdterm.c`** declared a buffer only the U-mode terminal uses on
       boards without one (a warning on the 7B since 47.7).
  * **Kept as instruments:**
    * the C6's console is copied to its **UART0** (bounded, never waits: the
      ROM has it at 115200; header P2 pin 22 on the NANO). That is how the
      cold boot was read when USB said nothing.
    * the tick breadcrumbs gain the return address and task (LP_AON stores 5
      and 6) beside the PC.
  * **The LCD-7B (2026-10-12, same build, P4 rev v3.2):** up at the first probe
    against its on-board C6 (vendor 0x0092, device 0x6666, CCCR rev 0x32, 4-bit).
    1646 KB/s at 10 MHz, as on the NANO -- **and 20 MHz carries data there:
    2823 KB/s**, the CIS reads byte-identical at 10 and 20 MHz. The NANO's
    data phase does not at 20 MHz, with the same driver and the same C6 slave
    hardware. So the limit is the board (or the v1.3 silicon's matrix/pad
    timing), not the protocol. Which one is not yet known: 49.6 tunes the
    sample phase on both. `test_esp32p4.py --board lcd7b`: 37/37.
  * **Not done:** the card interrupt (49.6).
    Concurrent use of both slots is serialised by the lock but has not been
    exercised under load (the shell runs one command at a time).

* **49.5 LugalOS on the C6-MINI — DONE 2026-10-12, on the NANO.** The NANO's
  C6 runs LugalOS from its own flash, joins Wi-Fi from its own identity record
  at boot, and passes the whole C6 suite: **21/21, flash-booted, `--no-load`.**
  * **Preset `esp32c6-mini`** (`cmake/board-esp32c6-mini.cmake`): the chip
    facts are the C6-Zero's (an ESP32-C6FH4, rev v0.2). The **4 MB map**
    (`cmake/flash_layout_esp32c6_mini.cmake`, selected by
    `LUGALOS_C6_MODULE=MINI`, since the map is read before the board file):
    stage 2 128 KB, OS slot A 1.5 MB, **slot B 1.5 MB reserved now for 49.8**,
    832 KB spare, identity at `0x3F0000`. The OS image is 1.0 MB. No `/flash0`;
    the C6 does not mount one. The persona follows from the board file's name.
  * **Console:** the C6's own USB on header P2 21/23 (§5.3, wiring B), so
    `c6run.py`/`c6flash.py`/the suite work unchanged. `c6flash.py` now takes
    the flash size from the manifest (a new `flash` line) instead of assuming
    8 MB.
  * **`--board zero|nano`** for `c6run.py`, `c6flash.py` and
    `test_esp32c6.py`, from `~/.config/lugalos/c6-ports.env` (serial = MAC),
    as the P4's `p4-ports.env`. With both C6s attached, no tool guesses.
  * **Provisioned** `nano-c6` with the WLAN credentials, through the suite's own
    helpers (only the derived PSK reaches the board; nothing printed). The
    record lands at the new map's `0x3F0000` and survives the flash boot.
  * **`reboot` works on the C6 now** (it said "not supported": `board_reset()`
    had no C6 arm). It calls the ROM's system reset (`software_reset`,
    0x40000090). **`reboot download`** sets `LP_AON_SYS_CFG.FORCE_DOWNLOAD_BOOT`
    first. Verified: the ROM waits in download mode, and esptool connects with
    `--before no-reset`. This is what a C6 behind a P4 needs, since the P4
    cannot reach its BOOT strap. **One trap, found and understood:** a kernel
    started by `c6run.py` (a USB reset *into* download mode, then `load-ram`)
    has its boot mode latched as download, so even a plain `reboot` comes back
    in download mode. A flash-booted C6 reboots into LugalOS (verified on the
    Zero). That is the chip's strap latch, not this code: IDF's restart behaves
    the same.
  * **The module's quirks are harmless:** GPIO8 (the Zero's LED) is a strap
    with a pull-up here; the RMT frame completes with nothing attached.
    GPIO0/1 (the Zero's I2C) are unconnected; the scan finds nothing and the
    sensor check skips.
  * **Measured, radio role on the MINI: 44 KB heap free** (no sensors or MQTT,
    unlike the Zero's 8–16 KB). This is the budget 49.6's SDIO slave comes out
    of. Ping to the gateway through the bridge: 31 ms.
  * **D7 note:** this overwrote the NANO's factory ESP-Hosted, the planned SDIO
    reference for 49.4. It is backed up and verified on the chip
    (`~/Source/gith/esp/p4nano-c6-factory-flash/`); the **LCD-7B's C6 is still
    factory** and is now 49.4's reference.

* **49.6 C6 SDIO slave + LLP over SDIO** (= 45.15). Packet mode, DMA
  descriptors, receive tokens as LLP's credits, interrupts both ways, link state
  in a shared register; the APM fence on the slave's DMA (§4). A loopback mode
  (the C6 echoes `ETH` frames) for `sdio bench` from the P4. *Done when:* echo
  runs 10 minutes both ways with no lost or corrupted frame, a `RESET` and a
  GPIO54 hard reset mid-stream both recover without a P4 reboot, and throughput
  is ≥ 5 MB/s each way (two orders above what the stack needs; the margin is the
  point).
  * **In progress 2026-10-13, on the LCD-7B** (the NANO was not attached).
    The owner chose to give up the 7B's factory C6 (§7's "keep it factory"
    is overridden). It is backed up first (`~/Source/gith/esp/p4lcd7b-c6-factory-flash/`:
    boot loader, partition table and `ota_0` byte-identical to the NANO's
    backup). Then `esp32c6-mini` was flashed over header H4 (UART0 + IO9).
    * **The 7B's C6 console is UART0**, since H4 has no USB pair: the C6's
      console now also *reads* UART0, and waits for room on it when no USB
      host reads. Reflashing needs no jumper: `reboot download`, then
      `c6flash.py --board lcd7b`.
    * **The binding** (`doc/dev/protocols/llp.md` §7.2, `kernel/include/kernel/llp_sdio.h`):
      packet mode, polled on both sides. 12 × 512 B receive buffers (tokens
      are the host → C6 flow control), `PKT_LEN` for C6 → host, frames
      packed and 4-byte aligned, no `FLOW`. Shared bytes carry the `LLP`
      magic, a generation, link/RSSI. Bit 0 of `0x8D` asks for a channel
      reset.
    * **`llp_node.c` gains a packet channel** beside the serial one. The P4
      builds it (host role, `wlan0`); the C6-MINI uses SDIO instead of USB.
      The P4's pump sleeps 1 ms when a round moved nothing.
    * **Three things found on the way:**
      1. **The APM.** The TEE puts the SDIO slave's DMA in REE2, and region
         0's reset attributes deny it silently: transfers "complete" and
         nothing moves. Now the slave is in REE1 and region 1 grants it
         exactly its channel block: the fence §4 asked for. The radio opens
         region 0 for REE0 and REE2 only (`0x707`, was `0x777`).
      2. **The host → slave interrupt byte** (`CONF_W7[15:8]`) is a plain
         register that keeps the interrupt raised. The slave must clear it.
         Until it did, every poll reset the channel.
      3. **A slot-1 data phase that never finished poisoned slot 0.** The
         microSD's next CMD17 failed. Failed CMD53s now end with an SDIO I/O
         abort (a stop/abort command) and a FIFO/DMA reset, then a channel
         reset.
    * **Measured:** link up within a second. `/c6` mounted on the P4 over
      the channel, `/proc/version` (taint line) and `/proc/node` read.
      **Loopback 2000 × 1514 B: 3163 KB/s each way, 0 lost, 0 damaged**
      (20 MHz 4-bit); 2000 × 64 B: 310 KB/s.
    * **Recovery checked:** a GPIO54 reset of the C6 with the link up. The
      P4 noticed the slave was gone, followed its new generation, and the
      link came back with the restarted C6's session, with no P4 reboot. The
      next bench passed 1000/1000. A `/c6` mount does not survive it: the
      C6's fids are gone, and `(unmount "c6")` + `(mount-remote "c6" "llp")`
      is needed. Re-attaching on a new peer session is 49.8's.
    * **Still open for *done when*:** the 10-minute soak, a reset
      mid-*bench*, ≥ 5 MB/s (a C6-side copy and the 1 ms poll
      are the first suspects), the card interrupt (DAT1) instead of polling,
      the NANO at 10 MHz, persona-at-boot (D4).

### Scenario 1 — networking for the P4 boards

* **49.7 `wlan0` on the P4** (= 45.16). `llp_node.c`'s host role over SDIO; credentials
  from the P4's identity record (D5); DHCP with the C6's MAC (D3); on the NANO,
  `eth0` and `wlan0` side by side with a stated rule for which carries the
  default route. Everything in §1.1 (a)–(e) over Wi-Fi: authenticated 9P from the
  host, `fuse-p9`, MQTT from the 7B's BME280, NTP. **`p9sync.py` to the LCD-7B
  over Wi-Fi** replaces the console `p9share` path for `/sd0` updates. *Done
  when:* `test_esp32p4.py` gains Wi-Fi checks (join, lease, ping, authenticated
  9P read, MQTT message seen by the broker) passing on both boards, the 30-minute
  soak of 45.11 repeated on the 7B (no rejoin, no reset), and a `radio rejoin` on
  the C6 does not drop an open 9P session on the P4.
  * **In progress 2026-10-13, on the LCD-7B.** Everything up to Wi-Fi itself
    works; the join waits for credentials on this host (`~/.config/lugalos/wifi.env`).
    * **D5 built.** The P4 gained an identity store
      (`drivers/idstore_esp32p4.c`, one sector at `0x00FFF000`, through
      `flash_esp32p4.c`'s guarded routines), so `wlan SSID PSKHEX` works on
      the P4. At every link-up a `c6attach` task mounts the C6 at `/c6`
      afresh, and writes `join SSID PSKHEX` to `/c6/dev/wlan` while the
      radio is not associated. `/dev/wlan` is a new ctl file on the C6
      (`radio_c6_ctl_*`), which starts the radio through the same
      join-and-supervise task the boot autostart uses. The key stays in RAM
      only. Checked with a dummy network: given, `starting`, cleared again.
    * **D4, as far as the 7B needs it:** the C6-MINI arms `llp radio` at boot.
      It bridges its interface only once a host's HELLO has brought the link
      up; until then it stays a node of its own. An LCD-7B starts `llp host`
      at boot. The NANO (with `eth0`) stays manual until the default-route
      rule exists. Result: power on, and the link, `wlan0` and `/c6` are
      there with nothing typed.
    * **Found:** the 9P attach under `vfs_mount_remote()` goes **9.1 KB
      deep**. A default 8 KB task ran into the stack below it (a
      `[Sched BUG]` on the next switch to `dhcpc`). `c6attach` has 16 KB. The
      shell never showed it, because its stack is large.
    * **Wi-Fi up, 2026-10-13** (credentials entered on the P4 with `wlan`):
      the P4 gets a lease for the C6's MAC (192.168.178.160). This PC pings
      it, 16–30 ms. **Authenticated 9P over Wi-Fi** reads the P4's
      `/proc/version` and, through the P4's server, the C6's
      `/c6/proc/version` (49.8's arrangement: the PC authenticates to the P4
      only). After a C6 reset, with nothing typed, the 7B answers pings
      again in ~15 s.
    * **Found on the way, fixed:**
      1. **Two readers on the radio's RX ring.** When the credentials arrive
         after the link is up, the radio's start path attached the C6's own
         stack (and DHCP) to the interface while the bridge was adopting it.
         The ring's reader overtook the writer (`466/3190428`) and replayed
         stale slots: 1.9 GB "received", 122 MB of it flooded to the P4, real
         DHCP offers drowned. Now one lock decides who reads the interface
         (`llp_node_offer_netif()`). A detached stack stays detached (no late
         binding by a DHCP lease). The ring reader counts `head - tail` and
         resynchronises instead of reading forever.
      2. **`sdinfo` read the microSD's block 0 without the controller's
         lock,** so with the LLP pump on slot 1 its CMD17 went to the C6 (R5
         `ILLEGAL_COMMAND` on the next CMD53).
    * **Two microSD cards corrupted on the 7B** (both new SanDisks; the
      second identified afterwards as "SD032, 30 MB, 2006, serial
      ffffffff", a card in a failure state, with block 0 all zeros).
      Investigated 2026-10-13:
      * **A real hazard on the shared controller, now closed.** A transfer a
        driver gives up on (500 ms) keeps the controller's data state machine
        busy until the hardware data timeout (~0.84 s at 20 MHz). Before the
        fix the controller could be handed to the other slot in that window,
        and a microSD write started there could send whatever the FIFO held.
        That happened at least once today, while the C6's reset storm made
        SDIO reads time out (`CMD17 ... failed (status 0x30c)` followed).
        Now `sdhost_data_reset()` waits for the state machine (or resets the
        controller), every failed transfer on either slot goes through it,
        and every data command refuses to start over a busy data path
        (counted in `sdinfo`).
      * **Brown-outs are the likelier cause of a card's own failure.** The
        card's switch (Q1) is fed from `ESP_3V3`, the C6's rail, and the C6
        transmits since today. Nothing in LugalOS arms the P4's brown-out
        detector (its reset enable defaults to 0; IDF arms it in every app),
        so a sag does not reset the P4 cleanly. The boot now logs the reset
        cause (`[Chip] last reset: ...`, BROWN-OUT = 15). Arming the detector
        needs the BOD threshold over REGI2C (only `clk_esp32p4.c` has
        REGI2C access today).
      * **Checked under load:** 4 × 1 MB FAT32 write + read-back verified
        (`fsbench /sd0`) during ~23 MB of SDIO traffic each way from a Wi-Fi
        ping flood: all verified, 0 SDIO failures, 0 busy-path resets.
      * Also fixed: `sdbench w` ran the read-only benchmark on the P4 (the
        board's `sdbench` matched first).
      * **The real cause, found next day-part:** the "corrupted" card read
        fine on a Linux PC (`fsck.fat` only reported the boot sector's dirty
        byte, 0x41, which LugalOS never writes). The card was healthy;
        LugalOS could not initialise it. Its power switch is held ON through
        every P4 reset, so the card never starts over. A P4 reset in the
        middle of a write (a boot loop, or just reflashing) leaves it
        mid-transfer, and the next boot fails: "CMD0 was not accepted" in 2
        of 6 resets during `fsbench`, a garbled CID/CSD in the case seen
        earlier. **Fixed:** every boot power-cycles the card (lines driven
        low, 200 ms off). 10 of 10 resets during writes then mounted. A write
        cut off still leaves a partial file (FAT32 has no journal), but the
        card comes back.
      * **The brown-out reset is armed** (`arch/riscv/common/bod_esp32p4.c`,
        IDF's sequence, level 7 ≈ 2.6 V, the threshold over REGI2C through
        `esp32p4_regi2c_write_field()`), so a supply dip now ends in a clean
        reset that the next boot names.
    * **Next:** `p9sync.py` over TCP, MQTT, NTP; the Wi-Fi checks in `test_esp32p4.py`; the 30-minute
      soak; a cold power cycle (both chips from off).

### Scenario 2 — full 9P integration

* **49.8 `/c6` in the P4's namespace, and updates through it.** The C6's
  namespace mounted at `/c6` over the channel's `P9` type. The LAN sees `/c6`
  through the P4's server (the host authenticates to the P4; the C6 trusts the
  channel as a cable, `P9_AUTH_NOT_REQUIRED`, which is the existing policy for
  attached links). **Main objective: everything known about the C6 is a file,
  and nothing about reaching it is P4- or SDIO-specific** — 49.10 integrates a
  C6 with the RP2350 by 9P alone, through the same files. Re-cut 2026-10-14
  after reading what 49.7 left in place; three parts, in this order.

  **49.8a — a complete, robust `/c6`.** Mounting already works (49.7: the
  `c6attach` task after every handshake; `/c6/proc/version` read from the LAN
  through the P4). Missing:
  1. **State as files.** Today the C6 exports the generic `/proc/*` (the taint
     is on `/proc/version`) and one ctl file, `/dev/wlan`, whose read is a
     single word (`idle|starting|joined|not joined`). Still shell-only:
     `radio stats`, `wdt` (and the last reset cause), `llp` (both ends), the
     scan, SSID/BSSID/channel/RSSI (RSSI is not even sourced:
     `llp_link_send_state()` always sends 0), `reboot [download]`, `llp loop`.
     Make `/dev/wlan`'s read `key value` lines (state, ssid, bssid, channel,
     rssi, mac, joins, rejoins); add `/proc/radio`, `/proc/llp`, `/proc/wdt`
     and a reboot ctl. `/proc/llp` exists on both nodes, so `/proc/llp` and
     `/c6/proc/llp` read side by side. **The same names on every radio node**
     (the RP2350W's CYW43 too), so a client never needs to know which radio a
     node has. This settles the names: the `/c6/wlan/{ctl,status,scan,mac}`
     sketched here earlier becomes `/dev/wlan` plus `/proc/*`.
  2. **A dead link fails fast.** Every RPC to an absent C6 waits
     `P9_CLIENT_TIMEOUT_MS` (10 s), and the P4's 9P server is synchronous: one
     LAN client touching `/c6` during a C6 reset stalls the P4's whole 9P
     service, 10 s per request. On LLP DOWN the mount answers Rerror at once;
     LAN fids into `/c6` held across a C6 restart get errors, never another
     file.
  3. **Flush on a new peer session.** `llp.c` drops the old session's state,
     but `llp_node.c`'s `g_p9_tx`/`g_p9_rx` keep queued T-messages, which then
     reach the restarted C6.
  4. **No mount cycles.** Once the C6 can mount its host (49.9), `/c6/p4/c6/…`
     is a loop between two synchronous servers: a deadlock until the timeout.
     A remote mount is not served back over the link it came from.
  5. **Attach on any channel.** `c6_attach_task` is under `#if CHAN_PACKET`
     (SDIO only); the RP2350's UART binding (49.10) must mount `/c6` too.

  *Done when:* every fact `radio`, `wdt` and `llp` print on the C6 is readable
  under `/c6` from the P4's shell and from the LAN through the P4;
  `test_esp32p4.py` checks that, and that a C6 reset during an open LAN session
  yields prompt errors and a working `/c6` again within ~15 s.
  * **Done 2026-10-14.** Files (all `key=value`, documented in
    `doc/admin/networking.md` §4): `/dev/wlan` (state, ssid, bssid, channel,
    rssi, mac, joins, last_disconnect; `join`, `rejoin`, `scan`),
    `/proc/wlanscan`, `/proc/radio`, `/proc/wdt`, `/proc/llp` (both nodes),
    `/dev/reboot` (`reboot`, `download`; RP2350 and C6). The radio samples the
    AP every 5 s and sends real RSSI in STATE (`peer_rssi`). The 9P link got
    `up()` and `generation` (`fs/p9_link.h`): requests fail at once while the
    link is down, a new peer session flushes queued 9P both ways, remote mounts
    take fid numbers no earlier mount used, and a mount is never re-entered
    over the link its request came in on. `/c6` is attached on any channel and
    retried every 5 s while unmounted.
    * **Found and fixed on the way: the 9P server had one fid table for all
      clients, and every Tversion emptied it** — a second LAN client, or the
      C6 mounting the P4, cost every other client its fids. Fids and the
      negotiated msize are now per connection (`p9_server_process_conn()`,
      the link a request came in on).
    * Verified: QEMU `test_llp_two_nodes` (both `/proc/llp`s through the
      host; the radio's Tversion on the host leaves the LAN session's fids
      alone; `/c6/p4/...` refused in < 3 s; radio killed: `/c6` fails after
      2.4 s instead of 10 s; a new radio is remounted by itself and reached
      from the LAN). Full QEMU suite 506/506. LCD-7B `test_esp32p4.py` 38/38
      with the new `/c6` check: 5 files read, rebooted through
      `/c6/dev/reboot`, remounted within 1.8 s, associated again, −64 dBm. A
      flash-booted C6 is back with a new session so fast that the P4 sees the
      new HELLO before the old link times out.
    * Not done here: `/dev/wlan` and `/proc/wlanscan` on the RP2350W's CYW43
      (the same names, for 52 or 49.10); the NANO (it starts `llp host` by
      hand) and a C6-Zero were not attached.

  **49.8b — OTA, generic shape; the C6 as its first backend.** **Found
  2026-10-14: the image the boot ROM loads is not a loader, it is the kernel's
  RAM half** (`0x0` on the C6, `0x2000` on the P4), and it carries slot A's
  flash offset as a compile-time constant (`xip_esp32c6.c`, `MMU_ITEM_CONTENT`
  from `LUGALOS_C6_OSIMAGE_BASE`). Writing slot B alone would pair a new XIP
  half with an old RAM half — the same trap an `os`-only flash springs on the
  P4. So D6's "the stage 2 never rewritten remotely" needs a real loader
  first. And since the P4 and the RP2350 need A/B updates just as much, the
  part above the boot mechanism is built once, for every node:

  * **Files** (every OTA-capable node, names to settle in the code): an image
    file, written sequentially, erasing ahead of itself; a ctl file — read:
    active slot, the other slot's state (`empty | written | verified | trial |
    confirmed`), each slot's build-id; write: `begin SIZE SHA256`, `commit`,
    `reboot`, `confirm`, `abort`. Pure 9P, so every path works unchanged: LAN
    → node, LAN → P4 → `/c6`, RP2350 → C6 over UART, the console's SLIP
    `p9share`. An RP2350 can update a C6 knowing nothing about ESP chips.
  * **One update image format:** header (magic, board/preset, **chip
    revision**, build-id, SHA-256 via `kernel/sha256.c`, section sizes), then
    the payload — both halves on the ESP chips, the IMAGE_DEF image on the
    RP2350. The header lets a node refuse a wrong image *before* erasing
    anything (a 7B build halts on a NANO at boot today; in a slot it would be
    worse). Room for a signature later; authenticity today is 9P
    authentication to the node.
  * **One state machine** (`kernel/ota.c`), board backends behind it
    (`drivers/ota_<chip>.c`). Semantics everywhere are the RP2350 bootrom's
    "try before you buy" (datasheet §5.1.17): the new slot boots *once* on
    trial, the kernel confirms itself (default: N seconds up plus a
    role-specific check — on the C6, a host's HELLO or joined Wi-Fi), the
    watchdog catches hangs (MWDT0 30 s on the C6), and an unconfirmed trial
    falls back to the old slot. **Rollback is the node's own business**: no
    GPIO54 from the P4, so it works behind an RP2350 or stand-alone.
  * **One host tool** (`tools/lota.py`, working name) over any 9P path;
    `c6flash.py`/`p4flash.py`/picotool remain the recovery route.
  * **The C6 backend:** a small permanent loader at `0x0` (ROM-loaded; reads
    the boot record, loads the chosen slot's RAM half into SRAM, maps that
    slot's pages at `0x42000000` — the XIP half is unchanged by which slot it
    sits in, only the MMU entries differ), both halves in each slot, a boot
    record that survives power loss at every step (advance its state by
    clearing bits, which needs no erase; confirm by switching to the second
    copy). Layouts: the C6-MINI's already has slot B (§5.2); the C6-Zero's
    8 MB map needs one (it has the room). The loader goes on once per board
    through the header (one more §5.3 user action); after that, never.
  * **Risk to measure first:** erasing ~1 MB runs with the flash cache off
    for tens of ms per sector, and the Wi-Fi blob executes from flash. Sector
    by sector with yields (or erase suspend), checked with Wi-Fi up.

  The P4 and RP2350 backends, and the rest of remote administration, are
  `plan/phase52_remote_admin.md`.

  *Done when:* a C6 image goes from the build directory to the 7B's C6 (and
  the NANO's) over Wi-Fi-then-SDIO with no header attached, through the
  generic files and `lota.py`; a wrong-board image is refused before any
  erase; a deliberately broken image (one that never confirms) is rolled back
  by itself; a power cut during the write leaves the old slot booting.
  * **Done 2026-10-14 on the LCD-7B's C6.** Built as planned, with these
    decisions on the way:
    * **Files:** `/dev/ota` (ctl; `begin SIZE`, `commit`, `reboot`,
      `confirm`, `abort`; status as `key=value`) and `/dev/otaimage`. The
      SHA-256 is in the image header, not an argument of `begin`.
    * **Nothing is erased before the header has arrived** and names this
      board (from the board file: `esp32c6-mini`) and chip; the slot's header
      sector goes first, the parts follow a 4 KB sector at a time (erase,
      program, yield), and the header is written back only at `commit`,
      after the SHA-256 of what is in flash matched. A slot cut off half-way
      has no header and is never booted.
    * **The loader** (`arch/riscv/esp32c6/loader.c`, 2.4 KB, at
      `0x40870000`, on the ROM's stack) passes the slot to the kernel in
      a0..a3; `esp32c6_xip_map()` maps that slot's XIP half. A RAM-loaded
      kernel (`c6run.py`) is not loader-booted and cannot update. On a trial
      the loader sets the RTC watchdog's own enable (60 s, system reset); the
      kernel's early boot clears only the ROM's flash-boot bit, so it keeps
      running until the confirm disarms it.
    * **Health** on the C6: the LLP link up when the node runs LLP, else
      associated when it has credentials, else just running; confirmed 10 s
      in at the earliest, given up 10 s before the watchdog.
    * Shared pieces: `kernel/ota.c` (generic), `kernel/ota_image.c` (format
      and boot-record logic, also in the loader; `tests/host/ota_host.c`
      under ASan/UBSan), `drivers/flash_esp32c6.c` (the identity store's
      guarded ROM calls, now with a write guard: identity, boot records, the
      slot not running), `drivers/ota_esp32c6.c`, `tools/lota.py`;
      `c6flash.py` writes the loader, erases the records and puts the same
      image into slot A.
    * **Verified on hardware** (through the P4's console and `/c6`): 1 MB at
      68 KB/s, Wi-Fi associated throughout the erases; trial on slot B
      confirmed after 10 s; a `--break` image (64 KB of kernel code zeroed)
      booted, died, was reset by the RTC watchdog (`last_reset=watchdog (RTC,
      system)`) and the loader fell back by itself (`tried`, `failed`,
      `fell_back=yes`); a C6 reset in the middle of a write booted the old
      slot normally, with the new slot empty; an `esp32c6-zero` image was
      refused at its header with the other slot untouched; a further update
      after all of that confirmed. LCD-7B `test_esp32p4.py` 38/38 (the `/c6`
      check reads `/c6/dev/ota` too).
    * **Power cut, by hand:** the 7B's power pulled 524 KB into a 1 MB
      update (`lota.py push --rate 4`, slot A's XIP half being written). Both
      chips came back by themselves: the C6 reported `last_reset=brown-out`,
      booted slot B without a trial, slot A `empty`; the P4 remounted `/c6`
      and the radio rejoined. The next update confirmed.
    * **Over Wi-Fi:** to the 7B's C6 through the P4's authenticated TCP
      9P server (console key set with `p9key` for the session) and SDIO:
      33 KB/s, confirmed; the 9P session to the P4 survived the C6's reboot
      (which takes the P4's Wi-Fi with it for a few seconds).
    * **The C6-Zero** (sensor persona: MQTT, BME280, its own IP): converted
      once with `c6flash.py` (its identity, credentials and MQTT settings
      kept), then updated over its own TCP 9P server, 40 KB/s, confirmed
      after 10 s (joined Wi-Fi). `test_esp32c6.py --no-load` 21/21 on the
      loader-booted kernel. It found two loader bugs:
      1. **The ROM's chip-size record says 2 MB** until told otherwise, and
         bounds-checks every read: the Zero's slot B at 0x220000 read as "no
         image" and every update there fell back. The loader now sets the
         record from the flash map first (the MINI's slot B header sits below
         2 MB, which is why the 7B never showed it).
      2. **The RTC watchdog's settings survive a system reset**, so the image
         booted after a failed trial inherited the armed watchdog and was
         reset every 60 s; and a watchdog *system* reset left the Zero's
         USB-Serial/JTAG unusable from the host (EPROTO on every open, the
         chip running on, a power cycle to recover). Now the watchdog does an
         RTC (whole-chip) reset, as IDF's bootloader and panic handler do,
         the loader disarms it on every non-trial boot, and so does the kernel
         (`ota_board_boot()`). Rollback re-verified on both boards: the 7B's
         C6 `rst:0x10 (LP_WDT_SYS)`, the Zero `last_reset=watchdog (RTC,
         system and RTC)` with its USB re-enumerated; no further resets
         (120 pings over two minutes).
    * **Open, not in the update logic:** 2 of ~12 updates sent over the
      P4's *console* (`--p4`, 9P on the console wire) died mid-way with
      "connection closed while reading a SLIP frame", and the P4 had then
      been reset (`power-on`, i.e. EN). Likely the failed session's port was
      closed without parking it, so the next open reset the LCD-7B; not
      confirmed. None over TCP.
    * **The NANO** (2026-10-14): its C6 converted once over its own USB
      (`c6flash.py --board nano`), its P4 flashed with the current build;
      through the P4 (`llp host`, `lota.py push --p4 nano --prefix /c6`):
      update 66 KB/s, confirmed; `--break` fell back by itself
      (`last_reset=watchdog (RTC, system and RTC)`, USB intact).
      `test_esp32p4.py --board nano` 38/38 (the `/c6` check now starts
      `llp host` itself on a NANO). `test_esp32c6.py --board nano --no-load`
      20/21: its USB LLP-host check now skips on a C6 that arms the radio
      role at boot toward its P4 (49.7) -- this PC cannot play the host over
      a USB that carries no LLP.
    * **Open: the C6-MINI's memory in the radio role.** `kobjselftest` fails
      on the NANO's C6 ("waiter 2 never started", "workers finished: 2 of
      3"): with the radio role armed at boot, 6 pages (24 KB) of 61 are
      free, and the role itself holds 24 KB (`memory_kb` in `/proc/llp`) plus
      its pump's stack -- the selftest's third task finds no stack. The
      stand-alone C6-Zero passes it. Not from 49.8 (nothing there holds pages
      outside an update); the §7 risk "C6 RAM in the modem persona", made
      real by 49.7's radio role at boot.

### Checking the verdict

* **49.9 Frames versus 9P-only, measured.** On one board: the C6 mounts the
  P4 at `/p4` (A1 — the same `vfs_mount_remote()` in the other direction), so a
  host can reach the same file both ways — through `wlan0` and the P4's own
  server, and through the C6's server and the channel. Measure file read/write
  throughput and round-trip latency for both, the C6's heap and CPU load in each,
  and session survival across a C6 restart. Record in §8. *Done when:* the
  numbers are in this plan and §1.5 is either confirmed or rewritten.
  * **Found 2026-10-14: not measurable on one board as written.** While it
    bridges, the C6's own stack is detached (49.7), so no LAN host reaches
    the C6's server; giving it an IP beside `wlan0` would be two addresses
    on one MAC, which D3 rules out. A1 needs a third persona: LLP up, 9P
    only, no bridge, the C6 keeping its own stack and IP. That persona is
    also what a pure-9P RP2350 integration (49.10) would be — the C6 the LAN
    node, the RP2350 a subtree of it — so it is worth building as a function,
    with the measurement as its by-product. Its costs: the LAN then
    authenticates to the tainted C6 (against D5's "peer keys never reach the
    C6"), and the C6 needs a 16 KB task for a remote attach (9.1 KB deep).
    **Tentative (2026-10-14), to be reviewed before 49.10 starts:** the
    RP2350 gets its own `wlan0` (frames, as the P4), with 9P for the C6's
    state; the 9P-only persona stays 49.9's, for the measurement. 49.8a holds
    either way.
  * **Done 2026-10-14, on the LCD-7B** (the C6-Zero as the stand-alone
    baseline). The 9P-only persona is `llp bridge off` on the radio: the
    interface goes back to the C6's own stack (its DHCP lease on the station
    MAC -- the same address the P4 had), STATE says no link so the P4's
    `wlan0` rests, and LLP carries 9P only; `(mount-remote "p4" "llp")` on
    the C6 gives `/p4`. `llp bridge on` hands it back. Measured with
    `tests/hw/p9_paths.py` (same file, `/sd0/bench.bin` on the P4, 256 KB
    written and read back three times, each read identical; 20 small reads),
    results in §8:
    * **Frames win on every axis measured.** Reads 108 vs 82 KB/s, writes
      54 vs 46 KB/s, a small read 102 vs 218 ms: the forwarding hop over LLP
      adds ~90 ms per four-RPC read (the C6's own server alone answers in
      126 ms, the Zero's in 130 ms -- the hop, not the C6's server, is the
      cost; its remote reads also go in 1 KB pieces, `p9_remote_read_chunk()`).
    * **9P-only costs the C6 36 KB** of the 60 KB it has free while bridging
      (its own stack's task and frame page, DHCP, the mount): 24 KB left.
    * **A session through the C6's server dies with the C6**; one to the P4
      read again 20 s after a C6 restart.
    * Not measured: the C6's CPU load -- the kernel has no per-task CPU
      accounting.
    * **§1.5 stands:** frames (C) for the network path, 9P for the C6's
      state. Nothing here argues for A1 beyond "it works": it is slower,
      dearer in the C6's RAM and loses sessions. The tentative 49.10 shape
      (the RP2350 with its own `wlan0`) follows from the same numbers.
    * **The 9P-only persona was removed again** right after the
      measurement: slower, dearer in the C6's RAM, sessions lost, and its
      runtime switch leaves the C6 and the P4 claiming one address (one
      MAC), with nothing to use it. Mounting the host from the C6 works
      without it (bridged, as in QEMU's two-node test). To measure again:
      `git show 5392bea -- kernel/llp_node.c kernel/shell.c` brings back
      `llp bridge off|on`; `tests/hw/p9_paths.py` stayed.

### The RP2350 terminal

* **49.10 C6-Zero as a Wi-Fi add-on for the RP2350-terminal** (= 45.17). LLP
  over SLIP + CRC on the RP2350's UART1 (`drivers/uart1_link_rp2350.c`'s pins,
  today 115200 baud). Bandwidth first: the highest baud both PL011 and the C6's
  UART hold without loss over the actual wiring. The RP2350's stack and its 2 KB
  msize stay as they are; `wlan0` is `llp_node.c`'s host role. *Done when:*
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
| CMD53 throughput (hardware slave, 4-bit, 256 B reads) | 1675 KB/s at 10 MHz (NANO); 2823 KB/s at 20 MHz (7B) | 49.4 |
| LLP loopback over SDIO, each way (1514 B frames) | 3163 KB/s (7B, 20 MHz 4-bit, polled) | 49.6 |
| C6 heap free, modem persona | — | 49.3 / 49.7 |
| 9P read of a 256 KB P4 file, host → P4: via `wlan0` (frames) / via the C6's `/p4` (9P-only) | 108 / 82 KB/s | 49.9 |
| 9P write of the same file: frames / 9P-only | 54 / 46 KB/s | 49.9 |
| Small 9P read (walk, open, read, clunk) of the P4's `/proc/version`: frames / 9P-only | 102 / 218 ms median | 49.9 |
| Small 9P read from a C6's own server: the 7B's C6 / the stand-alone C6-Zero | 126 / 130 ms median | 49.9 |
| C6 pages free (of 61), frames / 9P-only (its own stack, the `/p4` mount) | 15 (60 KB) / 6 (24 KB) | 49.9 |
| Session survives a C6 restart: frames / 9P-only | yes / no | 49.9 |
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
* BLE, 802.15.4, WPA3, low-power modes (phase 45 §8 stands).
  SoftAP for first-time setup is `plan/phase52_remote_admin.md`'s.
* OTA backends for the P4 and the RP2350, and a web admin interface
  (`plan/phase52_remote_admin.md`; 49.8b builds the generic OTA shape and the
  C6 backend only).
* The P4's USB-Serial/JTAG as a 9P link (deferred in 47.13; 49.7 makes it less
  needed still).
* TLS (`plan/phase52_remote_admin.md` §1.5).

## 11. References

* `plan/phase45_esp32c6.md` — the C6 node, the radio domain, frame rings (§4.3–§4.9), the sketches this phase replaces (§5–§7)
* `plan/phase27_esp32p4_bringup.md` §7 — the P4↔C6 wiring; `plan/phase47_esp32p4_lcd7b_ribbon.md` §9 — the 7B's C6
* `plan/phase5_distributed_design.md` Rule 2 ("9P *is* the service IPC"), A5 mounts; `fs/p9_link.c` (T/R demux), `fs/vfs_server.c` (`vfs_mount_remote()`)
* `net/include/net/netif.h`, `kernel/radio_netif_c6.c`, `drivers/cyw43_rp2350.c` — the `netif` seam and its templates
* `drivers/sdmmc_esp32p4.c` — the P4's SD/MMC controller (slot 0)
* C6 datasheet §3.1 (boot modes), §4.2.1.8 and TRM chapter "SDIO Slave Controller"; P4 TRM ch. 57 (SD/MMC host): `~/Source/gith/esp/datasheet/`
* IDF: `components/sdmmc/sdmmc_io.c` (host CMD52/53), `components/esp_driver_sdio/` (slave); ESP-Hosted as a bus reference only
