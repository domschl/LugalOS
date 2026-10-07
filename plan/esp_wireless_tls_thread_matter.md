# TLS, Thread and Matter on LugalOS — rough feasibility estimate

Status: estimate only, not a plan (2026-10-07).

Baseline is phase 45 (`plan/phase45_esp32c6.md`, ESP32-C6 Wi-Fi via the proprietary
blob in a U-mode domain) and the `build/esp32c6` sizes at commit `506317d`.
Footprints of third-party stacks are recalled public figures, not measured here —
treat them as ballpark numbers.

## Current budget (ESP32-C6, measured)

- **SRAM:** 512 KB total, 496 KB usable (top 16 KB is ROM radio data). Static data
  uses ~232 KB (`.bss` 145 KB, radio 33 KB, kobj/U-mode 28 KB, stack 16 KB) and the
  heap is 264 KB. Free heap: 92 KB with Wi-Fi joined, 84 KB with sensors,
  **60 KB with Wi-Fi + sensors + MQTT**. The Lisp pools (~46 KB) could be given up.
- **Flash:** OS image 0.95 MB in a 2 MB slot, out of 8 MB on the chip. Flash is not
  the constraint; RAM is. OTA would need a second slot.
- **Crypto:** WPA2 runs the supplicant's internal software crypto (SHA-256, HMAC,
  PBKDF2, AES, CCMP). The C6's hardware SHA/AES/ECC accelerators are unused. The RNG
  only gives real entropy while the RF is on.

## 1. TLS — feasible, moderate effort

| | Estimate |
|---|---|
| Flash | 60–120 KB (cut-down mbedTLS) or 50–80 KB (BearSSL) |
| RAM, one client connection | 25–40 KB at handshake peak, with records limited to 4 KB via `max_fragment_length` (default 2×16 KB record buffers alone would be ~33 KB) |
| Fits on the C6? | Yes, one connection (e.g. MQTT over TLS) fits in the 60 KB free, not much to spare |

- Freestanding C, no floating point: ECDHE/ECDSA, AES-GCM and ChaCha20 are all integer
  code. Could run in its own U-mode domain, like the Wi-Fi blob.
- Needs alongside it: a wall clock (SNTP) for certificate validity, a CA bundle in
  flash, and RNG gated on radio-on (fine, TLS only runs over Wi-Fi anyway).
- Cheaper first step: TLS-PSK only (pre-shared key, no X.509) to your own broker.
- On the ESP32-P4 and the RP2350-terminal (PSRAM), RAM is not an issue.

## 2. Thread radio — feasible on the C6 as an end device, not together with Wi-Fi

- The 802.15.4 MAC driver in ESP-IDF (`components/ieee802154`) is source; only the PHY
  is closed, and `libphy` is already linked for Wi-Fi. No second big U-mode blob like
  the Wi-Fi one.
- Network stack: OpenThread. C++, but embedded-friendly (no exceptions, no RTTI,
  static allocation). Its porting layer (`otPlat*`, ~20–30 functions: radio, alarm,
  entropy, settings) is much cleaner than the 127-entry `wifi_osi_funcs_t`.

| Role | Flash | RAM |
|---|---|---|
| Minimal / sleepy end device (MTD/SED) | ~150–250 KB | ~30–50 KB |
| Full Thread device / router (FTD) | ~300–400 KB | ~60–100 KB |

- One 2.4 GHz radio front end: Wi-Fi + Thread together only via time-sliced
  coexistence. Espressif does not recommend a Thread border router on a C6 alone
  (their reference design is S3 + H2).
- Bottom line: a Thread-only C6 sensor node (no Wi-Fi) is realistic. Wi-Fi + Thread +
  MQTT at once does not fit in RAM.
- Prerequisite: Thread is IPv6 only (6LoWPAN). The state of IPv6 in the LugalOS IP
  stack was not checked — possibly a sizeable sub-project.
- Thread alone does not give Apple Home integration: Apple Home needs Matter on top
  (HomeKit-over-Thread is legacy and being phased out).

## 3. Matter — the expensive one

- Espressif's Matter examples on the C6 use ~1.2–1.8 MB flash and want ~100–150 KB
  free heap while running. Flash is fine; RAM is ~2× what is left on the C6 with
  Wi-Fi joined.
- The real cost is porting the SDK (connectedhomeip): large C++17, expects an
  RTOS-like platform layer. Needs:
  - a C++ runtime in the freestanding kernel (new/delete, static ctors, parts of the
    C++ library);
  - its crypto: SPAKE2+, ECDSA P-256, AES-CCM, HKDF — heavy overlap with item 1;
  - mDNS/DNS-SD and IPv6;
  - persistent storage.

  Months of work — more than TLS and Thread combined.
- Commissioning usually goes over BLE (another closed blob on the C6). Matter also
  allows on-network commissioning for a device already on the IP network, avoiding BLE.
- Production devices need CSA certification (DAC). Test certificates work for
  development; Apple Home accepts uncertified devices with a warning.
- **"LugalOS as a Matter gateway" is the most realistic form:**
  - A **Matter bridge** exposes non-Matter devices (LugalOS sensors, RP2350 nodes
    behind MQTT/9P) to Apple Home as Matter endpoints. No Thread needed; runs over
    Ethernet or Wi-Fi.
  - The **ESP32-P4** is the natural host: Ethernet and 32 MB PSRAM on the NANO
    (whether LugalOS uses that PSRAM yet was not checked).
  - A Thread border router is not needed: HomePods / Apple TVs already provide one.

## Summary

| Feature | C6 (60 KB free) | P4 / RP2350 with PSRAM | Effort |
|---|---|---|---|
| TLS client (1 connection) | ✅ fits | ✅ easy | weeks |
| Thread end device | ✅ only without Wi-Fi | ❌ no 802.15.4 radio | 1–2 months, plus IPv6 |
| Wi-Fi + Thread at once | ❌ RAM | — | — |
| Matter device on the C6 | ❌ RAM, ~2× short | — | — |
| Matter bridge over Ethernet | — | ✅ plausible | many months (SDK port) |

Sensible order: TLS first (Matter reuses most of its crypto). Then either a Thread-only
C6 node, or a Matter bridge on the P4 that exposes the existing MQTT sensor network to
Apple Home — the bridge is the larger job but the one that actually reaches Apple Home.
