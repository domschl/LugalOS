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
#include "drivers/i2c_rtc.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "kernel/sched.h"
#include "kernel/chan.h"
#include "kernel/printk.h"
#include "lugalos_config.h"
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
static uint8_t g_row;
static uint16_t g_dim_on_us; /* 0 = full brightness (no PWM pulse needed) */

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

/* Forward declaration: pico_clock_green_init() (boot-time, before the task
 * exists) is the only caller of this outside the task body below. */
static void pico_clock_green_clear(void);

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

    adc_hw_init();

    pico_clock_green_clear();
    g_row = 0;
    g_dim_on_us = 0;
}

static void pico_clock_green_clear(void) {
    memset(g_disp_buf, 0, sizeof(g_disp_buf));
}

static void pico_clock_green_show_time(unsigned hour, unsigned minute) {
    if (hour > 23) hour = 23;
    if (minute > 59) minute = 59;

    pico_clock_green_clear();
    draw_glyph(COL_HOUR_TENS, GLYPH_DIGIT[hour / 10]);
    draw_glyph(COL_HOUR_ONES, GLYPH_DIGIT[hour % 10]);
    draw_glyph(COL_COLON, GLYPH_COLON);
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

static void pico_clock_green_scan_step(void) {
    oe_close();

    for (unsigned group = 0; group < 4; group++) {
        shift_byte(g_disp_buf[8 * group + g_row]);
    }
    latch_pulse();
    set_row_address(g_row);

    if (g_row == 0) {
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

/* Show time most of the time, temperature briefly and only when the DS3231
 * is actually present -- a board with no RTC just never leaves time mode
 * (re-checked every cycle rather than latched once, so a board that starts
 * without one can still pick it up if that ever changes, at negligible
 * cost). Re-renders only on an actual value change, not every loop
 * iteration -- draw_glyph()'s bit-packing is cheap, but there is no reason
 * to redo it 1000 times/sec for a value that changes once/minute. */
#define CLOCK_TIME_DISPLAY_MS 8000
#define CLOCK_TEMP_DISPLAY_MS 2000
#define CLOCK_ROW_PERIOD_US   1000

/* How often to poll the RTC over I2C -- deliberately *not* every
 * scan_step() call. Found on real hardware, not predicted: an I2C
 * transaction at 100kHz blocks for real time (~800us-1ms for the 7-byte
 * time read, ~300-400us for the 2-byte temperature read -- long enough,
 * relative to the ~1ms row period, to visibly disrupt the row-scan cadence
 * whichever row it lands on), so doing it every row made the display
 * flicker -- worse for time than temperature, matching the two reads'
 * different lengths exactly. Reading once/sec instead means only 1 row in
 * roughly 1000 ever pays that cost, not every row. */
#define CLOCK_READ_INTERVAL_MS 1000

static void pico_clock_green_hw_run(void) {
    /* Matches chess_run()'s own precedent (chess_ui.c): a stale interrupt
     * latched by an unrelated earlier Ctrl-C must not abort this run
     * before it does anything. */
    console_interrupt_clear();

    bool showing_temp = false;
    uint64_t mode_deadline_ms = time_get_ms() + CLOCK_TIME_DISPLAY_MS;
    uint64_t next_read_ms = 0; /* due immediately on the first iteration */
    bool have_rendered = false;
    unsigned last_hour = 0, last_minute = 0;
    int last_temp_c = 0;

    for (;;) {
        uint64_t now = time_get_ms();

        if (now >= mode_deadline_ms) {
            showing_temp = !showing_temp && i2c_rtc_is_detected();
            mode_deadline_ms = now + (showing_temp ? CLOCK_TEMP_DISPLAY_MS : CLOCK_TIME_DISPLAY_MS);
            have_rendered = false;
            next_read_ms = 0; /* refresh right away on a mode switch too */
        }

        if (now >= next_read_ms) {
            if (showing_temp) {
                int temp_c;
                if (i2c_rtc_read_temperature_c(&temp_c) && (!have_rendered || temp_c != last_temp_c)) {
                    pico_clock_green_show_temperature_c(temp_c);
                    last_temp_c = temp_c;
                    have_rendered = true;
                }
            } else {
                rtc_time_t tm;
                if (i2c_rtc_read_time(&tm) && (!have_rendered || tm.hour != last_hour || tm.min != last_minute)) {
                    pico_clock_green_show_time(tm.hour, tm.min);
                    last_hour = tm.hour;
                    last_minute = tm.min;
                    have_rendered = true;
                }
            }
            next_read_ms = now + CLOCK_READ_INTERVAL_MS;
        }

        pico_clock_green_scan_step();

        if (console_interrupt_requested()) {
            console_interrupt_clear();
            break;
        }

        time_delay_us(CLOCK_ROW_PERIOD_US);
    }

    /* Blank physically, not just in the buffer -- nothing calls
     * scan_step() once this returns, so a lit row would otherwise stay lit
     * (whatever it last shifted out) rather than actually going dark. */
    pico_clock_green_clear();
    oe_close();
}

/* M4.5, plan/phase12_microkernel_migration.md, Part B: the driver as a task
 * -- see drivers/include/drivers/pico_clock_green.h's comment on
 * pico_clock_green_run() for why the whole appliance loop is one
 * chan_call(), not one per ~1kHz row-scan step. Only two wire ops: nothing
 * else here ever had a caller outside this file.
 *
 *   'R' run        req: [op]  resp: 0 bytes (replied once Ctrl-C breaks the loop)
 *   'L' read_light req: [op]  resp: light(2, uint16_t) */
#define CLOCK_OP_RUN        ((uint8_t)'R')
#define CLOCK_OP_READ_LIGHT ((uint8_t)'L')

#define CLOCK_REQ_CAP  1u
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
            pico_clock_green_hw_run();
            break;
        case CLOCK_OP_READ_LIGHT: {
            uint16_t light = pico_clock_green_hw_read_light();
            g_clock_resp[0] = (uint8_t)(light >> 8);
            g_clock_resp[1] = (uint8_t)light;
            resp_len = 2;
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
    pico_clock_green_hw_run();
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
