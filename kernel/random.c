/*
 * Unpredictable bytes — N1, plan/phase18_networking_and_auth.md.
 * See kernel/include/kernel/random.h for the contract and why it is honest
 * about targets that have no entropy source.
 */

#include "kernel/random.h"
#include "kernel/sha256.h"
#include "kernel/time.h"
#include "kernel/console.h"
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

bool random_is_hardware(void) { return true; }
const char *random_source(void) { return "ROSC phase noise, SHA-256 whitened"; }

#else  /* the QEMU targets: no entropy source at all, and it must show */

bool random_is_hardware(void) { return false; }
const char *random_source(void) { return "timer+counter (NOT random)"; }

#endif

/* Rolled into every request so that two calls in the same microsecond, or two
 * boards with identical timers, cannot produce the same bytes. */
static uint64_t g_counter;

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
        g_counter++;
        sha256_update(&ctx, &g_counter, sizeof(g_counter));

#if defined(CONFIG_BOARD_RP2350)
        /* 512 raw bits, packed eight to a byte. */
        uint8_t pool[64];
        for (unsigned i = 0; i < sizeof(pool); i++) {
            uint8_t b = 0;
            for (unsigned k = 0; k < 8; k++) {
                b = (uint8_t)((b << 1) | rosc_bit(ROSC_SAMPLE_SPACING_US));
            }
            pool[i] = b;
        }
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

    if (ok) cprintf("RANDTEST_OK (usable at %u us spacing)\n", ROSC_SAMPLE_SPACING_US);
    else    cprintf("RANDTEST_WEAK (ones %u%%, flips %u%%, longest run %u -- "
                    "do NOT ship an auth nonce on this)\n",
                    ones_pct, flips_pct, (unsigned)spaced.longest);
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
