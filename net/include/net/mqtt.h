#ifndef NET_MQTT_H
#define NET_MQTT_H

#include <stdint.h>
#include <stdbool.h>
#include "net/ip.h"

/* MQTT 3.1.1 client (Q1, plan/phase26_mqtt_and_environment_sensors.md).
 *
 * The first protocol in this tree to run over a TCP *byte stream* rather than
 * over 9P framing, which is what Q0 built the stream for. It exists so a
 * sensor node can say what it measured to a broker the rest of the house
 * already talks to.
 *
 * **Version 3.1.1 (protocol level 4), deliberately.** It is what mosquitto
 * speaks by default and what every broker still speaks; 5.0's additions --
 * properties, reason strings, topic aliases, session expiry -- are all things
 * a node with four topics does not use, at the price of a larger CONNECT and
 * a second packet grammar. The phase 26 plan's §8 records that as a decision
 * rather than an omission.
 *
 * **What this deliberately is not.** No TLS (see below), no QoS 2 ever, no
 * QoS 1 yet (§3.3: TCP already retransmits, and what QoS 1 protects is the
 * reconnect -- for a periodic measurement that is superseded a minute later,
 * re-delivering the stale one is arguably worse than dropping it). No
 * persistent session, no broker-side queue, no topic aliasing, and one
 * connection to one broker at a time, because a node has one broker.
 *
 * ## The password crosses the LAN in the clear
 *
 * Stated here rather than in a footnote, because it is the one thing about
 * this client a user must know. MQTT's CONNECT packet carries the username
 * and password as plain length-prefixed UTF-8, and there is no TLS on this
 * board. Anyone with a packet capture on the segment -- or on the WiFi, if
 * the AP is open -- has the password and can replay it.
 *
 * That is phase 18 §1's threat model inherited without change: auth proves
 * who is talking and does not hide what they say. It was true of 9P over TCP
 * and it is true here. Give each node its own password, make it one that
 * means nothing anywhere else, and use the broker's ACLs to bound what that
 * account may publish.
 *
 * Two protocol details that cause support questions, recorded once:
 *
 *   * **A password requires a username** in 3.1.1 -- a password flag with no
 *     username flag is a protocol error, and a broker will reject the
 *     connection. (5.0 relaxed this; we do not implement 5.0.) A username
 *     with no password is legal.
 *   * **The client id must be unique on the broker.** Two nodes sharing one
 *     id disconnect each other in a permanent loop that looks exactly like a
 *     flaky network. The default is node_name(), which the identity record
 *     already makes per-board, so this is hard to cause by accident -- and
 *     when it happens, `identity name` is the fix.
 */

#define MQTT_DEFAULT_PORT      1883u
#define MQTT_DEFAULT_KEEPALIVE 60u

/* §3.5: the inbound reassembly bound. A packet larger than this is counted
 * and *discarded byte by byte* rather than dropped -- dropping without
 * consuming would leave the stream desynchronised at a packet boundary
 * forever, which is the classic way a small MQTT client wedges. */
#define MQTT_RX_MAX     512u
#define MQTT_TX_MAX     512u
#define MQTT_TOPIC_MAX  128u

typedef enum {
    MQTT_CLOSED = 0,
    MQTT_CONNECTING,
    MQTT_CONNECTED,
} mqtt_state_t;

typedef struct {
    uint8_t     broker[IPV4_LEN];
    uint16_t    port;             /* 0 = MQTT_DEFAULT_PORT */
    uint16_t    keepalive_s;      /* 0 = MQTT_DEFAULT_KEEPALIVE */
    const char *client_id;        /* NULL/"" = node_name() */
    const char *username;         /* NULL/"" = none */
    const char *password;         /* NULL/"" = none; needs a username (see above) */
    const char *will_topic;       /* NULL/"" = no will */
    const char *will_payload;
    bool        will_retain;
} mqtt_config_t;

/* --- The varint, which is the only arithmetic in the protocol ---
 *
 * Remaining Length: seven bits per byte, high bit as continuation, one to
 * four bytes, maximum 268435455. It is also the only place a wrong
 * implementation silently desynchronises a stream instead of failing, so it
 * is a pure function, built for every target, and exercised at every boundary
 * by `mqtt selftest` -- which needs no network and no broker. */

/* Writes 1-4 bytes into `out` (which must have room for 4). Returns the count,
 * or 0 if `value` exceeds the protocol's maximum. */
uint32_t mqtt_varint_encode(uint32_t value, uint8_t *out);

/* Returns the number of bytes consumed (1-4) and sets *value; 0 when `len`
 * holds only part of a varint (ask again with more), -1 when the encoding is
 * malformed -- a fifth continuation byte, which no valid packet contains. */
int mqtt_varint_decode(const uint8_t *buf, uint32_t len, uint32_t *value);

/* The boundary vector behind `mqtt selftest`. Returns the number of cases
 * that failed (0 is a pass) and, when `report` is true, prints each one. */
uint32_t mqtt_selftest(bool report);

/* --- The client --- */

/* Dials the broker, sends CONNECT, waits for CONNACK. Runs on the caller's
 * task and yields while it waits, because `netsrv` is what completes the
 * handshake and delivers the reply -- the same shape as ntp_query(). Returns
 * 0, or a negative MQTT_ERR_* below. */
int mqtt_connect(const mqtt_config_t *cfg, uint32_t timeout_ms);

/* --- Q3: the inbound half ---
 *
 * A sensor node that only publishes is a node nobody can talk to. Subscribing
 * is what lets one be asked a question -- and it is also the only place this
 * client parses bytes a broker chose, which is why §3.5's bounds are part of
 * the contract rather than an implementation detail:
 *
 *   * a packet larger than MQTT_RX_MAX is **counted and its bytes discarded**,
 *     not dropped. Dropping without consuming leaves the stream
 *     desynchronised at a packet boundary forever, which is the classic way a
 *     small MQTT client wedges;
 *   * a malformed Remaining Length closes the connection, because there is no
 *     resynchronising a stream whose framing is wrong and pretending
 *     otherwise is how a parser starts on whatever follows.
 *
 * QoS 0 only, in both directions. We never request more, so a broker has no
 * reason to send PUBREC/PUBREL/PUBCOMP, and receiving one is a protocol
 * error rather than something to implement. */

/* Called on netsrv's... no: on whichever task is pumping mqtt_service() when
 * a PUBLISH arrives. Keep it short and do not publish from inside it -- the
 * client is already inside its own lock, and the send buffer it would need is
 * the one being drained. Copy what you need and act later. */
typedef void (*mqtt_message_fn)(const char *topic, const uint8_t *payload,
                                uint32_t len, bool retained, void *ctx);
void mqtt_set_handler(mqtt_message_fn fn, void *ctx);

/* Subscribes at QoS 0 and waits for the SUBACK. Returns 0, or a negative
 * MQTT_ERR_*; MQTT_ERR_REFUSED when the broker granted 0x80, which means it
 * declined this filter (an ACL, usually) rather than that anything broke. */
int mqtt_subscribe(const char *filter, uint32_t timeout_ms);
int mqtt_unsubscribe(const char *filter, uint32_t timeout_ms);

/* Publishes at QoS 0. `len` may be 0 (an empty payload is legal and is how a
 * retained message is cleared). Returns 0 or a negative MQTT_ERR_*. */
int mqtt_publish(const char *topic, const void *payload, uint32_t len, bool retain);

/* Sends DISCONNECT and closes the connection. The broker will NOT publish the
 * will after this: a clean disconnect means the node meant to go. */
void mqtt_disconnect(void);

/* The pump. Reads whatever has arrived, answers the broker, and sends
 * PINGREQ when the keepalive interval is most of the way gone. Never blocks.
 * Called from mqtt_connect()'s own wait, from the shell while it waits, and
 * once per loop by `mqttd` (Q5). */
void mqtt_service(void);

mqtt_state_t mqtt_state(void);
const char  *mqtt_state_str(void);

/* The report behind `mqtt` and /proc/mqtt: broker, client id, state, uptime,
 * keepalive and the counters. */
void mqtt_print_status(void);

/* Counters, for /proc/mqtt and for a soak that needs a number rather than an
 * impression. */
typedef struct {
    uint32_t published;      /* PUBLISH packets we sent */
    uint32_t received;       /* PUBLISH packets delivered to us */
    uint32_t pings;          /* PINGREQ sent */
    uint32_t reconnects;     /* connections established after the first */
    uint32_t oversize;       /* inbound packets discarded for exceeding MQTT_RX_MAX */
    uint32_t proto_errors;   /* malformed framing; each one closed the connection */
    uint64_t connected_at_ms;
} mqtt_counters_t;
const mqtt_counters_t *mqtt_counters(void);

/* The CONNACK return code from the last connection attempt (0 = accepted).
 * Only meaningful after MQTT_ERR_REFUSED. */
uint8_t mqtt_last_connack(void);

#define MQTT_ERR_NO_NET      -1  /* no interface, or no address configured */
#define MQTT_ERR_TCP         -2  /* the connection was refused or never opened */
#define MQTT_ERR_TIMEOUT     -3  /* no CONNACK, or the write never drained */
#define MQTT_ERR_REFUSED     -4  /* the broker said no -- see mqtt_last_connack() */
#define MQTT_ERR_PROTO       -5  /* the broker sent something that is not MQTT */
#define MQTT_ERR_INTERRUPTED -6  /* Ctrl-C */
#define MQTT_ERR_TOOBIG      -7  /* topic + payload exceed MQTT_TX_MAX */
#define MQTT_ERR_STATE       -8  /* not connected */
#define MQTT_ERR_BADARG      -9

const char *mqtt_err_str(int rc);

#endif /* NET_MQTT_H */
