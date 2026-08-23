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

/* LDR threshold and dim duty, ported from the vendor firmware's own
 * already-tuned values (repeating_timer_callback_us(), Pico-Clock-Green.c)
 * rather than independently derived -- same physical LDR/divider hardware.
 * Vendor toggles OE open 1-in-3 calls (~33% duty) when adc_light > 2800 out
 * of a 12-bit 0-4095 range; here that's a single software-PWM pulse per row
 * instead of a separate microsecond timer (LugalOS has no generic
 * one-shot IRQ callback a driver can reuse for that -- see the plan doc's
 * L2 section for why this shape was chosen instead). */
#define LDR_DARK_THRESHOLD 2800
#define DIM_ON_TIME_US     330

/* 4 column-groups (32 bits, matching the two cascaded 16-channel SM16106s)
 * x 8 rows. Row 0 of each group is deliberately left blank (0x00) -- the
 * vendor firmware used it for status-indicator LEDs this driver's narrowed
 * scope doesn't implement. Indexed disp_buf[8*group + row], same layout as
 * the vendor's own disp_buf[] so the bit-packing math below is a direct,
 * checkable port of display_char(). */
static uint8_t g_disp_buf[32];

/* The indicator and weekday LEDs share the frame buffer with the digits, but
 * not their lifetime: a digit is redrawn every time the minute changes, and
 * pico_clock_green_clear() wipes the whole buffer to do it. So their state
 * lives here and is re-applied after every clear. The alternative -- teaching
 * clear() which bits to spare -- puts the same knowledge in a worse place.
 *
 * They never collide with a glyph. draw_glyph() writes rows 1-7 only and
 * preserves the low `col` bits of the first group it touches, and every
 * layout in this file starts at column 2, which is exactly why the vendor
 * firmware carries its own +2 disp_offset. */
static uint8_t g_ind_bits[8];   /* columns 0-1 of group 0, one entry per row */
static uint8_t g_weekday;       /* 0 = none lit, else 1 = Monday .. 7 = Sunday */

static uint8_t g_row;
static uint16_t g_dim_on_us; /* 0 = full brightness (no PWM pulse needed) */
/* -1 = the LDR decides, as phase 11 shipped it; 1..7 = a fixed level chosen
 * from the menu, which suspends the LDR entirely (clock_hw_set_brightness). */
static int g_brightness_level = -1;
#if CONFIG_CLOCK_BOOT_BEACON
/* Set by pico_clock_green_init(): before it, the beacon has clicks only. */
static bool g_matrix_pins_up;
#endif

/* Glyphs are 7 bytes (rows 1-7 of an 8-row cell), each byte's low bits are
 * the lit columns of that row, transcribed from ziku.h -- see this file's
 * header comment. */
static const uint8_t GLYPH_DIGIT[10][7] = {
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
static const uint8_t GLYPH_COLON[7] = {0x00, 0x03, 0x03, 0x00, 0x03, 0x03, 0x00};
static const uint8_t GLYPH_DEGC[7]  = {0x01, 0x0C, 0x12, 0x02, 0x02, 0x12, 0x0C};
static const uint8_t GLYPH_MINUS[7] = {0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00};

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

static inline void oe_open(void)  { REG(SIO_GPIO_OUT_CLR) = OE_MASK; } /* active low */
static inline void oe_close(void) { REG(SIO_GPIO_OUT_SET) = OE_MASK; }

static void shift_byte(uint8_t data) {
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

static inline void latch_pulse(void) {
    REG(SIO_GPIO_OUT_SET) = LE_MASK;
    REG(SIO_GPIO_OUT_CLR) = LE_MASK;
}

static void set_row_address(unsigned row) {
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

static uint16_t adc_read_light(void) {
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

static uint16_t pico_clock_green_hw_read_light(void) {
    return adc_read_light();
}

/* Direct port of the vendor's display_char() bit-packing (Pico-Clock-
 * Green.c:575-621), generalized from "char -> glyph lookup -> pack" to
 * "glyph -> pack" since this driver's glyph set is fixed and small enough
 * not to need the vendor's switch-statement lookup. col already includes
 * the vendor's disp_offset. Row 0 of each group (the status-LED row) is
 * never touched, same as upstream. */
static void draw_glyph(unsigned col, const uint8_t glyph[7]) {
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
#define TEXT_COL_FIRST 2u
#define TEXT_COL_LAST  23u

static inline void set_pixel(unsigned col, unsigned row, bool on) {
    if (col > TEXT_COL_LAST || col < TEXT_COL_FIRST || row == 0 || row > 7) return;
    unsigned g = col / 8u, b = col % 8u;
    if (on) g_disp_buf[8u * g + row] |= (uint8_t)(1u << b);
    else    g_disp_buf[8u * g + row] &= (uint8_t)~(1u << b);
}

/* Blit a column-major bitmap at `dest`, which may be negative or past the
 * right edge -- that is what makes scrolling a single expression instead of a
 * special case at each end. Only the text area is touched, so the indicator
 * and weekday LEDs survive untouched and do not need re-applying. */
static void draw_cols(int dest, const uint8_t *cols, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        int col = dest + (int)i;
        if (col < (int)TEXT_COL_FIRST || col > (int)TEXT_COL_LAST) continue;
        for (unsigned r = 0; r < CLOCK_GLYPH_ROWS; r++) {
            set_pixel((unsigned)col, r + 1u, (cols[i] & (1u << r)) != 0);
        }
    }
}

/* Wide enough for the longest label the menu will scroll ("TEMPERATURE" is 65
 * columns) with room over. Static rather than on the stack: this driver runs
 * on a 1 KB task stack. */
#define TEXT_COLS_MAX 96u
static uint8_t g_text_cols[TEXT_COLS_MAX];

/* Clear only the text area, leaving the LEDs alone -- the full clear() wipes
 * everything and then re-applies them, which is right when the whole screen
 * changes and wasteful when only the digits do. */
static void clear_text_area(void) {
    for (unsigned col = TEXT_COL_FIRST; col <= TEXT_COL_LAST; col++) {
        for (unsigned r = 1; r <= CLOCK_GLYPH_ROWS; r++) set_pixel(col, r, false);
    }
}

/* Draw `s` into the text area, centred if it fits and left-aligned if it does
 * not (in which case the tail is simply clipped -- a caller that cares should
 * scroll instead). */
static void draw_text(const char *s) {
    unsigned n = clock_font_render(s, g_text_cols, TEXT_COLS_MAX);
    unsigned avail = TEXT_COL_LAST - TEXT_COL_FIRST + 1u;
    int dest = (int)TEXT_COL_FIRST;
    if (n < avail) dest += (int)((avail - n) / 2u);
    clear_text_area();
    draw_cols(dest, g_text_cols, n);
}

/* Forward declarations: pico_clock_green_init() (boot-time, before the task
 * exists) is the only caller of these outside the task body below. */
static void pico_clock_green_clear(void);
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

    memset(g_ind_bits, 0, sizeof(g_ind_bits));
    g_weekday = 0;
    pico_clock_green_clear();
    g_row = 0;
    g_dim_on_us = 0;
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
static const uint8_t WEEKDAY_G0[8] = { 0, 0x18, 0xC0, 0,    0,    0,    0,    0    };
static const uint8_t WEEKDAY_G1[8] = { 0, 0,    0,    0x06, 0x30, 0x80, 0,    0    };
static const uint8_t WEEKDAY_G2[8] = { 0, 0,    0,    0,    0,    0x01, 0x0C, 0x60 };
#define WEEKDAY_G0_ALL 0xD8u
#define WEEKDAY_G1_ALL 0xB6u
#define WEEKDAY_G2_ALL 0x6Du

/* Columns 0-1 of group 0, one indicator per row -- the vendor's dis_* macros
 * (define.h:107-127). Two of the ten are a single column rather than a pair
 * (C/F share row 3, AM/PM share row 4), which is why this is a table of
 * (row, mask) and not just a row number. The top one is the vendor's
 * "MoveOn", which means nothing here; it is our DCF status LED. */
static const struct { uint8_t row, mask; } IND_BIT[CLOCK_IND_COUNT] = {
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
static void apply_leds(void) {
    for (unsigned row = 0; row < 8; row++) {
        g_disp_buf[row] = (uint8_t)((g_disp_buf[row] & ~0x03u) | g_ind_bits[row]);
    }
    unsigned d = (g_weekday >= 1 && g_weekday <= 7) ? g_weekday : 0;
    g_disp_buf[0]  = (uint8_t)((g_disp_buf[0]  & ~WEEKDAY_G0_ALL) | WEEKDAY_G0[d]);
    g_disp_buf[8]  = (uint8_t)((g_disp_buf[8]  & ~WEEKDAY_G1_ALL) | WEEKDAY_G1[d]);
    g_disp_buf[16] = (uint8_t)((g_disp_buf[16] & ~WEEKDAY_G2_ALL) | WEEKDAY_G2[d]);
}

static void pico_clock_green_hw_set_weekday(unsigned dow) {
    g_weekday = (dow >= 1 && dow <= 7) ? (uint8_t)dow : 0;
    apply_leds();
}

static void pico_clock_green_hw_indicator(clock_indicator_t ind, bool on) {
    if ((unsigned)ind >= CLOCK_IND_COUNT) return;
    uint8_t row = IND_BIT[ind].row, mask = IND_BIT[ind].mask;
    if (on) g_ind_bits[row] |= mask;
    else    g_ind_bits[row] = (uint8_t)(g_ind_bits[row] & ~mask);
    apply_leds();
}

static void pico_clock_green_clear(void) {
    memset(g_disp_buf, 0, sizeof(g_disp_buf));
    apply_leds();
}

static void pico_clock_green_show_time(unsigned hour, unsigned minute, bool colon) {
    if (hour > 23) hour = 23;
    if (minute > 59) minute = 59;

    pico_clock_green_clear();
    draw_glyph(COL_HOUR_TENS, GLYPH_DIGIT[hour / 10]);
    draw_glyph(COL_HOUR_ONES, GLYPH_DIGIT[hour % 10]);
    if (colon) draw_glyph(COL_COLON, GLYPH_COLON);
    draw_glyph(COL_MIN_TENS, GLYPH_DIGIT[minute / 10]);
    draw_glyph(COL_MIN_ONES, GLYPH_DIGIT[minute % 10]);
}

static void pico_clock_green_show_temperature_c(int temp_c) {
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

static const uint8_t BTN_PIN[CLOCK_KEY_COUNT] = {
    [CLOCK_KEY_SET]  = BTN_SET_PIN,
    [CLOCK_KEY_UP]   = BTN_UP_PIN,
    [CLOCK_KEY_DOWN] = BTN_DOWN_PIN,
};

typedef struct {
    bool     level;          /* debounced: true = pressed */
    bool     cand;           /* what the pin last read */
    uint64_t cand_since_ms;
    uint64_t pressed_at_ms;
    uint64_t next_repeat_ms; /* 0 = not repeating */
} btn_t;

static btn_t g_btn[CLOCK_KEY_COUNT];

/* An 8-entry ring. Deep enough that nothing a human can do overflows it at a
 * 1 ms drain, and when it does overflow the NEWEST event is dropped: the
 * queue then still describes a prefix of what happened, in order, rather than
 * a gap in the middle of it. The drop count is kept because a silent drop in
 * an input path is the kind of bug that gets blamed on the hardware. */
#define BTN_RING 8u
static uint8_t  g_key_ring[BTN_RING];
static unsigned g_key_head, g_key_tail;
static uint32_t g_key_drops;

static void key_push(clock_key_t key, clock_press_t press) {
    unsigned next = (g_key_head + 1u) % BTN_RING;
    if (next == g_key_tail) { g_key_drops++; return; }
    g_key_ring[g_key_head] = (uint8_t)(((unsigned)press << 2) | (unsigned)key);
    g_key_head = next;
}

static bool key_pop(clock_key_t *key, clock_press_t *press) {
    if (g_key_tail == g_key_head) return false;
    uint8_t e = g_key_ring[g_key_tail];
    g_key_tail = (g_key_tail + 1u) % BTN_RING;
    if (key)   *key   = (clock_key_t)(e & 0x03u);
    if (press) *press = (clock_press_t)((e >> 2) & 0x03u);
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

static void buttons_poll(uint64_t now_ms) {
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
                             held >= BTN_LONG_MS ? CLOCK_PRESS_LONG : CLOCK_PRESS_SHORT);
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
                    key_push((clock_key_t)i, CLOCK_PRESS_REPEAT);
                    b->next_repeat_ms = now_ms + BTN_REPEAT_PERIOD_MS;
                }
            } else if (now_ms >= b->next_repeat_ms) {
                key_push((clock_key_t)i, CLOCK_PRESS_REPEAT);
                b->next_repeat_ms = now_ms + BTN_REPEAT_PERIOD_MS;
            }
        }
    }
}

static void pico_clock_green_scan_step(void) {
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
        g_dim_on_us = (adc_read_light() > LDR_DARK_THRESHOLD) ? DIM_ON_TIME_US : 0;
    }

    if (g_dim_on_us == 0) {
        oe_open(); /* left on until the next scan_step() call overwrites it */
    } else {
        oe_open();
        time_delay_us(g_dim_on_us);
        oe_close();
    }

    g_row = (g_row + 1) & 7;
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
 * this, read that, one row of scan. No policy.
 */
#define CLOCK_ROW_PERIOD_US   1000

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

void clock_hw_scan_step(void)                 { pico_clock_green_scan_step(); }
void clock_hw_clear(void)                     { pico_clock_green_clear(); }
void clock_hw_show_temperature_c(int c)       { pico_clock_green_show_temperature_c(c); }
void clock_hw_draw_text(const char *s)        { draw_text(s); }
void clock_hw_set_weekday(unsigned dow)       { pico_clock_green_hw_set_weekday(dow); }
void clock_hw_indicator(clock_indicator_t i, bool on) { pico_clock_green_hw_indicator(i, on); }
void clock_hw_buttons_poll(uint64_t now)      { buttons_poll(now); }
bool clock_hw_key_pop(clock_key_t *k, clock_press_t *p) { return key_pop(k, p); }

void clock_hw_show_time(unsigned h, unsigned m, bool colon) {
    pico_clock_green_show_time(h, m, colon);
}

void clock_hw_blank(void) {
    /* Physically, not just in the buffer: nothing calls scan_step() once the
     * appliance loop returns, so a lit row would otherwise stay lit with
     * whatever it last shifted out rather than actually going dark. */
    pico_clock_green_clear();
    oe_close();
}

/* Automatic brightness is the LDR deciding once a frame, exactly as phase 11
 * shipped it. A fixed level bypasses that with a software-PWM on-time: the
 * row period is 1000 us, so level 7 means "never chop" and each step below
 * takes off about a seventh. Not a perceptual curve -- LED brightness is not
 * linear in duty cycle and seven steps do not deserve a lookup table -- just
 * seven distinguishable settings. */
void clock_hw_set_brightness(int level) {
    if (level >= 1 && level <= 7) {
        g_brightness_level = level;
        g_dim_on_us = (level >= 7) ? 0 : (uint16_t)(level * 140);
    } else {
        g_brightness_level = -1;   /* back to the LDR */
    }
}

/* Held for `ms` with the row scan still running, so a beep never blanks the
 * panel. Active HIGH, per the vendor firmware (gpio_put(BUZZ,1) is on). */
void clock_hw_beep(unsigned ms) {
#ifdef CONFIG_CLOCK_BUZZER_GPIO
    if (ms == 0) return;
    REG(SIO_GPIO_OUT_SET) = (1u << CONFIG_CLOCK_BUZZER_GPIO);
    uint64_t until = time_get_ms() + ms;
    while (time_get_ms() < until) {
        pico_clock_green_scan_step();
        time_delay_us(CLOCK_ROW_PERIOD_US);
    }
    REG(SIO_GPIO_OUT_CLR) = (1u << CONFIG_CLOCK_BUZZER_GPIO);
#else
    (void)ms;
#endif
}

/* Scroll `s` right-to-left once, driving the row scan meanwhile. Returns
 * false only if Ctrl-C interrupted it.
 *
 * 45 ms/column, and **abortable by a keypress**. Both of those are corrections
 * from the first hardware run. "TEMPERATURE" is 55 columns, which at the
 * vendor's 60 ms/column is 77 steps -- 4.6 seconds -- and the first version
 * did not poll the buttons while it ran, so every press during those seconds
 * was not merely ignored but never sampled at all. Backing out of the
 * TEMPERATURE item therefore looked like SET-long doing nothing: it had
 * worked, and the menu was then deaf for the length of an animation nobody
 * asked to watch twice.
 *
 * So the buttons are polled on the same cadence as everywhere else, and the
 * first event ends the scroll immediately -- left in the queue, so the press
 * still does whatever it was going to do. Pressing a button to skip an
 * animation is the behaviour everyone already expects. */
#define SCROLL_STEP_MS 45u

static bool scroll_text(const char *s) {
    unsigned n = clock_font_render(s, g_text_cols, TEXT_COLS_MAX);
    for (int dest = (int)TEXT_COL_LAST + 1; dest > (int)TEXT_COL_FIRST - (int)n; dest--) {
        clear_text_area();
        draw_cols(dest, g_text_cols, n);
        uint64_t until = time_get_ms() + SCROLL_STEP_MS;
        while (time_get_ms() < until) {
            uint64_t now = time_get_ms();
            buttons_poll(now);
            if (g_key_head != g_key_tail) return true;   /* skipped; event kept */
            pico_clock_green_scan_step();
            if (console_interrupt_requested()) { console_interrupt_clear(); return false; }
            time_delay_us(CLOCK_ROW_PERIOD_US);
        }
    }
    return true;
}

bool clock_hw_scroll_text(const char *s) { return scroll_text(s); }

/* One column per second, newest on the right, height from the score. The
 * panel is 22 text columns wide and the decoder keeps 24 scores, so the
 * oldest two fall off the left -- which is the right end to lose, and is why
 * the newest is pinned to the right edge rather than the run being centred.
 *
 * A score of 0 (nothing heard that second) draws nothing at all rather than a
 * one-pixel stub: an empty column is the most legible thing on this display
 * and "the signal dropped here" is exactly what someone moving an antenna
 * needs to see. */
void clock_hw_draw_bars(const uint8_t *scores, unsigned n) {
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

/* ------------------------------------------------- C1 diagnostics ------ */

/* Both of these own the display while they run, exactly like
 * pico_clock_green_hw_run() does, and neither is reachable while it is
 * running -- the caller blocks for the duration. That is what lets them touch
 * g_disp_buf and the button state directly. */

static const char *const KEY_NAME[CLOCK_KEY_COUNT] = { "SET", "UP", "DOWN" };
static const char *const PRESS_NAME[3] = { "short", "LONG", "repeat" };

static void pico_clock_green_hw_keys(unsigned secs) {
    if (secs == 0)  secs = 30;
    if (secs > 600) secs = 600;

    console_interrupt_clear();
    buttons_init();

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
        uint32_t in = REG(SIO_GPIO_IN);
        cprintf("Idle pin levels: SET=%u UP=%u DOWN=%u  (1 = released; all three\n"
                "should be 1 with nothing pressed -- these are active-low inputs\n"
                "with the internal pull-up on).\n\n",
                (in >> BTN_SET_PIN) & 1u, (in >> BTN_UP_PIN) & 1u, (in >> BTN_DOWN_PIN) & 1u);
    }

    uint64_t t0 = time_get_ms();
    uint64_t end = t0 + (uint64_t)secs * 1000u;
    uint64_t press_started[CLOCK_KEY_COUNT] = { 0, 0, 0 };
    unsigned n_events = 0;

    while (time_get_ms() < end) {
        uint64_t now = time_get_ms();
        buttons_poll(now);

        for (unsigned i = 0; i < CLOCK_KEY_COUNT; i++) {
            if (g_btn[i].level && press_started[i] == 0) press_started[i] = g_btn[i].pressed_at_ms;
        }

        clock_key_t k;
        clock_press_t pr;
        while (key_pop(&k, &pr)) {
            uint64_t held = (press_started[k] && pr != CLOCK_PRESS_REPEAT)
                              ? (now - press_started[k]) : 0;
            cprintf("  t=%3u.%03u  %-4s %-6s", (unsigned)((now - t0) / 1000),
                    (unsigned)((now - t0) % 1000), KEY_NAME[k], PRESS_NAME[pr]);
            if (held) cprintf("  held %u ms", (unsigned)held);
            cprintf("\n");
            if (pr != CLOCK_PRESS_REPEAT) press_started[k] = 0;
            n_events++;
        }

        pico_clock_green_scan_step();
        if (console_interrupt_requested()) { console_interrupt_clear(); break; }
        time_delay_us(CLOCK_ROW_PERIOD_US);
    }

    cprintf("\n%u events", n_events);
    if (g_key_drops) cprintf(", %u dropped (ring overflow -- nothing is draining)", (unsigned)g_key_drops);
    cprintf(".\n");
    if (n_events == 0) {
        cprintf("Nothing at all. Check the idle levels above: if they are already 0,\n"
                "the pin is not the button; if they are 1 and stay 1 while you press,\n"
                "the button is not wired to that GPIO.\n");
    }

    pico_clock_green_clear();
    oe_close();
}

static void pico_clock_green_hw_led_walk(void) {
    static const char *const DAY_NAME[8] = { "none", "Monday", "Tuesday",
        "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
    static const char *const IND_NAME[CLOCK_IND_COUNT] = {
        "DCF (vendor MoveOn)", "ALARM", "COUNTDOWN", "F", "C",
        "AM", "PM", "COUNTUP", "CHIME", "AUTOLIGHT" };

    console_interrupt_clear();
    cprintf("\nLED walk: each weekday LED, then each indicator LED, ~1 s apart.\n"
            "Ctrl-C stops. The digits stay blank so only the LED moves.\n\n");

    pico_clock_green_clear();

    /* One helper, used for every step: hold the current buffer on the display
     * for a second while keeping the row scan running, since these LEDs are
     * multiplexed like everything else and are invisible without it. */
    #define LED_WALK_HOLD_MS 1000u
    for (unsigned d = 1; d <= 7; d++) {
        cprintf("  weekday %u = %s\n", d, DAY_NAME[d]);
        pico_clock_green_hw_set_weekday(d);
        uint64_t until = time_get_ms() + LED_WALK_HOLD_MS;
        while (time_get_ms() < until) {
            pico_clock_green_scan_step();
            if (console_interrupt_requested()) { console_interrupt_clear(); goto done; }
            time_delay_us(CLOCK_ROW_PERIOD_US);
        }
    }
    pico_clock_green_hw_set_weekday(0);

    for (unsigned i = 0; i < CLOCK_IND_COUNT; i++) {
        cprintf("  indicator %u = %s\n", i, IND_NAME[i]);
        pico_clock_green_hw_indicator((clock_indicator_t)i, true);
        uint64_t until = time_get_ms() + LED_WALK_HOLD_MS;
        while (time_get_ms() < until) {
            pico_clock_green_scan_step();
            if (console_interrupt_requested()) { console_interrupt_clear(); goto done; }
            time_delay_us(CLOCK_ROW_PERIOD_US);
        }
        pico_clock_green_hw_indicator((clock_indicator_t)i, false);
    }

    cprintf("  all of them at once\n");
    for (unsigned i = 0; i < CLOCK_IND_COUNT; i++) pico_clock_green_hw_indicator((clock_indicator_t)i, true);
    pico_clock_green_hw_set_weekday(7);
    {
        uint64_t until = time_get_ms() + 2000u;
        while (time_get_ms() < until) {
            pico_clock_green_scan_step();
            if (console_interrupt_requested()) { console_interrupt_clear(); goto done; }
            time_delay_us(CLOCK_ROW_PERIOD_US);
        }
    }
    #undef LED_WALK_HOLD_MS

done:
    for (unsigned i = 0; i < CLOCK_IND_COUNT; i++) pico_clock_green_hw_indicator((clock_indicator_t)i, false);
    pico_clock_green_hw_set_weekday(0);
    pico_clock_green_clear();
    oe_close();
    cprintf("\nDone; display blanked.\n");
}

static void pico_clock_green_hw_text(const char *str, unsigned secs) {
    if (secs == 0)  secs = 5;
    if (secs > 300) secs = 300;

    console_interrupt_clear();
    pico_clock_green_clear();

    unsigned w = clock_font_text_width(str);
    unsigned avail = TEXT_COL_LAST - TEXT_COL_FIRST + 1u;
    cprintf("\n\"%s\": %u columns, %u available -- %s for %u s. Ctrl-C stops.\n",
            str, w, avail, (w <= avail) ? "static" : "scrolling", secs);

    uint64_t end = time_get_ms() + (uint64_t)secs * 1000u;
    if (w <= avail) {
        draw_text(str);
        while (time_get_ms() < end) {
            pico_clock_green_scan_step();
            if (console_interrupt_requested()) { console_interrupt_clear(); break; }
            time_delay_us(CLOCK_ROW_PERIOD_US);
        }
    } else {
        while (time_get_ms() < end) {
            if (!scroll_text(str)) break;
        }
    }

    pico_clock_green_clear();
    oe_close();
}

#if CONFIG_ENABLE_DCF77
/* `(dcf-monitor [secs])`: the SIG screen without the menu, and with a console
 * commentary the panel has no room for. The same 24-column bar chart the menu
 * item shows -- one column per second, newest on the right -- because the
 * point of this one is to be watched while an antenna is moved, and the
 * person doing the moving is usually not the person at the keyboard.
 *
 * Runs with the display ON by definition: it *is* the display. That makes it
 * the instrument for the interference measurement in the plan's section 3 --
 * compare a minute of it against the same minute of (dcf-raw) with the panel
 * dark, and the difference is the display's contribution. */
static void pico_clock_green_hw_monitor(unsigned secs, bool dark) {
    if (secs == 0)  secs = 120;
    if (secs > 3600) secs = 3600;

    console_interrupt_clear();
    dcf77_service_init();
    pico_clock_green_clear();

    if (dark) {
        /* The controlled half of the interference measurement (D5). The panel
         * is physically blanked -- zeros latched, OE closed, and crucially the
         * row scan not run at all, so there is no shift clock and no switching
         * current. Everything else about the run is identical: same decoder,
         * same scoring, same summary. Comparing two runs of this against each
         * other is a measurement; comparing the monitor against (dcf-raw)
         * would only have compared two different instruments. */
        pico_clock_green_static_load(false);
        oe_close();
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

    while (time_get_ms() < end) {
        uint64_t now = time_get_ms();
        dcf77_service_feed(now);

        dcf_status_t st;
        dcf77_service_status(&st);

        if (!dark) {
            if (st.pulse != led) {
                led = st.pulse;
                pico_clock_green_hw_indicator(CLOCK_IND_DCF, led);
            }
            clock_hw_draw_bars(st.decoder.quality, st.decoder.quality_count);
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

        if (!dark) pico_clock_green_scan_step();
        if (console_interrupt_requested()) { console_interrupt_clear(); break; }
        time_delay_us(CLOCK_ROW_PERIOD_US);
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

    pico_clock_green_hw_indicator(CLOCK_IND_DCF, false);
    pico_clock_green_clear();
    oe_close();
}
#endif /* CONFIG_ENABLE_DCF77 */

/* M4.5, plan/phase12_microkernel_migration.md, Part B: the driver as a task
 * -- see drivers/include/drivers/pico_clock_green.h's comment on
 * pico_clock_green_run() for why the whole appliance loop is one
 * chan_call(), not one per ~1kHz row-scan step. Only two wire ops: nothing
 * else here ever had a caller outside this file.
 *
 *   'R' run        req: [op]  resp: 0 bytes (replied once Ctrl-C breaks the loop)
 *   'L' read_light req: [op]  resp: light(2, uint16_t)
 *   'K' pop_key    req: [op]  resp: event(1) or 0 bytes when the ring is empty
 *   'W' weekday    req: [op, dow]        resp: 0 bytes
 *   'I' indicator  req: [op, ind, on]    resp: 0 bytes
 *   'B' keys test  req: [op, secs_hi, secs_lo]  resp: 0 (blocks like 'R')
 *   'D' led walk   req: [op]             resp: 0 bytes (blocks like 'R')
 *   'T' show text  req: [op, secs, chars...]  resp: 0 (blocks like 'R')
 *   'M' dcf monitor req: [op, secs_hi, secs_lo, dark] resp: 0 (blocks)
 *
 * 'K' is a poll, not a wait: an event only exists if something is running the
 * poller, and a caller that blocked here waiting for one would deadlock
 * against the very loop that produces them. */
#define CLOCK_OP_RUN        ((uint8_t)'R')
#define CLOCK_OP_READ_LIGHT ((uint8_t)'L')
#define CLOCK_OP_POP_KEY    ((uint8_t)'K')
#define CLOCK_OP_WEEKDAY    ((uint8_t)'W')
#define CLOCK_OP_INDICATOR  ((uint8_t)'I')
#define CLOCK_OP_KEYS       ((uint8_t)'B')
#define CLOCK_OP_LED_WALK   ((uint8_t)'D')
#define CLOCK_OP_TEXT       ((uint8_t)'T')
#define CLOCK_OP_MONITOR    ((uint8_t)'M')

/* Sized by the text op: two header bytes plus a label longer than anything
 * the 24-column panel can show without scrolling for a while. Everything else
 * needs three bytes. */
#define CLOCK_REQ_CAP  34u
#define CLOCK_RESP_CAP 2u

static uint8_t         g_clock_req[CLOCK_REQ_CAP];
static uint8_t         g_clock_resp[CLOCK_RESP_CAP];
static chan_endpoint_t *g_clock_ep;
static int              g_clock_task_pid = -1;

/* M4.5 verify: counts chan_call()s actually served -- see
 * drivers/uart_16550.c's g_uart_write_calls comment for the reasoning. Note
 * this stays small even across a long clock session -- a `run` call is one
 * served request no matter how many hours it blocks for, matching the
 * whole point of not putting scan_step() on the wire. */
static uint32_t g_clock_calls;

uint32_t pico_clock_green_task_call_count(void) { return g_clock_calls; }

static bool clock_task_alive(void) {
    if (g_clock_task_pid < 0) return false;
    int st = sched_task_state(g_clock_task_pid);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

/* This task, and only this task, may call pico_clock_green_hw_run()/
 * pico_clock_green_hw_read_light() while alive -- see uart_16550.c's
 * uart_task_body() for the fuller reasoning (never call back into anything
 * that could chan_call() this same endpoint; never take printk_lock() from
 * here). pico_clock_green_hw_run() itself calls i2c_rtc_read_time()/
 * read_temperature_c() (drivers/i2c_rtc.c's own public facades, not that
 * driver's _hw_ internals) -- a call from this task into the "i2c" task,
 * which is a valid strictly-top-down layering (this task never serves a
 * call from "i2c", so there is no cycle for chan.c's wait-for graph to
 * catch), not a violation of the same rule this file's own endpoint relies
 * on. */
static void clock_task_body(void *arg) {
    (void)arg;
    while (!g_clock_ep) sched_yield();

    for (;;) {
        uint32_t req_len = chan_serve_wait(g_clock_ep);
        if (req_len < 1) { chan_serve_reply(g_clock_ep, 0); continue; }
        g_clock_calls++;

        uint8_t op = g_clock_req[0];
        uint32_t resp_len = 0;
        switch (op) {
        case CLOCK_OP_RUN:
            clock_app_run();
            break;
        case CLOCK_OP_READ_LIGHT: {
            uint16_t light = pico_clock_green_hw_read_light();
            g_clock_resp[0] = (uint8_t)(light >> 8);
            g_clock_resp[1] = (uint8_t)light;
            resp_len = 2;
            break;
        }
        case CLOCK_OP_POP_KEY: {
            clock_key_t k;
            clock_press_t pr;
            if (key_pop(&k, &pr)) {
                g_clock_resp[0] = (uint8_t)(((unsigned)pr << 2) | (unsigned)k);
                resp_len = 1;
            }
            break;
        }
        case CLOCK_OP_WEEKDAY:
            if (req_len >= 2) pico_clock_green_hw_set_weekday(g_clock_req[1]);
            break;
        case CLOCK_OP_INDICATOR:
            if (req_len >= 3) {
                pico_clock_green_hw_indicator((clock_indicator_t)g_clock_req[1],
                                              g_clock_req[2] != 0);
            }
            break;
        case CLOCK_OP_KEYS:
            pico_clock_green_hw_keys(req_len >= 3
                ? (unsigned)(((unsigned)g_clock_req[1] << 8) | g_clock_req[2])
                : 30u);
            break;
        case CLOCK_OP_LED_WALK:
            pico_clock_green_hw_led_walk();
            break;
#if CONFIG_ENABLE_DCF77
        case CLOCK_OP_MONITOR:
            pico_clock_green_hw_monitor(
                req_len >= 3 ? (unsigned)(((unsigned)g_clock_req[1] << 8) | g_clock_req[2]) : 120u,
                req_len >= 4 && g_clock_req[3] != 0);
            break;
#endif
        case CLOCK_OP_TEXT: {
            /* The wire carries a length, not a terminator, so it is copied
             * into a buffer this side terminates -- never handed to a string
             * function as-is. */
            char text[CLOCK_REQ_CAP];
            uint32_t n = (req_len > 2) ? (req_len - 2) : 0;
            if (n > sizeof(text) - 1) n = sizeof(text) - 1;
            for (uint32_t i = 0; i < n; i++) text[i] = (char)g_clock_req[2 + i];
            text[n] = '\0';
            pico_clock_green_hw_text(text, g_clock_req[1]);
            break;
        }
        default:
            break;
        }
        chan_serve_reply(g_clock_ep, resp_len);
    }
}

/* Called from kernel/main.c, after sched_init(). Not fatal if it fails:
 * pico_clock_green_run()/read_light() fall back to direct hardware access
 * whenever the task is not alive, same as every other M4.5 conversion. */
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

void pico_clock_green_run(void) {
    if (clock_task_alive()) {
        uint8_t req[1] = { CLOCK_OP_RUN };
        uint8_t resp[CLOCK_RESP_CAP];
        /* No retry-on-failure loop here, deliberately: a failed first
         * attempt (endpoint busy, or the task not yet reachable) should
         * fall back to direct access below rather than spin waiting on a
         * call that -- once it does succeed -- blocks for the whole clock
         * session anyway. */
        if (chan_call(g_clock_ep, req, sizeof(req), resp, sizeof(resp)) >= 0) return;
    }
    clock_app_run();
}

/* The same "through the task if it is alive, direct otherwise" shape as
 * pico_clock_green_run() above, for the C1 surface. Written out rather than
 * folded into a helper because the request and response lengths differ per
 * op and the resulting helper would take more arguments than it saves. */
bool pico_clock_green_pop_key(clock_key_t *key, clock_press_t *press) {
    if (clock_task_alive()) {
        uint8_t req[1] = { CLOCK_OP_POP_KEY };
        uint8_t resp[CLOCK_RESP_CAP];
        int n = chan_call(g_clock_ep, req, sizeof(req), resp, sizeof(resp));
        if (n >= 0) {
            if (n < 1) return false;
            if (key)   *key   = (clock_key_t)(resp[0] & 0x03u);
            if (press) *press = (clock_press_t)((resp[0] >> 2) & 0x03u);
            return true;
        }
    }
    return key_pop(key, press);
}

void pico_clock_green_set_weekday(unsigned dow) {
    if (clock_task_alive()) {
        uint8_t req[2] = { CLOCK_OP_WEEKDAY, (uint8_t)dow };
        uint8_t resp[CLOCK_RESP_CAP];
        if (chan_call(g_clock_ep, req, sizeof(req), resp, sizeof(resp)) >= 0) return;
    }
    pico_clock_green_hw_set_weekday(dow);
}

void pico_clock_green_indicator(clock_indicator_t ind, bool on) {
    if (clock_task_alive()) {
        uint8_t req[3] = { CLOCK_OP_INDICATOR, (uint8_t)ind, (uint8_t)(on ? 1 : 0) };
        uint8_t resp[CLOCK_RESP_CAP];
        if (chan_call(g_clock_ep, req, sizeof(req), resp, sizeof(resp)) >= 0) return;
    }
    pico_clock_green_hw_indicator(ind, on);
}

void pico_clock_green_keys(unsigned secs) {
    if (clock_task_alive()) {
        uint8_t req[3] = { CLOCK_OP_KEYS, (uint8_t)(secs >> 8), (uint8_t)secs };
        uint8_t resp[CLOCK_RESP_CAP];
        if (chan_call(g_clock_ep, req, sizeof(req), resp, sizeof(resp)) >= 0) return;
    }
    pico_clock_green_hw_keys(secs);
}

void pico_clock_green_led_walk(void) {
    if (clock_task_alive()) {
        uint8_t req[1] = { CLOCK_OP_LED_WALK };
        uint8_t resp[CLOCK_RESP_CAP];
        if (chan_call(g_clock_ep, req, sizeof(req), resp, sizeof(resp)) >= 0) return;
    }
    pico_clock_green_hw_led_walk();
}

#if CONFIG_ENABLE_DCF77
void pico_clock_green_dcf_monitor(unsigned secs, bool dark) {
    if (clock_task_alive()) {
        uint8_t req[4] = { CLOCK_OP_MONITOR, (uint8_t)(secs >> 8), (uint8_t)secs,
                           (uint8_t)(dark ? 1 : 0) };
        uint8_t resp[CLOCK_RESP_CAP];
        if (chan_call(g_clock_ep, req, sizeof(req), resp, sizeof(resp)) >= 0) return;
    }
    pico_clock_green_hw_monitor(secs, dark);
}
#endif

void pico_clock_green_show_text(const char *str, unsigned secs) {
    if (!str) return;
    if (clock_task_alive()) {
        uint8_t req[CLOCK_REQ_CAP];
        uint8_t resp[CLOCK_RESP_CAP];
        req[0] = CLOCK_OP_TEXT;
        req[1] = (uint8_t)(secs > 255 ? 255 : secs);
        uint32_t n = 0;
        while (str[n] && n < CLOCK_REQ_CAP - 2u) { req[2 + n] = (uint8_t)str[n]; n++; }
        if (chan_call(g_clock_ep, req, 2 + n, resp, sizeof(resp)) >= 0) return;
    }
    pico_clock_green_hw_text(str, secs);
}

uint16_t pico_clock_green_read_light(void) {
    if (clock_task_alive()) {
        uint8_t req[1] = { CLOCK_OP_READ_LIGHT };
        uint8_t resp[CLOCK_RESP_CAP];
        for (int attempt = 0; attempt < 8; attempt++) {
            int n = chan_call(g_clock_ep, req, sizeof(req), resp, sizeof(resp));
            if (n >= 2) return ((uint16_t)resp[0] << 8) | resp[1];
            if (n >= 0) break; /* short reply -- treat as failure, fall through */
            sched_yield();
        }
    }
    return pico_clock_green_hw_read_light();
}
