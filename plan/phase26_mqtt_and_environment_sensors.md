# Phase 26 — A node that measures something, and says so

**Status: planned 2026-09-05.** Succeeds `plan/phase19_ip_stack_and_ethernet.md`
(concluded) and stands beside `plan/phase24_dcf77_precision_and_ntp_server.md`
(planned, in progress). It needs nothing from phase 24 and phase 24 needs
nothing from it; they share only the stack underneath.

**Scope.** Two halves that meet in one appliance.

* **An MQTT 3.1.1 client** over the IP stack this project already wrote, with
  optional username/password authentication and **no encryption** — publish,
  subscribe, keepalive, last will, and a reconnect that survives a router
  rebooting. Tested against a real broker at `nalanda` (192.168.178.32:1883),
  and, in CI, against a small broker of our own on the host.
* **BMP280 and BME280 environment sensors** on the I2C bus the RTC and the
  EEPROM already share — pressure and temperature on both parts, humidity on
  the one that has it — riding the existing shared `i2c` driver task rather
  than opening a second bus arbiter.

And the thing that makes both worth having: a **sensor persona** that joins
its WiFi, samples on a period and publishes, from a USB power adapter, with
no console attached and nothing typed.

**The one piece of new plumbing under all of this** is not MQTT and not I2C:
it is a **raw TCP byte stream**. Today `net/tcp.c` hands out `p9_link_t`s and
nothing else, so the only thing that can be spoken over a connection is 9P.
That is §2, it is Q0, and it is perhaps 120 lines.

**Out of scope, explicitly:** TLS, MQTT 5.0, QoS 2, a DNS resolver, a broker
of our own on the board, Home Assistant discovery, and any sensor that is not
on I2C. Each is argued in §8 rather than merely listed.

---

## 0. Why this is a phase, and why it is a small one

Every layer this needs already exists and has been exercised on real hardware:

| What an MQTT sensor node needs | Where it already is |
|---|---|
| An address on a wireless segment | R5 + `netcfg`, unattended since phase 19 |
| Outbound TCP (active open) | R3b, `tcp_connect()` |
| A periodic background service that yields | `wifiup`, `netsrv`, `dcf77_p0log` |
| A shared, isolated I2C bus master | M4.5's `i2c` task, U-mode since M5 |
| Per-board persistent configuration | The identity record (I6/I7a) |
| Deterministic tests with no hardware | `tests/netpeer.py`, QEMU slirp |

So the honest description of this phase is **assembly plus two protocols**,
where one protocol (MQTT 3.1.1) is a byte-counting exercise with one varint in
it, and the other (BMP280/BME280) is a register map plus the fixed-point
compensation arithmetic from a datasheet. Neither is a state machine on the
scale of TCP, and neither parses anything from an untrusted peer — with the
one exception §3.5 names and bounds.

What that means for the schedule: **nothing here should be debugged by
flashing.** The varint codec, the packet builders and the compensation
arithmetic are all pure functions, all built for every target, and all tested
in QEMU before a sensor is wired to anything — the same argument
`CMakeLists.txt` already makes for `net/ntp.c` and `kernel/sha256.c`.

It is also worth saying what this phase finally closes:
`plan/raw_ideas.md` has carried *"I2C environment sensors"* and *"remote
environment sensors"* since the beginning, and phase 19 §7 promised DHCP "when
the first sensor persona wants one". The first sensor persona is this one, and
§8 answers that promise (the answer is still no, and the reason changed).

## 1. What is actually being built, end to end

A `Pico 2 W` with a BME280 on four wires, powered from a phone charger:

```
lsh> sensor
bme280 @0x76: 21.94 C, 1004.31 hPa, 47.8 %RH   (forced mode, 3 samples/read)

lsh> mqtt
mqtt: broker 192.168.178.32:1883, client-id "kitchen-sensor"
      state CONNECTED (up 4m12s), keepalive 60s, publishes 4, drops 0
```

and on the host, unattended:

```
$ mosquitto_sub -h nalanda -t 'lugalos/#' -v
lugalos/kitchen-sensor/status       online
lugalos/kitchen-sensor/temperature  21.94
lugalos/kitchen-sensor/pressure     1004.31
lugalos/kitchen-sensor/humidity     47.8
```

and, when the board is unplugged, the broker publishes the will the client
registered when it connected:

```
lugalos/kitchen-sensor/status       offline
```

That last line is the whole reason last-will is in scope and not deferred: a
sensor node's most important message is the one it *cannot* send.

## 2. The byte stream we do not have yet

`net/tcp.c` is written around one assumption stated in its own header
comment: an accepted connection **is** a `p9_link_t`. That was exactly right
for phase 19 — it is what made phase 18's auth gate, `p9auth` and the entire
host side work over TCP on the first day — and it is why `link_poll()` and
`link_recv_frame()` are written in terms of 9P's four-byte length prefix.

MQTT has its own framing and does not want ours. So Q0 adds a second view of
the same connection:

```c
/* An outbound connection carrying bytes, not 9P frames. Same slot table,
 * same buffers, same pump -- the framing is simply not applied. */
typedef struct tcp_stream tcp_stream_t;      /* opaque */

tcp_stream_t *tcp_stream_open(const uint8_t ip[IPV4_LEN], uint16_t port);
int  tcp_stream_ready(tcp_stream_t *s);      /* 1 up, 0 handshaking, -1 gone */
int  tcp_stream_read(tcp_stream_t *s, uint8_t *buf, uint32_t max);   /* 0 = nothing yet */
int  tcp_stream_write(tcp_stream_t *s, const uint8_t *buf, uint32_t len); /* bytes accepted */
void tcp_stream_close(tcp_stream_t *s);      /* graceful: FIN, not RST */
```

Four facts make this small rather than a second transport:

* **The receive path is already a byte buffer.** `link_recv_frame()` copies a
  prefix out of `c->rx` and `memmove()`s the rest down. `tcp_stream_read()` is
  that function with the framing check deleted.
* **The send buffer already compacts on acknowledgement.** The ACK path in
  `tcp_input()` drops the acked prefix and advances `tx_seq`, so appending to
  `c->tx + c->tx_len` is well-defined whenever there is room. That is what
  makes a *partial* write legitimate: `tcp_stream_write()` returns how many
  bytes it took, and the caller loops with `sched_yield()` — which is exactly
  right, because the task that drains the buffer is `netsrv`, not the caller.
  (`link_send_frame()` refuses a second frame outright instead; a 9P reply is
  all-or-nothing and a stream is not.)
* **No new memory.** `ensure_bufs()` already allocates
  `TCP_MAX_CONNS * P9_MAX_MSIZE * 2` on first use — 16 KB on RP2350 — and a
  stream takes one of the two existing slots. A board that dials a broker
  therefore pays nothing it did not already pay to serve 9P.
* **`tcp_service()` already leaves client connections alone.** Its 9P service
  and auto-FIN are both guarded by `!c->is_client`, which is precisely the
  distinction a stream needs.

**Two things Q0 must get right, and they are the review points.**

1. **A stream connection must never be offered to the 9P server.** `is_client`
   is true for one, but relying on that alone conflates "we dialled" with
   "this is not 9P" — and R3b's `(net-mount)` dials 9P deliberately. So a
   stream carries its own flag, and `conn_init_link()` is not called for it at
   all: a stream connection has no `p9_link_t` face, which is the only way to
   be sure nothing can reach it through one.
2. **The epoch check comes along unchanged.** A slot is reused; a handle held
   across a reconnect must fail cleanly rather than attach itself to a
   stranger's connection. This is the aliasing hazard `link_conn()` already
   solves, and `tcp_stream_*` solves it the same way rather than inventing a
   second scheme.

**The two-connection limit becomes visible here**, and that is worth stating
before someone meets it: a board that is serving 9P to two peers cannot also
dial a broker. On a sensor persona that is a non-issue (it serves nothing).
On a gateway that also publishes, it is a real constraint, and the release
valve is the same one phase 19 §8 named — raise `TCP_MAX_CONNS` and pay
8 KB per slot. Not raised speculatively here.

## 3. MQTT, and the parts of it we implement

Target: **MQTT 3.1.1** (protocol level 4). It is what `mosquitto` on
`nalanda` speaks by default, it is what every broker still speaks, and its
CONNECT packet is half the size of 5.0's — a version whose main additions
(properties, reason strings, shared subscriptions, session expiry) are all
things a sensor node with one topic does not use. §8 keeps 5.0 out with that
reasoning rather than by omission.

### 3.1 The wire, and the one piece of fiddly arithmetic

Every packet is a two-to-five-byte fixed header — a type nibble, four flag
bits, and a **Remaining Length** varint — followed by a variable header and a
payload. The varint is the only arithmetic in the protocol: seven bits per
byte, high bit as continuation, one to four bytes, maximum 268,435,455.

It is also the only place a wrong implementation silently desynchronises a
stream instead of failing, so it gets treated the way this project treats
checksums and epoch conversions: a pure function, built for every target,
tested exhaustively at the boundaries (0, 127, 128, 16383, 16384, 2097151,
2097152, 268435455) plus the two malformed cases (a fifth continuation byte,
and a length that never terminates before the buffer ends).

Packets implemented, and nothing else:

| Out | In |
|---|---|
| CONNECT | CONNACK |
| PUBLISH (QoS 0; QoS 1 behind Q8) | PUBLISH (QoS 0) |
| SUBSCRIBE / UNSUBSCRIBE | SUBACK / UNSUBACK |
| PINGREQ | PINGRESP |
| DISCONNECT | — |
| PUBACK (Q8, if it happens) | PUBACK (Q8) |

PUBREC/PUBREL/PUBCOMP are QoS 2 and are not implemented; a broker will never
send them unless we publish at QoS 2 or subscribe at QoS 2, and we do neither.

### 3.2 Authentication without encryption, stated plainly

The client supports username and password because the user asked for it and
because a shared broker usually wants it. What it does **not** do is hide
them, and this document says so once, clearly, so the README can repeat it:

> **The password crosses the LAN in the clear.** MQTT's CONNECT packet carries
> the username and password as plain length-prefixed UTF-8, and there is no
> TLS on this board. Anyone with a packet capture on the segment — or on the
> WiFi, if the AP is open — has the password and can replay it. Use a password
> that means nothing anywhere else, give each node its own, and use the
> broker's ACLs to limit what that account may publish to.

This is phase 18 §1's threat model inherited without change: *auth proves who
is talking, and does not hide what they say.* It was true of 9P over TCP and
it is true here. Two protocol details worth recording because they cause
support questions:

* In 3.1.1, **a password requires a username** (a password flag with no
  username flag is a protocol error). MQTT 5.0 relaxed that; we do not
  implement 5.0. A username with no password is legal and is what an
  `allow_anonymous false` broker with per-user ACLs but no passwords wants.
* **The client id must be unique on the broker.** Two nodes sharing one id
  disconnect each other in a permanent loop that looks exactly like a flaky
  network. We derive it from `node_name()`, which the identity record already
  makes per-board — so the fix for the loop is `identity name`, and the
  failure is nearly impossible to cause by accident.

### 3.3 QoS 0, and the argument for it

**Publishing is QoS 0 by default and that is the recommendation.** The reasons
are specific rather than "it is simpler":

* TCP already retransmits. Within a connection, QoS 1 adds nothing except an
  acknowledgement the transport layer effectively gave us.
* What QoS 1 genuinely protects is the *reconnect*: a publish in flight when
  the connection dies is lost at QoS 0 and retried at QoS 1. For a periodic
  measurement that is superseded a minute later, retrying the stale one is
  arguably worse than dropping it.
* QoS 1 costs a packet-id allocator, an in-flight table, a retransmit timer
  and a `DUP` flag — the beginnings of exactly the machinery TCP already has.

**Q8 implements QoS 1 publish anyway, as an optional milestone, and only if
something asks for it** — an event that is *not* periodic (a door contact, a
threshold crossing) is the case where losing one message matters, and the
node that has one will make the case. With one in-flight message and no
pipelining it is perhaps 60 lines. Written down here so it is a decision
rather than an omission.

### 3.4 Keepalive, will, and the reconnect that actually matters

* **Keepalive 60 s.** PINGREQ goes out when nothing else has been sent for
  ~45 s (three quarters of the interval, so a slow round trip does not race
  the broker's 1.5× timeout). A missing PINGRESP after one further interval is
  a dead connection: close and reconnect, do not wait for TCP to notice.
* **Last will**, registered in CONNECT: topic `lugalos/<node>/status`, payload
  `offline`, **retained**. On a successful connect the client immediately
  publishes `online` retained to the same topic. That pair is what makes a
  subscriber able to tell "quiet because nothing changed" from "gone".
* **Reconnect with backoff, and no giving up.** 1 s, 2 s, 4 s … capped at
  30 s, with logging that stops after the first few attempts. This is
  deliberately the same shape and the same reasoning as R5's `wifiup` retry
  (README §4c): the power-cut case is a board and its router coming back at
  the same moment, where the router is slower — so the one attempt made at
  boot is exactly the attempt that fails.
* **Every wait loop consults `console_interrupt_requested()`**, at the same
  cheap interval `ntp_query()` uses, and returns `MQTT_ERR_INTERRUPTED`. This
  is the project's standing rule for blocking waits and it is not optional: a
  `mqtt pub` typed at a prompt against an unreachable broker must be
  Ctrl-C-able.

### 3.5 The one place we parse a stranger's bytes, and how it is bounded

Inbound PUBLISH is the only untrusted input in the client, and a broker we
subscribed to can send an arbitrarily large one. The rules:

* A fixed **512-byte** reassembly buffer. A packet whose Remaining Length
  exceeds what we can hold is **not** dropped — its bytes are *counted and
  discarded* until the packet ends, and a counter increments. Dropping without
  consuming would leave the stream desynchronised at a packet boundary
  forever, which is the classic way a small MQTT client wedges.
* A malformed varint (a fifth continuation byte) closes the connection. There
  is no resynchronising a stream whose framing is wrong, and pretending
  otherwise is how a parser starts executing whatever follows.
* Topic and payload lengths are checked against the *declared* remaining
  length before either is read, not after.

## 4. BMP280 and BME280

### 4.1 One driver, two parts, and how it finds out which

The parts are register-compatible except that the BME280 adds humidity. So
the driver probes and adapts rather than being configured:

* **Address** 0x76 (SDO tied low) or 0x77 (SDO high) — both probed, in that
  order, so a module with either strapping just works.
* **Register 0xD0** is the chip id: **0x60 = BME280**, **0x58 = BMP280**
  (engineering samples read 0x56/0x57 and are accepted as BMP280). Anything
  else is refused with the value printed — notably **0x55 is a BMP180**, a
  different part with a different register map that a lookalike breakout may
  carry, and **0x61 is a BME680**. Naming them here saves the next person an
  afternoon.
* Calibration at 0x88–0xA1 (dig_T1–T3, dig_P1–P9, dig_H1), plus 0xE1–0xE7
  (dig_H2–H6) on a BME280. Read **once** at init, not per sample.
* **Forced mode**, not normal mode: wake, take one measurement at the
  configured oversampling, return to sleep. It is the right mode for a node
  that samples once a minute, and it matters for accuracy rather than for
  power — a continuously converting BME280 self-heats, and the temperature
  reading is the first casualty. Phase 17 spent real time on a 1–4 °C offset
  on the DS3231's die sensor for exactly this class of reason; the same
  mistake is avoidable here by construction.
* **`ctrl_hum` (0xF2) must be written before `ctrl_meas` (0xF4)** — the
  humidity oversampling setting only takes effect on the *next* write to
  `ctrl_meas`. A driver that writes them in the obvious order silently reads
  humidity at the wrong oversampling, or zero. This is the single most common
  BME280 bug and it is one line in the right order.

### 4.2 The compensation is arithmetic, so it is tested without hardware

The raw registers are meaningless: temperature and pressure are 20-bit
values, humidity 16-bit, and the datasheet's fixed-point compensation
formulas turn them into 0.01 °C, Q24.8 pascals and Q22.10 %RH using the
per-part calibration constants. `t_fine` from the temperature step feeds both
of the others, so an error in temperature quietly corrupts all three.

This is precisely the shape of thing this tree refuses to debug by flashing.
So:

* The compensation functions are **pure**, take raw values plus a calibration
  struct, and are compiled for every target including both QEMU ones.
* A **`sensor selftest`** command (shell and Lisp) runs a built-in vector —
  a fixed calibration block and fixed raw readings — and prints the three
  results. It needs no I2C and no hardware, so it runs in CI on rv32 and
  rv64.
* The expected values come from an **independent implementation**: a Python
  transcription of the same datasheet formulas in the test file, which must
  agree **bit for bit** with the C. Two implementations of the same integer
  arithmetic, written from the datasheet rather than from each other — the
  method R5 used with embassy-rs as an independent reference, and it is what
  makes agreement mean something.
* Once a real part is in hand, one **golden vector** captured from it (raw
  registers + its own calibration + the values our driver produced, plus a
  reading from a second thermometer) is added to the same test. The synthetic
  vector proves the arithmetic; the golden one proves the register map.
* The 64-bit pressure formula is used, not the 32-bit one. `net/ntp.c`
  already does signed 64-bit division on rv32, so the libgcc helpers this
  needs are proven present; the 32-bit variant's documented accuracy loss buys
  nothing.

### 4.3 Riding the existing I2C task, with one generic operation

`drivers/i2c_rtc.c` owns the bus: a U-mode task since M5, reached by
`i2c_task_call()` with an opcode-per-operation protocol
(`I2C_OP_RTC_READ_TIME`, `I2C_OP_EE_WRITE`, …). `drivers/at24c32.c` already
routes through it rather than duplicating arbitration, and that precedent is
the one to follow — a second bus master on the same pins is not a design, it
is a race.

But adding `I2C_OP_BME_READ_CALIB`, `I2C_OP_BME_READ_RAW`, … would grow the
U-mode dispatch for every future part. Instead, **one generic operation**:

```
I2C_OP_XFER: addr, wlen, w[wlen], rlen  ->  status, r[rlen]
```

write-then-repeated-start-read, which is every register access these parts
need and every register access an I2C device generally needs. Then:

* `drivers/bme280.c` is **entirely M-mode-side**: it builds requests, calls
  `i2c_task_call()`, and does the compensation. It never runs in U-mode, so
  it does **not** need adding to `CMakeLists.txt`'s `-fno-jump-tables` list,
  and the U-mode hazards catalogued in M5 Phase 4 do not apply to it.
* The U-mode task grows exactly **one** case, in a file already built with
  `-fno-jump-tables` and already hardened.
* Every future I2C part — a CardKB, an INA219, an SHT4x — is then a
  self-contained M-mode file with no kernel-surface change at all. That is
  the actual payoff, and it is worth more than this phase's use of it.

**Is a generic transfer a loss of safety?** No, and the reason is worth
stating so it is not re-litigated: the U-mode isolation of the `i2c` task
protects *the kernel from a bus driver's bugs*, not *the bus from its
callers*. Every caller is kernel-side code in this same tree; a generic
opcode gives them nothing they could not already have by adding a case. The
bound that does matter — request and response sizes — is enforced by
`I2C_REQ_CAP`/`I2C_RESP_CAP` exactly as it is today.

**Pins: none are new.** The BME280 goes on the RTC's own I2C0 (GP4/GP5 on
every RP2350 persona), at 0x76/0x77, alongside a DS3231 at 0x68 and an
AT24C32 at 0x50. `i2c scan` already lists what answers. On the
`rp2350-wifi` persona those pins are declared and currently answer nothing,
so **a BME280 can be fitted to that board with no board-file change at all** —
which is what makes Q4 testable before any new persona exists.

## 5. Configuration: where the broker's address lives

The same argument the address itself settled (README §4b): **the identity
record, not `init.lisp`.** The `/flash0` image is byte-identical on every
board since I7a and that is worth keeping; the record is already per-board,
already survives reflashing, and already holds a credential (the WLAN PSK).

So one new TLV field, `IDSTORE_FIELD_MQTT = 8`, holding:

```
broker ip[4], port u16, keepalive_s u16, period_s u16,
username (len-prefixed, may be empty), password (len-prefixed, may be empty),
topic prefix (len-prefixed, default "lugalos")
```

with `tools/provision.py --mqtt HOST:PORT[,user,pass]` writing it from the
host and `mqttcfg` writing it from the shell, both mirroring `netcfg`'s
existing shape down to `mqttcfg clear` and a bare `mqttcfg` reporting what is
stored. The client id is `node_name()` and is not stored separately — one name
per node, used for 9P `uname`, for the topic and for the client id.

**The password is stored in the clear in the record**, like the WLAN PSK, and
`/proc/mqtt` and `mqttcfg` print it as `set` rather than as its value — the
same rule §6 of phase 21 applies to the device key. A record dump on the host
shows it; that is a property of an unencrypted store on a device someone can
pick up, and phase 21 §"What it does not defend" already covers it.

**Stored broker + stored WLAN credentials = the intent to publish**, with no
separate enable flag — the rule R5 already established for joining. A board
with no stored broker runs `mqttd` not at all.

## 6. Milestones

Q0 gates Q1–Q3. **Q4 depends on nothing** and can be built in parallel or
first, which is deliberate: it is the half that might wait on a part arriving.

### Q0 — A byte stream over our own TCP
`tcp_stream_open/ready/read/write/close` on the existing connection table
(§2). No new allocation, no new state machine, a stream connection has no
`p9_link_t` face, and the epoch check carries over.
**Exit:** a QEMU test opens a stream through slirp to a host-side echo server,
round-trips 64 KB in both directions with the writer looping over partial
writes, and closes gracefully (FIN, both ways, no RST). `net` still reports
the connection. Existing TCP and 9P tests unchanged and passing.

### Q1 — The client core: connect, publish, ping, disconnect
`net/mqtt.c` + `net/include/net/mqtt.h`. Varint codec, CONNECT (with
optional username/password and a will), CONNACK with all five return codes
named in text, PUBLISH out at QoS 0, PINGREQ/PINGRESP, DISCONNECT. Built for
every target.
**Exit:** the varint boundary test passes on rv32 and rv64; a QEMU test
publishes to `tests/mqttbroker.py` (§7) and the broker asserts the exact bytes
of the CONNECT and PUBLISH; a wrong password produces CONNACK 0x04 and a
distinguishable error, not a hang.

### Q2 — The surfaces: `mqtt`, `/proc/mqtt`, Lisp
`mqtt connect [host[:port]] | pub <topic> <payload> | status | disconnect`,
`(mqtt-connect …)` / `(mqtt-publish "topic" "payload")` /
`(mqtt-status)`, and `/proc/mqtt` reporting broker, client id, state,
uptime, keepalive, counters (published, received, reconnects, oversize
drops) — inside the 896-byte `proc_buf`, like `/proc/net`.
**Exit:** a QEMU test drives a publish from the shell *and* from Lisp and
sees both at the host broker; Ctrl-C interrupts a publish to a black-holed
address within a second.

### Q3 — Inbound: subscribe and receive
SUBSCRIBE/SUBACK (granted QoS checked, 0x80 = refused and said so),
UNSUBSCRIBE/UNSUBACK, inbound PUBLISH at QoS 0 with §3.5's bounds. Delivery
to a registered callback; `mqtt sub <filter>` prints arrivals at the shell.
**Exit:** a QEMU test subscribes, the host broker publishes to it, and the
board prints the payload; the oversize path is exercised with a 4 KB payload
and the *next* small publish still arrives, proving the stream stayed in sync;
a malformed varint closes the connection rather than wedging it.

### Q4 — The sensors
`I2C_OP_XFER` in the shared task; `drivers/bme280.c` with probe, calibration,
forced-mode read and the datasheet compensation; `sensor` and
`sensor selftest` in the shell, `(sensor-read)` in Lisp, `/proc/sensors`.
**Exit:** `sensor selftest` produces bit-identical results to the Python
reference on both QEMU targets in CI; on hardware, a BME280 on the
`rp2350-wifi` persona reports plausible values that track a hand-held
reference within a degree, `i2c scan` shows RTC and sensor coexisting, and
`i2c_task_call_count()` shows both routed through the one task.

### Q5 — `mqttd`, the service that ties them together
A background task: connect, publish `online` retained, sample every
`period_s`, publish each measurement to `lugalos/<node>/<name>`, keepalive,
reconnect with backoff, will on death. Yields properly; visible in `ps`.
**Exit:** in QEMU with a fake sensor source, 30 published cycles with the
host broker killed and restarted in the middle — the node reconnects,
republishes `online`, and the broker sees the will in between. On hardware,
the same against `nalanda`.

### Q6 — Persistence and provisioning
`IDSTORE_FIELD_MQTT`, `mqttcfg` (+`clear`), `tools/provision.py --mqtt`,
`/proc/node` reporting the broker, and `mqttd` autostarting when a broker and
credentials are both stored.
**Exit:** the roundtrip test that `test_wlan_credential_roundtrip` already
models — provision on the host, boot, read it back over 9P — extended to the
MQTT field; a board with a stored broker publishes after a power cycle with
nothing typed.

### Q7 — The sensor persona, and hardware
`cmake/board-rp2350-sensor.cmake`: a Pico 2 W with a BME280 and no SD card,
`CONFIG_ENABLE_CC`/`ED`/`CHESS` off, `CONFIG_ENABLE_MQTT` and
`CONFIG_ENABLE_BME280` on, `mqttd` autostarted. Plus `tests/hw/test_sensor.py`
and the README sections.
**Exit:** a board on a USB charger, no console, publishing to `nalanda` for a
**24-hour soak** with no reconnect it did not recover from; `sizecheck`
passes on every persona with the growth named and re-baselined deliberately.

**Why the persona exists at all**, given §4.3 says the sensor needs no
board-file change: because **autostart is what a persona is for**. The
project's own rule — set during phase 11 and worth keeping — is that a
program starts by itself only on an appliance persona, never in the general
build. Everything else in Q0–Q6 works on `rp2350-wifi` by hand, and that is
how it gets tested; the persona adds unattendedness and subtracts a compiler.

### Q8 — QoS 1 publish (optional, see §3.3)
Only if an aperiodic publisher appears. One in-flight message, packet id,
PUBACK wait, `DUP` on retransmit, retry across a reconnect.

## 7. How it is tested, in three layers

**(a) A broker of our own, on the host, through slirp — the CI layer.**
`tests/mqttbroker.py`: a ~200-line single-connection MQTT 3.1.1 broker that
speaks the real protocol over a real TCP socket and, crucially, can also
*misbehave* on demand — split a fixed header across two segments, stall
mid-payload, send a 4 KB PUBLISH, close mid-packet, refuse a CONNECT with
each return code. QEMU's `-netdev user` reaches the host at **10.0.2.2**, so
the guest dials `10.0.2.2:<ephemeral>` and the broker asserts byte-for-byte
on what arrived — the same relationship `test_ntp_client`'s responder already
has with the NTP client. This is where every protocol assertion lives, and it
runs with no hardware and no external service.

> **First thing Q1 must confirm:** that a guest-originated connection to
> 10.0.2.2 reaches a listener on the host's loopback on the QEMU builds this
> project uses. It should — that is what slirp's gateway address is — but it
> is a one-line experiment, and if it does not hold, `guestfwd` maps a
> guest-visible address to a host port explicitly. Confirm it before writing
> the tests that depend on it, not after.

**(b) Packet-level adversarial tests — the `netpeer.py` layer.**
Reserved for what a well-behaved socket cannot do: a RST mid-session, a
zero-window stall while we have a publish queued, a segment lost on the path
that carries a PUBLISH. These test **Q0**, not MQTT — they are the stream's
tests, and `test_tcp_under_impairment` is their model.

**(c) The real broker and the real part — the hardware layer.**
`tests/hw/test_sensor.py`, run against a board already joined to WiFi and a
live `mosquitto` on `nalanda`: subscribe from the host, assert the board's
retained `online`, assert three measurements arrive within `period_s` and are
in plausible ranges, pull the board's power and assert the will arrives, then
restore and assert `online` returns. Skipped, not failed, when no board is
attached — the rule the whole `tests/hw` suite already follows.

**This layer is not optional and not a formality.** QEMU has hidden a
Hazard3/RP2350 divergence six times in this project's history, and every one
was found only on silicon. The specific things QEMU cannot tell us here: that
the CYW43's transmit path keeps up with a publish every `period_s` for a day,
that the I2C bus is stable with two devices on it and a radio running, that
forced-mode self-heating is not biasing the temperature, and that a router
rebooting at 04:00 is recovered from rather than merely handled in a test
where we chose the moment.

## 8. Explicitly not in this phase

* **TLS.** The user asked for none, and the honest reason to agree is not
  effort but trust: a TLS stack is a larger correctness surface than TCP was,
  and a bad one is worse than none because it *claims* confidentiality. §3.2
  states the exposure instead. If encryption is ever wanted, the smaller
  honest answer is a pre-shared-key payload encryption (the device key is
  already there) rather than a hand-written TLS 1.3.
* **MQTT 5.0.** Properties, reason codes, topic aliases, session expiry and
  shared subscriptions — a sensor node with four topics uses none of them, and
  every broker still speaks 3.1.1. Revisit only if a broker appears that
  refuses 3.1.1, which none in this house does.
* **QoS 2.** Two extra round trips and a four-packet handshake to solve
  duplicate delivery, for measurements that are idempotent by nature. No.
* **A DNS resolver.** The broker is configured by address. A single-question
  A-record client over UDP is perhaps 120 lines and R2 left room for it, but
  nothing here needs it: a broker's address is as stable as the board's own,
  and both are already pinned in the record. It gets built when something
  needs a *name*, which so far nothing does.
* **DHCP — still no, and the reason has changed.** Phase 19 §7 said DHCP
  "gets its own milestone when the first sensor persona wants one". This is
  that persona, and it does not want one: `netcfg` plus a router reservation
  keyed on the radio's MAC already brings a board up unattended, and the
  argument for DHCP was "hand-assigning addresses across several nodes is the
  actual cost" — which is true, and is a cost paid on the *router*, once per
  node, either way. The promise is discharged: revisit when there are enough
  nodes that the reservation list is the annoyance, not before.
* **Home Assistant MQTT discovery.** A retained JSON config message per
  entity on `homeassistant/sensor/<node>/<measure>/config` would make these
  nodes appear automatically in a home dashboard, and it is genuinely small
  once retained publishes exist (Q5). It is left out because it is a *policy*
  about someone else's software, and it belongs in `init.lisp` or a host-side
  script — which is exactly where Q2's `(mqtt-publish)` makes it a five-line
  addition anyone can write without changing the kernel.
* **A broker on the board.** Interesting, and not this. A node that publishes
  needs no broker; a segment that needs one has a Raspberry Pi.
* **Non-I2C sensors** (DS18B20 on 1-Wire, DHT22, analog). The bus is the
  work, not the part; `I2C_OP_XFER` makes the *next I2C* sensor free and does
  nothing for a 1-Wire one.
* **Sleep and battery operation.** A sensor node on a charger is this phase.
  Deep sleep, wake-on-timer and the power measurements to justify them are a
  phase of their own, and they interact with the radio's association state in
  ways that deserve their own plan.

## 9. Risks, and what each looks like

* **The stream and 9P share a connection table, and a bug crosses them.**
  Looks like: a 9P session that breaks only when the broker is also
  connected. Mitigated by §2's two rules (no `p9_link_t` face on a stream;
  the epoch check kept) and by running the existing TCP/9P suites unchanged as
  Q0's exit criterion — a regression there is the signal.
* **The two-connection limit bites on a persona that both serves and
  publishes.** Looks like: `mqtt connect` fails while two peers are mounted.
  Named in §2 so it is recognised rather than investigated; the valve is
  `TCP_MAX_CONNS` at 8 KB per slot.
* **The compensation arithmetic is subtly wrong.** Looks like: plausible
  temperature, pressure off by tens of hPa, or humidity that saturates. This
  is the highest-probability *silent* failure in the phase, which is why §4.2
  buys two independent implementations and a golden vector rather than
  eyeballing one reading.
* **Self-heating biases temperature.** Looks like: a reading consistently
  1–3 °C high, worse when the radio is busy. Forced mode is the structural
  mitigation; the hardware exit criterion compares against a reference
  thermometer, which is the only way to see it at all.
* **The radio and the I2C bus interfere in timing, not in wiring.** Looks
  like: occasional failed sensor reads only once WiFi is up — the same class
  as the clock-display flicker in `plan/open_issues.md`, and the same cause
  (a bit-banged transfer competing with something that must not be delayed).
  Now cheaper to fix than it was: phases 22/23 shipped, and pinning `mqttd`
  to the second core is available if it happens.
* **The 24-hour soak fails at hour 19.** Looks like: a reconnect loop, a
  wedged parser, or a heap that crept. This is the risk the soak exists to
  find, and the counters in `/proc/mqtt` are there so the answer is a number
  rather than a guess.

## 10. Budget

The numbers to hold this to, checked with `sizecheck` at Q7:

* **New static RAM:** the client's 512-byte reassembly buffer, its state
  struct, `mqttd`'s task stack, and the sensor's calibration block (~40
  bytes). Target: **under 2 KB** on top of today's baseline for a persona with
  both features on, and **zero** for one with them off.
* **New heap:** none. The stream reuses the TCP buffers `ensure_bufs()`
  already allocates.
* **Flash:** MQTT ≈ 4 KB, the sensor driver ≈ 3 KB including the 64-bit
  compensation. Both behind `CONFIG_ENABLE_*` flags (phase 8's mechanism), so
  the chess and clock personas pay nothing.
