# The web admin transport: 9P over WebSocket, over HTTP or HTTPS

**Status:** built in phase 52 (52.11, 52.15). Implementations: `net/httpd.c` and `net/websocket.c` (the server), `net/tls.c` (TLS 1.3), `host/p9lib` (`connect_ws()`, with `tls=` for `wss://`), and the admin page `tools/sd_root/www/admin.html`. Tests: `tests/host/websocket_host.c`, `tests/host/tls_host.c`, and the QEMU tests "Admin Web Server" and "HTTPS Admin" in `tests/runner.py`. Who may do what over it: [ADR 006](../../history/adr/006_remote_admin_trust_model.md).

## 1. Endpoints

| Request | Answer |
| :--- | :--- |
| `GET /`, `/index.html`, `/admin.html` | The admin page. To a client that sends `Accept-Encoding: gzip` (every browser), the copy built into the image, gzip-compressed, so the page always matches the firmware. Otherwise `/flash0/www/admin.html` or `/sd0/www/admin.html` if there is one, else 406. On port 80 of a node serving HTTPS: 302 to `https://HOST/`. |
| `GET /board` | The public facts of `/proc/board`, as text, before anyone has signed in. |
| `GET /9p` with `Upgrade: websocket` | A WebSocket (RFC 6455) carrying 9P, below. The client may name the subprotocol `9p`. |
| anything else | 404 (`GET`), 405 (other methods). |

On a node's setup access point every request but `/9p` and `/board` goes to the setup page instead (`doc/admin/identity.md`).

## 2. 9P over the WebSocket

Each 9P message travels as one **binary** message (fragmented or not), in both directions; text messages close the connection (1003). The client masks its frames, as RFC 6455 requires; the server does not. The link is a `p9_link_t` with `auth_required`: nothing is reachable before `Tauth` and `Tattach` with a granted key, exactly as over TCP. msize is what `Tversion` agrees, at most the node's `P9_MAX_MSIZE` (2048 on the RP2350 and the ESP32-C6, 4096 elsewhere).

Over plain HTTP the link is plaintext: a client that offers `9P2000.lp` gets a [protected session](protected_9p.md) and may write; one that does not reads only, unless the console said `httpd write on`. On a node that serves HTTPS, plain HTTP reads only, protected or not. Over HTTPS the link is not plaintext: `9P2000.lp` is declined (TLS already protects every byte), writes follow the grant, and secrets may be written.

Ping and pong keep a quiet session alive: the server pings after 15 s without a frame and closes after 45 s without an answer. A closed WebSocket releases its fids (`p9_link_closed()`).

## 3. TLS (port 443)

`net/tls.c` is a TLS 1.3 server (RFC 8446) with one choice each:

| | |
| :--- | :--- |
| version | TLS 1.3 only (`supported_versions`); a TLS 1.2 client gets `protocol_version` |
| cipher suite | `TLS_CHACHA20_POLY1305_SHA256` |
| key exchange | X25519; a HelloRetryRequest when the client offered X25519 in `supported_groups` but sent another share |
| signature | `ecdsa_secp256r1_sha256`, with the node's own key (`kernel/tls_cert.h`) |
| certificate | the leaf the owner's CA issued; no chain is sent (the root is installed on the client) |
| records | up to 16 KB + 256 in, 4 KB out; ChaCha20-Poly1305 with the per-record nonce of section 5.3 |
| after the handshake | KeyUpdate (answered when asked), close_notify; no NewSessionTicket |
| not built | PSK, resumption, 0-RTT, client certificates, ALPN, `record_size_limit` |

A handshake message must arrive within one record. The server sends a ChangeCipherSpec after its first ServerHello when the client sent a session id (middlebox compatibility, Appendix D.4).

The engine is transport-independent: `tls_server_init()` takes non-blocking `recv` and `send` functions and two buffers (`TLS_RECORD_MAX`, `TLS_TX_CAP`); httpd hands it a TCP stream and buffers from the bulk zone, `tests/host/tls_host.c` a socket.

---

**Navigation:** [<- Developer Manual](../index.md) | [Documentation Index](../../README.md)
