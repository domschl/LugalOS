#ifndef NET_MQTTD_H
#define NET_MQTTD_H

#include <stdint.h>
#include <stdbool.h>
#include "net/ip.h"

/* `mqttd` -- the task that makes a sensor node an appliance (Q5,
 * plan/phase26_mqtt_and_environment_sensors.md).
 *
 * Everything Q1-Q4 built is driven by hand: someone types `mqtt connect`, then
 * `sensor`, then `mqtt pub`. This is the part that does it by itself, forever,
 * from a USB charger with no console attached -- connect, announce, sample on
 * a period, keep the session alive, and get back on after the network goes
 * away.
 *
 * **It also closes a hole Q1 left open.** Nothing serviced an MQTT connection
 * between shell commands, so an idle session was dropped at the keepalive
 * while the client still believed it was connected -- observed on hardware as
 * `state CONNECTED, pings 0` over a TCP connection that had gone to
 * CLOSE_WAIT. A task whose loop calls mqtt_service() is the answer, and it is
 * the reason this is a milestone rather than a script.
 *
 * ## What it publishes
 *
 *     <prefix>/<node>/status         "online" / "offline", retained
 *     <prefix>/<node>/<measurement>  one per registered source
 *
 * `<prefix>` defaults to "lugalos" and `<node>` is node_name(), which the
 * identity record already makes per-board. The status pair is what makes a
 * subscriber able to tell "quiet because nothing changed" from "gone": the
 * "offline" half is registered as the connection's *will*, so the broker
 * publishes it when the node stops without saying goodbye. A sensor node's
 * most important message is the one it cannot send.
 *
 * ## Sources, and why they are registered rather than called
 *
 * mqttd knows nothing about I2C or about any particular sensor. A source is a
 * name and a function that formats one value, so:
 *
 *   * drivers/bme280.c registers its three when a part is actually fitted,
 *     and a board with no sensor registers none rather than publishing zeroes;
 *   * a test can register a synthetic source and exercise the whole publish,
 *     reconnect and will machinery in QEMU, where no I2C controller exists.
 *
 * The second is not a convenience. Without it this task could only ever be
 * tested on hardware, and the thing most worth testing -- what it does when
 * the broker disappears mid-run -- is precisely what is hard to arrange there.
 */

#define MQTTD_MAX_SOURCES 4
#define MQTTD_VALUE_MAX   32

/* Formats one measurement into `out`. False when the reading failed, which is
 * published as nothing at all rather than as a stale or zero value. */
typedef bool (*mqttd_sample_fn)(char *out, uint32_t max, void *ctx);

/* Registers a measurement under `name` (the topic's last element). Returns 0,
 * or -1 if the table is full. Safe to call before mqttd_start(). */
int  mqttd_add_source(const char *name, mqttd_sample_fn fn, void *ctx);
void mqttd_clear_sources(void);
uint32_t mqttd_source_count(void);

typedef struct {
    uint8_t     broker[IPV4_LEN];
    uint16_t    port;          /* 0 = MQTT's default */
    uint16_t    period_s;      /* 0 = MQTTD_DEFAULT_PERIOD */
    uint16_t    keepalive_s;   /* 0 = MQTT's default */
    const char *username;      /* copied; the caller's storage need not last */
    const char *password;
    const char *prefix;        /* NULL/"" = "lugalos" */
} mqttd_config_t;

#define MQTTD_DEFAULT_PERIOD 60u

/* Starts the task. Returns its pid, or -1. Starting it twice stops the first.
 * Must follow sched_init(). */
int  mqttd_start(const mqttd_config_t *cfg);

/* Asks the task to disconnect cleanly and exit. A clean DISCONNECT means the
 * broker does *not* publish the will: the node meant to go. */
void mqttd_stop(void);

bool mqttd_running(void);
void mqttd_print_status(void);

#endif /* NET_MQTTD_H */
