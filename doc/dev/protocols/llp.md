# LLP — the LugalOS link protocol, version 1.0

**Status:** specified and host-tested in 49.1 (`plan/phase49_c6_network_nodes.md`).
Implementations: `kernel/llp.c` (C, every target) and `host/llp/` (Python
reference). Where this document and either implementation disagree, this
document is wrong or the code is. Either way it is a bug, and
`tests/host/llp_vectors.txt` exists to find it.

## 1. What it is for

LLP connects two LugalOS nodes that sit next to each other: an ESP32-P4 and the
ESP32-C6 on the same board (over SDIO), or an RP2350 and an ESP32-C6-Zero (over
a UART). One of them, the **radio**, has a Wi-Fi station. The other, the
**host**, owns the IP address. Over one channel LLP carries:

* **Ethernet frames.** The host's own IP stack reaches the network through the
  radio, which only forwards frames.
* **9P messages.** Each node's namespace is reachable from the other. Control
  of the radio (scan, join, status) is a set of 9P files, not a command set of
  LLP's own.
* **What neither can do for itself:** version check, session start, flow
  control on channels without it, and the radio's link state.

The reasoning behind the split is in `plan/phase49_c6_network_nodes.md` §1:
frames, not 9P alone, because the host's outbound protocols (MQTT, NTP, a 9P
client, a future web server) need its own IP stack.

## 2. Conventions

* All multi-byte integers are **little-endian** (as in 9P).
* `u8`, `u16`, `u32`: unsigned; `i8`: two's complement.
* Sequence numbers are 8-bit and compared modulo 256: `a - b` is computed as a
  `u8`. A difference below 128 is "ahead", 128 or more is "behind".
* Times are milliseconds on a free-running `u32` clock, compared by
  difference, so they wrap safely.

## 3. Frames

### 3.1 Header

Every frame, on every channel:

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | u8 | `type` | `0x01` LINK, `0x02` ETH, `0x03` P9 |
| 1 | u8 | `seq` | data frames: sequence number (§5.3); LINK: 0, ignored |
| 2 | u16 | `len` | payload length in bytes |
| 4 | `len` | payload | |

A receiver drops a frame whose `len` disagrees with the bytes the channel
delivered, and a frame of an unknown type. Both are counted, neither is
answered. New types require a new minor version, and are sent only to a peer
whose HELLO announced that minor version or a later one.

### 3.2 Payloads

* **ETH:** one Ethernet II frame, destination MAC first, no FCS, 14 to 1514
  bytes. Shorter is dropped.
* **P9:** one whole 9P2000 message. Its `size[4]` field must equal `len`, and
  `len` ≥ 7. Anything else is dropped. Both directions carry both T- and
  R-messages: the 9P layer tells them apart by type parity, as `fs/p9_link.c`
  already does.
* **LINK:** first byte is the operation (§4–§6). A LINK body shorter than its
  operation needs is dropped. Bytes beyond what the operation defines are
  ignored, so a later minor version may extend a body.

### 3.3 Sizes

* The protocol's ceiling is `len` ≤ **4096**.
* Each side announces the largest payload it accepts (`max_payload`, in HELLO).
  It must be at least **1514**: every node takes a whole Ethernet frame.
  A sender never exceeds its peer's value, and a receiver drops anything
  larger than its own.
* The 9P msize the two nodes negotiate must fit in the smaller `max_payload`.
  9P's own `Tversion` takes care of that.

## 4. Session and handshake

### 4.1 HELLO (`op 0x01`, 22 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | `op` = 0x01 |
| 1 | u8 | `major` (1) |
| 2 | u8 | `minor` (0) |
| 3 | u8 | `role`: 1 host, 2 radio |
| 4 | u8 | `flags`: bit 0 `UP` (the sender considers the link up), bit 1 `FLOW` (the sender's receive side uses LLP credits, §5) |
| 5 | u32 | `session`: the sender's, nonzero, new at every start |
| 9 | u32 | `peer_session`: the last `session` the sender saw from us, 0 if none |
| 13 | u16 | `max_payload` |
| 15 | u8 | `window`: with `FLOW`, the data frames the sender can accept now (§5.2); otherwise 0 |
| 16 | 6 | `mac`: the radio's station MAC; zeros from a host |

### 4.2 The rules

A link is **DOWN**, **UP** or **MISMATCH**. It starts DOWN and sends a HELLO at
once. While DOWN it repeats the HELLO every **500 ms**.

On receiving a HELLO with `session` = 0, drop it. Otherwise:

1. **A new peer** (`session` differs from the one recorded):
   1. Record the peer: its session, version, role, flags, `max_payload`,
      `window` (as the send window, with the send acknowledgement at 0) and
      MAC.
   2. Forget the old session, in both directions: send sequence = 0, receive
      sequence = 0.
   3. If the link was UP, it goes DOWN (event *down*).
   4. If the peer's `major` differs from ours, or its `max_payload` < 1514,
      the link goes to MISMATCH (event *mismatch*). Send one HELLO, so the
      peer finds out too, and stop.
2. **The same peer while in MISMATCH:** ignore the HELLO. MISMATCH ends only
   when the peer restarts with a new session.
3. If the HELLO's `peer_session` is our `session`, the peer has seen this
   session of ours:
   * if not UP, the link goes UP (event *up*);
   * if the HELLO's `UP` flag is clear, answer with a HELLO.
4. Otherwise, answer with a HELLO.

This converges in three HELLOs when one side is already listening when the
other starts, and in six when both start at once (the `handshake` vectors show
the exchange). It never loops: a HELLO is only answered when the peer has not
yet seen us, or does not yet consider the link up, and both of those stop being
true.

### 4.3 Restarts

A node that restarts picks a new `session`. Its peer sees a new peer (rule 1),
drops everything about the old session and handshakes again. Nothing about a
session survives it: a frame in flight from the old session arrives as
sequence-number garbage and is dropped as stale or counted as lost.

### 4.4 Versions

* **Major** differs: no data passes, and both sides say so (event *mismatch*,
  the binding logs both versions). The two dies on a board are separate builds,
  flashed separately, so this will happen. It must be loud, not silent.
* **Minor** differs: the link comes up. Each side uses only what the smaller
  minor version defines.

## 5. Flow control

### 5.1 Who needs it

* **SDIO** has flow control in hardware: the C6 slave's receive-buffer tokens
  and its packet-length register. Neither side sets `FLOW`, and LLP sends data
  whenever the channel takes it.
* **A UART** has none. The receiver sets `FLOW` and LLP's credits apply.

A sender applies credits when the *peer's* HELLO had `FLOW` set.

### 5.2 Credits

The receiver tells the sender how many data frames it may send beyond the last
one the receiver has accounted for:

**CREDIT** (`op 0x02`, 3 bytes): `u8 ack`, `u8 window`.

* `ack` is the receiver's next expected sequence number. Every frame before it
  has arrived or is known lost (§5.3).
* `window` is how many data frames it can take now.
* The sender may send while `(tx_seq - ack) mod 256 < window`.
* `window` ≤ 64, so sequence arithmetic stays unambiguous.

When the receiver sends CREDIT:

* when the far edge of its window (`ack + window`) has moved by at least a
  quarter of its configured window (at least one frame) since it last
  advertised it, checked whenever the binding calls tick, which it does after
  consuming data;
* in answer to every PROBE;
* as the keepalive (§6).

A CREDIT that would acknowledge frames never sent is dropped. Data frames only
consume credit. LINK frames never do, and a receiver must always be able to
take a LINK frame.

### 5.3 Sequence numbers, loss and PROBE

Data frames (ETH and P9 share one sequence space per direction) carry `seq`,
starting at 0 per session.

* `seq` ahead of the expected one by *g* (< 128): *g* frames were lost (CRC
  failures on a UART, for example). Count them, accept this frame, and expect
  `seq + 1` next.
* `seq` behind (≥ 128): stale or duplicated. Drop it.

A sender that has been held back for credit for **100 ms** sends:

**PROBE** (`op 0x03`, 2 bytes): `u8 next`, the sequence number it will use
next.

It repeats this every 100 ms while still held back. The receiver treats every
frame before `next` that has not arrived as lost, advances to `next` and
answers with a CREDIT. Without this, a lost data frame would hold its credit
forever.

**Ordering rule:** a binding may send LINK frames ahead of queued data frames
(HELLO, CREDIT, STATE), but **never a PROBE**. A PROBE says "everything before
`next` has been sent". If it overtook a data frame, the receiver would skip
past that frame and then drop it as stale.

## 6. Liveness and the radio's state

* **Keepalive:** a side that is UP and has sent nothing for **1000 ms** sends
  a CREDIT (`window` 0 on a link without `FLOW`, which the peer then ignores
  for flow purposes).
* **Dead peer:** a side that is UP and has received nothing for **3000 ms**
  goes DOWN (event *down*) and starts sending HELLOs. It keeps its session, so
  a peer that was only cut off, not restarted, comes back UP with sequence
  state intact.
* **STATE** (`op 0x04`, 5 bytes): `u8 link` (1 = the Wi-Fi station is
  associated and passing frames), `i8 rssi` (dBm), `u16` reserved (0). Sent by
  the radio when either changes, and once on every *up* event (the radio's
  binding does this; `llp.c` only carries it). Accepted only while UP (event
  *peer_state*). On SDIO the same two values are also mirrored
  in a slave register (§7.2).

## 7. Bindings

### 7.1 Serial (UART, a USB CDC port, a test pipe)

Each frame is sent as

```
SLIP_END  escape( header | payload | crc32 )  SLIP_END
```

* SLIP is RFC 1055 (`END` 0xC0, `ESC` 0xDB, `ESC_END` 0xDC, `ESC_ESC` 0xDD), as
  in `drivers/slip.c`.
* `crc32` is CRC-32/ISO-HDLC (the zlib/Ethernet CRC: reflected polynomial
  0xEDB88320, init and final XOR 0xFFFFFFFF; check value 0xCBF43926), over the
  header and payload, appended little-endian.
* A receiver ignores empty frames (repeated `END`s). It drops, counting each
  case separately:
  * frames shorter than header + CRC (*runt*);
  * frames failing the CRC (*crc_err*);
  * frames whose header `len` disagrees with their size (*bad_len*);
  * frames longer than its buffer, which are dropped whole up to the next
    `END` (*overflow*).
* Worst case on the wire: `2 + 2 × (4 + len + 4)` bytes.
* The receiver sets `FLOW`. Its `window` is the number of frames its binding
  can buffer, at most 64.

### 7.2 SDIO (P4 host ↔ C6 slave): outline, fixed in 49.6

* Frames are packed back to back in one CMD53 transfer, each padded to a
  4-byte boundary. The transfer is padded to the 512-byte block. The bus CRC
  covers integrity, so there is no LLP CRC.
* Host → C6: the host writes only as many packets as the slave's
  receive-buffer token count allows. That is the flow control, so `FLOW` is
  not set.
* C6 → host: the slave raises its interrupt and the host reads the pending
  length.
* The radio's `link` and `rssi` are also kept in a slave shared register, so
  the host can read them without a frame.

## 8. Conformance

`tests/host/llp_vectors.txt` is generated by the Python reference
(`cd host/llp && uv run llp-vectors > ../../tests/host/llp_vectors.txt`) and
replayed by `tests/host/llp_host.c` against the C implementation. It contains:

* the serial binding's exact bytes for a set of frames, including every SLIP
  escape and the largest Ethernet frame;
* receiver streams with their expected counters (CRC error, runt, bad length,
  overflow followed by a good frame);
* scenarios: two links driven step by step (handshake, credits, a lost HELLO,
  a restart, a version mismatch, PROBE recovery, a dead peer, STATE, no-FLOW
  links, a full receive buffer, duplicates), with every frame, event and
  delivery each step must produce.

An implementation conforms when it reproduces the file exactly. A change to
the protocol is a change to the reference, a regenerated file, and the C
implementation following it.
