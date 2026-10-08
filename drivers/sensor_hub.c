#include "drivers/sensor_hub.h"
#include "drivers/bme280.h"
#include "drivers/bme680.h"
#include "drivers/ccs811.h"
#include "drivers/sgp30.h"
#include "drivers/tsl2561.h"
#include "drivers/tsl2591.h"
#include "drivers/mics6814.h"
#include "drivers/mhz19b.h"
#include "drivers/at24c32.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/identity.h"
#include <string.h>

/* Centralized Sensor Hub (Category D, phase 46 §5 & §7).
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

typedef struct {
    int32_t  raw_val;
    int32_t  filtered_val;
    bool     valid;
    uint64_t last_sample_ms;
} derived_chan_t;

static sensor_dev_t  *s_drivers[SENSOR_HUB_MAX_DEVS];
static uint32_t       s_driver_count = 0;

static dev_cache_t    s_caches[SENSOR_HUB_MAX_DEVS];
static uint32_t       s_active_count = 0;

static derived_chan_t s_derived[SENSOR_CHAN_MAX];
static int32_t        s_bme680_r_base = 0;
static bool           s_mox_contaminated = false;
static bool           s_fresh_air_verified = false;
static int32_t        s_co2_ratio_pct = 100;

static uint32_t       s_sample_period_s = SENSOR_HUB_DEFAULT_PERIOD_S;
static int            s_sampler_pid = -1;

static int32_t        s_altitude_m = 0;
static bool           s_have_altitude = false;

void sensor_hub_altitude_reload(void) {
    int32_t alt = 0;
    s_have_altitude = node_altitude(&alt);
    s_altitude_m = s_have_altitude ? alt : 0;
}

bool    sensor_hub_is_mox_contaminated(void)   { return s_mox_contaminated; }
bool    sensor_hub_is_fresh_air_verified(void) { return s_fresh_air_verified; }
int32_t sensor_hub_co2_ratio_pct(void)         { return s_co2_ratio_pct; }

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
    sensor_hub_register(&mhz19b_sensor_dev);
    sensor_hub_register(&mics6814_sensor_dev);
    sensor_hub_register(&tsl2591_sensor_dev);
    sensor_hub_register(&tsl2561_sensor_dev);
    sensor_hub_register(&ccs811_sensor_dev);
    sensor_hub_register(&sgp30_sensor_dev);
    sensor_hub_register(&bme680_sensor_dev);
    sensor_hub_register(&bme280_sensor_dev);

    s_active_count = 0;
    memset(s_caches, 0, sizeof(s_caches));
    memset(s_derived, 0, sizeof(s_derived));

    for (uint32_t i = 0; i < s_driver_count; i++) {
        sensor_dev_t *dev = s_drivers[i];
        if (!dev || !dev->ops || !dev->ops->init) continue;
        if (dev->ops->init(dev)) {
            s_caches[s_active_count].dev = dev;
            s_active_count++;
        }
    }

    sensor_hub_altitude_reload();

    sensor_cal_blob_t saved_cal;
    if (sensor_hub_cal_eeprom_has_saved(&saved_cal)) {
        sensor_hub_cal_apply(&saved_cal);
        printk("[Sensor Hub] Restored fast calibration from EEPROM (0x57).\n");
    } else if (sensor_hub_cal_has_saved(&saved_cal)) {
        sensor_hub_cal_apply(&saved_cal);
        printk("[Sensor Hub] Restored persistent calibration from identity store.\n");
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

    /* Cross-Sensor Compensation Pipeline (Phase 46 §5.2 / §4.9):
     * If ambient temperature and humidity are available from BME280/BME680,
     * feed them into gas sensors (such as CCS811) for real-time baseline tuning. */
    int32_t t = 0, h = 0;
    if (sensor_hub_get(SENSOR_CHAN_TEMP, &t, NULL) &&
        sensor_hub_get(SENSOR_CHAN_HUMIDITY, &h, NULL)) {
        if (ccs811_is_detected()) {
            ccs811_set_env_data(t, h);
        }
        if (sgp30_is_detected()) {
            sgp30_set_absolute_humidity(t, h);
        }
    }

    /* --- Derived Metrics Pipeline (Phase 46 §7.3 & §7.4) --- */
    uint64_t now = time_get_ms();

    /* 1. Mean Sea Level Pressure (P_msl / QNH) */
    int32_t p = 0;
    if (sensor_hub_get(SENSOR_CHAN_PRESSURE, &p, NULL) &&
        sensor_hub_get(SENSOR_CHAN_TEMP, &t, NULL) &&
        s_have_altitude) {
        int32_t p_msl = sensor_calc_sea_level_pa(p, t, s_altitude_m);
        s_derived[SENSOR_CHAN_PRESSURE_MSL].raw_val = p_msl;
        s_derived[SENSOR_CHAN_PRESSURE_MSL].filtered_val = p_msl;
        s_derived[SENSOR_CHAN_PRESSURE_MSL].valid = true;
        s_derived[SENSOR_CHAN_PRESSURE_MSL].last_sample_ms = now;
    } else {
        s_derived[SENSOR_CHAN_PRESSURE_MSL].valid = false;
    }

    /* 2. Absolute Humidity and Dew Point */
    if (sensor_hub_get(SENSOR_CHAN_TEMP, &t, NULL) &&
        sensor_hub_get(SENSOR_CHAN_HUMIDITY, &h, NULL)) {
        int32_t ah = sensor_calc_abs_humidity_c100(t, h);
        int32_t dp = sensor_calc_dew_point_c100(t, h);

        s_derived[SENSOR_CHAN_AH].raw_val = ah;
        s_derived[SENSOR_CHAN_AH].filtered_val = ah;
        s_derived[SENSOR_CHAN_AH].valid = true;
        s_derived[SENSOR_CHAN_AH].last_sample_ms = now;

        s_derived[SENSOR_CHAN_DEW_POINT].raw_val = dp;
        s_derived[SENSOR_CHAN_DEW_POINT].filtered_val = dp;
        s_derived[SENSOR_CHAN_DEW_POINT].valid = true;
        s_derived[SENSOR_CHAN_DEW_POINT].last_sample_ms = now;
    }

    /* 3. Open-Source BME680 IAQ Index */
    int32_t gas_r = 0;
    if (sensor_hub_get_dev(&bme680_sensor_dev, SENSOR_CHAN_GAS_RES, &gas_r, NULL) &&
        sensor_hub_get(SENSOR_CHAN_TEMP, &t, NULL) &&
        sensor_hub_get(SENSOR_CHAN_HUMIDITY, &h, NULL)) {
        int32_t iaq = sensor_calc_bme680_iaq(gas_r, t, h, &s_bme680_r_base);
        s_derived[SENSOR_CHAN_IAQ].raw_val = iaq;
        s_derived[SENSOR_CHAN_IAQ].filtered_val = iaq;
        s_derived[SENSOR_CHAN_IAQ].valid = true;
        s_derived[SENSOR_CHAN_IAQ].last_sample_ms = now;
    }

    /* 4. Ground-Truth Calibration & Contamination Disambiguation (Phase 46 §7.5) */
    int32_t co2_ndir = 0;
    int32_t eco2 = 0;
    int32_t tvoc = 0;
    bool have_ndir = sensor_hub_get_dev(&mhz19b_sensor_dev, SENSOR_CHAN_CO2, &co2_ndir, NULL);
    bool have_eco2 = sensor_hub_get(SENSOR_CHAN_ECO2, &eco2, NULL);
    bool have_tvoc = sensor_hub_get(SENSOR_CHAN_TVOC, &tvoc, NULL);

    if (have_ndir && have_eco2 && co2_ndir > 0) {
        s_co2_ratio_pct = (int32_t)(((int64_t)eco2 * 100LL) / co2_ndir);
        /* If eCO2 > 1.8x true CO2, flag MOX sensor as chemically contaminated by VOC/solvents */
        s_mox_contaminated = (s_co2_ratio_pct > 180);
        /* If true CO2 is low outdoor baseline and TVOC is low, fresh outdoor air confirmed */
        s_fresh_air_verified = (co2_ndir <= 430 && (!have_tvoc || tvoc <= 25));
    } else {
        s_mox_contaminated = false;
        s_fresh_air_verified = false;
        s_co2_ratio_pct = 100;
    }

    if (at24c32_is_detected()) {
        static uint64_t s_last_eeprom_ckpt_ms = 0;
        static bool s_was_fresh_air = false;
        bool fresh_transition = s_fresh_air_verified && !s_was_fresh_air;
        s_was_fresh_air = s_fresh_air_verified;

        /* Checkpoint baseline to EEPROM on fresh air confirmation or once per hour (3600s) */
        if (fresh_transition || (s_last_eeprom_ckpt_ms != 0 && (now - s_last_eeprom_ckpt_ms >= 3600000ULL)) ||
            (s_last_eeprom_ckpt_ms == 0 && now >= 300000ULL)) {
            sensor_hub_cal_eeprom_save();
            s_last_eeprom_ckpt_ms = now;
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

    s_sampler_pid = task_create_sized("sensor_hub", sensor_hub_sampler_body, NULL, 3);
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

    /* 1. Handle derived channels */
    if (chan == SENSOR_CHAN_PRESSURE_MSL || chan == SENSOR_CHAN_AH ||
        chan == SENSOR_CHAN_DEW_POINT || chan == SENSOR_CHAN_IAQ) {
        if (s_derived[chan].valid) {
            *out_val = s_derived[chan].raw_val;
            if (age_s) {
                uint64_t now = time_get_ms();
                *age_s = (now >= s_derived[chan].last_sample_ms)
                             ? (uint32_t)((now - s_derived[chan].last_sample_ms) / 1000u)
                             : 0u;
            }
            return true;
        }
        return false;
    }

    /* 2. For CO2: Prioritize true physical NDIR optical absorption (mhz19b) */
    if (chan == SENSOR_CHAN_CO2) {
        if (sensor_hub_get_dev(&mhz19b_sensor_dev, SENSOR_CHAN_CO2, out_val, age_s)) {
            return true;
        }
        /* Fallback to eCO2 if NDIR is absent */
        return sensor_hub_get(SENSOR_CHAN_ECO2, out_val, age_s);
    }

    /* 3. Physical sensor cache search */
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

bool sensor_hub_get_filtered(sensor_chan_t chan, int32_t *out_val) {
    if (chan >= SENSOR_CHAN_MAX || !out_val) return false;

    /* 1. Derived channels */
    if (chan == SENSOR_CHAN_PRESSURE_MSL || chan == SENSOR_CHAN_AH ||
        chan == SENSOR_CHAN_DEW_POINT || chan == SENSOR_CHAN_IAQ) {
        if (s_derived[chan].valid) {
            *out_val = s_derived[chan].filtered_val;
            return true;
        }
        return false;
    }

    /* 2. Physical sensor cache search */
    for (uint32_t i = 0; i < s_active_count; i++) {
        dev_cache_t *c = &s_caches[i];
        if (c->valid[chan]) {
            *out_val = c->filtered_val[chan];
            return true;
        }
    }
    return false;
}

uint32_t sensor_hub_selftest(bool report) {
    uint32_t failed = 0;
    /* Ensure default drivers are registered */
    sensor_hub_register(&mhz19b_sensor_dev);
    sensor_hub_register(&mics6814_sensor_dev);
    sensor_hub_register(&bme280_sensor_dev);
    sensor_hub_register(&bme680_sensor_dev);
    sensor_hub_register(&ccs811_sensor_dev);
    sensor_hub_register(&sgp30_sensor_dev);
    sensor_hub_register(&tsl2561_sensor_dev);
    sensor_hub_register(&tsl2591_sensor_dev);

    for (uint32_t i = 0; i < s_driver_count; i++) {
        sensor_dev_t *dev = s_drivers[i];
        if (dev && dev->ops && dev->ops->selftest) {
            failed += dev->ops->selftest(report);
        }
    }

    /* Test derived math models */
    failed += sensor_derived_selftest(report);
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
        if (c->valid[SENSOR_CHAN_CO]) {
            int32_t val = c->raw_val[SENSOR_CHAN_CO];
            cprintf(" %ld.%02ld ppm CO,", (long)(val / 100), (long)(val % 100));
        }
        if (c->valid[SENSOR_CHAN_NO2]) {
            int32_t val = c->raw_val[SENSOR_CHAN_NO2];
            cprintf(" %ld.%02ld ppm NO2,", (long)(val / 100), (long)(val % 100));
        }
        if (c->valid[SENSOR_CHAN_NH3]) {
            int32_t val = c->raw_val[SENSOR_CHAN_NH3];
            cprintf(" %ld.%02ld ppm NH3,", (long)(val / 100), (long)(val % 100));
        }
        if (c->valid[SENSOR_CHAN_CO2]) {
            cprintf(" %ld ppm CO2 (NDIR),", (long)c->raw_val[SENSOR_CHAN_CO2]);
        }
        if (strcmp(dev->name, "mics6814") == 0) {
            uint32_t rem = 0;
            if (mics6814_is_warming_up(&rem)) {
                cprintf(" (warming up: %lum %02lus left),", (unsigned long)(rem / 60), (unsigned long)(rem % 60));
            }
        }
        if (strcmp(dev->name, "mhz19b") == 0) {
            uint32_t rem = 0;
            if (mhz19b_is_warming_up(&rem)) {
                cprintf(" (warming up: %lum %02lus left),", (unsigned long)(rem / 60), (unsigned long)(rem % 60));
            }
        }
        cprintf("\n");

        cprintf("  /proc/sensors: sampler every %lu s, %lu read%s, %lu failure%s\n",
                (unsigned long)s_sample_period_s,
                (unsigned long)c->sample_count, c->sample_count == 1u ? "" : "s",
                (unsigned long)c->fail_count, c->fail_count == 1u ? "" : "s");
    }

    /* Print derived metrics report */
    int32_t p_msl = 0, ah = 0, dp = 0, iaq = 0;
    bool has_p_msl = sensor_hub_get(SENSOR_CHAN_PRESSURE_MSL, &p_msl, NULL);
    bool has_ah    = sensor_hub_get(SENSOR_CHAN_AH, &ah, NULL);
    bool has_dp    = sensor_hub_get(SENSOR_CHAN_DEW_POINT, &dp, NULL);
    bool has_iaq   = sensor_hub_get(SENSOR_CHAN_IAQ, &iaq, NULL);

    if (has_p_msl || has_ah || has_dp || has_iaq) {
        cprintf("derived metrics:\n");
        if (has_p_msl && s_have_altitude) {
            cprintf("  sea level pressure: %ld.%02ld hPa (altitude: %ld m)\n",
                    (long)(p_msl / 100), (long)(p_msl % 100), (long)s_altitude_m);
        }
        if (has_dp) {
            cprintf("  dew point: %ld.%02ld C\n", (long)(dp / 100), (long)(dp % 100));
        }
        if (has_ah) {
            cprintf("  absolute humidity: %ld.%02ld g/m3\n", (long)(ah / 100), (long)(ah % 100));
        }
        if (has_iaq) {
            cprintf("  BME680 IAQ: %ld (baseline: %ld kOhm)\n",
                    (long)iaq, (long)(s_bme680_r_base / 1000));
        }
    }

    /* Print ground-truth calibration analysis */
    int32_t co2_ndir = 0, eco2 = 0;
    if (sensor_hub_get_dev(&mhz19b_sensor_dev, SENSOR_CHAN_CO2, &co2_ndir, NULL) &&
        sensor_hub_get(SENSOR_CHAN_ECO2, &eco2, NULL)) {
        cprintf("calibration & ground truth:\n");
        cprintf("  true CO2: %ld ppm (NDIR), eCO2: %ld ppm (MOX ratio: %ld%%)\n",
                (long)co2_ndir, (long)eco2, (long)s_co2_ratio_pct);
        if (s_mox_contaminated) {
            cprintf("  [WARN] MOX contaminated by VOC/solvents (eCO2 > 1.8x true CO2)\n");
        } else if (s_fresh_air_verified) {
            cprintf("  [OK] Clean outdoor ventilation confirmed (CO2 <= 430 ppm)\n");
        }
    }

    sensor_cal_blob_t saved;
    if (sensor_hub_cal_has_saved(&saved)) {
        cprintf("stored calibration:\n");
        cprintf("  persisted in identity store: yes\n");
    }
}

/* --- Calibration Operations (Phase 46) --- */

bool sensor_hub_cal_get_current(sensor_cal_blob_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->magic = SENSOR_CAL_MAGIC;
    out->version = SENSOR_CAL_VERSION;
    out->flags = 1u | (s_fresh_air_verified ? 2u : 0u);

    if (sgp30_is_detected()) {
        uint16_t eco2 = 0, tvoc = 0;
        if (sgp30_get_baseline(&eco2, &tvoc)) {
            out->sgp30_eco2_base = eco2;
            out->sgp30_tvoc_base = tvoc;
        }
    }
    if (ccs811_is_detected()) {
        uint16_t base = 0;
        if (ccs811_get_baseline(&base)) {
            out->ccs811_base = base;
        }
    }
    if (mics6814_is_detected()) {
        uint16_t nh3 = 0, co = 0, no2 = 0;
        if (mics6814_get_r0(&nh3, &co, &no2)) {
            out->mics6814_r0_nh3 = nh3;
            out->mics6814_r0_co  = co;
            out->mics6814_r0_no2 = no2;
        }
    }
    out->bme680_r_base = (uint32_t)(s_bme680_r_base > 0 ? s_bme680_r_base : 34000);
    return true;
}

bool sensor_hub_cal_apply(const sensor_cal_blob_t *cal) {
    if (!cal || cal->magic != SENSOR_CAL_MAGIC) return false;

    if (sgp30_is_detected() && (cal->sgp30_eco2_base || cal->sgp30_tvoc_base)) {
        sgp30_set_baseline(cal->sgp30_eco2_base, cal->sgp30_tvoc_base);
    }
    if (ccs811_is_detected() && cal->ccs811_base) {
        ccs811_set_baseline(cal->ccs811_base);
    }
    if (mics6814_is_detected() && (cal->mics6814_r0_nh3 || cal->mics6814_r0_co || cal->mics6814_r0_no2)) {
        mics6814_set_r0(cal->mics6814_r0_nh3, cal->mics6814_r0_co, cal->mics6814_r0_no2);
    }
    if (cal->bme680_r_base > 0) {
        s_bme680_r_base = (int32_t)cal->bme680_r_base;
    }
    return true;
}

bool sensor_hub_cal_has_saved(sensor_cal_blob_t *out) {
    sensor_cal_blob_t blob;
    if (!node_sensor_cal(&blob)) return false;
    if (blob.magic != SENSOR_CAL_MAGIC) return false;
    if (out) *out = blob;
    return true;
}

bool sensor_hub_cal_save(void) {
    sensor_cal_blob_t cal;
    if (!sensor_hub_cal_get_current(&cal)) return false;
    if (at24c32_is_detected()) {
        (void)sensor_hub_cal_eeprom_save();
    }
    return node_identity_set_sensor_cal(&cal) == NODE_ID_OK;
}

bool sensor_hub_cal_restore(void) {
    sensor_cal_blob_t cal;
    if (!sensor_hub_cal_has_saved(&cal)) return false;
    return sensor_hub_cal_apply(&cal);
}

bool sensor_hub_cal_clear(void) {
    return node_identity_clear_sensor_cal() == NODE_ID_OK;
}

bool sensor_hub_cal_eeprom_has_saved(sensor_cal_blob_t *out) {
    if (!at24c32_is_detected()) return false;
    sensor_cal_blob_t blob;
    int r = at24c32_read(SENSOR_CAL_EEPROM_ADDR, (uint8_t *)&blob, sizeof(blob));
    if (r != (int)sizeof(blob)) return false;
    if (blob.magic != SENSOR_CAL_MAGIC || blob.version != SENSOR_CAL_VERSION) return false;
    if (out) *out = blob;
    return true;
}

bool sensor_hub_cal_eeprom_save(void) {
    if (!at24c32_is_detected()) return false;
    sensor_cal_blob_t cal;
    if (!sensor_hub_cal_get_current(&cal)) return false;
    int w = at24c32_write(SENSOR_CAL_EEPROM_ADDR, (const uint8_t *)&cal, sizeof(cal));
    return w == (int)sizeof(cal);
}

bool sensor_hub_cal_eeprom_restore(void) {
    sensor_cal_blob_t cal;
    if (!sensor_hub_cal_eeprom_has_saved(&cal)) return false;
    return sensor_hub_cal_apply(&cal);
}

bool sensor_hub_cal_eeprom_clear(void) {
    if (!at24c32_is_detected()) return false;
    uint8_t zeros[sizeof(sensor_cal_blob_t)];
    memset(zeros, 0, sizeof(zeros));
    int w = at24c32_write(SENSOR_CAL_EEPROM_ADDR, zeros, sizeof(zeros));
    return w == (int)sizeof(zeros);
}

/* --- Publishing the hub's channels (Phase 45 §11, Phase 46 §7.2) --- */
#include "net/mqttd.h"

static bool hub_chan_source(int32_t *out, void *ctx) {
    sensor_chan_t chan = (sensor_chan_t)(uintptr_t)ctx;
    uint32_t age = 0;
    if (!sensor_hub_get(chan, out, &age)) return false;
    return age <= 3u * sensor_hub_sample_period_s() + 5u;
}

void sensor_hub_register_sources(void) {
    static const mqttd_rule_t env_rule  = { .min_interval_s = 5, .max_interval_s = 300, .delta = 10,   .alpha_shift = 3 };
    static const mqttd_rule_t lux_rule  = { .min_interval_s = 5, .max_interval_s = 300, .delta = 500,  .alpha_shift = 2 };
    static const mqttd_rule_t gas_rule  = { .min_interval_s = 5, .max_interval_s = 300, .delta = 25,   .alpha_shift = 2 };
    static const mqttd_rule_t res_rule  = { .min_interval_s = 5, .max_interval_s = 300, .delta = 5000, .alpha_shift = 3 };

    uint32_t have = 0;
    for (uint32_t i = 0; i < sensor_hub_device_count(); i++) {
        const sensor_dev_t *dev = sensor_hub_device_get(i);
        if (dev) have |= dev->chan_mask;
    }

    /* Include derived channels if dependencies are met */
    if ((have & (1u << SENSOR_CHAN_PRESSURE)) && s_have_altitude) {
        have |= (1u << SENSOR_CHAN_PRESSURE_MSL);
    }
    if ((have & (1u << SENSOR_CHAN_TEMP)) && (have & (1u << SENSOR_CHAN_HUMIDITY))) {
        have |= (1u << SENSOR_CHAN_AH) | (1u << SENSOR_CHAN_DEW_POINT);
    }
    if (bme680_is_detected()) {
        have |= (1u << SENSOR_CHAN_IAQ);
    }

    for (uint32_t ch = 0; ch < SENSOR_CHAN_MAX; ch++) {
        if (!(have & (1u << ch))) continue;
        const mqttd_rule_t *rule = ch == SENSOR_CHAN_LUX ? &lux_rule
                                 : (ch == SENSOR_CHAN_ECO2 || ch == SENSOR_CHAN_TVOC ||
                                    ch == SENSOR_CHAN_CO || ch == SENSOR_CHAN_NO2 || ch == SENSOR_CHAN_NH3 ||
                                    ch == SENSOR_CHAN_CO2 || ch == SENSOR_CHAN_IAQ) ? &gas_rule
                                 : ch == SENSOR_CHAN_GAS_RES ? &res_rule : &env_rule;
        (void)mqttd_add_source(sensor_chan_name((sensor_chan_t)ch), hub_chan_source,
                               (void *)(uintptr_t)ch, sensor_chan_decimals((sensor_chan_t)ch), rule);
    }
}
