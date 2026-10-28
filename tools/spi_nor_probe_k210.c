/* K210 SPI-NOR truth probe v3 (65.5).
 *
 * v1/v2 failed with all-zero reads because the SoC map they used was
 * wrong on three points, now corrected from
 * arduino-k210/cores/k210/k210-sdk/bsp/k210_platform.h (complete vendor
 * header):
 *   - SYSCTL is 0x50440000 (NOT 0x50020000 -- that window read-zeroed
 *     every clock enable write, so SPI0 never got its device clock)
 *   - GPIOHS is 0x38001000 with the SiFive-style layout
 *     (input_val 0x00, input_en 0x04, output_en 0x08, output_val 0x0c,
 *     pullup_en 0x10) -- 0x1A000000 does not exist and faulted
 *   - FPIOA is 0x502B0000
 * Board nets (Maix Bit schematic 1.09, extracted from the PDF): the NOR
 * hangs off SPI0 with SCLK=IO_27, MOSI=IO_28, CS0=IO_29, MISO=IO_26;
 * IO_31/32 are IO2(WP#)/IO3(HOLD#) and MUST be held high or the part
 * mutes. FPIOA function 24+n drives GPIOHS bit n (function 0 is slow
 * GPIO, which no HS pad may use).
 *
 * Phase 1: controller RDID/RDSR/READ. Phase 2: GPIO bit-bang RDID to
 * separate controller-from-pad routing from part state.
 */

#include <stdint.h>
#include <stdbool.h>

#define REG(a)  (*(volatile uint32_t *)(uintptr_t)(a))

#define UARTHS          0x38000000UL
#define GPIOHS          0x38001000UL
#define GPIO_IN         REG(GPIOHS + 0x00)
#define GPIO_IN_EN      REG(GPIOHS + 0x04)
#define GPIO_OUT_EN     REG(GPIOHS + 0x08)
#define GPIO_OUT        REG(GPIOHS + 0x0c)
#define GPIO_PU         REG(GPIOHS + 0x10)
#define SYSCTL          0x50440000UL
#define CLK_EN_CENT     REG(SYSCTL + 0x28)
#define CLK_EN_PERI     REG(SYSCTL + 0x2c)
#define FPIOA           0x502B0000UL
#define FPIOA_FN(p)     REG(FPIOA + 4u * (p))
#define SPI0            0x54000000UL  /* SPI3: the NOR's controller, dedicated pads */
#define SPI_CTRLR0      REG(SPI0 + 0x00)
#define SPI_CTRLR1      REG(SPI0 + 0x04)
#define SPI_SSIENR      REG(SPI0 + 0x08)
#define SPI_SER         REG(SPI0 + 0x10)
#define SPI_BAUDR       REG(SPI0 + 0x14)
#define SPI_TXFTLR      REG(SPI0 + 0x18)
#define SPI_RXFTLR      REG(SPI0 + 0x1c)
#define SPI_TXFLR       REG(SPI0 + 0x20)
#define SPI_RXFLR       REG(SPI0 + 0x24)
#define SPI_SR          REG(SPI0 + 0x28)
#define SPI_IMR         REG(SPI0 + 0x2c)
#define SPI_DR          REG(SPI0 + 0x60)

#define PERI_GPIO       (1u << 3)
#define PERI_SPI0       (1u << 7)
#define PERI_FPIOA      (1u << 18)
#define PERI_GPIOHS     (1u << 23)
#define CENT_APB2       (1u << 5)

/* GPIOHS bit indices used for bit-banging (FPIOA fn = 24 + bit). */
#define HS_MISO 2
#define HS_CLK  3
#define HS_MOSI 4
#define HS_CS   5
#define HS_WP   7
#define HS_HOLD 8

static void putc_(char c) {
    while (REG(UARTHS) & (1u << 31))
        ;
    REG(UARTHS) = (uint32_t)c;
}
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puth(uint32_t v) {
    static const char hx[] = "0123456789abcdef";
    int started = 0;
    for (int sh = 28; sh >= 0; sh -= 4) {
        uint32_t d = (v >> sh) & 0xFu;
        if (d) started = 1;
        if (started) putc_(hx[d]);
    }
    if (!started) putc_('0');
}
static void rpt(const char *label, uint32_t v) {
    puts_("RPT "); puts_(label); putc_(' '); puth(v); puts_("\n");
}
static void delay(volatile uint32_t n) { while (n--) ; }

static uint32_t spi_xfer(const uint8_t *tx, uint32_t n_tx,
                         uint8_t *rx, uint32_t n_tot) {
    uint32_t rxgot = 0, sent = 0;
    /* The RX FIFO survives commands on this part (nothing clears it but
     * a disable), and stale beats shift the answer window -- measured.
     * Drain, then disable/enable to reset both FIFOs, before framing. */
    while (SPI_RXFLR & 0x3Fu)
        (void)SPI_DR;
    SPI_SSIENR = 0;
    SPI_SSIENR = 1;
    SPI_SER = 1;
    while (sent < n_tot) {
        uint32_t spin = 0;
        while ((SPI_TXFLR & 0x3Fu) >= 16u) {
            while (rxgot < n_tot && (SPI_RXFLR & 0x3Fu))
                rx[rxgot++] = (uint8_t)SPI_DR;
            if (++spin > 2000000u) break;
        }
        SPI_DR = (sent < n_tx) ? tx[sent] : 0xFFu;
        sent++;
    }
    for (uint32_t spin = 0; rxgot < n_tot && spin < 2000000u; spin++)
        while (rxgot < n_tot && (SPI_RXFLR & 0x3Fu))
            rx[rxgot++] = (uint8_t)SPI_DR;
    delay(400);
    SPI_SER = 0;
    delay(400);
    return rxgot;
}

static int spi_cmd(const uint8_t *cmd, uint32_t n_tx,
                   uint8_t *rx, uint32_t n_extra) {
    uint8_t buf[24] = {0};
    uint32_t got = spi_xfer(cmd, n_tx, buf, n_tx + n_extra);
    if (rx) for (uint32_t i = 0; i < got; i++) rx[i] = buf[i];
    return (int)got;
}

uint64_t g_trap_ticks[8]; uintptr_t g_try_buf[8];
void trap_c(void *f) { (void)f; for (;;) ; }
void k210_probe_main(void);
int main(void) { k210_probe_main(); for (;;) ; }

void k210_probe_main(void) {
    CLK_EN_CENT |= CENT_APB2;
    CLK_EN_PERI |= PERI_FPIOA | PERI_GPIOHS | PERI_GPIO | PERI_SPI0;
    rpt("peri", CLK_EN_PERI);           /* must be nonzero now */

    /* WP#/HOLD# released first -- before any flash traffic. */
    /* No FPIOA: SPI3 drives the NOR's dedicated pads directly. */

    SPI_SSIENR = 0;
    SPI_CTRLR0 = 0;
    SPI_CTRLR1 = 0;
    SPI_BAUDR  = 64;
    SPI_TXFTLR = 0;
    SPI_RXFTLR = 0;
    SPI_IMR    = 0;
    SPI_SSIENR = 1;
    rpt("ssi_sr", SPI_SR);

    uint8_t ff = 0xFF, r_en = 0x66, r_st = 0x99;
    spi_cmd(&ff, 1, 0, 0);
    spi_cmd(&ff, 1, 0, 0);
    spi_cmd(&r_en, 1, 0, 0);            /* enable reset */
    spi_cmd(&r_st, 1, 0, 0);            /* reset: exits QPI etc. */
    delay(200000);

    uint8_t id[16];
    uint8_t cmd = 0x9F;
    int got = spi_cmd(&cmd, 1, id, 4);
    puts_("RPT rdid ");
    for (int i = 0; i < got; i++) { puth(id[i]); putc_(' '); }
    puts_("\n");
    rpt("jedec", ((uint32_t)id[1] << 16) | ((uint32_t)id[2] << 8) | id[3]);

    cmd = 0x05;
    got = spi_cmd(&cmd, 1, id, 2);
    rpt("rdsr", got > 1 ? id[1] : 0xBADu);

    /* Word-window dump. This SSI variant populates the low byte of the
     * data register (a byte read with the high bits forced to 0xff
     * shows the pattern exactly); the window below is read as 32-bit
     * words for an unambiguous transcript of the first 64 bytes and a
     * couple of landmarks. */
    for (uint32_t base = 0; base <= 0x800000; base += (base ? 0x800000 : 0x10)) {
        for (int w = 0; w < (base ? 1 : 16); w++) {
            uint8_t rd[4] = {0x03, (uint8_t)((base + w*4) >> 16),
                             (uint8_t)((base + w*4) >> 8), (uint8_t)(base + w*4)};
            /* Answer beats start after the 4 command beats. */
            uint8_t ans[16];
            uint32_t word = 0;
            int got = spi_cmd(rd, 4, ans, 4);
            for (int i = 0; i < 4 && 4 + i < got; i++)
                word |= (uint32_t)ans[4 + i] << (8*i);
            puts_("RPT w"); puth(base + w*4); putc_(' '); puth(word); puts_("\n");
        }
    }
    puts_("RPT done\n");
    for (;;)
        ;
}
