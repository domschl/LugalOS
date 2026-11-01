# Phase 68 — Advanced remote admin: crypto hardware, phone setup of P4 boards, a pocket console

**Status: placeholder. Written 2026-11-01. Not scheduled.** Follows phase 52 (remote admin, done 2026-11-01), whose
52.15b and 52.13 move here with the two Android checks 52.16 left open, and whose trust model (ADR 006)
every milestone below keeps.
Milestone scheme: `68.1`, `68.2`, …

---

## 0. Scope

Phase 52 made every LugalOS node administrable from the network: signed updates, the web admin page,
claiming, the owner's CA and HTTPS. Three things were left for later because none of them is needed
for that to work:

1. **Speed:** HTTPS on the P4 runs on software crypto (handshake 0.13-0.28 s). The P4 has AES, SHA
   and ECC blocks that phase 52 does not use.
2. **Phone setup of the P4 boards:** only the Pico 2 W can be set up from a phone today, through its
   own access point. A P4 board's Wi-Fi is its C6, which cannot be an access point yet: the C6 radio
   domain has no WPA2 authenticator.
3. **Headless appliances without a phone or a computer:** a node with no screen, no keyboard and no
   setup access point (the C6-Zero) is provisioned from a computer's serial console. A small plug-in
   console would do the same in the field.

## 1. Milestones

* **68.1 P4 crypto hardware** (was 52.15b). *Board: the NANO, then the LCD-7B and Tab5.*
  * The P4's AES, SHA and ECC (ECDSA) blocks behind the 52.6 crypto API, chosen at build time, with
    the software code kept as the reference and for the boards that lack them. Register-level detail
    from the P4 TRM (`${ESP_ROOT}/datasheet/`).
  * With the AES block, `TLS_AES_128_GCM_SHA256` beside ChaCha20-Poly1305 (GHASH in software,
    constant-time).
  * Who owns the blocks: one driver task or a lock (`sysreg_update()` for the clock and reset gates),
    since TLS, OTA signature checks and the 9P protected sessions all use them.
  * *Verification:* the 52.6 test vectors on hardware against the software results; handshake time
    and `wss://` throughput with and without the blocks; the host TLS tests unchanged.

* **68.2 Setup access point on the C6 for P4 boards** (was 52.13). *Board: the NANO, then the LCD-7B
  and the Tab5.*
  * Port the WPA2 authenticator (IDF `wpa_supplicant`'s `src/ap/` + `esp_hostap`) into the C6's
    confined radio domain, replacing the `wpa_receive()` stub.
  * `/c6/dev/wlan ap start`, and AP frames on a second LLP interface (`ap0`). The P4 runs `dhcpd`,
    `dnsd` and `httpd` on `ap0` (D3: the P4 owns the IP), and the setup page and `/dev/setup` claim of
    52.12 unchanged.
  * Memory first: a C6 in the LLP radio role with MQTT and httpd up had 0 KB free in phase 52's
    suite (52.13b), and the radio heap is a 64 KB arena. Measure the authenticator's code and RAM
    against that before porting all of it; phase 66's stack sizing may have to come first.
  * Then the stand-alone C6-Zero: the same access point would give it 52.12's phone setup.
  * *Verification:* a phone-only setup of the NANO, then of the Tab5 (no cable but power); the
    C6-MINI's RAM and code size measured.

* **68.3 Pocket console: an I2C keyboard and character display** (idea, 2026-11-01). *Boards: the
  C6-Zero first (sensor bus GPIO0/1), then the Pico 2 W.*
  * **The idea:** a small hand-held unit made of an I2C keyboard (M5Stack CardKB2) and a 20x4
    HD44780 character LCD on an I2C backpack (PCF8574), on one cable. Plugged into a headless
    appliance's I2C connector it is that node's console for as long as it is attached: name, Wi-Fi,
    a grant, the setup window, an owner reset. Then it is unplugged and goes to the next appliance.
  * **Trust:** it is physical presence, the same class as the serial console in ADR 006 ("being
    there"), so it needs no keys and no network. That is also its risk: anyone who can reach the
    connector is the owner. To decide: whether it is always a console, or only while the node is
    unclaimed or after the 5 s button press (a claimed appliance in a public place).
  * **Shape:** no new protocol. The appliance drives both parts itself: polls for the keyboard's and
    the backpack's addresses every few seconds on the bus the sensors use (one address probe each,
    no scan), and when both answer, opens a console session (`session_claim_t`) on a 20x4 text view.
    A menu rather than a shell (status, Wi-Fi, grant, setup, reset), since a 20x4 screen does not
    hold a shell's output. Each part is a self-contained I2C driver on phase 26's generic transfer.
  * **To check against the parts' datasheets before anything is built:** the CardKB2's address,
    read protocol and supply (the CardKB v1.1 answered at 0x5F, one ASCII byte per read); the
    backpack's address (0x27 or 0x3F) and its pull-ups: these backpacks usually pull SDA and SCL up
    to 5 V, which the ESP32-C6's pins do not tolerate, so either the pull-ups go or a level shifter
    goes in. The LCD itself needs 5 V for its contrast.
  * **The other shape, not chosen yet:** the pocket unit as a LugalOS node of its own (a Pico 2 W
    with the keyboard and display), holding the owner key and claiming appliances over their setup
    access point. More capable (a claimed node's key never has to be typed), but it needs 68.2 on
    the C6 boards and makes the unit itself a secret worth stealing.
  * *Verification:* a C6-Zero provisioned from nothing to a claimed node on the home Wi-Fi with the
    pocket console only; unplugged and plugged in again mid-menu without a hang; sensors on the same
    bus keep reading meanwhile.

* **68.4 The phone checks phase 52 left open** (from 52.16). *Boards: the Pico 2 W, a P4 board, an
  Android phone.* No new code expected; each is a check against what phase 52 built, and a bug it
  finds is fixed where it lies.
  * Chrome (Android) over HTTPS with the owner's root installed (`doc/admin/tls.md`, the Android
    steps): a P4 board's page shown as secure. Safari (iOS) and Chrome (Linux) passed on the Tab5 on
    2026-11-01.
  * A Pico 2 W set up from an Android phone, end to end through its setup access point (52.12): the
    captive page opens, the claim takes, the node joins the home network. The iPhone did this in 52.12
    and the host playing the phone in `test_wifi.py --claim-cycle`.
  * The LCD-7B's certificate again (`lugalos-ca issue --tcp esp32p4-lcd7b-4ed2.local --keyfile
    ~/.config/lugalos/keys/lcd7b.p9key`): an owner-reset test removed its key and certificate.
  * *Verification:* each check by the owner on the phone; the result recorded here.

## 2. Depends on

* Phase 52 (all of it), ADR 006.
* Phase 26 (the I2C generic transfer) for 68.3.
* Phase 66 (RAM and stack sizing) possibly before 68.2.

---

**Navigation:** [Phase 52](phase52_remote_admin.md) | [Open issues](open_issues.md)
