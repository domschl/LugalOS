/*
 * Unpredictable bytes.
 * See kernel/include/kernel/random.h for the contract and why it is honest
 * about targets that have no entropy source.
 */

#include "kernel/random.h"
#include "kernel/sha256.h"
#include "kernel/time.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/lock.h"
#include "lugalos_config.h"

#include <string.h>

#if defined(CONFIG_BOARD_RP2350)

/* The ring oscillator's RANDOMBIT register (RP2350 datasheet; address and
 * offset taken from the Pico SDK's own hardware/regs/rosc.h rather than
 * transcribed from prose). Bit 0 is a sample of the free-running oscillator's
 * phase noise against the system clock. */
#define ROSC_BASE      0x400e8000UL
#define ROSC_RANDOMBIT (*(volatile uint32_t *)(ROSC_BASE + 0x20))

#define REG(a) (*(volatile uint32_t *)(a))

/* Microseconds between raw samples.
 *
 * Read back to back, RANDOMBIT is strongly serially correlated -- the
 * oscillator has not moved far between two reads a few clock cycles apart, so
 * consecutive bits repeat. Spacing the samples is what turns it into a source
 * rather than a pattern, and `randtest` measures both spacings side by side
 * precisely so this number is a measurement rather than a guess. */
#define ROSC_SAMPLE_SPACING_US 1u

static void spin_us(uint32_t us) {
    uint64_t start = time_get_us();
    while (time_get_us() - start < us) { /* spin */ }
}

static uint32_t rosc_bit(uint32_t spacing_us) {
    if (spacing_us) spin_us(spacing_us);
    return ROSC_RANDOMBIT & 1u;
}

/* --- the TRNG block (RP2350 datasheet, TRNG chapter; offsets from the Pico
 * SDK's hardware/regs/trng.h) ---------------------------------------------
 *
 * Its own ring oscillator, sampled into a 192-bit Entropy Holding Register
 * (EHR) through a von Neumann balancer, with two health tests in hardware:
 *   CRNGT     two consecutive 16-bit blocks equal. A fair source does that
 *             once in 65536 blocks, so it means "discard this EHR", not "broken".
 *   AUTOCORR  the autocorrelation test failed four times in a row, and VN
 *             (von Neumann) 32 identical raw bits: a stuck or locked source.
 * The Pico SDK runs it raw (tests and balancer off) because it hashes
 * everything; this hashes everything too, but keeps the tests on, because
 * they are what lets random_is_hardware() turn false when the source dies
 * instead of silently whitening a constant. */
#define TRNG_BASE            0x400f0000UL
#define TRNG_RNG_ISR         (TRNG_BASE + 0x104)
#define TRNG_RNG_ICR         (TRNG_BASE + 0x108)
#define TRNG_CONFIG          (TRNG_BASE + 0x10c)
#define TRNG_EHR_DATA0       (TRNG_BASE + 0x114)
#define TRNG_RND_SOURCE_EN   (TRNG_BASE + 0x12c)
#define TRNG_SAMPLE_CNT1     (TRNG_BASE + 0x130)
#define TRNG_DEBUG_CONTROL   (TRNG_BASE + 0x138)
#define TRNG_SW_RESET        (TRNG_BASE + 0x140)
#define TRNG_ISR_EHR_VALID   (1u << 0)
#define TRNG_ISR_AUTOCORR    (1u << 1)
#define TRNG_ISR_CRNGT       (1u << 2)
#define TRNG_ISR_VN          (1u << 3)
#define TRNG_EHR_WORDS       6u

/* RESETS: the atomic clear alias (+0x3000) takes the block out of reset
 * without a read-modify-write of the shared register. */
#define RESETS_BASE          0x40020000UL
#define RESETS_RESET_CLR     (RESETS_BASE + 0x3000)
#define RESETS_RESET_DONE    (RESETS_BASE + 0x8)
#define RESETS_TRNG          (1u << 25)

/* rng_clk cycles between two oscillator samples. The reset value (65535) makes
 * one EHR take hundreds of milliseconds; the SDK uses 0 with the balancer off.
 * `randtest` sweeps several values (correlation of the output bits, health
 * errors, time per EHR) and this is the one chosen from that sweep. */
#define TRNG_SAMPLE_CNT      256u
/* Measured on a Pico 2 W (2026-10-25), 128 EHRs each:
 *     64  ones 49%  flips 42%  2-4 stuck-source resets   0.35 ms/EHR
 *    256  ones 49%  flips 49%  none                      1.3 ms/EHR
 *   1024  ones 49%  flips 50%  none                      5.3 ms/EHR
 * At 64 the oscillator has not decorrelated between samples: the balancer
 * evens out the bias, not the correlation, and the autocorrelation test
 * trips. 256 is the fastest clean value. */
/* Per attempt: generous at TRNG_SAMPLE_CNT (one EHR takes ~1.3 ms), and
 * scaled with the interval for `randtest`'s sweep. */
#define TRNG_FILL_TIMEOUT_US(cnt) (5000u + (cnt) * 40u)
#define TRNG_FILL_ATTEMPTS   4u

/* guarded by: g_trng_lock -- the EHR is one register set, and two callers
 * reading the same 192 bits would derive correlated keys. A yielding lock,
 * not a spinlock: one EHR takes ~1.3 ms of polling, far too long to hold
 * interrupts off, and every caller is a task. */
static ylock_t g_trng_lock;
/* guarded by: g_trng_lock */
static bool     g_trng_up;
/* guarded by: g_trng_lock (the counters `randtest` reports) */
static uint32_t g_trng_fills, g_trng_crngt, g_trng_stuck, g_trng_timeouts;
/* guarded by: g_trng_lock. TRNG_SAMPLE_CNT except while `randtest` sweeps. */
static uint32_t g_trng_sample_cnt = TRNG_SAMPLE_CNT;
/* guarded by: g_trng_lock for the write; read without it by
 * random_is_hardware(), where a stale "still good" for one call is accepted:
 * the flag only ever goes from false to true, and stays until reboot. */
static volatile bool g_trng_failed;

static void trng_configure_locked(void) {
    REG(TRNG_SW_RESET) = 1u;
    REG(TRNG_DEBUG_CONTROL) = 0u;          /* every test and the balancer on */
    REG(TRNG_SAMPLE_CNT1) = g_trng_sample_cnt;
    REG(TRNG_CONFIG) = 0u;
    REG(TRNG_RNG_ICR) = 0xfu;
    REG(TRNG_RND_SOURCE_EN) = 1u;          /* starts collecting into the EHR */
}

static bool trng_start_locked(void) {
    if (g_trng_up) return true;
    REG(RESETS_RESET_CLR) = RESETS_TRNG;
    uint64_t t0 = time_get_us();
    while (!(REG(RESETS_RESET_DONE) & RESETS_TRNG)) {
        if (time_get_us() - t0 > 1000u) return false;
    }
    trng_configure_locked();
    g_trng_up = true;
    return true;
}

/* One EHR: 0 and six words, or -1 when the source failed TRNG_FILL_ATTEMPTS
 * times running -- which marks the TRNG failed until reboot, unless
 * `measuring` (randtest's sweep tries intervals that may well be bad).
 * A CRNGT hit is retried; a stuck source or a timeout resets the block
 * first. Reading the last EHR word starts the next collection. */
static int trng_fill_locked(uint32_t out[TRNG_EHR_WORDS], bool measuring) {
    if (!trng_start_locked()) return -1;
    for (unsigned attempt = 0; attempt < TRNG_FILL_ATTEMPTS; attempt++) {
        uint64_t t0 = time_get_us();
        uint32_t isr;
        while (!((isr = REG(TRNG_RNG_ISR)) & 0xfu)) {
            if (time_get_us() - t0 > TRNG_FILL_TIMEOUT_US(g_trng_sample_cnt)) break;
        }
        REG(TRNG_RNG_ICR) = isr;
        if (isr & (TRNG_ISR_AUTOCORR | TRNG_ISR_VN)) {
            g_trng_stuck++;
            trng_configure_locked();
            continue;
        }
        if (isr & TRNG_ISR_CRNGT) {
            g_trng_crngt++;
            for (unsigned i = 0; i < TRNG_EHR_WORDS; i++) (void)REG(TRNG_EHR_DATA0 + 4u * i);
            continue;
        }
        if (!(isr & TRNG_ISR_EHR_VALID)) {
            g_trng_timeouts++;
            trng_configure_locked();
            continue;
        }
        for (unsigned i = 0; i < TRNG_EHR_WORDS; i++) out[i] = REG(TRNG_EHR_DATA0 + 4u * i);
        g_trng_fills++;
        return 0;
    }
    if (!measuring) g_trng_failed = true;
    return -1;
}

static int trng_fill_mode(uint32_t out[TRNG_EHR_WORDS], bool measuring) {
    ylock_acquire(&g_trng_lock);
    int rc = trng_fill_locked(out, measuring);
    ylock_release(&g_trng_lock);
    return rc;
}

static int trng_fill(uint32_t out[TRNG_EHR_WORDS]) { return trng_fill_mode(out, false); }

/* Starts the TRNG and takes one health-tested EHR, so a dead source is
 * reported at boot rather than at the first key. */
void random_init(void) {
    uint32_t ehr[TRNG_EHR_WORDS];
    (void)trng_fill(ehr);
    memset(ehr, 0, sizeof(ehr));
    printk("[Random] %s\n", random_source());
}

bool random_is_hardware(void) { return !g_trng_failed; }
const char *random_source(void) {
    return g_trng_failed ? "ROSC only: the TRNG failed its health tests (NOT trusted for keys)"
                         : "TRNG (health-tested) + ROSC phase noise, SHA-256 whitened";
}

#elif defined(CONFIG_BOARD_ESP32C6)

/* The C6's RNG (LPPERI_RNG_DATA, IDF's WDEV_RND_REG). Espressif's own
 * statement of what it is worth: true random numbers *while an entropy source
 * runs* -- the RF subsystem, or the SAR ADC's noise source that IDF's
 * bootloader_random_enable() switches on -- and not otherwise. The second is
 * not ported (the PHY shares that ADC for its temperature sensor), so this
 * target counts as having a hardware source exactly while the radio's RF is
 * up (45.8). Before that, key generation refuses, as on a target with none;
 * a network-facing nonce is not needed before there is a network. */
#include "kernel/radio_c6.h"
#define RNG_DATA (*(volatile uint32_t *)0x600B2808UL)

/* IDF's esp_random() on the C6 (esp_hw_support/hw_random.c): one byte per
 * 160 * 16 APB cycles -- 64 us at the C6's 40 MHz APB -- "the rate at which
 * the RNG was tested", every read in that window XORed in. The register is a
 * PRNG the RF noise stirs; read faster and the words carry its state more than
 * the noise, which no bit statistics can show (they measured fine at 1 us). */
#define C6_RNG_BYTE_US 64u

static uint32_t rng_word(void) {
    uint32_t w = 0;
    for (unsigned i = 0; i < 4; i++) {
        uint64_t t0 = time_get_us();
        uint32_t x = 0;
        do { x ^= RNG_DATA; } while (time_get_us() - t0 < C6_RNG_BYTE_US);
        w ^= x << (8 * i);
    }
    return w ^ RNG_DATA;
}

/* Nothing to start: the source is the radio's RF, which the radio brings up. */
void random_init(void) {}

bool random_is_hardware(void) { return radio_c6_rf_on(); }
const char *random_source(void) {
    return radio_c6_rf_on() ? "RNG with the radio's RF on, SHA-256 whitened"
                            : "RNG without an entropy source (radio off: NOT random)";
}

#elif defined(CONFIG_BOARD_ESP32P4)

/* The P4's RNG register is only random while an entropy source runs, and the
 * P4 has no radio: the source is the SAR ADC sampling an internal channel
 * continuously. The sequence below is IDF's bootloader_random_enable()
 * (components/bootloader_support/src/bootloader_random_esp32p4.c), with each
 * low-level call expanded from esp_hal_ana_conv/esp32p4/include/hal/adc_ll.h,
 * esp_hal_regi2c/esp32p4/include/hal/regi2c_ctrl_ll.h and hal/rng_ll.h;
 * addresses and fields from soc/esp32p4/register/hw_ver{1,3}, whose layouts
 * agree for every field used here. Nothing else in LugalOS uses the P4's SAR
 * ADC, so it runs for good once started.
 *
 * Revisions: v1.x reads LP_SYSTEM_REG_RNG_DATA_REG; v3.x has a separate
 * LP_TRNG block that needs its own clock, reset release and sampling timer. */
#include "kernel/sysreg.h"
#include "arch/clk_esp32p4.h"

#define P4_HP_SYS_CLKRST        0x500E6000UL
#define P4_SOC_CLK_CTRL2        (P4_HP_SYS_CLKRST + 0x1c)   /* [5] ADC_APB_CLK_EN */
#define P4_PERI_CLK_CTRL22      (P4_HP_SYS_CLKRST + 0x9c)   /* [31:30] ADC_CLK_SRC_SEL, 0 = XTAL */
#define P4_PERI_CLK_CTRL23      (P4_HP_SYS_CLKRST + 0xa0)   /* [0] ADC_CLK_EN, [8:1] DIV_NUM,
                                                              [16:9] NUMERATOR, [24:17] DENOMINATOR */
#define P4_HP_RST_EN2           (P4_HP_SYS_CLKRST + 0xc8)   /* [10] RST_EN_ADC */

#define P4_ADC                  0x500DE000UL
#define P4_ADC_CTRL_REG         (P4_ADC + 0x00)  /* [5] SAR_CLK_GATED, [13:6] SAR_CLK_DIV,
                                                    [17:14] SAR1_PATT_LEN, [27:26] XPD_SAR1_FORCE */
#define P4_ADC_CTRL2            (P4_ADC + 0x04)  /* [11] TIMER_SEL, [23:12] TIMER_TARGET, [24] TIMER_EN */
#define P4_ADC_SAR1_PATT_TAB1   (P4_ADC + 0x18)  /* four 6-bit entries, the first in [23:18] */

#define P4_LP_ADC               0x50127000UL
#define P4_LP_ADC_MEAS1_CTRL2   (P4_LP_ADC + 0x0c)  /* [18] MEAS1_START_FORCE, [31] SAR1_EN_PAD_FORCE */
#define P4_LP_ADC_MEAS1_MUX     (P4_LP_ADC + 0x10)  /* [31] SAR1_DIG_FORCE */

#define P4_PMU_RF_PWC           0x5011515CUL     /* [26] PERIF_I2C_RSTB, [27] XPD_PERIF_I2C */

/* The SAR ADC's analog block on the internal I2C (soc/regi2c_saradc.h), and
 * its master select (esp_hal_regi2c/esp32p4/regi2c_impl.c). */
#define I2C_SAR_ADC             0x69u
#define I2C_SAR_ADC_MST_SEL     (1u << 7)
#define SAR1_INIT_CODE          2166u            /* IDF's I2C_SAR_ADC_INIT_CODE_VAL */

#if CONFIG_ESP32P4_REV >= 300
#define P4_TRNG                 0x50126000UL
#define P4_TRNG_CFG             (P4_TRNG + 0x0)  /* [0] SAMPLE_ENABLE, [8:1] TIMER_PSCALE, [9] TIMER_EN */
#define P4_RNG_DATA             (P4_TRNG + 0x4)
#define P4_TRNG_RSTN            (P4_TRNG + 0x8)  /* [0] RSTN */
#define P4_TRNG_DATE            (P4_TRNG + 0xc)  /* [31] CLK_EN */
#else
#define P4_RNG_DATA             0x501101A4UL     /* LP_SYSTEM_REG_RNG_DATA_REG */
#endif

#define REG(a) (*(volatile uint32_t *)(a))

/* accepted: written once by random_init() at boot, before any task that asks
 * for random bytes exists; read-only after. */
static bool g_p4_entropy_on;
/* accepted: as g_p4_entropy_on. Microseconds per byte, see rng_word(). */
static uint32_t g_p4_byte_us = 60;

/* IDF's esp_random() on the P4 (esp_hw_support/hw_random.c): the register is
 * a PRNG that the noise source stirs a few bits per APB cycle, "tested with
 * around 75 KHz bytes reading frequency" -- one byte per 5040 APB cycles,
 * every read in that window XORed in. Read faster and the words are the
 * PRNG's state more than the noise: at 1 us per word the NANO measured 58%
 * ones over 65536 bits, steadily. So: four bytes, each g_p4_byte_us long. */
static uint32_t rng_word(void) {
    uint32_t w = 0;
    for (unsigned i = 0; i < 4; i++) {
        uint64_t t0 = time_get_us();
        uint32_t x = 0;
        do { x ^= REG(P4_RNG_DATA); } while (time_get_us() - t0 < g_p4_byte_us);
        w ^= x << (8 * i);
    }
    return w ^ REG(P4_RNG_DATA);
}

static bool sar_write(uint8_t reg, unsigned msb, unsigned lsb, uint8_t val) {
    return esp32p4_regi2c_write_field(I2C_SAR_ADC, I2C_SAR_ADC_MST_SEL, reg, msb, lsb, val);
}

/* A source that has stopped hands back the same word, or a few. Not a
 * statistical test (`randtest` is the measurement): a guard against
 * declaring a dead source live. */
static bool rng_looks_alive(void) {
    uint32_t prev = rng_word(), repeats = 0, ored = prev, anded = prev;
    for (unsigned i = 0; i < 64; i++) {
        uint32_t w = rng_word();
        if (w == prev) repeats++;
        ored |= w; anded &= w;
        prev = w;
    }
    return repeats < 4 && ored == 0xffffffffu && anded == 0;
}

void random_init(void) {
    /* Reset the ADC, then clock it from the crystal, undivided. */
    sysreg_set(P4_HP_RST_EN2, 1u << 10);
    sysreg_clear(P4_HP_RST_EN2, 1u << 10);
    sysreg_set(P4_SOC_CLK_CTRL2, 1u << 5);
    sysreg_update(P4_PERI_CLK_CTRL23, 0x01FFFFFEu, 0);      /* DIV_NUM, NUMERATOR, DENOMINATOR = 0 */
    sysreg_set(P4_PERI_CLK_CTRL23, 1u << 0);
    sysreg_clear(P4_PERI_CLK_CTRL22, 3u << 30);
    REG(P4_ADC_CTRL_REG) |= 1u << 5;

    /* The analog I2C path to the SAR block: reset, power, release. */
    sysreg_clear(P4_PMU_RF_PWC, 1u << 26);
    time_delay_us(1);
    sysreg_set(P4_PMU_RF_PWC, 1u << 27);
    sysreg_set(P4_PMU_RF_PWC, 1u << 26);

    /* adc_ll_regi2c_init() (DTEST 0, ENT 1: the entropy tap), then SAR1's
     * initial code. */
    bool ok = sar_write(0x9, 3, 0, 0) && sar_write(0x9, 4, 4, 1) &&
              sar_write(0x1, 3, 0, (uint8_t)(SAR1_INIT_CODE >> 8)) &&
              sar_write(0x0, 7, 0, (uint8_t)(SAR1_INIT_CODE & 0xffu));

    /* One pattern, repeated in all four slots: ADC1, channel 10, 12 dB
     * attenuation -> (atten 3) | (channel 10 << 2) = 0x2B. Length 1. */
    uint32_t e = 0x2Bu;
    REG(P4_ADC_SAR1_PATT_TAB1) = (e << 18) | (e << 12) | (e << 6) | e;
    REG(P4_ADC_CTRL_REG) &= ~(0xFu << 14);

    /* Digital controller owns ADC1; powered on by software; ADC clock /15;
     * a conversion every 100 cycles, timer-triggered. */
    REG(P4_LP_ADC_MEAS1_MUX) |= 1u << 31;
    REG(P4_LP_ADC_MEAS1_CTRL2) |= (1u << 18) | (1u << 31);
    REG(P4_ADC_CTRL_REG) = (REG(P4_ADC_CTRL_REG) & ~((3u << 26) | (0xFFu << 6)))
                         | (3u << 26) | (15u << 6) | (1u << 5);
    REG(P4_ADC_CTRL2) = (REG(P4_ADC_CTRL2) & ~(0xFFFu << 12)) | (100u << 12);
    REG(P4_ADC_CTRL2) |= (1u << 11) | (1u << 24);

#if CONFIG_ESP32P4_REV >= 300
    REG(P4_TRNG_DATE) |= 1u << 31;
    REG(P4_TRNG_RSTN) = 0;
    REG(P4_TRNG_RSTN) = 1;
    REG(P4_TRNG_CFG) = (255u << 1) | (1u << 9) | (1u << 0);
#endif

    esp32p4_clocks_t clk;
    esp32p4_clocks_read(&clk);
    if (clk.apb_hz >= 1000000u) g_p4_byte_us = (uint32_t)((5040ull * 1000000u + clk.apb_hz - 1u) / clk.apb_hz);

    g_p4_entropy_on = ok && rng_looks_alive();
    printk("[Random] %s (%u us per byte)\n", random_source(), (unsigned)g_p4_byte_us);
}

bool random_is_hardware(void) { return g_p4_entropy_on; }
const char *random_source(void) {
    return g_p4_entropy_on ? "RNG with the SAR ADC noise source on, SHA-256 whitened"
                           : "RNG without its noise source (NOT random)";
}

#elif defined(CONFIG_BOARD_K210)

/* The K210 has no documented entropy source for the general case (65.6):
 * the crypto blocks (SHA512, KPU) are deterministic accelerators, there
 * is no ring oscillator or PLL-jitter reader in any public register map,
 * and no vendor driver claims one. Rather than sample something quietly
 * and call it entropy, the board reports the truth: not hardware, and
 * `identity key --generate` refuses with NO_ENTROPY the way the C6 rule
 * refuses without its radio. random_bytes() still hashes timer+counter,
 * which is fine for public identifiers (a UID provisioned into the
 * identity sector) and worthless against an adversary.
 *
 * An open question for a later milestone, deliberately not decided
 * here: the SAR ADC's floating inputs probably do carry thermal noise
 * the way the P4's internal channel does, and one measured campaign
 * (sample, count LSB jitter, decorrelate) could earn this branch a
 * true source. Until that measurement exists, the answer is no. */
bool random_is_hardware(void) { return false; }
const char *random_source(void) {
    return "none: K210 has no documented entropy source (key generation refuses)";
}

void random_init(void) {}

#else  /* the QEMU targets: no entropy source at all, and it must show */

bool random_is_hardware(void) { return false; }
const char *random_source(void) { return "timer+counter (NOT random)"; }

void random_init(void) {}

#endif

/* Rolled into every request so that two calls in the same microsecond, or two
 * boards with identical timers, cannot produce the same bytes.
 * guarded by: an atomic add; each call hashes the value it took, not the
 * shared word, which another task may have moved on since. */
static uint32_t g_counter;

/*
 * The construction, and why it is a hash rather than the raw bits:
 *
 * A physical source has bias and correlation -- ours certainly does, which is
 * what `randtest` is for. The standard answer is to gather several times more
 * raw bits than you need and compress them through a hash, so that whatever
 * unpredictability is spread thinly across many biased bits ends up
 * concentrated in the output. 512 raw bits per 32 output bytes is a
 * deliberately generous ratio: even if the source were only a fraction of a
 * bit of real entropy per sample, the pool would still be full.
 *
 * The timer and the counter go in too. They are not entropy and are not
 * claimed to be -- they are there so that a request can never repeat a
 * previous one even if the physical source stalls completely.
 */
void random_bytes(void *out, uint32_t len) {
    uint8_t *p = (uint8_t *)out;

    while (len > 0) {
        sha256_ctx_t ctx;
        sha256_init(&ctx);

        uint64_t t = time_get_us();
        sha256_update(&ctx, &t, sizeof(t));
        uint32_t n = __atomic_add_fetch(&g_counter, 1u, __ATOMIC_RELAXED);
        sha256_update(&ctx, &n, sizeof(n));

#if defined(CONFIG_BOARD_RP2350)
        /* Three EHRs (576 balanced, health-tested bits), each taken under the
         * lock on its own so no caller holds interrupts off for all three.
         * Then ROSC bits on top: 128 normally, the full 512 when the TRNG has
         * failed, which is what this pool held before the TRNG was used. */
        uint32_t ehr[TRNG_EHR_WORDS];
        bool trng_ok = true;
        for (unsigned k = 0; k < 3 && trng_ok; k++) {
            trng_ok = trng_fill(ehr) == 0;
            if (trng_ok) sha256_update(&ctx, ehr, sizeof(ehr));
        }
        memset(ehr, 0, sizeof(ehr));
        uint8_t pool[64];
        unsigned rosc_bytes = trng_ok ? 16u : sizeof(pool);
        for (unsigned i = 0; i < rosc_bytes; i++) {
            uint8_t b = 0;
            for (unsigned k = 0; k < 8; k++) {
                b = (uint8_t)((b << 1) | rosc_bit(ROSC_SAMPLE_SPACING_US));
            }
            pool[i] = b;
        }
        sha256_update(&ctx, pool, rosc_bytes);
        memset(pool, 0, sizeof(pool));
#elif defined(CONFIG_BOARD_ESP32C6) || defined(CONFIG_BOARD_ESP32P4)
        uint32_t pool[16];                       /* 512 raw bits */
        for (unsigned i = 0; i < 16; i++) pool[i] = rng_word();
        sha256_update(&ctx, pool, sizeof(pool));
        memset(pool, 0, sizeof(pool));
#endif

        uint8_t digest[SHA256_DIGEST_LEN];
        sha256_final(&ctx, digest);

        uint32_t take = (len < SHA256_DIGEST_LEN) ? len : SHA256_DIGEST_LEN;
        for (uint32_t i = 0; i < take; i++) p[i] = digest[i];
        memset(digest, 0, sizeof(digest));
        p += take;
        len -= take;
    }
}

/* --------------------------------------------------- the measurement --- */

#if defined(CONFIG_BOARD_RP2350)

typedef struct {
    uint32_t ones;
    uint32_t flips;      /* adjacent samples that differ */
    uint32_t longest;    /* longest run of identical samples */
} rand_stats_t;

static void measure(unsigned bits, uint32_t spacing_us, rand_stats_t *st) {
    st->ones = 0;
    st->flips = 0;
    st->longest = 0;

    uint32_t prev = rosc_bit(spacing_us);
    uint32_t run = 1;
    st->ones += prev;

    for (unsigned i = 1; i < bits; i++) {
        uint32_t b = rosc_bit(spacing_us);
        st->ones += b;
        if (b != prev) {
            st->flips++;
            if (run > st->longest) st->longest = run;
            run = 1;
        } else {
            run++;
        }
        prev = b;
    }
    if (run > st->longest) st->longest = run;
}

/* Both figures are percentages of the ideal. A fair, independent source gives
 * 50% ones and 50% flips; the second is the one that matters for a ring
 * oscillator, since correlation is its characteristic failure and bias is
 * not. */
static void report(const char *label, unsigned bits, const rand_stats_t *st) {
    unsigned ones_pct  = (unsigned)((uint64_t)st->ones * 100u / bits);
    unsigned flips_pct = (unsigned)((uint64_t)st->flips * 100u / (bits - 1u));
    cprintf("  %-22s ones %3u%%   flips %3u%%   longest run %u\n",
            label, ones_pct, flips_pct, (unsigned)st->longest);
}

typedef struct {
    rand_stats_t st;
    unsigned bits, failed;
    uint32_t stuck, crngt;      /* health events during this measurement */
    uint64_t us_per_fill;
} trng_stats_t;

/* `fills` EHRs at sample interval `cnt`, then back to the interval in use. */
static void trng_measure(uint32_t cnt, unsigned fills, trng_stats_t *out) {
    ylock_acquire(&g_trng_lock);
    g_trng_sample_cnt = cnt;
    if (g_trng_up) trng_configure_locked();
    uint32_t stuck0 = g_trng_stuck, crngt0 = g_trng_crngt;
    ylock_release(&g_trng_lock);

    memset(out, 0, sizeof(*out));
    uint32_t prev = 2, run = 0;
    uint64_t t0 = time_get_us();
    for (unsigned i = 0; i < fills; i++) {
        uint32_t ehr[TRNG_EHR_WORDS];
        if (trng_fill_mode(ehr, true) != 0) { out->failed++; continue; }
        for (unsigned w = 0; w < TRNG_EHR_WORDS; w++) {
            for (unsigned k = 0; k < 32; k++) {
                uint32_t b = (ehr[w] >> k) & 1u;
                out->st.ones += b;
                if (b == prev) { run++; }
                else { if (prev != 2) out->st.flips++; if (run > out->st.longest) out->st.longest = run; run = 1; }
                prev = b;
            }
        }
    }
    if (run > out->st.longest) out->st.longest = run;
    out->us_per_fill = (time_get_us() - t0) / (fills ? fills : 1u);
    out->bits = (fills - out->failed) * 192u;

    ylock_acquire(&g_trng_lock);
    out->stuck = g_trng_stuck - stuck0;
    out->crngt = g_trng_crngt - crngt0;
    g_trng_sample_cnt = TRNG_SAMPLE_CNT;
    if (g_trng_up) trng_configure_locked();
    ylock_release(&g_trng_lock);
}

static void trng_report(uint32_t cnt, const trng_stats_t *t) {
    if (!t->bits) { cprintf("    SAMPLE_CNT1=%-5u no EHR delivered\n", (unsigned)cnt); return; }
    cprintf("    SAMPLE_CNT1=%-5u ones %3u%%  flips %3u%%  longest run %2u  %6lu us/EHR  "
            "%lu stuck, %lu CRNGT\n", (unsigned)cnt,
            (unsigned)((uint64_t)t->st.ones * 100u / t->bits),
            (unsigned)((uint64_t)t->st.flips * 100u / (t->bits - 1u)),
            (unsigned)t->st.longest, (unsigned long)t->us_per_fill,
            (unsigned long)t->stuck, (unsigned long)t->crngt);
}

int random_selftest(unsigned bits) {
    if (bits < 1024) bits = 1024;
    if (bits > 65536) bits = 65536;

    cprintf("Entropy source: %s\n", random_source());
    cprintf("Measuring %u raw ROSC bits. A fair source reads 50%% / 50%%;\n"
            "the flip rate is the one that matters -- correlation is a ring\n"
            "oscillator's characteristic failure, bias is not.\n", bits);

    rand_stats_t fast, spaced;
    measure(bits, 0, &fast);
    measure(bits, ROSC_SAMPLE_SPACING_US, &spaced);

    report("back-to-back reads", bits, &fast);
    report("1 us apart (in use)", bits, &spaced);

    unsigned ones_pct  = (unsigned)((uint64_t)spaced.ones * 100u / bits);
    unsigned flips_pct = (unsigned)((uint64_t)spaced.flips * 100u / (bits - 1u));

    /* Deliberately loose. This is a sanity check on a physical source feeding
     * a hash, not a statistical test suite: what it has to catch is a source
     * that is stuck, nearly stuck, or a counter in disguise -- not one that is
     * merely imperfect, which it certainly is and which the whitening in
     * random_bytes() exists to absorb. */
    bool ok = (ones_pct >= 35u && ones_pct <= 65u)
           && (flips_pct >= 25u && flips_pct <= 75u)
           && (spaced.longest < 32u);

    cprintf("  (ROSC is the secondary source now; its figures are for comparison.)\n");

    /* The TRNG: a sweep over the sample interval, then the value in use with
     * the full sample. Flips are the figure that matters: the balancer
     * removes bias, not correlation, and a source sampled faster than its
     * oscillator decorrelates shows fewer flips and more autocorrelation
     * resets. */
    static const uint32_t sweep[] = { 64u, 256u, 1024u };
    cprintf("  TRNG sweep (128 EHRs each):\n");
    for (unsigned i = 0; i < sizeof(sweep) / sizeof(sweep[0]); i++) {
        trng_stats_t st;
        trng_measure(sweep[i], 128u, &st);
        trng_report(sweep[i], &st);
    }
    trng_stats_t use;
    trng_measure(TRNG_SAMPLE_CNT, (bits + 191u) / 192u, &use);
    cprintf("  TRNG in use:\n");
    trng_report(TRNG_SAMPLE_CNT, &use);

    ylock_acquire(&g_trng_lock);
    uint32_t crngt = g_trng_crngt, stuck = g_trng_stuck, timeouts = g_trng_timeouts, total = g_trng_fills;
    ylock_release(&g_trng_lock);
    cprintf("  TRNG since boot: %lu fills, %lu CRNGT retries, %lu stuck-source resets, %lu timeouts%s\n",
            (unsigned long)total, (unsigned long)crngt, (unsigned long)stuck, (unsigned long)timeouts,
            g_trng_failed ? "; FAILED (keys refused)" : "");

    bool trng_ok = !g_trng_failed && use.failed == 0 && use.bits > 0;
    unsigned t_ones = 0, t_flips = 0;
    if (trng_ok) {
        t_ones = (unsigned)((uint64_t)use.st.ones * 100u / use.bits);
        t_flips = (unsigned)((uint64_t)use.st.flips * 100u / (use.bits - 1u));
        trng_ok = t_ones >= 45u && t_ones <= 55u && t_flips >= 45u && t_flips <= 55u && use.st.longest < 32u;
    }
    (void)ok;
    ok = trng_ok;
    if (ok) cprintf("RANDTEST_OK (TRNG healthy at SAMPLE_CNT1=%u)\n", TRNG_SAMPLE_CNT);
    else    cprintf("RANDTEST_WEAK (TRNG ones %u%%, flips %u%%, longest run %u%s)\n",
                    t_ones, t_flips, (unsigned)use.st.longest,
                    g_trng_failed ? ", failed its health tests" : "");
    return ok ? 0 : 1;
}

#elif defined(CONFIG_BOARD_ESP32C6) || defined(CONFIG_BOARD_ESP32P4)

/* The RNG register hands out whole words. The same three figures as the
 * RP2350's measurement, over the words' bits, plus repeated words: a stalled
 * source shows as the same word twice long before its bits look biased. */
int random_selftest(unsigned bits) {
    if (bits < 1024) bits = 1024;
    if (bits > 65536) bits = 65536;
    unsigned words = bits / 32u;

    cprintf("Entropy source: %s\n", random_source());
    cprintf("Measuring %u RNG words, read as random_bytes() reads them.\n", words);

    uint32_t ones = 0, flips = 0, longest = 0, run = 0, prev_bit = 2, repeats = 0;
    uint32_t prev_word = rng_word();
    for (unsigned i = 0; i < words; i++) {
        uint32_t w = rng_word();
        if (w == prev_word) repeats++;
        prev_word = w;
        for (unsigned k = 0; k < 32; k++) {
            uint32_t b = (w >> k) & 1u;
            ones += b;
            if (b == prev_bit) { run++; }
            else { if (prev_bit != 2) flips++; if (run > longest) longest = run; run = 1; }
            prev_bit = b;
        }
    }
    if (run > longest) longest = run;
    unsigned n = words * 32u;
    unsigned ones_pct = (unsigned)((uint64_t)ones * 100u / n);
    unsigned flips_pct = (unsigned)((uint64_t)flips * 100u / (n - 1u));
    cprintf("  RNG words              ones %3u%%   flips %3u%%   longest run %u   repeated words %u\n",
            ones_pct, flips_pct, (unsigned)longest, (unsigned)repeats);

    bool ok = random_is_hardware() && ones_pct >= 45u && ones_pct <= 55u &&
              flips_pct >= 45u && flips_pct <= 55u && longest < 32u && repeats == 0;
    if (ok) cprintf("RANDTEST_OK\n");
    else if (!random_is_hardware())
        cprintf("RANDTEST_WEAK (the entropy source is off: these bits are NOT random, and key\n"
                "  generation refuses)\n");
    else cprintf("RANDTEST_WEAK (ones %u%%, flips %u%%, longest run %u, repeated words %u)\n",
                 ones_pct, flips_pct, (unsigned)longest, (unsigned)repeats);
    return ok ? 0 : 1;
}

#else

int random_selftest(unsigned bits) {
    (void)bits;
    cprintf("Entropy source: %s\n", random_source());
    cprintf("RANDTEST_SKIP (this target has no hardware entropy source; the\n"
            "  measurement is a hardware deliverable and there is nothing here\n"
            "  to measure. random_bytes() still returns bytes -- they are fine\n"
            "  for tests and worthless against an adversary, which is what\n"
            "  random_is_hardware() reports to the auth path.)\n");
    return 0;
}

#endif
