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
 * 36.3 drives the 16 data pins from SIO with one constant colour: the point
 * of this milestone is sync, polarity and pin order, in isolation. 36.4
 * replaces that with a pixel SM fed from a framebuffer.
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
 *     EXECCTRL: WRAP_TOP [16:12], WRAP_BOTTOM [11:7]. SHIFTCTRL: FJOIN_TX 30,
 *     PULL_THRESH [29:25] (0 = 32), OUT_SHIFTDIR 19 (1 = right), AUTOPULL 17.
 *     PINCTRL: SIDESET_COUNT [31:29], SET_COUNT [28:26], OUT_COUNT [25:20],
 *     IN_BASE [19:15], SIDESET_BASE [14:10], SET_BASE [9:5], OUT_BASE [4:0].
 *   DMA_BASE 0x50000000, channel n at 0x40*n: READ_ADDR 0x0, WRITE_ADDR 0x4,
 *     TRANS_COUNT 0x8 ([31:28] MODE: 0xf ENDLESS), CTRL_TRIG 0xc (EN 0,
 *     DATA_SIZE [3:2], INCR_READ 4, RING_SIZE [11:8], RING_SEL 12 (0 = read),
 *     CHAIN_TO [16:13] (itself = no chain), TREQ_SEL [22:17]).
 *     DREQ_PIO2_TX0 = 16.
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
#define FDEBUG_TXSTALL_SM0   (1u << 24)

#define DMA_CH               0u     /* the only DMA user in this tree so far */
#define DMA_BASE             0x50000000UL
#define DMA_READ_ADDR        (DMA_BASE + 0x40u * DMA_CH + 0x0)
#define DMA_WRITE_ADDR       (DMA_BASE + 0x40u * DMA_CH + 0x4)
#define DMA_TRANS_COUNT      (DMA_BASE + 0x40u * DMA_CH + 0x8)
#define DMA_CTRL_TRIG        (DMA_BASE + 0x40u * DMA_CH + 0xc)
#define DMA_MODE_ENDLESS     (0xfu << 28)
#define DREQ_PIO2_TX0        16u

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

static uint16_t *g_table;         /* one heap page, never freed while the panel runs */
static bool      g_running;
static uint16_t  g_colour;
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

/* The 16 data pins as SIO outputs holding one colour: bit n of the RGB565
 * word drives GP24+n, which splits at GPIO 32 between the two SIO banks. */
void lcd7_set_colour(uint16_t rgb565) {
    g_colour = rgb565;
    REG(SIO_OUT_CLR) = 0xffu << 24;
    REG(SIO_OUT_SET) = (uint32_t)(rgb565 & 0xffu) << 24;
    REG(SIO_HI_OUT_CLR) = 0xffu;
    REG(SIO_HI_OUT_SET) = (uint32_t)(rgb565 >> 8) & 0xffu;
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

    /* Data pins: SIO outputs (36.3), black to start. */
    REG(SIO_OUT_CLR) = 0xffu << 24;
    REG(SIO_HI_OUT_CLR) = 0xffu;
    REG(SIO_OE_SET) = 0xffu << 24;
    REG(SIO_HI_OE_SET) = 0xffu;
    for (unsigned g = CONFIG_LCD_DATA0_GPIO; g < CONFIG_LCD_DATA0_GPIO + 16u; g++) {
        REG(PADS_BANK0(g)) = PAD_OUT_8MA_FAST;
        REG(IO_BANK0_CTRL(g)) = FUNCSEL_SIO;
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
     * DMA read ring requires. */
    g_table = (uint16_t *)palloc_pages(1);
    if (!g_table) {
        printk("[LCD] no page for the timing table; panel stays dark\n");
        return -1;
    }
    build_table();

    /* PIO2: program, state machine, pins. */
    REG(PIO_CTRL) = 0;
    REG(PIO_GPIOBASE) = 16u;
    for (unsigned i = 0; i < sizeof(k_timing_prog) / sizeof(k_timing_prog[0]); i++) {
        REG(PIO_INSTR_MEM(i)) = k_timing_prog[i];
    }
    REG(PIO_SM0_CLKDIV) = 1u << 16;                                  /* 1.0 */
    REG(PIO_SM0_EXECCTRL) = (PROG_WRAP_TOP << 12) | (PROG_WRAP_BOTTOM << 7);
    REG(PIO_SM0_SHIFTCTRL) = (1u << 30) | (1u << 19) | (1u << 17);   /* FJOIN_TX, right, autopull 32 */
    /* SET covers DE..PCLK so the init below can take all four; OUT is the
     * three framing pins; side-set is PCLK. */
    REG(PIO_SM0_PINCTRL) = (1u << 29) | (4u << 26) | (3u << 20) |
                           ((uint32_t)PIN_PCLK << 10) | ((uint32_t)PIN_DE << 5) | (uint32_t)PIN_DE;
    REG(PIO_SM0_INSTR) = 0xe08f;       /* set pindirs, 0b1111 : DE..PCLK outputs */
    REG(PIO_SM0_INSTR) = 0xe006;       /* set pins, 0b0110 : DE 0, VS 1, HS 1, PCLK 0 */
    REG(PIO_SM0_INSTR) = 0x0000;       /* jmp 0 */
    for (unsigned g = CONFIG_LCD_DE_GPIO; g <= CONFIG_LCD_PCLK_GPIO; g++) {
        REG(PADS_BANK0(g)) = PAD_OUT_8MA_FAST;
        REG(IO_BANK0_CTRL(g)) = FUNCSEL_PIO2;
    }

    /* DMA: the table into SM0's TX FIFO, forever. */
    REG(DMA_READ_ADDR) = (uint32_t)(uintptr_t)g_table;
    REG(DMA_WRITE_ADDR) = PIO_TXF0;
    REG(DMA_TRANS_COUNT) = DMA_MODE_ENDLESS | 1u;
    REG(DMA_CTRL_TRIG) = 1u                          /* EN */
                       | (2u << 2)                   /* 32-bit */
                       | (1u << 4)                   /* INCR_READ */
                       | (12u << 8)                  /* ring 4 KB, on the read side */
                       | (DMA_CH << 13)              /* chain to itself: no chain */
                       | (DREQ_PIO2_TX0 << 17);

    REG(PIO_FDEBUG) = FDEBUG_TXSTALL_SM0;            /* clear, then start */
    REG(PIO_CTRL) = 1u;                              /* SM0_ENABLE */
    g_running = true;

    lcd7_set_colour(0x001f);                         /* blue: unmistakably "on" */
    lcd7_set_backlight(100);
    printk("[LCD] 800x480 panel running: PCLK %lu kHz, %u x %u total, %lu.%lu Hz\n",
           (unsigned long)(CONFIG_CLK_SYS_HZ / LCD_CYCLES_PER_PCLK / 1000u),
           (unsigned)(LCD_H_PULSE + LCD_H_BACK + LCD_H_ACTIVE + LCD_H_FRONT), (unsigned)LCD_LINES,
           (unsigned long)(CONFIG_CLK_SYS_HZ / LCD_CYCLES_PER_PCLK /
                           ((LCD_H_PULSE + LCD_H_BACK + LCD_H_ACTIVE + LCD_H_FRONT) * LCD_LINES)),
           (unsigned long)((CONFIG_CLK_SYS_HZ / LCD_CYCLES_PER_PCLK * 10u /
                           ((LCD_H_PULSE + LCD_H_BACK + LCD_H_ACTIVE + LCD_H_FRONT) * LCD_LINES)) % 10u));
    return 0;
}

void lcd7_report(void) {
    if (!g_running) {
        cprintf("lcd: not running\n");
        return;
    }
    uint32_t fdebug = REG(PIO_FDEBUG);
    cprintf("lcd: running, colour 0x%04x, backlight %u%%\n", g_colour, g_brightness);
    cprintf("lcd: PIO2 SM0 at pc %lu, TX stall (underrun) %s; DMA read at 0x%08lx, ctrl 0x%08lx\n",
            (unsigned long)(REG(PIO_SM0_ADDR) & 0x1fu),
            (fdebug & FDEBUG_TXSTALL_SM0) ? "SEEN" : "none",
            (unsigned long)REG(DMA_READ_ADDR), (unsigned long)REG(DMA_CTRL_TRIG));
}

#else

int lcd7_init(void) { return -1; }
void lcd7_set_colour(uint16_t rgb565) { (void)rgb565; }
void lcd7_set_backlight(unsigned percent) { (void)percent; }
void lcd7_report(void) { }

#endif /* CONFIG_BOARD_RP2350 && CONFIG_LCD_PCLK_GPIO */
