# HTTPS Certificates: Your Own Certificate Authority

A node that serves HTTPS (the ESP32-P4 boards, and the QEMU targets for testing; `CONFIG_TLS_SERVER`) proves who it is with a certificate. No public certificate authority will issue one for a name like `esp32p4-nano-2fdf.local`, so you run your own: a root key on your computer (`host/ca`), whose root certificate you install once on every device that opens the nodes' admin pages. The node makes its own key and never gives it out; the CA signs the node's request and the node keeps the certificate.

The HTTPS server itself is plan 52.15. Until it lands, the key and the certificate are made, checked and stored as described here, and the admin page is plain HTTP.

---

## 1. Create the CA (once)

```bash
uv run --project host/ca lugalos-ca init
```

This writes `~/.config/lugalos/ca/ca.key.pem` (the root key, mode 0600: keep it on this computer, back it up privately) and `~/.config/lugalos/ca/ca.crt.pem` (the root certificate you install on devices), and prints the root's SHA-256 fingerprint. `lugalos-ca show` prints them again. `--name` sets the root's name (default `LugalOS CA (USER)`, with your login name); `--force` replaces the root, after which every node's certificate must be issued again and every device given the new root.

## 2. Issue a node's certificate

The CA talks to the node over 9P with an **admin** grant (see [Identity](identity.md)):

```bash
uv run --project host/ca lugalos-ca issue --tcp esp32p4-nano-2fdf.local --keyfile ~/.config/lugalos/keys/nano.p9key
```

It asks the node for a key if it has none, reads the node's certificate request, checks its signature and that it names only `.local` names and private addresses, signs a certificate valid for 825 days, writes it to the node and reads it back. `--rekey` has the node make a new key first; `--save-csr FILE` and `--save-cert FILE` keep copies (`openssl req -in FILE -noout -verify -text`, `openssl verify -CAfile ~/.config/lugalos/ca/ca.crt.pem FILE`).

The certificate names `NAME.local` and the address the node had when it made the request. Use the name: a DHCP address can change, and then only the name still matches. Issue again after renaming a node, after `--rekey`, after an owner reset (which deletes the key and the certificate), and before the 825 days run out.

### On the node

The same steps by hand, at the console or over 9P:

| | |
| :--- | :--- |
| `tls` or `cat /dev/tls` | The key's fingerprint, the certificate's subject, issuer and dates, whether certificate and key match, and the names a request would carry. |
| `tls keygen`, `echo keygen > /dev/tls` | Makes a key when there is none. Refused without hardware entropy. |
| `tls rekey`, `echo rekey > /dev/tls` | A new key; the old certificate is deleted with the old key. |
| `tls clear`, `echo clear > /dev/tls` | Deletes the key and the certificate. |
| `tls csr`, `cat /dev/tlscsr` | The certificate request (PKCS#10, PEM). Built once and kept, so a client reading it in parts gets one request. |
| `tls cert`, `cat /dev/tlscert` | The installed certificate (PEM). |
| write a PEM certificate to `/dev/tlscert` | Installs it if it is for this node's key; refused otherwise. A chain may follow; only the first certificate is kept. |

Writing `/dev/tls` and `/dev/tlscert` over 9P needs the `admin` grant. The private key is in the identity record's owner part; no file serves it, and an owner reset deletes it.

## 3. Install the root certificate on your devices

Install `ca.crt.pem` (a file that contains no secret) as a trusted root. Check the fingerprint against `lugalos-ca show` wherever the system displays one.

**Linux (system-wide: curl, Python, most programs).** Arch and Fedora: `sudo trust anchor --store ~/.config/lugalos/ca/ca.crt.pem`. Debian and Ubuntu: `sudo cp ~/.config/lugalos/ca/ca.crt.pem /usr/local/share/ca-certificates/lugalos-ca.crt && sudo update-ca-certificates`.

**Chrome and Chromium on Linux** keep their own store: *Settings > Privacy and security > Security > Manage certificates > Authorities > Import*, then tick *Trust this certificate for identifying websites*. From a shell: `certutil -d sql:$HOME/.pki/nssdb -A -t "C,," -n "LugalOS CA" -i ~/.config/lugalos/ca/ca.crt.pem` (`certutil` is in `nss` / `libnss3-tools`). On Windows and macOS, Chrome uses the system store.

**Firefox** (every system): *Settings > Privacy & Security > Certificates > View Certificates > Authorities > Import*, select the file, tick *Trust this CA to identify websites*.

**Windows:** double-click the file (renamed to `lugalos-ca.crt`) > *Install Certificate* > *Local Machine* > *Place all certificates in the following store* > *Trusted Root Certification Authorities*.

**macOS:** open the file in *Keychain Access*, add it to the *System* keychain, open it, and under *Trust* set *When using this certificate* to *Always Trust*.

**Android:** copy the file to the phone, then *Settings > Security & privacy > More security settings > Encryption & credentials > Install a certificate > CA certificate* (the path differs a little between makers; search the settings for "CA certificate"). Chrome and the system browser trust it; apps that do not opt in to user-installed CAs do not.

**iPhone and iPad:** send the file to the device (AirDrop, mail, or a download in Safari), open it, and allow the profile download. Then *Settings > General > VPN & Device Management*, select the profile and *Install*. Finally *Settings > General > About > Certificate Trust Settings*, and turn on full trust for the LugalOS CA. iOS refuses server certificates valid for more than 825 days, which is why `issue` stops there.

---

**Navigation:** [<- Updating Nodes](updates.md) | [Administrator Manual](index.md) | [Board Reference Guides ->](boards/index.md) | [Documentation Index](../README.md)
