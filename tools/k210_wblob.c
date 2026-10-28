/* K210 flash writer blob (tools/k210flash.py loads this over ISP, boots
 * it, and drives the NOR through it — our own writer end to end, so
 * provisioning never depends on a vendor stub's flash geometry or its
 * idea of which SPI block the board's NOR hangs off).
 *
 * WHICH FLASH (two chips, and the truth cost a day):
 *   The Maix Bit carries TWO 16MB NORs. The W25-series part hangs off
 *   the BROM's XPI window — the SPI3 SSIC at 0x54000000 — and is what
 *   the BROM and the ISP stub read. The GD25LQ (RDID C8 60 18) hangs
 *   off SPI0 at 0x52000000 through FPIOA-muxed breakout pads
 *   (27=SCLK fn17, 28=D0, 29=SS0, 26=D1+pullup) and is unreachable
 *   while the ISP stub owns the chip — RDID there only answered
 *   because the ISP stub itself was flash-read, and dies as soon as
 *   the stub is out of the way.
 *
 *   The LugalOS boot flash is the XPI part (SPI3): BROM boots from
 *   it, the ISP stub reads it for flash-mode, and our loader must
 *   live where BROM looks. Commands carry a bus select in addr bit31
 *   (set = SPI3/XPI = the boot flash, the default the host uses;
 *   clear = SPI0/GD25LQ for experiments).
 *
 *   SPI3 shares its pads with no other master once the ISP stub is
 *   gone — but the stub must be gone: while it is resident it holds
 *   SPI3, and a second master on the same pads answers garbage. The
 *   host therefore boots this blob BEFORE issuing any flash-read
 *   ops, and retries every op once against a re-silenced bus.
 *
 * TRANSFER SHAPE (measured on this silicon; none of it portable from
 * DWSSI documentation or from the vendor's EE-mode flow): standard
 * TMOD set once; SER raised before the FIFO pushes; TXFLR-level
 * pacing (the SR busy bit lies); answers begin at beat n_tx; RX
 * drained on every path including send-only, because the 16-deep
 * FIFO overflowing is how a page vanished once.
 *
 * Linked flat at 0x80000000; k210run's ISP memory-write places it and
 * boot starts it. It talks raw bytes on UARTHS.
 *
 * Frame (host -> blob): 'K' op addr[4LE] len[2LE] [payload:len] ck,
 * ck = sum of all preceding bytes & 0xFF. Payloads ride only with 'W'.
 * Responses: '+' ok, '-' error; 'R' answers exactly len raw bytes;
 * 'I' answers 3 bytes of RDID.
 *
 * Deliberately compiled WITHOUT the kernel's write floor: this blob is
 * the provisioning tool's arm, and provisioning writes the loader and
 * the boot records. */

#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define UARTHS            0x38000000UL
#define UART_TX           REG(UARTHS + 0x00)   /* bit31 full */
#define UART_RX           REG(UARTHS + 0x04)   /* bit31 empty; read pops */
#define UART_RXCTRL       REG(UARTHS + 0x0c)   /* bit0 rxenable */
#define UART_IE           REG(UARTHS + 0x10)

#define SYSCTL_CENT       REG(0x50440000UL + 0x28)
#define SYSCTL_PERI       REG(0x50440000UL + 0x2c)
#define FPIOA_FN(p)       REG(0x502B0000UL + 4u * (p))

#define SPI3_BASE         0x54000000UL
#define SPI0_BASE         0x52000000UL

static volatile uint32_t *sp;            /* current controller window */
#define SPI_CTRLR0        sp[0x00 / 4]
#define SPI_CTRLR1        sp[0x04 / 4]
#define SPI_SSIENR        sp[0x08 / 4]
#define SPI_SER           sp[0x10 / 4]
#define SPI_BAUDR         sp[0x14 / 4]
#define SPI_TXFLR         sp[0x20 / 4]
#define SPI_RXFLR         sp[0x24 / 4]
#define SPI_IMR           sp[0x2c / 4]
#define SPI_DR            sp[0x60 / 4]

#define CAPACITY          0x01000000u          /* GD25LQ128 */
#define SPIN_MAX          20000000u

static void tx1(uint8_t b) {
    while (UART_TX & (1u << 31)) ;
    UART_TX = b;
}
static uint8_t rx1(void) {
    for (;;) {
        uint32_t v = UART_RX;
        if (!(v & (1u << 31))) return (uint8_t)v;   /* read popped */
    }
}

/* Select the controller for the next command; bit31 of the frame's
 * address is the bus, and it is stripped before any flash use. */
static void bus_select(uint32_t addr) {
    /* Host sends bit31 set for the boot flash (SPI3/XPI); the SPI0
     * side stays addressable for probes. */
    sp = (addr & 0x40000000u) ? (volatile uint32_t *)SPI0_BASE
                              : (volatile uint32_t *)SPI3_BASE;
}

/* Full-duplex transfer. Echo/answer beats always arrive (the
 * controller is read/write symmetric here) so RX is drained on every
 * path: into rx[] when the caller wants it, into a sink otherwise —
 * an un-drained 260-beat page echo overflows the 16-deep FIFO, and
 * that overflow is what ate one NOR page silently. Answers for a
 * command of n_tx beats begin at received index n_tx; the caller (or
 * the 'R' op) slices them out of a single stream stored in rx. */
/* Full-duplex transfer. Two silicon facts (both measured the hard
 * way) shape this loop:
 *   * SER (CS) must be raised BEFORE the first FIFO push — an
 *     unselected slave means the controller never shifts;
 *   * the RX FIFO is only 4 entries deep and overflows silently, so
 *     RX is drained on every iteration of the pacer, not only when TX
 *     stalls. Answers for an n_tx-beat command begin at received
 *     index n_tx; the whole stream lands in rx (or a sink when the
 *     caller only sends). The 16-deep TX FIFO needs the same pacing:
 *     push only while TXFLR < 16. */
static uint32_t spi_xfer(const uint8_t *tx, uint32_t n_tx,
                         uint8_t *rx, uint32_t n_tot) {
    uint32_t rxgot = 0, sent = 0;
    /* Do NOT re-disable/re-enable the SSIC here: the enable edge puts
     * glitch clocks (and byte garbage) on the wire while CS floats,
     * which the NOR can latch as a partial command — observed as
     * randomly set BP bits and pages that program only halfway. The
     * controller keeps a coherent idle state between transfers; only
     * the RX drain is needed. */
    while (SPI_RXFLR & 0x3Fu) (void)SPI_DR;
    SPI_SER = 1;
    uint32_t spin = 0;
    while (rxgot < n_tot && spin++ < SPIN_MAX) {
        while (SPI_RXFLR & 0x3Fu) {
            uint8_t b = (uint8_t)SPI_DR;
            if (rx && rxgot < n_tot) rx[rxgot++] = b;
            else rxgot++;
        }
        if (sent < n_tot && (SPI_TXFLR & 0x3Fu) < 16u) {
            SPI_DR = (sent < n_tx) ? tx[sent] : 0xFFu;
            sent++;
        }
    }
    /* CS must stay down until SSIC has fully shifted the last beat. */
    while ((SPI_TXFLR & 0x3Fu) && spin-- < 2u * SPIN_MAX) ;
    for (volatile uint32_t d = 0; d < 200u; d++) ;
    SPI_SER = 0;
    return rxgot;
}

static uint8_t status_r(void) {
    uint8_t cmd = 0x05, ans[2] = {0xFF, 0xFF};
    uint32_t got = spi_xfer(&cmd, 1, ans, 2);
    return got > 1 ? ans[1] : 0xFF;   /* answer follows the command beat */
}
static int wait_wip(void) {
    for (uint32_t s = 0; s < SPIN_MAX; s++)
        if (!(status_r() & 1u)) return 0;
    return -1;
}
static void wren(void) { uint8_t c = 0x06; spi_xfer(&c, 1, 0, 1); }

static int op_cmd(uint8_t c, uint32_t addr) {
    if (wait_wip()) return -1;
    wren();
    uint8_t f[4] = { c, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8),
                     (uint8_t)addr };
    spi_xfer(f, 4, 0, 4);
    /* see the program-path note: WIP rises after CS falls */
    for (volatile uint32_t d = 0; d < 100000u; d++) ;
    return wait_wip();
}

/* entry glue: the shared minimal_k210_entry.S calls k210_probe_main and
 * carries a trap recorder this blob never arms */
uint64_t g_trap_ticks[8]; uintptr_t g_try_buf[8];
void trap_c(void *f) { (void)f; for (;;) ; }

static void rpt_en(void) {
    if ((SPI_SSIENR & 1u) == 0u) for (;;) ;
}

static void wblob_start(void) {
    SYSCTL_CENT |= (1u << 5);                 /* apb2 */
    /* Vendor spi_clk_init does more than the clock bit: the SPI clock
     * THRESHOLD (clk_th1, 8 bits per SPI at SYSCTL+0x3c) must be 0 or
     * the block runs too slow to shift, and peri_reset (SYSCTL+0x34,
     * spi3_reset = bit 9) must release the block — the ISP stub's
     * reset domain handoff leaves it held. Without these, SSIENR
     * reads 1 through the bus while no SCLK ever appears: RX beats
     * sampled after a transfer are just the stub's stale FIFO
     * residue, which is exactly how a phantom c86018 RDID kept
     * "working" for us. */
    /* THE key: SYSCTL peri register (+0x58) bit15 spi3_xip_en stays
     * asserted after BROM/ISP XPI reads, and while it is set the XPI
     * hardware owns the NOR pads — SSIC transfers clock nothing and
     * every RX beat is stale FIFO residue (the phantom C86018 that
     * answered for hours was the ISP stub's own JEDEC read left in
     * the RX FIFO). Clearing it hands the pads to SSIC master mode. */
    REG(0x50440000UL + 0x58) &= ~((1u << 15) | (1u << 12));
    REG(0x50440000UL + 0x3c) &= ~(0xFFu << 24);      /* spi3 threshold = 0 */
    REG(0x50440000UL + 0x3c) &= ~0xFFu;              /* spi0 threshold = 0 */
    SYSCTL_PERI |= (1u << 6)                  /* spi0_clk_en (struct order: */
                 | (1u << 9)                  /* spi3) | fpioa is bit 20 */
                 | (1u << 20)
                 | (1u << 5);                 /* gpio (pads for SPI0 fnmux) */
    /* Maix Bit 1.09 wires the NOR's HOLD# to pad IO3 and WP# to pad
     * IO23. Left as inputs the pins float (board pull values differ
     * per unit); driving both HIGH is the documented operating
     * state. GPIO pads need direction AND source set before data
     * reaches the pin (the source register is the pad enable). */
    {
        const uint32_t nfl = (1u << 16) | (3u << 5) | (1u << 12);
        /* FUNC_GPIO0 is 56, not 24 -- FUNC_GPIOHS0 is 24. Mapping pad 3
         * to "24+3" selects SPI3_D0 and fights the XPI bus (this
         * mistake poisoned every SPI3 read for a while). */
        FPIOA_FN(3)  = (56 + 3)  | nfl;
        FPIOA_FN(23) = (56 + 23) | nfl;
        REG(0x50200000UL + 0x04) |= (1u << 3) | (1u << 23);   /* dir: output */
        REG(0x50200000UL + 0x08) |= (1u << 3) | (1u << 23);   /* source: pad driver on */
        REG(0x50200000UL + 0x00) |= (1u << 3) | (1u << 23);   /* data: high  */
    }

    /* SPI0 reaches the flash pads only through FPIOA (the pads are the
     * breakout's, muxed; schematic nets SPI0_*): */
    /* flags as in the proven probe: poweron | (drive 3)<<5 | pulldown
     * clear | pullup — MISO (D1, pad 26) reads all-zero without the
     * pullup enable. The sdk's own init passes these unconditionally
     * (fioa_init in arch/.../start.S). */
    const uint32_t fl = (1u << 16) | (3u << 5) | (0u << 4) | (1u << 12);
    FPIOA_FN(27) = 17 | fl;                   /* SPI0_SCLK */
    FPIOA_FN(28) = 4  | fl;                   /* SPI0_D0  (MOSI) */
    FPIOA_FN(29) = 12 | fl;                   /* SPI0_SS0 */
    FPIOA_FN(26) = 5  | fl;                   /* SPI0_D1  (MISO) */
    UART_RXCTRL = 1;
    UART_IE = 0;
    /* Both controllers get the same quiet setup; the per-command
     * bus_select picks one. The 0/1 enable sequence here (and only
     * here — never mid-stream) flushes whatever the ISP stub left
     * behind; CS is deasserted for the whole init so the wire stays
     * clean. */
    sp = (volatile uint32_t *)SPI3_BASE;      /* default: the boot flash */
    SPI_SSIENR = 0;
    SPI_CTRLR0 = 0;
    SPI_CTRLR1 = 0;
    SPI_BAUDR  = 64;
    SPI_IMR    = 0;
    SPI_SSIENR = 1;
    sp = (volatile uint32_t *)SPI0_BASE;
    SPI_SSIENR = 0;
    SPI_CTRLR0 = 0;
    SPI_CTRLR1 = 0;
    SPI_BAUDR  = 64;
    SPI_IMR    = 0;
    SPI_SSIENR = 1;
    (void)rpt_en;

    /* Return the NOR to bare SPI mode: a part left in continuous
     * read, QPI, or deep power-down by a previous boot answers zeros
     * to everything. 0xFF releases DP, 66/99 is the software reset;
     * the working probe sent exactly this before RDID. */
    {
        uint8_t w = 0xFF, r1 = 0x66, r2 = 0x99, e9 = 0xE9;
        sp = (volatile uint32_t *)SPI3_BASE;
        spi_xfer(&w, 1, 0, 1);
        spi_xfer(&r1, 1, 0, 1);
        spi_xfer(&r2, 1, 0, 1);
        /* Force 3-byte address width. A previous owner (our own
         * earlier builds, BROM, the vendor stub) can leave 4-byte
         * mode latched, and every fixed-width 3-byte frame then
         * shifts by one byte: reads show a leading 0xFF, programs
         * land one byte early. E9h after the 66/99 reset pins the
         * width deterministically; 99h alone would already do it
         * unless the reset itself was ignored mid-busy. */
        spi_xfer(&e9, 1, 0, 1);
        /* Unprotect the whole array. Junk from earlier experiments
         * (or any firmware that set BP/SRWD) survives as status
         * 0x18-ish, and programs into protected blocks complete
         * WIP-clean while writing nothing — the worst possible
         * failure mode for a provisioning tool. */
        {
            uint8_t en = 0x06, wr[2] = { 0x01, 0x00 };
            spi_xfer(&en, 1, 0, 1);
            spi_xfer(wr, 2, 0, 2);
        }
        sp = (volatile uint32_t *)SPI0_BASE;
        spi_xfer(&w, 1, 0, 1);
        spi_xfer(&r1, 1, 0, 1);
        spi_xfer(&r2, 1, 0, 1);
        sp = (volatile uint32_t *)SPI3_BASE;
    }
    tx1('K');                                 /* hello: the blob is alive */
}

void k210_probe_main(void) {
    wblob_start();

    for (;;) {
        uint8_t h[8];
        uint32_t sum = 0;
        h[0] = rx1();
        if (h[0] != 'K') { tx1(h[0]); continue; }
        sum = 'K';
        for (int i = 1; i < 8; i++) { h[i] = rx1(); sum += h[i]; }
        uint8_t  op = h[1];
        uint32_t addr = (uint32_t)h[2] | ((uint32_t)h[3] << 8) |
                        ((uint32_t)h[4] << 16) | ((uint32_t)h[5] << 24);
        uint32_t len = (uint32_t)h[6] | ((uint32_t)h[7] << 8);
        bus_select(addr);
        addr &= 0x3FFFFFFFu;

        uint8_t payload[256];
        if (op == 'W') {
            if (len > 256) { tx1('-'); continue; }
            for (uint32_t i = 0; i < len; i++) { payload[i] = rx1(); sum += payload[i]; }
        }
        uint8_t ck = rx1();
        if (ck != (uint8_t)sum) { tx1('-'); continue; }
        if (addr + len > CAPACITY) { tx1('-'); continue; }

        int rc = 0;
        switch (op) {
        case 'E': rc = op_cmd(0x20, addr); break;           /* sector erase */
        case 'B': rc = op_cmd(0xD8, addr); break;           /* block erase  */
        case 'S': {                            /* RDSR, clearing stale BP locks */
            uint8_t en = 0x06, wr[2] = { 0x01, 0x00 };
            spi_xfer(&en, 1, 0, 1);
            spi_xfer(wr, 2, 0, 2);
            wait_wip();
            tx1(status_r());
            break;
        }
        case 'I': {                                         /* RDID answer */
            uint8_t cmd = 0x9F, ans[8] = {0};
            spi_xfer(&cmd, 1, ans, 4);
            for (int i = 1; i <= 3; i++) tx1(ans[i]);
            break;
        }
        case 'T': {              /* WRSR loopback: write BP0 into status, read it back */
            uint8_t wr[2] = { 0x01, 0x08 };
            if (!wait_wip()) {
                spi_xfer(wr, 2, 0, 2);        /* WRSR BP0 (needs WREN first) */
                wait_wip();
            }
            tx1('t'); tx1(status_r());
            break;
        }
        case 'U': {              /* WREN then WRSR (the legal order) */
            uint8_t en = 0x06;
            uint8_t wr[2] = { 0x01, 0x08 };
            spi_xfer(&en, 1, 0, 1);
            spi_xfer(wr, 2, 0, 2);
            wait_wip();
            tx1('u'); tx1(status_r());
            break;
        }
        case 'C': {              /* WRSR clear all protect bits */
            uint8_t en = 0x06;
            uint8_t wr[2] = { 0x01, 0x00 };
            spi_xfer(&en, 1, 0, 1);
            spi_xfer(wr, 2, 0, 2);
            wait_wip();
            tx1('c'); tx1(status_r());
            break;
        }
        case 'P': {              /* raw sector: erase then immediate FF-check, one CS frame each */
            uint8_t en = 0x06;
            uint8_t er[4] = { 0x20, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8),
                              (uint8_t)addr };
            spi_xfer(&en, 1, 0, 1);
            spi_xfer(er, 4, 0, 4);
            int ok = !wait_wip();
            tx1('p'); tx1(ok ? '+' : '-'); tx1(status_r());
            break;
        }
        case 'F': {              /* 0B fast read: cmd+addr+dummy, then len bytes */
            uint8_t c[5] = { 0x0B, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8),
                             (uint8_t)addr, 0xFF };
            static uint8_t buf[264];
            uint32_t got = spi_xfer(c, 5, buf, 5 + len);
            tx1('f'); tx1(got >= 5 + len ? '+' : '-');
            for (uint32_t i = 0; i < len && got >= 5 + len; i++) tx1(buf[5 + i]);
            break;
        }
        case 'G': {              /* 13 read, 4-byte address */
            uint8_t c[5] = { 0x13, 0, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8),
                             (uint8_t)addr };
            static uint8_t buf[264];
            uint32_t got = spi_xfer(c, 5, buf, 5 + len);
            tx1('g'); tx1(got >= 5 + len ? '+' : '-');
            for (uint32_t i = 0; i < len && got >= 5 + len; i++) tx1(buf[5 + i]);
            break;
        }
        case 'm': {                    /* alias of W (historical PP probe) */
            static uint8_t frame[4 + 256];
            frame[0] = 0x02;
            frame[1] = (uint8_t)(addr >> 16);
            frame[2] = (uint8_t)(addr >> 8);
            frame[3] = (uint8_t)addr;
            for (uint32_t i = 0; i < len; i++) frame[4 + i] = payload[i];
            if (wait_wip()) rc = -1;
            else { wren(); spi_xfer(frame, 4 + len, 0, 4 + len); rc = wait_wip(); }
            break;
        }
        case 'n': {                    /* plain 03h read (3-byte address) */
            uint8_t cmd[4] = { 0x03, (uint8_t)(addr >> 16),
                               (uint8_t)(addr >> 8), (uint8_t)addr };
            static uint8_t buf[260];
            uint32_t got = spi_xfer(cmd, 4, buf, 4 + len);
            if (got >= 4 + len)
                for (uint32_t i = 0; i < len; i++) tx1(buf[4 + i]);
            else
                tx1('-');
            break;
        }
        case 'e': rc = op_cmd(0x20, addr); break;   /* alias of E */
        case 'X': {   /* raw 03-read: report got count and every RX byte, unsliced */
            uint8_t c[4] = { 0x03, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8),
                             (uint8_t)addr };
            static uint8_t buf[64];
            uint32_t got = spi_xfer(c, 4, buf, 4 + len);
            tx1('x'); tx1((uint8_t)got); tx1((uint8_t)(got >> 8));
            for (uint32_t i = 0; i < 4 + len; i++) tx1(buf[i]);
            break;
        }
        case 'V': {   /* dump sysctl words the SPI path runs on: clk_en_cent,
                        clk_en_peri, peri_reset, clk_th1, and SPI3 ctrlr0+baudr */
            uint32_t w[8] = { REG(0x50440000UL + 0x28), REG(0x50440000UL + 0x2c),
                              REG(0x50440000UL + 0x34), REG(0x50440000UL + 0x3c),
                              REG(SPI3_BASE), REG(SPI3_BASE + 0x14),
                              REG(0x50440000UL + 0x58), REG(SPI3_BASE + 0x28) };
            for (int i = 0; i < 8; i++)
                for (int b = 0; b < 4; b++) tx1((uint8_t)(w[i] >> (8*b)));
            for (int i = 6; i < 8; i++)
                for (int b = 0; b < 4; b++) tx1((uint8_t)(w[i] >> (8*b)));
            break;
        }
        case 'D':                                           /* set BAUDR */
            SPI_SSIENR = 0;
            SPI_BAUDR = len;
            SPI_SSIENR = 1;
            tx1('+');
            break;
        case 'W': {
            static uint8_t frame[4 + 256];
            frame[0] = 0x02;
            frame[1] = (uint8_t)(addr >> 16);
            frame[2] = (uint8_t)(addr >> 8);
            frame[3] = (uint8_t)addr;
            for (uint32_t i = 0; i < len; i++) frame[4 + i] = payload[i];
            if (wait_wip()) rc = -1;
            else {
                wren();
                spi_xfer(frame, 4 + len, 0, 4 + len);
                /* The chip raises WIP some microseconds after the CS
                 * fall; an RDSR issued instantly can look "idle" and
                 * let the next command collide with the running
                 * program. Coarse settle before the first poll. */
                for (volatile uint32_t d = 0; d < 100000u; d++) ;
                rc = wait_wip();
            }
            break;
        }
        case 'R': {
            uint8_t cmd[4] = { 0x03, (uint8_t)(addr >> 16),
                               (uint8_t)(addr >> 8), (uint8_t)addr };
            static uint8_t buf[260];
            uint32_t got = spi_xfer(cmd, 4, buf, 4 + len);
            if (got >= 4 + len)
                for (uint32_t i = 0; i < len; i++) tx1(buf[4 + i]);
            else
                tx1('-');
            break;
        }
        default: rc = -1; break;
        }
        if (op == 'W' || op == 'E' || op == 'B' || op == 'm' || op == 'e')
            tx1(rc ? '-' : '+');
    }
}
