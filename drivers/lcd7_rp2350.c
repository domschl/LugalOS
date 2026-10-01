/*
 * The RP2350-LCD-7's 800x480 RGB panel (ST7262) -- 36.3,
 * plan/phase36_rp2350_lcd7_terminal.md §3.1.
 *
 * An RGB panel has no frame memory: it must be sent every pixel of every
 * frame, with HSYNC/VSYNC/DE framing and a continuous pixel clock. This file
 * produces that framing with zero CPU in steady state:
 *
 *   - PIO2 SM0 runs a four-instruction program that executes 16-bit
 *     *segment commands* -- {DE/VSYNC/HSYNC levels, length in PCLKs} --
 *     driving PCLK by side-set the whole time.
 *   - The whole frame is one 4096-byte command table (512 lines x 4
 *     segments x 2 bytes), and one DMA channel in RP2350's ENDLESS mode reads
 *     it through a 4 KB read ring into the SM's TX FIFO, forever. No IRQ, no
 *     re-arm, no CPU.
 *
 * 36.4 adds the picture: a 1-bpp framebuffer (800 x 480 bits, 48 000 bytes,
 * 12 heap pages) that a second PIO2 state machine turns into pixels, fed by a
 * DMA pair that restarts itself every frame. Still zero CPU.
 *
 * **Colour is not in the pixel path at all.** The pixel SM drives all 16 data
 * pins to 1s or 0s; each data pin's IO_BANK0 OUTOVER then maps that bit to
 * the chosen foreground/background pair: pass it (fg 1, bg 0 in that bit),
 * invert it (fg 0, bg 1), or force it low/high where both colours agree.
 * Any two RGB565 colours, changed instantly, no PIO registers spent on them.
 *
 * Guard-by-pin-map: empty unless the board file declares the panel.
 *
 * Register provenance (RP2350 datasheet, RP-008373-DS-2):
 *   RESETS: DMA bit 2, PIO2 bit 13, PWM bit 16.
 *   FUNCSEL: PWM 4, SIO 5, PIO0 6, PIO1 7, PIO2 8 (GPIO function table).
 *   PIO2_BASE 0x50400000: CTRL 0x000, FDEBUG 0x008, TXF0 0x010,
 *     INSTR_MEM0 0x048, SM0_CLKDIV 0x0c8, SM0_EXECCTRL 0x0cc,
 *     SM0_SHIFTCTRL 0x0d0, SM0_ADDR 0x0d4, SM0_INSTR 0x0d8, SM0_PINCTRL 0x0dc,
 *     GPIOBASE 0x168 (0 or 16; PIO pin n is then GPIO n+16, JMP_PIN too).
 *     TXF1 0x014; SM1_CLKDIV 0x0e0, SM1_EXECCTRL 0x0e4, SM1_SHIFTCTRL 0x0e8,
 *     SM1_ADDR 0x0ec, SM1_INSTR 0x0f0, SM1_PINCTRL 0x0f4. FDEBUG TXSTALL [27:24].
 *   IO_BANK0 GPIOn_CTRL: OUTOVER [13:12] (0 normal, 1 invert, 2 low, 3 high).
 *     EXECCTRL: WRAP_TOP [16:12], WRAP_BOTTOM [11:7]. SHIFTCTRL: FJOIN_TX 30,
 *     PULL_THRESH [29:25] (0 = 32), OUT_SHIFTDIR 19 (1 = right), AUTOPULL 17.
 *     PINCTRL: SIDESET_COUNT [31:29], SET_COUNT [28:26], OUT_COUNT [25:20],
 *     IN_BASE [19:15], SIDESET_BASE [14:10], SET_BASE [9:5], OUT_BASE [4:0].
 *   DMA_BASE 0x50000000, channel n at 0x40*n: READ_ADDR 0x0, WRITE_ADDR 0x4,
 *     TRANS_COUNT 0x8 ([31:28] MODE: 0xf ENDLESS), CTRL_TRIG 0xc (EN 0,
 *     DATA_SIZE [3:2], INCR_READ 4, RING_SIZE [11:8], RING_SEL 12 (0 = read),
 *     CHAIN_TO [16:13] (itself = no chain), TREQ_SEL [22:17]).
 *     DREQ_PIO2_TX0 = 16, DREQ_PIO2_TX1 = 17, TREQ 0x3f = unpaced.
 *     CHn_AL3_READ_ADDR_TRIG at 0x3c: writing it sets READ_ADDR and triggers.
 *   PWM_BASE 0x400a8000, slice n at 0x14*n: CSR 0x0 (EN bit 0), DIV 0x4
 *     (INT [11:4], FRAC [3:0]), CC 0xc (A = [15:0]), TOP 0x10. GP44 = PWM10 A.
 *   PADS_BANK0 GPIOn at 0x40038004 + 4n: ISO 8, OD 7, IE 6, DRIVE [5:4],
 *     PUE 3, PDE 2, SCHMITT 1, SLEWFAST 0.
 *   SIO: GPIO_OUT_SET 0x018, GPIO_HI_OUT_SET 0x01c, GPIO_OUT_CLR 0x020,
 *     GPIO_HI_OUT_CLR 0x024, GPIO_OE_SET 0x038, GPIO_HI_OE_SET 0x03c.
 *
 * Panel timing (ST7262 datasheet §7.3.4 gives DCLK 23-27 MHz, HBP/HFP 4-48,
 * VBP/VFP 4-12; Waveshare's demo runs 16 MHz with porches well outside the
 * vertical limits, which is the empirical floor). Start conservative, move
 * toward the datasheet: see LCD_* below.
 */

#include "drivers/lcd7.h"

#include <string.h>

#include "drivers/fbtext.h"
#include "drivers/font8x16.h"
#include "drivers/uart.h"
#include "drivers/usb_cdc.h"
#include "drivers/screen.h"
#include "drivers/vtterm.h"
#include "drivers/driver_task.h"
#include "drivers/lcdterm_attr.h"
#include "kernel/chan.h"
#include "kernel/device.h"
#include "kernel/ipc.h"
#include "kernel/mem_domain.h"
#include "kernel/sched.h"
#include "arch/umode.h"
#include "arch/rp2350_clocks.h"
#include "kernel/console.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include "lugalos_config.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_LCD_PCLK_GPIO)

/* The PIO program's pin layout: DE, VSYNC, HSYNC, PCLK consecutive, data
 * after, all inside the GPIOBASE = 16 window. */
_Static_assert(CONFIG_LCD_PCLK_GPIO == CONFIG_LCD_DE_GPIO + 3,
               "lcd7: DE, VSYNC, HSYNC, PCLK must be consecutive");
_Static_assert(CONFIG_LCD_DATA0_GPIO == CONFIG_LCD_PCLK_GPIO + 1,
               "lcd7: the 16 data pins must follow PCLK");
_Static_assert(CONFIG_LCD_DE_GPIO >= 16 && CONFIG_LCD_DATA0_GPIO + 15 <= 47,
               "lcd7: the panel pins must lie in PIO's GPIOBASE=16 window");
_Static_assert(CONFIG_LCD_DATA0_GPIO == 24,
               "lcd7: the SIO colour path below splits the data pins at GPIO 32");
_Static_assert(CONFIG_LCD_RST_GPIO >= 32 && CONFIG_LCD_EN_GPIO >= 32 && CONFIG_LCD_BL_GPIO >= 32,
               "lcd7: RST/EN/BL are driven through the GPIO_HI registers");

#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

#define RESETS_BASE          0x40020000UL
#define RESETS_RESET_CLR     (RESETS_BASE + 0x0 + 0x3000)
#define RESETS_RESET_DONE    (RESETS_BASE + 0x8)
#define RESET_DMA            (1u << 2)
#define RESET_PIO2           (1u << 13)
#define RESET_PWM            (1u << 16)

#define IO_BANK0_CTRL(n)     (0x40028004UL + (n) * 8u)
#define PADS_BANK0(n)        (0x40038004UL + (n) * 4u)
#define FUNCSEL_PWM          4u
#define FUNCSEL_SIO          5u
#define FUNCSEL_PIO2         8u
#define PAD_OUT_8MA_FAST     ((1u << 6) | (2u << 4) | 1u)   /* IE, 8 mA, fast slew; ISO clear */

#define SIO_BASE             0xd0000000UL
#define SIO_OUT_SET          (SIO_BASE + 0x018)
#define SIO_HI_OUT_SET       (SIO_BASE + 0x01c)
#define SIO_OUT_CLR          (SIO_BASE + 0x020)
#define SIO_HI_OUT_CLR       (SIO_BASE + 0x024)
#define SIO_OE_SET           (SIO_BASE + 0x038)
#define SIO_HI_OE_SET        (SIO_BASE + 0x03c)
#define HI(gpio)             (1u << ((gpio) - 32u))

#define PIO2_BASE            0x50400000UL
#define PIO_CTRL             (PIO2_BASE + 0x000)
#define PIO_FDEBUG           (PIO2_BASE + 0x008)
#define PIO_TXF0             (PIO2_BASE + 0x010)
#define PIO_INSTR_MEM(i)     (PIO2_BASE + 0x048 + 4u * (i))
#define PIO_SM0_CLKDIV       (PIO2_BASE + 0x0c8)
#define PIO_SM0_EXECCTRL     (PIO2_BASE + 0x0cc)
#define PIO_SM0_SHIFTCTRL    (PIO2_BASE + 0x0d0)
#define PIO_SM0_ADDR         (PIO2_BASE + 0x0d4)
#define PIO_SM0_INSTR        (PIO2_BASE + 0x0d8)
#define PIO_SM0_PINCTRL      (PIO2_BASE + 0x0dc)
#define PIO_GPIOBASE         (PIO2_BASE + 0x168)
#define PIO_TXF1             (PIO2_BASE + 0x014)
#define PIO_SM1_CLKDIV       (PIO2_BASE + 0x0e0)
#define PIO_SM1_EXECCTRL     (PIO2_BASE + 0x0e4)
#define PIO_SM1_SHIFTCTRL    (PIO2_BASE + 0x0e8)
#define PIO_SM1_ADDR         (PIO2_BASE + 0x0ec)
#define PIO_SM1_INSTR        (PIO2_BASE + 0x0f0)
#define PIO_SM1_PINCTRL      (PIO2_BASE + 0x0f4)
#define FDEBUG_TXSTALL_SM0   (1u << 24)
#define FDEBUG_TXSTALL_SM1   (1u << 25)
#define OUTOVER_SHIFT        12u

#define DMA_CH               0u     /* the only DMA user in this tree so far */
#define DMA_BASE             0x50000000UL
#define DMA_READ_ADDR        (DMA_BASE + 0x40u * DMA_CH + 0x0)
#define DMA_WRITE_ADDR       (DMA_BASE + 0x40u * DMA_CH + 0x4)
#define DMA_TRANS_COUNT      (DMA_BASE + 0x40u * DMA_CH + 0x8)
#define DMA_CTRL_TRIG        (DMA_BASE + 0x40u * DMA_CH + 0xc)
#define DMA_MODE_ENDLESS     (0xfu << 28)
#define DREQ_PIO2_TX0        16u

/* The framebuffer pair: FB_CH streams the frame into SM1, then chains to
 * RL_CH, which writes the frame's start address into FB_CH's
 * AL3_READ_ADDR_TRIG -- re-arming and re-triggering it. TRANS_COUNT reloads
 * on every trigger, so the pair runs forever with no CPU. */
#define FB_CH                1u
#define RL_CH                2u
#define DMA_CH_REG(ch, off)  (DMA_BASE + 0x40u * (ch) + (off))
#define DREQ_PIO2_TX1        17u
#define TREQ_UNPACED         0x3fu

#define PWM_BASE             0x400a8000UL
#define PWM_SLICE            10u    /* GP44 = PWM10 A */
#define PWM_CSR              (PWM_BASE + 0x14u * PWM_SLICE + 0x0)
#define PWM_DIV              (PWM_BASE + 0x14u * PWM_SLICE + 0x4)
#define PWM_CC               (PWM_BASE + 0x14u * PWM_SLICE + 0xc)
#define PWM_TOP              (PWM_BASE + 0x14u * PWM_SLICE + 0x10)
#define PWM_WRAP             1000u
#define PWM_HZ               5000u

/* PIO pin numbers are GPIO - 16 once GPIOBASE is 16. */
#define PIN_DE               (CONFIG_LCD_DE_GPIO - 16)
#define PIN_PCLK             (CONFIG_LCD_PCLK_GPIO - 16)
#define PIN_DATA0            (CONFIG_LCD_DATA0_GPIO - 16)

/* --- Timing -------------------------------------------------------------
 *
 * PCLK = clk_sys / LCD_CYCLES_PER_PCLK: one PCLK is two program instructions,
 * each LCD_CYCLES_PER_PCLK / 2 cycles long (1 + a delay). 6 gives 24 MHz at
 * 144 MHz, the ST7262's typical DCLK. 8 (18 MHz) is what first lit the panel
 * (2026-09-30), and the fallback if a board objects.
 *
 * A line is four segments: HSYNC pulse, back porch, active (DE high on an
 * active line, low on a blanking line), front porch. A frame is 512 lines,
 * which is what makes the command table exactly 4096 bytes -- the size the
 * DMA read ring needs. 4 + 12 + 480 + 16 = 512 (VBP 12 is the datasheet max,
 * VFP 16 is over it, as the demo's 32 is). */
#define LCD_CYCLES_PER_PCLK  6u
#define LCD_HALF_DELAY       (LCD_CYCLES_PER_PCLK / 2u - 1u)   /* the [n] on every instruction */
_Static_assert(LCD_CYCLES_PER_PCLK % 2u == 0 && LCD_HALF_DELAY <= 15u,
               "lcd7: PCLK must be an even number of cycles, delay fits 4 bits");
#define LCD_H_ACTIVE         800u
#define LCD_H_PULSE          4u
#define LCD_H_BACK           16u
#define LCD_H_FRONT          16u
#define LCD_V_ACTIVE         480u
#define LCD_V_PULSE          4u
#define LCD_V_BACK           12u
#define LCD_V_FRONT          16u
#define LCD_LINES            (LCD_V_PULSE + LCD_V_BACK + LCD_V_ACTIVE + LCD_V_FRONT)
#define LCD_SEGS_PER_LINE    4u
#define LCD_TABLE_BYTES      (LCD_LINES * LCD_SEGS_PER_LINE * 2u)
_Static_assert(LCD_TABLE_BYTES == 4096u, "lcd7: the command table must be exactly the 4 KB DMA ring");

/* Segment command: bits [2:0] levels, [15:3] x, and the segment lasts x + 2
 * PCLKs (one for the two `out`s, x + 1 for the loop). Levels: bit 0 DE (high
 * = active), bit 1 VSYNC, bit 2 HSYNC (both active low). */
#define LV_DE                1u
#define LV_VS                2u
#define LV_HS                4u
static uint16_t seg(unsigned levels, unsigned pclks) {
    return (uint16_t)(((pclks - 2u) << 3) | (levels & 7u));
}

/* --- The PIO program ------------------------------------------------------
 *
 * Side-set: 1 bit (PCLK), not optional, so every instruction's delay field is
 * [side:1][delay:4].
 *
 *   0: out pins, 3    side 0 [d]   ; DE/VS/HS for this segment
 *   1: out x, 13      side 1 [d]   ; its length - 2
 *   2: nop            side 0 [d]   ; (mov y, y)
 *   3: jmp x-- 2      side 1 [d]
 * with d = LCD_HALF_DELAY.
 *      .wrap 3 -> 0
 *
 * Hand-encoded (this tree has no pioasm): OUT = 011 ddddd ooo nnnnn with
 * destination 000 pins, 001 x; MOV = 101 ddddd ddd oo sss; JMP = 000 ddddd
 * ccc aaaaa with condition 010 X--. The delay/side-set field is bits 12:8:
 * side in bit 12, delay in 11:8. At d = 3 these are 0x6303 0x732d 0xa342
 * 0x1342, the words that first lit the panel. */
#define PIO_DS(side)         ((((uint16_t)(side) << 4) | LCD_HALF_DELAY) << 8)
static const uint16_t k_timing_prog[] = {
    (uint16_t)(0x6003 | PIO_DS(0)),   /* out pins, 3   side 0 [d] */
    (uint16_t)(0x602d | PIO_DS(1)),   /* out x, 13     side 1 [d] */
    (uint16_t)(0xa042 | PIO_DS(0)),   /* mov y, y      side 0 [d] */
    (uint16_t)(0x0042 | PIO_DS(1)),   /* jmp x--, 2    side 1 [d] */
};
#define PROG_WRAP_TOP        3u
#define PROG_WRAP_BOTTOM     0u

/* --- The pixel program (SM1) ------------------------------------------
 *
 * Cycle-locked to the timing SM rather than following PCLK's edges. It syncs
 * on DE rising, then runs exactly LCD_CYCLES_PER_PCLK cycles per pixel, so its
 * data write lands at a fixed phase of PCLK. Both loop paths are
 *   out x,1 (1) + jmp !x (1) + mov pins [d] (1+d) + jmp y-- (1) = 4 + d
 * cycles, with the mov at offset 2 in both.
 *
 * The phase: DE rises in the cycle PCLK falls (t0, the timing SM's
 * `out pins,3 side 0`). The pixel SM sees it through the 2-flop input
 * synchroniser at t0+2, so `mov y,isr` runs at t0+3, the first `out x,1` at
 * t0+4, and the first data write at t0+6, the next falling edge. Every write
 * then lands on a falling edge, with half a PCLK (3 cycles, 21 ns at 144 MHz)
 * of setup before the rising edge that samples it, and the same of hold.
 *
 * That makes the SM one pixel late to *start* a line: by the time it sees DE,
 * the first rising edge (which samples pixel 0) has already been set up. So
 * pixel 0 is output a PCLK *early*, as the last value of the previous line's
 * loop; the loop emits pixels 1..799 and then the next line's pixel 0. Each
 * line still consumes exactly 800 bits, so a frame is the whole framebuffer.
 * The first pixel 0 of all is put out once at start, synchronised to VSYNC,
 * which is also what aligns the stream to line 0.
 *
 * Registers: X the current bit, Y the pixel count, ISR holds 799 (loaded
 * once through the FIFO), OSR the data. IN_BASE is DE, so `pin 0` is DE and
 * `pin 1` is VSYNC. Loaded after the timing program, at PIX_ORG. Encodings
 * checked by a small assembler in Python before use (36.4 notes). */
#define PIX_ORG              4u
#define PIX_D                (LCD_CYCLES_PER_PCLK - 4u)
_Static_assert(LCD_CYCLES_PER_PCLK >= 4u && PIX_D <= 31u, "lcd7: pixel loop cannot fit the PCLK period");
#define PIX_MOV_D(d)         ((uint16_t)((d) << 8))
static const uint16_t k_pixel_prog[] = {
    /* +0  */ 0x2021,                                    /* wait 0 pin 1     VSYNC low       */
    /* +1  */ 0x20a1,                                    /* wait 1 pin 1     ...pulse over   */
    /* +2  */ 0x6021,                                    /* out x, 1         line 0, pixel 0 */
    /* +3  */ (uint16_t)(0x0020 | (PIX_ORG + 6)),        /* jmp !x, bg0                      */
    /* +4  */ 0xa00b,                                    /* mov pins, ~null                  */
    /* +5  */ (uint16_t)(0x0000 | (PIX_ORG + 7)),        /* jmp line                         */
    /* +6  */ 0xa003,                                    /* bg0: mov pins, null              */
    /* +7  */ 0x2020,                                    /* line: wait 0 pin 0  DE low  (wrap bottom) */
    /* +8  */ 0x20a0,                                    /* wait 1 pin 0     DE high         */
    /* +9  */ 0xa046,                                    /* mov y, isr       799             */
    /* +10 */ 0x6021,                                    /* px: out x, 1                     */
    /* +11 */ (uint16_t)(0x0020 | (PIX_ORG + 15)),       /* jmp !x, bg                       */
    /* +12 */ (uint16_t)(0xa00b | PIX_MOV_D(PIX_D)),     /* mov pins, ~null [d]              */
    /* +13 */ (uint16_t)(0x0080 | (PIX_ORG + 10)),       /* jmp y--, px                      */
    /* +14 */ (uint16_t)(0x0000 | (PIX_ORG + 7)),        /* jmp line                         */
    /* +15 */ (uint16_t)(0xa003 | PIX_MOV_D(PIX_D)),     /* bg: mov pins, null [d]           */
    /* +16 */ (uint16_t)(0x0080 | (PIX_ORG + 10)),       /* jmp y--, px     (wrap top)       */
};
#define PIX_WRAP_BOTTOM      (PIX_ORG + 7u)
#define PIX_WRAP_TOP         (PIX_ORG + 16u)
_Static_assert(PIX_ORG + sizeof(k_pixel_prog) / 2u <= 32u, "lcd7: PIO2 has 32 instruction slots");

/* --- The framebuffer ------------------------------------------------------
 *
 * Bit x of line y is bit (x % 32) of word y * 25 + x / 32: the pixel SM shifts
 * right, so the leftmost pixel is the least significant bit. Byte-wise, byte
 * x / 8 of the line, bit x % 8 -- which is why a font for this screen is
 * stored with its leftmost pixel in bit 0 (plan §4.2). 1 = foreground. */
#define FB_WORDS_PER_LINE    (LCD_H_ACTIVE / 32u)
#define FB_WORDS             (FB_WORDS_PER_LINE * LCD_V_ACTIVE)
#define FB_BYTES             (FB_WORDS * 4u)
#define FB_PAGES             ((FB_BYTES + 4095u) / 4096u)
_Static_assert(LCD_H_ACTIVE % 32u == 0, "lcd7: a line must be whole words");

static uint16_t *g_table;         /* one heap page, never freed while the panel runs */
static uint32_t *g_fb;            /* FB_PAGES heap pages, likewise */
static uint32_t  g_fb_start;      /* what RL_CH writes back into FB_CH each frame */
static bool      g_running;
static uint16_t  g_fg, g_bg;
static unsigned  g_brightness;

static void unreset(uint32_t mask) {
    REG(RESETS_RESET_CLR) = mask;
    for (int i = 0; i < 100000 && (REG(RESETS_RESET_DONE) & mask) != mask; i++) { }
}

static void build_table(void) {
    uint16_t *t = g_table;
    for (unsigned line = 0; line < LCD_LINES; line++) {
        bool vs_pulse = line < LCD_V_PULSE;
        bool active = line >= LCD_V_PULSE + LCD_V_BACK &&
                      line < LCD_V_PULSE + LCD_V_BACK + LCD_V_ACTIVE;
        unsigned vs = vs_pulse ? 0u : LV_VS;
        *t++ = seg(vs, LCD_H_PULSE);                              /* HS low */
        *t++ = seg(vs | LV_HS, LCD_H_BACK);
        *t++ = seg(vs | LV_HS | (active ? LV_DE : 0u), LCD_H_ACTIVE);
        *t++ = seg(vs | LV_HS, LCD_H_FRONT);
    }
}

/* Foreground and background, by per-pin output override: data pin n carries
 * bit n of the RGB565 word, and the pixel SM drives every data pin to the
 * pixel's bit. Where fg and bg agree in bit n the pin is forced; where they
 * differ it passes the pixel (fg 1) or its inverse (fg 0). */
void lcd7_set_colours(uint16_t fg, uint16_t bg) {
    g_fg = fg;
    g_bg = bg;
    for (unsigned n = 0; n < 16u; n++) {
        unsigned f = (fg >> n) & 1u, b = (bg >> n) & 1u;
        uint32_t over = (f == b) ? (f ? 3u : 2u) : (f ? 0u : 1u);
        REG(IO_BANK0_CTRL(CONFIG_LCD_DATA0_GPIO + n)) = (over << OUTOVER_SHIFT) | FUNCSEL_PIO2;
    }
}

/* 36.3's whole-screen colour, kept for the pin-order checks: fg = bg. */
void lcd7_set_colour(uint16_t rgb565) {
    lcd7_set_colours(rgb565, rgb565);
}

/* 0..100. The backlight converter dims as LCD_BL rises (feedback injection,
 * see the board file), so the duty cycle is inverted; 0 also switches the
 * converter off through LCD_EN. */
void lcd7_set_backlight(unsigned percent) {
    if (percent > 100u) percent = 100u;
    g_brightness = percent;
    REG(PWM_CC) = PWM_WRAP * (100u - percent) / 100u;
    if (percent == 0) REG(SIO_HI_OUT_CLR) = HI(CONFIG_LCD_EN_GPIO);
    else REG(SIO_HI_OUT_SET) = HI(CONFIG_LCD_EN_GPIO);
}

uint32_t *lcd7_framebuffer(void) {
    return g_fb;
}

/* --- Test patterns (36.4) -------------------------------------------------
 *
 * Diagnostic, not decorative: each fails in a way that names the fault. */
static void fb_fill(uint32_t word) {
    for (unsigned i = 0; i < FB_WORDS; i++) g_fb[i] = word;
}

static void fb_set(unsigned x, unsigned y) {
    g_fb[y * FB_WORDS_PER_LINE + x / 32u] |= 1u << (x % 32u);
}

int lcd7_test_pattern(const char *name) {
    if (!g_fb) return -1;
    if (name[0] == 'c' && name[1] == 'l') {                 /* clear */
        fb_fill(0);
    } else if (name[0] == 'b') {                             /* border */
        /* A one-pixel frame inset by one pixel: the outermost row and column
         * stay background. Correctly placed, a thin background gap shows
         * between the bezel and the frame on all four sides; an image shifted
         * by a pixel loses that gap on one side and doubles it on the other.
         * (A frame on the very edge was indistinguishable from the bezel --
         * the owner's observation, 2026-09-30.) */
        fb_fill(0);
        for (unsigned x = 1; x < LCD_H_ACTIVE - 1u; x++) { fb_set(x, 1); fb_set(x, LCD_V_ACTIVE - 2u); }
        for (unsigned y = 1; y < LCD_V_ACTIVE - 1u; y++) { fb_set(1, y); fb_set(LCD_H_ACTIVE - 2u, y); }
    } else if (name[0] == 's') {                             /* stripes */
        /* One-pixel vertical stripes: a wrong PCLK phase or a short pixel
         * period shows as grey mush instead of crisp lines. */
        fb_fill(0x55555555u);
    } else if (name[0] == 'c' && name[1] == 'h') {           /* checker */
        /* 8 x 16 cells: the text grid-to-be, and byte alignment. */
        for (unsigned y = 0; y < LCD_V_ACTIVE; y++) {
            uint32_t w = ((y / 16u) & 1u) ? 0xff00ff00u : 0x00ff00ffu;
            for (unsigned i = 0; i < FB_WORDS_PER_LINE; i++) g_fb[y * FB_WORDS_PER_LINE + i] = w;
        }
    } else if (name[0] == 'g') {                             /* grid */
        /* A line every 50 pixels both ways, the far edges, and a 1-pixel
         * diagonal from the top left: geometry at a glance, and a line that
         * steps sideways shows a per-line drift. */
        fb_fill(0);
        for (unsigned y = 0; y < LCD_V_ACTIVE; y++)
            for (unsigned x = 0; x < LCD_H_ACTIVE; x++)
                if (x % 50u == 0 || y % 50u == 0 || x == y ||
                    x == LCD_H_ACTIVE - 1u || y == LCD_V_ACTIVE - 1u)
                    fb_set(x, y);
    } else if (name[0] == 'r') {                             /* ruler */
        /* Four horizontal bands. In band k (top = 0) exactly one column is
         * set near each edge: x = k on the left, x = 799 - k on the right.
         * Which bands show a line, and how far from the bezel, measures a
         * horizontal offset to the pixel on each side. Band k also carries k+1
         * short dashes in the middle, so a band is identifiable on its own. */
        fb_fill(0);
        for (unsigned k = 0; k < 4u; k++) {
            unsigned y0 = k * (LCD_V_ACTIVE / 4u), y1 = y0 + LCD_V_ACTIVE / 4u;
            for (unsigned y = y0 + 4u; y < y1 - 4u; y++) {
                fb_set(k, y);
                fb_set(LCD_H_ACTIVE - 1u - k, y);
            }
            for (unsigned d = 0; d <= k; d++)
                for (unsigned x = 380u + d * 12u; x < 388u + d * 12u; x++)
                    fb_set(x, (y0 + y1) / 2u);
        }
    } else if (name[0] == 'i') {                             /* invert: swap fg/bg */
        lcd7_set_colours(g_bg, g_fg);
    } else {
        return -1;
    }
    return 0;
}

/* --- Text (36.5) -----------------------------------------------------------
 *
 * The 100 x 30 cell grid of 8 x 16 glyphs that exactly tiles the panel. */
#define TEXT_COLS            (LCD_H_ACTIVE / FONT8X16_W)
#define TEXT_ROWS            (LCD_V_ACTIVE / FONT8X16_H)
_Static_assert(LCD_H_ACTIVE % FONT8X16_W == 0 && LCD_V_ACTIVE % FONT8X16_H == 0,
               "lcd7: the text grid must tile the panel exactly");

/* 37.1: the screen's state -- the status bar, the terminal, and the cell
 * shadow (100 x 29 x 2 bytes) -- in its own 8 KB, 8 KB-aligned block, which
 * the lcdterm domain is granted as one more naturally aligned region. The
 * framebuffer's 1 152-byte spare tail, where 36.6a kept the terminal, is too
 * small for a shadow. Five regions in all (stack, text, framebuffer as two,
 * this), which is exactly what a domain may have (kernel/mem_domain.h). */
#define STATE_PAGES          2u
#define STATE_BYTES          (STATE_PAGES * 4096u)
_Static_assert(SCREEN_BYTES(TEXT_COLS, TEXT_ROWS) <= STATE_BYTES,
               "lcd7: the screen's state and its shadow must fit their block");

static void text_line(fbtext_t *t, unsigned row, const char *s, bool inverse) {
    for (unsigned col = 0; *s && col < t->cols; col++, s++) fbtext_putc(t, col, row, *s, inverse);
}

/* --- The screen as a terminal (36.6) ----------------------------------------
 *
 * Two writers feed it, the console stream (lcd7_console_putc) and the kernel
 * log's `lcd` sink (lcd7_screen_putc), and both run under console_lock(), so
 * the emulator never sees two at once. Since 37.1 it is a screen_t
 * (drivers/screen.h): a status bar on the top row and the terminal below. */
/* --- 36.6a: the terminal as the U-mode `lcdterm` task ---------------------
 *
 * The task's domain is exactly: its stack, .lcdtermtext (code and font,
 * R/X), the framebuffer as a 32 KB and a 16 KB naturally aligned piece, and
 * the screen's 8 KB state block (37.1). No MMIO at all: drawing is plain
 * stores to RAM, and PIO/DMA stay kernel-owned.
 *
 * Writers batch into g_batch and hand it over in one chan_call(); the batch
 * is flushed at every console_flush() and before the console waits for input
 * (kernel/console.c's screen hooks), so an echoed keystroke appears at once.
 * Until the task is alive -- boot, or if it never starts -- the facade draws
 * directly, the fallback every driver task here has. */
static screen_t *g_scr;             /* STATE_PAGES heap pages */
static bool      g_vt_ready;

#define LCDTERM_BATCH     256u
#define LCDTERM_OP_WRITE  'W'       /* terminal output */
#define LCDTERM_OP_TITLE  'T'       /* 37.1: the status bar's title */
#define LCDTERM_OP_RIGHT  'S'       /* 37.1: its indicators */
static uint8_t          g_batch[LCDTERM_BATCH];
static uint32_t         g_batch_len;
static uint8_t          g_lcdterm_req[1u + LCDTERM_BATCH];
static uint8_t          g_lcdterm_resp[1];
static chan_endpoint_t *g_lcdterm_ep;
static int              g_lcdterm_pid = -1;
static uint32_t         g_lcdterm_calls;

static bool lcdterm_alive(void) {
    if (g_lcdterm_pid < 0 || !g_lcdterm_ep) return false;
    int st = sched_task_state(g_lcdterm_pid);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

/* Hand-rolled per file, as every U-mode driver here does: an LCDTERM_UTEXT
 * function must not call anything outside its own region. */
__attribute__((always_inline)) static inline long lcdterm_usys_serve_wait(const char *name, uint8_t *buf, long max) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_WAIT;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = max;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}

__attribute__((always_inline)) static inline long lcdterm_usys_serve_reply(const char *name, const uint8_t *buf, long len) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_REPLY;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = len;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}

/* The task, in U-mode. `arg` is the emulator's state (arch_enter_user() puts
 * the spec's arg in a0, as spisd's body relies on). The endpoint name is built
 * in a volatile stack array, not a literal: a literal is .rodata, outside
 * this domain -- the fault drivers/tm1638_rp2350.c documents. */
LCDTERM_UTEXT static void lcdterm_umode_body(uintptr_t arg) {
    screen_t *scr = (screen_t *)arg;
    volatile char name[8];
    name[0] = 'l'; name[1] = 'c'; name[2] = 'd'; name[3] = 't';
    name[4] = 'e'; name[5] = 'r'; name[6] = 'm'; name[7] = '\0';
    for (;;) {
        uint8_t req[1u + LCDTERM_BATCH];
        long n = lcdterm_usys_serve_wait((const char *)name, req, (long)sizeof(req));
        if (n >= 1) {
            const char *p = (const char *)req + 1;
            uint32_t len = (uint32_t)(n - 1);
            if (req[0] == LCDTERM_OP_WRITE) screen_write(scr, p, len);
            else if (req[0] == LCDTERM_OP_TITLE) screen_set_title(scr, p, len);
            else if (req[0] == LCDTERM_OP_RIGHT) screen_set_right(scr, p, len);
        }
        lcdterm_usys_serve_reply((const char *)name, 0, 0);
    }
}

/* 2 KB: screen_write's deepest chain is a handful of frames, plus the
 * 257-byte request buffer. */
static uint8_t g_lcdterm_ustack[2048] __attribute__((aligned(2048)))
                                       __attribute__((section(".ustacks2048")));

static void lcdterm_task_body(void *arg) {
    (void)arg;
    while (!g_lcdterm_ep) sched_yield();
    uintptr_t fb = (uintptr_t)g_fb;
    const driver_umode_spec_t spec = {
        .name         = "lcdterm",
        .fallback     = "the terminal keeps drawing from the kernel.",
        .body         = (void (*)(void))lcdterm_umode_body,
        .text_region  = board_lcdterm_text_region,
        .stack_base   = (uintptr_t)g_lcdterm_ustack,
        .stack_size   = sizeof(g_lcdterm_ustack),
        .regions      = { { fb,                   32768u,      MEM_R | MEM_W },
                          { fb + 32768u,          16384u,      MEM_R | MEM_W },
                          { (uintptr_t)g_scr,     STATE_BYTES, MEM_R | MEM_W } },
        .region_count = 3,
        .arg          = (uintptr_t)g_scr,
    };
    (void)driver_umode_enter(&spec);
}

int lcd7_task_start(void) {
    if (!g_vt_ready) return -1;
    int pid = task_create_driver("lcdterm", lcdterm_task_body, NULL, 1);
    if (pid < 0) {
        printk("[LCD] could not start the lcdterm task; the terminal keeps drawing from the kernel\n");
        return -1;
    }
    if (chan_register_task("lcdterm", pid, g_lcdterm_req, sizeof(g_lcdterm_req),
                           g_lcdterm_resp, sizeof(g_lcdterm_resp)) != 0) {
        printk("[LCD] could not register the lcdterm endpoint; the terminal keeps drawing from the kernel\n");
        return -1;
    }
    g_lcdterm_pid = pid;
    g_lcdterm_ep = chan_lookup("lcdterm");
    printk("[LCD] terminal running as U-mode task #%d, reachable via chan_call(\"lcdterm\", ...)\n", pid);
    return pid;
}

uint32_t lcd7_task_call_count(void) { return g_lcdterm_calls; }

/* One request to the task: op, then payload. False if the task is not
 * there to take it (it died, or never started), and the caller draws. */
static bool lcdterm_send(uint8_t op, const char *p, uint32_t len) {
    if (!lcdterm_alive()) return false;
    uint8_t req[1u + LCDTERM_BATCH];
    if (len > LCDTERM_BATCH) len = LCDTERM_BATCH;
    req[0] = op;
    for (uint32_t i = 0; i < len; i++) req[1u + i] = (uint8_t)p[i];
    for (int attempt = 0; attempt < 8; attempt++) {
        if (chan_call(g_lcdterm_ep, req, 1u + len, g_lcdterm_resp, sizeof(g_lcdterm_resp)) >= 0) {
            g_lcdterm_calls++;
            return true;
        }
        sched_yield();
    }
    return false;
}

/* Called with the batch full, from console_flush(), and before input waits.
 * Takes console_lock itself (re-entrant) because writers append under it. */
void lcd7_screen_flush(void) {
    if (g_batch_len == 0) return;
    console_lock();
    if (g_batch_len > 0) {
        uint32_t len = g_batch_len;
        g_batch_len = 0;
        /* The task died or never answered: draw it here rather than lose it. */
        if (!lcdterm_send(LCDTERM_OP_WRITE, (const char *)g_batch, len))
            screen_write(g_scr, (const char *)g_batch, len);
    }
    console_unlock();
}

void lcd7_screen_putc(char c) {
    if (!g_vt_ready) return;
    if (!lcdterm_alive()) {
        screen_write(g_scr, &c, 1);
        return;
    }
    g_batch[g_batch_len++] = (uint8_t)c;
    if (g_batch_len == LCDTERM_BATCH) lcd7_screen_flush();
}

/* The batch goes first, so a title and the output before it arrive in the
 * order they were made. */
static void lcdterm_op(uint8_t op, const char *p, uint32_t len) {
    console_lock();
    lcd7_screen_flush();
    if (!lcdterm_send(op, p, len)) {
        if (op == LCDTERM_OP_TITLE) screen_set_title(g_scr, p, len);
        else screen_set_right(g_scr, p, len);
    }
    console_unlock();
}

void lcd7_set_title(const char *title) {
    if (!g_vt_ready) return;
    uint32_t n = (uint32_t)strlen(title);
    /* The task owns the state; reading it here is only a check that saves a
     * round trip when the title is already showing. */
    if (n < VT_TITLE_MAX && strcmp(g_scr->vt.title, title) == 0) return;
    lcdterm_op(LCDTERM_OP_TITLE, title, n);
}

/* The status bar's clock: HH:MM once the clock has been set, nothing
 * before (an unset clock shows the build's instant, which would be a lie).
 * Checked at most once a second, from the console's flush -- which runs on
 * every turn of an input wait and at every write, so the minute moves while
 * the shell is idle and while a program prints. A program that computes for
 * minutes in silence leaves it stale until it next writes or reads. */
static uint64_t g_clock_next_us;
static int16_t  g_clock_shown = -1;     /* hour * 60 + minute, or -1 */

static void clock_tick(void) {
    uint64_t now = time_get_us();
    if (now < g_clock_next_us) return;
    g_clock_next_us = now + 1000000u;
    int16_t m = -1;
    rtc_time_t tm;
    if (time_is_set()) {
        time_get_local(&tm);
        m = (int16_t)(tm.hour * 60 + tm.min);
    }
    if (m == g_clock_shown) return;
    g_clock_shown = m;
    char buf[8];
    uint32_t n = 0;
    if (m >= 0) {
        buf[0] = (char)('0' + tm.hour / 10); buf[1] = (char)('0' + tm.hour % 10); buf[2] = ':';
        buf[3] = (char)('0' + tm.min / 10);  buf[4] = (char)('0' + tm.min % 10);
        n = 5;
    }
    lcdterm_op(LCDTERM_OP_RIGHT, buf, n);
}

static void lcd7_console_flush(void) {
    if (!g_vt_ready) return;
    lcd7_screen_flush();
    clock_tick();
}

static bool lcd7_console_size(unsigned *cols, unsigned *rows) {
    if (!g_vt_ready) return false;
    screen_text_size(g_scr, cols, rows);
    return true;
}

static const console_screen_t g_console_screen = {
    .flush     = lcd7_console_flush,
    .size      = lcd7_console_size,
    .set_title = lcd7_set_title,
};

const console_screen_t *lcd7_console_screen(void) {
    return g_vt_ready ? &g_console_screen : NULL;
}

/* `lcdtermisotest`: the lcdterm domain, but a body that stores into kernel
 * memory. Must fault, and the canary must survive -- the check every isolated
 * driver here has (st7735isotest, blkisotest, ...). */
static volatile uintptr_t g_lcdterm_canary = 0xC0FFEE;
static volatile bool      g_lcdterm_intruder_entered;

LCDTERM_UTEXT static void lcdterm_intruder(void) {
    g_lcdterm_canary = 0xDEAD;
    for (;;) { }                    /* only reached if the store was not stopped */
}

static void lcdterm_intruder_task_body(void *arg) {
    uint8_t *ustack = (uint8_t *)arg;
    mem_domain_t dom;
    mem_domain_init(&dom);
    mem_domain_add(&dom, (uintptr_t)ustack, 4096, MEM_R | MEM_W);
    uintptr_t tbase, tsize;
    board_lcdterm_text_region(&tbase, &tsize);
    mem_domain_add(&dom, tbase, tsize, MEM_R | MEM_X);
    mem_domain_add(&dom, (uintptr_t)g_fb, 32768u, MEM_R | MEM_W);
    mem_domain_add(&dom, (uintptr_t)g_fb + 32768u, 16384u, MEM_R | MEM_W);
    mem_domain_add(&dom, (uintptr_t)g_scr, STATE_BYTES, MEM_R | MEM_W);
    if (task_set_domain(sched_current_pid(), &dom) != 0) {
        printk("[LcdtermIso] refusing to enter U-mode: memory domain not enforceable\n");
        return;
    }
    g_lcdterm_intruder_entered = true;
    arch_enter_user(lcdterm_intruder, (uintptr_t)ustack + 4096, 0, 0, 0);
}

bool lcd7_isolation_test(uintptr_t *out_canary, bool *out_exited_clean) {
    g_lcdterm_canary = 0xC0FFEE;
    g_lcdterm_intruder_entered = false;
    if (!g_fb || !g_scr) return false;
    void *ustack = palloc_pages(1);
    if (!ustack) return false;
    int pid = task_create("lcdterm_intruder", lcdterm_intruder_task_body, ustack);
    if (pid < 0) {
        palloc_free(ustack, 1);
        return false;
    }
    for (int i = 0; i < 10000 && sched_task_state(pid) != TASK_DEAD; i++) sched_yield();
    *out_exited_clean = (sched_task_state(pid) == TASK_DEAD);
    *out_canary = g_lcdterm_canary;
    palloc_free(ustack, 1);
    return g_lcdterm_intruder_entered;
}

/* The `lcd` console device: the screen, plus a tee so a host sees what the
 * screen shows. Where the tee goes is a setting (`lcd tee`), because the
 * UART path paces everything: uart_putc() feeds UART0 at 115200 baud and
 * mirrors to USB ACM0, and the screen then draws no faster than 11.5 KB/s --
 * visible as the `e` editor painting its box line by line (the owner's
 * observation, 2026-09-30). */
/* USB by default. Measured on the board with `lcd outbench` (1980 characters
 * through console_putc): tee to the UART path 11 313 chars/s -- exactly
 * 115200 baud -- against 89 795 chars/s to ACM0 alone. The host console is on
 * USB anyway; nothing is attached to header H7 unless someone puts an adapter
 * there, and then `lcd tee uart` is one command. The kernel log still reaches
 * UART0 through its own sink either way. */
static uint8_t g_tee = LCD_TEE_USB;

void lcd7_set_tee(unsigned mode) {
    if (mode <= LCD_TEE_OFF) g_tee = (uint8_t)mode;
}

unsigned lcd7_tee(void) {
    return g_tee;
}

void lcd7_console_putc(char c) {
    lcd7_screen_putc(c);
    if (g_tee == LCD_TEE_UART) uart_putc(c);
    else if (g_tee == LCD_TEE_USB) (void)usb_cdc_putc_wait(c, 20000u);   /* never drop mid-sequence */
}

uint32_t lcd7_unknown_sequences(void) {
    return g_vt_ready ? g_scr->vt.unknown : 0;
}

/* `lcd test text`: every glyph, a pangram, reversed video, and a cursor. */
int lcd7_text_test(void) {
    if (!g_fb) return -1;
    fbtext_t t;
    fbtext_init(&t, g_fb, LCD_H_ACTIVE / 8u, TEXT_COLS, TEXT_ROWS);
    fbtext_clear_rows(&t, 0, TEXT_ROWS);
    text_line(&t, 0, "LugalOS on the RP2350-LCD-7: Spleen 8x16, 100 x 30 cells, 1-bpp at 56 Hz", false);
    unsigned row = 2, col = 0;
    for (int c = FONT8X16_FIRST; c <= FONT8X16_LAST; c++) {
        fbtext_putc(&t, col, row, (char)c, false);
        if (++col == 64u) { col = 0; row++; }
    }
    text_line(&t, 5, "The quick brown fox jumps over the lazy dog. 0123456789", false);
    text_line(&t, 6, "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG! (){}[]<>=+-*/\\|~^", false);
    text_line(&t, 8, " Reverse video: SGR 7 is exact on a 1-bpp screen. ", true);
    text_line(&t, 10, "Cursor after this >", false);
    fbtext_cursor_xor(&t, 19, 10);
    for (unsigned c = 0; c < TEXT_COLS; c++) fbtext_putc(&t, c, TEXT_ROWS - 1u, (char)('0' + c % 10u), false);
    return 0;
}

/* `lcd scroll <n>`: n numbered lines, each written after scrolling the grid up
 * one row -- what `cat` of a long file does. Times each scroll alone. */
void lcd7_scroll_test(unsigned n) {
    if (!g_fb) return;
    if (n == 0) n = 1000u;
    fbtext_t t;
    fbtext_init(&t, g_fb, LCD_H_ACTIVE / 8u, TEXT_COLS, TEXT_ROWS);
    uint64_t total = 0, worst = 0, best = ~(uint64_t)0;
    uint64_t t_all = time_get_us();
    char buf[TEXT_COLS + 1];
    for (unsigned i = 1; i <= n; i++) {
        uint64_t t0 = time_get_us();
        fbtext_scroll_up(&t, 1);
        uint64_t d = time_get_us() - t0;
        total += d;
        if (d > worst) worst = d;
        if (d < best) best = d;
        ksnprintf(buf, sizeof(buf), "line %5u of %u: the quick brown fox jumps over the lazy dog", i, n);
        text_line(&t, TEXT_ROWS - 1u, buf, false);
    }
    uint64_t all = time_get_us() - t_all;
    cprintf("lcd scroll: %u lines in %lu ms; one scroll (a %lu-byte memmove) took %lu us on average, "
            "best %lu, worst %lu\n",
            n, (unsigned long)(all / 1000u),
            (unsigned long)((TEXT_ROWS - 1u) * FONT8X16_H * (LCD_H_ACTIVE / 8u)),
            (unsigned long)(total / n), (unsigned long)best, (unsigned long)worst);
}

int lcd7_init(void) {
    unreset(RESET_DMA | RESET_PIO2 | RESET_PWM);

    /* Control pins: RST and EN as SIO outputs, low. */
    const unsigned ctl[] = { CONFIG_LCD_RST_GPIO, CONFIG_LCD_EN_GPIO };
    for (unsigned i = 0; i < 2; i++) {
        REG(SIO_HI_OUT_CLR) = HI(ctl[i]);
        REG(SIO_HI_OE_SET) = HI(ctl[i]);
        REG(PADS_BANK0(ctl[i])) = PAD_OUT_8MA_FAST;
        REG(IO_BANK0_CTRL(ctl[i])) = FUNCSEL_SIO;
    }

    /* Backlight PWM, off (level = WRAP is full dim, and EN is low anyway). */
    uint32_t div16 = (uint32_t)(((uint64_t)CONFIG_CLK_SYS_HZ * 16u) / (PWM_HZ * PWM_WRAP));
    REG(PWM_CSR) = 0;
    REG(PWM_DIV) = div16;              /* INT [11:4], FRAC [3:0] */
    REG(PWM_TOP) = PWM_WRAP - 1u;
    REG(PWM_CC) = PWM_WRAP;
    REG(PWM_CSR) = 1u;
    REG(PADS_BANK0(CONFIG_LCD_BL_GPIO)) = PAD_OUT_8MA_FAST;
    REG(IO_BANK0_CTRL(CONFIG_LCD_BL_GPIO)) = FUNCSEL_PWM;

    /* Panel power and reset, per the demo's sequence: EN high, RST pulse. */
    REG(SIO_HI_OUT_SET) = HI(CONFIG_LCD_EN_GPIO);
    time_delay_us(20000);
    REG(SIO_HI_OUT_SET) = HI(CONFIG_LCD_RST_GPIO);
    time_delay_us(200000);

    /* The command table: one page, which is also the 4 KB alignment the
     * DMA read ring requires. The framebuffer: 12 pages, cleared. */
    g_table = (uint16_t *)palloc_pages(1);
    /* 32 KB-aligned, so the lcdterm domain can grant it as two naturally
     * aligned pieces (32 KB + 16 KB) -- PMP NAPOT regions must be. */
    g_fb = (uint32_t *)palloc_pages_aligned(FB_PAGES, 8);
    if (!g_table || !g_fb) {
        printk("[LCD] no memory for the timing table or framebuffer; panel stays dark\n");
        return -1;
    }
    build_table();
    fb_fill(0);
    g_fb_start = (uint32_t)(uintptr_t)g_fb;

    /* PIO2: both programs, both state machines, all pins. */
    REG(PIO_CTRL) = 0;
    REG(PIO_GPIOBASE) = 16u;
    for (unsigned i = 0; i < sizeof(k_timing_prog) / sizeof(k_timing_prog[0]); i++) {
        REG(PIO_INSTR_MEM(i)) = k_timing_prog[i];
    }
    for (unsigned i = 0; i < sizeof(k_pixel_prog) / sizeof(k_pixel_prog[0]); i++) {
        REG(PIO_INSTR_MEM(PIX_ORG + i)) = k_pixel_prog[i];
    }

    /* SM0, timing. SET covers DE..PCLK so the init below can take all four;
     * OUT is the three framing pins; side-set is PCLK. */
    REG(PIO_SM0_CLKDIV) = 1u << 16;                                  /* 1.0 */
    REG(PIO_SM0_EXECCTRL) = (PROG_WRAP_TOP << 12) | (PROG_WRAP_BOTTOM << 7);
    REG(PIO_SM0_SHIFTCTRL) = (1u << 30) | (1u << 19) | (1u << 17);   /* FJOIN_TX, right, autopull 32 */
    REG(PIO_SM0_PINCTRL) = (1u << 29) | (4u << 26) | (3u << 20) |
                           ((uint32_t)PIN_PCLK << 10) | ((uint32_t)PIN_DE << 5) | (uint32_t)PIN_DE;
    REG(PIO_SM0_INSTR) = 0xe08f;       /* set pindirs, 0b1111 : DE..PCLK outputs */
    REG(PIO_SM0_INSTR) = 0xe006;       /* set pins, 0b0110 : DE 0, VS 1, HS 1, PCLK 0 */
    REG(PIO_SM0_INSTR) = 0x0000;       /* jmp 0 */

    /* SM1, pixels. OUT is the 16 data pins, IN_BASE is DE (pin 0 DE, pin 1
     * VSYNC). ISR gets 799 through the FIFO, and OSR is then emptied so the
     * first `out` autopulls framebuffer data rather than that count. */
    REG(PIO_SM1_CLKDIV) = 1u << 16;
    REG(PIO_SM1_EXECCTRL) = (PIX_WRAP_TOP << 12) | (PIX_WRAP_BOTTOM << 7);
    REG(PIO_SM1_SHIFTCTRL) = (1u << 30) | (1u << 19) | (1u << 17);
    REG(PIO_SM1_PINCTRL) = (16u << 20) | ((uint32_t)PIN_DE << 15) | (uint32_t)PIN_DATA0;
    REG(PIO_SM1_INSTR) = 0xa06b;       /* mov pindirs, ~null : the 16 data pins out */
    REG(PIO_TXF1) = LCD_H_ACTIVE - 1u;
    REG(PIO_SM1_INSTR) = 0x80a0;       /* pull block */
    REG(PIO_SM1_INSTR) = 0xa0c7;       /* mov isr, osr */
    REG(PIO_SM1_INSTR) = 0x6060;       /* out null, 32 */
    REG(PIO_SM1_INSTR) = (uint16_t)(0x0000 | PIX_ORG);   /* jmp start */

    for (unsigned g = CONFIG_LCD_DE_GPIO; g <= CONFIG_LCD_PCLK_GPIO; g++) {
        REG(PADS_BANK0(g)) = PAD_OUT_8MA_FAST;
        REG(IO_BANK0_CTRL(g)) = FUNCSEL_PIO2;
    }
    for (unsigned g = CONFIG_LCD_DATA0_GPIO; g < CONFIG_LCD_DATA0_GPIO + 16u; g++) {
        REG(PADS_BANK0(g)) = PAD_OUT_8MA_FAST;
    }
    /* Black on white: the Macintosh's default, and phase 36 §7.1's. */
    lcd7_set_colours(0x0000, 0xffff);

    /* DMA 0: the timing table into SM0, forever. */
    REG(DMA_READ_ADDR) = (uint32_t)(uintptr_t)g_table;
    REG(DMA_WRITE_ADDR) = PIO_TXF0;
    REG(DMA_TRANS_COUNT) = DMA_MODE_ENDLESS | 1u;
    REG(DMA_CTRL_TRIG) = 1u                          /* EN */
                       | (2u << 2)                   /* 32-bit */
                       | (1u << 4)                   /* INCR_READ */
                       | (12u << 8)                  /* ring 4 KB, on the read side */
                       | (DMA_CH << 13)              /* chain to itself: no chain */
                       | (DREQ_PIO2_TX0 << 17);

    /* DMA 2 (reload): one word, the frame start, into FB_CH's
     * AL3_READ_ADDR_TRIG. Configured through AL1_CTRL (0x10), which does not
     * trigger: FB_CH's chain does that. */
    REG(DMA_CH_REG(RL_CH, 0x0)) = (uint32_t)(uintptr_t)&g_fb_start;
    REG(DMA_CH_REG(RL_CH, 0x4)) = DMA_CH_REG(FB_CH, 0x3c);
    REG(DMA_CH_REG(RL_CH, 0x8)) = 1u;
    REG(DMA_CH_REG(RL_CH, 0x10)) = 1u | (2u << 2) | (RL_CH << 13) | (TREQ_UNPACED << 17);

    /* DMA 1 (frame): the framebuffer into SM1, then chain to the reload. */
    REG(DMA_CH_REG(FB_CH, 0x0)) = g_fb_start;
    REG(DMA_CH_REG(FB_CH, 0x4)) = PIO_TXF1;
    REG(DMA_CH_REG(FB_CH, 0x8)) = FB_WORDS;
    REG(DMA_CH_REG(FB_CH, 0xc)) = 1u | (2u << 2) | (1u << 4) | (RL_CH << 13) | (DREQ_PIO2_TX1 << 17);

    REG(PIO_FDEBUG) = FDEBUG_TXSTALL_SM0 | FDEBUG_TXSTALL_SM1;   /* clear, then start */
    REG(PIO_CTRL) = 3u;                                          /* SM0 + SM1 */
    g_running = true;

    g_scr = (screen_t *)palloc_pages_aligned(STATE_PAGES, STATE_PAGES);
    if (!g_scr) {
        printk("[LCD] no memory for the screen's state; the panel runs without a terminal\n");
    } else {
        screen_init(g_scr, g_fb, LCD_H_ACTIVE / 8u, TEXT_COLS, TEXT_ROWS);
        g_vt_ready = true;
    }

    lcd7_set_backlight(100);
    printk("[LCD] 800x480 panel running: PCLK %lu kHz, %u x %u total, %lu.%lu Hz, 1-bpp framebuffer at 0x%08lx\n",
           (unsigned long)(CONFIG_CLK_SYS_HZ / LCD_CYCLES_PER_PCLK / 1000u),
           (unsigned)(LCD_H_PULSE + LCD_H_BACK + LCD_H_ACTIVE + LCD_H_FRONT), (unsigned)LCD_LINES,
           (unsigned long)(CONFIG_CLK_SYS_HZ / LCD_CYCLES_PER_PCLK /
                           ((LCD_H_PULSE + LCD_H_BACK + LCD_H_ACTIVE + LCD_H_FRONT) * LCD_LINES)),
           (unsigned long)((CONFIG_CLK_SYS_HZ / LCD_CYCLES_PER_PCLK * 10u /
                           ((LCD_H_PULSE + LCD_H_BACK + LCD_H_ACTIVE + LCD_H_FRONT) * LCD_LINES)) % 10u),
           (unsigned long)g_fb_start);
    return 0;
}

void lcd7_report(void) {
    if (!g_running) {
        cprintf("lcd: not running\n");
        return;
    }
    uint32_t fdebug = REG(PIO_FDEBUG);
    cprintf("lcd: running, fg 0x%04x bg 0x%04x, backlight %u%%\n", g_fg, g_bg, g_brightness);
    if (g_vt_ready) {
        cprintf("lcd: text window %u x %u below the status bar, cursor at row %u col %u, "
                "%lu unknown sequences swallowed; title \"%s\"\n",
                (unsigned)g_scr->vt.text.cols, (unsigned)g_scr->vt.text.rows,
                (unsigned)g_scr->vt.row, (unsigned)g_scr->vt.col,
                (unsigned long)g_scr->vt.unknown, g_scr->vt.title);
    }
    cprintf("lcd: terminal %s (task #%d, %lu batches served)\n",
            lcdterm_alive() ? "in the U-mode lcdterm task" : "drawn from the kernel",
            g_lcdterm_pid, (unsigned long)g_lcdterm_calls);
    cprintf("lcd: timing SM pc %lu, underrun %s; pixel SM pc %lu, underrun %s\n",
            (unsigned long)(REG(PIO_SM0_ADDR) & 0x1fu),
            (fdebug & FDEBUG_TXSTALL_SM0) ? "SEEN" : "none",
            (unsigned long)(REG(PIO_SM1_ADDR) & 0x1fu),
            (fdebug & FDEBUG_TXSTALL_SM1) ? "SEEN" : "none");
    cprintf("lcd: table DMA at 0x%08lx; frame DMA at 0x%08lx (%lu words left), fb 0x%08lx..0x%08lx\n",
            (unsigned long)REG(DMA_READ_ADDR),
            (unsigned long)REG(DMA_CH_REG(FB_CH, 0x0)),
            (unsigned long)(REG(DMA_CH_REG(FB_CH, 0x8)) & 0x0fffffffu),
            (unsigned long)g_fb_start, (unsigned long)(g_fb_start + FB_BYTES));
}

#else

int lcd7_init(void) { return -1; }
void lcd7_set_colour(uint16_t rgb565) { (void)rgb565; }
void lcd7_set_colours(uint16_t fg, uint16_t bg) { (void)fg; (void)bg; }
uint32_t *lcd7_framebuffer(void) { return 0; }
void lcd7_screen_putc(char c) { (void)c; }
void lcd7_console_putc(char c) { (void)c; }
void lcd7_set_tee(unsigned mode) { (void)mode; }
unsigned lcd7_tee(void) { return LCD_TEE_OFF; }
uint32_t lcd7_unknown_sequences(void) { return 0; }
int lcd7_task_start(void) { return -1; }
void lcd7_screen_flush(void) { }
const console_screen_t *lcd7_console_screen(void) { return NULL; }
void lcd7_set_title(const char *title) { (void)title; }
uint32_t lcd7_task_call_count(void) { return 0; }
bool lcd7_isolation_test(uintptr_t *out_canary, bool *out_exited_clean) { (void)out_canary; (void)out_exited_clean; return false; }
int lcd7_text_test(void) { return -1; }
void lcd7_scroll_test(unsigned n) { (void)n; }
int lcd7_test_pattern(const char *name) { (void)name; return -1; }
void lcd7_set_backlight(unsigned percent) { (void)percent; }
void lcd7_report(void) { }

#endif /* CONFIG_BOARD_RP2350 && CONFIG_LCD_PCLK_GPIO */
