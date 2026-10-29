# Protected 9P sessions (9P2000.lp)

**Status:** specified in 52.11b (`plan/phase52_remote_admin.md`). Implementations: `fs/p9_protect.c` and
`fs/p9_link.c` (C), `host/p9lib` (Python), the admin page `tools/sd_root/www/admin.html` (JavaScript).
`tests/host/p9_protect_host.c`, `tests/web/admin_core_check.js` and `p9lib.client` all check the same
vectors (section 4), and `tests/runner.py` runs a proxy that tampers with a live session.

## 1. What it is for

The 9P auth gate (a HMAC over a challenge, see [identity](../../admin/identity.md)) proves who attached.
After that nothing protects the conversation. On a link without TLS (a TCP connection on a LAN, or a
WebSocket over plain HTTP) anyone who can inject a packet can send a write as the owner, replay a
captured one, or alter a reply. A protected session closes that with the SHA-256 the tree already has.

**This is integrity, not confidentiality.** Every frame is readable on the wire. So the server also
refuses to take a secret over such a link, protected or not (section 6).

## 2. Negotiation

The client offers the extension in `Tversion`: the version string `9P2000.lp` instead of `9P2000`. A
server that protects the link answers `9P2000.lp`. Any other answer (an older node, a cable, a local
channel) means a plain session, and the client carries on as before.

Which links protect: a TCP connection a node accepts, and a WebSocket session. Not a cable or a local
channel, which are trusted by the same argument that trusts the boards.

A client that does not offer the extension still gets a session on such a link, **read-only** (the 9P
server treats the link as `P9_AUTH_REQUIRED_RO`). The console can allow plain writes over HTTP with
`httpd write on`.

## 3. The session key and the tags

After the server accepts the auth response, both ends derive

    PRK = HMAC-SHA-256(key = nonce, msg = grant key)
    K   = HMAC-SHA-256(key = PRK, msg = "LugalOS 9P v1" 0x00 uname 0x00 aname 0x00 0x01)

(HKDF-SHA-256 with the challenge as salt and one output block.) The grant key never crosses the wire;
`uname` and `aname` are those of the `Tauth`.

The `Rwrite` that accepts the auth response is the **last plain frame** in both directions. From the
next frame on, each frame is followed by an 8-byte tag:

    tag = HMAC-SHA-256(K, dir || seq || frame)[0..8)

`dir` is one byte, 0 for client to server and 1 for server to client. `seq` is four bytes, little
endian: the number of frames already sent in that direction since activation, from 0. `frame` is the
whole 9P message, its own `size[4]` included, but not the tag. A TCP stream carries `frame || tag`
with `size` unchanged; a WebSocket message carries `frame || tag` too. A frame may be up to `msize`
including the tag, so the node answers with at most `msize - 8` bytes.

## 4. Test vectors

    grant key  01 02 ... 10 (16 bytes)      nonce  64 65 ... 83 (32 bytes: 100..131)
    uname "lugal"   aname "/"
    K          efd2818fdd1968a223551d03afd03e7dfc82e74335e776e5fbfe0426f39acb39
    frame      0b000000 64 ffff 00100000 ... (11 bytes: 0b 00 00 00 64 ff ff 00 10 00 00)
    client to server, seq 0   0d6abc4a07e0f2ff
    server to client, seq 0   0c08bacd3c090cf0
    client to server, seq 7   212ed7169c0581e4

## 5. What fails, and what then

The first frame that is missing its tag, has the wrong tag or the wrong position (altered, replayed,
reordered, dropped before it, injected, reflected from the other direction) ends the session: the node
logs it, forgets the key and closes the connection. There is no recovery inside a session; the client
authenticates again on a new connection. A second `Tversion` on a protected connection is an error: the
key belongs to one connection and one auth.

A reply the client cannot verify is an error on the client, which closes.

## 6. Secrets stay off a readable link

A node refuses a write that carries a secret when it arrives over a link without TLS, protected or
not: a WLAN key (`wlan` in `/dev/owner`), a grant's key (`grant`), the owner key and WLAN of a claim
(`/dev/setup`). The rest of `/dev/owner` (`name`, `ungrant`, `setup start|stop`) is allowed to an admin
over a protected session. The console, a cable, TLS (52.15) and the setup access point (52.12) are
exempt.

---

**Navigation:** [Developer Manual](../index.md) | [LLP](llp.md) | [Documentation Index](../../README.md)
