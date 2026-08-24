/*
 * LugalOS Hardware Driver: Waveshare Pico-Clock-Green LED matrix (L2,
 * plan/phase11_pico_clock_green.md).
 *
 * Hardware Mapping (cmake/board-rp2350-clock.cmake is the source of truth):
 *   GP10 : SM16106 CLK  (column shift clock, Function 5 - SIO GPIO)
 *   GP11 : SM16106 SDI  (column shift data,  Function 5 - SIO GPIO)
 *   GP12 : SM16106 LE   (column latch,       Function 5 - SIO GPIO)
 *   GP13 : SM16106/SM5166P OE (output enable, active low, Function 5 - SIO)
 *   GP16 : SM5166P A0   (row address bit 0,  Function 5 - SIO GPIO)
 *   GP18 : SM5166P A1   (row address bit 1,  Function 5 - SIO GPIO)
 *   GP22 : SM5166P A2   (row address bit 2,  Function 5 - SIO GPIO)
 *   GP26 : LDR          (ADC0, analog input)
 *
 * A bit-banged 3-wire column shift (same shape as drivers/tm1638_rp2350.c's
 * SIO GPIO_OUT_SET/CLR bit-banging) plus a 3-bit binary row address, not a
 * real SPI/I2C peripheral. Protocol (row-scan cadence, SM16106 bit order,
 * SM5166P row addressing, LDR-threshold dimming) reverse-engineered from
 * the vendor firmware at ~/gith/Pico-Clock-Green (Pico-Clock-Green.c
 * send_data()/display_char()/repeating_timer_callback_ms()/_us(), ziku.h) --
 * see plan/phase11_pico_clock_green.md's L0 section for exact citations.
 *
 * ADC register facts (base address, CS/RESULT layout, RESETS bit) verified
 * against ~/gith/pico/pico-sdk/src/rp2350/hardware_regs/include/hardware/
 * regs/{adc,resets}.h rather than assumed from RP2040 memory -- RP2350
 * moved several peripheral base addresses (see phase9 H0's own note on
 * this file for the SPI0/SPI1 precedent).
 *
 * Font glyphs (7 rows tall, 4-5 columns wide depending on glyph) are
 * transcribed directly from ~/gith/Pico-Clock-Green/ziku.h's first, clean
 * occurrence of each character (digits 0-9, ':', the Celsius-degree glyph,
 * '-') -- that file's later entries (past index ~26) have an inconsistent
 * stride (8 bytes instead of 7 for a few glyphs) that display_char()'s own
 * fixed 7-byte-stride indexing can't actually reach, so those aren't
 * ported; not needed for this driver's narrowed 24h/Celsius-only scope
 * anyway.
 */

#include "drivers/pico_clock_green.h"
#include "pico_clock_font.h"
#include "drivers/pico_clock_internal.h"
#include "drivers/i2c_rtc.h"
#include "kernel/timezone.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "kernel/sched.h"
#include "kernel/chan.h"
#include "kernel/printk.h"
#include "kernel/mem_domain.h"
#include "kernel/device.h"
#include "kernel/ipc.h"
#include "kernel/palloc.h"
#include "arch/umode.h"
#include "lugalos_config.h"
#if CONFIG_ENABLE_DCF77
/* After lugalos_config.h, necessarily: the guard is defined there. */
#include "drivers/dcf77_service.h"
#endif
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define IO_BANK0_BASE           0x40028000UL
#define IO_BANK0_CTRL(n)        (IO_BANK0_BASE + 0x004 + (n) * 8)

#define PADS_BANK0_BASE         0x40038000UL
#define PADS_BANK0_PAD(n)       (PADS_BANK0_BASE + 0x004 + (n) * 4)

/* RP2350's Secure/Non-secure split, and the third time this project has had
 * to learn it (M5 Phase 1 for GPIO, Phase 3 for I2C, Phase 6 for UART0 --
 * drivers/uart_rp2350.c and drivers/i2c_rtc.c carry the datasheet citations).
 * It is a filter entirely upstream of PMP that a U-mode task's own domain
 * cannot grant its way around: the peripheral has to be marked Non-secure
 * accessible from M-mode, before the task exists.
 *
 * Phase 17b needed all three shapes at once, which is what made this the
 * expensive lesson it was:
 *
 *   - **GPIO** is per-pin (GPIO_NSMASK0, one bit per GPIO, no password) and
 *     fails SILENTLY: writes to a pin whose bit is clear are ignored and the
 *     panel simply stays dark.
 *   - **ADC and TIMER0** are per-peripheral registers with SP/NSP/SU/NSU bits
 *     (reset 0xfc: Secure only) and need the 0xacce write password, and they
 *     fail LOUDLY: a load access fault, which is how this was found --
 *     `[Trap] User task faulted: cause 5, epc=0x1002800e` on the very first
 *     TIMER0 read of the very first frame, on hardware, 2026-08-24.
 *
 * TIMER0 is the one no previous driver needed, because no previous U-mode
 * driver kept its own clock; it is also the one whose absence is least
 * obvious, since kernel-mode code reads the same register through
 * time_get_us() without any of this applying. */
#define ACCESSCTRL_BASE          0x40060000UL
#define ACCESSCTRL_GPIO_NSMASK0  (ACCESSCTRL_BASE + 0x0c)  /* GPIO 0-31, no password */
#define ACCESSCTRL_ADC           (ACCESSCTRL_BASE + 0x7c)
#define ACCESSCTRL_TIMER0        (ACCESSCTRL_BASE + 0x98)
#define ACCESSCTRL_NSP           (1u << 1)
#define ACCESSCTRL_NSU           (1u << 0)
#define ACCESSCTRL_WRITE_PASSWORD 0xacce0000UL

#define SIO_BASE                0xD0000000UL
#define SIO_GPIO_OUT_SET        (SIO_BASE + 0x018)
#define SIO_GPIO_OUT_CLR        (SIO_BASE + 0x020)
#define SIO_GPIO_OE_SET         (SIO_BASE + 0x038)
#define SIO_GPIO_OE_CLR         (SIO_BASE + 0x040)
#define SIO_GPIO_IN             (SIO_BASE + 0x004)

#define RESETS_BASE             0x40020000UL
#define RESETS_RESET_SET        (RESETS_BASE + 0x2000)
#define RESETS_RESET_CLR        (RESETS_BASE + 0x3000)
#define RESETS_RESET_DONE       (RESETS_BASE + 0x0008)
#define ADC_RESET_BIT           (1u << 0) /* RESETS_RESET_ADC, resets.h */

#define CLOCKS_BASE              0x40010000UL
#define CLOCKS_CLK_ADC_CTRL      (CLOCKS_BASE + 0x6C)
#define CLOCKS_CLK_ADC_ENABLE_BIT (1u << 11) /* CLOCKS_CLK_ADC_CTRL_ENABLE, clocks.h */

#define ADC_BASE                0x400a0000UL
#define ADC_CS                  (ADC_BASE + 0x00)
#define ADC_RESULT              (ADC_BASE + 0x04)
#define ADC_CS_READY_BIT        (1u << 8)
#define ADC_CS_START_ONCE_BIT   (1u << 2)
#define ADC_CS_EN_BIT           (1u << 0)
#define ADC_CS_AINSEL_LSB       12

#define REG(addr) (*(volatile uint32_t *)(addr))

#define OE_PIN  CONFIG_CLOCK_OE_GPIO
#define SDI_PIN CONFIG_CLOCK_SDI_GPIO
#define CLK_PIN CONFIG_CLOCK_CLK_GPIO
#define LE_PIN  CONFIG_CLOCK_LE_GPIO
#define A0_PIN  CONFIG_CLOCK_A0_GPIO
#define A1_PIN  CONFIG_CLOCK_A1_GPIO
#define A2_PIN  CONFIG_CLOCK_A2_GPIO
#define LDR_PIN CONFIG_CLOCK_ADC_LIGHT_GPIO

#define BTN_SET_PIN  CONFIG_CLOCK_BTN_SET_GPIO
#define BTN_UP_PIN   CONFIG_CLOCK_BTN_UP_GPIO
#define BTN_DOWN_PIN CONFIG_CLOCK_BTN_DOWN_GPIO

#define OE_MASK  (1u << OE_PIN)
#define SDI_MASK (1u << SDI_PIN)
#define CLK_MASK (1u << CLK_PIN)
#define LE_MASK  (1u << LE_PIN)
#define A0_MASK  (1u << A0_PIN)
#define A1_MASK  (1u << A1_PIN)
#define A2_MASK  (1u << A2_PIN)

/* The driver's own clock, and deliberately not kernel/time.h's (phase 17b).
 *
 * Everything below this comment is the server half: from S2 it runs in U-mode
 * under a five-region domain, where `time_get_ms()` is not reachable -- it is
 * kernel code, and its boot offset is a kernel `.bss` variable no grant
 * covers. What IS granted is TIMER0, read-only, so the server keeps its own
 * monotonic millisecond count from the raw 1 MHz counter.
 *
 * It does not agree with the kernel's clock and does not need to: the only
 * things it measures are button debounce, long-press and repeat intervals --
 * all differences, never absolutes -- and every event crossing the wire
 * carries a duration rather than a timestamp. Deltas are taken in 32-bit
 * unsigned arithmetic so the ~71-minute wrap of TIMERAWL is a non-event
 * rather than the once-an-hour glitch a 64-bit widening would have made it.
 *
 * TIMERAWL, not the TIMEHR/TIMELR pair kernel/time.c reads: the latching pair
 * exists to make a coherent 64-bit read, and 32 raw bits is all this needs. */
#define TIMER0_BASE      0x400B0000UL
#define TIMER0_TIMERAWL  (*(volatile uint32_t *)(TIMER0_BASE + 0x28))

CLOCK_UATTR static inline uint32_t srv_raw_us(void) { return TIMER0_TIMERAWL; }

/* Brightness, all of it, in one place: seven levels of OE on-time out of the
 * 1000 us row period, and the ambient-light readings that pick one of them
 * when brightness is automatic.
 *
 * The levels are geometric, not linear. Phase 11 used level * 140 us and
 * phase 17 shipped it that way; on real hardware in a dark room the bottom
 * of that ramp is still glaring (user, 2026-08-24), because a 14% duty cycle
 * is nowhere near 1/7th of the perceived brightness of a 100% one -- the eye
 * is roughly logarithmic in luminance, so evenly spaced duty cycles bunch up
 * at the top and leave nothing usable at the bottom. Each step here is about
 * 2.2x the one below, which spreads the seven settings over the range that
 * is actually visible and puts level 1 at ~0.8% duty: dim enough for a dark
 * bedroom, which is what the bottom of the scale is for.
 *
 * 8 us is the floor on purpose. The pulse is a busy-wait between two GPIO
 * writes, so anything much shorter stops being reliably reproducible from
 * row to row and the panel starts to shimmer instead of just being dim.
 *
 * Level 7 is 0, meaning "do not chop at all" -- OE simply stays open, one
 * less thing happening in the scan loop at full brightness. */
#define LEVEL_MIN 1
#define LEVEL_MAX 7
CLOCK_UDATA static const uint16_t LEVEL_ON_US[LEVEL_MAX] = { 8, 18, 40, 90, 200, 450, 0 };

/* Ambient light -> level, ascending, in raw 12-bit ADC counts. The LDR
 * divider reads *higher* in the dark, so entry i is the reading at or above
 * which the level drops to LEVEL_MAX-1-i.
 *
 * 2800 is the vendor firmware's own dimming threshold (repeating_timer_
 * callback_us(), Pico-Clock-Green.c) on the same physical LDR and divider,
 * and it stays put as the boundary where dimming begins; what phase 11
 * ported was the single threshold and its single 330 us dim duty, so
 * automatic brightness was a two-state affair -- full, or one fixed dim. The
 * rest of the ladder is this driver's, so that "automatic" tracks the room
 * instead of falling off a cliff, and so that a genuinely dark room reaches
 * the bottom of the scale rather than stopping around level 4's worth of
 * light output. */
CLOCK_UDATA static const uint16_t AUTO_DARKER_AT[LEVEL_MAX - 1] = {
    1200, 1900, 2400, 2800, 3200, 3600
};

/* The deadband around each of those boundaries, in ADC counts. Without it a
 * room sitting exactly on a threshold flickers: the LDR is noisy, the reading
 * is re-taken every frame, and each sample lands on a different side of the
 * line (user, 2026-08-24). A level that is already engaged holds until the
 * reading has come back 150 counts *past* the boundary, so crossing back and
 * forth takes a real change in the light, not noise. Kept below half the
 * narrowest threshold spacing (400) so the bands stay ordered. */
#define AUTO_HYSTERESIS 150

/* Second half of the same fix, and the one that handles slow drift rather
 * than sample noise: the reading that feeds the ladder is an exponential
 * moving average, not the raw conversion. Shift 4 over a sample every 8 ms
 * (one per frame, row 0) is a time constant of about 130 ms -- fast enough
 * that switching a lamp on is not perceived as a delay, slow enough that a
 * hand passing over the sensor never reaches a threshold at all. */
#define AUTO_EMA_SHIFT 4

/* One row of the eight, and the number the whole scan hangs off: 8 rows at
 * 1 ms is a 125 Hz frame, above flicker fusion, and it is the denominator
 * every LEVEL_ON_US[] entry above is a numerator of. */
#define CLOCK_ROW_PERIOD_US   1000u

/* ======================================= the server's memory ============
 *
 * Every mutable byte the server half touches, in one place, because from
 * phase 17b it runs in U-mode and a PMP domain grants *regions*, not
 * variables. Scattered `static` globals in ordinary `.bss` would each need
 * their own grant; there is a budget of five for the whole task and three are
 * already spoken for by SIO, ADC and TIMER0.
 *
 * ## Why the stack is in here too
 *
 * That budget is why. Stack, text, SIO, ADC, TIMER0 is already five, and this
 * state would be a sixth. drivers/usb_cdc.c keeps its state region separate
 * because it needs only two MMIO windows; the clock needs three, so the stack
 * and the state share one 2 KB NAPOT block.
 *
 * The order inside that block is the safety property: **stack low, state
 * high**. The stack pointer starts at the top of `stack[]` and grows *down*
 * toward the region's base, so an overflow runs off the bottom of the granted
 * region and takes a fault. Laid out the other way round -- state below a
 * downward-growing stack -- an overflow would quietly scribble on the frame
 * buffer and the button state instead, which is exactly the failure the
 * separate regions elsewhere in this tree exist to avoid.
 *
 * ## It is NOT zeroed by boot
 *
 * `.ustacks2048` is a NOLOAD section outside `__bss_start..__bss_end`, so
 * boot_header.S never clears it -- the fault that made both personas fail to
 * start from a bare USB charger until phase 17 §9 found it in usb_cdc's own
 * region. pico_clock_green_init() zeroes this explicitly, by hand, and the
 * `-1` and `LEVEL_MAX` seeds below are applied there rather than being
 * initialisers the loader would have to honour.
 */
#define TEXT_COLS_MAX 96u
#define BTN_RING      8u

typedef struct {
    bool     level;          /* debounced: true = pressed */
    bool     cand;           /* what the pin last read */
    uint64_t cand_since_ms;
    uint64_t pressed_at_ms;
    uint64_t next_repeat_ms; /* 0 = not repeating */
} btn_t;

typedef struct {
    /* The frame buffer: 4 column-groups (32 bits, matching the two cascaded
     * 16-channel SM16106s) x 8 rows, indexed disp_buf[8*group + row] -- the
     * same layout as the vendor's own disp_buf[], so the bit-packing math
     * below stays a direct, checkable port of display_char(). */
    uint8_t  disp_buf[32];

    /* The indicator and weekday LEDs share that buffer with the digits but
     * not their lifetime: a digit is redrawn whenever the minute changes and
     * pico_clock_green_clear() wipes the whole buffer to do it, so their
     * state is kept here and re-applied after every clear. Teaching clear()
     * which bits to spare would put the same knowledge in a worse place.
     *
     * They never collide with a glyph: draw_glyph() writes rows 1-7 only and
     * preserves the low `col` bits of the first group it touches, and every
     * layout in this file starts at column 2 -- which is exactly why the
     * vendor firmware carries its own +2 disp_offset. */
    uint8_t  ind_bits[8];    /* columns 0-1 of group 0, one entry per row */
    uint8_t  weekday;        /* 0 = none lit, else 1 = Monday .. 7 = Sunday */

    uint8_t  row;            /* which row the scan is on */
    uint16_t dim_on_us;      /* 0 = full brightness (no PWM pulse needed) */
    /* -1 = the LDR decides, as phase 11 shipped it; 1..7 = a fixed level from
     * the menu, which suspends the LDR entirely (clock_hw_set_brightness). */
    int      brightness_level;
    /* Automatic brightness only: the smoothed reading and the level it holds.
     * The level is state, not a pure function of the reading -- that is what
     * makes the hysteresis hysteresis. 0 = not sampled yet, so the first frame
     * after switching to automatic seeds the average outright rather than
     * fading up to it from nowhere. */
    uint16_t light_ema;
    int      auto_level;

    /* The server's own clock (see srv_time_advance()). */
    uint32_t srv_last_us;    /* TIMERAWL at the previous advance */
    uint32_t srv_frac_us;    /* microseconds not yet worth a millisecond */
    uint64_t srv_ms;         /* monotonic, server-local */

    uint8_t  text_cols[TEXT_COLS_MAX];   /* one rendered string, column-major */

    btn_t    btn[CLOCK_KEY_COUNT];
    uint8_t  key_ring[BTN_RING][2];
    unsigned key_head, key_tail;
    uint32_t key_drops;
} clock_state_t;

#define CLOCK_USTACK_SIZE  1536u
#define CLOCK_REGION_SIZE  2048u

typedef union {
    struct {
        uint8_t       stack[CLOCK_USTACK_SIZE];   /* grows DOWN from its top */
        clock_state_t st;
    } f;
    uint8_t pad[CLOCK_REGION_SIZE];
} clock_region_t;

_Static_assert(sizeof(((clock_region_t *)0)->f) <= CLOCK_REGION_SIZE,
               "clock stack + state no longer fit their PMP region");

static clock_region_t g_clock_region __attribute__((aligned(CLOCK_REGION_SIZE)))
                                     __attribute__((section(".ustacks2048")));

/* Every call site below reads `g_disp_buf` and friends exactly as it did when
 * they were separate globals -- drivers/usb_cdc.c's `#define g_usb` sets the
 * precedent, and for the same reason: the alternative is several hundred
 * mechanical edits whose diff would hide the ones that matter. */
#define S                  (g_clock_region.f.st)
#define g_disp_buf         S.disp_buf
#define g_ind_bits         S.ind_bits
#define g_weekday          S.weekday
#define g_row              S.row
#define g_dim_on_us        S.dim_on_us
#define g_brightness_level S.brightness_level
#define g_light_ema        S.light_ema
#define g_auto_level       S.auto_level
#define g_srv_last_us      S.srv_last_us
#define g_srv_frac_us      S.srv_frac_us
#define g_srv_ms           S.srv_ms
#define g_text_cols        S.text_cols
#define g_btn              S.btn
#define g_key_ring         S.key_ring
#define g_key_head         S.key_head
#define g_key_tail         S.key_tail
#define g_key_drops        S.key_drops

#if CONFIG_CLOCK_BOOT_BEACON
/* Set by pico_clock_green_init(): before it, the beacon has clicks only. */
static bool g_matrix_pins_up;
#endif

CLOCK_UATTR static void srv_time_advance(void) {
    uint32_t now = srv_raw_us();
    uint32_t delta = now - g_srv_last_us;    /* wrap-safe by construction */
    g_srv_last_us = now;
    g_srv_frac_us += delta;
    while (g_srv_frac_us >= 1000u) { g_srv_frac_us -= 1000u; g_srv_ms++; }
}


/* Glyphs are 7 bytes (rows 1-7 of an 8-row cell), each byte's low bits are
 * the lit columns of that row, transcribed from ziku.h -- see this file's
 * header comment. */
CLOCK_UDATA static const uint8_t GLYPH_DIGIT[10][7] = {
    {0x06, 0x09, 0x09, 0x09, 0x09, 0x09, 0x06}, /* 0 */
    {0x04, 0x06, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* 1 */
    {0x06, 0x09, 0x08, 0x04, 0x02, 0x01, 0x0F}, /* 2 */
    {0x06, 0x09, 0x08, 0x06, 0x08, 0x09, 0x06}, /* 3 */
    {0x08, 0x0C, 0x0A, 0x09, 0x0F, 0x08, 0x08}, /* 4 */
    {0x0F, 0x01, 0x07, 0x08, 0x08, 0x09, 0x06}, /* 5 */
    {0x04, 0x02, 0x01, 0x07, 0x09, 0x09, 0x06}, /* 6 */
    {0x0F, 0x09, 0x04, 0x04, 0x04, 0x04, 0x04}, /* 7 */
    {0x06, 0x09, 0x09, 0x06, 0x09, 0x09, 0x06}, /* 8 */
    {0x06, 0x09, 0x09, 0x0E, 0x08, 0x04, 0x02}, /* 9 */
};
CLOCK_UDATA static const uint8_t GLYPH_COLON[7] = {0x00, 0x03, 0x03, 0x00, 0x03, 0x03, 0x00};
CLOCK_UDATA static const uint8_t GLYPH_DEGC[7]  = {0x01, 0x0C, 0x12, 0x02, 0x02, 0x12, 0x0C};
CLOCK_UDATA static const uint8_t GLYPH_MINUS[7] = {0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00};

/* Column positions (disp_offset already folded in, unlike the vendor's own
 * +2 done at the display_char() call site). Time layout matches the
 * vendor's exact "HH:MM" columns (Show_Time(), Pico-Clock-Green.c) --
 * already validated on real hardware there, so kept identical rather than
 * re-derived. Temperature layout is this driver's own (the vendor firmware
 * never had a non-scrolling temperature readout to port); both fit inside
 * the physical 24-column matrix with room to spare. */
#define COL_HOUR_TENS  2
#define COL_HOUR_ONES  7
#define COL_COLON      12
#define COL_MIN_TENS   15
#define COL_MIN_ONES   20

#define COL_TEMP_SIGN  2
#define COL_TEMP_TENS  6
#define COL_TEMP_ONES  11
#define COL_TEMP_DEGC  16

static void gpio_out_init(unsigned pin) {
    REG(IO_BANK0_CTRL(pin)) = 5; /* GPIO_FUNC_SIO */
    REG(PADS_BANK0_PAD(pin)) = 0x5A; /* PUE=1, PDE=0, IE=1 -- same convention as tm1638_rp2350.c */
    REG(SIO_GPIO_OE_SET) = (1u << pin);
}

CLOCK_UATTR static inline void oe_open(void)  { REG(SIO_GPIO_OUT_CLR) = OE_MASK; } /* active low */
CLOCK_UATTR static inline void oe_close(void) { REG(SIO_GPIO_OUT_SET) = OE_MASK; }

CLOCK_UATTR static void shift_byte(uint8_t data) {
    /* LSB-first, matching the vendor's send_data(): CLK low, drive SDI,
     * CLK high -- no explicit delay, same as the vendor (SM16106's shift
     * clock tolerates far faster edges than a software loop produces). */
    for (int i = 0; i < 8; i++) {
        REG(SIO_GPIO_OUT_CLR) = CLK_MASK;
        if (data & 1) {
            REG(SIO_GPIO_OUT_SET) = SDI_MASK;
        } else {
            REG(SIO_GPIO_OUT_CLR) = SDI_MASK;
        }
        data >>= 1;
        REG(SIO_GPIO_OUT_SET) = CLK_MASK;
    }
}

CLOCK_UATTR static inline void latch_pulse(void) {
    REG(SIO_GPIO_OUT_SET) = LE_MASK;
    REG(SIO_GPIO_OUT_CLR) = LE_MASK;
}

CLOCK_UATTR static void set_row_address(unsigned row) {
    if (row & 1) REG(SIO_GPIO_OUT_SET) = A0_MASK; else REG(SIO_GPIO_OUT_CLR) = A0_MASK;
    if (row & 2) REG(SIO_GPIO_OUT_SET) = A1_MASK; else REG(SIO_GPIO_OUT_CLR) = A1_MASK;
    if (row & 4) REG(SIO_GPIO_OUT_SET) = A2_MASK; else REG(SIO_GPIO_OUT_CLR) = A2_MASK;
}

static void adc_hw_init(void) {
    /* clk_adc has no glitchless mux and is disabled at reset
     * (CLOCKS_CLK_ADC_CTRL_ENABLE_RESET=0, clocks.h) -- found on real
     * hardware, not in any QEMU run: without this, every register access
     * below hangs the bus waiting for a clock edge that never comes, a
     * hang no software timeout in this function can catch since it's the
     * bus transaction itself that never completes, not a polling loop.
     * Same requirement drivers/uart_rp2350.c already handles for clk_peri
     * (CLOCKS_BASE+0x48 bit 11) -- not unique to the ADC. */
    REG(CLOCKS_CLK_ADC_CTRL) = CLOCKS_CLK_ADC_ENABLE_BIT;

    REG(RESETS_RESET_SET) = ADC_RESET_BIT;
    for (volatile int i = 0; i < 1000; i++);
    REG(RESETS_RESET_CLR) = ADC_RESET_BIT;
    int timeout = 10000;
    while (!(REG(RESETS_RESET_DONE) & ADC_RESET_BIT) && --timeout > 0);

    /* GP26 as a pure analog input: output driver disabled (OD=1), digital
     * input buffer disabled (IE=0), no pulls -- matches the Pico SDK's own
     * adc_gpio_init(), which likewise leaves the pin's IO_BANK0 function
     * select untouched (the ADC taps the pad directly). */
    REG(PADS_BANK0_PAD(LDR_PIN)) = 0x80;
    REG(SIO_GPIO_OE_CLR) = (1u << LDR_PIN);

    REG(ADC_CS) = ADC_CS_EN_BIT;
    time_delay_us(10); /* brief settle before the first conversion */
}

CLOCK_UATTR static uint16_t adc_read_light(void) {
    /* GP26 = ADC channel 0 always on this board (the only LDR pin wired),
     * so AINSEL is a fixed 0 rather than a general "GPIO to channel"
     * computation. */
    uint32_t cs = REG(ADC_CS);
    cs &= ~(0xFu << ADC_CS_AINSEL_LSB);
    cs |= ADC_CS_START_ONCE_BIT;
    REG(ADC_CS) = cs;

    int timeout = 10000;
    while (!(REG(ADC_CS) & ADC_CS_READY_BIT) && --timeout > 0);
    return (uint16_t)(REG(ADC_RESULT) & 0xFFFu);
}

CLOCK_UATTR static uint16_t pico_clock_green_hw_read_light(void) {
    return adc_read_light();
}

/* Direct port of the vendor's display_char() bit-packing (Pico-Clock-
 * Green.c:575-621), generalized from "char -> glyph lookup -> pack" to
 * "glyph -> pack" since this driver's glyph set is fixed and small enough
 * not to need the vendor's switch-statement lookup. col already includes
 * the vendor's disp_offset. Row 0 of each group (the status-LED row) is
 * never touched, same as upstream. */
CLOCK_UATTR static void draw_glyph(unsigned col, const uint8_t glyph[7]) {
    unsigned j = col / 8;
    unsigned k = col % 8;
    for (unsigned i = 1; i < 8; i++) {
        if (k > 0) {
            g_disp_buf[8 * j + i] = (uint8_t)((g_disp_buf[8 * j + i] & (0xFFu >> (8 - k))) | (glyph[i - 1] << k));
            if (j < 3) {
                g_disp_buf[8 * j + 8 + i] = (uint8_t)((g_disp_buf[8 * j + 8 + i] & (0xFFu << (8 - k))) | (glyph[i - 1] >> (8 - k)));
            }
        } else {
            g_disp_buf[8 * j + i] = glyph[i - 1];
        }
    }
}

/* ------------------------------------------------ text rendering ------- */

/* The visible text area: columns 2 to 23 inclusive, 22 of them.
 *
 * The panel is 24 columns wide in total and the first two are the indicator
 * LEDs -- which is what the vendor's +2 disp_offset was always avoiding -- so
 * text gets the remaining 22, not 24. The frame buffer is 32 columns because
 * that is how many outputs two cascaded 16-channel SM16106s have; columns
 * 24-31 are shifted out and land nowhere.
 *
 * This was 25 on the first cut, and the evidence that it was wrong came off
 * the hardware rather than out of the datasheet: a 22-column "DD.MM" centred
 * in a 24-column area starts at 3 and ends at 24, and the last column of the
 * month simply was not there. The existing clock face agrees -- its rightmost
 * digit sits at column 20 and is 4 wide, ending at exactly 23.
 *
 * Anything outside is clipped rather than wrapped: text that does not fit
 * scrolls (below), it does not reappear on the left. */
#define TEXT_COL_FIRST ((unsigned)CLOCK_TEXT_COL_FIRST)
#define TEXT_COL_LAST  ((unsigned)CLOCK_TEXT_COL_LAST)

CLOCK_UATTR static inline void set_pixel(unsigned col, unsigned row, bool on) {
    if (col > TEXT_COL_LAST || col < TEXT_COL_FIRST || row == 0 || row > 7) return;
    unsigned g = col / 8u, b = col % 8u;
    if (on) g_disp_buf[8u * g + row] |= (uint8_t)(1u << b);
    else    g_disp_buf[8u * g + row] &= (uint8_t)~(1u << b);
}

/* Blit a column-major bitmap at `dest`, which may be negative or past the
 * right edge -- that is what makes scrolling a single expression instead of a
 * special case at each end. Only the text area is touched, so the indicator
 * and weekday LEDs survive untouched and do not need re-applying. */
CLOCK_UATTR static void draw_cols(int dest, const uint8_t *cols, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        int col = dest + (int)i;
        if (col < (int)TEXT_COL_FIRST || col > (int)TEXT_COL_LAST) continue;
        for (unsigned r = 0; r < CLOCK_GLYPH_ROWS; r++) {
            set_pixel((unsigned)col, r + 1u, (cols[i] & (1u << r)) != 0);
        }
    }
}

/* `text_cols` (with the rest of the server state, above) is wide enough for
 * the longest label the menu will scroll -- "TEMPERATURE" is 65 columns --
 * with room over. In the state region rather than on the stack, which from
 * phase 17b is 1536 bytes and shares its PMP region with that very state. */

/* Clear only the text area, leaving the LEDs alone -- the full clear() wipes
 * everything and then re-applies them, which is right when the whole screen
 * changes and wasteful when only the digits do. */
CLOCK_UATTR static void clear_text_area(void) {
    for (unsigned col = TEXT_COL_FIRST; col <= TEXT_COL_LAST; col++) {
        for (unsigned r = 1; r <= CLOCK_GLYPH_ROWS; r++) set_pixel(col, r, false);
    }
}

/* Draw `s` into the text area, centred if it fits and left-aligned if it does
 * not (in which case the tail is simply clipped -- a caller that cares should
 * scroll instead). */
CLOCK_UATTR static void draw_text(const char *s) {
    unsigned n = clock_font_render(s, g_text_cols, TEXT_COLS_MAX);
    unsigned avail = TEXT_COL_LAST - TEXT_COL_FIRST + 1u;
    int dest = (int)TEXT_COL_FIRST;
    if (n < avail) dest += (int)((avail - n) / 2u);
    clear_text_area();
    draw_cols(dest, g_text_cols, n);
}

/* Forward declarations: pico_clock_green_init() (boot-time, before the task
 * exists) is the only caller of these outside the task body below. */
CLOCK_UATTR static void pico_clock_green_clear(void);
static void buttons_init(void);

void pico_clock_green_init(void) {
    gpio_out_init(OE_PIN);
    gpio_out_init(SDI_PIN);
    gpio_out_init(CLK_PIN);
    gpio_out_init(LE_PIN);
    gpio_out_init(A0_PIN);
    gpio_out_init(A1_PIN);
    gpio_out_init(A2_PIN);

    oe_close(); /* stay blanked until the first real frame is shifted out */
    REG(SIO_GPIO_OUT_CLR) = SDI_MASK | CLK_MASK | LE_MASK | A0_MASK | A1_MASK | A2_MASK;

#if CONFIG_CLOCK_BOOT_BEACON
    g_matrix_pins_up = true;
#endif
    adc_hw_init();
    buttons_init();
#ifdef CONFIG_CLOCK_BUZZER_GPIO
    /* Driven low BEFORE the output is enabled, so bringing the pad up cannot
     * produce a click on every boot. */
    REG(SIO_GPIO_OUT_CLR) = (1u << CONFIG_CLOCK_BUZZER_GPIO);
    REG(IO_BANK0_CTRL(CONFIG_CLOCK_BUZZER_GPIO)) = 5;
    REG(PADS_BANK0_PAD(CONFIG_CLOCK_BUZZER_GPIO)) = 0x52;  /* SCHMITT|4mA|IE */
    REG(SIO_GPIO_OE_SET) = (1u << CONFIG_CLOCK_BUZZER_GPIO);
#endif

    /* Everything this driver's U-mode server will touch has to be marked
     * Non-secure accessible first, from here -- M-mode, before the task
     * exists. See ACCESSCTRL_GPIO_NSMASK0's own comment above for why each
     * of these three lines is shaped differently.
     *
     * The GPIO mask covers every pin the server drives or reads: the seven
     * matrix pins, the three buttons, and the buzzer. Not the LDR (GP26 is an
     * analog input; the ADC reads it through its own peripheral, which is the
     * next line) and not the DCF-77 pin (its decoder runs in the caller's
     * task, in kernel mode, and is not confined at all). */
    REG(ACCESSCTRL_GPIO_NSMASK0) |= (OE_MASK | SDI_MASK | CLK_MASK | LE_MASK
                                     | A0_MASK | A1_MASK | A2_MASK
                                     | (1u << BTN_SET_PIN)
                                     | (1u << BTN_UP_PIN)
                                     | (1u << BTN_DOWN_PIN)
#ifdef CONFIG_CLOCK_BUZZER_GPIO
                                     | (1u << CONFIG_CLOCK_BUZZER_GPIO)
#endif
                                     );
    REG(ACCESSCTRL_ADC) = ACCESSCTRL_WRITE_PASSWORD | REG(ACCESSCTRL_ADC)
                          | ACCESSCTRL_NSP | ACCESSCTRL_NSU;
    REG(ACCESSCTRL_TIMER0) = ACCESSCTRL_WRITE_PASSWORD | REG(ACCESSCTRL_TIMER0)
                             | ACCESSCTRL_NSP | ACCESSCTRL_NSU;

    /* The whole state region, by hand, because nothing else will: it lives in
     * .ustacks2048, a NOLOAD section outside the range boot_header.S clears,
     * so on a cold start it holds whatever SRAM held. That is not a
     * hypothetical -- it is the exact fault that kept both personas from
     * booting on a bare USB charger until phase 17 §9 found it in usb_cdc's
     * own region. A byte loop rather than memset(): this is the one place
     * that touches the region before the server owns it, and a call into libc
     * here would be the only reason to keep one linked in reach. */
    {
        volatile uint8_t *p = (volatile uint8_t *)&S;
        for (unsigned i = 0; i < sizeof(S); i++) p[i] = 0;
    }
    /* The two fields whose zero is not their default. */
    g_brightness_level = -1;          /* automatic: the LDR decides */
    g_auto_level = LEVEL_MAX;

    srv_time_advance();               /* seed the server clock from TIMERAWL */
    pico_clock_green_clear();
}

/* ------------------------------------------------- indicator LEDs ------ */

/* Row 0 of groups 0/1/2 carries the seven weekday LEDs, in an order that is
 * neither contiguous nor regular -- Friday is even split across two groups.
 * Transcribed bit for bit from the vendor's Monday..Sunday macros
 * (~/gith/pico-clock-green/define.h:91-105) rather than derived, because
 * there is nothing to derive it from: it is how the panel is wired.
 *
 * Bits 2 and 5 of group 0's row 0 are the vendor's "back light" pair and are
 * left alone by everything here. */
CLOCK_UDATA static const uint8_t WEEKDAY_G0[8] = { 0, 0x18, 0xC0, 0,    0,    0,    0,    0    };
CLOCK_UDATA static const uint8_t WEEKDAY_G1[8] = { 0, 0,    0,    0x06, 0x30, 0x80, 0,    0    };
CLOCK_UDATA static const uint8_t WEEKDAY_G2[8] = { 0, 0,    0,    0,    0,    0x01, 0x0C, 0x60 };
#define WEEKDAY_G0_ALL 0xD8u
#define WEEKDAY_G1_ALL 0xB6u
#define WEEKDAY_G2_ALL 0x6Du

/* Columns 0-1 of group 0, one indicator per row -- the vendor's dis_* macros
 * (define.h:107-127). Two of the ten are a single column rather than a pair
 * (C/F share row 3, AM/PM share row 4), which is why this is a table of
 * (row, mask) and not just a row number. The top one is the vendor's
 * "MoveOn", which means nothing here; it is our DCF status LED. */
CLOCK_UDATA static const struct { uint8_t row, mask; } IND_BIT[CLOCK_IND_COUNT] = {
    [CLOCK_IND_DCF]       = { 0, 0x03 },
    [CLOCK_IND_ALARM]     = { 1, 0x03 },
    [CLOCK_IND_COUNTDOWN] = { 2, 0x03 },
    [CLOCK_IND_F]         = { 3, 0x01 },
    [CLOCK_IND_C]         = { 3, 0x02 },
    [CLOCK_IND_AM]        = { 4, 0x01 },
    [CLOCK_IND_PM]        = { 4, 0x02 },
    [CLOCK_IND_COUNTUP]   = { 5, 0x03 },
    [CLOCK_IND_CHIME]     = { 6, 0x03 },
    [CLOCK_IND_AUTOLIGHT] = { 7, 0x03 },
};

/* Stamp the LED state into the frame buffer. Called after every clear, and
 * after any change to either. */
CLOCK_UATTR static void apply_leds(void) {
    for (unsigned row = 0; row < 8; row++) {
        g_disp_buf[row] = (uint8_t)((g_disp_buf[row] & ~0x03u) | g_ind_bits[row]);
    }
    unsigned d = (g_weekday >= 1 && g_weekday <= 7) ? g_weekday : 0;
    g_disp_buf[0]  = (uint8_t)((g_disp_buf[0]  & ~WEEKDAY_G0_ALL) | WEEKDAY_G0[d]);
    g_disp_buf[8]  = (uint8_t)((g_disp_buf[8]  & ~WEEKDAY_G1_ALL) | WEEKDAY_G1[d]);
    g_disp_buf[16] = (uint8_t)((g_disp_buf[16] & ~WEEKDAY_G2_ALL) | WEEKDAY_G2[d]);
}

CLOCK_UATTR static void pico_clock_green_hw_set_weekday(unsigned dow) {
    g_weekday = (dow >= 1 && dow <= 7) ? (uint8_t)dow : 0;
    apply_leds();
}

CLOCK_UATTR static void pico_clock_green_hw_indicator(clock_indicator_t ind, bool on) {
    if ((unsigned)ind >= CLOCK_IND_COUNT) return;
    uint8_t row = IND_BIT[ind].row, mask = IND_BIT[ind].mask;
    if (on) g_ind_bits[row] |= mask;
    else    g_ind_bits[row] = (uint8_t)(g_ind_bits[row] & ~mask);
    apply_leds();
}

CLOCK_UATTR static void pico_clock_green_clear(void) {
    /* A byte loop, not memset(): this runs in U-mode from phase 17b, where a
     * call out of .clocktext faults -- and under -fno-builtin (this tree's
     * flags) memset() is a real call, not an inlined store loop. */
    for (unsigned i = 0; i < sizeof(g_disp_buf); i++) g_disp_buf[i] = 0;
    apply_leds();
}

CLOCK_UATTR static void pico_clock_green_show_time(unsigned hour, unsigned minute, bool colon) {
    if (hour > 23) hour = 23;
    if (minute > 59) minute = 59;

    pico_clock_green_clear();
    draw_glyph(COL_HOUR_TENS, GLYPH_DIGIT[hour / 10]);
    draw_glyph(COL_HOUR_ONES, GLYPH_DIGIT[hour % 10]);
    if (colon) draw_glyph(COL_COLON, GLYPH_COLON);
    draw_glyph(COL_MIN_TENS, GLYPH_DIGIT[minute / 10]);
    draw_glyph(COL_MIN_ONES, GLYPH_DIGIT[minute % 10]);
}

CLOCK_UATTR static void pico_clock_green_show_temperature_c(int temp_c) {
    bool negative = temp_c < 0;
    int mag = negative ? -temp_c : temp_c;
    if (mag > 99) mag = 99; /* display is two digits wide */

    pico_clock_green_clear();
    if (negative) {
        draw_glyph(COL_TEMP_SIGN, GLYPH_MINUS);
    }
    if (mag >= 10) {
        draw_glyph(COL_TEMP_TENS, GLYPH_DIGIT[mag / 10]);
    }
    draw_glyph(COL_TEMP_ONES, GLYPH_DIGIT[mag % 10]);
    draw_glyph(COL_TEMP_DEGC, GLYPH_DEGC);
}

/* A DC load with no switching whatsoever: shift one all-on column pattern
 * out once, latch it, park the row address, and hold OE open. The row stays
 * lit continuously -- no multiplexing, no OE chopping, no shift clock -- so
 * it draws real current (tens of mA through 24 LEDs) while radiating
 * essentially nothing.
 *
 * Two users, both from plan/phase17_clock_ui_and_dcf77.md. The first is the
 * quiet-sync indicator (section 3, M5): the only way to light anything on
 * this board without running the multiplexer. The second is the M0
 * experiment -- the Pico's buck regulator idles in PFM/power-save at light
 * load, and its variable-frequency ripple is a known DCF-77 killer; on a
 * Pico 2 W the PWM-mode pin is behind the wireless chip and unreachable, so
 * raising the load is the one lever left. This separates "more current on
 * 3V3" from "the display is switching", which lighting it normally would
 * not. */
void pico_clock_green_static_load(bool on) {
    if (!on) {
        oe_close();
        for (unsigned group = 0; group < 4; group++) shift_byte(0x00);
        latch_pulse();
        return;
    }
    oe_close();
    for (unsigned group = 0; group < 4; group++) shift_byte(0xFF);
    latch_pulse();
    set_row_address(0);
    oe_open();
}

/* ------------------------------------------------------- buttons ------- */

/*
 * Polled from inside the ~1 ms row-scan loop, not from a task of its own. The
 * cadence is already exactly right for a 20 ms debounce, the SIO window is
 * already in the clock task's hands, and a second task polling the same
 * registers would duplicate a hardware grant to gain nothing. The cost is one
 * SIO read plus a little arithmetic per row -- against a 1000 us period.
 *
 * Active LOW with the internal pull-up, per the vendor firmware (see the
 * board file). A press is classified on RELEASE, so short and long are one
 * event type each and never both: holding SET for a second produces exactly
 * one LONG when it comes back up, not a SHORT on the way there. UP and DOWN
 * additionally auto-repeat while held, because the time-set screens are
 * unusable without it -- and a repeat stream ends with no further event, so a
 * held-then-released UP is "repeat, repeat, repeat", never "...and a LONG".
 */
#define BTN_DEBOUNCE_MS       20u
#define BTN_LONG_MS          400u   /* the vendor uses 300; 400 is easier to
                                     * hit deliberately without catching an
                                     * ordinary slow press */
#define BTN_REPEAT_DELAY_MS  600u
#define BTN_REPEAT_PERIOD_MS 200u   /* 5/s */

CLOCK_UDATA static const uint8_t BTN_PIN[CLOCK_KEY_COUNT] = {
    [CLOCK_KEY_SET]  = BTN_SET_PIN,
    [CLOCK_KEY_UP]   = BTN_UP_PIN,
    [CLOCK_KEY_DOWN] = BTN_DOWN_PIN,
};


/* An 8-entry ring (BTN_RING, with the rest of the state). Deep enough that
 * nothing a human can do overflows it at a one-frame drain, and when it does
 * overflow the NEWEST event is dropped: the queue then still describes a
 * prefix of what happened, in order, rather than a gap in the middle of it.
 * The drop count is kept because a silent drop in an input path is the kind
 * of bug that gets blamed on the hardware. */
/* Two bytes an entry since phase 17b: the event, and how long the button was
 * held. The duration belongs to the event, not to the button -- by the time a
 * frame's worth of events is drained the same key may be down again -- so it
 * travels with it rather than being looked up per key afterwards. */

CLOCK_UATTR static void key_push(clock_key_t key, clock_press_t press, uint32_t held_ms) {
    unsigned next = (g_key_head + 1u) % BTN_RING;
    if (next == g_key_tail) { g_key_drops++; return; }
    g_key_ring[g_key_head][0] = (uint8_t)(((unsigned)press << 2) | (unsigned)key);
    /* 16 ms units: the wire is bytes, a press worth measuring is hundreds of
     * milliseconds, and 255 * 16 ms covers every gesture a person makes at a
     * clock with three buttons. */
    g_key_ring[g_key_head][1] = (uint8_t)(held_ms > 4080u ? 255u : (held_ms / 16u));
    g_key_head = next;
}

CLOCK_UATTR static bool key_pop(clock_key_t *key, clock_press_t *press, uint8_t *held16) {
    if (g_key_tail == g_key_head) return false;
    uint8_t e = g_key_ring[g_key_tail][0];
    uint8_t h = g_key_ring[g_key_tail][1];
    g_key_tail = (g_key_tail + 1u) % BTN_RING;
    if (key)    *key    = (clock_key_t)(e & 0x03u);
    if (press)  *press  = (clock_press_t)((e >> 2) & 0x03u);
    if (held16) *held16 = h;
    return true;
}

static void buttons_init(void) {
    for (unsigned i = 0; i < CLOCK_KEY_COUNT; i++) {
        REG(IO_BANK0_CTRL(BTN_PIN[i])) = 5;              /* GPIO_FUNC_SIO */
        REG(PADS_BANK0_PAD(BTN_PIN[i])) = 0x5A;          /* SCHMITT|PUE|IE, input */
        REG(SIO_GPIO_OE_CLR) = (1u << BTN_PIN[i]);
        g_btn[i] = (btn_t){ false, false, 0, 0, 0 };
    }
    g_key_head = g_key_tail = 0;
    g_key_drops = 0;
}

CLOCK_UATTR static void buttons_poll(uint64_t now_ms) {
    uint32_t in = REG(SIO_GPIO_IN);

    for (unsigned i = 0; i < CLOCK_KEY_COUNT; i++) {
        btn_t *b = &g_btn[i];
        bool raw = (in & (1u << BTN_PIN[i])) == 0;   /* active low */

        if (raw != b->cand) {
            b->cand = raw;
            b->cand_since_ms = now_ms;
            continue;
        }
        if (raw != b->level && (now_ms - b->cand_since_ms) >= BTN_DEBOUNCE_MS) {
            b->level = raw;
            if (raw) {
                b->pressed_at_ms = b->cand_since_ms;
                b->next_repeat_ms = 0;
            } else {
                /* Released. A press that already auto-repeated has been
                 * reported in full; reporting a short or long press on top of
                 * it would double-count the same gesture. */
                if (b->next_repeat_ms == 0) {
                    uint64_t held = b->cand_since_ms - b->pressed_at_ms;
                    key_push((clock_key_t)i,
                             held >= BTN_LONG_MS ? CLOCK_PRESS_LONG : CLOCK_PRESS_SHORT,
                             (uint32_t)held);
                }
                b->next_repeat_ms = 0;
            }
            continue;
        }

        /* Auto-repeat, UP/DOWN only: SET's long press is a distinct command
         * (back/exit), so repeating it would fire that command over and over
         * while the finger is still down. */
        if (b->level && (i == CLOCK_KEY_UP || i == CLOCK_KEY_DOWN)) {
            if (b->next_repeat_ms == 0) {
                if (now_ms - b->pressed_at_ms >= BTN_REPEAT_DELAY_MS) {
                    key_push((clock_key_t)i, CLOCK_PRESS_REPEAT, 0);
                    b->next_repeat_ms = now_ms + BTN_REPEAT_PERIOD_MS;
                }
            } else if (now_ms >= b->next_repeat_ms) {
                key_push((clock_key_t)i, CLOCK_PRESS_REPEAT, 0);
                b->next_repeat_ms = now_ms + BTN_REPEAT_PERIOD_MS;
            }
        }
    }
}

/* One boundary of the ladder, with the deadband applied in whichever
 * direction it needs to be: a level already engaged has to see the light come
 * back past `t - AUTO_HYSTERESIS` to disengage, one not yet engaged has to
 * see `t + AUTO_HYSTERESIS` to engage. Between the two nothing happens, which
 * is the point. */
CLOCK_UATTR static int auto_level_for(uint16_t light, int cur) {
    int level = LEVEL_MAX;
    for (unsigned i = 0; i < LEVEL_MAX - 1; i++) {
        uint16_t t = AUTO_DARKER_AT[i];
        bool engaged = (cur <= (int)(LEVEL_MAX - 1 - i));
        uint16_t edge = engaged ? (uint16_t)(t - AUTO_HYSTERESIS)
                                : (uint16_t)(t + AUTO_HYSTERESIS);
        if (light >= edge) level--;
    }
    return level;
}

/* Sampled once per frame from row 0, so this runs at ~125 Hz. */
CLOCK_UATTR static void auto_brightness_update(void) {
    uint16_t raw = adc_read_light();

    if (g_light_ema == 0) g_light_ema = raw;   /* first sample: seed, don't fade */
    else g_light_ema = (uint16_t)(g_light_ema
                                  + ((int32_t)raw - (int32_t)g_light_ema) / (1 << AUTO_EMA_SHIFT));

    g_auto_level = auto_level_for(g_light_ema, g_auto_level);
    g_dim_on_us = LEVEL_ON_US[g_auto_level - 1];
}

/* The row's own waits, and deliberately not time_delay_us(): that one
 * services USB between polls of the clock, and a single usb_cdc_task() call
 * is longer than the whole 8 us bottom brightness level. A row whose pulse
 * overran would simply be brighter than its neighbours, which is exactly the
 * shimmer the short levels exist to avoid. From S2 it is also unreachable:
 * this half runs in U-mode, where the only clock is the granted TIMER0.
 *
 * Nothing else may happen inside these windows. */
CLOCK_UATTR static inline void spin_us(uint32_t us) {
    uint32_t start = srv_raw_us();
    while ((uint32_t)(srv_raw_us() - start) < us) { /* spin */ }
}

CLOCK_UATTR static void pico_clock_green_scan_step(void) {
    oe_close();

    for (unsigned group = 0; group < 4; group++) {
        shift_byte(g_disp_buf[8 * group + g_row]);
    }
    latch_pulse();
    set_row_address(g_row);

    /* Only sample the LDR while brightness is automatic. A fixed level is a
     * deliberate choice and must not be overridden a millisecond later by a
     * passing shadow. */
    if (g_row == 0 && g_brightness_level < 0) {
        auto_brightness_update();
    }

    if (g_dim_on_us == 0) {
        oe_open(); /* left on until the next scan_step() call overwrites it */
    } else {
        oe_open();
        spin_us(g_dim_on_us);
        oe_close();
    }

    g_row = (g_row + 1) & 7;
}

/* One row, including the rest of its period -- the unit the panel is actually
 * made of. Splitting the wait out of the scan (as every caller used to do,
 * with its own `time_delay_us(CLOCK_ROW_PERIOD_US)`) meant the row period was
 * really "however long the caller took plus 1000 us", which was tolerable
 * while the caller was three lines away in this file and is not now that it
 * is on the other side of a channel. Anchoring the wait to when the row
 * STARTED keeps the panel's cadence a property of the driver. */
CLOCK_UATTR static void scan_row(void) {
    uint32_t started = srv_raw_us();
    pico_clock_green_scan_step();
    while ((uint32_t)(srv_raw_us() - started) < CLOCK_ROW_PERIOD_US) { /* spin */ }
}

/* One frame: every row once, buttons polled per row so their timing keeps the
 * ~1 ms resolution it had when the appliance loop lived in here.
 *
 * It ends with OE closed, and that closing line is load-bearing. scan_row()
 * leaves the row it lit switched ON -- the next row's scan_step() is what
 * normally turns it off -- so whatever row is current when the scanning stops
 * keeps burning for however long the gap lasts. Before phase 17b the gap was
 * the appliance's once-a-second I2C read, landing on a different row each
 * time because the row index kept advancing: the panel showed a slightly
 * brighter line that crawled up the display once a second (reported on
 * hardware in phase 11 and lived with). After the split the gap moved to the
 * end of the frame op, so it always landed on row 7 -- the same crawl, now
 * pinned to the bottom line, which is more noticeable rather than less
 * (user, 2026-08-24).
 *
 * Closing OE here removes it outright instead of moving it again: every row
 * gets exactly its own row period lit and nothing extra, and the gap between
 * frames is dark for all eight equally. The cost is the gap's worth of
 * average brightness, spread evenly, which is what a dimming level does
 * anyway -- and at any brightness below 7 OE was already closed, which is why
 * this only ever showed at full brightness. */
CLOCK_UATTR static unsigned pico_clock_green_hw_scan_frame(uint8_t *out, unsigned max_ev) {
    for (unsigned r = 0; r < 8u; r++) {
        srv_time_advance();
        buttons_poll(g_srv_ms);
        scan_row();
    }
    oe_close();

    unsigned n = 0;
    clock_key_t k;
    clock_press_t pr;
    uint8_t held16;
    while (n < max_ev && key_pop(&k, &pr, &held16)) {
        out[n * 2]     = (uint8_t)(((unsigned)pr << 2) | (unsigned)k);
        out[n * 2 + 1] = held16;
        n++;
    }
    return n;
}

/* --------------------------------------------- the boot beacon --------- */

/*
 * A boot-progress indicator for a board with no console, no LED and no SD
 * card (user, 2026-08-23). Its only outputs are the matrix and the buzzer, so
 * this uses both: `stage` short clicks, and `stage` LEDs lit in the top row.
 *
 * The matrix half is the useful half, and the reason is that it **latches**.
 * The pattern is shifted out once, latched, the row address parked and OE
 * held open -- no multiplexing, no scanning, nothing that needs a running
 * loop. So if the boot then hangs, the last stage reached stays lit,
 * indefinitely, and the panel reports where it stopped. A blinking heartbeat
 * cannot do that: it stops, and every failure looks the same.
 *
 * Deliberately direct hardware, and deliberately safe to call before
 * pico_clock_green_init(): the very first stages happen before there is a
 * display driver, a scheduler or a task to route through. The buzzer pin is
 * configured on first use; the matrix half does nothing until its pins exist,
 * which the stage numbering accounts for.
 */
#if CONFIG_CLOCK_BOOT_BEACON

static bool g_beacon_pins_up;

/* The beacon's own delay, and deliberately NOT time_delay_us().
 *
 * time_delay_us() pumps usb_cdc_task() on every iteration, so each click was
 * calling hundreds of times into the very subsystem this beacon is being used
 * to investigate. That makes it useless as evidence in two directions at
 * once: it perturbs what it measures, and a hang inside usb_cdc_task() would
 * be reported as "the code after the last click hung" when in truth the click
 * itself never returned. An instrument must not touch the thing under test.
 *
 * A raw spin on the hardware timer touches nothing: time_get_us() is a pair
 * of register reads with no side effects, and this runs before there is a
 * scheduler to yield to anyway. */
static void beacon_delay_us(uint64_t us) {
    uint64_t start = time_get_us();
    while (time_get_us() - start < us) { }
}

void pico_clock_green_boot_mark(unsigned stage) {
    if (stage == 0) stage = 1;
    if (stage > 24) stage = 24;

#ifdef CONFIG_CLOCK_BUZZER_GPIO
    if (!g_beacon_pins_up) {
        REG(SIO_GPIO_OUT_CLR) = (1u << CONFIG_CLOCK_BUZZER_GPIO);
        REG(IO_BANK0_CTRL(CONFIG_CLOCK_BUZZER_GPIO)) = 5;
        REG(PADS_BANK0_PAD(CONFIG_CLOCK_BUZZER_GPIO)) = 0x52;
        REG(SIO_GPIO_OE_SET) = (1u << CONFIG_CLOCK_BUZZER_GPIO);
        g_beacon_pins_up = true;
    }
    /* Countable by ear: short clicks with a clear gap. The whole beacon costs
     * about a fifth of a second per stage, which is invisible against a boot
     * and audible enough to count from across a room. */
    /* ONE click per mark, not `stage` of them (user, 2026-08-23). The marks
     * are consecutive, so the total number of clicks heard *is* the number of
     * steps completed -- the same information, in a quarter of the time, and
     * without asking anyone to count "one, pause, two, pause, three" while
     * keeping track of which group they are in. The stage number still drives
     * the LED row below, where a count is read at a glance rather than
     * remembered. */
    REG(SIO_GPIO_OUT_SET) = (1u << CONFIG_CLOCK_BUZZER_GPIO);
    beacon_delay_us(25000);
    REG(SIO_GPIO_OUT_CLR) = (1u << CONFIG_CLOCK_BUZZER_GPIO);
    beacon_delay_us(150000);
#endif

    if (!g_matrix_pins_up) return;  /* no matrix pins yet: the clicks stand alone */

    /* `stage` LEDs along the top row, latched and left lit. */
    oe_close();
    for (unsigned group = 0; group < 4; group++) {
        uint8_t bits = 0;
        for (unsigned b = 0; b < 8; b++) {
            unsigned col = group * 8u + b;
            if (col < stage) bits |= (uint8_t)(1u << b);
        }
        shift_byte(bits);
    }
    latch_pulse();
    set_row_address(0);
    oe_open();
}

#endif /* CONFIG_CLOCK_BOOT_BEACON */

/* ------------------------------------------- the app-facing seam ------- */

/*
 * C2's idle screen and C3's menu used to live here, in the middle of a file
 * about shift registers. They are now drivers/pico_clock_app.c, and what is
 * left below is the hardware half of drivers/pico_clock_internal.h: draw
 * this, read that, one frame of scan. No policy.
 *
 * Phase 17b moved the *client* half of that header out from under this
 * comment as well: what follows is what runs INSIDE the task, and the
 * `clock_hw_*` functions the app calls are further down, past the wire.
 */

/* The DS3231 measures its own die, inside a closed case, next to a
 * self-heating LED matrix and an RP2350: it reads high by a degree or two and
 * no amount of software fixes that. The offset that corrects it is applied in
 * the display path only -- i2c_rtc_read_temperature_c() keeps returning the
 * raw reading, because a sensor driver that quietly hands back a fudged
 * number is a trap for every other caller. It is a UI setting now (C3's
 * OFFS item), defaulting to the board file's figure. */
#ifndef CONFIG_CLOCK_TEMP_OFFSET_C
#define CONFIG_CLOCK_TEMP_OFFSET_C (-2)
#endif

CLOCK_UATTR static void pico_clock_green_hw_blank(void) {
    /* Physically, not just in the buffer: nothing scans once the appliance
     * loop returns, so a lit row would otherwise stay lit with whatever it
     * last shifted out rather than actually going dark. */
    pico_clock_green_clear();
    oe_close();
}

/* The idle pin levels, for the button diagnostic on the far side of the wire
 * -- the one question about the buttons that no amount of event watching can
 * answer, since a pin stuck low produces no events at all. */
CLOCK_UATTR static uint8_t pico_clock_green_hw_pin_levels(void) {
    uint32_t in = REG(SIO_GPIO_IN);
    return (uint8_t)(((in >> BTN_SET_PIN)  & 1u)
                   | (((in >> BTN_UP_PIN)   & 1u) << 1)
                   | (((in >> BTN_DOWN_PIN) & 1u) << 2));
}

/* Fixed and automatic brightness are the same seven levels of LEVEL_ON_US[];
 * the only difference is who picks the index. A fixed level is picked once,
 * here, and the LDR stops being read at all until brightness goes back to
 * automatic -- at which point the next frame re-seeds the average from the
 * room as it is now, rather than resuming from whatever it was before. */
CLOCK_UATTR static void pico_clock_green_hw_set_brightness(int level) {
    if (level >= LEVEL_MIN && level <= LEVEL_MAX) {
        g_brightness_level = level;
        g_dim_on_us = LEVEL_ON_US[level - 1];
    } else {
        g_brightness_level = -1;   /* back to the LDR */
        g_light_ema = 0;
    }
}

/* Held for `ms` with the row scan still running, so a beep never blanks the
 * panel. Active HIGH, per the vendor firmware (gpio_put(BUZZ,1) is on).
 *
 * The one op that deliberately blocks: 10-40 ms of buzzer is shorter than the
 * round trip it would take to start and stop it from outside, and a beep that
 * the caller has to remember to end is a beep that eventually never ends. */
CLOCK_UATTR static void pico_clock_green_hw_beep(unsigned ms) {
#ifdef CONFIG_CLOCK_BUZZER_GPIO
    if (ms == 0) return;
    if (ms > 2000u) ms = 2000u;   /* bounded: the wire is not a trust boundary */
    REG(SIO_GPIO_OUT_SET) = (1u << CONFIG_CLOCK_BUZZER_GPIO);
    srv_time_advance();
    uint64_t until = g_srv_ms + ms;
    while (g_srv_ms < until) {
        srv_time_advance();
        buttons_poll(g_srv_ms);
        scan_row();
    }
    REG(SIO_GPIO_OUT_CLR) = (1u << CONFIG_CLOCK_BUZZER_GPIO);
    oe_close();   /* same reason as the frame op: never leave a row burning */
#else
    (void)ms;
#endif
}

/* One frame of a scroll: the text rendered with its left edge at `dest`,
 * which is expected to be off one end of the panel or the other. draw_cols()
 * clips, so no caller has to think about the edges.
 *
 * The loop that walks `dest` used to live here (`scroll_text()`), together
 * with its 45 ms step, its Ctrl-C check and its "any keypress skips the
 * animation" rule. All three are policy, all three need the console or the
 * key queue, and phase 17b moved them to drivers/pico_clock_app.c where both
 * are legal. What is left is the pixels. */
CLOCK_UATTR static void pico_clock_green_hw_draw_text_at(int dest, const char *s) {
    unsigned n = clock_font_render(s, g_text_cols, TEXT_COLS_MAX);
    clear_text_area();
    draw_cols(dest, g_text_cols, n);
}

/* One column per second, newest on the right, height from the score. The
 * panel is 22 text columns wide and the decoder keeps 24 scores, so the
 * oldest two fall off the left -- which is the right end to lose, and is why
 * the newest is pinned to the right edge rather than the run being centred.
 *
 * A score of 0 (nothing heard that second) draws nothing at all rather than a
 * one-pixel stub: an empty column is the most legible thing on this display
 * and "the signal dropped here" is exactly what someone moving an antenna
 * needs to see. */
CLOCK_UATTR static void pico_clock_green_hw_draw_bars(const uint8_t *scores, unsigned n) {
    clear_text_area();
    if (!scores || n == 0) return;

    unsigned avail = TEXT_COL_LAST - TEXT_COL_FIRST + 1u;
    unsigned first = (n > avail) ? (n - avail) : 0;   /* drop the oldest */
    unsigned col = TEXT_COL_LAST - (n - first) + 1u;

    for (unsigned i = first; i < n; i++, col++) {
        unsigned h = scores[i];
        if (h > CLOCK_GLYPH_ROWS) h = CLOCK_GLYPH_ROWS;
        /* Grown from the bottom row upwards, like any bar chart. */
        for (unsigned k = 0; k < h; k++) set_pixel(col, CLOCK_GLYPH_ROWS - k, true);
    }
}


/* ================================================== the wire ============
 *
 * M4.5 (plan/phase12_microkernel_migration.md) made this driver a task and
 * gave it two ops, one of which -- 'R' run -- was the entire appliance loop
 * served as a single call. Phase 17b (plan/phase17b_clock_task_split.md)
 * takes that back out: the appliance runs in the caller's task now, exactly
 * where chess's UI loop runs, and what crosses this wire is one logical
 * hardware operation per call.
 *
 * The op that makes that affordable is 'F': eight rows of scan, ~8 ms,
 * buttons polled per row, key events returned. ~125 calls a second, not the
 * ~1000 a per-row op would have cost -- which is the number M4.5 was right
 * to refuse. Every microsecond-sensitive loop stays on this side.
 *
 *   'F' scan frame  req: [op]                     resp: [n, (ev, held16) * n]
 *   'C' clear       req: [op]                     resp: 0
 *   'Z' blank       req: [op]                     resp: 0  (buffer AND OE)
 *   'H' show time   req: [op, hour, min, colon]   resp: 0
 *   'E' show temp   req: [op, temp_c(i8)]         resp: 0
 *   'T' draw text   req: [op, chars...]           resp: 0
 *   'X' text at     req: [op, col(i8), chars...]  resp: 0
 *   'A' draw bars   req: [op, scores...]          resp: 0
 *   'W' weekday     req: [op, dow]                resp: 0
 *   'I' indicator   req: [op, ind, on]            resp: 0
 *   'S' brightness  req: [op, level(i8)]          resp: 0
 *   'P' beep        req: [op, ms_hi, ms_lo]       resp: 0  (blocks, scanning)
 *   'L' read light  req: [op]                     resp: light(2, big-endian)
 *   'G' pin levels  req: [op]                     resp: levels(1)
 *
 * Gone with the appliance: 'R' (run), 'K' (pop key -- 'F' returns them now),
 * and 'B'/'D'/'M' (the button, LED-walk and DCF-monitor diagnostics). The
 * last three did not move for tidiness: every one of them prints to the
 * console, and from S2 this task runs in U-mode where that is not reachable.
 * They are client-side loops over the ops above now, further down this file.
 */
#define CLOCK_OP_SCAN_FRAME ((uint8_t)'F')
#define CLOCK_OP_CLEAR      ((uint8_t)'C')
#define CLOCK_OP_BLANK      ((uint8_t)'Z')
#define CLOCK_OP_SHOW_TIME  ((uint8_t)'H')
#define CLOCK_OP_SHOW_TEMP  ((uint8_t)'E')
#define CLOCK_OP_TEXT       ((uint8_t)'T')
#define CLOCK_OP_TEXT_AT    ((uint8_t)'X')
#define CLOCK_OP_BARS       ((uint8_t)'A')
#define CLOCK_OP_WEEKDAY    ((uint8_t)'W')
#define CLOCK_OP_INDICATOR  ((uint8_t)'I')
#define CLOCK_OP_BRIGHTNESS ((uint8_t)'S')
#define CLOCK_OP_BEEP       ((uint8_t)'P')
#define CLOCK_OP_READ_LIGHT ((uint8_t)'L')
#define CLOCK_OP_PIN_LEVELS ((uint8_t)'G')

/* Sized by the text ops: two header bytes plus a label longer than anything
 * the 24-column panel shows without scrolling for a while. The response is
 * sized by 'F': one count byte plus two per event, for a ring that holds
 * eight. */
#define CLOCK_REQ_CAP  34u
#define CLOCK_RESP_CAP (1u + 2u * BTN_RING)

static uint8_t         g_clock_req[CLOCK_REQ_CAP];
static uint8_t         g_clock_resp[CLOCK_RESP_CAP];
static chan_endpoint_t *g_clock_ep;
static int              g_clock_task_pid = -1;

/* Counts chan_call()s actually served -- see drivers/uart_16550.c's
 * g_uart_write_calls comment for the reasoning.
 *
 * The number this reports changed meaning in phase 17b, which is worth
 * knowing before reading it as a regression: under M4.5 a whole clock session
 * was ONE served call ('R'), so `clockstats` read `calls=1` no matter how
 * long the clock ran. It now advances ~125 times a second while the appliance
 * is up, because that is the frame op doing its job. */
static uint32_t g_clock_calls;

uint32_t pico_clock_green_task_call_count(void) { return g_clock_calls; }

static bool clock_task_alive(void) {
    if (g_clock_task_pid < 0) return false;
    int st = sched_task_state(g_clock_task_pid);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

/* Hand-rolled per translation unit, not shared with any other driver's own
 * usys_*() stubs or user/progs/usys.h -- a CLOCK_UATTR function must not call
 * anything the compiler might place outside .clocktext, and a cross-file
 * inline is not a guarantee. */
__attribute__((always_inline)) static inline long clock_usys_chan_serve_wait(const char *name, uint8_t *buf, long buf_max) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_WAIT;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = buf_max;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}
__attribute__((always_inline)) static inline long clock_usys_chan_serve_reply(const char *name, const uint8_t *buf, long len) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_REPLY;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = len;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}

/* The server loop, in U-mode. Everything it calls is CLOCK_UATTR and
 * everything it touches is either the granted state region, one of the three
 * granted MMIO windows, or its own stack.
 *
 * Since phase 17b it calls nothing outside this file at all: no i2c, no
 * dcf77, no console, no printk. The cross-task call chain M4.5 documented
 * here (clock -> i2c) left with the appliance loop and now runs in the
 * caller's task, where it is shell/lisp -> i2c and shell/lisp -> clock: two
 * independent top-down calls rather than a chain.
 *
 * An if/else chain, not a switch, and not for style: GCC compiles a dispatch
 * this wide into a jump table in ordinary .rodata, which the indirect jump
 * then reads at run time -- outside every region this domain grants.
 * st7735_umode_body() found that on real hardware as a load access fault;
 * -fno-jump-tables on this file (CMakeLists.txt) is the belt to this
 * braces, since GCC's switch-conversion pass can rebuild a table out of an
 * if/else chain too. */
CLOCK_UATTR static void clock_umode_body(void) {
    /* Not a string literal: a literal lands in ordinary .rodata, outside
     * every region this task's domain grants -- the bug that hung the board
     * the first time this mechanism ran on hardware (M5 Phase 2). volatile,
     * because gcc otherwise recognises the run of stores and turns it back
     * into a copy from a .rodata blob. */
    volatile char name[8];
    name[0]='c'; name[1]='l'; name[2]='o'; name[3]='c';
    name[4]='k'; name[5]='\0';

    for (;;) {
        uint8_t req[CLOCK_REQ_CAP];
        uint8_t resp[CLOCK_RESP_CAP];
        long req_len = clock_usys_chan_serve_wait((const char *)name, req, sizeof(req));
        if (req_len < 1) {
            clock_usys_chan_serve_reply((const char *)name, resp, 0);
            continue;
        }

        uint8_t op = req[0];
        long resp_len = 0;

        if (op == CLOCK_OP_SCAN_FRAME) {
            unsigned n = pico_clock_green_hw_scan_frame(&resp[1], BTN_RING);
            resp[0] = (uint8_t)n;
            resp_len = 1 + 2 * (long)n;
        } else if (op == CLOCK_OP_CLEAR) {
            pico_clock_green_clear();
        } else if (op == CLOCK_OP_BLANK) {
            pico_clock_green_hw_blank();
        } else if (op == CLOCK_OP_SHOW_TIME) {
            if (req_len >= 4) pico_clock_green_show_time(req[1], req[2], req[3] != 0);
        } else if (op == CLOCK_OP_SHOW_TEMP) {
            if (req_len >= 2) pico_clock_green_show_temperature_c((int8_t)req[1]);
        } else if (op == CLOCK_OP_TEXT || op == CLOCK_OP_TEXT_AT) {
            /* The wire carries a length, not a terminator, so it is copied
             * into a buffer this side terminates -- never handed to a string
             * function as-is. A byte loop rather than memcpy(): under
             * -fno-builtin (this tree's flags) memcpy() is a real call, and a
             * call out of .clocktext faults. */
            unsigned hdr = (op == CLOCK_OP_TEXT_AT) ? 2u : 1u;
            char text[CLOCK_REQ_CAP];
            long n = (req_len > (long)hdr) ? (req_len - (long)hdr) : 0;
            if (n > (long)sizeof(text) - 1) n = (long)sizeof(text) - 1;
            for (long k = 0; k < n; k++) text[k] = (char)req[hdr + k];
            text[n] = '\0';
            if (op == CLOCK_OP_TEXT) draw_text(text);
            else pico_clock_green_hw_draw_text_at((int8_t)req[1], text);
        } else if (op == CLOCK_OP_BARS) {
            pico_clock_green_hw_draw_bars(&req[1], req_len > 1 ? (unsigned)(req_len - 1) : 0);
        } else if (op == CLOCK_OP_WEEKDAY) {
            if (req_len >= 2) pico_clock_green_hw_set_weekday(req[1]);
        } else if (op == CLOCK_OP_INDICATOR) {
            if (req_len >= 3) {
                pico_clock_green_hw_indicator((clock_indicator_t)req[1], req[2] != 0);
            }
        } else if (op == CLOCK_OP_BRIGHTNESS) {
            if (req_len >= 2) pico_clock_green_hw_set_brightness((int8_t)req[1]);
        } else if (op == CLOCK_OP_BEEP) {
            if (req_len >= 3) {
                pico_clock_green_hw_beep((unsigned)(((unsigned)req[1] << 8) | req[2]));
            }
        } else if (op == CLOCK_OP_READ_LIGHT) {
            uint16_t light = pico_clock_green_hw_read_light();
            resp[0] = (uint8_t)(light >> 8);
            resp[1] = (uint8_t)light;
            resp_len = 2;
        } else if (op == CLOCK_OP_PIN_LEVELS) {
            resp[0] = pico_clock_green_hw_pin_levels();
            resp_len = 1;
        }

        clock_usys_chan_serve_reply((const char *)name, resp, resp_len);
    }
}

/* 1536 bytes of stack, sharing one 2048-byte PMP region with the server state
 * (see clock_region_t): the deepest chain here is
 * clock_umode_body -> draw_text -> clock_font_render, and the two 34-byte
 * buffers in the dispatch frame are the largest thing on it. Measured from
 * the disassembly the way g_st7735_ustack's 1392 bytes were, not guessed. */
static mem_domain_t g_clock_domain;

/* This task's own kernel-mode entry point: task_create_sized() calls this
 * (ordinary kernel stack, kernel privilege) to build the domain and make the
 * one-way jump into U-mode. Five regions, which is exactly
 * MEM_DOMAIN_MAX_REGIONS on RP2350 -- see plan/phase17b_clock_task_split.md
 * for why the stack and the state share the first of them.
 *
 * TIMER0 is granted READ ONLY. The server needs to know the time and has no
 * business setting it; a display driver that could reprogram the system clock
 * would be a strange thing to have built deliberately. */
static void clock_task_body(void *arg) {
    (void)arg;
    while (!g_clock_ep) sched_yield();

    mem_domain_init(&g_clock_domain);
    mem_domain_add(&g_clock_domain, (uintptr_t)&g_clock_region, sizeof(g_clock_region),
                   MEM_R | MEM_W);

    uintptr_t tbase, tsize;
    board_clock_text_region(&tbase, &tsize);
    mem_domain_add(&g_clock_domain, tbase, tsize, MEM_R | MEM_X);

    mem_domain_add(&g_clock_domain, SIO_BASE, 4096, MEM_R | MEM_W);
    mem_domain_add(&g_clock_domain, ADC_BASE, 4096, MEM_R | MEM_W);
    mem_domain_add(&g_clock_domain, TIMER0_BASE, 4096, MEM_R);

    if (task_set_domain(sched_current_pid(), &g_clock_domain) != 0) {
        printk("[Clock] Refusing to enter U-mode: memory domain not enforceable; "
               "the panel stays on direct hardware access.\n");
        return;
    }
    arch_enter_user(clock_umode_body,
                    (uintptr_t)g_clock_region.f.stack + CLOCK_USTACK_SIZE, 0, 0, 0);
}

/* Phase 17b's own "Verify" deliverable, and the thing phase 17's C7 asked for
 * and could not have: does the real clock domain (stack+state, .clocktext,
 * SIO, ADC, TIMER0-read-only) actually confine the task? Modeled directly on
 * drivers/st7735_rp2350.c's st7735_isolation_test() -- same idea (a deliberate
 * out-of-domain store, asserted to fault), a separate canary rather than
 * reaching into another file's, for the same reason the syscall stubs above
 * are hand-rolled per file. */
static volatile uintptr_t g_clock_canary = 0xC0FFEE;

CLOCK_UATTR static void clock_intruder(void) {
    g_clock_canary = 0xDEAD;
    for (;;) { } /* only reached if the store was NOT stopped */
}

static volatile bool g_clock_intruder_entered;

/* `arg` is the U-mode stack -- allocated by clock_isolation_test() below, not
 * here, so it can be freed once the task is confirmed DEAD. The real server's
 * own stack is not reused for this: it is inside the state region the running
 * server is using, and a probe must not be able to disturb what it measures. */
static void clock_intruder_task_body(void *arg) {
    uint8_t *ustack = (uint8_t *)arg;
    mem_domain_t dom;
    mem_domain_init(&dom);
    mem_domain_add(&dom, (uintptr_t)ustack, 4096, MEM_R | MEM_W);
    uintptr_t tbase, tsize;
    board_clock_text_region(&tbase, &tsize);
    mem_domain_add(&dom, tbase, tsize, MEM_R | MEM_X);
    /* The exact grants the real clock server runs under -- this is what's on
     * trial, TIMER0's read-only-ness included. */
    mem_domain_add(&dom, SIO_BASE, 4096, MEM_R | MEM_W);
    mem_domain_add(&dom, ADC_BASE, 4096, MEM_R | MEM_W);
    mem_domain_add(&dom, TIMER0_BASE, 4096, MEM_R);

    if (task_set_domain(sched_current_pid(), &dom) != 0) {
        printk("[ClockIso] Refusing to enter U-mode: memory domain not enforceable\n");
        return;
    }
    g_clock_intruder_entered = true;
    arch_enter_user(clock_intruder, (uintptr_t)ustack + 4096, 0, 0, 0);
}

/* Runs the probe to completion and reports what actually happened. Returns
 * false if the task never reached U-mode -- matching every other driver's own
 * isolation-test contract. */
bool clock_isolation_test(uintptr_t *out_canary, bool *out_exited_clean) {
    g_clock_canary = 0xC0FFEE;
    g_clock_intruder_entered = false;

    void *ustack = palloc_pages(1);
    if (!ustack) {
        *out_canary = g_clock_canary;
        *out_exited_clean = true;
        return false;
    }

    int pid = task_create("clock_intruder", clock_intruder_task_body, ustack);
    if (pid < 0) {
        palloc_free(ustack, 1);
        *out_canary = g_clock_canary;
        *out_exited_clean = true;
        return false;
    }
    for (int i = 0; i < 10000 && sched_task_state(pid) != TASK_DEAD; i++) {
        sched_yield();
    }
    long status;
    *out_exited_clean = sched_task_exited_cleanly(pid, &status);
    *out_canary = g_clock_canary;
    palloc_free(ustack, 1);
    return g_clock_intruder_entered;
}

/* Called from kernel/main.c, after sched_init(). Not fatal if it fails:
 * every client call below falls back to direct hardware access whenever the
 * task is not alive, same as every other M4.5 conversion. */
int pico_clock_green_task_start(void) {
    int pid = task_create_sized("clock", clock_task_body, NULL, 1);
    if (pid < 0) {
        printk("[Clock] Could not start the clock task; the appliance stays on direct hardware access.\n");
        return -1;
    }
    if (chan_register_task("clock", pid, g_clock_req, sizeof(g_clock_req),
                           g_clock_resp, sizeof(g_clock_resp)) != 0) {
        printk("[Clock] Could not register the clock channel endpoint; falling back to direct hardware access.\n");
        return -1;
    }
    g_clock_ep = chan_lookup("clock");
    g_clock_task_pid = pid;
    printk("[Clock] Driver running as task #%d, reachable via chan_call(\"clock\", ...)\n", pid);
    return pid;
}

/* ============================================ the client side ===========
 *
 * Everything below runs in whatever task called it -- the shell/Lisp task for
 * `(clock)` and the diagnostics -- and never inside the clock task. It may
 * use the console, the kernel clock and other drivers' facades freely; the
 * half above may use none of them.
 */

/* One request, one reply, or -1. Every call below is short and idempotent
 * except 'P' (which blocks for the length of a beep), so a failed call is
 * simply a dropped frame rather than something to retry: the next frame is
 * 8 ms away. read_light is the exception and retries, because a diagnostic
 * that returns a wrong number is worse than one that takes longer. */
static int clock_call(const uint8_t *req, uint32_t len, uint8_t *resp, uint32_t cap) {
    if (!clock_task_alive()) return -1;
    int n = chan_call(g_clock_ep, req, len, resp, cap);
    /* Counted here, on the client side, and not by the server: a U-mode task
     * cannot touch g_clock_calls, an ordinary kernel .bss global no domain
     * grants it. Same reasoning as st7735_call_with_retry()'s own counter. */
    if (n >= 0) g_clock_calls++;
    return n;
}

unsigned clock_hw_scan_frame(clock_event_t *ev, unsigned max) {
    uint8_t req[1] = { CLOCK_OP_SCAN_FRAME };
    uint8_t resp[CLOCK_RESP_CAP];
    unsigned n = 0;

    int r = clock_call(req, sizeof(req), resp, sizeof(resp));
    if (r >= 1) {
        n = resp[0];
        if (n > BTN_RING) n = BTN_RING;
        if ((uint32_t)r < 1u + 2u * n) n = (unsigned)((r - 1) / 2);
        if (n > max) n = max;
        for (unsigned i = 0; i < n; i++) {
            uint8_t e = resp[1 + i * 2];
            ev[i].key     = (clock_key_t)(e & 0x03u);
            ev[i].press   = (clock_press_t)((e >> 2) & 0x03u);
            ev[i].held_ms = (unsigned)resp[2 + i * 2] * 16u;
        }
        return n;
    }

    /* Fallback: the task is not alive, so this caller owns the hardware. */
    {
        uint8_t raw[2 * BTN_RING];
        n = pico_clock_green_hw_scan_frame(raw, BTN_RING);
        if (n > max) n = max;
        for (unsigned i = 0; i < n; i++) {
            ev[i].key     = (clock_key_t)(raw[i * 2] & 0x03u);
            ev[i].press   = (clock_press_t)((raw[i * 2] >> 2) & 0x03u);
            ev[i].held_ms = (unsigned)raw[i * 2 + 1] * 16u;
        }
    }
    return n;
}

void clock_hw_clear(void) {
    uint8_t req[1] = { CLOCK_OP_CLEAR };
    uint8_t resp[CLOCK_RESP_CAP];
    if (clock_call(req, sizeof(req), resp, sizeof(resp)) >= 0) return;
    pico_clock_green_clear();
}

void clock_hw_blank(void) {
    uint8_t req[1] = { CLOCK_OP_BLANK };
    uint8_t resp[CLOCK_RESP_CAP];
    if (clock_call(req, sizeof(req), resp, sizeof(resp)) >= 0) return;
    pico_clock_green_hw_blank();
}

void clock_hw_show_time(unsigned h, unsigned m, bool colon) {
    uint8_t req[4] = { CLOCK_OP_SHOW_TIME, (uint8_t)h, (uint8_t)m, (uint8_t)(colon ? 1 : 0) };
    uint8_t resp[CLOCK_RESP_CAP];
    if (clock_call(req, sizeof(req), resp, sizeof(resp)) >= 0) return;
    pico_clock_green_show_time(h, m, colon);
}

void clock_hw_show_temperature_c(int c) {
    if (c > 99) c = 99;
    if (c < -99) c = -99;
    uint8_t req[2] = { CLOCK_OP_SHOW_TEMP, (uint8_t)(int8_t)c };
    uint8_t resp[CLOCK_RESP_CAP];
    if (clock_call(req, sizeof(req), resp, sizeof(resp)) >= 0) return;
    pico_clock_green_show_temperature_c(c);
}

void clock_hw_draw_text(const char *s) {
    if (!s) return;
    uint8_t req[CLOCK_REQ_CAP];
    uint8_t resp[CLOCK_RESP_CAP];
    req[0] = CLOCK_OP_TEXT;
    uint32_t n = 0;
    while (s[n] && n < CLOCK_REQ_CAP - 1u) { req[1 + n] = (uint8_t)s[n]; n++; }
    if (clock_call(req, 1 + n, resp, sizeof(resp)) >= 0) return;
    draw_text(s);
}

void clock_hw_draw_text_at(int col, const char *s) {
    if (!s) return;
    if (col > 127) col = 127;
    if (col < -128) col = -128;
    uint8_t req[CLOCK_REQ_CAP];
    uint8_t resp[CLOCK_RESP_CAP];
    req[0] = CLOCK_OP_TEXT_AT;
    req[1] = (uint8_t)(int8_t)col;
    uint32_t n = 0;
    while (s[n] && n < CLOCK_REQ_CAP - 2u) { req[2 + n] = (uint8_t)s[n]; n++; }
    if (clock_call(req, 2 + n, resp, sizeof(resp)) >= 0) return;
    pico_clock_green_hw_draw_text_at(col, s);
}

void clock_hw_draw_bars(const uint8_t *scores, unsigned n) {
    if (!scores) n = 0;
    if (n > CLOCK_REQ_CAP - 1u) { scores += n - (CLOCK_REQ_CAP - 1u); n = CLOCK_REQ_CAP - 1u; }
    uint8_t req[CLOCK_REQ_CAP];
    uint8_t resp[CLOCK_RESP_CAP];
    req[0] = CLOCK_OP_BARS;
    for (unsigned i = 0; i < n; i++) req[1 + i] = scores[i];
    if (clock_call(req, 1 + n, resp, sizeof(resp)) >= 0) return;
    pico_clock_green_hw_draw_bars(scores, n);
}

void clock_hw_set_weekday(unsigned dow) {
    uint8_t req[2] = { CLOCK_OP_WEEKDAY, (uint8_t)dow };
    uint8_t resp[CLOCK_RESP_CAP];
    if (clock_call(req, sizeof(req), resp, sizeof(resp)) >= 0) return;
    pico_clock_green_hw_set_weekday(dow);
}

void clock_hw_indicator(clock_indicator_t ind, bool on) {
    uint8_t req[3] = { CLOCK_OP_INDICATOR, (uint8_t)ind, (uint8_t)(on ? 1 : 0) };
    uint8_t resp[CLOCK_RESP_CAP];
    if (clock_call(req, sizeof(req), resp, sizeof(resp)) >= 0) return;
    pico_clock_green_hw_indicator(ind, on);
}

void clock_hw_set_brightness(int level) {
    uint8_t req[2] = { CLOCK_OP_BRIGHTNESS, (uint8_t)(int8_t)level };
    uint8_t resp[CLOCK_RESP_CAP];
    if (clock_call(req, sizeof(req), resp, sizeof(resp)) >= 0) return;
    pico_clock_green_hw_set_brightness(level);
}

void clock_hw_beep(unsigned ms) {
    uint8_t req[3] = { CLOCK_OP_BEEP, (uint8_t)(ms >> 8), (uint8_t)ms };
    uint8_t resp[CLOCK_RESP_CAP];
    if (clock_call(req, sizeof(req), resp, sizeof(resp)) >= 0) return;
    pico_clock_green_hw_beep(ms);
}

uint8_t clock_hw_pin_levels(void) {
    uint8_t req[1] = { CLOCK_OP_PIN_LEVELS };
    uint8_t resp[CLOCK_RESP_CAP];
    if (clock_call(req, sizeof(req), resp, sizeof(resp)) >= 1) return resp[0];
    return pico_clock_green_hw_pin_levels();
}

/* Public facades, for callers outside the appliance loop. */
void pico_clock_green_set_weekday(unsigned dow)                { clock_hw_set_weekday(dow); }
void pico_clock_green_indicator(clock_indicator_t ind, bool on) { clock_hw_indicator(ind, on); }

void pico_clock_green_run(void) {
    /* Straight into the appliance loop, in this caller's own task. Under
     * M4.5 this was a chan_call() that blocked for the whole session; the
     * loop is on this side of the wire now, which is the phase-17b change in
     * one line. */
    clock_app_run();
}

uint16_t pico_clock_green_read_light(void) {
    uint8_t req[1] = { CLOCK_OP_READ_LIGHT };
    uint8_t resp[CLOCK_RESP_CAP];
    if (clock_task_alive()) {
        for (int attempt = 0; attempt < 8; attempt++) {
            int n = clock_call(req, sizeof(req), resp, sizeof(resp));
            if (n >= 2) return ((uint16_t)resp[0] << 8) | resp[1];
            if (n >= 0) break; /* short reply -- treat as failure, fall through */
            sched_yield();
        }
    }
    return pico_clock_green_hw_read_light();
}

/* ------------------------------------------------- C1 diagnostics ------
 *
 * All four of these used to run inside the clock task, which is why all four
 * used to be `_hw_` functions. They print, and printing is the one thing the
 * task will not be able to do from S2 onwards -- so they are ordinary client
 * loops now: hold a frame, look at what came back, say something about it.
 * Each still owns the display for its duration, by the simple fact that
 * nothing else is calling the frame op meanwhile.
 */

static const char *const KEY_NAME[CLOCK_KEY_COUNT] = { "SET", "UP", "DOWN" };
static const char *const PRESS_NAME[3] = { "short", "LONG", "repeat" };

void pico_clock_green_keys(unsigned secs) {
    if (secs == 0)  secs = 30;
    if (secs > 600) secs = 600;

    console_interrupt_clear();

    cprintf("\nButton test on GP%d (SET), GP%d (UP), GP%d (DOWN) for %u s.\n",
            BTN_SET_PIN, BTN_UP_PIN, BTN_DOWN_PIN, secs);
    cprintf("Expect: short < %u ms, LONG >= %u ms (both reported on release),\n"
            "and UP/DOWN repeating %u/s after holding %u ms. Ctrl-C stops.\n\n",
            (unsigned)BTN_LONG_MS, (unsigned)BTN_LONG_MS,
            1000u / (unsigned)BTN_REPEAT_PERIOD_MS, (unsigned)BTN_REPEAT_DELAY_MS);

    /* The idle levels, before anything is touched. All three should read 1
     * (released, pulled up); a 0 here is a stuck button or a pull-up that is
     * not doing its job, and no amount of event watching will say so as
     * plainly. */
    {
        uint8_t lv = clock_hw_pin_levels();
        cprintf("Idle pin levels: SET=%u UP=%u DOWN=%u  (1 = released; all three\n"
                "should be 1 with nothing pressed -- these are active-low inputs\n"
                "with the internal pull-up on).\n\n",
                lv & 1u, (lv >> 1) & 1u, (lv >> 2) & 1u);
    }

    uint64_t t0 = time_get_ms();
    uint64_t end = t0 + (uint64_t)secs * 1000u;
    unsigned n_events = 0;

    while (time_get_ms() < end) {
        clock_event_t ev[CLOCK_EVENTS_MAX];
        unsigned n = clock_hw_scan_frame(ev, CLOCK_EVENTS_MAX);
        uint64_t now = time_get_ms();

        for (unsigned i = 0; i < n; i++) {
            cprintf("  t=%3u.%03u  %-4s %-6s", (unsigned)((now - t0) / 1000),
                    (unsigned)((now - t0) % 1000), KEY_NAME[ev[i].key],
                    PRESS_NAME[ev[i].press]);
            /* Rounded to 16 ms by the wire, and said so rather than printing
             * a precision the number does not have. */
            if (ev[i].held_ms) cprintf("  held ~%u ms", ev[i].held_ms);
            cprintf("\n");
            n_events++;
        }
        if (console_interrupt_requested()) { console_interrupt_clear(); break; }
    }

    cprintf("\n%u events.\n", n_events);
    if (n_events == 0) {
        cprintf("Nothing at all. Check the idle levels above: if they are already 0,\n"
                "the pin is not the button; if they are 1 and stay 1 while you press,\n"
                "the button is not wired to that GPIO.\n");
    }

    clock_hw_blank();
}

/* Hold whatever is in the frame buffer on the display for `ms`, scanning.
 * Returns false if Ctrl-C asked to stop. The shape every diagnostic below
 * needs, and the client-side replacement for the old in-task
 * `while (now < until) { scan_step(); delay(); }`. */
static bool hold_frames(unsigned ms) {
    uint64_t until = time_get_ms() + ms;
    while (time_get_ms() < until) {
        clock_event_t ev[CLOCK_EVENTS_MAX];
        (void)clock_hw_scan_frame(ev, CLOCK_EVENTS_MAX);
        if (console_interrupt_requested()) { console_interrupt_clear(); return false; }
    }
    return true;
}

void pico_clock_green_led_walk(void) {
    static const char *const DAY_NAME[8] = { "none", "Monday", "Tuesday",
        "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
    static const char *const IND_NAME[CLOCK_IND_COUNT] = {
        "DCF (vendor MoveOn)", "ALARM", "COUNTDOWN", "F", "C",
        "AM", "PM", "COUNTUP", "CHIME", "AUTOLIGHT" };

    console_interrupt_clear();
    cprintf("\nLED walk: each weekday LED, then each indicator LED, ~1 s apart.\n"
            "Ctrl-C stops. The digits stay blank so only the LED moves.\n\n");

    clock_hw_clear();

    for (unsigned d = 1; d <= 7; d++) {
        cprintf("  weekday %u = %s\n", d, DAY_NAME[d]);
        clock_hw_set_weekday(d);
        if (!hold_frames(1000u)) goto done;
    }
    clock_hw_set_weekday(0);

    for (unsigned i = 0; i < CLOCK_IND_COUNT; i++) {
        cprintf("  indicator %u = %s\n", i, IND_NAME[i]);
        clock_hw_indicator((clock_indicator_t)i, true);
        bool ok = hold_frames(1000u);
        clock_hw_indicator((clock_indicator_t)i, false);
        if (!ok) goto done;
    }

    cprintf("  all of them at once\n");
    for (unsigned i = 0; i < CLOCK_IND_COUNT; i++) clock_hw_indicator((clock_indicator_t)i, true);
    clock_hw_set_weekday(7);
    (void)hold_frames(2000u);

done:
    for (unsigned i = 0; i < CLOCK_IND_COUNT; i++) clock_hw_indicator((clock_indicator_t)i, false);
    clock_hw_set_weekday(0);
    clock_hw_blank();
    cprintf("\nDone; display blanked.\n");
}

/* Scroll `s` right-to-left once, at 45 ms a column. Returns false if Ctrl-C
 * stopped it. Keypresses are read and discarded -- this is a console
 * diagnostic, not a menu; the appliance's own scroll (which stops on a
 * keypress and keeps the event) is drivers/pico_clock_app.c's. */
#define SCROLL_STEP_MS 45u

static bool scroll_text_once(const char *s) {
    unsigned w = clock_font_text_width(s);
    for (int dest = (int)TEXT_COL_LAST + 1; dest > (int)TEXT_COL_FIRST - (int)w; dest--) {
        clock_hw_draw_text_at(dest, s);
        if (!hold_frames(SCROLL_STEP_MS)) return false;
    }
    return true;
}

void pico_clock_green_show_text(const char *str, unsigned secs) {
    if (!str) return;
    if (secs == 0)  secs = 5;
    if (secs > 300) secs = 300;

    console_interrupt_clear();
    clock_hw_clear();

    unsigned w = clock_font_text_width(str);
    unsigned avail = TEXT_COL_LAST - TEXT_COL_FIRST + 1u;
    cprintf("\n\"%s\": %u columns, %u available -- %s for %u s. Ctrl-C stops.\n",
            str, w, avail, (w <= avail) ? "static" : "scrolling", secs);

    uint64_t end = time_get_ms() + (uint64_t)secs * 1000u;
    if (w <= avail) {
        clock_hw_draw_text(str);
        while (time_get_ms() < end) {
            if (!hold_frames(200u)) break;
        }
    } else {
        while (time_get_ms() < end) {
            if (!scroll_text_once(str)) break;
        }
    }

    clock_hw_blank();
}

#if CONFIG_ENABLE_DCF77
/* `(dcf-monitor [secs])`: the SIG screen without the menu, and with a console
 * commentary the panel has no room for. The same 24-column bar chart the menu
 * item shows -- one column per second, newest on the right -- because the
 * point of this one is to be watched while an antenna is moved, and the
 * person doing the moving is usually not the person at the keyboard.
 *
 * Runs with the display ON by definition: it *is* the display. That makes it
 * the instrument for the interference measurement in phase 17's section 3 --
 * compare a minute of it against the same minute with `dark`, and the
 * difference is the display's contribution.
 *
 * It is also, since phase 17b, the instrument for that phase's own open
 * question: with the display on, the decoder is now fed once per frame
 * (~125 Hz) instead of once per row (~1 kHz), because the caller is blocked
 * in the frame op in between. That is inside dcf77_service_feed()'s stated
 * "50 Hz or better", and the way to find out whether stated is good enough is
 * to run this before and after and compare the summary -- which is what it
 * was built to make possible. The `dark` half is unaffected: nothing is
 * scanning there, so it still feeds at ~1 kHz. */
void pico_clock_green_dcf_monitor(unsigned secs, bool dark) {
    if (secs == 0)  secs = 120;
    if (secs > 3600) secs = 3600;

    console_interrupt_clear();
    dcf77_service_init();
    clock_hw_clear();

    if (dark) {
        /* The controlled half of the interference measurement (D5). The panel
         * is physically blanked -- zeros latched, OE closed, and crucially the
         * row scan not run at all, so there is no shift clock and no switching
         * current. Everything else about the run is identical: same decoder,
         * same scoring, same summary. */
        pico_clock_green_static_load(false);
        clock_hw_blank();
        cprintf("\nDCF-77 signal monitor for %u s, display DARK (no scanning,\n"
                "no shift clock, no switching current). Ctrl-C stops.\n\n", secs);
    } else {
        cprintf("\nDCF-77 signal monitor for %u s, on the panel and here.\n"
                "One column per second, newest on the right, height 0-7. The DCF\n"
                "indicator LED follows the second pulse. Ctrl-C stops.\n\n", secs);
    }

    uint64_t t0 = time_get_ms();
    uint64_t end = t0 + (uint64_t)secs * 1000u;
    uint64_t next_report = t0 + 10000;
    bool led = false;
    uint8_t  last_bars[32];
    unsigned last_bars_n = 0xFFFFu;   /* nothing drawn yet: first look redraws */

    while (time_get_ms() < end) {
        uint64_t now = time_get_ms();
        dcf77_service_feed(now);

        dcf_status_t st;
        dcf77_service_status(&st);

        if (!dark) {
            if (st.pulse != led) {
                led = st.pulse;
                clock_hw_indicator(CLOCK_IND_DCF, led);
            }
            /* When the bars actually change, not on a timer. A timer was the
             * first version of this and it was wrong in a way worth
             * recording: the scores change once a second, so quantising the
             * redraw to 250 ms made the *visible* shift land 750, 1000 or
             * 1250 ms after the last one depending on where the loop's phase
             * happened to sit -- an even second's worth of data displayed at
             * an uneven rate, reported from hardware as "mostly one update a
             * second, sometimes a small burst" (user, 2026-08-24). Comparing
             * against what was last drawn costs 24 bytes and a memcmp, redraws
             * the instant the decoder has something new, and sends nothing
             * across the wire when it does not. */
            unsigned qn = st.decoder.quality_count;
            if (qn > sizeof(last_bars)) qn = sizeof(last_bars);
            if (qn != last_bars_n || memcmp(st.decoder.quality, last_bars, qn) != 0) {
                clock_hw_draw_bars(st.decoder.quality, st.decoder.quality_count);
                last_bars_n = qn;
                for (unsigned i = 0; i < qn; i++) last_bars[i] = st.decoder.quality[i];
            }
        }

        if (now >= next_report) {
            cprintf("  [%3u s] pulses=%u (%u bad)  glitches=%u  losses=%u  frames %u/%u  q=",
                    (unsigned)((now - t0) / 1000), (unsigned)st.decoder.pulses_seen,
                    (unsigned)st.decoder.pulses_bad, (unsigned)st.decoder.glitches,
                    (unsigned)st.decoder.sync_losses,
                    (unsigned)st.decoder.frames_accepted, (unsigned)st.decoder.frames_seen);
            for (unsigned i = 0; i < st.decoder.quality_count; i++)
                cprintf("%c", '0' + (st.decoder.quality[i] > 7 ? 7 : st.decoder.quality[i]));
            cprintf("\n");
            next_report = now + 10000;
        }

        if (dark) {
            /* Same ~1 kHz feed cadence the pre-split monitor had, and the
             * reason this branch keeps time_delay_us(): with nothing being
             * scanned there is no frame op to pace the loop. */
            time_delay_us(CLOCK_ROW_PERIOD_US);
        } else {
            clock_event_t ev[CLOCK_EVENTS_MAX];
            (void)clock_hw_scan_frame(ev, CLOCK_EVENTS_MAX);
        }
        if (console_interrupt_requested()) { console_interrupt_clear(); break; }
    }
    /* The summary exists so two runs can be compared by reading one line. The
     * mean score is the figure that matters: pulse counts saturate (a signal
     * either arrives once a second or it does not), while the mean moves
     * smoothly with reception because the score does. */
    {
        dcf_status_t st;
        dcf77_service_status(&st);
        uint64_t elapsed = time_get_ms() - t0;
        unsigned el_s = (unsigned)(elapsed / 1000u);
        if (el_s == 0) el_s = 1;

        unsigned mean10 = st.decoder.quality_total
            ? (unsigned)((st.decoder.quality_sum * 10u) / st.decoder.quality_total) : 0;

        cprintf("\n--- %u s, display %s ---\n", el_s, dark ? "DARK" : "RUNNING");
        /* Sync losses first, and frames beside them, because those are what
         * decide whether a clock can ever set itself. The mean below is a
         * measure of *typical* pulse health and can sit almost unchanged
         * while every frame in five minutes is destroyed -- which is exactly
         * what the panel-on measurement of 2026-08-23 did. A rare fatal event
         * does not move a mean. Frame assembly needs 59 CONSECUTIVE clean
         * seconds, so one bad second in twelve is total failure, not an 8%
         * degradation. */
        cprintf("  sync losses   : %u          <-- compare these two first\n",
                (unsigned)st.decoder.sync_losses);
        cprintf("  frames        : %u seen, %u accepted\n",
                (unsigned)st.decoder.frames_seen, (unsigned)st.decoder.frames_accepted);
        /* A frame is 59 CONSECUTIVE seconds. A run of 58 is worth exactly as
         * much as a run of 3, which is why this is here and not just a loss
         * count -- 6 losses in 300 seconds reads like 98% success and can
         * still mean no frame ever completes. */
        cprintf("  longest run   : %u s of %u needed for one frame%s\n",
                (unsigned)st.decoder.clean_run_max, 59u,
                st.decoder.clean_run_max >= 59u ? "" : "   <-- too short");
        if (st.decoder.frames_seen > st.decoder.frames_accepted) {
            cprintf("  frames failed : parity %u, framing %u, range %u, weekday %u\n",
                    (unsigned)st.decoder.parity_errors, (unsigned)st.decoder.framing_errors,
                    (unsigned)st.decoder.range_errors, (unsigned)st.decoder.weekday_errors);
            if (st.decoder.parity_errors == 0 && st.decoder.framing_errors == 0 &&
                st.decoder.range_errors == 0 && st.decoder.weekday_errors == 0) {
                cprintf("                  none of those: the frames decoded, but no two\n"
                        "                  agreed yet. One more good minute finishes it.\n");
            }
        }
        cprintf("  pulses        : %u of ~%u expected (%u a bad width)\n",
                (unsigned)st.decoder.pulses_seen, el_s, (unsigned)st.decoder.pulses_bad);
        if (st.decoder.pulses_seen > el_s + 2u) {
            cprintf("                  ^ MORE than one a second: pulses are being\n"
                    "                    invented, and each one breaks the frame.\n");
        }
        cprintf("  mean quality  : %u.%u / 7   (typical pulse health, not sync health)\n",
                mean10 / 10, mean10 % 10);
        cprintf("  glitches      : %u (%u per minute)\n",
                (unsigned)st.decoder.glitches,
                (unsigned)((uint64_t)st.decoder.glitches * 60u / el_s));
        cprintf("  worst spacing : %u ms off the 1 s grid\n",
                (unsigned)st.decoder.spacing_err_max_ms);
    }

    clock_hw_indicator(CLOCK_IND_DCF, false);
    clock_hw_blank();
}
#endif /* CONFIG_ENABLE_DCF77 */
