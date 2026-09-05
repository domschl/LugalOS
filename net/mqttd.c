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

static struct {
    const char     *name;
    mqttd_sample_fn fn;
    void           *ctx;
} g_src[MQTTD_MAX_SOURCES];
static uint32_t g_src_count;

int mqttd_add_source(const char *name, mqttd_sample_fn fn, void *ctx) {
    if (!name || !*name || !fn) return -1;
    if (g_src_count >= MQTTD_MAX_SOURCES) return -1;
    g_src[g_src_count].name = name;
    g_src[g_src_count].fn = fn;
    g_src[g_src_count].ctx = ctx;
    g_src_count++;
    return 0;
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

static void publish_all(void) {
    for (uint32_t i = 0; i < g_src_count; i++) {
        char value[MQTTD_VALUE_MAX];
        value[0] = '\0';
        if (!g_src[i].fn(value, sizeof(value), g_src[i].ctx) || !value[0]) {
            /* A failed reading publishes nothing. A stale or zero value on a
             * measurement topic is worse than a gap: a gap is visible. */
            g.skipped++;
            continue;
        }
        char topic[160];
        topic_for(topic, sizeof(topic), g_src[i].name);
        if (mqtt_publish(topic, value, (uint32_t)strlen(value), false) == 0) {
            g.published++;
        } else {
            /* Left for the loop to notice: mqtt_publish() has already dropped
             * the connection if the transport failed. */
            return;
        }
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
                /* Publish immediately on connect rather than waiting a whole
                 * period: the first thing anyone does after a reconnect is
                 * look for a fresh value. */
                g.last_sample_ms = 0;
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
        uint32_t period_ms = (uint32_t)(g.cfg.period_s ? g.cfg.period_s
                                                       : MQTTD_DEFAULT_PERIOD) * 1000u;
        if (mqtt_state() == MQTT_CONNECTED &&
            (g.last_sample_ms == 0 || now - g.last_sample_ms >= period_ms)) {
            g.last_sample_ms = now;
            publish_all();
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
    g.cfg.period_s = cfg->period_s;
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
    cprintf("mqttd: %u.%u.%u.%u:%u, every %us, topics under %s/%s/\n",
            g.cfg.broker[0], g.cfg.broker[1], g.cfg.broker[2], g.cfg.broker[3],
            g.cfg.port ? g.cfg.port : (unsigned)MQTT_DEFAULT_PORT,
            g.cfg.period_s ? g.cfg.period_s : (unsigned)MQTTD_DEFAULT_PERIOD,
            prefix(), node_name());
    cprintf("       up %lum%lus, %s, published %lu, skipped %lu, connect failures %lu\n",
            (unsigned long)(up / 60u), (unsigned long)(up % 60u),
            mqtt_state_str(), (unsigned long)g.published,
            (unsigned long)g.skipped, (unsigned long)g.connect_fails);
    for (uint32_t i = 0; i < g_src_count; i++)
        cprintf("       source: %s/%s/%s\n", prefix(), node_name(), g_src[i].name);
}
