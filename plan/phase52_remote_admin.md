# Phase 52 — Remote admin: OTA over 9P, a web admin interface, first-time setup, TLS

**Status: stub, written 2026-10-14; the identity, claiming and certificate
decisions in §1.3 and §1.5 confirmed by the owner the same day.** Holds the
key information so far; the milestones are elaborated later. Follows phase
49, whose 49.8b builds the generic OTA shape and its first backend (the C6),
and whose 49.8a makes a node's state readable as files. Everything here
builds on those two.

**The idea in one line:** a LugalOS node is administered entirely through its
9P namespace: updates, state and configuration. The web interface and the
setup access point are two more ways to reach the same files. Neither has a
function of its own that the namespace lacks.

**The product case it serves:** hardware that can be shipped to someone who
configures it with nothing but a phone. Phase 21 §6 ruled that out
("provisioning over the network is refused"). §1.3 replaces that rule with
one that allows it without giving up what it protected.

**Added section Architectural Review Required:** to evaluate the support of
the Espressif default-firmware update-protocol for easier initial installation.

---

## 1. Topics

### 1.1 Full OTA support around 9P

The interface, the image format and the state machine come from 49.8b
(`kernel/ota.c`, an image file plus a ctl file per node, "try before you buy"
semantics everywhere, `tools/lota.py`). This phase adds the remaining
backends and the board-level update.

* **ESP32-P4 (NANO v1.3, LCD-7B v3.2).** Same structure as the C6: the image
  the ROM loads from `0x2000` is the kernel's RAM half, not a loader. That is
  why an `os`-only flash traps today. The C6 backend's design carries over:
  a permanent loader at `0x2000`, both halves in each slot, and a boot record
  that survives power loss. Differences are ROM addresses, cache and MMU
  registers, and the two silicon revisions. The chip revision in the image
  header must be checked, because a build for the wrong revision halts at
  boot. 16 MB of flash, so plenty of room. The brown-out reset is armed now,
  so a dip in the supply during an update ends in a clean reset.
* **RP2350.** The bootrom does most of it: partition tables, A/B partition
  pairs, try-before-you-buy (a `FLASH_UPDATE` reboot, then `explicit_buy()`
  within a fixed 16.7 s watchdog), and address translation so either
  partition runs at `0x10000000` (datasheet §5.1.7, §5.1.17, §5.1.19, §5.10.4).
  The image needs an IMAGE_DEF block. **Flash is tight at 4 MB**: today
  1.5 MB is reserved for the OS (~470 KB used), plus `/flash0` and the
  identity sector. Two slots mean shrinking them to about 1 MB each and
  repartitioning every board once (UF2 + partition table), moving `/flash0`'s
  contents along. Not every RP2350 preset has a network. The terminal is
  updated over its USB 9P port (`if02`) or through a C6 (49.10); the same
  files work on both.
* **Updating a whole board.** P4 + C6 boards update C6 first, then the P4,
  checked against LLP's `HELLO` version. The result is a 7B updated entirely
  over Wi-Fi.
* **What OTA does not cover.** `/flash0`, `/sd0` and the identity record are
  data. Their formats must stay readable across versions (the identity record
  is versioned already, phase 21 §4).
* **Later:** signed images (the RP2350's secure boot with keys in OTP; ESP
  secure boot unused so far). Until then, only someone authenticated to the
  node over 9P can write an image.

### 1.2 A remote admin web server

* **Nothing exists yet:** there is no HTTP server in the tree (phase 49
  §1.1 (f) names it as what comes next).
* **Architecture** (AGENTS.md): plain HTML, vanilla JavaScript and CSS,
  WebSockets + JSON. **A generic bridge, not endpoints per feature**: the page
  lists, reads and writes 9P files (`/proc/*`, ctl files, the OTA files) as
  JSON over one WebSocket. Every admin function is a file first, the web view
  comes second, and a feature reachable from the web is reachable from
  `fuse-p9`, the shell and Lisp too. Remote mounts are visible like any other
  directory, so a P4's page shows `/c6` as well.
* **Where it runs:** the P4 boards and RP2350s with PSRAM (the TLS buffers,
  §1.5). The C6 serves at most the setup page in §1.3, if anything.
* **Constraints:** `TCP_MAX_CONNS 2` (`net/tcp.c`), a receive window the size
  of the 9P msize, and synchronous servers. The page's assets are static
  files (on `/sd0` or `/flash0`, or in `.rodata` for a board with neither).
* **Authentication:** grants from `peers` (phase 21 §5.2), with an `admin`
  level for writing the owner identity (§1.3). How a browser proves it holds
  a key is open (§3).

### 1.3 Two-part identity, claiming, and setup from a phone (decided)

**Replaces phase 21 §6's bootstrap rule** (amendment noted there).

* **The identity record has two parts.**
  * **Factory part:** uid, device key, the per-device **setup secret**, and
    (later) the device certificate's key. Written only through the console or
    a host tool (`tools/provision.py`), as phase 21 requires today.
  * **Owner part:** node name, WLAN SSID and PSK, the owner's peer key, later
    settings (MQTT broker, ...). The only part the network may ever write.
* **Unclaimed and claimed.** A node with an empty owner part is *unclaimed*
  and accepts setup over the network. The first successful setup installs the
  owner's key and **claims** the node, which closes the setup path. After
  that, owner-part changes need authenticated 9P with the `admin` grant.
  Phase 21's sentence still holds in a sharper form: a device that accepts an
  identity from the network has no identity *yet*. A claimed one refuses.
* **Reset is physical.** A long button press (§1.4) or a console command wipes
  the owner part and returns the node to unclaimed. The factory part
  survives, so the device keeps being the same device.
* **A build flag,** e.g. `CONFIG_NETWORK_CLAIM`, decides whether a build offers
  network claiming at all. Product builds enable it; development presets keep
  console-only provisioning. A build without it cannot be claimed over the
  network even if the state machine has a bug.
* **The setup secret stops someone nearby from claiming the device first.**
  It is the setup access point's WPA2 key, printed on a label or QR code, or
  shown on the panel where there is one. Inside that encrypted Wi-Fi link,
  setup over plain HTTP is acceptable.
* **The passphrase still never reaches the board** (phase 21 §5.3, I6). The
  setup page derives the PSK in the browser: PBKDF2-HMAC-SHA1, 4096
  iterations, salted with the SSID, sending only the 256-bit PSK.
  **WebCrypto (`crypto.subtle`) is not available on plain HTTP** (it needs a
  secure context), so the setup page carries PBKDF2 in plain JavaScript
  (~100 lines, fast enough on a phone). With TLS (§1.5), WebCrypto works for
  the admin pages.
* **Missing pieces:**
  * SoftAP mode in each radio. The C6 uses the Espressif blob's AP mode
    (phase 45 and 49 left SoftAP out). The RP2350W's CYW43 has its own AP
    mode.
  * A DHCP server (the tree has only the client).
  * Optionally a catch-all DNS server, so phones open the setup page by
    themselves (captive portal).
* **On P4 boards the radio is the C6** (D3 of phase 49: the P4 owns the IP).
  The C6 runs the access point and bridges frames, and the P4 serves the page
  and the DHCP leases on `wlan0`. Reaching the C6's AP mode is a write to
  `/c6/dev/wlan`, in keeping with 49.8a.

### 1.4 Triggers, including buttons

Setup mode, and the owner-part reset, are started by:

* an unclaimed node at boot (setup mode);
* the console (both), or the `admin` grant over 9P (setup mode only);
* **a button**, as a long press (e.g. 5 s for setup mode, longer for the
  reset), so that a strap pin pressed at reset keeps its ROM meaning.
  Candidates, **none verified yet**:

| Board | Button | Note |
|---|---|---|
| ESP32-C6-Zero / C6-MINI | BOOT (GPIO9) | strap pin; readable after boot? |
| ESP32-P4 NANO / LCD-7B | BOOT (GPIO35) | strap, shared with the CH34x/CH343 DTR line on the dev setup |
| LCD-7B, RP2350 touch terminal | touch (GT911) | e.g. a long press on the menu bar; no extra hardware |
| RP2350 boards | BOOTSEL | readable only by briefly giving up the flash's CS (the SDK's trick: XIP stops meanwhile) |
| Pico-Clock-Green | SET / UP / DOWN | already polled (`drivers/pico_clock_*.c`) |
| RP2350 chess | TM1638 keys | already read |

### 1.5 TLS (decided: in this phase)

Moved here from phase 49 §10.

* **Trust comes from a LugalOS certificate authority the user runs.** No
  public CA issues certificates for `192.168.x.x` or `.local` names, so
  there is no way around this. The tree provides scripts to create the CA
  (its key stays on the user's machine) and to issue device certificates. A
  user who wants TLS installs the root certificate on their devices and
  browsers once.
* **Issuing as files:** the node generates its TLS key itself (the private key
  never leaves it) and offers a certificate signing request as a file. The
  CA script reads it over 9P (or the web page hands it over), signs it, and
  writes the certificate back. The certificate belongs in the owner part:
  it is the owner's CA that signs it.
* **Scope:** server-side HTTPS first (the admin pages). Client-side TLS
  (MQTT to a broker, §1.6) after that. TLS 1.3 with X25519, AES-GCM or
  ChaCha20-Poly1305, Ed25519 or P-256: all integer arithmetic, fine without
  floating point. Hardware accelerators where the chip has them (P4 and C6:
  AES, SHA, ECC per their TRMs; RP2350: SHA-256), to verify.
* **Memory:** about 16 KB of record buffer each way per connection, because
  browsers don't negotiate smaller records reliably. Fine on the P4
  (PSRAM). **An RP2350 serving TLS needs PSRAM** (accepted restriction). This
  is about the RP2350 ending TLS itself. Whether a non-PSRAM RP2350 can still
  have Wi-Fi without TLS through a C6 is up to the 49.10 shape.
* **Entropy:** TLS keys need a real random source. Check what feeds
  `kernel/random` on each chip: the RP2350 has a hardware RNG; the ESP chips'
  RNG is only truly random with RF or the ADC running.
* **A benefit specific to this tree:** with TLS ending on the P4, the tainted
  C6 forwards only ciphertext. The Espressif blob can't read admin traffic or
  credentials.

### 1.6 Crypto on C6 sensor nodes (to investigate)

A stand-alone C6 (sensor persona: own IP, MQTT, its 9P server) has no
encryption today. Whether there is room for it, measured on the C6-MINI build
(2026-10-14): `.bss` is 162 KB plus 27 KB `.radio_bss`, with ~60 KB of heap
free once the radio is up and MQTT running (45.11). The C6 boards use the
default sizes for everything; nothing is trimmed for them yet. The largest
static buffers, by owner:

| Owner | Buffers | KB |
|---|---|---|
| Lisp (`user/lisp/lisp.c`) | `string_large` 16, `node_pool` 16, `string_small` 10, `gc_work_stack` 4 | 46 |
| 9P (`fs/p9_chan.c`, `fs/p9_link.c`, `fs/vfs_server.c`) | `g_handles` 9, `g_waiters` 8, `g_req_buf`/`g_resp_buf`/`g_reply`/`g_remote_tx`/`g_remote_rx` 4 each | 37 |
| klog ring (`kernel/klog.c`) | `g_ring` | 8 |
| Shell (`kernel/line_editor.c`, `kernel/vterm.c`) | `history_stack` 5, `g_vterms` 3.4 (terminals, on a board with no screen) | 8 |

A sensor-node profile (no Lisp or a small one, phase 48's dual profiles; 9P
buffers and waiter tables sized for one client; no vterms) plausibly frees
50–70 KB. That is room for a TLS client, if records stay small:

* **TLS 1.3 client with certificates from the LugalOS CA** (§1.5), with small
  records requested through `max_fragment_length` / `record_size_limit`.
  Whether the broker (mosquitto/OpenSSL) honours them is to be checked.
* **Or TLS-PSK** (mosquitto supports it): no certificates and no public-key
  arithmetic, only AES-GCM and SHA-256 (already in `kernel/sha256.c`). It is
  tiny, but has no forward secrecy and needs a key per node in the broker.
* The C6's AES/SHA/ECC accelerators cut code and CPU time.

In the bridging persona the C6 needs no crypto at all: the host's TLS passes
through it end to end.

## 2. Depends on

* 49.8a (state and control as files, the same names on every radio node),
  49.8b (OTA's generic shape and the C6 backend), and the 49.10 decision
  (pure 9P versus frames for the RP2350 and its C6).
* Phase 21: identity, `peers` grants, the write policy; its §6 bootstrap rule
  is replaced by §1.3 here.
* Phase 48: the dual resource profiles (for §1.6).
* Identity stores: `kernel/idstore.c` (RP2350, QEMU),
  `drivers/idstore_esp32p4.c`, `drivers/idstore_esp32c6.c`. All three need the
  factory/owner split.

## 3. Open questions (to settle when this phase is elaborated)

1. How a browser authenticates against phase 21's key model (a key generated
   in the browser at claim time and kept there? a password derived to a key?).
2. How long setup mode stays open, and whether it closes after the first
   failed attempt.
3. The RP2350 repartition: slot sizes, what happens to `/flash0`, and moving
   existing boards over once.
4. Which buttons are usable per board, once checked on hardware.
5. Whether the C6 itself ever serves a page (stand-alone persona), or only
   through a host.
6. Device certificates: the owner's CA signs them after claiming. Is there
   also a manufacturer's certificate in the factory part (proving the device
   is genuine)?
7. For §1.6: certificate-based TLS or TLS-PSK on the sensor nodes, and which
   brokers have to work.

### 3.1 Architectural review required (added 2026-10-15)

1. For P4-C6 systems, initial installation requires multiple USB
   connections to individually flash each component. On some systems,
   the C6 flash interfaces are not easily accessible (hardware like
   M5Stack Tab 5, a tablet-like P4-C6 that would require mainboard
   modifications to access the flash ports with hight probability of
   breaking things)
2. Evaluate to support the "ESP-Hosted Transport OTA (implemented via the
   host_performs_slave_ota framework)" (see ESP-IDF SDK, https://components.espressif.com/components/espressif/esp_hosted/versions/2.9.6/examples/host_performs_slave_ota?language= ), since that is what is
   already factory-installed on C6:
   2.1 Possible to make our approch and Espressif's implementation
   compatible? We also need to use the same A/B firmware partition layout.
   2.2 Alternative: both update-systems co-exist, and both directions are
   supported: LugalOS <-> ESP-Hosted OTA
   2.3: Testing: on easily accessible systems (P4-Nano, Waveshare P4 Wifi
   terminal) we test both directions of updates between the different
   firmware versions, and once things work, we progress towards more
   closed, higher-risk systems like the M5Stack Tab 5.
