#include "drivers/mhz19b.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "lugalos_config.h"
#include <string.h>

/* MH-Z19B NDIR Infrared Carbon Dioxide Sensor.
 * Phase 46, plan/phase46_sensor_framework.md.
 *
 * Implements:
 * - 9600 baud 8N1 UART command/response query protocol (PL011 UART1 on RP2350)
 * - True NDIR physical CO2 concentration (0-2000 or 0-5000 ppm)
 * - Optical chamber temperature extraction
 * - 3-minute preheat stabilization countdown
 * - Sensor Device Contract instance (Category D)
 */

#define CMD_READ_CO2    0x86u
#define CMD_CAL_ZERO    0x87u
#define CMD_CAL_SPAN    0x88u
#define CMD_SET_ABC     0x79u
#define CMD_SET_RANGE   0x99u

static struct {
    bool             detected;
    uint64_t         power_on_ms;

    mhz19b_reading_t last;
    uint64_t         last_ms;
    bool             have_last;

    uint32_t         read_count;
    uint32_t         fail_count;
    const char      *last_fail;
} g;

static bool fail(const char *where) {
    g.last_fail = where;
    g.fail_count++;
    return false;
}

bool        mhz19b_is_detected(void)  { return g.detected; }
const char *mhz19b_part_name(void)    { return "mhz19b"; }

uint32_t    mhz19b_read_count(void)   { return g.read_count; }
uint32_t    mhz19b_fail_count(void)   { return g.fail_count; }
const char *mhz19b_last_failure(void) { return g.last_fail; }

bool mhz19b_is_warming_up(uint32_t *remaining_s) {
    if (!g.detected || g.power_on_ms == 0) return false;
    uint64_t now = time_get_ms();
    uint64_t total_ms = (uint64_t)MHZ19B_WARMUP_PERIOD_S * 1000u;
    if (now >= g.power_on_ms && (now - g.power_on_ms) < total_ms) {
        if (remaining_s) *remaining_s = (uint32_t)((total_ms - (now - g.power_on_ms)) / 1000u);
        return true;
    }
    if (remaining_s) *remaining_s = 0;
    return false;
}

bool mhz19b_cached(mhz19b_reading_t *out, uint32_t *age_s) {
    if (!out || !g.have_last) return false;
    *out = g.last;
    if (age_s) {
        uint64_t now = time_get_ms();
        *age_s = (now >= g.last_ms) ? (uint32_t)((now - g.last_ms) / 1000u) : 0u;
    }
    return true;
}

/* --- Packet Checksum and Decoding --- */

static uint8_t mhz19b_calc_checksum(const uint8_t *pkt) {
    uint8_t sum = 0;
    for (int i = 1; i < 8; i++) {
        sum += pkt[i];
    }
    return (uint8_t)(~sum + 1u);
}

static bool mhz19b_decode_response(const uint8_t *buf, int32_t *out_co2, int32_t *out_temp_c100) {
    if (!buf || buf[0] != 0xFF || buf[1] != CMD_READ_CO2) return false;
    uint8_t want_chk = mhz19b_calc_checksum(buf);
    if (buf[8] != want_chk) return false;

    uint16_t co2 = (uint16_t)(((uint16_t)buf[2] << 8) | buf[3]);
    if (out_co2) *out_co2 = (int32_t)co2;

    if (out_temp_c100) {
        int32_t t = ((int32_t)buf[4] - 40) * 100;
        *out_temp_c100 = t;
    }
    return true;
}

/* --- Hardware UART Layer --- */

#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_MHZ19B_UART_BASE)
#include "arch/rp2350_clocks.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

#define RESETS_BASE       0x40020000UL
#define RESETS_RESET_CLR  (RESETS_BASE + 0x3000 + 0x0)
#define RESETS_RESET_DONE (RESETS_BASE + 0x8)
#define RESETS_UART1_BIT  (1u << 27)

#define IO_BANK0_BASE     0x40028000UL
#define IO_BANK0_CTRL(n)  (IO_BANK0_BASE + 0x004 + (n) * 8)
#define PADS_BANK0_BASE   0x40038000UL
#define PADS_BANK0_PAD(n) (PADS_BANK0_BASE + 0x004 + (n) * 4)

#define U1        ((uintptr_t)CONFIG_MHZ19B_UART_BASE)
#define U1_DR     (U1 + 0x00)
#define U1_FR     (U1 + 0x18)
#define U1_IBRD   (U1 + 0x24)
#define U1_FBRD   (U1 + 0x28)
#define U1_LCR_H  (U1 + 0x2C)
#define U1_CR     (U1 + 0x30)

#define FR_RXFE   (1u << 4)   /* receive FIFO empty */
#define FR_TXFF   (1u << 5)   /* transmit FIFO full */

static bool set_funcsel(unsigned gpio, uint32_t fn) {
    for (int attempt = 0; attempt < 10; attempt++) {
        REG(IO_BANK0_CTRL(gpio)) = fn;
        if ((REG(IO_BANK0_CTRL(gpio)) & 0x1fu) == fn) return true;
    }
    return false;
}

static bool hw_uart_init(void) {
    REG(RESETS_RESET_CLR) = RESETS_UART1_BIT;
    int timeout = 10000;
    while (!(REG(RESETS_RESET_DONE) & RESETS_UART1_BIT) && --timeout > 0) {}
    if (timeout == 0) return false;

    if (!set_funcsel(CONFIG_MHZ19B_TX_GPIO, 2u)) return false; /* UART1 TX */
    REG(PADS_BANK0_PAD(CONFIG_MHZ19B_TX_GPIO)) = 0x56;          /* schmitt, 4mA, IE */
    if (!set_funcsel(CONFIG_MHZ19B_RX_GPIO, 2u)) return false; /* UART1 RX */
    REG(PADS_BANK0_PAD(CONFIG_MHZ19B_RX_GPIO)) = 0x5A;          /* schmitt, PUE, 4mA, IE */

    /* Baud rate: 9600, 8N1 */
    uint32_t div64 = (uint32_t)((4ull * CONFIG_CLK_SYS_HZ) / 9600u);
    REG(U1_IBRD) = div64 / 64u;
    REG(U1_FBRD) = div64 % 64u;
    REG(U1_LCR_H) = (3u << 5) | (1u << 4); /* 8 bits, FIFOs enabled */
    REG(U1_CR) = (1u << 0) | (1u << 8) | (1u << 9); /* UARTEN, TXE, RXE */
    return true;
}

static void hw_uart_flush(void) {
    while ((REG(U1_FR) & FR_RXFE) == 0) {
        (void)REG(U1_DR);
    }
}

static bool hw_uart_tx(const uint8_t *pkt, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        int guard = 100000;
        while ((REG(U1_FR) & FR_TXFF) && --guard > 0) {}
        if (guard == 0) return false;
        REG(U1_DR) = pkt[i];
    }
    return true;
}

static bool hw_uart_rx(uint8_t *pkt, uint32_t len, uint32_t timeout_ms) {
    uint64_t deadline = time_get_ms() + timeout_ms;
    for (uint32_t i = 0; i < len; i++) {
        while ((REG(U1_FR) & FR_RXFE)) {
            if (time_get_ms() >= deadline) return false;
            sched_yield();
        }
        pkt[i] = (uint8_t)(REG(U1_DR) & 0xFF);
    }
    return true;
}

static bool hw_uart_xfer(const uint8_t *tx, uint8_t *rx, uint32_t timeout_ms) {
    hw_uart_flush();
    if (!hw_uart_tx(tx, 9u)) return false;
    if (!rx) return true;
    return hw_uart_rx(rx, 9u, timeout_ms);
}

#else

static bool hw_uart_init(void) { return false; }
static bool hw_uart_xfer(const uint8_t *tx, uint8_t *rx, uint32_t timeout_ms) {
    (void)tx; (void)rx; (void)timeout_ms; return false;
}

#endif

/* --- Driver Operations --- */

bool mhz19b_init(void) {
    g.detected = false;

    if (!hw_uart_init()) {
        return false;
    }

    g.power_on_ms = time_get_ms();

    /* Probe sensor with read command */
    mhz19b_reading_t r;
    if (!mhz19b_read(&r)) {
        return false;
    }

    g.detected = true;
    printk("[MH-Z19B] NDIR CO2 sensor active on UART1 (GP%u/GP%u, CO2=%ld ppm, chamber=%ld.%02ld C).\n",
#if defined(CONFIG_MHZ19B_TX_GPIO) && defined(CONFIG_MHZ19B_RX_GPIO)
           CONFIG_MHZ19B_TX_GPIO, CONFIG_MHZ19B_RX_GPIO,
#else
           0, 0,
#endif
           (long)r.co2_ppm, (long)(r.temp_c100 / 100), (long)(r.temp_c100 % 100));
    return true;
}

bool mhz19b_read(mhz19b_reading_t *out) {
    static const uint8_t query_cmd[9] = {
        0xFF, 0x01, CMD_READ_CO2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79
    };
    uint8_t resp[9];

    if (!hw_uart_xfer(query_cmd, resp, 80u)) {
        return fail("uart timeout/no response");
    }

    int32_t co2 = 0;
    int32_t temp_c100 = 0;
    if (!mhz19b_decode_response(resp, &co2, &temp_c100)) {
        return fail("bad packet checksum or framing");
    }

    mhz19b_reading_t r;
    r.co2_ppm = co2;
    r.temp_c100 = temp_c100;
    uint32_t rem = 0;
    r.is_warming_up = mhz19b_is_warming_up(&rem);
    r.warmup_remaining_s = (uint16_t)rem;

    if (out) *out = r;
    g.last = r;
    g.last_ms = time_get_ms();
    g.have_last = true;
    g.read_count++;
    return true;
}

bool mhz19b_set_abc(bool enable) {
    uint8_t cmd[9] = {
        0xFF, 0x01, CMD_SET_ABC, enable ? 0xA0u : 0x00u, 0, 0, 0, 0, 0
    };
    cmd[8] = mhz19b_calc_checksum(cmd);
    return hw_uart_xfer(cmd, NULL, 50u);
}

bool mhz19b_calibrate_zero(void) {
    static const uint8_t cmd[9] = {
        0xFF, 0x01, CMD_CAL_ZERO, 0, 0, 0, 0, 0, 0x78
    };
    return hw_uart_xfer(cmd, NULL, 50u);
}

uint32_t mhz19b_selftest(bool report) {
    uint32_t failed = 0;

    /* 1. Verify command checksums */
    uint8_t cmd86[9] = { 0xFF, 0x01, 0x86, 0, 0, 0, 0, 0, 0 };
    if (mhz19b_calc_checksum(cmd86) != 0x79) failed++;

    uint8_t cmd79[9] = { 0xFF, 0x01, 0x79, 0, 0, 0, 0, 0, 0 };
    if (mhz19b_calc_checksum(cmd79) != 0x86) failed++;

    uint8_t cmd87[9] = { 0xFF, 0x01, 0x87, 0, 0, 0, 0, 0, 0 };
    if (mhz19b_calc_checksum(cmd87) != 0x78) failed++;

    /* 2. Verify response packet decoding */
    static const uint8_t resp_400ppm[9] = {
        0xFF, 0x86, 0x01, 0x90, 0x42, 0x00, 0x00, 0x00, 0xA7
    };
    int32_t co2 = 0, temp = 0;
    if (!mhz19b_decode_response(resp_400ppm, &co2, &temp) || co2 != 400 || temp != 2600) {
        failed++;
        if (report) cprintf("  mhz19b vector 1 (400ppm) decode failed\n");
    }

    static const uint8_t resp_1000ppm[9] = {
        0xFF, 0x86, 0x03, 0xE8, 0x46, 0x00, 0x00, 0x00, 0x49
    };
    if (!mhz19b_decode_response(resp_1000ppm, &co2, &temp) || co2 != 1000 || temp != 3000) {
        failed++;
        if (report) cprintf("  mhz19b vector 2 (1000ppm) decode failed\n");
    }

    /* 3. Corrupted checksum rejection */
    static const uint8_t resp_bad_chk[9] = {
        0xFF, 0x86, 0x03, 0xE8, 0x46, 0x00, 0x00, 0x00, 0x50
    };
    if (mhz19b_decode_response(resp_bad_chk, &co2, &temp)) {
        failed++;
        if (report) cprintf("  mhz19b bad checksum was erroneously accepted\n");
    }

    /* 4. Corrupted start byte rejection */
    static const uint8_t resp_bad_start[9] = {
        0xFE, 0x86, 0x03, 0xE8, 0x46, 0x00, 0x00, 0x00, 0x49
    };
    if (mhz19b_decode_response(resp_bad_start, &co2, &temp)) {
        failed++;
        if (report) cprintf("  mhz19b bad start byte was erroneously accepted\n");
    }

    if (report) {
        cprintf("mhz19b selftest: %lu case%s failed\n",
                (unsigned long)failed, failed == 1u ? "" : "s");
    }
    return failed;
}

void mhz19b_print_status(void) {
    if (!g.detected) {
        cprintf("sensor: mhz19b not detected\n");
        return;
    }
    mhz19b_reading_t r;
    if (!mhz19b_read(&r)) {
        cprintf("sensor: mhz19b read failed: %s (fail count %lu)\n",
                g.last_fail ? g.last_fail : "?", (unsigned long)g.fail_count);
        return;
    }
    uint32_t rem = 0;
    if (mhz19b_is_warming_up(&rem)) {
        cprintf("mhz19b at UART1 (warming up: %lum %02lus left): %ld ppm CO2 (NDIR true), chamber %ld.%02ld C\n",
                (unsigned long)(rem / 60), (unsigned long)(rem % 60),
                (long)r.co2_ppm, (long)(r.temp_c100 / 100), (long)(r.temp_c100 % 100));
    } else {
        cprintf("mhz19b at UART1: %ld ppm CO2 (NDIR true), chamber %ld.%02ld C\n",
                (long)r.co2_ppm, (long)(r.temp_c100 / 100), (long)(r.temp_c100 % 100));
    }
}

/* --- Sensor Device Contract (Category D) --- */

static bool mhz19b_dev_init(struct sensor_dev *dev) {
    if (!mhz19b_init()) return false;
    dev->addr = 0; /* UART interface, not I2C */
    dev->chan_mask = (1u << SENSOR_CHAN_CO2);
    return true;
}

static bool mhz19b_dev_sample(struct sensor_dev *dev) {
    (void)dev;
    mhz19b_reading_t r;
    return mhz19b_read(&r);
}

static bool mhz19b_dev_get_value(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out_val) {
    (void)dev;
    if (!g.have_last || !out_val) return false;
    if (chan == SENSOR_CHAN_CO2) {
        *out_val = g.last.co2_ppm;
        return true;
    }
    return false;
}

static uint32_t mhz19b_dev_selftest(bool report) {
    return mhz19b_selftest(report);
}

static const sensor_ops_t mhz19b_ops = {
    .init      = mhz19b_dev_init,
    .sample    = mhz19b_dev_sample,
    .get_value = mhz19b_dev_get_value,
    .selftest  = mhz19b_dev_selftest,
};

sensor_dev_t mhz19b_sensor_dev = {
    .name      = "mhz19b",
    .addr      = 0,
    .chan_mask = (1u << SENSOR_CHAN_CO2),
    .ops       = &mhz19b_ops,
    .priv      = NULL,
};
