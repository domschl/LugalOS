/*
 * LugalOS Hardware Driver: DCF-77 longwave time-signal receiver, RP2350 pin
 * layer + bring-up probe (D2, plan/phase17_clock_ui_and_dcf77.md).
 *
 * Hardware Mapping (cmake/board-rp2350-clock.cmake is the source of truth):
 *   GP27 : OUT  (demodulated second pulse, digital input, header pin 32)
 *   GP28 : PON  (receiver enable,          digital output, header pin 34)
 *   module VDD/GND -> 3V3(OUT)/GND (header pins 36/38)
 *
 * Same SIO/PADS/IO_BANK0 bit-banging shape as drivers/pico_clock_green_
 * rp2350.c and drivers/tm1638_rp2350.c -- plain GPIO, no peripheral
 * involved. The two pins are the ones farthest from the LED matrix's
 * GP10-13, which is the loudest interferer on this board (phase17 section 3).
 *
 * What this file deliberately does NOT contain: the DCF-77 frame decoder.
 * That is target-independent (it consumes timestamped samples, not
 * registers) and lives in its own file so it can be tested on QEMU without a
 * radio -- see plan/phase17_clock_ui_and_dcf77.md D1. This file's job is to
 * make the pin talk and to prove, on real hardware, which way round the two
 * polarities actually are.
 *
 * Both polarities are genuinely unknown until measured. Module datasheets
 * for this class of receiver disagree with each other: PON is "power on when
 * low" on most and the opposite on a few, and OUT idles low with high pulses
 * on most and inverted on a few. Neither is something the code can look up,
 * so dcf77_probe() works both out from what the pin does and reports what it
 * found.
 */

#include "drivers/dcf77.h"
#include "drivers/dcf77_decode.h"
#include "drivers/i2c_rtc.h"
#include "kernel/timezone.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "lugalos_config.h"

/* After lugalos_config.h, necessarily: the guard is defined there. */
#if CONFIG_ENABLE_PICO_CLOCK_GREEN
#include "drivers/pico_clock_green.h"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IO_BANK0_BASE           0x40028000UL
#define IO_BANK0_CTRL(n)        (IO_BANK0_BASE + 0x004 + (n) * 8)

#define PADS_BANK0_BASE         0x40038000UL
#define PADS_BANK0_PAD(n)       (PADS_BANK0_BASE + 0x004 + (n) * 4)

#define SIO_BASE                0xD0000000UL
#define SIO_GPIO_IN             (SIO_BASE + 0x004)
#define SIO_GPIO_OUT_SET        (SIO_BASE + 0x018)
#define SIO_GPIO_OUT_CLR        (SIO_BASE + 0x020)
#define SIO_GPIO_OE_SET         (SIO_BASE + 0x038)
#define SIO_GPIO_OE_CLR         (SIO_BASE + 0x040)

#define SIO_GPIO_OE             (SIO_BASE + 0x030)

/* ADC (D2 pin diagnostic only). Same register facts as
 * drivers/pico_clock_green_rp2350.c's own ADC block, which verified them
 * against the RP2350 SDK headers rather than RP2040 memory -- GP26/27/28/29
 * are ADC channels 0/1/2/3, so the two DCF-77 pins can be *measured*, not
 * just read as a bit. That turns out to be the difference between "the line
 * is low" and knowing why it is low. */
#define RESETS_BASE             0x40020000UL
#define RESETS_RESET_CLR        (RESETS_BASE + 0x3000)
#define RESETS_RESET_DONE       (RESETS_BASE + 0x0008)
#define ADC_RESET_BIT           (1u << 0)

#define CLOCKS_BASE             0x40010000UL
#define CLOCKS_CLK_ADC_CTRL     (CLOCKS_BASE + 0x6C)
#define CLOCKS_CLK_ADC_ENABLE_BIT (1u << 11)

#define ADC_BASE                0x400a0000UL
#define ADC_CS                  (ADC_BASE + 0x00)
#define ADC_RESULT              (ADC_BASE + 0x04)
#define ADC_CS_EN_BIT           (1u << 0)
#define ADC_CS_START_ONCE_BIT   (1u << 2)
#define ADC_CS_READY_BIT        (1u << 8)
#define ADC_CS_AINSEL_LSB       12

#define REG(addr) (*(volatile uint32_t *)(addr))

/* PADS_BANK0 field layout (rp2350 pads_bank0.h): bit0 SLEWFAST, bit1
 * SCHMITT, bit2 PDE, bit3 PUE, bits4-5 DRIVE, bit6 IE, bit7 OD, bit8 ISO.
 * 0x5A = SCHMITT | PUE | DRIVE(4mA) | IE, and -- the part that matters on
 * RP2350 specifically -- ISO cleared, which resets to 1 and leaves the pad
 * isolated until something writes it. Same constant, same reason, as
 * pico_clock_green_rp2350.c's gpio_out_init(). */
#define PAD_SIO_DEFAULT 0x5A

/* The same pad with a different pull, for the diagnostic's pull test, and
 * two analog variants (OD=1, IE=0 -- output driver off and digital input
 * buffer off, which is what the SDK's own adc_gpio_init() does). */
#define PAD_IN_PULLUP   0x5A  /* SCHMITT | PUE | DRIVE 4mA | IE */
#define PAD_IN_PULLDOWN 0x56  /* SCHMITT | PDE | DRIVE 4mA | IE */
#define PAD_IN_NOPULL   0x52  /* SCHMITT |       DRIVE 4mA | IE */
#define PAD_ADC_PULLUP   0x88  /* OD | PUE */
#define PAD_ADC_PULLDOWN 0x84  /* OD | PDE */
#define PAD_ADC_NOPULL   0x80  /* OD */

/* PON is a static enable line: 2 mA drive is plenty, it is quieter than the
 * 4 mA default next to a longwave receiver (phase17 section 3, M2), and it
 * limits the current if pin 34 turns out to be bridged to the AGND pad
 * beside it. IE stays set so the pin's *actual* level can be read back --
 * SIO_GPIO_IN reads 0 for any pad whose input buffer is disabled, which
 * would make a drive-test readback meaningless. */
#define PAD_PON_OUT     0x42  /* SCHMITT | DRIVE 2mA | IE */

/* RP2350 datasheet, electrical characteristics at IOVDD = 3.3 V: the
 * internal pulls are specified only as a *range*, RPU 32-86 kOhm and RPD
 * 36-113 kOhm. Any resistance derived from a pad voltage is therefore a
 * range too, and printing a single number would be false precision -- a
 * factor of 2.7 of it. */
#define RPU_MIN_OHM  32000u
#define RPU_MAX_OHM  86000u
#define RPD_MIN_OHM  36000u
#define RPD_MAX_OHM 113000u

/* Whether PON is wired at all. A board that predates the flag keeps the old
 * behaviour; the clock persona sets it to 0, because the active receiver it
 * ended up with has no enable input -- its reference designs tie that pin to
 * ground -- so the module is simply always on (user, 2026-08-23). With no PON
 * there is nothing to drive, nothing to sweep, and no GPIO to reserve. */
#ifndef CONFIG_DCF77_PON_PRESENT
#define CONFIG_DCF77_PON_PRESENT 1
#endif

#define OUT_PIN  CONFIG_DCF77_OUT_GPIO
#define PON_PIN  CONFIG_DCF77_PON_GPIO
#define OUT_MASK (1u << OUT_PIN)
#define PON_MASK (1u << PON_PIN)

/* Both start from the board file and can be corrected by dcf77_probe().
 * Runtime state rather than compile-time truth, because the board file
 * records what the module is *believed* to do and the pin records what it
 * *does*. */
static bool g_pon_active_low  = (CONFIG_DCF77_PON_ACTIVE_LOW != 0);
/* From the board file, i.e. from the module's polarity jumper -- not guessed.
 * The probe still measures duty cycle, but now only to say when the jumper
 * and this setting disagree. */
static bool g_out_pulse_high  = (CONFIG_DCF77_OUT_ACTIVE_LOW == 0);
static bool g_initialized;

/* ---------------------------------------------------------------- pins -- */

static void pon_drive(bool receiver_on) {
#if CONFIG_DCF77_PON_PRESENT
    bool level = g_pon_active_low ? !receiver_on : receiver_on;
    if (level) REG(SIO_GPIO_OUT_SET) = PON_MASK;
    else       REG(SIO_GPIO_OUT_CLR) = PON_MASK;
#else
    (void)receiver_on;
#endif
}

void dcf77_init(void) {
    /* OUT: input, Schmitt trigger on (RP2350 pad default, kept), pull-up
     * enabled. The pull-up is harmless for a push-pull module output and
     * required for an open-collector one -- and which of those this module
     * is, is another thing only the hardware knows. If it turns out to be
     * open-collector on a long lead, the ~50k internal pull-up is too weak
     * to be quiet and wants an external 10k (phase17 section 2). */
    REG(IO_BANK0_CTRL(OUT_PIN)) = 5; /* GPIO_FUNC_SIO */
    REG(PADS_BANK0_PAD(OUT_PIN)) = PAD_SIO_DEFAULT;
    REG(SIO_GPIO_OE_CLR) = OUT_MASK;

#if CONFIG_DCF77_PON_PRESENT
    /* PON: drive the "receiver off" level *before* enabling the output, so
     * bringing the pad up cannot hand the module a momentary enable pulse. */
    pon_drive(false);
    REG(IO_BANK0_CTRL(PON_PIN)) = 5;
    REG(PADS_BANK0_PAD(PON_PIN)) = PAD_PON_OUT;
    REG(SIO_GPIO_OE_SET) = PON_MASK;
#endif

    g_initialized = true;
}

void dcf77_power(bool on) {
    if (!g_initialized) dcf77_init();
    pon_drive(on);
}

bool dcf77_raw_level(void) {
    return (REG(SIO_GPIO_IN) & OUT_MASK) != 0;
}

bool dcf77_pon_active_low(void)    { return g_pon_active_low; }
bool dcf77_out_pulse_is_high(void) { return g_out_pulse_high; }

static bool pin_level(unsigned pin) {
    return (REG(SIO_GPIO_IN) & (1u << pin)) != 0;
}

/* Polarity-corrected: true means "the carrier is attenuated right now", i.e.
 * a second pulse is in progress, whichever way the module wires that. */
static bool pulse_active(void) {
    bool level = dcf77_raw_level();
    return g_out_pulse_high ? level : !level;
}

/* ------------------------------------------------- shared timing ------ */

#define SAMPLE_US        1000u   /* 1 ms: ~100x finer than the 100/200 ms
                                  * distinction actually needs, so a lost
                                  * scheduling slice costs accuracy, never a
                                  * whole pulse */
/* Matches the decoder's own threshold, and for the same measured reason --
 * see drivers/dcf77_decode.c. These diagnostics exist to show what the
 * decoder sees, so filtering the two differently would make them lie. */
#define DEBOUNCE_MS        25u
#define SURVEY_MS        4000u   /* long enough for 4 second-pulses */
#define TRACE_STEP_MS      25u
#define TRACE_COLS         40u   /* 40 x 25 ms = one second per line */
#define TRACE_LINES         4u

#define BIT0_MIN_MS        60u   /* nominal 100 ms */
#define BIT0_MAX_MS       150u
#define BIT1_MIN_MS       150u   /* nominal 200 ms */
#define BIT1_MAX_MS       300u

/* Ctrl-C-abortable delay ([[standardized_interrupt_polling]], the same
 * console_interrupt_requested()/_clear() convention chess_ui.c and
 * pico_clock_green_rp2350.c already use). Returns false if interrupted.
 *
 * Yields every 50 ms rather than busy-waiting the whole span: unlike the
 * clock's own scan loop, this runs from whatever task evaluated the Lisp
 * primitive, and starving the console task for 30 s would also starve the
 * output this diagnostic exists to print. A yield can cost up to a
 * scheduler quantum of sample resolution, which against a 100 ms pulse is
 * noise, not a lost edge. */
/* How much of the warm-up is actually still ahead of us. With PON wired the
 * receiver was switched on a moment ago, so it is all of it. With PON absent
 * the module has been running since the board was powered, so only the part
 * that has not already elapsed since boot is left -- which, for anything but
 * a sync in the first few seconds of uptime, is none of it. Waiting the full
 * five seconds anyway would not be wrong, just a pointless five seconds on
 * every single sync. */
/* What "done with the receiver" actually amounts to. With PON wired we let go
 * of it; without, the module simply keeps running, and saying "PON released"
 * would be describing a wire that is not there (user, 2026-08-23). */
static const char *powered_down_note(void) {
#if CONFIG_DCF77_PON_PRESENT
    return "Receiver powered down (PON released).\n";
#else
    return "Done; the receiver stays on (no PON to release).\n";
#endif
}

static uint64_t warmup_remaining_ms(void) {
#if CONFIG_DCF77_PON_PRESENT
    return (uint64_t)CONFIG_DCF77_WARMUP_MS;
#else
    uint64_t up = time_get_ms();
    return (up >= (uint64_t)CONFIG_DCF77_WARMUP_MS)
             ? 0u : ((uint64_t)CONFIG_DCF77_WARMUP_MS - up);
#endif
}

static bool wait_abortable(uint64_t ms) {
    uint64_t end = time_get_ms() + ms;
    uint64_t next_yield = time_get_ms() + 50;
    while (time_get_ms() < end) {
        if (console_interrupt_requested()) { console_interrupt_clear(); return false; }
        if (time_get_ms() >= next_yield) { sched_yield(); next_yield = time_get_ms() + 50; }
        time_delay_us(SAMPLE_US);
    }
    return true;
}

/* Read a pad's digital level with the internal pull-down applied, correctly
 * on RP2350 silicon.
 *
 * RP2350-E9 (datasheet, "GPIO / RP2350-E9", affects A2): with IE set, OE
 * clear and the pad voltage anywhere in the undefined logic region, the pad
 * *sources* around 120 uA and latches at roughly 2.2 V. The internal
 * pull-down (36-113 kOhm) is far too weak to overcome that, so a floating
 * pin reads HIGH however hard you pull it down -- the erratum's own words:
 * "the pad pull-down ... isn't strong enough to pull the pad voltage low".
 *
 * The datasheet's workaround is exactly this: keep IE clear while the
 * pull-down does its work (the leakage "only occurs ... when the pad input
 * enable is enabled"), set IE for the instant of the read, and clear it
 * again. "If the pad is already a logic-0, re-enabling the input doesn't
 * disturb the pull-down state."
 *
 * This is not a subtlety that can be skipped: the first version of
 * dcf77_free_pin_survey() did the naive thing and reported all six of this
 * persona's genuinely-free pins as "something is on it". */
static bool read_level_pulled_down(unsigned pin) {
    REG(PADS_BANK0_PAD(pin)) = PAD_IN_PULLDOWN & ~0x40u; /* IE = 0 */
    time_delay_us(2000);
    REG(PADS_BANK0_PAD(pin)) = PAD_IN_PULLDOWN;          /* IE = 1, read now */
    bool lvl = pin_level(pin);
    REG(PADS_BANK0_PAD(pin)) = PAD_IN_PULLDOWN & ~0x40u; /* IE = 0 again */
    return lvl;
}

/* Print the external resistance implied by a pad voltage, as the range the
 * internal pull's own tolerance allows. `to_ground` selects which way round:
 * a pin sitting low under our pull-up has a path to ground of
 * R_pu * V / (Vdd - V); one sitting high under our pull-down has a path to
 * the rail of R_pd * (Vdd - V) / V. */
static void print_resistance(unsigned mv, bool to_ground) {
    uint32_t num = to_ground ? mv : (3300u - mv);
    uint32_t den = to_ground ? (3300u - mv) : mv;
    if (den == 0) { cprintf("open (no path)"); return; }
    uint32_t rmin = to_ground ? RPU_MIN_OHM : RPD_MIN_OHM;
    uint32_t rmax = to_ground ? RPU_MAX_OHM : RPD_MAX_OHM;
    uint32_t lo = (uint32_t)(((uint64_t)rmin * num) / den);
    uint32_t hi = (uint32_t)(((uint64_t)rmax * num) / den);
    if (hi < 10000u) {
        cprintf("%u - %u ohm", (unsigned)lo, (unsigned)hi);
    } else if (hi < 1000000u) {
        cprintf("%u - %u kohm", (unsigned)(lo / 1000), (unsigned)(hi / 1000));
    } else {
        cprintf("%u.%u - %u.%u Mohm",
                (unsigned)(lo / 1000000), (unsigned)((lo % 1000000) / 100000),
                (unsigned)(hi / 1000000), (unsigned)((hi % 1000000) / 100000));
    }
}

/* Count debounced transitions and the raw duty cycle over `ms`. Duty is what
 * settles OUT polarity: a DCF-77 pulse train is ~10-20% "active" by
 * construction (100-200 ms out of every 1000), so whichever level is in the
 * minority is the pulse. */
static bool survey(uint64_t ms, unsigned *edges_out, unsigned *high_permille_out) {
    uint64_t end = time_get_ms() + ms;
    unsigned samples = 0, high = 0, edges = 0;
    bool cur = dcf77_raw_level(), cand = cur;
    uint64_t cand_since = time_get_ms();
    uint64_t next_yield = time_get_ms() + 50;

    while (time_get_ms() < end) {
        bool lvl = dcf77_raw_level();
        uint64_t now = time_get_ms();
        samples++;
        if (lvl) high++;
        if (lvl != cand) { cand = lvl; cand_since = now; }
        else if (cand != cur && (now - cand_since) >= DEBOUNCE_MS) { cur = cand; edges++; }

        if (console_interrupt_requested()) { console_interrupt_clear(); return false; }
        if (now >= next_yield) { sched_yield(); next_yield = now + 50; }
        time_delay_us(SAMPLE_US);
    }
    *edges_out = edges;
    *high_permille_out = samples ? (unsigned)((high * 1000u) / samples) : 0;
    return true;
}

/* ----------------------------------------------------- pin diagnostic -- */
/*
 * Added 2026-08-22, after the first real probe on the soldered module found
 * OUT stuck low: "0 edges, pin high 0.0%" with the internal pull-up enabled.
 * That reading is *not* the "nothing is connected" case -- an unconnected
 * pad with a ~50k pull-up reads HIGH. Something was holding GP27 down, and
 * the probe could not say what, so this measures instead of guessing.
 *
 * GP26/27/28 are ADC channels 0/1/2, so the actual pad voltage is available,
 * and the voltage is what separates the candidates:
 *
 *   ~3.3 V with pull-up, ~0 V with pull-down  -> floating: the pad follows
 *       whatever we pull it to, so nothing outside is driving it. The OUT
 *       wire is not reaching GP27.
 *   ~0 V regardless of the pull                -> held low hard: either a
 *       powered module whose output idles low (which is *normal* when it is
 *       not decoding anything), or GP27 shorted to ground. Header pin 32
 *       (GP27) sits directly beside pin 33 (AGND), so a solder bridge is the
 *       first thing to rule out.
 *   ~0.3-0.8 V with pull-up, ~0 V without      -> the signature of an
 *       *unpowered* module: our pull-up is feeding current through its
 *       input/ESD diode into a dead VDD rail, and a diode drop is what a
 *       diode drop looks like. This one is worth its own case because it
 *       says "check VDD", which is otherwise a multimeter job.
 *
 * Read-only on OUT by design: no attempt to drive GP27 high to see what
 * happens, because if a powered module is driving it low that is a
 * deliberate short between two output drivers. PON is already an output, so
 * driving it both ways and reading it back costs nothing new -- and at 2 mA
 * (PAD_PON_OUT) it also survives the bridged-to-AGND case it is testing for.
 */

static bool g_adc_ready;

static void adc_ensure_init(void) {
    if (g_adc_ready) return;
    /* clk_adc is disabled at reset and has no glitchless mux, so every ADC
     * register access hangs the bus without this -- the boot hang
     * drivers/pico_clock_green_rp2350.c's adc_hw_init() records finding on
     * real hardware. Both files enable it; the write is idempotent. */
    REG(CLOCKS_CLK_ADC_CTRL) = CLOCKS_CLK_ADC_ENABLE_BIT;
    /* Release from reset only if something has not already done so. The
     * clock display driver's init runs at boot on this persona and gets
     * there first; re-resetting the block underneath it would be pointless
     * churn, and on a DCF77-only build (the flags are independent) nothing
     * else would have done it at all. */
    if (!(REG(RESETS_RESET_DONE) & ADC_RESET_BIT)) {
        REG(RESETS_RESET_CLR) = ADC_RESET_BIT;
        int timeout = 10000;
        while (!(REG(RESETS_RESET_DONE) & ADC_RESET_BIT) && --timeout > 0);
    }
    REG(ADC_CS) = ADC_CS_EN_BIT;
    time_delay_us(10);
    g_adc_ready = true;
}

/* Only GP26-29 reach the ADC. Every voltage this file prints is therefore
 * conditional on which pins the board file picked, and the pin numbers are
 * board facts precisely so they can be moved -- so the tools degrade to
 * digital-only rather than reading a garbage channel. */
#define PIN_IS_ADC(p) ((p) >= 26u && (p) <= 29u)
#define MV_NA 0xFFFFFFFFu

static uint16_t adc_read_raw(unsigned channel) {
    adc_ensure_init();
    uint32_t cs = REG(ADC_CS);
    cs &= ~(0xFu << ADC_CS_AINSEL_LSB);
    cs |= (channel & 0xFu) << ADC_CS_AINSEL_LSB;
    cs |= ADC_CS_START_ONCE_BIT;
    REG(ADC_CS) = cs;
    int timeout = 10000;
    while (!(REG(ADC_CS) & ADC_CS_READY_BIT) && --timeout > 0);
    return (uint16_t)(REG(ADC_RESULT) & 0xFFFu);
}

/* 12 bits over the 3.3 V reference: 0.806 mV per count. Integer maths, and
 * the absolute accuracy does not matter here -- the question is which of
 * ~0 mV / ~500 mV / ~3300 mV a reading is nearest, not its third digit. */
static unsigned adc_read_mv(unsigned channel) {
    return (unsigned)(((uint32_t)adc_read_raw(channel) * 3300u) / 4095u);
}

/* Voltage on a pin, or MV_NA if that pin has no ADC channel. */
static unsigned pin_mv(unsigned pin) {
    if (!PIN_IS_ADC(pin)) return MV_NA;
    return adc_read_mv(pin - 26u);
}

static void print_mv(unsigned mv) {
    if (mv == MV_NA) cprintf(" no ADC");
    else             cprintf("%4u mV", mv);
}


/* Set a pad, let it settle, then report both what the digital input buffer
 * sees and what the pad is actually sitting at. A 1 ms settle is orders of
 * magnitude more than a 50k pull-up needs against a few pF of pad and wire,
 * but a long antenna lead is not a few pF and this is not a hot path. */
static void measure(unsigned pin, unsigned adc_pin, uint32_t digital_pad,
                    uint32_t analog_pad, const char *label) {
    REG(PADS_BANK0_PAD(pin)) = digital_pad;
    time_delay_us(1000);
    bool lvl = pin_level(pin);

    REG(PADS_BANK0_PAD(pin)) = analog_pad;
    time_delay_us(1000);
    unsigned mv = pin_mv(adc_pin);

    /* kernel/printk.c supports neither a '-' flag nor a width on %s (only
     * a .N precision), so the column is padded in the string itself. */
    cprintf("  %s: %s (", label, lvl ? "HIGH" : "LOW ");
    print_mv(mv);
    cprintf(")\n");
}

void dcf77_pin_report(void) {
    dcf77_init();

    cprintf("\nDCF-77 pin diagnostic (electrical only -- no decoding).\n");
#if !CONFIG_DCF77_PON_PRESENT
    /* GP%d is still surveyed below, because "what is on this free pin" is a
     * useful answer -- but it is not the receiver's, and a reading here says
     * nothing about the module. */
    cprintf("Note: PON is not wired on this board, so GP%d below is a free pin,\n"
            "not the receiver's enable input.\n", PON_PIN);
#endif
    cprintf("\n");

    cprintf("Register readback (proves the pad writes landed):\n");
    cprintf("  GP%d OUT: IO_CTRL=0x%08x PADS=0x%08x OE=%d\n",
            OUT_PIN, (unsigned)REG(IO_BANK0_CTRL(OUT_PIN)),
            (unsigned)REG(PADS_BANK0_PAD(OUT_PIN)),
            (REG(SIO_GPIO_OE) & OUT_MASK) ? 1 : 0);
    cprintf("  GP%d PON: IO_CTRL=0x%08x PADS=0x%08x OE=%d\n",
            PON_PIN, (unsigned)REG(IO_BANK0_CTRL(PON_PIN)),
            (unsigned)REG(PADS_BANK0_PAD(PON_PIN)),
            (REG(SIO_GPIO_OE) & PON_MASK) ? 1 : 0);
    cprintf("  (funcsel 5 = SIO; PADS bit6 IE, bit3 PUE, bit2 PDE, bit8 ISO must be 0)\n\n");

    /* Sanity-check the ADC against a pin known to be wired to something
     * real, so a 0 mV reading on GP27 below is a measurement and not a dead
     * ADC path. GP26 is the LDR on this board, and it is never 0 or 3300. */
#if defined(CONFIG_CLOCK_ADC_LIGHT_GPIO)
    {
        unsigned mv = adc_read_mv(CONFIG_CLOCK_ADC_LIGHT_GPIO - 26);
        cprintf("ADC self-check: GP%d (LDR) = %u mV -- %s\n\n",
                CONFIG_CLOCK_ADC_LIGHT_GPIO, mv,
                (mv > 50 && mv < 3250) ? "plausible, the ADC path works"
                                       : "suspicious; treat the voltages below with care");
    }
#endif

    cprintf("GP%d (OUT) pull test:\n", OUT_PIN);
    REG(SIO_GPIO_OE_CLR) = OUT_MASK; /* input for all of it */
    measure(OUT_PIN, OUT_PIN, PAD_IN_PULLUP,   PAD_ADC_PULLUP, "internal pull-up  ");
    measure(OUT_PIN, OUT_PIN, PAD_IN_NOPULL,   PAD_ADC_NOPULL, "no pull           ");
    measure(OUT_PIN, OUT_PIN, PAD_IN_PULLDOWN, PAD_ADC_PULLDOWN, "internal pull-down");

    /* Re-measure the two that decide the verdict, now that the pad has
     * settled through the whole sequence. */
    REG(PADS_BANK0_PAD(OUT_PIN)) = PAD_ADC_PULLUP;
    time_delay_us(2000);
    unsigned mv_pu = pin_mv(OUT_PIN);
    REG(PADS_BANK0_PAD(OUT_PIN)) = PAD_IN_PULLUP;
    time_delay_us(1000);
    bool lvl_pu = pin_level(OUT_PIN);
    REG(PADS_BANK0_PAD(OUT_PIN)) = PAD_IN_PULLDOWN;
    time_delay_us(1000);
    bool lvl_pd = pin_level(OUT_PIN);
    REG(PADS_BANK0_PAD(OUT_PIN)) = PAD_IN_PULLUP;

    cprintf("\nGP%d (PON) drive test:\n", PON_PIN);
    REG(PADS_BANK0_PAD(PON_PIN)) = PAD_PON_OUT;
    REG(SIO_GPIO_OE_SET) = PON_MASK;
    REG(SIO_GPIO_OUT_SET) = PON_MASK;
    time_delay_us(1000);
    bool pon_hi = pin_level(PON_PIN);
    unsigned pon_hi_mv = pin_mv(PON_PIN);
    REG(SIO_GPIO_OUT_CLR) = PON_MASK;
    time_delay_us(1000);
    bool pon_lo = pin_level(PON_PIN);
    unsigned pon_lo_mv = pin_mv(PON_PIN);
    cprintf("  driven HIGH -> reads %s (", pon_hi ? "HIGH" : "LOW "); print_mv(pon_hi_mv); cprintf(")\n");
    cprintf("  driven LOW  -> reads %s (", pon_lo ? "HIGH" : "LOW "); print_mv(pon_lo_mv); cprintf(")\n");

    /* --- what each pin is, electrically -------------------------------- */
    /*
     * The measurement that separates "this pin is an input with a bias
     * resistor" from "this pin is an open-drain output with a pull-up",
     * which is the same question as "are OUT and PON swapped". An enable
     * input is biased with something large (100 kOhm upwards -- it only has
     * to beat leakage); an open-drain output is pulled up with something
     * small (4.7-10 kOhm -- it has to produce fast edges). Two orders of
     * magnitude apart, and both are readable with the ADC from here.
     */
    REG(SIO_GPIO_OE_CLR) = PON_MASK; /* PON as input for this, not driven */
    REG(PADS_BANK0_PAD(PON_PIN)) = PAD_ADC_PULLDOWN;
    time_delay_us(2000);
    unsigned pon_mv_pd = pin_mv(PON_PIN);

    if (!PIN_IS_ADC(OUT_PIN) && !PIN_IS_ADC(PON_PIN)) {
        cprintf("\n(Neither pin is on an ADC channel, so no voltages here -- only\n"
                "GP26-29 can be measured. The pull tests above still stand.)\n");
        goto verdict;
    }
    cprintf("\nWhat is on each pin (external resistance implied by the voltage):\n");
    cprintf("  GP%d OUT: %u mV under our pull-up  -> path to ground of ", OUT_PIN, mv_pu);
    print_resistance(mv_pu, true);
    cprintf("\n");
    cprintf("  GP%d PON: %u mV under our pull-down -> path to 3V3 of ", PON_PIN, pon_mv_pd);
    print_resistance(pon_mv_pd, false);
    cprintf("\n");
    /* A *low* reading here means a very high external resistance, which is
     * the most informative outcome -- so it must not be guarded away. An
     * earlier version skipped this line below 100 mV and printed nothing in
     * exactly the case that identifies the pin. */
    if (pon_mv_pd == 0) {
        cprintf("    -> nothing measurable pulling this pin up at all\n");
    } else {
        uint32_t r_typ = (uint32_t)(((uint64_t)RPD_MIN_OHM * (3300u - pon_mv_pd)) / pon_mv_pd);
        cprintf("    -> %s\n", (r_typ < 20000u)
                ? "small enough to be an open-drain OUTPUT's pull-up: suspect the wires ARE swapped"
                : "far too weak to be an output's pull-up (those are 4.7-10k, they have to\n"
                  "       make fast edges). This is an INPUT's bias resistor: the pin really\n"
                  "       is PON, and OUT/PON are NOT swapped.");
    }

    /* Leave PON as a driven output again, inactive. */
    REG(PADS_BANK0_PAD(PON_PIN)) = PAD_PON_OUT;
    REG(SIO_GPIO_OE_SET) = PON_MASK;
    pon_drive(false);

verdict:
    /* --- verdict ------------------------------------------------------- */
    cprintf("\nVerdict:\n");

    if (lvl_pu && !lvl_pd) {
        cprintf("  GP%d follows whichever way we pull it, so nothing outside is\n"
                "  driving it: the OUT wire is not reaching GP%d (header pin 32).\n"
                "  Check that joint, and check it is pin 32 and not a neighbour.\n",
                OUT_PIN, OUT_PIN);
    } else if (!lvl_pu && mv_pu >= 200 && mv_pu <= 900) {
        cprintf("  GP%d sits at %u mV against our pull-up -- a diode drop, not a\n"
                "  driven level. That is what an *unpowered* module looks like:\n"
                "  the pull-up is feeding current through its protection diode\n"
                "  into a dead supply rail. Check VDD at the module itself\n"
                "  (3V3(OUT) is header pin 36; VBUS/pin 40 is 5 V and would be\n"
                "  the wrong one) and check the GND joint on pin 38.\n",
                OUT_PIN, mv_pu);
    } else if (!lvl_pu && mv_pu < 200) {
        cprintf("  GP%d is held at %u mV against our pull-up -- a path to ground of\n"
                "  roughly half a kilohm to two. That is exactly what a MICROPOWER\n"
                "  CMOS output driving LOW looks like: a receiver IC with an 85 uA\n"
                "  total budget has a tiny output transistor, and hundreds of ohms to\n"
                "  a couple of kilohms of on-resistance is normal for one. It drives\n"
                "  a high-impedance input, not a load.\n"
                "  (An earlier version of this message called that resistance damning,\n"
                "  on the grounds that an 85 uA part cannot afford a 1k pull-down. The\n"
                "  arithmetic was right and the conclusion was wrong: a pull-down\n"
                "  drains current continuously, an output driver's on-resistance\n"
                "  drains none. A second, different module reading identically is what\n"
                "  showed it up.)\n"
                "  So: the module is powered, connected, and driving its output LOW.\n"
                "  That is a healthy idle state, and it means the module is simply not\n"
                "  detecting the carrier. The question is now reception, not wiring --\n"
                "  see (dcf-listen).\n",
                OUT_PIN, mv_pu);
    } else if (lvl_pu && lvl_pd) {
        cprintf("  GP%d is held HIGH even against our pull-down (%u mV with the\n"
                "  pull-up). Either the module output idles high -- an inverted\n"
                "  type, which is fine and dcf77_probe() would have detected it\n"
                "  from the duty cycle -- or the pin is shorted to 3V3.\n",
                OUT_PIN, mv_pu);
    } else {
        cprintf("  GP%d: pull-up -> %s, pull-down -> %s, %u mV. That combination\n"
                "  does not match any of the clean cases; suspect a partial or\n"
                "  intermittent joint.\n",
                OUT_PIN, lvl_pu ? "HIGH" : "LOW", lvl_pd ? "HIGH" : "LOW", mv_pu);
    }

    if (!pon_hi) {
        cprintf("  GP%d cannot be driven high (%u mV while driving): that pin is\n"
                "  shorted to ground -- pin 34 is beside pin 33 (AGND) too. The\n"
                "  receiver can never have been enabled.\n", PON_PIN, pon_hi_mv);
    } else if (pon_lo) {
        cprintf("  GP%d cannot be driven low: shorted to 3V3.\n", PON_PIN);
    } else {
        cprintf("  GP%d drives cleanly both ways, so PON is at least reaching the\n"
                "  pin. Whether it reaches the module is the joint at pin 34.\n",
                PON_PIN);
    }

    dcf77_power(false);
}

/* ---------------------------------------------------- PON state hunt --- */
/*
 * Second hardware round, 2026-08-22. dcf77_pin_report() found GP27 held at
 * 53 mV against the ~50k internal pull-up -- about 65 uA into roughly 800
 * ohms to ground. That is not a solder bridge (<1 ohm, ~0 mV, and a
 * multimeter confirmed none), not a CMOS output driving low (~50 ohms, ~3
 * mV), and not an unpowered chip's ESD clamp (~500 mV). It looks like a
 * series resistor into a low, or an on-board pull-down.
 *
 * It also exposed a flaw in that diagnostic: it measures OUT with the
 * receiver *disabled*, because dcf77_init() leaves PON inactive and the pull
 * test runs first. "Idle low" was guaranteed by construction, so the reading
 * said nothing about whether the module is alive.
 *
 * This is the test that does say something, and it needs no radio signal at
 * all: sweep PON through its three possible states and watch whether OUT's
 * voltage moves. If it does, the module is powered, PON reaches it, and the
 * only remaining question is reception. If OUT sits at the same voltage in
 * all three, either the module has no supply or the OUT wire is not on its
 * OUT pin -- and no amount of waiting for a signal will change that.
 *
 * The third state is the one the earlier probe never tried: PON left
 * floating. Some modules bias PON internally and expect a switch to ground
 * rather than a push-pull drive, so "high-Z" is a real operating mode, not
 * an absence of one. Floating it also *measures* that internal bias (GP28 is
 * ADC2), which says which way the module expects to be driven.
 *
 * Each window watches the line two ways at once. The digital read is what a
 * decoder would see; the ADC min/max is what is actually there. Those differ
 * exactly when the line is moving but never crossing a logic threshold --
 * a small-signal case a digital-only probe reports as "dead line", which is
 * precisely the failure mode a 1k series resistor into a weak pull-up would
 * produce.
 */

typedef enum { PON_LOW, PON_HIGH, PON_FLOAT } pon_state_t;

static void pon_set_state(pon_state_t st) {
    if (st == PON_FLOAT) {
        /* Input, no pulls of ours: whatever the pad settles at is the
         * module's own bias, which is the point. */
        REG(PADS_BANK0_PAD(PON_PIN)) = PAD_IN_NOPULL;
        REG(SIO_GPIO_OE_CLR) = PON_MASK;
        return;
    }
    REG(PADS_BANK0_PAD(PON_PIN)) = PAD_PON_OUT;
    if (st == PON_HIGH) REG(SIO_GPIO_OUT_SET) = PON_MASK;
    else                REG(SIO_GPIO_OUT_CLR) = PON_MASK;
    REG(SIO_GPIO_OE_SET) = PON_MASK;
}

typedef struct {
    unsigned edges;
    unsigned high_permille;
    unsigned mv_min, mv_max, mv_mean;
} window_stats_t;

/* Per-pin accumulator for watch_pins(). */
typedef struct {
    unsigned pin;
    unsigned samples, high, edges;
    unsigned mv_min, mv_max;
    uint64_t mv_sum;
    bool     cur, cand;
    uint64_t cand_since;
} watch_acc_t;

static void acc_init(watch_acc_t *a, unsigned pin) {
    a->pin = pin;
    a->samples = a->high = a->edges = 0;
    a->mv_min = 0xFFFFFFFFu;
    a->mv_max = 0;
    a->mv_sum = 0;
    a->cur = a->cand = pin_level(pin);
    a->cand_since = time_get_ms();
}

static void acc_step(watch_acc_t *a, uint64_t now) {
    bool lvl = pin_level(a->pin);
    unsigned mv = pin_mv(a->pin);
    a->samples++;
    if (lvl) a->high++;
    if (mv != MV_NA) {
        if (mv < a->mv_min) a->mv_min = mv;
        if (mv > a->mv_max) a->mv_max = mv;
        a->mv_sum += mv;
    }
    if (lvl != a->cand) { a->cand = lvl; a->cand_since = now; }
    else if (a->cand != a->cur && (now - a->cand_since) >= DEBOUNCE_MS) {
        a->cur = a->cand;
        a->edges++;
    }
}

static void acc_finish(const watch_acc_t *a, window_stats_t *out) {
    out->edges = a->edges;
    out->high_permille = a->samples ? (unsigned)((a->high * 1000u) / a->samples) : 0;
    out->mv_min = (a->mv_min == 0xFFFFFFFFu) ? 0 : a->mv_min;
    out->mv_max = a->mv_max;
    out->mv_mean = a->samples ? (unsigned)(a->mv_sum / a->samples) : 0;
}

/* Watch one or two pins for `ms`, digitally and with the ADC in the same
 * loop. Pads stay in digital-input mode (IE=1) throughout: the ADC taps the
 * pad regardless, and giving up a little analog accuracy is worth reading
 * both views of the same instant rather than two different windows.
 *
 * `pin_b` < 0 watches only `pin_a`. Watching both at once exists for the
 * swapped-wires case: if OUT and PON are the other way round, the pulses are
 * on the pin we have been treating as PON, and one pass finds that without
 * anyone rewiring anything. */
static bool watch_pins(unsigned pin_a, int pin_b, uint32_t ms,
                       window_stats_t *out_a, window_stats_t *out_b) {
    REG(PADS_BANK0_PAD(pin_a)) = PAD_IN_PULLUP;
    REG(SIO_GPIO_OE_CLR) = (1u << pin_a);
    if (pin_b >= 0) {
        REG(PADS_BANK0_PAD((unsigned)pin_b)) = PAD_IN_PULLUP;
        REG(SIO_GPIO_OE_CLR) = (1u << (unsigned)pin_b);
    }
    time_delay_us(2000);

    watch_acc_t a, b;
    acc_init(&a, pin_a);
    if (pin_b >= 0) acc_init(&b, (unsigned)pin_b);

    uint64_t end = time_get_ms() + ms;
    uint64_t next_yield = time_get_ms() + 50;

    while (time_get_ms() < end) {
        uint64_t now = time_get_ms();
        acc_step(&a, now);
        if (pin_b >= 0) acc_step(&b, now);

        if (console_interrupt_requested()) { console_interrupt_clear(); return false; }
        if (now >= next_yield) { sched_yield(); next_yield = now + 50; }
        time_delay_us(SAMPLE_US);
    }

    acc_finish(&a, out_a);
    if (pin_b >= 0 && out_b) acc_finish(&b, out_b);
    return true;
}

void dcf77_hunt(unsigned secs_per_state) {
#if !CONFIG_DCF77_PON_PRESENT
    /* This is a PON experiment, and PON is not wired on this board. Refusing
     * beats driving a pin that goes nowhere near the receiver and then
     * reporting whatever OUT happened to do meanwhile. */
    (void)secs_per_state;
    cprintf("\nPON is not wired on this board (CONFIG_DCF77_PON_PRESENT=0), so there\n"
            "is nothing for this test to switch. The receiver is permanently on:\n"
            "use (dcf-raw), (dcf-listen) or (dcf-sync) instead.\n");
    return;
#endif

    if (secs_per_state < 5)  secs_per_state = 5;
    if (secs_per_state > 120) secs_per_state = 120;

    dcf77_init();
    console_interrupt_clear();

    /* Deliberately longer than dcf77_probe()'s 5 s. Datasheets for this
     * class of receiver quote warm-up anywhere from ~1 s to ~30 s, and the
     * first probe's 5 s may simply have been too short to be evidence of
     * anything. */
    const uint32_t warm_ms = 15000;

    cprintf("\nDCF-77 PON hunt: three PON states, %u s of watching each,\n"
            "after %u s of warm-up each. Total about %u s. Ctrl-C aborts.\n\n",
            secs_per_state, (unsigned)(warm_ms / 1000),
            (unsigned)(3 * (secs_per_state + warm_ms / 1000)));

    static const char *const names[3] = { "driven LOW", "driven HIGH", "FLOATING" };
    const pon_state_t states[3] = { PON_LOW, PON_HIGH, PON_FLOAT };
    window_stats_t st[3];
    window_stats_t pon_win = {0, 0, 0, 0, 0};
    bool watched_pon = false;
    bool ok = true;

    for (int i = 0; i < 3 && ok; i++) {
        cprintf("State %d/3: PON %s\n", i + 1, names[i]);
        pon_set_state(states[i]);
        time_delay_us(2000);

        if (states[i] == PON_FLOAT) {
            unsigned pon_mv = pin_mv(PON_PIN);
            cprintf("  PON floats to %u mV -- %s\n", pon_mv,
                    pon_mv > 2000 ? "the module pulls PON up internally (it expects a switch to ground)"
                  : pon_mv < 500  ? "the module pulls PON down internally (it expects a switch to 3V3)"
                                  : "no clear internal bias, or nothing is connected to pin 34");
        }

        cprintf("  warming up (%u s)...\n", (unsigned)(warm_ms / 1000));
        if (!wait_abortable(warm_ms)) { ok = false; break; }

        /* In the floating state neither pin is driven by us, so both can be
         * watched safely -- which is the swapped-wires check. In the two
         * driven states PON is an output and must not be pulled up and read
         * as if it were an input. */
        if (states[i] == PON_FLOAT) {
            if (!watch_pins(OUT_PIN, (int)PON_PIN, secs_per_state * 1000u,
                            &st[i], &pon_win)) { ok = false; break; }
            watched_pon = true;
        } else {
            if (!watch_pins(OUT_PIN, -1, secs_per_state * 1000u, &st[i], NULL)) {
                ok = false; break;
            }
        }

        cprintf("  OUT over %u s: edges=%u  high=%u.%u%%  V min=%u max=%u mean=%u mV\n",
                secs_per_state, st[i].edges,
                st[i].high_permille / 10, st[i].high_permille % 10,
                st[i].mv_min, st[i].mv_max, st[i].mv_mean);
        if (states[i] == PON_FLOAT) {
            cprintf("  PON pin, same window (swap check): edges=%u  high=%u.%u%%  "
                    "V min=%u max=%u mean=%u mV\n",
                    pon_win.edges, pon_win.high_permille / 10, pon_win.high_permille % 10,
                    pon_win.mv_min, pon_win.mv_max, pon_win.mv_mean);
        }
        cprintf("\n");
    }

    pon_set_state(PON_LOW);
    dcf77_power(false);

    if (!ok) { cprintf("Hunt aborted.\n"); return; }

    /* --- what the three windows say together --------------------------- */
    cprintf("Comparison:\n");
    cprintf("  OUT mean : LOW=%u mV  HIGH=%u mV  FLOAT=%u mV\n",
            st[0].mv_mean, st[1].mv_mean, st[2].mv_mean);
    cprintf("  OUT edges: LOW=%u     HIGH=%u     FLOAT=%u\n",
            st[0].edges, st[1].edges, st[2].edges);

    unsigned lo = st[0].mv_mean, hi = st[0].mv_mean;
    for (int i = 1; i < 3; i++) {
        if (st[i].mv_mean < lo) lo = st[i].mv_mean;
        if (st[i].mv_mean > hi) hi = st[i].mv_mean;
    }
    unsigned swing = hi - lo;

    unsigned widest = 0;
    for (int i = 0; i < 3; i++) {
        unsigned w = st[i].mv_max - st[i].mv_min;
        if (w > widest) widest = w;
    }

    cprintf("\nVerdict:\n");
    if (watched_pon && pon_win.edges >= 2 && !st[2].edges) {
        cprintf("  Pulses are on GP%d -- the pin wired as PON -- and not on GP%d.\n"
                "  OUT and PON are swapped at one end or the other. Swap them (or\n"
                "  swap CONFIG_DCF77_OUT_GPIO and CONFIG_DCF77_PON_GPIO in\n"
                "  cmake/board-rp2350-clock.cmake) and re-run (dcf-raw 120).\n",
                (unsigned)PON_PIN, OUT_PIN);
    } else if (st[0].edges || st[1].edges || st[2].edges) {
        cprintf("  There are real digital edges in at least one state -- the module\n"
                "  is alive. Re-run (dcf-raw 120) with PON in whichever state above\n"
                "  produced them.\n");
    } else if (swing > 30) {
        cprintf("  No edges, but OUT's level *changes with PON* (%u mV between\n"
                "  states). That means the module is powered and PON reaches it:\n"
                "  the wiring is good and the problem is reception -- antenna\n"
                "  orientation, position, or simply this room.\n", swing);
    } else if (widest > 50) {
        cprintf("  No digital edges, but OUT moves by up to %u mV within a window\n"
                "  without ever crossing a logic threshold. Something is modulating\n"
                "  the line too weakly to read as a digital signal -- consistent with\n"
                "  an open-collector output into our ~50k pull-up being too weak.\n"
                "  Worth trying an external 4.7k-10k pull-up from GP%d to 3V3.\n",
                widest, OUT_PIN);
    } else {
        cprintf("  OUT is the same voltage in all three PON states (%u mV, within\n"
                "  %u mV) and never moves. The module is not responding to PON at\n"
                "  all, which leaves two possibilities that software cannot separate:\n"
                "    - it has no supply: measure VDD *at the module's own pad*,\n"
                "      against its own GND pad, with the board powered;\n"
                "    - the OUT wire is not on the module's OUT pin.\n"
                "  The ~%u mV against our pull-up says the wire does reach something\n"
                "  resistive (roughly 1k to ground), so it is not an open circuit.\n",
                st[0].mv_mean, swing, st[0].mv_mean);
    }
}

/* ------------------------------------------------- free-pin comparison -- */
/*
 * Answers a question the numbers above cannot: is the ~1k to ground on GP27
 * the *module*, or is it the baseboard? GP27/GP28 were chosen as free from
 * the vendor firmware's pin list, which only records the pins that firmware
 * *uses* -- it is not proof the baseboard leaves them unconnected.
 *
 * So compare against the persona's other unused pins. Input-only, nothing is
 * ever driven; each pad is saved and restored, so pins this OS does not
 * otherwise touch are left exactly as they were. A pin that follows whichever
 * way it is pulled has nothing on it; one that ignores both pulls has
 * something on it. If GP27 is the only one that ignores them, the load
 * arrived with the DCF-77 module. If several do, it is the board.
 *
 * GP9 is excluded on purpose: the heartbeat task drives it.
 */
void dcf77_free_pin_survey(void) {
    static const struct { uint8_t gpio; uint8_t header; const char *note; } pins[] = {
        {  4,  6, "" }, {  5,  7, "" }, {  8, 11, "" },
        { 19, 25, "" }, { 20, 26, "" }, { 21, 27, "" },
        { (uint8_t)OUT_PIN, 32, "  <- DCF-77 OUT" },
    };

    cprintf("\nFree-pin survey (input only, nothing driven; pads restored after).\n"
            "A pin with nothing on it follows whichever way it is pulled.\n\n");

    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        unsigned pin = pins[i].gpio;
        uint32_t saved_pad  = REG(PADS_BANK0_PAD(pin));
        uint32_t saved_ctrl = REG(IO_BANK0_CTRL(pin));
        bool     saved_oe   = (REG(SIO_GPIO_OE) & (1u << pin)) != 0;

        REG(IO_BANK0_CTRL(pin)) = 5;
        REG(SIO_GPIO_OE_CLR) = (1u << pin);

        REG(PADS_BANK0_PAD(pin)) = PAD_IN_PULLUP;
        time_delay_us(2000);
        bool up = pin_level(pin);

        bool down = read_level_pulled_down(pin); /* RP2350-E9 aware */

        const char *verdict = (up && !down) ? "floats (nothing on it)"
                            : (!up && !down) ? "HELD LOW  (something is on it)"
                            : (up && down)   ? "reads high even pulled down"
                                             : "inverted?? (suspect a bad joint)";
        cprintf("  GP%2d (header pin %2d): %s%s\n",
                pin, pins[i].header, verdict, pins[i].note);

        REG(PADS_BANK0_PAD(pin)) = saved_pad;
        REG(IO_BANK0_CTRL(pin)) = saved_ctrl;
        if (saved_oe) REG(SIO_GPIO_OE_SET) = (1u << pin);
        else          REG(SIO_GPIO_OE_CLR) = (1u << pin);
    }

    cprintf("\nA pin held LOW against the pull-up is a solid result -- RP2350-E9's\n"
            "leakage only ever pushes a pad UP, so it cannot manufacture a low.\n"
            "A pin that still reads high with the pull-down applied is weaker\n"
            "evidence even with the erratum workaround in place; treat it as\n"
            "'not obviously loaded' rather than proof of anything.\n"
            "If GP%d is the only one held low, that load came with the module.\n",
            OUT_PIN);
}

/* ------------------------------------------------------------ mirror --- */
/*
 * "Shouldn't we be able to connect an LED to OUT to see some flashing?"
 * (user, 2026-08-22). Not to *this* OUT: the output stage measured here has
 * roughly a kilohm of on-resistance and the part has an 85 uA budget, while
 * an LED wants milliamps. It would barely glow and would load the very
 * output we are trying to observe. (A high-efficiency LED behind 10k draws
 * ~0.3 mA and is visible in a dim room -- that variant is safe. A 330 ohm
 * one is not.)
 *
 * The instinct is right though: seeing it beats reading about it. So mirror
 * the line onto hardware that IS built to be driven -- the LED matrix. One
 * row lights whenever OUT is high, using the same static, unmultiplexed
 * lighting as the quiet-sync indicator, so the mirror itself adds no
 * switching noise for the receiver to hear.
 *
 * Note the side effect, since it confounds one experiment with another: a lit
 * row is also tens of milliamps of extra 3V3 load, which is exactly M0's
 * variable (plan/phase17_clock_ui_and_dcf77.md section 3). While OUT is low
 * the row is dark and the load is not there, so this is not a clean M0 test
 * -- (dcf-listen secs 1) is.
 */
void dcf77_mirror(unsigned secs) {
    if (secs < 5)   secs = 5;
    if (secs > 900) secs = 900;

    dcf77_init();
    console_interrupt_clear();
    dcf77_power(true);

    REG(PADS_BANK0_PAD(OUT_PIN)) = PAD_IN_PULLUP;
    REG(SIO_GPIO_OE_CLR) = OUT_MASK;

#if CONFIG_ENABLE_PICO_CLOCK_GREEN
    cprintf("\nMirroring GP%d onto the LED matrix for %u s: one row lights while\n"
            "OUT is high. Watch the display rather than this console. Ctrl-C stops.\n",
            OUT_PIN, secs);
#else
    cprintf("\nNo LED matrix in this build; reporting transitions on the console\n"
            "only, for %u s.\n", secs);
#endif

    cprintf("The row lights on %s, the configured pulse level.\n",
            g_out_pulse_high ? "HIGH" : "LOW");

    uint64_t end = time_get_ms() + (uint64_t)secs * 1000u;
    uint64_t next_yield = time_get_ms() + 50;
    bool shown = false;
    unsigned transitions = 0;

    while (time_get_ms() < end) {
        bool lvl = pulse_active(); /* polarity-corrected: lit == pulse */
        if (lvl != shown) {
            shown = lvl;
            transitions++;
#if CONFIG_ENABLE_PICO_CLOCK_GREEN
            pico_clock_green_static_load(lvl);
#endif
        }
        uint64_t now = time_get_ms();
        if (console_interrupt_requested()) { console_interrupt_clear(); break; }
        if (now >= next_yield) { sched_yield(); next_yield = now + 50; }
        time_delay_us(SAMPLE_US);
    }

#if CONFIG_ENABLE_PICO_CLOCK_GREEN
    pico_clock_green_static_load(false);
#endif
    dcf77_power(false);
    cprintf("\n%u transitions in %u s. %s\n", transitions, secs,
            transitions ? "The line moves -- run (dcf-listen 300) to see if it is DCF-77 shaped."
                        : "The line never moved once. Nothing to see, and no LED would have\n"
                          "shown anything either -- the module is not producing an output.");
}

/* ---------------------------------------------------- power-on capture -- */
/*
 * A hole in this file's own test design, found by re-reading what an RC8000
 * is documented to do: on being enabled it drives OUT **HIGH for a few
 * seconds**, and only then starts pulsing.
 *
 * dcf77_hunt() asserts PON and then waits out a 15 s warm-up *before* it
 * starts watching. If the module does exactly what it is supposed to do, the
 * entire transient is over before the first sample is taken. Every "OUT never
 * moved" result so far is therefore blind to the one event that would prove
 * the module reacts to PON at all.
 *
 * So: de-assert PON, wait long enough for the module to actually power down,
 * then assert it and sample from the same instant -- no warm-up, no settling,
 * nothing skipped. Printed as a trace at 100 ms per character, which shows a
 * multi-second startup high as an unmistakable run of '#' and, later, DCF-77
 * pulses as one marked cell in every ten.
 *
 * Requires PON on its own GPIO. With PON strapped to ground at the module
 * there is nothing to toggle, and this says so rather than reporting a flat
 * line as evidence of anything.
 */
#define PWRON_CELL_MS   100u
#define PWRON_COLS       50u   /* 50 x 100 ms = 5 s per line */

/* One capture: park PON at the "disabled" level for 3 s, flip it to the
 * "enabled" level, and sample from that instant. `enable_high` says which
 * level this phase treats as enabled. Returns false if interrupted. */
static bool capture_phase(bool enable_high, unsigned secs,
                          unsigned *first_high_ms, unsigned *longest_high_ms) {
    g_pon_active_low = !enable_high;

    cprintf("  PON: %s = enabled. Holding it disabled for 3 s...\n",
            enable_high ? "HIGH" : "LOW");
    dcf77_power(false);
    if (!wait_abortable(3000)) return false;
    dcf77_power(true);

    uint64_t t0 = time_get_ms();
    uint64_t end = t0 + (uint64_t)secs * 1000u;
    uint64_t first_high = 0;
    unsigned longest = 0, run = 0;
    char row[PWRON_COLS + 1];
    unsigned col = 0, line_start_s = 0;

    while (time_get_ms() < end) {
        /* Sample the whole 100 ms cell rather than point-sampling it, so a
         * 100 ms pulse can never fall between two looks. */
        unsigned high_samples = 0, samples = 0;
        uint64_t cell_end = time_get_ms() + PWRON_CELL_MS;
        while (time_get_ms() < cell_end) {
            samples++;
            if (pin_level(OUT_PIN)) {
                high_samples++;
                if (!first_high) first_high = time_get_ms() - t0;
                if (++run > longest) longest = run;
            } else {
                run = 0;
            }
            time_delay_us(SAMPLE_US);
        }

        row[col++] = (high_samples == 0) ? '.' : (high_samples >= samples) ? '#' : '+';
        if (col == PWRON_COLS) {
            row[col] = '\0';
            cprintf("    t=%3u s |%s|\n", line_start_s, row);
            col = 0;
            line_start_s += (PWRON_COLS * PWRON_CELL_MS) / 1000u;
            sched_yield();
        }
        if (console_interrupt_requested()) { console_interrupt_clear(); return false; }
    }
    if (col) {
        row[col] = '\0';
        cprintf("    t=%3u s |%s|\n", line_start_s, row);
    }

    *first_high_ms = (unsigned)first_high;
    *longest_high_ms = longest;
    return true;
}

void dcf77_power_on_capture(unsigned secs) {
#if !CONFIG_DCF77_PON_PRESENT
    /* This is a PON experiment, and PON is not wired on this board. Refusing
     * beats driving a pin that goes nowhere near the receiver and then
     * reporting whatever OUT happened to do meanwhile. */
    (void)secs;
    cprintf("\nPON is not wired on this board (CONFIG_DCF77_PON_PRESENT=0), so there\n"
            "is nothing for this test to switch. The receiver is permanently on:\n"
            "use (dcf-raw), (dcf-listen) or (dcf-sync) instead.\n");
    return;
#endif

    if (secs < 5)   secs = 5;
    if (secs > 120) secs = 120;

    dcf77_init();
    console_interrupt_clear();

    REG(PADS_BANK0_PAD(OUT_PIN)) = PAD_IN_PULLUP;
    REG(SIO_GPIO_OE_CLR) = OUT_MASK;

    bool saved_polarity = g_pon_active_low;

    cprintf("\nPower-on transient on GP%d, 100 ms per character.\n"
            "'#' = high for the whole cell, '+' = part of it, '.' = low.\n"
            "A receiver of this type drives OUT HIGH for a few seconds when it is\n"
            "enabled, before any pulses -- a reaction that needs no radio signal.\n\n",
            OUT_PIN);

    /* Informational only, deliberately not a gate: a PON input that biases
     * itself reads high when floated (3084 mV on the first module tried
     * here), but an input with no bias at all is perfectly legal and would
     * read the same as an unconnected pad. This says what it saw and leaves
     * the conclusion to the two captures below, which do not depend on it. */
    if (PIN_IS_ADC(PON_PIN)) {
        REG(PADS_BANK0_PAD(PON_PIN)) = PAD_ADC_NOPULL;
        REG(SIO_GPIO_OE_CLR) = PON_MASK;
        time_delay_us(5000);
        unsigned float_mv = pin_mv(PON_PIN);
        cprintf("PON pin floats to %u mV (%s).\n\n", float_mv,
                float_mv > 2500 ? "a module is biasing it up"
                                : "no bias -- either unconnected, or an input that simply has none");
        REG(PADS_BANK0_PAD(PON_PIN)) = PAD_PON_OUT;
        REG(SIO_GPIO_OE_SET) = PON_MASK;
    }

    /* Both polarities, because nothing in the evidence so far has excluded
     * either. The previous version tested only active-low, which is right for
     * an RC8000 and merely assumed for anything else -- and if a module
     * enables on PON HIGH, that run had it exactly backwards: the 3 s
     * "power down" was enabling it and the whole sample window had it off. */
    unsigned first_a = 0, longest_a = 0, first_b = 0, longest_b = 0;

    cprintf("Phase 1 of 2 -- assuming PON is active LOW:\n");
    if (!capture_phase(false, secs, &first_a, &longest_a)) goto done;

    cprintf("\nPhase 2 of 2 -- assuming PON is active HIGH:\n");
    if (!capture_phase(true, secs, &first_b, &longest_b)) goto done;

    cprintf("\nVerdict:\n");
    if (longest_a > 1000 && longest_b <= 1000) {
        cprintf("  Startup transient with PON LOW (first high at %u ms, %u ms long).\n"
                "  The module is alive, responds to PON, and PON is active LOW as the\n"
                "  board file says. Everything left is reception: (dcf-listen 300).\n",
                first_a, longest_a);
    } else if (longest_b > 1000 && longest_a <= 1000) {
        cprintf("  Startup transient with PON HIGH (first high at %u ms, %u ms long),\n"
                "  and nothing with PON low. This module enables on PON HIGH: set\n"
                "  CONFIG_DCF77_PON_ACTIVE_LOW to 0 in cmake/board-rp2350-clock.cmake.\n"
                "  The module is alive and has simply been switched off until now.\n",
                first_b, longest_b);
    } else if (longest_a > 1000 && longest_b > 1000) {
        cprintf("  A long high in both phases, so OUT is not reacting to PON. Two\n"
                "  innocent explanations before any guilty one:\n"
                "    - the module has no PON pin at all (a 3-wire GND/VCC/SIG part is\n"
                "      always enabled), in which case this test simply does not apply\n"
                "      and the trace above is just the idle line; or\n"
                "    - the module's output idles HIGH -- an inverted type, where the\n"
                "      pulses go low. Run (dcf-listen 300), which measures which level\n"
                "      is the pulse before it measures anything else.\n"
                "  Only if neither fits: OUT and PON shorted, or OUT on the PON pad.\n");
    } else if (first_a || first_b) {
        cprintf("  Short highs only (%u ms / %u ms). Not the startup transient, but the\n"
                "  line does move -- run (dcf-listen 300) to see if it is DCF-77 shaped.\n",
                longest_a, longest_b);
    } else {
        cprintf("  OUT never went high in either polarity, with PON on GP%d and driven\n"
                "  both ways. Combined with a pin proven good by (dcf-drivetest) and\n"
                "  3.3 V measured at the module's own pads, that is as far as this\n"
                "  board can take it: the module is not producing an output at all.\n"
                "  Two modules behaving identically makes a pair of dead modules\n"
                "  unlikely, so the remaining suspects are the supply's switching\n"
                "  noise (M0 -- unreachable in software on a Pico 2 W) and the\n"
                "  antenna/location. The battery test settles it: module on cells,\n"
                "  well away from this board, meter on OUT in DC volts. A receiver\n"
                "  producing pulses averages ~0.3-0.6 V; a flat 0.00 V convicts it.\n",
                PON_PIN);
    }

done:
    g_pon_active_low = saved_polarity;
    dcf77_power(false);
}

/* ------------------------------------------------------- drive test ---- */
/*
 * "Is the pin itself dead?" -- the one question every passive test above is
 * structurally unable to answer, because a stuck-low pad and a healthy pad
 * looking at a device driving low are indistinguishable when you only ever
 * look.
 *
 * It became the live question when a *second*, different module produced
 * byte-for-byte the same reading as the first. That was taken as evidence the
 * modules were fine -- but the common element across both experiments is the
 * Pico pin, not the module, and "GP27 is stuck low" explains the repeat just
 * as economically. The user spotted it.
 *
 * So drive the pin and read it back. What the result means:
 *   ~3.3 V   the pad drives, and nothing outside is holding it -- the pin is
 *            healthy and, if the module is connected, its output is high-Z
 *   ~1-2 V   the pad drives against roughly the ~1 kohm measured earlier: a
 *            healthy pin loaded by a module output driving low
 *   ~0 V     the pad cannot drive high at all: either the pad is damaged or
 *            something outside is a hard short
 *
 * Safety, because this deliberately breaks the "never drive OUT" rule the
 * pin report follows: the drive strength is set to 2 mA, the weakest the
 * RP2350 offers, and the measured external path is 0.5-1.4 kohm, so the
 * contention current is about 3 mA -- inside the RP2350's 12 mA per-pin
 * limit and inside the few-mA a CMOS output stage tolerates. It is held for
 * 50 ms, not indefinitely. Running it with the module unplugged is still the
 * cleaner experiment, and the printed result says which case it saw.
 */
void dcf77_drive_test(void) {

    dcf77_init();

    cprintf("\nDrive test on GP%d (2 mA, 50 ms). This is the only test here that\n"
            "drives the OUT pin; see the header comment for the current budget.\n",
            OUT_PIN);

    uint32_t saved_pad = REG(PADS_BANK0_PAD(OUT_PIN));

    /* 2 mA, Schmitt on, input buffer on so the pad can be read back. */
    REG(PADS_BANK0_PAD(OUT_PIN)) = 0x42;
    REG(SIO_GPIO_OUT_SET) = OUT_MASK;
    REG(SIO_GPIO_OE_SET)  = OUT_MASK;
    time_delay_us(50000);
    bool hi_lvl = pin_level(OUT_PIN);
    unsigned hi_mv = pin_mv(OUT_PIN);

    REG(SIO_GPIO_OUT_CLR) = OUT_MASK;
    time_delay_us(50000);
    bool lo_lvl = pin_level(OUT_PIN);
    unsigned lo_mv = pin_mv(OUT_PIN);

    /* Back to a safe input before anything else happens. */
    REG(SIO_GPIO_OE_CLR) = OUT_MASK;
    REG(PADS_BANK0_PAD(OUT_PIN)) = saved_pad;

    cprintf("  driven HIGH -> reads %s (", hi_lvl ? "HIGH" : "LOW "); print_mv(hi_mv); cprintf(")\n");
    cprintf("  driven LOW  -> reads %s (", lo_lvl ? "HIGH" : "LOW "); print_mv(lo_mv); cprintf(")\n");

    cprintf("\nVerdict:\n");
    if (hi_mv == MV_NA) {
        cprintf("  %s\n", hi_lvl
                ? "  The pad drives high and reads high: GP-pin is healthy."
                : "  The pad cannot drive high: suspect the pad or a hard short.");
    } else if (hi_mv > 2800) {
        cprintf("  GP%d drives to %u mV. The pad is healthy and nothing outside is\n"
                "  holding it down *while it is driven*. If the module is still\n"
                "  connected, its output is high-impedance rather than driving low --\n"
                "  so the ~1 kohm seen passively was its weak output stage, not a\n"
                "  fault, and not this pin.\n", OUT_PIN, hi_mv);
    } else if (hi_mv > 700) {
        cprintf("  GP%d only reaches %u mV while driving at 2 mA. The pad works; it is\n"
                "  fighting an external load of roughly the size measured passively.\n"
                "  That is a module output driving low -- healthy on both sides, and\n"
                "  it rules out a dead pin.\n", OUT_PIN, hi_mv);
    } else {
        cprintf("  GP%d stays at %u mV even while being driven high at 2 mA. That is a\n"
                "  hard short or a damaged pad. Unplug the module's OUT wire and run\n"
                "  this again: if it still cannot drive high with nothing attached,\n"
                "  the pin is dead and the board file should simply name another one\n"
                "  (CONFIG_DCF77_OUT_GPIO -- GP28 keeps the ADC diagnostics, since\n"
                "  only GP26-29 can be measured; GP21 or GP19 work digitally).\n",
                OUT_PIN, hi_mv);
    }

    dcf77_power(false);
}

/* --------------------------------------------------------- listen ------ */
/*
 * The tool for the phase this turned into: the wiring is proven, the module
 * drives its output low as a healthy micropower CMOS stage does, and nothing
 * ever pulses. That is a *reception* problem, and reception problems are
 * solved by moving an antenna around while watching a number change.
 *
 * So: assert PON, wait, then watch for minutes rather than seconds, printing
 * every pulse as it arrives and a progress line every 10 s. Leave it running
 * and move the ferrite rod.
 *
 * `with_load` lights one row of the LED matrix as a pure DC load
 * (pico_clock_green_static_load(), no multiplexing, no switching edges).
 * That is the M0 experiment from plan/phase17_clock_ui_and_dcf77.md section
 * 3: the Pico's buck regulator idles in PFM/power-save at light load, and
 * its variable-frequency ripple is a documented killer for exactly this
 * module. On a Pico 2 W the regulator's PWM-mode pin is behind the CYW43
 * wireless chip and unreachable from here, so raising the load is the only
 * lever this side of new hardware. If pulses appear only with the load on,
 * that is the answer -- and it inverts this phase's founding assumption that
 * the display is the enemy.
 */
void dcf77_listen(unsigned secs, bool with_load) {
    if (secs < 10)   secs = 10;
    if (secs > 1800) secs = 1800;

    dcf77_init();
    console_interrupt_clear();
    dcf77_power(true); /* harmless if PON is strapped to GND at the module */

#if CONFIG_ENABLE_PICO_CLOCK_GREEN
    if (with_load) {
        pico_clock_green_static_load(true);
        cprintf("One matrix row lit as a DC load (no multiplexing, no switching).\n");
    }
#else
    (void)with_load;
#endif

    cprintf("Listening on GP%d for %u s. Move the ferrite rod while this runs:\n"
            "horizontal, broadside to the transmitter, as far from the board and\n"
            "from any switching supply as the leads allow. Ctrl-C stops.\n\n",
            OUT_PIN, secs);

    if (!wait_abortable(10000)) goto done;

    REG(PADS_BANK0_PAD(OUT_PIN)) = PAD_IN_PULLUP;
    REG(SIO_GPIO_OE_CLR) = OUT_MASK;

    /* Work out which level is the pulse before measuring any widths. Only
     * dcf77_probe() used to do this; listen() and mirror() took the default
     * "pulses are high" on faith, which is wrong for an inverted module and
     * would have reported every *gap* as a pulse -- 800 ms "pulses" and a
     * confident, meaningless verdict. A DCF-77 pulse train is 10-20% active
     * by construction, so the minority level is the pulse. */
    {
        unsigned edges = 0, high_permille = 0;
        if (!survey(SURVEY_MS, &edges, &high_permille)) goto done;
        cprintf("Configured: pulses are %s (CONFIG_DCF77_OUT_ACTIVE_LOW=%d).\n"
                "Measured: line is high %u.%u%% of the time.\n",
                g_out_pulse_high ? "HIGH" : "LOW", CONFIG_DCF77_OUT_ACTIVE_LOW,
                high_permille / 10, high_permille % 10);
        /* Warn, never override. A pulse train is 10-20% active, so a duty
         * cycle on the wrong side of half is a real disagreement -- but with
         * a weak signal it can also just be noise, which is exactly why this
         * no longer gets to change the setting. */
        if ((g_out_pulse_high && high_permille > 600u) ||
            (!g_out_pulse_high && high_permille < 400u)) {
            cprintf("  >> These disagree. Check the module's polarity jumper against\n"
                    "  >> CONFIG_DCF77_OUT_ACTIVE_LOW in cmake/board-rp2350-clock.cmake.\n");
        }
        cprintf("\n");
    }

    uint64_t t0 = time_get_ms();
    uint64_t end = t0 + (uint64_t)secs * 1000u;
    uint64_t next_report = t0 + 10000;
    uint64_t next_yield = t0 + 50;

    bool cur = pulse_active(), cand = cur;
    uint64_t cand_since = t0, pulse_start = 0, prev_start = 0;
    bool have_pulse = false, have_prev = false;
    unsigned n_pulses = 0, n_plausible = 0;
    unsigned mv_min = 0xFFFFFFFFu, mv_max = 0;

    while (time_get_ms() < end) {
        bool act = pulse_active();
        unsigned mv = pin_mv(OUT_PIN);
        uint64_t now = time_get_ms();

        if (mv != MV_NA) {
            if (mv < mv_min) mv_min = mv;
            if (mv > mv_max) mv_max = mv;
        }

        if (act != cand) {
            cand = act;
            cand_since = now;
        } else if (cand != cur && (now - cand_since) >= DEBOUNCE_MS) {
            uint64_t at = cand_since;
            cur = cand;
            if (cur) {
                pulse_start = at;
                have_pulse = true;
            } else if (have_pulse) {
                uint32_t width = (uint32_t)(at - pulse_start);
                uint32_t gap = have_prev ? (uint32_t)(pulse_start - prev_start) : 0;
                n_pulses++;
                if (width >= BIT0_MIN_MS && width <= BIT1_MAX_MS) n_plausible++;
                uint64_t rel = pulse_start - t0;
                cprintf("  t=%3u.%03u  width=%4u ms  gap=%4u ms\n",
                        (unsigned)(rel / 1000), (unsigned)(rel % 1000),
                        (unsigned)width, (unsigned)gap);
                prev_start = pulse_start;
                have_prev = true;
            }
        }

        if (now >= next_report) {
            uint64_t rel = now - t0;
            cprintf("  [%3u s] pulses=%u (%u plausible)  V %u-%u mV\n",
                    (unsigned)(rel / 1000), n_pulses, n_plausible,
                    (mv_min == 0xFFFFFFFFu) ? 0 : mv_min, mv_max);
            mv_min = 0xFFFFFFFFu;
            mv_max = 0;
            next_report = now + 10000;
        }

        if (console_interrupt_requested()) { console_interrupt_clear(); break; }
        if (now >= next_yield) { sched_yield(); next_yield = now + 50; }
        time_delay_us(SAMPLE_US);
    }

    cprintf("\n%u pulses, %u of them a plausible DCF-77 width.\n", n_pulses, n_plausible);
    if (n_plausible >= 5) {
        cprintf("That is a signal. Run (dcf-raw 120) to see whether it decodes.\n");
    } else if (n_pulses) {
        cprintf("Edges, but not DCF-77-shaped ones -- noise rather than carrier.\n");
    } else {
        cprintf("Nothing at all. Next things worth trying, in order of how much\n"
                "they usually matter:\n"
                "  1. Power the module from a battery, well away from this board,\n"
                "     and watch OUT with a meter. That removes the Pico's switching\n"
                "     regulator from the experiment entirely, and on a Pico 2 W its\n"
                "     PWM-mode pin is behind the wireless chip where we cannot reach\n"
                "     it. If it pulses on a battery and not here, the supply is the\n"
                "     problem and the fix is hardware: filter the module's VDD, or\n"
                "     give it its own.\n"
                "  2. (dcf-listen 300 1) -- with the DC load on. More current can\n"
                "     push the regulator out of power-save mode into clean PWM.\n"
                "  3. Antenna orientation: horizontal, broadside to Mainflingen,\n"
                "     away from mains wiring, monitors and chargers. A ferrite rod\n"
                "     is directional and this is worth more than any code.\n"
                "  4. Try at night. Longwave propagation is genuinely better, and a\n"
                "     marginal indoor location can work at 02:00 and not at 19:00.\n");
    }

done:
#if CONFIG_ENABLE_PICO_CLOCK_GREEN
    if (with_load) pico_clock_green_static_load(false);
#endif
    dcf77_power(false);
}

/* ------------------------------------------------------------- probe --- */




/* A crude but very readable scope trace: one character per 25 ms, one line
 * per second. When nothing decodes, this is the picture that says why --
 * a clean signal is a short run of '#' at the same place on every line, a
 * dead one is all '.', and noise is confetti. */
static bool raw_trace(void) {
    cprintf("Raw OUT trace ('#' = pulse active, '.' = idle, 25 ms/char, 1 s/line):\n");
    for (unsigned line = 0; line < TRACE_LINES; line++) {
        char row[TRACE_COLS + 1];
        for (unsigned c = 0; c < TRACE_COLS; c++) {
            /* Sample across the whole 25 ms cell and call the cell active if
             * any sample in it was: a 100 ms pulse must never vanish between
             * two point samples. */
            bool any = false;
            uint64_t cell_end = time_get_ms() + TRACE_STEP_MS;
            while (time_get_ms() < cell_end) {
                if (pulse_active()) any = true;
                time_delay_us(SAMPLE_US);
            }
            row[c] = any ? '#' : '.';
            if (console_interrupt_requested()) { console_interrupt_clear(); return false; }
        }
        row[TRACE_COLS] = '\0';
        cprintf("  |%s|\n", row);
        sched_yield();
    }
    return true;
}

void dcf77_probe(unsigned secs) {
    if (secs == 0)   secs = 30;
    if (secs > 600)  secs = 600;

    dcf77_init();
    console_interrupt_clear();

#if CONFIG_DCF77_PON_PRESENT
    cprintf("\nDCF-77 probe: OUT=GP%d, PON=GP%d, board says PON active %s.\n",
            OUT_PIN, PON_PIN, (CONFIG_DCF77_PON_ACTIVE_LOW != 0) ? "LOW" : "HIGH");
#else
    cprintf("\nDCF-77 probe: OUT=GP%d, PON not wired (receiver permanently on).\n",
            OUT_PIN);
#endif
    cprintf("Ctrl-C aborts. The display is not scanning while you are in the shell,\n"
            "so this reading is the quiet-reference one (phase17 D5 step 1).\n\n");

    /* --- 1. PON polarity: try the board file's belief, then the other --- */
    unsigned edges = 0, high_permille = 0;
    bool alive = false;
    /* Only one attempt is possible with no PON to switch: the "other
     * polarity" run would drive nothing and re-measure the same pin. */
    const int pon_attempts = CONFIG_DCF77_PON_PRESENT ? 2 : 1;
    for (int attempt = 0; attempt < pon_attempts && !alive; attempt++) {
        g_pon_active_low = (attempt == 0) ? (CONFIG_DCF77_PON_ACTIVE_LOW != 0)
                                          : !(CONFIG_DCF77_PON_ACTIVE_LOW != 0);
        dcf77_power(true);
        {
            uint64_t warm = warmup_remaining_ms();
#if CONFIG_DCF77_PON_PRESENT
            cprintf("PON driven %s (receiver on); waiting %u ms for the AGC to settle...\n",
                    g_pon_active_low ? "LOW" : "HIGH", (unsigned)warm);
#else
            if (warm) cprintf("Receiver already on; %u ms of warm-up left.\n", (unsigned)warm);
#endif
            if (warm && !wait_abortable(warm)) goto aborted;
        }

        if (!survey(SURVEY_MS, &edges, &high_permille)) goto aborted;
        cprintf("  %u ms survey: %u edges, pin high %u.%u%% of the time.\n",
                (unsigned)SURVEY_MS, edges, high_permille / 10, high_permille % 10);
        if (edges >= 2) { alive = true; break; }
        dcf77_power(false);
        if (attempt == 0 && pon_attempts > 1)
            cprintf("  Nothing moved. Trying the opposite PON polarity.\n");
    }

    if (!alive) {
        cprintf("\nNo activity on OUT with either PON polarity.\n"
                "  - pin high %u.%u%% suggests the line is stuck %s\n"
                "  - check: module VDD (3V3, pin 36) and GND (pin 38), OUT on GP%d,\n"
                "    the antenna plugged in, and that you are not in a steel-framed\n"
                "    room or next to a switching supply. A DCF-77 module with power\n"
                "    and an antenna ticks once a second even with a poor signal.\n",
                high_permille / 10, high_permille % 10,
                high_permille > 500 ? "HIGH" : "LOW", OUT_PIN);
        dcf77_power(false);
        return;
    }

    /* --- 2. OUT polarity: configured, cross-checked against duty ------ */
    if ((g_out_pulse_high && high_permille > 600u) ||
        (!g_out_pulse_high && high_permille < 400u)) {
        cprintf("  Duty cycle disagrees with CONFIG_DCF77_OUT_ACTIVE_LOW=%d --\n"
                "  check the module's polarity jumper. Continuing with the\n"
                "  configured setting; this no longer overrides it, because\n"
                "  inferring polarity needs a good signal and a bad one is\n"
                "  exactly when it would be believed wrongly.\n",
                CONFIG_DCF77_OUT_ACTIVE_LOW);
    } else if (high_permille >= 400u && high_permille <= 600u) {
        cprintf("  Duty cycle %u.%u%% is not the ~10-20%% a DCF-77 pulse train should\n"
                "  give, so the widths below may be measuring noise rather than a\n"
                "  signal. That much duty usually means exactly that.\n",
                high_permille / 10, high_permille % 10);
    }
    cprintf("  -> PON active %s, OUT pulses %s.\n\n",
            g_pon_active_low ? "LOW" : "HIGH", g_out_pulse_high ? "HIGH" : "LOW");

    if (!raw_trace()) goto aborted;
    cprintf("\nPer-pulse detail (width -> the bit that width decodes to):\n");

    /* --- 3. Watch pulses ---------------------------------------------- */
    uint64_t t0 = time_get_ms();
    uint64_t end = t0 + (uint64_t)secs * 1000u;
    uint64_t next_yield = t0 + 50;

    bool cur = pulse_active(), cand = cur;
    uint64_t cand_since = t0;
    uint64_t pulse_start = 0, prev_start = 0;
    bool have_pulse = false, have_prev = false;

    unsigned n_pulses = 0, n_bit0 = 0, n_bit1 = 0, n_bad = 0, n_marks = 0;
    uint32_t gap_err_sum = 0, gap_err_max = 0, n_gaps = 0;

    while (time_get_ms() < end) {
        bool act = pulse_active();
        uint64_t now = time_get_ms();

        if (act != cand) {
            cand = act;
            cand_since = now;
        } else if (cand != cur && (now - cand_since) >= DEBOUNCE_MS) {
            /* Timestamp the transition at the *first* sample that saw the new
             * level, not at the moment the debounce completed -- otherwise
             * every width and every gap carries a systematic +DEBOUNCE_MS. */
            uint64_t at = cand_since;
            cur = cand;

            if (cur) {
                pulse_start = at;
                have_pulse = true;
            } else if (have_pulse) {
                uint32_t width = (uint32_t)(at - pulse_start);
                uint32_t gap = have_prev ? (uint32_t)(pulse_start - prev_start) : 0;
                const char *bit = "?";
                if (width >= BIT0_MIN_MS && width < BIT0_MAX_MS)      { bit = "0"; n_bit0++; }
                else if (width >= BIT1_MIN_MS && width <= BIT1_MAX_MS) { bit = "1"; n_bit1++; }
                else                                                    { n_bad++; }
                n_pulses++;

                uint64_t rel = pulse_start - t0;
                cprintf("  t=%3u.%03u  width=%4u ms", (unsigned)(rel / 1000),
                        (unsigned)(rel % 1000), (unsigned)width);
                if (have_prev) {
                    uint32_t err = gap > 1000 ? gap - 1000 : 1000 - gap;
                    /* Second 59 carries no pulse at all, so a ~2 s gap is the
                     * minute mark -- the one gap that is correct rather than
                     * an error, and the thing a decoder synchronises on. */
                    if (gap > 1700 && gap < 2300) {
                        n_marks++;
                        cprintf("  gap=%4u ms  -> %s   << MINUTE MARK", (unsigned)gap, bit);
                    } else {
                        gap_err_sum += err;
                        if (err > gap_err_max) gap_err_max = err;
                        n_gaps++;
                        cprintf("  gap=%4u ms  -> %s", (unsigned)gap, bit);
                    }
                } else {
                    cprintf("  gap=   -      -> %s", bit);
                }
                cprintf("\n");

                prev_start = pulse_start;
                have_prev = true;
            }
        }

        if (console_interrupt_requested()) { console_interrupt_clear(); break; }
        if (now >= next_yield) { sched_yield(); next_yield = now + 50; }
        time_delay_us(SAMPLE_US);
    }

    /* --- 4. Summary ---------------------------------------------------- */
    {
        uint64_t elapsed = time_get_ms() - t0;
        unsigned el_s = (unsigned)(elapsed / 1000);
        cprintf("\nSummary over %u s:\n", el_s);
        cprintf("  pulses seen   : %u   (one per second expected, minus the minute marks)\n", n_pulses);
        cprintf("  100 ms -> 0   : %u\n", n_bit0);
        cprintf("  200 ms -> 1   : %u\n", n_bit1);
        cprintf("  out of range  : %u\n", n_bad);
        cprintf("  minute marks  : %u\n", n_marks);
        if (n_gaps) {
            cprintf("  spacing error : mean %u ms, worst %u ms (against a 1000 ms grid)\n",
                    (unsigned)(gap_err_sum / n_gaps), (unsigned)gap_err_max);
        }
        cprintf("  PON polarity  : active %s%s\n", g_pon_active_low ? "LOW" : "HIGH",
                (g_pon_active_low == (CONFIG_DCF77_PON_ACTIVE_LOW != 0))
                    ? " (as the board file says)"
                    : " (BOARD FILE IS WRONG -- fix CONFIG_DCF77_PON_ACTIVE_LOW)");
        cprintf("  OUT polarity  : pulses are %s\n", g_out_pulse_high ? "HIGH" : "LOW");

        /* An honest verdict, so the number does not have to be interpreted by
         * whoever is holding the antenna. */
        unsigned expect = el_s ? el_s : 1;
        if (n_pulses == 0) {
            cprintf("  verdict       : no pulses -- see the wiring checklist above.\n");
        } else if (n_bad * 4 > n_pulses || n_pulses * 4 < expect * 3) {
            cprintf("  verdict       : signal present but poor. Move the antenna, turn it\n"
                    "                  broadside to the transmitter, get it away from the\n"
                    "                  panel and any switching supply, and re-run.\n");
        } else if (gap_err_max > 100) {
            cprintf("  verdict       : pulses are clean but the spacing wanders -- likely\n"
                    "                  interference adding or eating edges.\n");
        } else {
            cprintf("  verdict       : clean. This is good enough to decode from.\n");
        }
    }

    dcf77_power(false);
    cprintf("%s", powered_down_note());
    return;

aborted:
    dcf77_power(false);
    cprintf("\nProbe aborted.\n");
}

/* ------------------------------------------------- live decoding ------ */

/* `(dcf-sync [secs] [set])`: the missing link between the pin layer in this
 * file and the portable frame decoder in dcf77_decode.c. Everything above
 * this point measures the *signal* -- widths, gaps, voltages, polarities --
 * and prints it for a human. This one feeds the same pin, sample for sample,
 * into dcf77_feed() and reports the *time*.
 *
 * It is deliberately not yet D4's sync controller: no state machine, no
 * nightly auto-sync, no /proc/dcf77, no display policy. It is the bring-up
 * step that proves the decoder works against a real transmitter rather than
 * against dcf77_selftest()'s synthetic streams, and D4 is then a controller
 * around a known-good decode path rather than around two unproven halves.
 *
 * Why it takes minutes and not seconds: a frame is 59 seconds of radio, and
 * the decoder only believes a time when a *second* frame agrees with the
 * first (as many minutes later as our own monotonic clock says have passed,
 * see dcf77_decode.h). Two frames plus the warm-up is a little over two
 * minutes in the best case, so anything shorter cannot succeed by
 * construction and the timeout below is raised rather than honoured.
 *
 * Writing the clock is opt-in (`set`), not the default. A DS3231 holding a
 * slightly-wrong time is better than one holding a garbage time, and during
 * bring-up the interesting run is the one you watch, not the one that
 * silently changes something. */
void dcf77_sync(unsigned secs, bool set_rtc) {
    /* Warm-up + two frames + a spare minute for the first minute mark, which
     * on average arrives half a minute in. */
    const unsigned min_secs = (unsigned)(CONFIG_DCF77_WARMUP_MS / 1000u) + 200u;
    if (secs == 0) secs = 300;
    if (secs > 3600) secs = 3600;
    if (secs < min_secs) {
        cprintf("Raising the timeout from %u s to %u s: a validated time needs two\n"
                "frames, which is two minutes of radio however good the signal is.\n",
                secs, min_secs);
        secs = min_secs;
    }

    dcf77_init();
    console_interrupt_clear();

    cprintf("\nDCF-77 sync: OUT=GP%d, timeout %u s%s.\n", OUT_PIN, secs,
            set_rtc ? ", will SET the clock on success" : ", read-only (pass 1 to set the clock)");
    cprintf("Ctrl-C aborts, leaving the clock untouched.\n\n");

    dcf77_power(true);
    {
        uint64_t warm = warmup_remaining_ms();
#if CONFIG_DCF77_PON_PRESENT
        cprintf("PON (GP%d) driven %s; waiting %u ms for the AGC to settle...\n",
                PON_PIN, g_pon_active_low ? "LOW" : "HIGH", (unsigned)warm);
#else
        if (warm) cprintf("Receiver permanently on; %u ms of warm-up left.\n", (unsigned)warm);
#endif
        if (warm && !wait_abortable(warm)) goto aborted;
    }

    {
        unsigned edges = 0, high_permille = 0;
        if (!survey(SURVEY_MS, &edges, &high_permille)) goto aborted;
        cprintf("  %u ms survey: %u edges, pin high %u.%u%%; pulses configured %s.\n",
                (unsigned)SURVEY_MS, edges, high_permille / 10, high_permille % 10,
                g_out_pulse_high ? "HIGH" : "LOW");
        if (edges < 2) {
            cprintf("\nThe line is not moving. Nothing to decode -- run (dcf-raw 60) or\n"
                    "(dcf-pins) to find out why before spending %u s here.\n", secs);
            dcf77_power(false);
            return;
        }
        if ((g_out_pulse_high && high_permille > 600u) ||
            (!g_out_pulse_high && high_permille < 400u)) {
            cprintf("  >> Duty cycle disagrees with CONFIG_DCF77_OUT_ACTIVE_LOW=%d. If this\n"
                    "  >> run times out with pulses seen but no frames, that is the reason:\n"
                    "  >> the decoder is locked onto the gaps, not the pulses.\n",
                    CONFIG_DCF77_OUT_ACTIVE_LOW);
        }
    }

    static dcf77_t d;   /* ~200 bytes; static keeps it off a driver stack */
    dcf77_reset(&d, g_out_pulse_high);

    cprintf("\nDecoding. One frame per minute; the second one that agrees wins.\n");

    uint64_t t0 = time_get_ms();
    uint64_t end = t0 + (uint64_t)secs * 1000u;
    uint64_t next_report = t0 + 10000;
    uint64_t next_yield = t0 + 50;

    dcf77_stats_t st, prev_st;
    dcf77_get_stats(&d, &prev_st);

    rtc_time_t got;
    uint64_t   mark_ms = 0;
    bool have_time = false;

    while (time_get_ms() < end) {
        uint64_t now = time_get_ms();
        dcf77_feed(&d, dcf77_raw_level(), now);

        if (dcf77_take_time(&d, &got, &mark_ms)) { have_time = true; break; }

        dcf77_get_stats(&d, &st);

        /* A frame just completed: say so at the moment it happens, with the
         * reason if it was thrown away. Waiting for the summary to report
         * "3 frames, 0 accepted" tells you the same thing a minute later and
         * without saying which minute went wrong. */
        if (st.frames_seen != prev_st.frames_seen) {
            const char *why = "accepted";
            if (st.frames_accepted == prev_st.frames_accepted) {
                if (st.parity_errors  != prev_st.parity_errors)  why = "rejected: parity";
                else if (st.framing_errors != prev_st.framing_errors) why = "rejected: framing (bit 0/20 or timezone)";
                else if (st.range_errors   != prev_st.range_errors)   why = "rejected: field out of range";
                else if (st.weekday_errors != prev_st.weekday_errors) why = "rejected: weekday disagrees with date";
                else why = "held, waiting for a second frame to agree";
            }
            cprintf("  [%3u s] frame %u complete -- %s\n",
                    (unsigned)((now - t0) / 1000), (unsigned)st.frames_seen, why);
        }

        if (now >= next_report) {
            cprintf("  [%3u s] bit %2d/59  pulses=%u (%u bad)  sync losses=%u  q=",
                    (unsigned)((now - t0) / 1000), st.bit_index,
                    (unsigned)st.pulses_seen, (unsigned)st.pulses_bad,
                    (unsigned)st.sync_losses);
            /* One digit 0-7 per second, oldest first: the same score D3 will
             * draw as a bar per matrix column, in the one form a serial line
             * can show. */
            for (unsigned i = 0; i < st.quality_count; i++)
                cprintf("%c", '0' + (st.quality[i] > 7 ? 7 : st.quality[i]));
            cprintf("\n");
            next_report = now + 10000;
        }

        prev_st = st;
        if (console_interrupt_requested()) { console_interrupt_clear(); goto aborted; }
        if (now >= next_yield) { sched_yield(); next_yield = now + 50; }
        time_delay_us(SAMPLE_US);
    }

    dcf77_get_stats(&d, &st);
    dcf77_power(false);

    if (have_time) {
        static const char *const wd[8] = { "?", "Mon", "Tue", "Wed", "Thu",
                                           "Fri", "Sat", "Sun" };
        /* `got` is UTC: the frame states its own offset in Z1/Z2 and the
         * decoder applies it, so nothing here has to consult a timezone rule
         * to know what the transmitter meant. The local reading below is a
         * separate question -- what THIS clock should display -- and is the
         * one place CONFIG_TIMEZONE gets a say. */
        rtc_time_t loc;
        const char *abbrev = "";
        char iso[32];
        tz_utc_to_local(&got, &loc);
        tz_offset_min(&got, &abbrev, NULL);
        unsigned w = dcf77_weekday_from_date(loc.year, loc.month, loc.day);

        time_format_iso(&got, iso, sizeof(iso));
        cprintf("\nTIME DECODED: %s UTC, after %u s and %u frames.\n",
                iso, (unsigned)((time_get_ms() - t0) / 1000),
                (unsigned)st.frames_seen);
        time_format_iso(&loc, iso, sizeof(iso));
        cprintf("  Local: %s %s (%s), TZ=%s\n", iso, abbrev, wd[w > 7 ? 0 : w], tz_get());
        cprintf("  The frame said %s (UTC+%d), which is how the UTC above was reached --\n"
                "  by what the transmitter stated, not by any rule of ours.\n",
                st.is_dst ? "CEST" : "CET", st.utc_offset_min / 60);
        if (set_rtc) {
            /* Carried forward from the decoder's mark to *now* (P1,
             * plan/phase24_dcf77_precision_and_ntp_server.md). Setting `got`
             * directly, as this did, asserts that no time has passed since
             * the second it names -- and by this point a good deal has: the
             * debounce that confirmed the mark, the loop iteration that
             * noticed, and then six lines of console output above, which at
             * 115200 baud is tens of milliseconds on its own. The service
             * path has always carried this forward correctly; this
             * diagnostic, which is the one people use to set a clock by hand,
             * never did. */
            uint64_t elapsed = time_get_ms() - mark_ms;
            rtc_time_t set;
            time_from_epoch(time_to_epoch(&got) + (int64_t)(elapsed / 1000u), &set);
            set.ms = (uint16_t)(elapsed % 1000u);
            time_set_utc(&set);
            bool ds = i2c_rtc_write_time(&set);
            cprintf("  Kernel clock set%s.\n",
                    ds ? " and UTC written to the DS3231" :
                         "; no DS3231 answered, so this is lost at the next reset");
        } else {
            cprintf("  Clock NOT changed. Re-run as (dcf-sync %u 1) to set it.\n", secs);
        }
    } else {
        cprintf("\nNo validated time in %u s. What the decoder saw:\n", secs);
        cprintf("  pulses        : %u (%u of them a bad width)\n",
                (unsigned)st.pulses_seen, (unsigned)st.pulses_bad);
        cprintf("  sync losses   : %u   (spacing that fitted neither 1 s nor 2 s)\n",
                (unsigned)st.sync_losses);
        cprintf("  frames        : %u seen, %u accepted\n",
                (unsigned)st.frames_seen, (unsigned)st.frames_accepted);
        cprintf("  rejects       : parity %u, framing %u, range %u, weekday %u\n",
                (unsigned)st.parity_errors, (unsigned)st.framing_errors,
                (unsigned)st.range_errors, (unsigned)st.weekday_errors);
        cprintf("  worst spacing : %u ms off the 1 s grid\n",
                (unsigned)st.spacing_err_max_ms);
        if (st.pulses_seen == 0) {
            cprintf("  -> Nothing arrived at all. (dcf-raw 60) next.\n");
        } else if (st.frames_seen == 0) {
            cprintf("  -> Pulses but no minute mark, so the decoder never found second 59.\n"
                    "     A missing pulse looks exactly like a minute mark when it is the\n"
                    "     signal dropping out, and a real mark is missed when noise fills\n"
                    "     it in. This is a reception problem, not a decoder problem.\n");
        } else if (st.frames_accepted == 0) {
            cprintf("  -> Whole frames arrived but none survived its checks: individual\n"
                    "     bits are being flipped. Move the antenna before changing code.\n");
        } else {
            cprintf("  -> Frames were accepted but no two agreed. Either reception dropped\n"
                    "     between them or the run was too short; try a longer timeout.\n");
        }
        cprintf("Clock unchanged.\n");
    }
    cprintf("%s", powered_down_note());
    return;

aborted:
    dcf77_power(false);
    cprintf("\nSync aborted; clock unchanged.\n");
}
