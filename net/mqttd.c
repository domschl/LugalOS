#include "net/mqttd.h"
#include "net/mqtt.h"
#include "net/ip.h"
#include "kernel/identity.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <string.h>

/* See net/include/net/mqttd.h for what this is for. This file is the loop.
 *
 * Three properties it has to hold, none of which are about MQTT:
 *
 *   * **It never gives up.** A board and its router come back from a power cut
 *     at the same moment and the router is slower, so the one attempt made at
 *     boot is exactly the attempt that fails. Backoff, capped, forever -- the
 *     same shape and the same reasoning as R5's `wifiup` retry.
 *   * **It yields.** Every wait is task_sleep_ms(), so the console stays
 *     usable and `netsrv` -- which is what actually moves the bytes this task
 *     hands to TCP -- gets to run.
 *   * **It says what it is doing once, not once per retry.** A network that is
 *     simply not there must not fill /proc/kmsg.
 */

#define BACKOFF_MIN_MS   1000u
#define BACKOFF_MAX_MS  30000u
/* How often the loop wakes to service the connection. Short enough that a
 * keepalive is never missed by much, long enough to be free. */
#define TICK_MS           200u

static struct {
    mqttd_config_t cfg;
    char     username[64], password[64], prefix[32];
    char     status_topic[160];

    int      pid;
    bool     stop_requested;
    bool     running;

    uint32_t published;      /* measurements that reached the broker */
    uint32_t skipped;        /* sources that declined to produce a reading */
    uint32_t connect_fails;
    uint64_t last_sample_ms;
    uint64_t started_ms;
} g;

typedef struct {
    const char     *name;
    mqttd_sample_fn fn;
    void           *ctx;
    uint8_t         decimals;
    mqttd_rule_t    rule;

    /* --- filter state ---
     * `acc` holds the average scaled by 2^alpha_shift. Keeping the extra bits
     * is what makes the filter converge: the naive form, y += (x - y) >> k,
     * loses every difference smaller than 2^k to truncation, so it creeps
     * towards the input and stops a step or two short -- a permanent offset
     * that looks exactly like a miscalibrated sensor. */
    int32_t  acc;
    bool     primed;

    /* --- rule state --- */
    int32_t  filtered;
    int32_t  last_sent;
    uint64_t last_sent_ms;
    bool     ever_sent;
    uint32_t publishes;
} mqttd_source_state_t;

static mqttd_source_state_t g_src[MQTTD_MAX_SOURCES];
static uint32_t g_src_count;

static const mqttd_rule_t g_default_rule = {
    .min_interval_s = MQTTD_DEFAULT_MIN_INTERVAL_S,
    .max_interval_s = MQTTD_DEFAULT_MAX_INTERVAL_S,
    .delta          = 0,
    .alpha_shift    = MQTTD_DEFAULT_ALPHA_SHIFT,
};

int mqttd_add_source(const char *name, mqttd_sample_fn fn, void *ctx,
                     uint8_t decimals, const mqttd_rule_t *rule) {
    if (!name || !*name || !fn) return -1;
    if (g_src_count >= MQTTD_MAX_SOURCES) return -1;
    mqttd_source_state_t *st = &g_src[g_src_count];
    memset(st, 0, sizeof(*st));
    st->name = name;
    st->fn = fn;
    st->ctx = ctx;
    st->decimals = decimals;
    st->rule = rule ? *rule : g_default_rule;
    /* An alpha shift wide enough to overflow the accumulator is a
     * configuration mistake that would present as wild readings. 12 leaves
     * plenty of headroom over any fixed-point value a sensor produces. */
    if (st->rule.alpha_shift > 12u) st->rule.alpha_shift = 12u;
    if (st->rule.delta < 0) st->rule.delta = -st->rule.delta;
    g_src_count++;
    return 0;
}

static mqttd_source_state_t *find_source(const char *name) {
    if (!name) return NULL;
    for (uint32_t i = 0; i < g_src_count; i++)
        if (strcmp(g_src[i].name, name) == 0) return &g_src[i];
    return NULL;
}

int mqttd_set_rule(const char *name, const mqttd_rule_t *rule) {
    mqttd_source_state_t *st = find_source(name);
    if (!st || !rule) return -1;
    st->rule = *rule;
    if (st->rule.alpha_shift > 12u) st->rule.alpha_shift = 12u;
    if (st->rule.delta < 0) st->rule.delta = -st->rule.delta;
    /* The filter is re-primed rather than rescaled: acc is held scaled by the
     * *old* shift, and reinterpreting it under a new one would produce one
     * spectacularly wrong reading. */
    st->primed = false;
    return 0;
}

const mqttd_rule_t *mqttd_get_rule(const char *name) {
    mqttd_source_state_t *st = find_source(name);
    return st ? &st->rule : NULL;
}

/* y += (x - y) / 2^k, carried on an accumulator scaled by 2^k. */
static int32_t filter_step(mqttd_source_state_t *st, int32_t x) {
    uint8_t k = st->rule.alpha_shift;
    if (k == 0) return x;
    if (!st->primed) {
        /* Start at the first real reading, not at zero: a filter that ramps
         * up from nothing publishes a minute of fiction after every boot. */
        st->acc = x << k;
        st->primed = true;
        return x;
    }
    st->acc += x - (st->acc >> k);
    return st->acc >> k;
}

/* Signed fixed-point, `dec` places. Done by hand because this kernel's
 * printk has no float formatting, and by hand *here* rather than in each
 * source because the sign of a value between -1 and 0 is the kind of detail
 * that is got wrong once per source otherwise. */
static void fmt_fixed(char *out, uint32_t max, int32_t v, uint8_t dec) {
    if (!out || max == 0) return;
    char digits[16];
    uint32_t n = 0;
    bool neg = v < 0;
    uint64_t a = neg ? (uint64_t)(-(int64_t)v) : (uint64_t)v;
    do {
        digits[n++] = (char)('0' + (a % 10u));
        a /= 10u;
    } while ((a || n <= dec) && n < sizeof(digits));

    uint32_t o = 0;
    if (neg && o + 1u < max) out[o++] = '-';
    while (n > 0 && o + 1u < max) {
        n--;
        out[o++] = digits[n];
        if (n == dec && dec && o + 1u < max) out[o++] = '.';
    }
    out[o] = '\0';
}

/* The rule, in one place: has this value earned a publish yet? */
static bool due(const mqttd_source_state_t *st, uint64_t now) {
    if (!st->ever_sent) return true;          /* the first reading always goes */

    uint64_t since = now - st->last_sent_ms;
    if (st->rule.min_interval_s &&
        since < (uint64_t)st->rule.min_interval_s * 1000u)
        return false;                          /* too soon, whatever it did */

    int32_t moved = st->filtered - st->last_sent;
    if (moved < 0) moved = -moved;
    if (moved >= st->rule.delta && (st->rule.delta > 0 || moved > 0))
        return true;                           /* it moved enough */

    return st->rule.max_interval_s &&
           since >= (uint64_t)st->rule.max_interval_s * 1000u;   /* heartbeat */
}

void mqttd_clear_sources(void) { g_src_count = 0; }
uint32_t mqttd_source_count(void) { return g_src_count; }
bool mqttd_running(void) { return g.running; }

static const char *prefix(void) { return g.prefix[0] ? g.prefix : "lugalos"; }

static void topic_for(char *out, uint32_t max, const char *leaf) {
    ksnprintf(out, max, "%s/%s/%s", prefix(), node_name(), leaf);
}

/* One connection attempt, including the announcement that follows it. */
static bool connect_once(void) {
    mqtt_config_t mc;
    memset(&mc, 0, sizeof(mc));
    memcpy(mc.broker, g.cfg.broker, IPV4_LEN);
    mc.port = g.cfg.port;
    mc.keepalive_s = g.cfg.keepalive_s;
    if (g.username[0]) mc.username = g.username;
    if (g.password[0]) mc.password = g.password;

    /* The will, registered as part of CONNECT so the broker holds it for us.
     * Retained, so a subscriber that arrives after the node died still learns
     * it is gone rather than seeing nothing at all. */
    mc.will_topic = g.status_topic;
    mc.will_payload = "offline";
    mc.will_retain = true;

    int rc = mqtt_connect(&mc, 8000u);
    if (rc != 0) {
        g.connect_fails++;
        return false;
    }

    /* The other half of the pair. Retained for the same reason. */
    if (mqtt_publish(g.status_topic, "online", 6u, true) != 0)
        printk("[mqttd] Connected, but the online announcement did not go out.\n");
    return true;
}

/* Sample every source, filter, and publish the ones whose rule says so. */
static void sample_and_publish(void) {
    uint64_t now = time_get_ms();

    for (uint32_t i = 0; i < g_src_count; i++) {
        mqttd_source_state_t *st = &g_src[i];

        int32_t raw = 0;
        if (!st->fn(&raw, st->ctx)) {
            /* A failed reading publishes nothing and does not enter the
             * filter either: feeding it a fabricated value would drag the
             * average, and a gap in a series is visible where a wrong number
             * is not. */
            g.skipped++;
            continue;
        }

        st->filtered = filter_step(st, raw);
        if (!due(st, now)) continue;

        char value[MQTTD_VALUE_MAX];
        fmt_fixed(value, sizeof(value), st->filtered, st->decimals);

        char topic[160];
        topic_for(topic, sizeof(topic), st->name);
        if (mqtt_publish(topic, value, (uint32_t)strlen(value), false) != 0) {
            /* Left for the loop to notice: mqtt_publish() has already dropped
             * the connection if the transport failed. Deliberately not marked
             * as sent, so the value goes out on the next pass. */
            return;
        }
        st->last_sent = st->filtered;
        st->last_sent_ms = now;
        st->ever_sent = true;
        st->publishes++;
        g.published++;
    }
}

static void mqttd_body(void *arg) {
    (void)arg;
    uint32_t backoff_ms = BACKOFF_MIN_MS;
    uint32_t attempts = 0;
    bool announced_down = false;

    g.running = true;
    g.started_ms = time_get_ms();
    topic_for(g.status_topic, sizeof(g.status_topic), "status");

    while (!g.stop_requested) {
        if (mqtt_state() != MQTT_CONNECTED) {
            if (connect_once()) {
                printk("[mqttd] Connected to %u.%u.%u.%u -- publishing under %s/%s/\n",
                       g.cfg.broker[0], g.cfg.broker[1], g.cfg.broker[2],
                       g.cfg.broker[3], prefix(), node_name());
                backoff_ms = BACKOFF_MIN_MS;
                attempts = 0;
                announced_down = false;
                /* Sample immediately on connect rather than waiting a whole
                 * period: the first thing anyone does after a reconnect is
                 * look for a fresh value. Every source is marked unsent too,
                 * so each publishes once on the new connection regardless of
                 * how still it has been -- a subscriber that joined during
                 * the outage would otherwise wait a full heartbeat to see
                 * anything. */
                g.last_sample_ms = 0;
                for (uint32_t i = 0; i < g_src_count; i++) g_src[i].ever_sent = false;
            } else {
                /* Said a few times, then silently. The retries never stop;
                 * the log lines do. */
                if (attempts < 3u) {
                    printk("[mqttd] Could not reach the broker; retrying in %u s.\n",
                           backoff_ms / 1000u);
                } else if (!announced_down) {
                    announced_down = true;
                    printk("[mqttd] Still cannot reach the broker -- retrying "
                           "every %u s, quietly from here.\n", BACKOFF_MAX_MS / 1000u);
                }
                attempts++;
                task_sleep_ms(backoff_ms);
                backoff_ms = (backoff_ms * 2u > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS
                                                                : backoff_ms * 2u;
                continue;
            }
        }

        /* The keepalive and the inbound path. This call is the whole reason
         * an idle session survives here and did not under the shell. */
        mqtt_service();

        uint64_t now = time_get_ms();
        uint32_t sample_ms = (uint32_t)(g.cfg.sample_s ? g.cfg.sample_s
                                                       : MQTTD_DEFAULT_SAMPLE_S) * 1000u;
        if (mqtt_state() == MQTT_CONNECTED &&
            (g.last_sample_ms == 0 || now - g.last_sample_ms >= sample_ms)) {
            g.last_sample_ms = now;
            sample_and_publish();
        }

        task_sleep_ms(TICK_MS);
    }

    /* A clean goodbye, which is what tells the broker not to publish the will.
     * The retained "online" is replaced first, so a subscriber that looks
     * later sees the truth rather than a stale announcement. */
    if (mqtt_state() == MQTT_CONNECTED) {
        mqtt_publish(g.status_topic, "offline", 7u, true);
        mqtt_disconnect();
    }
    printk("[mqttd] Stopped.\n");
    g.running = false;
    g.pid = -1;
}

static void copy_opt(char *dst, uint32_t cap, const char *src) {
    dst[0] = '\0';
    if (!src || !*src) return;
    strncpy(dst, src, cap - 1u);
    dst[cap - 1u] = '\0';
}

int mqttd_start(const mqttd_config_t *cfg) {
    if (!cfg) return -1;
    if (g.running) mqttd_stop();

    memset(&g.cfg, 0, sizeof(g.cfg));
    memcpy(g.cfg.broker, cfg->broker, IPV4_LEN);
    g.cfg.port = cfg->port;
    g.cfg.sample_s = cfg->sample_s;
    g.cfg.keepalive_s = cfg->keepalive_s;
    /* Copied, not pointed at: the caller's strings are usually a shell
     * command line that is gone by the first reconnect. */
    copy_opt(g.username, sizeof(g.username), cfg->username);
    copy_opt(g.password, sizeof(g.password), cfg->password);
    copy_opt(g.prefix, sizeof(g.prefix), cfg->prefix);

    g.stop_requested = false;
    g.published = g.skipped = g.connect_fails = 0;
    g.last_sample_ms = 0;

    g.pid = task_create("mqttd", mqttd_body, NULL);
    if (g.pid < 0) printk("[mqttd] Could not start the task.\n");
    return g.pid;
}

void mqttd_stop(void) {
    if (!g.running) return;
    g.stop_requested = true;
    /* The task wakes at most TICK_MS from now; give it a few of those rather
     * than killing it mid-publish. */
    for (uint32_t i = 0; i < 40u && g.running; i++) task_sleep_ms(50);
    if (g.running) printk("[mqttd] Did not stop within two seconds.\n");
}

void mqttd_print_status(void) {
    if (!g.running) {
        cprintf("mqttd: not running (%lu source%s registered)\n",
                (unsigned long)g_src_count, g_src_count == 1u ? "" : "s");
        return;
    }
    uint64_t up = (time_get_ms() - g.started_ms) / 1000u;
    cprintf("mqttd: %u.%u.%u.%u:%u, sampling every %us, topics under %s/%s/\n",
            g.cfg.broker[0], g.cfg.broker[1], g.cfg.broker[2], g.cfg.broker[3],
            g.cfg.port ? g.cfg.port : (unsigned)MQTT_DEFAULT_PORT,
            g.cfg.sample_s ? g.cfg.sample_s : (unsigned)MQTTD_DEFAULT_SAMPLE_S,
            prefix(), node_name());
    cprintf("       up %lum%lus, %s, published %lu, skipped %lu, connect failures %lu\n",
            (unsigned long)(up / 60u), (unsigned long)(up % 60u),
            mqtt_state_str(), (unsigned long)g.published,
            (unsigned long)g.skipped, (unsigned long)g.connect_fails);
    for (uint32_t i = 0; i < g_src_count; i++) {
        const mqttd_source_state_t *st = &g_src[i];
        char last[MQTTD_VALUE_MAX], d[MQTTD_VALUE_MAX];
        fmt_fixed(last, sizeof(last), st->filtered, st->decimals);
        fmt_fixed(d, sizeof(d), st->rule.delta, st->decimals);
        cprintf("       %-12s %s  (sent %lu; publish on %s, %us..%us, ema 1/%u)\n",
                st->name, st->ever_sent ? last : "--",
                (unsigned long)st->publishes, d,
                st->rule.min_interval_s, st->rule.max_interval_s,
                1u << st->rule.alpha_shift);
    }
}
