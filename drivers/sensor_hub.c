#include "drivers/sensor_hub.h"
#include "drivers/bme280.h"
#include "drivers/bme680.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <string.h>

/* Centralized Sensor Hub (Category D, phase 46 §5).
 *
 * Keeps cached readings and staleness timers for all probed environmental
 * sensors. Bus-free readers (such as /proc/sensors and 9P) read from this cache.
 */

typedef struct {
    sensor_dev_t *dev;
    int32_t       raw_val[SENSOR_CHAN_MAX];
    int32_t       filtered_val[SENSOR_CHAN_MAX];
    bool          valid[SENSOR_CHAN_MAX];
    uint64_t      last_sample_ms[SENSOR_CHAN_MAX];
    uint32_t      sample_count;
    uint32_t      fail_count;
} dev_cache_t;

static sensor_dev_t *s_drivers[SENSOR_HUB_MAX_DEVS];
static uint32_t      s_driver_count = 0;

static dev_cache_t   s_caches[SENSOR_HUB_MAX_DEVS];
static uint32_t      s_active_count = 0;

static uint32_t      s_sample_period_s = SENSOR_HUB_DEFAULT_PERIOD_S;
static int           s_sampler_pid = -1;

bool sensor_hub_register(sensor_dev_t *dev) {
    if (!dev || s_driver_count >= SENSOR_HUB_MAX_DEVS) return false;
    for (uint32_t i = 0; i < s_driver_count; i++) {
        if (s_drivers[i] == dev) return true;
    }
    s_drivers[s_driver_count++] = dev;
    return true;
}

void sensor_hub_init(void) {
    /* Register built-in drivers */
    sensor_hub_register(&bme680_sensor_dev);
    sensor_hub_register(&bme280_sensor_dev);

    s_active_count = 0;
    memset(s_caches, 0, sizeof(s_caches));

    for (uint32_t i = 0; i < s_driver_count; i++) {
        sensor_dev_t *dev = s_drivers[i];
        if (!dev || !dev->ops || !dev->ops->init) continue;
        if (dev->ops->init(dev)) {
            s_caches[s_active_count].dev = dev;
            s_active_count++;
        }
    }
}

uint32_t sensor_hub_device_count(void) {
    return s_active_count;
}

sensor_dev_t *sensor_hub_device_get(uint32_t idx) {
    if (idx >= s_active_count) return NULL;
    return s_caches[idx].dev;
}

void sensor_hub_sample_all(void) {
    for (uint32_t i = 0; i < s_active_count; i++) {
        dev_cache_t *c = &s_caches[i];
        sensor_dev_t *dev = c->dev;
        if (!dev || !dev->ops || !dev->ops->sample) continue;

        if (dev->ops->sample(dev)) {
            c->sample_count++;
            uint64_t now = time_get_ms();
            for (uint32_t ch = 0; ch < SENSOR_CHAN_MAX; ch++) {
                if (!(dev->chan_mask & (1u << ch))) continue;
                int32_t val = 0;
                if (dev->ops->get_value(dev, (sensor_chan_t)ch, &val)) {
                    c->raw_val[ch] = val;
                    if (!c->valid[ch]) {
                        c->filtered_val[ch] = val;
                    } else {
                        /* Exponential moving average with alpha_shift = 2 (weight 1/4 new, 3/4 old) */
                        c->filtered_val[ch] += (val - c->filtered_val[ch]) >> 2;
                    }
                    c->valid[ch] = true;
                    c->last_sample_ms[ch] = now;
                }
            }
        } else {
            c->fail_count++;
        }
    }
}

static void sensor_hub_sampler_body(void *arg) {
    (void)arg;
    for (;;) {
        sensor_hub_sample_all();
        uint32_t period = s_sample_period_s ? s_sample_period_s : SENSOR_HUB_DEFAULT_PERIOD_S;
        task_sleep_ms((uint64_t)period * 1000u);
    }
}

int sensor_hub_sampler_start(uint32_t period_s) {
    if (s_active_count == 0) return -1;
    if (period_s != 0) s_sample_period_s = period_s;
    if (s_sampler_pid >= 0) return s_sampler_pid;

    s_sampler_pid = task_create_sized("sensor_hub", sensor_hub_sampler_body, NULL, 1);
    return s_sampler_pid;
}

uint32_t sensor_hub_sample_period_s(void) {
    return s_sample_period_s;
}

bool sensor_hub_get_dev(const sensor_dev_t *dev, sensor_chan_t chan, int32_t *out_val, uint32_t *age_s) {
    if (!dev || chan >= SENSOR_CHAN_MAX || !out_val) return false;
    for (uint32_t i = 0; i < s_active_count; i++) {
        dev_cache_t *c = &s_caches[i];
        if (c->dev == dev && c->valid[chan]) {
            *out_val = c->raw_val[chan];
            if (age_s) {
                uint64_t now = time_get_ms();
                *age_s = (now >= c->last_sample_ms[chan])
                             ? (uint32_t)((now - c->last_sample_ms[chan]) / 1000u)
                             : 0u;
            }
            return true;
        }
    }
    return false;
}

bool sensor_hub_get(sensor_chan_t chan, int32_t *out_val, uint32_t *age_s) {
    if (chan >= SENSOR_CHAN_MAX || !out_val) return false;
    for (uint32_t i = 0; i < s_active_count; i++) {
        dev_cache_t *c = &s_caches[i];
        if (c->valid[chan]) {
            *out_val = c->raw_val[chan];
            if (age_s) {
                uint64_t now = time_get_ms();
                *age_s = (now >= c->last_sample_ms[chan])
                             ? (uint32_t)((now - c->last_sample_ms[chan]) / 1000u)
                             : 0u;
            }
            return true;
        }
    }
    return false;
}

uint32_t sensor_hub_selftest(bool report) {
    uint32_t failed = 0;
    /* Ensure default drivers are registered */
    sensor_hub_register(&bme280_sensor_dev);
    sensor_hub_register(&bme680_sensor_dev);

    for (uint32_t i = 0; i < s_driver_count; i++) {
        sensor_dev_t *dev = s_drivers[i];
        if (dev && dev->ops && dev->ops->selftest) {
            failed += dev->ops->selftest(report);
        }
    }
    return failed;
}

void sensor_hub_print_status(void) {
    if (s_active_count == 0) {
        cprintf("sensor: none found at 0x76 or 0x77 on the shared I2C bus\n"
                "        `i2c scan` lists what is actually answering there\n");
        return;
    }

    for (uint32_t i = 0; i < s_active_count; i++) {
        dev_cache_t *c = &s_caches[i];
        sensor_dev_t *dev = c->dev;
        if (!dev) continue;

        /* If no sample has been taken yet, take one synchronously */
        bool has_sample = false;
        for (uint32_t ch = 0; ch < SENSOR_CHAN_MAX; ch++) {
            if (c->valid[ch]) { has_sample = true; break; }
        }
        if (!has_sample && dev->ops && dev->ops->sample) {
            if (dev->ops->sample(dev)) {
                c->sample_count++;
                uint64_t now = time_get_ms();
                for (uint32_t ch = 0; ch < SENSOR_CHAN_MAX; ch++) {
                    if (!(dev->chan_mask & (1u << ch))) continue;
                    int32_t val = 0;
                    if (dev->ops->get_value(dev, (sensor_chan_t)ch, &val)) {
                        c->raw_val[ch] = val;
                        c->filtered_val[ch] = val;
                        c->valid[ch] = true;
                        c->last_sample_ms[ch] = now;
                    }
                }
            } else {
                c->fail_count++;
            }
        }

        /* Print sensor identity and available channels */
        cprintf("%s at 0x%02x:", dev->name, dev->addr);
        if (c->valid[SENSOR_CHAN_TEMP]) {
            int32_t t = c->raw_val[SENSOR_CHAN_TEMP];
            int32_t w = t / 100;
            int32_t f = t < 0 ? -(t % 100) : (t % 100);
            cprintf(" %ld.%02ld C,", (long)w, (long)f);
        }
        if (c->valid[SENSOR_CHAN_PRESSURE]) {
            int32_t p = c->raw_val[SENSOR_CHAN_PRESSURE];
            cprintf(" %ld.%02ld hPa,", (long)(p / 100), (long)(p % 100));
        }
        if (c->valid[SENSOR_CHAN_HUMIDITY]) {
            int32_t h = c->raw_val[SENSOR_CHAN_HUMIDITY];
            cprintf(" %ld.%02ld %%RH,", (long)(h / 100), (long)(h % 100));
        }
        if (c->valid[SENSOR_CHAN_GAS_RES]) {
            int32_t g = c->raw_val[SENSOR_CHAN_GAS_RES];
            cprintf(" %ld Ohm (gas),", (long)g);
        }
        if (c->valid[SENSOR_CHAN_LUX]) {
            int32_t l = c->raw_val[SENSOR_CHAN_LUX];
            cprintf(" %ld.%02ld Lux,", (long)(l / 100), (long)(l % 100));
        }
        if (c->valid[SENSOR_CHAN_ECO2]) {
            cprintf(" %ld ppm eCO2,", (long)c->raw_val[SENSOR_CHAN_ECO2]);
        }
        if (c->valid[SENSOR_CHAN_TVOC]) {
            cprintf(" %ld ppb TVOC,", (long)c->raw_val[SENSOR_CHAN_TVOC]);
        }
        cprintf("\n");

        cprintf("  /proc/sensors: sampler every %lu s, %lu read%s, %lu failure%s\n",
                (unsigned long)s_sample_period_s,
                (unsigned long)c->sample_count, c->sample_count == 1u ? "" : "s",
                (unsigned long)c->fail_count, c->fail_count == 1u ? "" : "s");
    }
}
