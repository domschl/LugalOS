# ADR 006: Remote Administration Trust Model

## Status
Accepted (2026-10-31)

## Context
Phase 52 made a node administrable from the network: updates over 9P, a web admin page, claiming from a phone through the node's own access point, and HTTPS. Phase 21 had ruled provisioning over the network out altogether. Each new path had to be given exactly the authority it can defend, and no path may reach further than the console can see. This record states the result in one place; the plan (`plan/phase52_remote_admin.md`, sections 1.3 and 3.3) holds the reasoning step by step.

The identity record has three classes of field (`kernel/idstore.c`, `idstore_field_class()`): **factory** (uid, device key, release key, setup secret, manufacturer certificate), **owner** (name, Wi-Fi, address, grants, MQTT, altitude, power, orientation, the HTTPS key and certificate) and **hardware** (sensor calibration). A node is **claimed** when a grant carries `admin`.

## Decision

| Path | Authenticates by | May write | Secrets (Wi-Fi key, grants, keys) |
| :--- | :--- | :--- | :--- |
| Console (serial, USB console) | being there | everything: factory part, owner reset, `otakey`, an unsigned update once (`otakey allow-unsigned`) | yes |
| 9P over a cable (USB CDC `if02`, a P4's LLP to its own C6) | being there: the console's equivalent | as the console, except what the console alone does (factory part, owner reset) | yes |
| 9P over TCP (port 564) | a granted key (HMAC challenge) | only on a protected session (9P2000.lp, integrity per frame); owner files and `/dev/tls*` only with `admin` | no |
| WebSocket, plain HTTP (port 80), board without HTTPS | a granted key | as 9P over TCP (protected session needed); `httpd write on` at the console allows plain writes | no |
| WebSocket, plain HTTP, board serving HTTPS | a granted key | nothing: read-only; the page itself redirects to HTTPS | no |
| WebSocket over HTTPS (port 443) | a granted key; the node by its certificate | per grant; owner files with `admin` | yes (TLS protects them) |
| Setup access point (unclaimed node, window open) | the setup secret (WPA2 key and claim) | one claim through `/dev/setup`, no other path | yes (the claim carries the Wi-Fi key) |
| Anyone | nothing | nothing; reads `/proc/board` and `GET /board` only | no |

And across all of them:

1. **The factory part is written by the console and `tools/provision.py` only.** No network path, admin or not, writes it.
2. **An owner reset is physical:** the console, or holding the button 15 s. The network cannot unclaim a node, and the last `admin` grant cannot be removed over it.
3. **Update images are signed** with the release key once one is stored (`otakey set`) or the build requires it. A damaged signature is refused everywhere; an unsigned image only through the console's one-time override. Images arrive on any writing path above; the signature, not the path, is what makes them trustworthy.
4. **The node makes its own HTTPS key** and never serves it. The owner's CA (`host/ca`) signs the node's request; a certificate for another key is refused. The key sits in the owner part, so an owner reset removes it.
5. **Before claiming,** nothing on the network may write: there are no grants, and the setup window opens only on the access point, for one claim, closing after five wrong secrets or 15 minutes.

## Consequences
* **Positive**: Every write from the network is authenticated, integrity-protected (protected session or TLS) and, for the owner part, admin-only. A passive listener on a LAN learns no secret; an active one can forge, replay or alter nothing.
* **Positive**: A board too small for TLS (the Pico 2 W, the C6-Zero) is administrable with the same guarantees except confidentiality, and keeps its secrets off the network.
* **Negative**: On a board without TLS, a secret (a new Wi-Fi key, a grant) can only be set from the console, a cable or the setup access point.
* **Negative**: HTTPS needs a root certificate installed on every device that opens the admin page (`doc/admin/tls.md`), and certificates expire after 825 days.
