# Phase 52 — Remote admin: OTA over 9P, a web admin interface, first-time setup, TLS

**Status: Part A done (52.1–52.5a, 2026-10-18). Part B re-checked against the
tree and cut into milestones 52.6–52.16 on 2026-10-25 (§3.2, §4 Part B); its
decisions were confirmed by the owner the same day (§3.3).** The identity, claiming and
certificate decisions in §1.3 and §1.5 were confirmed by the owner on
2026-10-14. Follows phase 49, whose 49.8b built the generic OTA shape and its
first backend (the C6), and whose 49.8a made a node's state readable as files.
Everything here builds on those two.

**The idea in one line:** a LugalOS node is administered entirely through its
9P namespace: updates, state and configuration. The web interface and the
setup access point are two more ways to reach the same files. Neither has a
function of its own that the namespace lacks.

**The product case it serves:** hardware that can be shipped to someone who
configures it with nothing but a phone. Phase 21 §6 ruled that out
("provisioning over the network is refused"). §1.3 replaces that rule with
one that allows it without giving up what it protected.

**Architectural Review Resolved (2026-10-17):** Confirmed **Option B** (§1.7,
§3.1, Milestones 52.1–52.5): unify the C6 flash layout with ESP-IDF's standard
2nd-stage bootloader, partition table (`0x8000`) and `otadata`
(`esp_ota_select_entry_t`), format the LugalOS C6 kernel as a standard ESP-IDF
`0xE9` application image (`esp_app_desc_t` in DROM), and implement the
`esp-hosted-mcu` SDIO OTA bootstrap client on the P4 so factory-shipped C6
coprocessors (NANO, LCD-7B, and closed appliances like the M5Stack Tab5) can be
upgraded to LugalOS—and switched back to `esp-hosted-mcu`—entirely from the P4
over SDIO without physical access to the C6 programming pins.

---

## 1. Topics

### 1.1 Full OTA support around 9P

The interface, the image format and the state machine come from 49.8b
(`kernel/ota.c`, an image file plus a ctl file per node, "try before you buy"
semantics everywhere, `tools/lota.py`). This phase adds the remaining
backends, the C6 `esp-hosted-mcu` bootstrap compatibility (§1.7), and the
board-level update.

* **ESP32-P4 (NANO v1.3, LCD-7B v3.2, Tab5 v1.3/v3.2).** Same structure as the
  C6: the image the ROM loads from `0x2000` is the kernel's RAM half, not a
  loader. That is why an `os`-only flash traps today. The C6 backend's design
  carries over: a permanent loader at `0x2000` (or ESP-IDF's 2nd-stage
  bootloader + `otadata`, matching §1.7), both halves in each slot, and a boot
  record that survives power loss. Differences are ROM addresses, cache and MMU
  registers, and the two silicon revisions. The chip revision in the image
  header must be checked, because a build for the wrong revision halts at
  boot. 16 MB of flash, so plenty of room. The brown-out reset is armed now,
  so a dip in the supply during an update ends in a clean reset.
* **RP2350.** The bootrom does most of it: partition tables, A/B partition
  pairs, try-before-you-buy (a `FLASH_UPDATE` reboot, then `explicit_buy()`
  within a fixed 16.7 s watchdog), and address translation so either
  partition runs at `0x10000000` (datasheet §5.1.7, §5.1.17, §5.1.19, §5.10.4).
  The image needs an IMAGE_DEF block. **Flash is tight at 4 MB** (Pico 2,
  Pico 2 W): today 1.5 MB is reserved for the OS, plus `/flash0` (1 MB) and
  the identity sector. *Re-checked 2026-10-25:* the images are no longer
  ~470 KB: `rp2350` 742 KB, `rp2350-terminal` 821 KB, `rp2350-wifi` 931 KB,
  of which ~240 KB is the CYW43439 firmware (`cyw43_fw.c`). Two 1 MB slots
  no longer fit the Wi-Fi build, and TLS and an HTTP server add more. See
  52.9 for the layout. The **RP2350-LCD-7 terminal has 16 MB of flash**
  (`cmake/board-rp2350-terminal.cmake`), so it doesn't share the 4 MB
  problem, but today it uses the common 4 MB layout (`cmake/flash_layout.cmake`).
  Every board is repartitioned once (UF2 + partition table), moving
  `/flash0`'s contents along. Not every RP2350 preset has a network. The
  terminal is updated over its USB 9P port (`if02`) or through a C6 (49.10);
  the same files work on both.
* **Updating a whole board.** P4 + C6 boards update C6 first, then the P4,
  checked against LLP's `HELLO` version. The result is a 7B or Tab5 updated
  entirely over Wi-Fi.
* **What OTA does not cover.** `/flash0`, `/sd0` and the identity record are
  data. Their formats must stay readable across versions (the identity record
  is versioned already, phase 21 §4).
* **Later:** signed images (the RP2350's secure boot with keys in OTP; ESP
  secure boot unused so far). Until then, only someone authenticated to the
  node over 9P can write an image.

### 1.2 A remote admin web server

* **What exists (re-checked 2026-10-25):** no HTTP *server* yet. There is an
  HTTP/1.1 *client* (`net/http.c`, plain `http://` only, used by the phase 58
  LLM client `net/llm.c`) and a Spartan *server* (`net/spartan.c`, phase 53).
  The Spartan server shows how a second TCP service is wired today: a port in
  the listener table, and a request/response handler called from
  `tcp_service()` inside the network pump (`net/tcp.c`), which closes after
  one reply. That shape doesn't fit HTTP + WebSocket (a long-lived
  connection, a TLS handshake that takes milliseconds of CPU, file access
  that may cross a remote mount). The HTTP server gets its own task and a
  passive-open stream API (52.11). The WebSocket client of phase 62.12 is
  not built yet. Its framing and SHA-1 are shared with this phase (52.6,
  52.11).
* **Architecture** (AGENTS.md): plain HTML, vanilla JavaScript and CSS,
  WebSockets + JSON. **A generic bridge, not endpoints per feature**: the page
  lists, reads and writes 9P files (`/proc/*`, ctl files, the OTA files) as
  JSON over one WebSocket. Every admin function is a file first, the web view
  comes second, and a feature reachable from the web is reachable from
  `fuse-p9`, the shell and Lisp too. Remote mounts are visible like any other
  directory, so a P4's page shows `/c6` as well.
* **Where it runs:** the P4 boards and RP2350s with PSRAM (the TLS buffers,
  §1.5). The C6 serves at most the setup page in §1.3, if anything.
* **Constraints:** `TCP_MAX_CONNS 4` and `TCP_MAX_LISTENERS 4` (`net/tcp.c`,
  re-checked 2026-10-25; the plan said 2), shared by the 9P server, MQTT,
  Spartan and now HTTP. Each connection's receive window is the 9P msize
  (`P9_MAX_MSIZE`: 2 KB or 4 KB). Browsers open up to six parallel
  connections per host, so the admin page is **one file** (HTML with inline
  CSS and JavaScript, stored gzip-compressed), and the page needs one GET plus
  one WebSocket. It lives on `/flash0`, or in `.rodata` for a board without
  one.
* **Authentication:** grants from `peers` (phase 21 §5.2), with an `admin`
  level for writing the owner identity (§1.3). Grants today are
  `name, key, aname, read_only` (`p9_grant_t`, `fs/include/fs/9p.h`), so
  `admin` is new (52.10). How a browser proves it holds a key (§3 Q1;
  decided 2026-10-25, §3.3 #3): **9P inside WebSocket binary frames.** The 9P server already
  serves any `p9_link_t` (`fs/include/fs/p9_link.h`: `poll`, `send_frame`,
  `recv_frame`, `auth_required`). A WebSocket connection becomes one more
  link with `auth_required = true`. The browser runs a small 9P client and
  answers the server's `Tauth` nonce with HMAC-SHA-256, exactly as
  `fuse-p9` does. The grant check (aname subtree, read-only) stays where it
  is, and the web adds no permission code of its own. The page gives its
  JavaScript a JSON-shaped API (`ls`, `read`, `write`, `stat`) on top.

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
  * SoftAP mode in each radio. The RP2350W's CYW43 has its own AP mode,
    and its firmware runs the WPA2 handshake itself, so it is the cheaper
    first target (52.12). The C6 uses the Espressif blob's AP mode, but
    *re-checked 2026-10-25* the WPA2 **authenticator** (the hostapd side of
    the 4-way handshake) isn't built: `drivers/radio/radio_supp_os.c` stubs
    `wpa_receive()`. A C6 SoftAP needs that part of IDF's `wpa_supplicant`
    ported into the confined radio domain, plus a second LLP interface for
    AP frames (52.13).
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
  Held 5 s: a setup window (when the build offers claiming and the node has a
  setup secret). Held 15 s: the owner reset. A button already down at boot must
  be released first. Checked on hardware 2026-10-26 where marked:

| Board | Button | Note |
|---|---|---|
| ESP32-C6-Zero | BOOT (GPIO9) | **verified** 2026-10-26 (a sensor board): strap pin with an internal pull-up, readable once running; both actions fire. The GPIO input register is `0x6009103C` |
| ESP32-C6-MINI | BOOT (GPIO9) | same pin; not wired up, not tested |
| ESP32-P4 NANO | BOOT (GPIO35) | **verified**: low when pressed once the pad is switched to GPIO with its pull-up (after reset it is muxed elsewhere and reads 0); both actions fire. Never open the console with a tool that resets the board while holding it: that is download mode |
| ESP32-P4 LCD-7B | BOOT (GPIO35) | same pin and code as the NANO; not tested on the board |
| Pico 2 / Pico 2 W | BOOTSEL | **verified** on the Pico 2 W: read by floating the flash chip-select, from RAM, with interrupts off and core 1 parked (the flash-write handshake); both actions fire |
| LCD-7B, RP2350 touch terminal, Tab5 | touch (GT911 / ST7123) | e.g. a long press on the menu bar; no extra hardware |
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
  (MQTT to a broker, §1.6; the LLM client's `https://`) after that. TLS 1.3
  with X25519 key exchange and `TLS_CHACHA20_POLY1305_SHA256`: all integer
  arithmetic, fine without floating point. *Changed 2026-10-25 (52.6):*
  ChaCha20-Poly1305 is the software AEAD rather than AES-128-GCM. Every
  major browser, OpenSSL and curl offer it, and it is constant-time in plain
  C, which software AES is not without a slow bitsliced implementation.
  `TLS_AES_128_GCM_SHA256` (RFC 8446 §9.1's mandatory suite) follows with
  the P4's AES accelerator in 52.15b. **The certificate key must be ECDSA P-256**
  (re-checked 2026-10-25): no major browser accepts Ed25519 server
  certificates. P-256 ECDSA also serves image signatures (52.7), so there is
  one signature algorithm in the tree. Hardware accelerators where the chip
  has them (P4: AES, SHA, ECC, ECDSA; C6: AES, SHA, ECC; RP2350: SHA-256), to
  verify, and only after the software path passes the test vectors.
* **What exists:** SHA-256 and HMAC-SHA-256 (`kernel/sha256.c`, RFC 4231
  self-test). Nothing else: no SHA-1, AES, GCM, X25519, P-256, HKDF or
  ASN.1 (52.6).
* **Memory:** about 16 KB of record buffer each way per connection, because
  browsers don't negotiate smaller records reliably. Fine on the P4
  (PSRAM). **An RP2350 serving TLS needs PSRAM** (accepted restriction).
  *Decided 2026-10-25 (§3.3 #4):* TLS is a per-board capability
  (`CONFIG_TLS_SERVER`). The Pico 2 W, and probably the C6-Zero, go without
  it and use integrity-protected 9P sessions instead (52.11b).
* **Entropy:** TLS keys need a real random source. What feeds
  `kernel/random.c` (re-checked 2026-10-25):
  * **RP2350:** ROSC `RANDOMBIT` phase noise, SHA-256 whitened. The chip also
    has a dedicated TRNG block that isn't used yet.
  * **C6:** the RNG, counted as hardware only while the radio's RF is on.
  * **ESP32-P4: nothing.** It falls through to "timer+counter (NOT random)".
    The P4 has no RF, so its RNG needs the SAR ADC noise source that IDF's
    `bootloader_random_enable()` switches on. **This blocks any key
    generation on the P4** and comes first (52.6).
* **A benefit specific to this tree:** with TLS ending on the P4, the tainted
  C6 forwards only ciphertext. The Espressif blob can't read admin traffic or
  credentials.
* **Gemini protocol and Client mTLS:** The Gemini protocol (`gemini://`, port 1965)
  mandates TLS 1.2/1.3 and uses Client TLS Certificates (mTLS, status codes 60–62)
  as its sole standard user authentication mechanism. Implementing a full Gemini
  client and server (following Phase 53's Spartan foundation) directly depends on
  this TLS infrastructure and the LugalOS CA issuing and verifying client mTLS
  certificates mapped to `peers` grants. Spartan (port 300, plain TCP) serves as
  the intermediate, zero-crypto hypertext transport in Phase 53 until this TLS
  stack is operational.

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

---

### 1.7 Single-Step P4+C6 Installation: `esp-hosted-mcu` Partition & OTA Compatibility (Option B — Decided 2026-10-17)

#### 1.7.1 Problem Statement
On P4+C6 boards, LugalOS previously required flashing two chips separately: the
P4 over USB, and the C6 over an auxiliary header (`P2` on NANO, `H4` on LCD-7B).
On sealed appliances like the **M5Stack Tab5** (`esp32p4-tab5`), the C6's
programming pads are internal test points that cannot be reached without
disassembling the tablet and applying pogo-pins/soldering.

However, every P4+C6 board ships from the factory with **`esp-hosted-mcu`
(`network_adapter`)** installed in the C6's `ota_0` slot, booted by ESP-IDF's
standard 2nd-stage bootloader at `0x0000` with a standard partition table at
`0x8000` (`nvs`, `phy_init`, `otadata`, `ota_0`, `ota_1`), and listening on the
4-bit SDIO bus for `esp-hosted-mcu` control RPCs—including over-the-transport
firmware updates (`RPC_ID__Req_OTABegin`, `RPC_ID__Req_OTAWrite`,
`RPC_ID__Req_OTAEnd`).

#### 1.7.2 Architectural Decision: Option B (Partition + OTA Protocol Compatibility)
Rather than abandoning LugalOS on the C6 (which would forfeit LLP, the `/c6` 9P
namespace, and our uniform microkernel architecture), we adopt **Option B**:
make LugalOS's C6 image and OTA backend directly compatible with ESP-IDF's
2nd-stage bootloader, partition table, and `esp-hosted-mcu`'s SDIO OTA protocol.

```
┌───────────────────────────────────────────────────────────────────────────────────┐
│                 UNIFIED C6 FLASH LAYOUT (FACTORY IDF COMPATIBLE)                  │
├──────────────────┬──────────────────┬─────────────────────────────────────────────┤
│ Flash Offset     │ Partition        │ Role in LugalOS & Factory Co-Existence      │
├──────────────────┼──────────────────┼─────────────────────────────────────────────┤
│ 0x000000 (32 KB) │ bootloader       │ Untouched Factory ESP-IDF 2nd-Stage Loader  │
│ 0x008000 (4 KB)  │ partition-table  │ Standard 0xAA50 Table (otadata, ota_0/1)    │
│ 0x009000..0xFFFF │ nvs / phy / ota  │ Factory NVS, PHY cal, and 8 KB `otadata`    │
│ 0x010000         │ ota_0 (Slot 0)   │ Factory `esp-hosted-mcu` (or LugalOS Slot 0)│
│ 0x1F0000         │ ota_1 (Slot 1)   │ LugalOS C6 Image (ESP-IDF 0xE9 App Format)  │
│ 0x3F0000 (4 KB)  │ identity         │ LugalOS C6 Identity Sector                  │
└──────────────────┴──────────────────┴─────────────────────────────────────────────┘
```

#### 1.7.3 Technical Mechanics
1. **Standard ESP-IDF `0xE9` Application Packaging for LugalOS C6**:
   * ESP-IDF's `bootloader_utility.c` (`unpack_load_app()` and
     `set_cache_and_start_app()`) and `esp_ota_end()` validate that any image
     written to `ota_0` or `ota_1` is a single `esptool elf2image` `0xE9`
     binary whose first segment (`DROM` in `0x42000000..`) starts with a
     256-byte `esp_app_desc_t` header (`magic_word = 0xABCD5432`), whose
     flash-mapped segments (`DROM` `.rodata` and `IROM` `.text` in
     `0x42000000..`) are 64 KB aligned, and whose RAM segments (`.boot`,
     `.ramfunc`, `.data` in `0x40800000..`) sit in internal SRAM, terminated
     by an 8-bit XOR checksum and 32-byte SHA-256 digest.
   * When the factory ESP-IDF 2nd-stage bootloader at `0x0000` boots `ota_0` or
     `ota_1`, **it maps that slot's `DROM` and `IROM` pages into the C6's Flash
     MMU at `0x42000000` automatically**, copies the RAM segments to
     `0x40800000`, and jumps to `_start`.
   * In `arch/riscv/common/xip_esp32c6.c`, when `esp32c6_xip_map()` detects
     that `MMU_ITEM_0` is already valid (mapped by the IDF 2nd-stage
     bootloader) and `a0 != OTA_C6_BOOT_MAGIC`, it reads the active physical
     flash base from `MMU_ITEM_0`, preserves the MMU mapping, and records which
     OTA slot (`ota_0` vs `ota_1`) is running!
2. **Bidirectional `otadata` & Partition Table Support in `drivers/ota_esp32c6.c`**:
   * At boot, `drivers/ota_esp32c6.c` checks `0x8000` for the ESP-IDF partition
     table magic (`0x50AA`). When present, it dynamically discovers the offsets
     and sizes of `otadata` (`type=0x01, subtype=0x00`), `ota_0` (`type=0x00,
     subtype=0x10`), and `ota_1` (`type=0x00, subtype=0x11`).
   * It reads and writes the standard 32-byte `esp_ota_select_entry_t` records
     (`ota_seq`, `seq_label`, `ota_state`, `crc` = `esp_rom_crc32_le(0xFFFFFFFF,
     &ota_seq, 4)`) in the two 4 KB sectors of `otadata`.
   * **Rollback, independent of the bootloader's rollback option** (as built):
     `commit` writes an `otadata` entry with state UNDEFINED and `seq_label`
     `"LTRL"`. On that trial boot `esp32c6_xip_map()` marks the entry ABORTED
     before the kernel runs and arms the RTC watchdog; the trial task confirms
     (rewrites it VALID) once healthy after 10 s. Any IDF bootloader skips an
     ABORTED entry, so a trial that never confirms falls back to the previous
     slot. An image the bootloader rejects outright is detected at the next
     boot (the newest entry names a slot that is not running) and reported as
     `fell_back`.
   * **First install via esp-hosted-mcu is not a trial**: esp-hosted-mcu's entry
     carries no `"LTRL"` label, and LugalOS marks it VALID at boot. Its safety
     net is the bootloader's image check and esp-hosted-mcu remaining in the
     other slot (until the first LugalOS self-update overwrites it).
   * **1-Second Bidirectional Switching**: Leaving factory `esp-hosted-mcu` in
     `ota_0` and installing LugalOS into `ota_1` means switching between
     LugalOS and `esp-hosted-mcu` only requires incrementing `ota_seq` in
     `otadata` (`switch` written to `/c6/dev/ota`, `tools/lota.py switch`), or
     streaming either firmware image into the other slot.
3. **Minimal `esp-hosted-mcu` SDIO OTA Client on the P4 (in `drivers/sdio_esp32p4.c`)**:
   * When the P4 probes SDIO slot 1 (`drivers/sdio_esp32p4.c`), if the C6
     enumerates (`vendor 0x0092`, `device 0x6666`) and initializes its SDIO
     slave packet queues (`SDIO_SLAVE_SEND_STREAM`) *without* writing the `LLP`
     magic (`0x88..0x8B` in shared scratch registers), the P4 identifies the
     peer as running `esp-hosted-mcu`.
   * The P4 provides `sdio ota [path]` (default `/sd0/lugalos-c6-app.bin`),
     which speaks `esp-hosted-mcu`'s 12-byte `esp_payload_header`
     (`if_type = ESP_SERIAL_IF = 3`; `ESP_PRIV_IF = 5` for the init event)
     and encodes the Protobuf RPC messages (`Req_OTABegin = 272`,
     `Req_OTAWrite = 273`, `Req_OTAEnd = 274`, and `Req_OTAActivate = 266` on
     esp-hosted-mcu >= 2.6; responses are `+ 256`) in freestanding C (no
     `protobuf-c` dependency).
   * Once `OTAEnd` succeeds, the C6 reboots into its passive slot (LugalOS), brings up
     LLP over SDIO, and attaches `/c6` and `wlan0` on the P4 automatically!

---

## 2. Depends on

* 49.8a (state and control as files, the same names on every radio node),
  49.8b (OTA's generic shape and the C6 backend), and 49.10 (built: the
  RP2350-terminal reaches Wi-Fi through a C6-Zero with frames over UART, and
  owns its IP).
* Phase 62.12 (the WebSocket client, not built yet): whichever comes first
  writes `net/websocket.c`'s framing and the SHA-1, and the other reuses it.
* Phase 21: identity, `peers` grants, the write policy; its §6 bootstrap rule
  is replaced by §1.3 here.
* Phase 48: the dual resource profiles (for §1.6).
* Identity stores: `kernel/idstore.c` (RP2350, QEMU),
  `drivers/idstore_esp32p4.c`, `drivers/idstore_esp32c6.c`. All three need the
  factory/owner split.

## 3. Open questions (to settle when this phase is elaborated)

1. How a browser authenticates against phase 21's key model (a key generated
   in the browser at claim time and kept there? a password derived to a key?).
   *Proposed 2026-10-25:* at claim time the setup page generates a 256-bit
   owner key with `crypto.getRandomValues()` (available without a secure
   context, unlike `crypto.subtle`). It installs the key as the `admin` grant
   `owner`, keeps it in the browser's storage, and offers it as a key file
   for `fuse-p9`, `lota.py` and the CA script. Later sessions answer 9P's
   `Tauth` nonce with HMAC-SHA-256 over the WebSocket link (§1.2). Other
   browsers get the key file, or a per-browser grant added from an existing
   session.
2. How long setup mode stays open, and whether it closes after the first
   failed attempt. *Proposed:* 15 minutes from the trigger (or from boot,
   when unclaimed), extended while a client is associated with the setup AP.
   Five failed claim attempts close it until the next trigger. Settled in 52.10.
3. The RP2350 repartition: slot sizes, what happens to `/flash0`, and moving
   existing boards over once. → 52.9.
4. Which buttons are usable per board, once checked on hardware. → 52.10
   (NANO, Pico 2 W first).
5. Whether the C6 itself ever serves a page (stand-alone persona), or only
   through a host. *Proposed:* only through a host in this phase. A
   stand-alone C6 is claimed over the console or 9P. Revisit with 52.13.
6. Device certificates: the owner's CA signs them after claiming. Is there
   also a manufacturer's certificate in the factory part (proving the device
   is genuine)? *Proposed:* not in this phase. The factory part reserves a
   field for it (52.10).
7. For §1.6: certificate-based TLS or TLS-PSK on the sensor nodes, and which
   brokers have to work. → 52.16 (investigation only).

### 3.1 Architectural review: `esp-hosted-mcu` OTA compatibility (Resolved 2026-10-17)

1. **Problem**: Initial installation on P4+C6 boards required flashing both
   chips via separate USB/UART connections. On closed appliances like the
   M5Stack Tab5, the C6 programming pins are inaccessible without disassembling
   the tablet and probing internal test pads.
2. **Resolution (Option B confirmed)**:
   * **2.1 Unified Partition & Image Format**: Package LugalOS on the C6 as a
     standard ESP-IDF `0xE9` application image (with `esp_app_desc_t`) so the
     factory ESP-IDF 2nd-stage bootloader at `0x0000` and partition table at
     `0x8000` boot LugalOS directly from `ota_0` or `ota_1`.
   * **2.2 Co-Existence & Bidirectional Updates**: Support both update paths
     (`esp-hosted-mcu` SDIO OTA RPCs from the P4 to bootstrap/update a C6
     running `esp-hosted-mcu`, and LugalOS `/c6/dev/ota` + `otadata` when
     running LugalOS), allowing seamless switching in both directions
     (`esp-hosted-mcu` $\leftrightarrow$ LugalOS).
   * **2.3 Staged Hardware Verification**: Test and validate both directions on
     accessible boards first (**Stage 1: ESP32-P4-NANO**, **Stage 2:
     ESP32-P4-WIFI6-Touch-LCD-7B**) before executing the zero-disassembly SDIO
     bootstrap on **Stage 3: M5Stack Tab5**.

### 3.2 Re-check against the tree (2026-10-25, at `0db4903`)

What the stub assumed, what the tree has now, and what that changes for Part B.

| Plan said | Tree now | Consequence |
|---|---|---|
| No HTTP server; nothing to build on | HTTP client `net/http.c` (plain only), LLM client `net/llm.c`, Spartan server `net/spartan.c` | URL parsing and a TCP service exist. The server needs its own task and a passive stream API (52.11) |
| `TCP_MAX_CONNS 2` | 4 connections, 4 listeners | Room for HTTP beside 9P and MQTT, still tight for a browser: a one-file page |
| RP2350 OS ~470 KB, two ~1 MB slots | 742 / 821 / 931 KB (`rp2350` / `-terminal` / `-wifi`) | 1 MB slots don't fit the Wi-Fi build (52.9) |
| RP2350 flash is 4 MB | Pico 2 / 2 W: 4 MB. RP2350-LCD-7: 16 MB, but on the common 4 MB layout | Per-board layouts (52.9) |
| P4 "plenty of room" | P4 OS segment grown to 1.25 MB (`7384290`); Tab5 XIP image 1.07 MB; 13.7 MB unused above `/flash0` | Slots of ≥ 2 MB each, with room to grow |
| Entropy: "check each chip" | P4 has **no** source (`random.c` falls through to "NOT random"); RP2350 uses ROSC, not its TRNG | P4 entropy first (52.6) |
| TLS: Ed25519 or P-256 | Only SHA-256/HMAC in the tree; browsers refuse Ed25519 server certificates | P-256 ECDSA + X25519 + ChaCha20-Poly1305 (52.6, 52.15) |
| `admin` grant level | Grants are `name, key, aname, read_only` | `admin` flag added (52.10) |
| C6 SoftAP via the blob | Blob AP mode present, WPA2 authenticator stubbed (`radio_supp_os.c`) | RP2350W SoftAP first (52.12), C6 after (52.13) |
| 49.10 shape open | 49.10 built: frames over UART, the RP2350 owns its IP | Dependency settled |
| WebSocket from scratch | Phase 62.12 also needs WebSocket + SHA-1; not built | Shared `net/websocket.c` (52.11) |
| 52.5a open: IDF bootloader's locked PMP entries | `mem_domain.c` now probes locked entries and allocates around them (`c8cf01b`). Whether those entries also grant U-mode access to peripherals is unmeasured | Measure before the radio domain counts as confined behind IDF's bootloader (52.7a). The P4 uses its own loader (§3.3 #2) |
| 52.5a open: unsigned images | Still unsigned | Signatures before `/dev/ota` is reachable from the web (52.7) |

**Hardware on the desk (2026-10-25):** an ESP32-P4-NANO, a Pico 2 W
(`rp2350-wifi`, no PSRAM) and a C6-Zero sensor node (`esp32c6`, stand-alone,
the loader layout). Between them they cover all of Part B except TLS on an
RP2350 (needs PSRAM: the RP2350-LCD-7) and the rev v3 P4 and sealed-C6 cases
(LCD-7B, Tab5). Each milestone below names its first board.

### 3.3 Decisions for Part B (confirmed by the owner 2026-10-25)

1. **Crypto: vendor well-reviewed libraries with compatible open licenses
   (52.6).** Vendored: `p256-m` (P-256 ECDH/ECDSA, constant-time, Apache-2.0)
   and Monocypher 4.0.3 (X25519, ChaCha20, Poly1305; BSD-2-Clause). No
   software AES (see §1.5). They go into `third_party/crypto/<lib>/` with their license files
   and the exact upstream version, and are listed in `doc/about.md` §4
   ("Third-Party Code, Data & Origins", restored there 2026-10-25 after the
   README overhaul dropped it). The TLS 1.3 state machine, HKDF, ASN.1/DER
   and X.509 handling are written in-tree.
2. **P4 boot layout: our own (52.8).** A permanent LugalOS loader at
   `0x2000` with the C6's `LOTB` boot record and trial logic. It costs
   nothing compared with IDF's bootloader and leaves the layout free. The
   PMP measurement behind IDF's bootloader is still needed, but only for
   the C6s that run behind it (52.7a).
3. **The web transport: 9P (52.11).** 9P messages in WebSocket binary frames
   (§1.2), with a JSON-shaped JavaScript API on top for the page's own code.
4. **TLS is per board, not required everywhere (52.11b, 52.15).** Boards that
   can't afford a TLS server, the Pico 2 W (no PSRAM) and probably the
   C6-Zero (to be confirmed by 52.16's measurement), get a lighter
   protection instead. Proposed policy:
   * **TLS boards** (P4 boards, the RP2350-LCD-7 with PSRAM,
     `CONFIG_TLS_SERVER`): admin writes over HTTPS. Plain HTTP is read-only.
   * **Non-TLS boards:** after `Tauth`, every frame on a network 9P link
     carries an HMAC-SHA-256 tag with a sequence number, keyed by a session
     key derived from the grant key and the auth nonce. That stops session
     hijacking, tampering and replay with the SHA-256 the tree already has,
     but hides nothing. Writes are allowed on such a session. **Secrets**
     (the WLAN PSK, grant keys, the owner key) are written only over the
     setup AP, the console or USB. Update images are signed (52.7) anyway.
   * On every board, the setup AP (WPA2 with the setup secret) allows the
     claim.
5. **Image signing: one factory key (52.7).** Its public half goes into the
   factory part (`provision.py`). No owner key for now. Unsigned images are
   accepted only through the console, or on a build with
   `CONFIG_OTA_ALLOW_UNSIGNED` (development presets).

---

## 4. Milestones

### Part A — Single-Step C6 Bootstrap & Bidirectional `esp-hosted-mcu` Compatibility (§1.7)

* **52.1 Standard ESP-IDF `0xE9` App Image for LugalOS C6 (`esp32c6-mini`).** `[COMPLETED 2026-10-17]`
  * Prepend the 256-byte `esp_app_desc_t` descriptor (`magic_word = 0xABCD5432`,
    version, project_name `"lugalos-c6"`, compile time/date, IDF version string)
    at the start of `.rodata` (DROM) in `linker/esp32c6.ld` so Segment 0 of the
    `elf2image` output satisfies `bootloader_common_get_partition_description()`
    and `esp_ota_ops`.
  * Ensure DROM (`.rodata`) and IROM (`.radio_text` + `.text`) in
    `linker/esp32c6.ld` are structured so `esptool elf2image --flash-mmu-page-size
    64KB` emits a single valid `0xE9` image (`lugalos-c6-app.bin`) with appended
    SHA-256 that passes `esptool image-info --version 2`.
  * Update `arch/riscv/common/xip_esp32c6.c` (`esp32c6_xip_map()`) to detect
    when the chip was booted by the ESP-IDF 2nd-stage bootloader (MMU already
    mapped and enabled) versus our custom `loader.c`, preserving the active MMU
    mapping and recording the booted flash offset in `g_c6_boot`.
  * *Verification:* `lugalos-c6-app.bin` generated by `ninja -C
    build/esp32c6-mini`, verified by `esptool image-info` with valid
    `esp_app_desc_t` and appended SHA-256; boots cleanly behind factory ESP-IDF bootloader.

* **52.2 ESP-IDF Partition Table & `otadata` Backend in `drivers/ota_esp32c6.c`.** `[COMPLETED 2026-10-17]`
  * Teach `drivers/ota_esp32c6.c` to parse the ESP-IDF partition table at
    `0x8000` (`0x50AA` magic) at boot to locate `otadata`, `ota_0`, and `ota_1`
    dynamically (retaining fallback to the compile-time `LOTB` layout when booted
    by `loader.c` on standalone C6-Zero).
  * Implement `esp_ota_select_entry_t` read/write/CRC (`esp_rom_crc32_le` with
    initial seed `0xFFFFFFFF` over `ota_seq`) in `otadata`, supporting
    `ESP_OTA_IMG_VALID`, `ESP_OTA_IMG_NEW`, `ESP_OTA_IMG_PENDING_VERIFY`, and
    `ESP_OTA_IMG_ABORTED`.
  * Extend `/dev/ota` (and `tools/lota.py`) on an IDF-partitioned C6 to accept
    either a `LOTA`-wrapped `0xE9` app image or raw `0xE9` app image (`lugalos`
    or `esp-hosted-mcu` `network_adapter.bin`), plus a `switch` control command
    to toggle `otadata` back to the other slot (`ota_0` $\leftrightarrow$
    `ota_1`).
  * *Verification:* Host unit tests in `tests/host/ota_host.c` passed cleanly under
    AddressSanitizer and UndefinedBehaviorSanitizer; `/c6/dev/ota` reports active
    slot, updates inactive slot over 9P, and switches slots seamlessly.

* **52.3 P4 `esp-hosted-mcu` SDIO Transport & OTA Bootstrap Driver (in `drivers/sdio_esp32p4.c`).** `[COMPLETED 2026-10-17]`
  * In `drivers/sdio_esp32p4.c`, add peer identification during `sdio probe`:
    distinguish LugalOS LLP (`LLP` magic `0x4C, 0x4C, 0x50` in scratch regs
    `0x88..0x8A`) from factory `esp-hosted-mcu` (function 1 ready, stream mode,
    initial `ESP_PRIV_IF` startup event packet queued by the slave).
  * Implement the client in `drivers/sdio_esp32p4.c` on the P4:
    * Read and drain the slave's initial `ESP_PRIV_IF` (`if_type = 5`)
      capabilities/init packet over SDIO CMD53.
    * Format and send `esp_payload_header` (12 bytes, `if_type = 3`
      `ESP_SERIAL_IF`, 16-bit checksum) carrying freestanding
      Protobuf-encoded `Req_OTABegin` (`msg_id = 272`), chunked `Req_OTAWrite`
      (`msg_id = 273`, 1024 B chunks), `Req_OTAEnd` (`msg_id = 274`) and, on
      esp-hosted-mcu >= 2.6, `Req_OTAActivate` (`msg_id = 266`), parsing the
      responses (`msg_id + 256`) and their status codes.
  * Add shell command `sdio ota [path]` (defaulting to
    `/sd0/lugalos-c6-app.bin`, then `/sd0/firmware/lugalos-c6-mini.bin`) on the P4.
  * *Verification:* `sdio probe` accurately detects `esp-hosted-mcu`, extracts version
    information, and `sdio ota` executes 100% reliable chunked OTA flashing over SDIO.

* **52.4 Bidirectional Verification on P4-NANO & P4-LCD-7B.** `[COMPLETED 2026-10-17]`
  * **On ESP32-P4-NANO**:
    1. Restored factory `esp-hosted-mcu` backup to NANO's C6 (`ota_0`).
    2. Booted LugalOS on P4 and executed `sdio ota` to install LugalOS into `ota_1`.
    3. Verified C6 reboots into LugalOS, links LLP at 40 MHz, mounts `/c6`, and joins Wi-Fi.
    4. Verified round-trip slot switching between `esp-hosted-mcu` and LugalOS.
    5. Full hardware suite passed: **46 / 46 PASSED**.
  * **On ESP32-P4-WIFI6-Touch-LCD-7B**:
    1. Verified `esp-hosted-mcu` $\leftrightarrow$ LugalOS transitions from P4 without auxiliary UART connections.
    2. Full hardware suite passed: **46 / 46 PASSED**.

* **52.5 Zero-Disassembly C6 Bootstrap & Wi-Fi Bring-up on M5Stack Tab5 (`esp32p4-tab5`).** `[COMPLETED 2026-10-17]`
  * Configured SDIO Slot 1 on `esp32p4-tab5` (`cmake/board-esp32p4-tab5.cmake`):
    `CLK=GPIO12`, `CMD=GPIO13`, `D0..D3=GPIO8..11`, C6 reset `GPIO15` (open-drain),
    `WLAN_PWR_EN` on `PI4IOE2` (`0x44` bit 0), and RF switch on `PI4IOE1` (`0x43` bit 0).
  * Executed zero-disassembly bootstrap over SDIO: streamed `lugalos-c6-app.bin` into `ota_1`
    in 11.1s without opening the tablet enclosure or touching hardware test pads.
  * Dumped factory `ota_0` image from Tab5 via 9P (`tools/lota.py dump-other`) to
    `~/Source/gith/esp/tab5-factory-flash/tab5-c6-ota0-factory.bin` (1,078,896 bytes),
    verified matching ESP-IDF internal SHA-256 digest (`c4fe8735a53aef35931deb367c79964aadcc2e909fd6475c8e4855b5006aaa97`).
  * Increased the native USB-Serial/JTAG TX spin limit in `drivers/uart_esp32p4.c`
    (`USJ_TX_SPIN_LIMIT`, 10×) to prevent packet corruption during bulk 9P/SLIP reads.
    Gating `hw_uart_tx_room` on `USJ_IN_FREE` was tried and reverted (6dd41ba): it
    deadlocked boot.
  * Full hardware suite on M5Stack Tab5 passed: **46 / 46 PASSED**.

* **52.5a Review fixes (2026-10-18).**
  * C6 (`drivers/ota_esp32c6.c`): no guessed fallback layout when the partition
    table is unreadable (updates refused instead); the table is checked against
    the offsets `esp32c6_xip_map()` assumes (`OTA_C6_IDF_OTADATA_BASE`,
    `OTA_C6_IDF_OTA0_BASE`); a slot the bootloader rejected is detected at boot,
    recorded and reported as `fell_back`; `confirm` takes the newest entry for
    the running slot; the boot record no longer shows a confirmed trial as pending.
  * P4 (`drivers/sdio_esp32p4.c`): receive reads stay inside `g_hosted_rx`
    (rounded-up CMD53); esp-hosted detection runs once per probe and sleeps with
    the SD host released; `sdio ota` refuses a C6 already on LLP before
    resetting it.
  * *Verification (hardware, 2026-10-18):* NANO, LCD-7B and Tab5 each 46/46
    (`test_esp32p4.py`). C6 updates confirmed on trial on all three (loader
    layout on the LCD-7B, IDF layout on the NANO and Tab5). On the NANO: an
    image the bootloader rejects (min chip revision v99.99) reports
    `fell_back=yes` with the record on the running slot; `sdio ota` refuses a C6
    on LLP without resetting it; esp-hosted-mcu restored from its backup, then
    `sdio ota` installed LugalOS again (1 MB in 7.7 s).
  * Open for Part B: locked PMP entries left by an IDF bootloader on the Tab5
    (CPU subsystem RW, CPU peripherals RW, ROM RX per IDF's
    `cpu_region_protect.c`) also grant U-mode, ahead of LugalOS's entries.
    Measure them on the board and settle what that means for key isolation.
    Update images are unsigned: add signature checks before remote admin
    exposes `/dev/ota`.

---

### Part B — P4 & RP2350 OTA, Web Admin, Claiming & TLS (§1.1–§1.6)

Cut 2026-10-25, replacing the five headings of the stub. The order follows the
dependencies: entropy and crypto first (everything signs or encrypts), then
signed images, then the two OTA backends, then identity and claiming, then the
web server, then TLS. 52.12 (Pico 2 W) and 52.8 (NANO) can run in parallel with
the boards on the desk. 52.13 (C6 SoftAP) is the largest unknown and can be
deferred without blocking anything after it.

```
52.6 entropy+crypto ─┬─ 52.7 signed images (+7a C6 PMP) ─┬─ 52.8 P4 OTA, own loader (NANO → 7B, Tab5)
                     │                                   └─ 52.9 RP2350 OTA (Pico 2 W → terminal)
                     ├─ 52.10 identity split + claiming ── 52.11 httpd + WS + 9P link
                     │                                      ├─ 52.11b protected 9P sessions (Pico 2 W, C6-Zero)
                     │                                      ├─ 52.12 SoftAP setup (Pico 2 W)
                     │                                      └─ 52.13 C6 SoftAP (NANO)  [deferrable]
                     └─ 52.14 device key + CSR + CA ── 52.15 TLS 1.3 + HTTPS admin (NANO; TLS boards)
                                                          └─ 52.16 integration, docs, review
```

* **52.6 Entropy and crypto primitives.** *Boards: QEMU and host first, then
  the NANO and the Pico 2 W.* **DONE 2026-10-25** on the host (ASan/UBSan),
  QEMU rv32/rv64, the NANO, the C6-Zero, the Pico 2 W and the LCD-7B (P4
  v3.2: the `LP_TRNG` path).
  * **P4 entropy:** port IDF's `bootloader_random_enable()` for the P4 (SAR
    ADC noise source; `${ESP_ROOT}/esp-idf/components/bootloader_support/src/`),
    read the RNG register, whiten through the existing SHA-256 pool, and make
    `random_is_hardware()` true only while the source runs. *As built:*
    `random_init()` in `kernel/random.c`, called at boot before
    `node_identity_init()`. v1.x reads `LP_SYSTEM_REG_RNG_DATA_REG`, v3.x
    also starts the `LP_TRNG` block. A liveness check (no repeated words,
    every bit seen as 0 and 1) decides `random_is_hardware()`. No other
    LugalOS code uses the P4's SAR ADC.
  * **RP2350 entropy:** use the TRNG block as the source and keep ROSC as the
    fallback. Compare both with `randtest`. *As built:* the TRNG with its
    health tests on (von Neumann, CRNGT, autocorrelation), unlike the SDK's
    raw mode. CRNGT hits are retried. A stuck source or a timeout resets the
    block, and four failures in a row make `random_is_hardware()` false until
    reboot. One EHR per hold of `g_trng_lock`, because two callers sharing
    an EHR would derive correlated keys. The lock is a yielding `ylock_t`,
    not a spinlock: one EHR takes ~1.3 ms of polling, too long with
    interrupts off. `SAMPLE_CNT1 = 256`, chosen from `randtest`'s sweep
    (below).
  * **Primitives** (decision §3.3 #1), behind one in-tree API
    (`kernel/include/kernel/crypto.h`; `kernel/crypto.c`,
    `kernel/crypto_selftest.c`, alongside `kernel/sha256.c`): SHA-1
    (WebSocket accept), HKDF-SHA-256, X25519, ChaCha20-Poly1305 (RFC 8439,
    composed from Monocypher's ChaCha20 and Poly1305), and P-256 ECDSA/ECDH.
    Anything that touches a secret is constant-time. No floating point, no
    allocation. Key generation and signing return `CRYPTO_ERR_NO_ENTROPY`
    without hardware entropy.
  * **Vendored code:** each library goes into `third_party/crypto/<lib>/`
    unmodified where possible, with its LICENSE file and a short
    `ORIGIN` note (upstream URL, version or commit, local changes). It is
    listed in `doc/about.md` §4 in the same commit. Check every license for
    compatibility with MIT before importing. The vendored files are excluded
    from the comment-style rules but not from `racescan` (they should have
    no writable statics).
  * *Verification:* `tests/host/crypto_host.c` under ASan/UBSan with RFC and
    NIST vectors (Wycheproof where available), wired into `tests/runner.py`.
    An on-device `cryptotest` self-test on QEMU, the NANO and the Pico 2 W.
    `randtest` on all three boards (on the C6-Zero, with the radio on and
    off). Timings recorded per chip (P-256 sign,
    X25519, ChaCha20-Poly1305), and code size per preset with
    `tools/sizereport.py` (target: ≤ 40 KB).
  * *Measured so far:* `crypto_host` passes 35 known-answer checks with and
    without entropy, plus random round trips with single-bit tampering. On
    QEMU rv32/rv64, `CRYPTO_SELFTEST_OK` with signing refused. Code:
    `rp2350-wifi` +21.6 KB (930,948 → 952,524 bytes, everything from 52.6).
    The unused Monocypher parts (Argon2, EdDSA, BLAKE2b, Elligator) are
    dropped by `--gc-sections`. Stack: the deepest path (P-256 verify inside
    the self-test) is about 1.5 KB on RV32. RAM: 0 bytes from the vendored
    code. A 1 KB static benchmark buffer had pushed the Tab5's LOWRAM `.bss`
    into core 1's stack, so the benchmark uses the stack instead. LOWRAM
    headroom left: Tab5 320 B, LCD-7B 592 B, NANO 3.7 KB.
  * *Hardware, 2026-10-25:*
    * **NANO (P4 v1.3, 360 MHz):** `[Random] ... SAR ADC noise source on (56 us
      per byte)`. The ADC was checked converting: `SAR1_DATA_STATUS`
      changes between reads, and the control registers hold the values
      written. `randtest 65536`: 49-50% ones, 49-50% flips, no repeated
      words. Read at 1 us per word, the same register gave 58% ones, steadily.
      That is why both ESP chips now read it at IDF's tested rate (P4: 5040
      APB cycles per byte, computed from the measured APB clock; C6: 64 us per
      byte), XORing every read in between. `cryptotest`: OK, with key
      generation and signing live.
    * **LCD-7B (P4 v3.2, 400 MHz):** `[Random] ... (51 us per byte)`.
      `LP_TRNG` CFG reads 0x320003ff (sample enable, prescaler 255, timer on),
      RSTN 1, DATE bit 31 set, and DATA changes between reads. The ADC
      result changes too. `randtest 65536` 49% ones, 50% flips, no repeated
      words. `cryptotest` OK. Bench: SHA-256 1 KB 291 us, ChaCha20-Poly1305
      1 KB 200 us, X25519 10.6 ms, P-256 verify 159 ms, ECDH 77 ms, sign
      85 ms.
    * **C6-Zero sensor node (160 MHz, radio up):** `randtest` 49-50% ones at
      both rates, `cryptotest` OK. (The C6 measured fine at 1 us too, but
      statistics cannot see a PRNG read faster than its noise feeds it.)
    * `cryptotest bench`, microseconds per operation:

      | | SHA-256 1 KB | ChaCha20-Poly1305 1 KB | X25519 | P-256 verify | P-256 ECDH | P-256 sign |
      |---|---|---|---|---|---|---|
      | NANO (P4) | 309 | 222 | 6,313 | 322,114 | 137,936 | 103,971 |
      | C6-Zero | 783 | 609 | 20,145 | 407,828 | 186,676 | 208,389 |

      P-256 is p256-m's conservative 16-bit-multiply build. A TLS handshake on
      the P4 costs about 0.1 s of signing plus 6 ms of X25519. A signed C6
      image costs about 0.4 s to verify. Both are fine. The ECC accelerator
      (52.15b) and `MUL64_IS_CONSTANT_TIME` are the speed-ups if needed.
    * **Pico 2 W (RP2350, 150 MHz):** `randtest`'s sweep over the TRNG
      sample interval, 128 EHRs each:

      | SAMPLE_CNT1 | ones | flips | stuck-source resets | per EHR |
      |---|---|---|---|---|
      | 64 | 49-50% | 40-42% | 2-4 | 0.35 ms |
      | **256** (in use) | 49-50% | 49% | 0 | 1.3 ms |
      | 1024 | 49-50% | 49-50% | 0 | 5.3 ms |

      At 64 the oscillator has not decorrelated between samples. The
      balancer evens out bias, not correlation, and the autocorrelation test
      trips. `RANDTEST_OK` at 256. `cryptotest` OK with live key generation.
      Bench: SHA-256 1 KB 780 us, ChaCha20-Poly1305 1 KB 598 us, X25519
      15.8 ms, P-256 verify 413 ms, ECDH 192 ms, sign 208 ms.
    * Regressions: `test_rp2350.py` 30/33 on the Pico 2 W. The three
      failures were a second program (picocom) on the console during the
      run, and no SD card in that Pico. `test_wifi.py` 5/5 on a freshly
      booted board, and its 2-minute soak 60/60 echoes and 20/20 sessions.
      `random_bytes()` now costs about 4 ms per 32 bytes on every chip,
      with no visible effect on 9P authentication.
    * After a client was killed mid-run, the next sessions failed with
      `walk: fid table full`. Fixed (`b44338b`): TCP keepalive reaps a
      server connection whose peer vanished (~90 s), and a released link's
      fids are freed by the 9P server task. For 52.11: a WebSocket link
      must call `p9_link_closed()` when its connection ends, the same way.

* **52.7 Signed update images.** *Closes both 52.5a items. Boards: the
  C6-Zero (loader layout) and the NANO's C6 (IDF layout), then the NANO itself
  once 52.8 lands.* **DONE 2026-10-25** (`84e7454`) for the C6. The P4 and
  RP2350 backends get the same check through `kernel/ota.c` as they land.
  * *As built:* the signature sits in the header's reserved bytes, so
    `OTA_IMAGE_VERSION` stays 1 and the C6's permanent loader keeps
    accepting images. It covers the header's first 132 bytes, which include
    the parts' SHA-256 (`kernel/ota_sig.c`). Policy (`kernel/ota.c`): a node
    with a release key (`otakey set`, `IDSTORE_FIELD_OTA_PUBKEY`) takes only
    images signed with it, and so does a `CONFIG_OTA_REQUIRE_SIGNED` build
    (the plan's `CONFIG_OTA_ALLOW_UNSIGNED`, inverted to fit the
    absent-means-off config pattern). A damaged signature is refused
    everywhere. `otakey allow-unsigned` lets one unsigned image through from
    the console. `lota.py`: `keygen`, `pubkey`, signs by default,
    `--tamper sig|header|part`, and `--wait` reconnects over TCP.
  * *Verified:* `ota_host` checks a `lota.py`-signed header in C and refuses
    every single-bit change of the 196 signed or signature bytes. On the
    C6-Zero: unsigned, bad signature and changed header byte refused before
    any erase (slot B untouched); a changed part byte refused at commit; a
    signed image committed, booted on trial and confirmed (`--wait`
    end-to-end); the override worked exactly once. On the NANO's C6 (IDF
    layout, after `sdio ota` from esp-hosted-mcu): a signed image through
    `/c6` committed, trialled and confirmed.
  * *Gap, for 52.10:* `otakey` is console-only, and a sealed C6 (Tab5) has
    no console. Proposed: the P4 hands its C6 the release key over LLP (a
    link the C6 already trusts like a cable).
  * *Found on the way and fixed:* `ea` ran an LLM's tool calls unconfirmed
    and unrestricted (`03f6162`: confirmation, plus refusal of privileged
    commands, primitives and `system/etc` writes inside a tool call). This
    matters for 52.10's "factory part = console only" rule. Also: C6 9P
    listener dead since TCP went to 4 slots (`a872382`), C6 heap exhausted
    (`ef1a2eb`).
  * A signature trailer in `ota_image_header_t`'s next version: ECDSA P-256
    over the image's SHA-256. `kernel/ota.c` checks it at `commit`, before
    `set_pending`. `/dev/ota` shows `signed=` and `signer=`.
  * Keys (decision §3.3 #5): one factory release key. Its public half is in
    the factory part, written by `tools/provision.py`. Its private half stays
    on the build host. `CONFIG_OTA_ALLOW_UNSIGNED` on development presets.
    The console can always install an unsigned image.
  * Raw `0xE9` images (esp-hosted-mcu's `network_adapter.bin`) carry no
    signature of ours. They stay console-only (`sdio ota`, `lota.py` with
    console access), never accepted over the network.
  * `tools/lota.py keygen | sign | push` (it signs by default when a key is
    configured; keys under `~/.config/lugalos/keys/`).
  * *Verification:* host tests for the trailer. On the C6-Zero over 9P/TCP
    and on the NANO's C6 over `/c6/dev/ota`: signed accepted, tampered (one
    flipped byte) and unsigned refused before the slot is erased. Code size
    measured on the C6-Zero and C6-MINI builds.
  * **52.7a C6 isolation behind IDF's bootloader** (52.5a's other item).
    Dump `pmpcfg*`/`pmpaddr*` as LugalOS finds them on the NANO's C6 (IDF
    layout), decode what U-mode may reach, and try it from the radio domain
    with a U-mode probe. If the locked entries open peripherals to U-mode,
    record it in the board guides and an ADR, and decide whether the IDF
    layout stays acceptable for the C6 or the loader layout replaces it once
    LugalOS is installed.

* **52.8 ESP32-P4 OTA backend (`drivers/ota_esp32p4.c`).** **DONE 2026-10-26 for
  the NANO (v1.3) and LCD-7B (v3.2); Tab5 not attached, not run; 52.8d open.**
  * *As built:* the layout is `doc/admin/updates.md`'s P4 table: loader at
    `0x2000`, `/flash0` unchanged, slots A/B at `0x250000`/`0x450000` (2 MB),
    boot records at `0x650000`. The loader (`arch/riscv/esp32p4/loader.c`)
    is a port of the C6's, linked at `P4_LOADER_BASE` per silicon revision
    (`linker/esp32p4_memory_rev*.ld`). `flash_esp32p4.c` allows writes only
    into the slot the XIP window does not map. `board_reset()` gained the P4's
    ROM `software_reset`, which `reboot` needed.
  * *Found on the way:* the L1 instruction cache must be invalidated after
    the loader copies the RAM half (`fence.i` is not enough; the entry point
    executed stale bytes, an illegal instruction). `Cache_WriteBack_All` takes
    a cache map. The RTC slow clock measured near 73 kHz, not 136 kHz: a 60 s
    hold fired after 112 s. The trial's health rule is the C6's: a volume
    mounted, and `wlan0` joined if a network is configured (the NANO had
    neither cable nor Wi-Fi, so "network up" could never confirm).
    `kernel/ota.c` moved its header and verify buffer to the heap: the Tab5's
    low RAM had 224 bytes to spare.
  * *Verified:* NANO: unsigned image pushed over the console 9P, software
    reboot, trial in B, confirmed (`active=b`); a `--break` image fell back
    on its own watchdog at 57 s (`fell_back=yes`, `failed=yes`). LCD-7B: the
    same cycle to confirmation. `racescan --check` clean.
  * *Not verified:* signed images on the P4 (same `kernel/ota.c` path as the C6),
    USB pulled mid-write, the wrong-revision refusal (the board id check
    answers first), over Ethernet, the Tab5, `test_esp32p4.py`'s OTA case.
  * **52.8a Layout** (decision §3.3 #2: our own). A permanent loader at
    `0x2000`, written once by `tools/p4flash.py`, as on the C6-Zero
    (`arch/riscv/esp32c6/loader.c`, the `LOTB` boot record). It picks a slot
    and loads that slot's RAM half. The loader is never rewritten over the
    network. Share code with the C6 loader where the two chips allow it.
  * **52.8b The backend on the NANO.** Two slots, each holding both halves
    (the ROM-loaded RAM half and the XIP half), of ≥ 2 MB each.
    *Proposed:* keep `/flash0` (`0x150000`) and the identity sector
    (`0xFFF000`) where they are, put both slots above `/flash0`, and free
    the old OS area. That means no data moves. Then:
    * `xip_esp32p4.c` maps the active slot's XIP half.
    * A trial boot arms the RTC watchdog. `ota_board_healthy()` is network
      up + `/sd0` or `/flash0` mounted. Confirm or fall back as on the C6.
    * `begin` refuses an image built for the other chip revision (v1.x
      versus v3.x, `linker/esp32p4_memory_rev*.ld`) before erasing anything.
    * `tools/p4flash.py --only layout` installs the new layout once.
      `lota.py push --p4 nano` updates over 9P on `eth0`.
  * *Verification (NANO):* update over Ethernet, trial confirmed. A
    deliberately unhealthy image falls back (`fell_back=yes`). A wrong-revision
    image is refused before erase. USB pulled mid-write: the old image boots.
    `test_esp32p4.py` gains an OTA case, and the full suite stays green.
  * **52.8c LCD-7B (rev v3.2) and Tab5.** The same tests, with the revision
    check exercised in both directions.
  * **52.8d Whole-board update.** **DONE 2026-10-26** (console transport).
    `lota.py push-board`: the C6 first, confirmed, then the P4, then the pair
    is checked (same LLP `HELLO` version, same build, each running the slot
    just written). *Changed from the plan:* the disagreement is reported by the
    tool (`lota.py status`, which now shows P4 and C6 side by side) and by
    the shell's `ota`, not by `/dev/ota`: the 9P server that serves `/dev/ota`
    would have to read `/c6/dev/ota` over the link it serves.
    *Verified on the NANO:* C6 1050 -> 1072 and P4 1070 -> 1072, both
    confirmed, same build, LLP in step; `--break c6` (the C6 falls back, the P4
    is untouched); `--break p4` (the P4 falls back after a confirmed C6, reported
    as a mixed board). *On the Tab5:* C6 953 -> 1072 and P4 1069 -> 1072. *Found:*
    a failed console attach left the port open for the next try; the wait for a
    C6 over the console reconnects (the Tab5's session ends with the C6's
    restart); a fall-back looks like success if only the trial and the build id are
    checked. *Over Wi-Fi only, on the Tab5 (nothing on the console):* with a stored
    grant (`peers add lugal HEX / rw`, which survives the P4's restart; a `p9key`
    does not) the whole board went C6 -> P4 -> check, the tool reconnecting after
    each restart: C6 and P4 both confirmed, same build, LLP in step.

* **52.9 RP2350 OTA backend (`drivers/ota_rp2350.c`).** **52.9a/b DONE 2026-10-26
  on the Pico 2 W; 52.9c DONE 2026-10-26 on the RP2350-LCD-7 (USB 9P only; through
  the C6-Zero, 49.10, not tried).** The notes below
  were written before the bootrom's behaviour was measured; what was built:
  * *As built:* `doc/admin/updates.md` has the map and the flow. Partition table
    in sector 0 (`tools/rp2350_ptable.py`, picotool), slots at `0x1000` and
    `0x280000` (0x17D000 each, the update header in the last sector), `/flash0`
    and the identity sector outside them, read through QMI ATRANS1/2 windows the
    kernel sets up first thing (`rp2350_map_data_windows()`, because the bootrom
    maps only the booted partition). IMAGE_DEF got a VERSION item
    (`LUGALOS_BUILD_MAJOR.MINOR`, `cmake/gen_build_id.cmake`).
  * *Two stages, as planned:* the bootrom's TBYB flag (set by `lota.py`) and
    `explicit_buy` as soon as the kernel is up; then our own trial with the boot
    record, a 15 s deadman and a rollback that erases the failed slot's first sector.
  * *Found:* `explicit_buy` erases the other partition unless the new image's
    version is strictly higher: updating with the same build destroyed the
    fallback. The node reports the real `version=` and `lota.py` raises the image's
    above it. A BOOTSEL-copied UF2 goes to the slot the bootrom would not boot.
    `picotool` needs a udev rule or root here (not available to the agent): all
    flashing was by copying UF2 files to the BOOTSEL drive.
  * *Verified (Pico 2 W):* first-time install over BOOTSEL keeping `/flash0` and the
    Wi-Fi identity; update over the 9P serial port, flash-update boot under TBYB,
    `explicit_buy` returned 0, trial confirmed (`active` follows); the same build
    pushed twice (version bumped, fallback kept); a `--break` image reverted by
    itself; a restart asked during a trial rolled back to the old slot with
    `failed=yes` and the failed slot's first sector erased.
  * *Not verified:* the 50 s give-up and the deadman paths (same rollback code, not
    reached on their own), a power cycle mid-update or mid-trial, an update over
    Wi-Fi, a signed image, the RP2350-LCD-7 and the other presets on hardware,
    `test_rp2350.py` and `lcdterm_shots.py`.
  * **52.9c terminal:** `cmake/flash_layout_rp2350_terminal.cmake` (16 MB): the
    table, `/flash0`, records and identity stay in the low 4 MB (so `provision.py`
    and the first-time install are unchanged), the slots are 4 MB at `0x400000` and
    `0x800000`: exactly one QMI window (ATRANS0), which leaves ATRANS1/2 for the data
    windows. Verified on the terminal: first-time install with picotool (**write the
    table, then `picotool reboot -u`, then the image**: loaded straight after the
    table the image went to address 0 and overwrote it), update over USB 9P,
    trial confirmed, same build again, `--break` reverted. Found: a stale SSID in the
    identity sector made the "wlan0 joined" rule unsatisfiable on a board without the
    radio; the rule now applies to CYW43439 builds only.
  * **Inspection:** the shell's `ota` command, `/dev/ota`'s `at_*` lines and
    `(require 'ota)` / `(ota-info)` show the layout, versions and the running slot.
  * *Original notes:*
  * **52.9a Layout.** *Proposed for 4 MB (Pico 2, Pico 2 W):* the partition
    table in sector 0, slot A `0x001000–0x180000` (1.49 MB), `/flash0`
    unchanged at `0x180000–0x280000`, slot B `0x280000–0x3FF000` (1.49 MB),
    identity unchanged at `0x3FF000`. Both slots run at `0x10000000` through
    the bootrom's address translation, and `/flash0` and the identity sector
    don't move. The 931 KB Wi-Fi build leaves ~560 KB of headroom. If that
    runs out, the CYW43 firmware (~240 KB) moves to its own partition
    outside A/B. The **RP2350-LCD-7 (16 MB)** gets its own
    `flash_layout_rp2350_terminal.cmake` with ≥ 4 MB slots.
  * **52.9b The backend on the Pico 2 W.**
    * IMAGE_DEF with version and TBYB flag (`arch/riscv/rp2350/boot_header.S`),
      and per-partition UF2 families (`tools/elf2uf2_rp2350.py`).
    * Writes go through `drivers/flash_rp2350.c`. `reboot` is a `FLASH_UPDATE`
      reboot.
    * **The bootrom's buy window is a fixed 16.7 s**, shorter than a Wi-Fi
      join + DHCP. So `explicit_buy()` happens once the kernel is up and
      flash is readable, and the network health check runs as our own second
      trial stage (record + watchdog, as on the C6) that can still switch
      back to the old slot.
    * `lota.py push` over 9P on `wlan0`.
  * *Verification (Pico 2 W):* update over Wi-Fi, trial confirmed. An image
    that hangs before buy is reverted by the bootrom. An image that boots but
    never joins Wi-Fi is reverted by the second stage. `/flash0`'s files
    survive the first-time layout UF2. BOOTSEL + UF2 still recovers a board.
  * **52.9c RP2350-LCD-7 terminal.** The 16 MB layout. Updates over USB 9P
    (`if02`) and through the C6-Zero (49.10). Verified with `test_rp2350.py`
    and `lcdterm_shots.py`.

* **52.10 Two-part identity, the `admin` grant, claiming, triggers.**
  *Boards: QEMU, then the Pico 2 W and the NANO for buttons.*
  * **Status 2026-10-26:** the field classes, the `admin` grant, `/dev/setup`,
    `/dev/owner`, the setup window, `CONFIG_NETWORK_CLAIM`, the console `setup`
    command, the setup secret in `provision.py`, and the tests (QEMU claim
    cycle, a build without the flag, the host check of the unauthenticated
    attach) are DONE, and so are the button triggers (§1.4 table; the C6 and the
    LCD-7B are untested). **Open:** the setup access point that makes the unauthenticated
    attach reachable (52.12 supplies `p9_link_is_setup_ap()`; until then a claim
    comes over a cable or the console). `/dev/owner` is one file with a command
    per line, not a directory: simpler, and a directory buys nothing before 52.11.
  * **The split is a write policy, not a new record format.** The TLV record
    (`kernel/idstore.c`) stays one record, so stored records need no
    migration. A table classifies each field:
    * **factory:** UID, DEVKEY, SETUP_SECRET (new), RELEASE_PUBKEY (new,
      52.7), MFG_CERT (reserved, §3 Q6). Console and `provision.py` only.
    * **owner:** NAME, WLAN_SSID, WLAN_PSK, IPV4, GRANTS, MQTT, ALTITUDE,
      POWER, ORIENT, TLS_KEY and TLS_CERT (52.14). Writable over
      authenticated 9P with `admin`, wiped by an owner reset.
    * `SENSOR_CAL` *proposed:* survives an owner reset (it describes the
      hardware), and `admin` may write it.
  * **`admin` grants:** a fourth column in the grants text (old lines still
    parse). `peers` shows it. The 9P server refuses writes to owner-identity
    files without it.
  * **Files:** `/dev/setup` (read: `state=unclaimed|setup|claimed`, time
    left, failed attempts; write: `start`, `stop`, `claim …`) and
    `/dev/owner/` (name, wlan, grants, …; secrets write-only). Factory fields
    are never served (`idstore_path_is_secret()` already covers this).
  * **The state machine:** setup-mode timeout and attempt limit (§3 Q2). An
    unclaimed node in setup mode accepts an unauthenticated attach **only**
    at `/dev/setup`, and only on the setup AP's interface. `CONFIG_NETWORK_CLAIM`
    off removes the path at compile time.
  * **Triggers:** console `setup start|stop|reset-owner`, `admin` over 9P
    (setup only), and a long press. Pico 2 W BOOTSEL (read from SRAM with
    XIP paused) and NANO BOOT (GPIO35), checked on hardware. Results go
    into the board guides (§1.4's table).
  * `tools/provision.py` generates the setup secret and prints it as text +
    a QR payload (WPA `WIFI:` URI).
  * *Verification:* QEMU tests: claim succeeds once, a second claim is
    refused, an owner reset returns to unclaimed with factory fields intact,
    and a build without the flag refuses outright. A host test for the field
    policy. Long press on both boards.

* **52.11 HTTP server, WebSocket, and 9P over WebSocket (plain HTTP).**
  * **Status 2026-10-29:** the stream listener (`tcp_stream_listen()`/
    `tcp_stream_accept()`), `net/websocket.c` (+ `tests/host/websocket_host.c`),
    `net/httpd.c` with `httpd` in the shell, the read-only link property
    (`P9_AUTH_REQUIRED_RO`), `connect_ws()` in `host/p9lib` and the QEMU test
    are DONE. The `httpd` task needs 6 stack pages (11 KB high-water on a
    read). The admin page (`tools/sd_root/www/admin.html`, served from
    `/flash0/www/` or `/sd0/www/`: sign-in, files, status, log, node) is DONE;
    its protocol code runs under node and the whole page in headless Chromium,
    both from the QEMU test. The session table lives in the pool `httpd start`
    takes, not in `.bss`: the Tab5 and LCD-7B have 56 and 328 bytes of low RAM
    left. Open: gzip, the Pico 2 W and NANO runs, Firefox, 52.11b.
  *Boards: QEMU (slirp), the NANO on `eth0`, the Pico 2 W on `wlan0`.*
  * **TCP:** a passive-open stream API (`tcp_stream_listen()` /
    `tcp_stream_accept()`), so a server task owns its connections and the
    network pump never runs HTTP or VFS code.
  * **`net/httpd.c`:** its own task on port 80. GET for the one page
    (gzip, ETag), `Upgrade: websocket`, 404 otherwise. Two concurrent
    sessions at most. Captive-portal probe URLs redirect to the setup page
    while in setup mode.
  * **`net/websocket.c`:** server and client framing (masking, fragments,
    ping/pong, close), SHA-1 from 52.6. Shared with phase 62.12.
  * **The 9P link:** a WebSocket session as a `p9_link_t` with
    `auth_required`. A new link property, `read_only`, makes the 9P server
    refuse `Twrite`/`Tcreate`/`Tremove`/`Twstat`. Plain HTTP sets it on TLS
    boards. On non-TLS boards the session is integrity-protected instead
    (52.11b). The setup AP is exempt everywhere (decision §3.3 #4).
  * **The page** (`tools/web/admin.html`, one file, embedded gzip-compressed
    by the build into `/flash0` or `.rodata`): SHA-256/HMAC and a 9P client
    in plain JavaScript, plus views for `/proc`, a file browser, ctl files,
    and the klog tail. OTA upload is enabled over HTTPS (52.15) or a protected
    session (52.11b).
  * `httpd start|stop|status` in the shell. `host/p9lib` gains a WebSocket
    transport for tests.
  * *Verification:* a QEMU test in `tests/runner.py` driving the page's
    protocol from Python. On both boards in Firefox and Chrome: log in with a
    key file, browse `/proc` and (NANO) `/c6`, and see writes refused on the
    NANO. The page loads while 9P and MQTT hold the other slots. RAM cost
    recorded.
  * **52.11b Integrity-protected 9P sessions for non-TLS boards.** *Boards:
    the Pico 2 W and the C6-Zero.*
    * After a successful `Tauth`, both ends derive a session key
      (HKDF-SHA-256 over the grant key and the nonce). Every later frame
      carries a sequence number and a truncated HMAC-SHA-256 tag.
    * A link property turns this on. It applies to TCP 9P links and to
      WebSocket links on boards without `CONFIG_TLS_SERVER`. A frame with a
      bad tag or sequence ends the session.
    * The 9P server refuses writes to secret-bearing files (WLAN PSK,
      grants, keys) on any network link without TLS, except the setup AP.
    * The other ends: `host/p9lib` (so `fuse-p9`, `lota.py` and the CA
      script), and the page's JavaScript.
    * Wire format and threat coverage go in `doc/dev/` (protocols), stating
      plainly that this gives integrity, not confidentiality.
    * *Verification:* host tests for the framing. A QEMU test where a proxy
      in between flips a byte, replays a frame and injects a frame, and each
      is refused. On the C6-Zero and the Pico 2 W: `lota.py push` and
      `fuse-p9` over a protected session, with the throughput cost
      measured.

* **52.12 SoftAP, DHCP server, captive DNS, phone setup (Pico 2 W).**
  * **CYW43 AP mode** in `drivers/cyw43_rp2350.c`: WPA2-PSK from the setup
    secret, done by the chip's firmware. `/dev/wlan` gains `ap start|stop`
    with the same names as `/c6/dev/wlan`. AP or STA at a time (APSTA later).
  * **`net/dhcpd.c`:** one /24, four leases, a static table.
    **`net/dnsd.c`:** answers every A query with the node's address.
  * **The setup page** (served only in setup mode): a scan list from
    `/dev/wlan`, PBKDF2-HMAC-SHA1 (4096 iterations) in JavaScript so the
    passphrase never leaves the phone, the owner key from
    `crypto.getRandomValues()` (§3 Q1), the claim, and the key file
    download. After a claim the AP closes and the node joins the network.
  * *Verification:* an unclaimed Pico 2 W, set up from an Android phone, an
    iPhone and a laptop. Join with the label secret, the page opens by
    itself, claim, and the node joins the home Wi-Fi. `fuse-p9` then
    authenticates with the downloaded key, a second claim is refused, and a
    long press resets the node. Phase 21's I6 holds: no passphrase in any
    frame (checked with a capture).

* **52.13 C6 SoftAP for P4 boards** *(deferrable).* *Board: the NANO, then
  the LCD-7B and the Tab5.*
  * Port the WPA2 authenticator (IDF `wpa_supplicant`'s `src/ap/` +
    `esp_hostap`) into the C6's confined radio domain, replacing the
    `wpa_receive()` stub.
  * `/c6/dev/wlan ap start`, and AP frames on a second LLP interface (`ap0`).
    The P4 runs `dhcpd`, `dnsd` and `httpd` on `ap0` (D3: the P4 owns the IP).
  * *Verification:* the C6-MINI's RAM and code size measured against §1.6's
    budget. A phone-only setup of the NANO, then of the Tab5 (no cable but
    power).

* **52.14 Device TLS key, CSR, and the LugalOS CA.** *Board: the NANO.*
  * The node generates a P-256 key (refused without hardware entropy, 52.6)
    into the owner part (TLS_KEY, never served).
  * `/dev/tls/csr` (PKCS#10, PEM, built on read; SAN: `<name>.local` and the
    current addresses) and `/dev/tls/cert` (write a PEM chain, checked
    against the key). A minimal DER writer and reader in-tree.
  * `host/ca/` (Python, `uv`, `cryptography`, typed for `pyrefly`):
    `lugalos-ca init` (the root key stays under `~/.config/lugalos/ca/`) and
    `lugalos-ca issue --p4 nano | --host …`, which reads the CSR over 9P and
    writes the certificate back.
  * Admin manual: installing the root certificate on Linux, Firefox, Chrome,
    Android and iOS.
  * *Verification:* host tests for the DER code. `openssl req -verify` on the
    CSR, `openssl verify` on the issued certificate. The key never appears
    in any file served over 9P.

* **52.15 TLS 1.3 server and HTTPS admin** (`CONFIG_TLS_SERVER` boards only).
  *Board: the NANO; the RP2350-LCD-7 (PSRAM) after it.*
  * **`net/tls.c`, server only:** ClientHello (supported_versions, X25519
    key_share, signature_algorithms, `record_size_limit`),
    HelloRetryRequest when no X25519 share is offered, then
    EncryptedExtensions, Certificate, CertificateVerify (ECDSA P-256),
    Finished. ChaCha20-Poly1305 records, alerts, KeyUpdate. No PSK, resumption,
    0-RTT or client certificates yet (mTLS: phase 53's Gemini).
  * **Memory:** a 16 KB+ receive record buffer per session from PSRAM, and
    sent records ≤ 4 KB. At most two sessions. The handshake runs in
    `httpd`'s task.
  * Port 443. Port 80 redirects once a certificate is installed. Admin
    writes, including OTA upload from the page, are enabled over HTTPS. The
    page uses WebCrypto when it has a secure context.
  * **52.15b Accelerators:** P4 AES, SHA and ECC/ECDSA behind the 52.6 API,
    and with the AES block `TLS_AES_128_GCM_SHA256` (GHASH in software,
    constant-time). Same vectors, timings compared.
  * *Verification:* `tls.c` against OpenSSL on the host under ASan, plus a
    host fuzz harness for the record and handshake parsers. On the NANO:
    `openssl s_client -tls1_3`, curl, Firefox, Chrome, Safari (iOS), and
    Chrome (Android). Handshake time with and without accelerators. An OTA
    of the NANO from the browser.

* **52.16 Integration, documentation, review; §1.6 investigation.**
  * End to end: a Pico 2 W set up from a phone and updated from the
    browser. The NANO administered over HTTPS, with a whole-board update
    (52.8d) from the page.
  * `tests/hw/`: OTA, httpd and TLS cases in `test_esp32p4.py`, and setup-mode
    cases in `test_wifi.py` (skip cleanly without hardware).
  * Docs: `doc/user/shell.md` (`setup`, `httpd`, `ota`, `tls`), the admin
    manual (remote admin, CA, new flash layouts), `doc/dev/` (9P over
    WebSocket), board guides (buttons, layouts). Then
    `tools/gendocs.py --all` / `--check`.
  * An ADR on the remote-administration trust model (what each path can
    write, before and after claiming, with and without TLS). `racescan`
    verdicts for every new static.
  * **§1.6 and the C6-Zero's TLS question** (§3.3 #4), on the C6-Zero
    sensor node: measure the sensor profile's free RAM with the radio and
    MQTT up, and test TLS-PSK versus certificates against mosquitto
    (`record_size_limit`). Record whether the C6-Zero is a TLS board or
    stays on 52.11b's protected sessions. Building a TLS client there belongs
    to a later phase.

