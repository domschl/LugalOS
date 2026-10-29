# LugalOS Identity, Authentication & Security

[Documentation](../README.md) > [Administrator Manual](index.md) > **Identity, Authentication & Security**

---

LugalOS implements a decentralized identity and authentication architecture designed for embedded microcontroller networks. The identity record (`idstore`) anchors board provisioning, network credentials, and mutual authentication.

---

## 1. The Identity Record (`idstore`)

Each LugalOS board maintains an immutable or cryptographic identity record persisted in on-chip flash:
* **Board Identity**: Unique cryptographic fingerprint, node persona, and assigned hardware roles.
* **Network Credentials**: Stored Wi-Fi SSID, derived 64-character hex pre-shared key (PSK), and static IP configuration.
* **Service Configurations**: MQTT broker destination, telemetry sample rates, and last-will parameters.

To view the current identity record from the shell:
```text
lsh> identity
```

---

## 2. Safe Wireless Credential Handling

To protect user Wi-Fi credentials from accidental leakage in shell logs or serial traces:
1. **Never Send Plain-Text Passwords**: Plain-text passwords are never accepted by `wifi join` or stored on the device.
2. **Host-Side Derivation**: The PBKDF2 64-hexadecimal PSK is derived once on the host computer:
   ```bash
   wpa_passphrase "NetworkSSID" "CleartextPassword"
   # Outputs: psk=a1b2c3d4... (64 hex characters)
   ```
3. **Local Host Environment**: Automated test runners read network parameters from `~/.config/lugalos/wifi.env`:
   ```bash
   LUGALOS_WIFI_SSID="MyNetwork"
   LUGALOS_WIFI_PSK="a1b2c3d4..."
   ```
4. **Permanent Provisioning**: On the board, store the PSK directly into the identity record:
   ```text
   lsh> wlan MyNetwork a1b2c3d4...
   ```
   Upon subsequent boots, the board associates with the wireless network automatically with zero manual entry.

---

## 3. 9P Filesystem Authentication (`p9auth`)

When exposing 9P filesystem namespaces over TCP or untrusted links:
* **HMAC-SHA256 Challenge-Response**: LugalOS gates 9P attach requests using an HMAC challenge-response handshake based on shared keys.
* **Enabling Authentication**:
  ```text
  # Require auth on a specific link (e.g. TCP port 564)
  lsh> p9auth tcp on

  # Set current boot authentication key (or commit to identity store)
  lsh> p9key 0123456789abcdef...
  ```
* Unauthorized read or write requests to protected volumes (`/sd0`, `/flash0`) are rejected at the channel boundary before reaching filesystem drivers.

## 4. Claiming a Node and the Owner Part

The identity record has a **factory part** (uid, device key, release key, setup secret), which only the console and `tools/provision.py` write, and an **owner part** (name, Wi-Fi, IP, grants, MQTT, altitude, power) that survives nothing: `setup reset-owner` wipes it. Sensor calibration describes the hardware and survives a reset.

A node is **claimed** when a grant carries `admin`; otherwise it is **unclaimed**. An unclaimed node with a setup secret opens a 15-minute **setup window** at boot (and on `setup start`). While it is open, a client that knows the secret writes one line to `/dev/setup`:

```text
claim SECRET OWNERKEYHEX [name NAME] [wlan SSID PSKHEX64]
```

The owner key becomes the `owner` admin grant, the window closes, and from then on `/dev/owner` accepts `name`, `wlan`, `grant`, `ungrant` and `setup start|stop` from an `admin` peer. Five wrong secrets close the window until the next trigger. The last admin grant cannot be removed over the network, and an owner reset is physical: the console, never 9P. The write returns at once and a worker task does it; read the outcome from `result=` in `/dev/setup`.

The setup secret is the WPA2 key of the node's setup access point (52.12). `tools/provision.py --setup-secret-generate` prints it with the label text and a `WIFI:` QR payload. A build without `CONFIG_NETWORK_CLAIM` has no `/dev/setup`, no `/dev/owner` and no window.

### Which board is this?

`/proc/board` (shell: `board`, web: `GET /board`) answers without a key, on any link: `preset`, `board`, `hardware`, `chip`, `version`, `build`, `tainted`, `name` and `uid`. It is the one path a 9P client may attach to without authentication (attach with the path `/proc/board`; the fid is read-only), so a discovery tool or the admin page's sign-in screen can say which board it is talking to before it has a key. It holds nothing about the network or the owner.

---

**Navigation:** [<- Networking Guide](networking.md) | [Administrator Manual](index.md) | [Storage Guide ->](storage.md) | [Documentation Index](../README.md)
