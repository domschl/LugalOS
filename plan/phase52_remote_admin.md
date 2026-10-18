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
  The image needs an IMAGE_DEF block. **Flash is tight at 4 MB**: today
  1.5 MB is reserved for the OS (~470 KB used), plus `/flash0` and the
  identity sector. Two slots mean shrinking them to about 1 MB each and
  repartitioning every board once (UF2 + partition table), moving `/flash0`'s
  contents along. Not every RP2350 preset has a network. The terminal is
  updated over its USB 9P port (`if02`) or through a C6 (49.10); the same
  files work on both.
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
| LCD-7B, RP2350 touch terminal, Tab5 | touch (GT911 / ST7123) | e.g. a long press on the menu bar; no extra hardware |
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

* **52.6 ESP32-P4 & RP2350 OTA Backends (`drivers/ota_esp32p4.c`, `drivers/ota_rp2350.c`).**
* **52.7 Two-Part Identity Store Split (Factory vs. Owner) & Claiming State Machine.**
* **52.8 SoftAP Mode, DHCP Server & First-Time Phone Setup Page.**
* **52.9 LugalOS CA Scripts, Device CSR Generation & TLS 1.3 Stack.**
* **52.10 WebSocket-to-9P Generic Admin Server over HTTPS.**

