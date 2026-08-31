/*
 * Infineon/Cypress CYW43439 wireless chip -- R5 milestone 1,
 * plan/phase19_ip_stack_and_ethernet.md. See drivers/include/drivers/cyw43.h
 * for what this is and where it sits.
 *
 * This file is the bus layer only: RP2350's PIO0 block, programmed by hand
 * (no PIO exists anywhere else in this tree yet, so there is no assembler
 * to lean on -- the six-instruction gSPI program below is the same
 * `spi_gap01_sample0` program the Pico SDK's own cyw43 driver uses,
 * hand-assembled into the raw 16-bit PIO instruction words one at a time)
 * plus the gSPI command framing (georgerobotics/cyw43-driver's
 * src/cyw43_spi.c, the driver the Pico SDK itself vendors). Firmware
 * download, the ioctl layer, and the netif_t seam are later milestones,
 * not built here -- this milestone only proves the bus: can PIO0 clock a
 * command out over one bidirectional wire and read a known pattern back.
 *
 * gSPI has one data pin (WL_D) that flips direction mid-transfer -- out
 * for the command word (and any write data), in for whatever the chip
 * answers -- which is why this cannot be the RP2350's hardware SPI
 * peripheral (as ENC28J60 on SPI0 is) and has to be PIO. The PIO program
 * shifts `x` bits out, flips WL_D to input, shifts `y` bits in; `x`/`y`
 * (bit counts, not byte counts) are loaded into the SM before each
 * transfer via a direct FIFO push + an injected `OUT X,32`/`OUT Y,32`,
 * exactly mirroring cyw43_spi_transfer()'s own sequence.
 *
 * No DMA driver exists in this tree either (also a first): the reference
 * DMAs FIFO<->memory, this file polls PIO_FSTAT's TXFULL/RXEMPTY bits and
 * pushes/pulls a word at a time. Slower, not wrong -- bring-up traffic
 * here is a handful of 32-bit words, not the ~230 KB firmware blob (that
 * milestone, when it arrives, may need this revisited). This substitution
 * is exact: DMA's own `bswap` (a *full* 4-byte reversal, RP2350 datasheet)
 * is reproduced word-for-word by pio_gspi_transfer()'s own
 * __builtin_bswap32() calls.
 *
 * A second, unrelated swap sits on top of that for exactly two calls
 * (read_reg_u32_swap()/write_reg_u32_swap(), the bring-up-only ones used
 * here): the reference's own software SWAP32() macro. It is easy to
 * assume this is the same full reversal as DMA's bswap -- it looks like
 * one operating on the same 32-bit words right next to it -- but it is
 * not: SWAP32 swaps bytes only *within* each 16-bit half (see
 * swap16x2() below, where this cost a full round of hardware testing:
 * the PIO mechanics turned out to be correct on the first working build,
 * but every command still came back wrong because this file used a full
 * reversal here too instead of a half-swap).
 */

#include "drivers/cyw43.h"
#include "kernel/time.h"
#include "kernel/printk.h"
#include "lugalos_config.h"

#include <stdint.h>
#include <string.h>

#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_WL_CS_GPIO)

#define REG(addr) (*(volatile uint32_t *)(addr))

/* --- shared RP2350 blocks, same addresses drivers/enc28j60_rp2350.c uses -- */

#define RESETS_BASE             0x40020000UL
#define RESETS_RESET_CLR        (RESETS_BASE + 0x3000 + 0x0)
#define RESETS_RESET_DONE       (RESETS_BASE + 0x8)
#define RESETS_PIO0_BIT         (1u << 11)

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

/* Same reasoning as drivers/enc28j60_rp2350.c and drivers/spisd_rp2350.c:
 * RP2350 gates every peripheral to Secure-privileged by default, upstream
 * of PMP, so PIO0 and these pins must be opened here (still M-mode at
 * init) or a later U-mode conversion starts with a load access fault. */
#define ACCESSCTRL_BASE          0x40060000UL
#define ACCESSCTRL_GPIO_NSMASK0  (ACCESSCTRL_BASE + 0x0c)
#define ACCESSCTRL_PIO0          (ACCESSCTRL_BASE + 0x4c)
#define ACCESSCTRL_NSP           (1u << 1)
#define ACCESSCTRL_NSU           (1u << 0)
#define ACCESSCTRL_WRITE_PASSWORD 0xacce0000UL

#define WL_ON_PIN   CONFIG_WL_ON_GPIO
#define WL_DATA_PIN CONFIG_WL_DATA_GPIO
#define WL_CS_PIN   CONFIG_WL_CS_GPIO
#define WL_CLK_PIN  CONFIG_WL_CLK_GPIO
#define WL_ON_MASK   (1u << WL_ON_PIN)
#define WL_DATA_MASK (1u << WL_DATA_PIN)
#define WL_CS_MASK   (1u << WL_CS_PIN)

#define GPIO_FUNCSEL_SIO  5
#define GPIO_FUNCSEL_PIO0 6

/* --- PIO0 register block (RP2350 datasheet, "PIO" register block) ------ */

#define PIO0_BASE          0x50200000UL
#define PIO_CTRL           (PIO0_BASE + 0x00)
#define PIO_FSTAT          (PIO0_BASE + 0x04)
#define PIO_FDEBUG         (PIO0_BASE + 0x08)
#define PIO_TXF0           (PIO0_BASE + 0x10)
#define PIO_RXF0           (PIO0_BASE + 0x20)
#define PIO_INPUT_SYNC_BYPASS (PIO0_BASE + 0x38)
#define PIO_INSTR_MEM0     (PIO0_BASE + 0x48)
#define PIO_SM0_BASE       (PIO0_BASE + 0xc8)
#define PIO_SM_STRIDE      0x18u   /* SM1_CLKDIV - SM0_CLKDIV */
#define PIO_SM_CLKDIV(n)   (PIO_SM0_BASE + (n) * PIO_SM_STRIDE + 0x00)
#define PIO_SM_EXECCTRL(n) (PIO_SM0_BASE + (n) * PIO_SM_STRIDE + 0x04)
#define PIO_SM_SHIFTCTRL(n) (PIO_SM0_BASE + (n) * PIO_SM_STRIDE + 0x08)
#define PIO_SM_ADDR(n)     (PIO_SM0_BASE + (n) * PIO_SM_STRIDE + 0x0c)
#define PIO_SM_INSTR(n)    (PIO_SM0_BASE + (n) * PIO_SM_STRIDE + 0x10)
#define PIO_SM_PINCTRL(n)  (PIO_SM0_BASE + (n) * PIO_SM_STRIDE + 0x14)

#define PIO_FSTAT_TXFULL_LSB  16
#define PIO_FSTAT_RXEMPTY_LSB 8

#define PIO_SM 0u /* SM0: the only state machine this driver uses. */

/* --- the gSPI PIO program, hand-assembled ------------------------------
 *
 * Source (Pico SDK, cyw43_bus_pio_spi.pio, program `spi_gap01_sample0`,
 * "for high cpu speed" per its own comment -- matches this board's 150 MHz
 * sys_clk same as drivers/enc28j60_rp2350.c's SPI0 choice):
 *
 *   .side_set 1
 *   lp:      out pins, 1   side 0
 *            jmp x-- lp    side 1
 *   lp1_end: set pindirs,0 side 0
 *            nop           side 1
 *   lp2:     in pins, 1    side 0
 *            jmp y-- lp2   side 1
 *   end:
 *
 * Loaded starting at instruction memory address 0 (this driver owns PIO0
 * outright -- nothing else shares its instruction memory, so there is no
 * offset to add, unlike a multi-program PIO the Pico SDK has to share).
 * PIO instruction encoding (RP2040/2350 datasheet, "PIO instruction
 * encoding"): bits[15:13] opcode, bits[12:8] delay/side-set (top 1 bit is
 * side-set here, since `.side_set 1` with no `opt`; low 4 bits delay,
 * always 0 -- this program uses none), then an 8-bit operand field whose
 * shape depends on the opcode. `nop` assembles to the standard PIO idiom
 * `mov y, y` (a destination write that changes nothing).
 *
 *   addr 0  out pins,1   side 0   -> 0110 0000 0000 0001  0x6001
 *   addr 1  jmp x-- 0    side 1   -> 0001 0000 0100 0000  0x1040
 *   addr 2  set pindirs,0 side 0  -> 1110 0000 1000 0000  0xE080  (lp1_end)
 *   addr 3  mov y,y      side 1   -> 1011 0000 0100 0010  0xB042  (nop)
 *   addr 4  in pins,1    side 0   -> 0100 0000 0000 0001  0x4001  (lp2)
 *   addr 5  jmp y-- 4    side 1   -> 0001 0000 1000 0100  0x1084  (end-1)
 *
 * Cross-checked against ~/gith/pico/pico-sdk/tools/pioasm (built locally,
 * run against this exact program lifted verbatim from
 * cyw43_bus_pio_spi.pio) after the very first hardware run of this driver
 * stalled: pioasm's own hex output is 6001 1040 e080 b042 4001 1084 --
 * catching a real bug in the hand-assembly above, addr 4's opcode field
 * one bit low (0x2000 is WAIT's base, not IN's -- 0x4000). Left the wrong
 * derivation crossed out mentally by writing the right one in its place;
 * the point of running pioasm at all was to stop trusting hand arithmetic
 * for this specific field, not just this once.
 *
 * SPI_OFFSET_LP1_END = 2 (wrap target for a write-only transfer: loop
 * just addr 0-1). SPI_OFFSET_END = 6 (wrap target for a full transfer:
 * loop addr 0-5). Both are load-offset-relative and the load offset is
 * always 0 here, so they double as final wrap-top values (minus 1, set
 * below).
 */
static const uint16_t g_gspi_prog[6] = {
    0x6001, 0x1040, 0xE080, 0xB042, 0x4001, 0x1084,
};
#define GSPI_OFFSET_LP1_END 2

#define GSPI_OFFSET_END 6

/* Bring-up-only gSPI protocol constants (georgerobotics/cyw43-driver,
 * src/cyw43_spi.h / cyw43_internal.h). */
#define BUS_FUNCTION 0u
#define TEST_PATTERN 0xFEEDBEADu
#define SPI_READ_TEST_REGISTER 0x0014u
#define SPI_BUS_CONTROL        0x0000u
#define WORD_LENGTH_32          0x01u
#define ENDIAN_BIG              0x02u
#define HIGH_SPEED_MODE         0x10u
#define INTERRUPT_POLARITY_HIGH 0x20u
#define WAKE_UP                 0x80u
#define INTR_WITH_STATUS        0x02u

/* --- GPIO/pad setup ------------------------------------------------------ */

static void wl_gpio_setup(void) {
    REG(RESETS_RESET_CLR) = RESETS_PIO0_BIT;
    int timeout = 10000;
    while (!(REG(RESETS_RESET_DONE) & RESETS_PIO0_BIT) && --timeout > 0);

    /* Open the access gates BEFORE muxing any pin to PIO0, not after.
     * Order matters here in a way it does not for the SIO pins below:
     * writing ACCESSCTRL_PIO0 resets the IO mux of every pad currently
     * routed to PIO0 back to FUNCSEL_NULL (0x1f), which is sound
     * security behaviour -- a peripheral's access attributes changing
     * out from under a pin should not leave that pin still wired to it
     * -- but it silently undid this driver's own funcsel writes when
     * they came first. Measured directly: the writes landed (readback
     * 0x06), and read back 0x1f again immediately after this block,
     * while the two SIO-muxed pins were untouched. That left the state
     * machine's clock driving nothing, and cost this bring-up most of
     * its debugging. */
    REG(ACCESSCTRL_GPIO_NSMASK0) |= (WL_ON_MASK | WL_DATA_MASK | WL_CS_MASK |
                                      (1u << WL_CLK_PIN));
    REG(ACCESSCTRL_PIO0) = ACCESSCTRL_WRITE_PASSWORD | REG(ACCESSCTRL_PIO0)
                           | ACCESSCTRL_NSP | ACCESSCTRL_NSU;

    /* WL_ON: plain push-pull output, pull-up so the module stays held in
     * reset (active-high enable, so pull-up is the "off" side) if this
     * code hasn't run yet -- same 0x5A pad convention as every other
     * plain digital pin in this tree (drivers/enc28j60_rp2350.c's
     * CS/RST). */
    REG(IO_BANK0_CTRL(WL_ON_PIN)) = GPIO_FUNCSEL_SIO;
    REG(PADS_BANK0_PAD(WL_ON_PIN)) = 0x5A;
    REG(SIO_GPIO_OE_SET) = WL_ON_MASK;
    REG(SIO_GPIO_OUT_CLR) = WL_ON_MASK; /* held in reset until wl_reset() */

    /* WL_CS: plain SIO output, deselected (high) -- gSPI's CS is driven
     * directly by this code, never by PIO (cyw43-driver's own cs_set()
     * uses a plain gpio_put(), not the state machine). */
    REG(IO_BANK0_CTRL(WL_CS_PIN)) = GPIO_FUNCSEL_SIO;
    REG(PADS_BANK0_PAD(WL_CS_PIN)) = 0x5A;
    REG(SIO_GPIO_OE_SET) = WL_CS_MASK;
    REG(SIO_GPIO_OUT_SET) = WL_CS_MASK;

    /* WL_D and WL_CLK: PIO0 owns these permanently from here on. Pad
     * config for WL_D matches the chip's own bidirectional-pin needs
     * (schmitt + pull-down, cyw43-driver's gpio_set_pulls(..,false,true)
     * + gpio_set_input_hysteresis_enabled(..,true)) -- IE=1,DRIVE=4mA,
     * PDE=1,SCHMITT=1, everything else 0, which is this pad's own reset
     * value (0x56); written explicitly rather than left implicit so the
     * requirement is visible here rather than assumed from silicon
     * defaults. WL_CLK gets 12 mA drive + slew-fast, matching
     * cyw43-driver's own PADS_DRIVE_STRENGTH choice for the pin doing
     * all the SPI clock switching. */
    REG(IO_BANK0_CTRL(WL_DATA_PIN)) = GPIO_FUNCSEL_PIO0;
    /* No pull, schmitt, 12 mA, fast slew (0x73). Not the pull-down the
     * SDK's gpio_set_pulls(DATA,false,true) implies: embassy-rs's
     * independent driver sets this pin Pull::None + 12mA + SlewRate::Fast,
     * and that is the configuration that matches what this bus actually
     * needs at 37.5 MHz. A pull-down fighting the line, driven by a 4 mA
     * slow-slew pad, is what every read here looked like -- a slowly
     * rising edge sampled as eight bit-times of zero before anything
     * legible. */
    REG(PADS_BANK0_PAD(WL_DATA_PIN)) = 0x73;
    REG(IO_BANK0_CTRL(WL_CLK_PIN)) = GPIO_FUNCSEL_PIO0;
    REG(PADS_BANK0_PAD(WL_CLK_PIN)) = 0x77;
    /* Input synchroniser bypassed on the data pin, as both the SDK and
     * embassy-rs do. */
    REG(PIO_INPUT_SYNC_BYPASS) |= (1u << WL_DATA_PIN);
}

/* Datasheet-mandated power-up sequence (cyw43-driver's cyw43_spi_reset()):
 * hold WL_ON low, release, then give the chip time to bring its own
 * clocks up before the bus is touched. */
static void wl_reset(void) {
    REG(SIO_GPIO_OUT_CLR) = WL_ON_MASK;
    time_delay_us(20000);
    REG(SIO_GPIO_OUT_SET) = WL_ON_MASK;
    /* Reference uses 250ms here; every command still reads back all-1s at
     * that delay on this specific hardware, with everything else now
     * verified byte-for-byte against the SDK. One remaining untested
     * variable rather than a new guess: is this chip slower to settle
     * than the reference's default assumes? 2s to find out cheaply,
     * revert to 250ms once this stops being a live question either way. */
    time_delay_us(2000000);
}

/* --- PIO0 SM0 bring-up --------------------------------------------------- */

/* Force one instruction to execute immediately, regardless of the SM's
 * current PC or enable state (RP2040/2350 datasheet: writing SMx_INSTR
 * latches an instruction that executes on the next clock, without being
 * fetched from -- or written to -- instruction memory). Used exactly the
 * way cyw43-driver's own pio_sm_exec() calls are used: one-off pin setup
 * outside the program's normal loop. */
static void pio_exec(uint16_t instr) {
    REG(PIO_SM_INSTR(PIO_SM)) = instr;
}

static void pio_gspi_init(void) {
    /* PIO_INSTR_MEMn is write-only on this silicon (RP2350 datasheet:
     * ACCESS "WO") -- an earlier version of this function tried to
     * verify these writes by reading them back and chased a phantom bug
     * for a round of hardware testing before that turned up. Nothing to
     * verify here; loaded on faith the way the reference driver does. */
    for (unsigned i = 0; i < 6; i++) {
        REG(PIO_INSTR_MEM0 + i * 4) = g_gspi_prog[i];
    }

    /* CLKDIV: sys_clk/2, matching cyw43-driver's own default
     * (CYW43_PIO_CLOCK_DIV_INT=2, FRAC8=0) rather than inventing a new
     * number -- this program shifts one bit per two SM clocks, so the
     * wire ends up at sys_clk/4. */
    REG(PIO_SM_CLKDIV(PIO_SM)) = (2u << 16);

    /* PINCTRL: OUT/SET/IN all target WL_D (1 pin each), side-set targets
     * WL_CLK (1 pin, no "opt" -- SIDE_EN stays 0, the side-set field is
     * never shared with delay). */
    uint32_t pinctrl = (1u << 29) |               /* SIDESET_COUNT */
                       (1u << 26) |                /* SET_COUNT */
                       (1u << 20) |                /* OUT_COUNT */
                       ((uint32_t)WL_DATA_PIN << 15) | /* IN_BASE */
                       ((uint32_t)WL_CLK_PIN << 10) |  /* SIDESET_BASE */
                       ((uint32_t)WL_DATA_PIN << 5) |  /* SET_BASE */
                       ((uint32_t)WL_DATA_PIN << 0);   /* OUT_BASE */
    REG(PIO_SM_PINCTRL(PIO_SM)) = pinctrl;

    /* SHIFTCTRL: autopull/autopush at 32 bits, shift left both ways
     * (MSB of whatever 32-bit word is loaded goes out/arrives first) --
     * matches cyw43-driver's sm_config_set_{in,out}_shift(.., false, true,
     * 32) calls exactly (shift_right=false -> the *_SHIFTDIR bits stay
     * 0). Threshold 32 is encoded as 0 (PIO's own "0 means 32" rule). */
    REG(PIO_SM_SHIFTCTRL(PIO_SM)) = (1u << 17) | (1u << 16); /* AUTOPULL|AUTOPUSH */

    /* EXECCTRL: wrap the full loop (0..GSPI_OFFSET_END-1) by default --
     * pio_gspi_transfer() narrows this to the write-only loop when there
     * is nothing to read back. */
    REG(PIO_SM_EXECCTRL(PIO_SM)) = ((GSPI_OFFSET_END - 1u) << 12) | (0u << 7);

    /* WL_CLK is PIO-driven output, permanently, from here on -- the one
     * pin the program's own instructions never set a direction for
     * (side-set drives its *value*, not its direction). Set via the same
     * instruction-injection trick as everything else here: point SET at
     * WL_CLK just long enough to run one `SET PINDIRS,1`, then restore
     * PINCTRL to the WL_D-based layout the program actually runs with. */
    REG(PIO_SM_PINCTRL(PIO_SM)) = (1u << 26) | ((uint32_t)WL_CLK_PIN << 5); /* SET_BASE=CLK, SET_COUNT=1 */
    pio_exec(0xE081); /* set pindirs,1 */
    REG(PIO_SM_PINCTRL(PIO_SM)) = pinctrl;

    /* Hold WL_D as a driven LOW output across the chip's power-up.
     * Both references do this and it is not cosmetic: the SDK's
     * gpio_setup() drives DATA low *after* its PIO init and before the
     * WL_ON pulse, and embassy sets both clk and io Level::Low before
     * its own reset. Broadcom parts strap bus mode (gSPI vs SDIO) from
     * pin levels at reset, so a data line left floating or held high
     * while WL_ON rises can bring the chip up on the wrong bus
     * entirely -- alive, but deaf to every gSPI command, which is
     * exactly the symptom here. */
    REG(PIO_SM_PINCTRL(PIO_SM)) = (1u << 26) | ((uint32_t)WL_DATA_PIN << 5);
    pio_exec(0xE081); /* set pindirs,1 -- WL_D is an output ... */
    pio_exec(0xE000); /* set pins,0    -- ... driven low */
    REG(PIO_SM_PINCTRL(PIO_SM)) = pinctrl;
}

/* --- gSPI bus transfer ---------------------------------------------------
 *
 * Mirrors cyw43_spi_transfer(): tx_len/rx_len are byte counts; when both
 * tx and rx are non-NULL this is a full-duplex transfer (rx_len must be
 * >= tx_len; the leading tx_len bytes of rx are the electrical gap while
 * the command word itself was going out on this half-duplex pin -- zeroed,
 * not captured -- and the real answer starts at rx+tx_len); when rx is
 * NULL this is a write-only transfer. Read-only
 * (tx NULL) is not implemented -- cyw43-driver doesn't implement it
 * either (panic_unsupported()), nothing in this bus protocol needs it. */
/* Bound on this file's own polling loops: never spin on hardware
 * forever. A bug here has already once hung the whole board solid enough
 * that even the console stopped responding -- every wait below now
 * counts down instead, and reports what it saw rather than going silent. */
#define PIO_POLL_LIMIT 1000000u


static bool pio_gspi_transfer(const uint8_t *tx, uint32_t tx_len,
                               uint8_t *rx, uint32_t rx_len) {
    REG(PIO_CTRL) &= ~(1u << PIO_SM); /* SM_ENABLE[SM] = 0 */

    uint32_t wrap_top = rx ? (GSPI_OFFSET_END - 1u) : (GSPI_OFFSET_LP1_END - 1u);
    REG(PIO_SM_EXECCTRL(PIO_SM)) = (wrap_top << 12) | (0u << 7);

    /* Drain any stale words left by an aborted prior transfer -- the
     * equivalent of cyw43-driver's pio_sm_clear_fifos(), done here by
     * polling instead of the FJOIN toggle trick, since this driver has
     * no reason to touch FJOIN at all otherwise. Bounded at the FIFO's
     * own depth (4 words) plus margin, not PIO_POLL_LIMIT -- a drain that
     * doesn't finish in a handful of reads means RXEMPTY itself is stuck,
     * which no amount of extra polling fixes. */
    for (int guard = 0; guard < 16; guard++) {
        if ((REG(PIO_FSTAT) >> PIO_FSTAT_RXEMPTY_LSB) & (1u << PIO_SM)) break;
        (void)REG(PIO_RXF0);
    }

    /* Re-assert the IO mux for WL_D and WL_CLK on every transfer, exactly
     * as cyw43-driver's start_spi_comms() does (two gpio_set_function()
     * calls it makes every single time, despite cyw43_spi_init() having
     * already done it once). That looked redundant when this driver was
     * first written, so it was hoisted to one-time setup -- and that was
     * the bug that cost this bring-up the most: GP29's FUNCSEL read back
     * as 0x1f (NULL, nothing connected to the pad) by the time a transfer
     * ran, so the state machine's side-set clock went nowhere, while the
     * identical setup on a scratch pin worked. Set it here, every time,
     * where the reference sets it. */
    REG(IO_BANK0_CTRL(WL_DATA_PIN)) = GPIO_FUNCSEL_PIO0;
    REG(IO_BANK0_CTRL(WL_CLK_PIN)) = GPIO_FUNCSEL_PIO0;

    REG(SIO_GPIO_OUT_CLR) = WL_CS_MASK; /* assert CS (active low) */

    REG(PIO_CTRL) |= (1u << (4 + PIO_SM)); /* SM_RESTART[SM] */
    REG(PIO_CTRL) |= (1u << (8 + PIO_SM)); /* CLKDIV_RESTART[SM] */

    /* Force WL_D to output for the write phase -- the program's own
     * `set pindirs,0` only ever turns it *off* (for the read phase);
     * something outside the loop has to turn it back on before each
     * transfer starts, exactly like cyw43-driver's
     * pio_sm_set_pindirs_with_mask64() call in the same place. */
    REG(PIO_SM_PINCTRL(PIO_SM)) = (1u << 26) | ((uint32_t)WL_DATA_PIN << 5); /* SET_BASE=WL_D, SET_COUNT=1 */
    pio_exec(0xE081); /* set pindirs,1 */
    /* And WL_CLK's direction too, every transfer rather than once in
     * pio_gspi_init(). SM_RESTART runs just above on every transfer and
     * the PIO documentation does not say whether pin directions survive
     * it; the reference sets DATA's direction here for the same reason.
     * A clock pin left as an input drives nothing, which looks exactly
     * like a chip that is powered but ignoring the bus. */
    REG(PIO_SM_PINCTRL(PIO_SM)) = (1u << 26) | ((uint32_t)WL_CLK_PIN << 5);
    pio_exec(0xE081); /* set pindirs,1 */
    REG(PIO_SM_PINCTRL(PIO_SM)) =
        (1u << 29) | (1u << 26) | (1u << 20) |
        ((uint32_t)WL_DATA_PIN << 15) | ((uint32_t)WL_CLK_PIN << 10) |
        ((uint32_t)WL_DATA_PIN << 5) | ((uint32_t)WL_DATA_PIN << 0);

    /* Load X (bits to write, minus 1) and Y (bits to read, minus 1) via
     * a direct FIFO push + an injected `PULL`+`OUT` pair that consumes
     * it. cyw43-driver's own equivalent (pio_sm_put()+pio_sm_exec(out(x,
     * 32))) skips the PULL, relying on SM_RESTART having left the OSR
     * "empty" so autopull fires as part of the OUT -- correct, but a
     * restart-then-autopull interaction this driver would rather not
     * stake a first hardware run on. An explicit blocking PULL first
     * pulls unconditionally regardless of shift-counter state (and won't
     * actually block: the FIFO write just above already satisfies it),
     * so X/Y load correctly either way.
     * PULL (not-if-empty, block): opcode 100 with bit7 set (0x8080 base)
     * distinguishes it from PUSH (bit7 clear); block=1 in bits[6:5].
     * OUT encoding: opcode 011, dest in bits[7:5] (X=001, Y=010), count
     * in bits[4:0] (00000 = 32, PIO's "0 means 32" rule). */
    REG(PIO_TXF0) = tx_len * 8 - 1;
    pio_exec(0x80A0); /* pull block */
    pio_exec(0x6020); /* out x,32 */
    REG(PIO_TXF0) = rx ? (rx_len - tx_len) * 8 - 1 : 0;
    pio_exec(0x80A0); /* pull block */
    pio_exec(0x6040); /* out y,32 */
    pio_exec(0x0000); /* jmp 0 -- back to the program's own top */

    REG(PIO_CTRL) |= (1u << PIO_SM); /* enable */

    /* The SM only ever shifts in (rx_len - tx_len) bytes worth of real
     * data -- that's exactly what Y was loaded with above. The leading
     * tx_len bytes of rx are the electrical gap while the command word
     * itself was going out (nothing real to capture, half-duplex data
     * pin); the reference zeroes them for the same reason instead of
     * ever writing them from hardware. Getting this wrong (reading
     * rx_len/4 words instead) is exactly the bug this had on the first
     * hardware run of this fix: ti reached target, ri got exactly one
     * real word, then waited forever for a second one the SM was never
     * going to produce. */
    if (rx) memset(rx, 0, tx_len);
    uint32_t tx_words = tx_len / 4;
    uint32_t rx_words = rx ? (rx_len - tx_len) / 4 : 0;
    uint32_t ti = 0, ri = 0;
    uint32_t spins = 0;
    uint32_t pc_trace[8];
    bool ok = true;
    while (ti < tx_words || ri < rx_words) {
        if (spins < 8 * 20000u && spins % 20000u == 0)
            pc_trace[spins / 20000u] = REG(PIO_SM_ADDR(PIO_SM));
        if (++spins > PIO_POLL_LIMIT) {
            printk("cyw43: pio transfer stuck (ti=%u/%u ri=%u/%u ctrl=0x%08x "
                   "fstat=0x%08x fdebug=0x%08x pc=%u "
                   "pc_trace=%u,%u,%u,%u,%u,%u,%u,%u)\n",
                   ti, tx_words, ri, rx_words, REG(PIO_CTRL), REG(PIO_FSTAT),
                   REG(PIO_FDEBUG), REG(PIO_SM_ADDR(PIO_SM)),
                   pc_trace[0], pc_trace[1], pc_trace[2], pc_trace[3],
                   pc_trace[4], pc_trace[5], pc_trace[6], pc_trace[7]);
            ok = false;
            break;
        }
        uint32_t fstat = REG(PIO_FSTAT);
        if (ti < tx_words && !((fstat >> PIO_FSTAT_TXFULL_LSB) & (1u << PIO_SM))) {
            uint32_t w;
            memcpy(&w, tx + ti * 4, 4);
            /* Pack MSB-first onto the wire -- the byte-swap-into-the-FIFO
             * step cyw43-driver's DMA does unconditionally (see the file
             * header comment for the full derivation). */
            uint32_t swapped = __builtin_bswap32(w);
            REG(PIO_TXF0) = swapped;
            ti++;
        }
        if (ri < rx_words && !((fstat >> PIO_FSTAT_RXEMPTY_LSB) & (1u << PIO_SM))) {
            uint32_t w = REG(PIO_RXF0);
            uint32_t unswapped = __builtin_bswap32(w);
            memcpy(rx + tx_len + ri * 4, &unswapped, 4);
            ri++;
        }
    }
    /* Wait for the SM to finish shifting the last word out/in -- FSTAT
     * only tells us the FIFOs are drained, not that the shift register
     * has too, so also wait for RX (the last thing the program does) to
     * go idle at its wrap point. Cheap and bounded: at most one more
     * 32-bit word's worth of SM clocks. */
    for (volatile int i = 0; i < 4096; i++) { }

    /* Drop the driven pin value before releasing the bus, the way
     * cyw43_spi_transfer() closes out ("for next time we turn output
     * on"), then deassert CS. */
    /* Park the clock LOW before letting go of the bus. The program's
     * last instruction is `jmp y-- lp2 side 1`, so stopping the state
     * machine right after the read loop can leave SCK parked high --
     * and the next transfer would then assert CS with the clock already
     * high, which is not a valid mode-0 frame start and gives the chip
     * no clean edge to sync on. Both reference drivers end their
     * programs on side 0 for this reason (embassy's trailing
     * `wait`/`irq` instructions both carry side 0). An injected
     * instruction still applies its side-set, so a `nop side 0` is
     * enough to drive it low. */
    pio_exec(0xA042); /* nop (mov y,y) side 0 -- park SCK low */
    pio_exec(0xA003); /* mov pins, null */
    REG(PIO_CTRL) &= ~(1u << PIO_SM);
    REG(SIO_GPIO_OUT_SET) = WL_CS_MASK; /* deassert CS */
    return ok;
}

/* --- protocol: bring-up-only test pattern probe -------------------------- */

static inline uint32_t make_cmd(int write, int inc, uint32_t fn, uint32_t addr, uint32_t sz) {
    return ((uint32_t)(write != 0) << 31) | ((uint32_t)(inc != 0) << 30) |
           (fn << 28) | ((addr & 0x1FFFFu) << 11) | sz;
}

/* The reference's own SWAP32() macro (cyw43_bus_pio_spi.c) is NOT a full
 * 4-byte reversal -- easy to assume it is, since the name and the DMA
 * "bswap" it sits next to both say "swap" and both operate on 32-bit
 * words, but they are different operations. SWAP32 swaps bytes *within
 * each 16-bit half* only (its RISC-V body is `rev8` -- a real full
 * reversal -- immediately undone down to a half-swap by a 16-bit
 * rotate: `rori x, x, 16`). Confusing the two was a real bug on this
 * driver's first hardware run: every transfer completed cleanly (the PIO
 * mechanics were right) but the chip never recognised the command,
 * because __builtin_bswap32() (a full reversal) is not this macro's
 * inverse the way it's pio_gspi_transfer()'s own DMA-equivalent bswap
 * is (see that function's header comment) -- it is not this macro's
 * inverse at all. */
static inline uint32_t swap16x2(uint32_t x) {
    return ((x & 0x00FF00FFu) << 8) | ((x & 0xFF00FF00u) >> 8);
}

/* read_reg_u32_swap(): the reference applies swap16x2() (its own SWAP32)
 * in software on top of pio_gspi_transfer()'s unconditional per-word
 * full bswap (the same DMA "bswap" step -- see that function's header
 * comment, unaffected by the fix above). The two are different
 * operations and do not cancel; reproduced as two distinct steps here
 * rather than trying to collapse them into one. */
static bool cyw43_read_reg_u32_swap(uint32_t fn, uint32_t reg, uint32_t *out) {
    uint32_t cmd = swap16x2(make_cmd(0, 1, fn, reg, 4));
    /* 8 bytes read, not 4: gSPI appends a 32-bit status word to every
     * transfer, so a single 32-bit register read clocks 64 bits back.
     * The Pico SDK hides this (its own transfer sizes stop at the data
     * word); embassy-rs's independent driver makes it explicit --
     * read_bits = read.len() * 32 + 32 - 1. The value is the first word;
     * the status word follows it. */
    uint8_t txbuf[4], rxbuf[12];
    memcpy(txbuf, &cmd, 4);
    memset(rxbuf, 0, sizeof(rxbuf));
    if (!pio_gspi_transfer(txbuf, 4, rxbuf, sizeof(rxbuf))) return false;
    uint32_t raw;
    memcpy(&raw, rxbuf + 4, 4);   /* rxbuf + 8 holds the status word */
    *out = swap16x2(raw);
    return true;
}

/* write_reg_u32_swap(): the bring-up-mode write. Both the command and
 * the value take the software half-swap, on top of the transfer layer's
 * own full bswap -- same pairing as the read above. */
static bool cyw43_write_reg_u32_swap(uint32_t fn, uint32_t reg, uint32_t val) {
    uint32_t buf[2];
    buf[0] = swap16x2(make_cmd(1, 1, fn, reg, 4));
    buf[1] = swap16x2(val);
    return pio_gspi_transfer((const uint8_t *)buf, 8, NULL, 0);
}

/* The plain read used once the bus has been switched to 32-bit mode: no
 * software half-swap on either side, only the transfer layer's own
 * DMA-equivalent bswap. */
static bool cyw43_read_reg_u32(uint32_t fn, uint32_t reg, uint32_t *out) {
    uint32_t cmd = make_cmd(0, 1, fn, reg, 4);
    uint8_t txbuf[4], rxbuf[8];
    memcpy(txbuf, &cmd, 4);
    memset(rxbuf, 0, sizeof(rxbuf));
    if (!pio_gspi_transfer(txbuf, 4, rxbuf, 8)) return false;
    memcpy(out, rxbuf + 4, 4);
    return true;
}

bool cyw43_gspi_probe(void) {
    wl_gpio_setup();
    pio_gspi_init();
    wl_reset();

    /* Bring-up order straight from cyw43_ll_bus_init(): poll the test
     * register, then switch the bus to 32-bit/big-endian/high-speed and
     * confirm the switch by reading it back. */
    for (int i = 0; i < 10; i++) {
        uint32_t reg = 0;
        if (!cyw43_read_reg_u32_swap(BUS_FUNCTION, SPI_READ_TEST_REGISTER, &reg)) {
            printk("cyw43: bus transfer did not complete\n");
            return false;
        }
        if (reg == TEST_PATTERN) {
            printk("cyw43: test pattern ok (0x%08x)\n", reg);
            break;
        }
        if (i == 9) {
            printk("cyw43: test pattern not seen (last read 0x%08x)\n", reg);
            return false;
        }
        time_delay_us(1000);
    }

    uint32_t val = WORD_LENGTH_32 | ENDIAN_BIG | HIGH_SPEED_MODE | WAKE_UP |
                   INTERRUPT_POLARITY_HIGH |
                   (0x4u << 8) |                       /* SPI_RESPONSE_DELAY */
                   ((uint32_t)INTR_WITH_STATUS << 16); /* SPI_STATUS_ENABLE */
    if (!cyw43_write_reg_u32_swap(BUS_FUNCTION, SPI_BUS_CONTROL, val)) {
        printk("cyw43: bus control write did not complete\n");
        return false;
    }
    time_delay_us(1000);

    uint32_t bc = 0;
    if (!cyw43_read_reg_u32(BUS_FUNCTION, SPI_BUS_CONTROL, &bc)) return false;
    printk("cyw43: switched to 32-bit mode, bus control reads 0x%08x\n", bc);
    return true;
}

#else

bool cyw43_gspi_probe(void) { return false; }

#endif
