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
 * name and a function that reads one number, so:
 *
 *   * drivers/bme280.c registers its three when a part is actually fitted,
 *     and a board with no sensor registers none rather than publishing zeroes;
 *   * a test can register a synthetic source and exercise the whole publish,
 *     reconnect and will machinery in QEMU, where no I2C controller exists.
 *
 * The second is not a convenience. Without it this task could only ever be
 * tested on hardware, and the thing most worth testing -- what it does when
 * the broker disappears mid-run -- is precisely what is hard to arrange there.
 *
 * **A source returns an integer, not a string.** It cannot be filtered or
 * compared against a threshold otherwise, and those are the two things this
 * file exists to do. The number is fixed-point: `decimals` says where the
 * point goes, so a BME280 reporting 26.11 C hands over 2611 with decimals=2,
 * and mqttd does the formatting -- once, in one place, correctly for negative
 * values below 1 (where -0.42 has a whole part of zero and the sign has to be
 * carried explicitly).
 *
 * ## Filtering, and the publish rule
 *
 * A sensor read twice in a row disagrees with itself in the last digit or
 * two. Publishing that raw does two bad things: it fills a broker and a
 * database with noise, and it makes any change-based rule fire on the noise
 * rather than on the weather.
 *
 * So each source carries an **exponential moving average** and a **rule**:
 *
 *     publish when   the filtered value has moved by at least `delta`
 *                    since the last published one
 *     but never      more often than `min_interval_s`
 *     and always     at least every `max_interval_s`
 *
 * That is three different jobs and each earns its place. The delta is what
 * makes a fast-changing measurement report quickly and a still one stay
 * quiet. The minimum is what stops a noisy or fast-moving signal from
 * flooding the broker. The maximum is a heartbeat: a value that has not
 * changed all afternoon still has to arrive, or a subscriber cannot tell it
 * from a node that died -- and the retained `status` topic answers "is it
 * alive", not "is this reading current".
 *
 * The comparison is against the last **published** value, not the last
 * sampled one, which is what gives it hysteresis: a value drifting slowly
 * across the threshold publishes once, not once per sample.
 */

#define MQTTD_MAX_SOURCES 4
#define MQTTD_VALUE_MAX   32

/* Reads one measurement as a fixed-point integer. False when the reading
 * failed, which publishes nothing at all rather than a stale or zero value --
 * a gap in a series is visible, a fabricated number is not. */
typedef bool (*mqttd_sample_fn)(int32_t *out, void *ctx);

typedef struct {
    /* Never publish more often than this, however fast the value moves.
     * 0 means "no limit", which is rarely what anyone wants. */
    uint16_t min_interval_s;
    /* Always publish at least this often, however still the value is. The
     * heartbeat: 0 disables it, and then a constant value is never reported
     * again after the first time. */
    uint16_t max_interval_s;
    /* Publish when the filtered value has moved this far from the last
     * published one, in the source's own fixed-point units (so 10 is 0.10 for
     * a source with decimals=2). 0 publishes on any change at all. */
    int32_t  delta;
    /* Exponential moving average: y += (x - y) / 2^alpha_shift, computed on a
     * scaled accumulator so it converges rather than stalling a step short.
     * 0 disables filtering, 3 (a quarter-ish) is a sensible start, and above
     * about 6 a real change takes minutes to show up. */
    uint8_t  alpha_shift;
} mqttd_rule_t;

/* Registers a measurement under `name` (the topic's last element), reported
 * with `decimals` places. `rule` may be NULL for the defaults below. Returns
 * 0, or -1 if the table is full. Safe to call before mqttd_start(). */
int  mqttd_add_source(const char *name, mqttd_sample_fn fn, void *ctx,
                      uint8_t decimals, const mqttd_rule_t *rule);

/* Changes a registered source's rule, by name. Returns 0, or -1 if there is
 * no such source. Takes effect on the next sample. */
int  mqttd_set_rule(const char *name, const mqttd_rule_t *rule);
const mqttd_rule_t *mqttd_get_rule(const char *name);

/* The defaults, and the reasoning: five seconds is faster than any physical
 * quantity a board like this measures actually changes, so it bounds the
 * damage a noisy source can do; five minutes is short enough that a stalled
 * reading is noticed within one dashboard refresh. */
#define MQTTD_DEFAULT_MIN_INTERVAL_S   5u
#define MQTTD_DEFAULT_MAX_INTERVAL_S 300u
#define MQTTD_DEFAULT_ALPHA_SHIFT      3u
void mqttd_clear_sources(void);
uint32_t mqttd_source_count(void);

typedef struct {
    uint8_t     broker[IPV4_LEN];
    uint16_t    port;          /* 0 = MQTT's default */
    /* How often each source is *sampled*. Publishing is governed by each
     * source's own rule, so this is the filter's input rate rather than the
     * output rate -- sampling faster makes the average smoother and a change
     * noticed sooner, and costs one forced-mode conversion. */
    uint16_t    sample_s;      /* 0 = MQTTD_DEFAULT_SAMPLE_S */
    uint16_t    keepalive_s;   /* 0 = MQTT's default */
    const char *username;      /* copied; the caller's storage need not last */
    const char *password;
    const char *prefix;        /* NULL/"" = "lugalos" */
} mqttd_config_t;

/* Five seconds: at the BME280's x1 oversampling a conversion is under 10 ms,
 * so this is a 0.2% duty cycle -- enough to keep self-heating out of the
 * temperature, which is the whole reason forced mode was chosen. */
#define MQTTD_DEFAULT_SAMPLE_S 5u

/* Starts the task. Returns its pid, or -1. Starting it twice stops the first.
 * Must follow sched_init(). */
int  mqttd_start(const mqttd_config_t *cfg);

/* Asks the task to disconnect cleanly and exit. A clean DISCONNECT means the
 * broker does *not* publish the will: the node meant to go. */
void mqttd_stop(void);

/* Q6: starts the task if -- and only if -- a broker is on record. Returns the
 * pid, or -1 when there is nothing stored, which is the normal state of every
 * board that is not a sensor node. Called once at boot. */
int  mqttd_autostart(void);

bool mqttd_running(void);
void mqttd_print_status(void);

#endif /* NET_MQTTD_H */
