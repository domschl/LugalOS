/* See drivers/include/drivers/gps_pps.h for what this is for and why it never
 * sets a clock. */

#include "drivers/gps_pps.h"
#include "drivers/edgecap.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include "lugalos_config.h"
#include <string.h>

#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_GPS_UART_BASE)

#define REG(addr) (*(volatile uint32_t *)(addr))

#define RESETS_BASE       0x40020000UL
#define RESETS_RESET_CLR  (RESETS_BASE + 0x3000 + 0x0)
#define RESETS_RESET_DONE (RESETS_BASE + 0x8)
#define RESETS_UART1_BIT  (1u << 27)

#define IO_BANK0_BASE     0x40028000UL
#define IO_BANK0_CTRL(n)  (IO_BANK0_BASE + 0x004 + (n) * 8)
#define PADS_BANK0_BASE   0x40038000UL
#define PADS_BANK0_PAD(n) (PADS_BANK0_BASE + 0x004 + (n) * 4)

#define U   ((uintptr_t)CONFIG_GPS_UART_BASE)
#define U_DR    (U + 0x00)
#define U_FR    (U + 0x18)
#define U_IBRD  (U + 0x24)
#define U_FBRD  (U + 0x28)
#define U_LCR_H (U + 0x2C)
#define U_CR    (U + 0x30)
#define U_RSR   (U + 0x04)   /* read: latched RX errors; write: clears them */
#define FR_RXFE (1u << 4)
#define FR_TXFF (1u << 5)

/* The error bits the PL011 puts in the *upper* half of UARTDR, alongside the
 * byte they belong to. Reading only the low byte -- which this driver used to
 * do -- throws them away and leaves the latched copy in UARTRSR set forever.
 * They are the difference between "the module went quiet" and "our receiver
 * is wedged", which otherwise look identical from here: both are a byte
 * counter that stops moving. */
#define DR_OE   (1u << 11)   /* overrun: FIFO was full, a byte was lost */
#define DR_BE   (1u << 10)   /* break: RX held low longer than a frame */
#define DR_PE   (1u <<  9)   /* parity */
#define DR_FE   (1u <<  8)   /* framing: no stop bit where one was due --
                              * the classic wrong-baud signature */
#define DR_ERRS (DR_OE | DR_BE | DR_PE | DR_FE)

#define SIO_GPIO_OE_CLR   0xD0000040UL

static gps_status_t g;
static char     g_line[96];
static uint32_t g_line_len;
static bool     g_overflow;      /* the current line is too long; discard it */
static bool     g_rx_muxed;      /* the RX pin actually took its UART function */
/* What the module actually says, which is how it gets identified.
 *
 * The talker ids and the sentence set narrow a module to a family, and the
 * last sentence catches anything unusual -- a $GPTXT boot banner, or a reply
 * to a version poll. Needed because the timepulse rate turned out to be a
 * property of the *module* rather than of its lock state, and fixing that
 * means sending it a configuration command in its own dialect. Guessing the
 * dialect by shotgunning both is how a working receiver gets misconfigured. */
static char     g_types[12][6];  /* "GPGGA" &c, without the '$' */
static uint8_t  g_ntypes;
static char     g_last[88];

static uint32_t g_tp5_sent;      /* UBX-CFG-TP5 frames written */
static uint32_t g_ubx_saves;     /* UBX-CFG-CFG saves issued */
static bool     g_tp5_save_due;
static bool     g_tp5_needed = true;   /* the module's stored config is
                                        * unknown at boot, so assume it wants
                                        * correcting exactly once */
static bool     g_tp5_save_needed;     /* ...but do not spend its flash until
                                        * something proves that it does */
static uint32_t g_storms_seen;

/* Inbound UBX, which exists to answer one question: does the module hear us?
 *
 * ubx_tp5_sent counts frames this driver *queued*. It says nothing about
 * whether they arrived -- a TX wire on the wrong pin produces exactly the same
 * count -- and reading it as proof of delivery was a mistake worth not
 * repeating. u-blox acknowledges every configuration write with UBX-ACK-ACK or
 * refuses it with UBX-ACK-NAK, so parsing those is the difference between "the
 * module rejected our frame" and "our frame never got there", which need
 * completely different fixes.
 *
 * It also stops the replies being counted as corrupt NMEA: a binary frame fed
 * to a sentence parser is so many bad checksums, which is part of why that
 * counter has looked worse than the link actually is. */
static uint8_t  g_ux_state, g_ux_cls, g_ux_id, g_ux_a, g_ux_b;
static uint16_t g_ux_len, g_ux_got;
static uint8_t  g_ux_pay[32];

static void ubx_feed(uint8_t c) {
    switch (g_ux_state) {
    case 1: g_ux_state = (c == 0x62u) ? 2u : 0u; return;
    case 2: g_ux_cls = c; g_ux_a = c; g_ux_b = c; g_ux_state = 3; return;
    case 3: g_ux_id = c; goto acc;
    case 4: g_ux_len = c; goto acc;
    case 5: g_ux_len |= (uint16_t)c << 8; g_ux_got = 0;
            g_ux_a = (uint8_t)(g_ux_a + c); g_ux_b = (uint8_t)(g_ux_b + g_ux_a);
            /* A length this driver cannot be looking at is skipped rather than
             * buffered: the only replies it cares about are two bytes long. */
            g_ux_state = g_ux_len ? 6u : 7u;
            return;
    case 6:
        if (g_ux_got < sizeof(g_ux_pay)) g_ux_pay[g_ux_got] = c;
        g_ux_got++;
        g_ux_a = (uint8_t)(g_ux_a + c); g_ux_b = (uint8_t)(g_ux_b + g_ux_a);
        if (g_ux_got >= g_ux_len) g_ux_state = 7;
        return;
    case 7: g_ux_state = (c == g_ux_a) ? 8u : 0u; return;
    case 8:
        g_ux_state = 0;
        if (c != g_ux_b) return;                       /* corrupt; ignore */
        if (g_ux_cls == 0x05u && g_ux_len >= 2u) {     /* UBX-ACK-*        */
            g.ubx_ack_cls = g_ux_pay[0];
            g.ubx_ack_id  = g_ux_pay[1];
            if (g_ux_id == 0x01u) g.ubx_acks++;        /* ACK-ACK          */
            else if (g_ux_id == 0x00u) g.ubx_naks++;   /* ACK-NAK          */
        } else if (g_ux_cls == 0x06u && g_ux_id == 0x31u && g_ux_len >= 32u) {
            /* CFG-TP5 readback: what this output is really set to. */
            g.tp5_idx = g_ux_pay[0];
            g.tp5_freq = (uint32_t)g_ux_pay[8] | ((uint32_t)g_ux_pay[9] << 8) |
                         ((uint32_t)g_ux_pay[10] << 16) | ((uint32_t)g_ux_pay[11] << 24);
            g.tp5_freq_lock = (uint32_t)g_ux_pay[12] | ((uint32_t)g_ux_pay[13] << 8) |
                              ((uint32_t)g_ux_pay[14] << 16) | ((uint32_t)g_ux_pay[15] << 24);
            g.tp5_flags = (uint32_t)g_ux_pay[28] | ((uint32_t)g_ux_pay[29] << 8) |
                          ((uint32_t)g_ux_pay[30] << 16) | ((uint32_t)g_ux_pay[31] << 24);
            g.tp5_reads++;
        }
        g.ubx_frames++;
        return;
    default: g_ux_state = 0; return;
    }
acc:
    g_ux_a = (uint8_t)(g_ux_a + c); g_ux_b = (uint8_t)(g_ux_b + g_ux_a);
    g_ux_state++;
}
static uint64_t g_tp5_last_ms;
static uint64_t g_last_rx_ms;    /* when the last byte arrived, so a stream
                                  * that stops is a measured silence rather
                                  * than a counter someone has to watch */

static edge_t     g_pps_ring[16];
static edgecap_t  g_pps;
static bool       g_prev_pps_valid;
static uint64_t   g_prev_pps_us;

/* A short history of pulse starts, so a measurement taken slightly after the
 * fact can still be referred to the right second. P4 needs this: a DCF-77
 * frame is only recognised as complete once its minute mark has passed, and
 * by then the PPS edge that began that second is already history. */
#define PPS_HISTORY 8
static uint64_t   g_pps_hist[PPS_HISTORY];
static uint32_t   g_pps_hist_n;

/* Written, read back, retried -- the third lesson from
 * drivers/uart1_link_rp2350.c, and the one that makes the other two
 * diagnosable: "a write, a read-back and a retry is cheap; believing a write
 * is not". A refused mux and an unplugged module both produce zero bytes, and
 * without this there is no way to tell them apart from the far end of a
 * network mount. */
static bool set_funcsel(unsigned gpio, uint32_t fn) {
    for (int attempt = 0; attempt < 10; attempt++) {
        REG(IO_BANK0_CTRL(gpio)) = fn;
        if ((REG(IO_BANK0_CTRL(gpio)) & 0x1Fu) == fn) return true;
        time_delay_us(1000);
    }
    printk("[GPS] GP%u mux REFUSED FUNCSEL %u (reads 0x%08lx) -- the pin is not "
           "connected to the UART\n", gpio, (unsigned)fn,
           (unsigned long)REG(IO_BANK0_CTRL(gpio)));
    return false;
}

static bool u_has_char(void) { return (REG(U_FR) & FR_RXFE) == 0; }

#if CONFIG_GPS_UBX_TIMEPULSE
/* Outbound UBX is queued, never spun on.
 *
 * The first version busy-waited on TXFF. At 9600 baud a 40-byte frame is 42 ms
 * of line time and only 32 bytes fit the FIFO, so it stalled the clock's frame
 * loop for ~8 ms with interrupts still on and no yield -- during early boot,
 * which is exactly when the radio is joining. Whether or not that was what
 * broke the join, a driver that blocks a shared loop to talk to its own
 * peripheral is wrong, and the queue costs sixty bytes. */
static uint8_t g_txbuf[64];
static uint8_t g_txlen, g_txpos;

static void ubx_tx_pump(void) {
    while (g_txpos < g_txlen && !(REG(U_FR) & FR_TXFF))
        REG(U_DR) = g_txbuf[g_txpos++];
    if (g_txpos >= g_txlen) g_txlen = g_txpos = 0;
}

static void u_putc(uint8_t c) {
    if (g_txlen < sizeof(g_txbuf)) g_txbuf[g_txlen++] = c;
}

/* A UBX frame: sync, class, id, little-endian length, payload, and the
 * 8-bit Fletcher checksum over everything from `class` onward. */
static void ubx_send(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len) {
    const uint8_t hdr[4] = { cls, id, (uint8_t)(len & 0xFFu), (uint8_t)(len >> 8) };
    uint8_t a = 0, b = 0;
    u_putc(0xB5u); u_putc(0x62u);
    for (uint32_t i = 0; i < 4u; i++) { a = (uint8_t)(a + hdr[i]); b = (uint8_t)(b + a); u_putc(hdr[i]); }
    for (uint16_t i = 0; i < len; i++) { a = (uint8_t)(a + payload[i]); b = (uint8_t)(b + a); u_putc(payload[i]); }
    u_putc(a); u_putc(b);
}

/* UBX-CFG-TP5: one pulse per second, aligned to the top of the UTC second.
 *
 * Needed because the timepulse rate is a property of the *module*, not of its
 * lock state. This board's NEO-M8N arrived emitting ~1 kHz once locked --
 * `pps_storm_rate` measured 2004-2006 edges/sec against a valid eight-
 * satellite fix -- which is not the u-blox factory default, so some earlier
 * owner's configuration was still sitting in its battery-backed RAM. A 1 kHz
 * train is useless here whatever its accuracy: every pulse lands on a
 * millisecond boundary, the second's start is one of them, and NMEA cannot say
 * which of the thousand it is.
 *
 * Followed by a UBX-CFG-CFG save when CONFIG_GPS_UBX_PERSIST is set, which
 * the owner of this board explicitly authorised ("feel free to rewrite the
 * config", 2026-09-02). It is off by default everywhere else, because writing
 * somebody's module permanently to suit our requirement is not a decision a
 * driver gets to make on its own.
 *
 * The save has to name **flash**, not just battery-backed RAM, and the reason
 * is a diagnosis worth keeping. A BBR that loses power does not come back
 * holding rubbish; it comes back holding the ROM factory defaults, and the
 * M8N's factory timepulse is already 1 Hz with a 100 ms pulse. So a long
 * disconnection would have cured this rather than caused it, and the 1 kHz
 * must live somewhere that survives power loss. Saving to BBR alone would
 * work until the next power cut and then quietly revert.
 *
 * Field layout from the u-blox M8 protocol spec; the flags are active,
 * lockGnssFreq, lockedOtherSet, isLength, alignToTow and polarity=rising. */
static void ubx_set_timepulse_1hz(uint8_t tp_idx) {
    uint8_t tp5[32] = {
        0x00,                    /* tpIdx, overwritten below         */
        0x01,                    /* version                          */
        0x00, 0x00,              /* reserved                         */
        0x00, 0x00,              /* antCableDelay ns                 */
        0x00, 0x00,              /* rfGroupDelay ns                  */
        0x40, 0x42, 0x0F, 0x00,  /* freqPeriod      = 1000000 us     */
        0x40, 0x42, 0x0F, 0x00,  /* freqPeriodLock  = 1000000 us     */
        0x00, 0x00, 0x00, 0x00,  /* pulseLenRatio   = 0 (no pulse
                                  * while unlocked, which is what
                                  * makes an unlocked module quiet
                                  * rather than a storm source)      */
        0xA0, 0x86, 0x01, 0x00,  /* pulseLenRatioLock = 100000 us    */
        0x00, 0x00, 0x00, 0x00,  /* userConfigDelay                  */
        0x77, 0x00, 0x00, 0x00,  /* flags                            */
    };
    tp5[0] = tp_idx;
    ubx_send(0x06u, 0x31u, tp5, sizeof(tp5));
}

/* Ask the module what it actually has. The write is acknowledged either way,
 * so an ACK proves only that the frame was understood -- not that it landed on
 * the output that is wired to GP19. The readback is the only thing that can
 * tell those apart. */
static void ubx_poll_timepulse(uint8_t tp_idx) {
    ubx_send(0x06u, 0x31u, &tp_idx, 1u);
}

#if CONFIG_GPS_UBX_PERSIST
/* UBX-CFG-CFG: commit the running configuration to non-volatile storage.
 *
 * saveMask covers every defined section rather than only the timepulse's,
 * because "save" means "save what the module is running now" -- and what it is
 * running now is its own configuration with one field corrected. Narrowing the
 * mask would not protect anything and risks omitting whichever section TP5
 * actually lives in.
 *
 * deviceMask names BBR, flash and EEPROM together. A breakout without one of
 * them simply refuses that device; asking for all three is how this works on
 * boards that differ, rather than guessing which the module has. */
static void ubx_save_config(void) {
    static const uint8_t cfg[13] = {
        0x00, 0x00, 0x00, 0x00,  /* clearMask: clear nothing            */
        0xFF, 0xFF, 0x00, 0x00,  /* saveMask:  every defined section    */
        0x00, 0x00, 0x00, 0x00,  /* loadMask:  load nothing             */
        0x07,                    /* deviceMask: BBR | flash | EEPROM    */
    };
    ubx_send(0x06u, 0x09u, cfg, sizeof(cfg));
}
#endif
#endif

/* Reads the byte *and* its error bits, counts them, and clears the latched
 * copy. The clear is the point: UARTRSR latches until written, so without it
 * one overrun during boot makes every later diagnosis read "overrun" and the
 * register stops being evidence of anything. */
static uint8_t u_getc(void) {
    uint32_t dr = REG(U_DR);
    if (dr & DR_ERRS) {
        if (dr & DR_OE) g.rx_err_overrun++;
        if (dr & DR_BE) g.rx_err_break++;
        if (dr & DR_PE) g.rx_err_parity++;
        if (dr & DR_FE) g.rx_err_frame++;
        REG(U_RSR) = 0xFu;          /* any write clears all four */
    }
    return (uint8_t)(dr & 0xFFu);
}

void gps_init(void) {
    REG(RESETS_RESET_CLR) = RESETS_UART1_BIT;
    int timeout = 10000;
    while (!(REG(RESETS_RESET_DONE) & RESETS_UART1_BIT) && --timeout > 0);

    /* Two things carried over from drivers/uart1_link_rp2350.c rather than
     * rediscovered, because both cost that driver an afternoon:
     *
     * **RX gets a pull-UP (0x5A), never a pull-down.** A UART line idles
     * high, so an unconnected RX held low sits at the start-bit level forever
     * and the receiver produces a stream of framing errors -- garbage out of
     * a wire that is not there. With a pull-up an unplugged GPS produces
     * *nothing*, and "0 bytes" then means "no cable" instead of "no cable, or
     * a cable, who knows".
     *
     * **No ACCESSCTRL GPIO_NSMASK grant.** Marking a pin non-secure hands it
     * to the non-secure side and takes it from the secure one; this driver is
     * M-mode only, so the grant would buy nothing and cost the pins -- FUNCSEL
     * reads back as NULL and the peripheral transmits into a pad no longer
     * connected to it.
     *
     * If a third UART1 user ever appears, this setup should be extracted
     * rather than written a third time. */
    g_rx_muxed = set_funcsel(CONFIG_GPS_RX_GPIO, 2u);   /* F2 = UART1 RX */
    REG(PADS_BANK0_PAD(CONFIG_GPS_RX_GPIO)) = 0x5A;     /* schmitt, PUE, 4mA, IE */
#ifdef CONFIG_GPS_TX_GPIO
    /* The transmit path exists so a module whose timepulse is set wrong can be
     * corrected without rewiring -- see ubx_set_timepulse_1hz() below, which
     * is why the pin was reserved and is now the reason it is used. */
    set_funcsel(CONFIG_GPS_TX_GPIO, 2u);                /* F2 = UART1 TX */
    REG(PADS_BANK0_PAD(CONFIG_GPS_TX_GPIO)) = 0x56;     /* schmitt, 4mA, IE */
#endif

    /* The PL011 divisor is clk_peri / (16 * baud) in 6.6 fixed point, which is
     * exactly (4 * clk_peri) / baud as an integer -- the same derivation
     * uart1_link_rp2350.c spells out, rather than two magic numbers. */
    /* 150 MHz clk_peri, the same literal drivers/uart1_link_rp2350.c uses --
     * this board's clock tree is fixed by arch/riscv/rp2350/boot_header.S and
     * neither driver has a way to ask it. */
    uint32_t div64 = (uint32_t)((4ull * 150000000ull) / (uint32_t)CONFIG_GPS_BAUD);
    REG(U_IBRD) = div64 / 64u;
    REG(U_FBRD) = div64 % 64u;
    REG(U_LCR_H) = (3u << 5) | (1u << 4);               /* 8N1, FIFOs on */
    REG(U_CR) = (1u << 0) | (1u << 8) | (1u << 9);      /* UARTEN | TXE | RXE */

    /* The PPS pad, configured BEFORE the capture is armed.
     *
     * edgecap's contract is that the caller owns the pad, and the first
     * version of this driver did not honour it: GP19 was registered while
     * still in its power-on state, and a floating input with both-edge
     * interrupts is a storm that starved the display into darkness within a
     * second of boot, twice.
     *
     * **The pull follows the polarity, and the polarity is a board fact**
     * (CONFIG_GPS_PPS_ACTIVE_LOW), configured rather than inferred -- the
     * same decision phase 17 made for the DCF-77 pin after inference was
     * shown to need a good signal exactly when there is not one. It matters
     * more than it looks: a module driving PPS push-pull idles low and pulses
     * high, while an open-collector one idles high (through this pull-up) and
     * pulses low, so "the pulse" is a rising edge on one and a falling edge on
     * the other. Getting it wrong does not produce noise, it produces a
     * timestamp of the wrong instant -- the end of the pulse instead of its
     * start, which is the module's pulse width folded into a measurement that
     * has no business containing it.
     *
     * Either way the line has *a* pull, and that is what stops an unconnected
     * pin oscillating. The direction is chosen so a disconnected module reads
     * as "no pulse" rather than as a permanent one. */
    set_funcsel(CONFIG_GPS_PPS_GPIO, 5u);               /* F5 = SIO */
    REG(SIO_GPIO_OE_CLR) = 1u << CONFIG_GPS_PPS_GPIO;   /* input, never driven */
#if CONFIG_GPS_PPS_ACTIVE_LOW
    REG(PADS_BANK0_PAD(CONFIG_GPS_PPS_GPIO)) = 0x5A;    /* schmitt, PUE, 4mA, IE */
#else
    REG(PADS_BANK0_PAD(CONFIG_GPS_PPS_GPIO)) = 0x56;    /* schmitt, PDE, 4mA, IE */
#endif

    if (edgecap_attach(&g_pps, CONFIG_GPS_PPS_GPIO, g_pps_ring,
                       (uint16_t)(sizeof(g_pps_ring) / sizeof(g_pps_ring[0]))) != 0) {
        printk("[GPS] could not capture PPS on GP%d\n", CONFIG_GPS_PPS_GPIO);
    }

    g.enabled = true;
    g.rx_muxed = g_rx_muxed;
    g.rx_pad = REG(IO_BANK0_CTRL(CONFIG_GPS_RX_GPIO));
    printk("[GPS] NMEA on GP%d at %d baud, PPS on GP%d (a transfer standard: "
           "nothing here sets a clock)\n",
           CONFIG_GPS_RX_GPIO, CONFIG_GPS_BAUD, CONFIG_GPS_PPS_GPIO);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* NMEA's checksum is an XOR of everything between '$' and '*'. Verified rather
 * than assumed: a wrong baud rate produces sentences that *look* like
 * sentences, and a corrupted one carrying a plausible fix quality is exactly
 * the input this must not act on. */
static bool nmea_checksum_ok(const char *s, uint32_t len) {
    if (len < 4 || s[0] != '$') return false;
    uint32_t star = 0;
    for (uint32_t i = len; i-- > 1; ) if (s[i] == '*') { star = i; break; }
    if (star == 0 || star + 2 >= len) return false;
    uint8_t sum = 0;
    for (uint32_t i = 1; i < star; i++) sum ^= (uint8_t)s[i];
    int hi = hexval(s[star + 1]), lo = hexval(s[star + 2]);
    if (hi < 0 || lo < 0) return false;
    return sum == (uint8_t)((hi << 4) | lo);
}

/* Copies field `n` (comma-separated, field 0 is the sentence type) into `out`.
 * Empty is a legitimate value in NMEA and comes back as an empty string. */
static void nmea_field(const char *s, uint32_t len, uint32_t n, char *out, uint32_t cap) {
    out[0] = '\0';
    uint32_t field = 0, i = 0;
    while (i < len && field < n) { if (s[i] == ',') field++; i++; }
    if (field != n) return;
    uint32_t o = 0;
    while (i < len && s[i] != ',' && s[i] != '*' && o + 1 < cap) out[o++] = s[i++];
    out[o] = '\0';
}

static uint32_t dec2(const char *s) {
    return (uint32_t)((s[0] - '0') * 10 + (s[1] - '0'));
}

static void nmea_apply(const char *s, uint32_t len) {
    /* Match on the last three characters of the sentence type, not the whole
     * of it: the talker id varies with constellation ($GP.. GPS, $GN.. mixed,
     * $GL.. GLONASS) and a module that changes it mid-session is normal. */
    if (len < 6) return;
    const char *type = s + 3;

    /* Census first, so it covers sentences this parser has no other use for
     * -- which are exactly the ones that identify a module. */
    {
        uint32_t n = len < sizeof(g_last) - 1u ? len : sizeof(g_last) - 1u;
        for (uint32_t i = 0; i < n; i++) g_last[i] = s[i];
        g_last[n] = '\0';

        bool seen = false;
        for (uint8_t i = 0; i < g_ntypes && !seen; i++) {
            seen = g_types[i][0] == s[1] && g_types[i][1] == s[2] &&
                   g_types[i][2] == s[3] && g_types[i][3] == s[4] &&
                   g_types[i][4] == s[5];
        }
        if (!seen && g_ntypes < (uint8_t)(sizeof(g_types) / sizeof(g_types[0]))) {
            for (uint32_t i = 0; i < 5u; i++) g_types[g_ntypes][i] = s[1 + i];
            g_types[g_ntypes][5] = '\0';
            g_ntypes++;
        }
    }

    char f[16];
    if (type[0] == 'G' && type[1] == 'G' && type[2] == 'A') {
        nmea_field(s, len, 6, f, sizeof(f));
        if (f[0]) g.fix_quality = (uint8_t)(f[0] - '0');
        nmea_field(s, len, 7, f, sizeof(f));
        if (f[0]) {
            uint32_t n = 0;
            for (const char *p = f; *p >= '0' && *p <= '9'; p++) n = n * 10u + (uint32_t)(*p - '0');
            g.satellites = (uint8_t)(n > 255u ? 255u : n);
        }
    } else if (type[0] == 'R' && type[1] == 'M' && type[2] == 'C') {
        nmea_field(s, len, 2, f, sizeof(f));
        g.rmc_valid = (f[0] == 'A');

        char t[16], d[16];
        nmea_field(s, len, 1, t, sizeof(t));   /* hhmmss.ss */
        nmea_field(s, len, 9, d, sizeof(d));   /* ddmmyy */
        if (strlen(t) >= 6 && strlen(d) >= 6) {
            g.utc.hour = (uint8_t)dec2(t);
            g.utc.min  = (uint8_t)dec2(t + 2);
            g.utc.sec  = (uint8_t)dec2(t + 4);
            g.utc.ms   = 0;
            g.utc.day   = (uint8_t)dec2(d);
            g.utc.month = (uint8_t)dec2(d + 2);
            g.utc.year  = (uint16_t)(2000u + dec2(d + 4));
            g.have_utc = true;
        }
    }
}

void gps_poll(void) {
    if (!g.enabled) return;

    /* Bounded: this runs from the clock application's frame loop, and a UART
     * delivering faster than it is drained must not turn one call into an
     * unbounded one. 256 bytes is more than a second's worth of NMEA at
     * 9600 baud. */
    for (uint32_t budget = 0; budget < 256u && u_has_char(); budget++) {
        char c = (char)u_getc();
        g.bytes++;
        g_last_rx_ms = time_get_ms();

        /* UBX before NMEA: a binary reply must not reach the sentence
         * assembler, both because it is not a sentence and because counting it
         * as one hides the answer we actually need. */
        if (g_ux_state) { ubx_feed((uint8_t)c); continue; }
        if ((uint8_t)c == 0xB5u) { g_ux_state = 1; continue; }

        if (c == '\r') continue;
        if (c == '\n') {
            if (!g_overflow && g_line_len > 0) {
                if (nmea_checksum_ok(g_line, g_line_len)) {
                    g.sentences++;
                    nmea_apply(g_line, g_line_len);
                } else {
                    g.bad_checksum++;
                }
            }
            g_line_len = 0; g_overflow = false;
            continue;
        }
        if (c == '$') { g_line_len = 0; g_overflow = false; }
        if (g_line_len + 1 >= sizeof(g_line)) { g_overflow = true; continue; }
        g_line[g_line_len++] = c;
    }

    edge_t e;
    while (edgecap_pop(&g_pps, &e)) {
        /* The edge that *starts* the pulse, whichever that is on this module.
         * The other one carries the pulse width -- a property of the module,
         * not of the second -- and timing off it would import that width into
         * the measurement. */
        bool is_pulse_start = CONFIG_GPS_PPS_ACTIVE_LOW ? (e.level == 0)
                                                        : (e.level != 0);
        if (!is_pulse_start) continue;
        if (g_prev_pps_valid) {
            uint64_t d = e.t_us - g_prev_pps_us;
            g.pps_interval_us = (uint32_t)(d > 0xFFFFFFFFu ? 0xFFFFFFFFu : d);
        }
        g_prev_pps_us = e.t_us;
        g_prev_pps_valid = true;
        g.pps_last_us = e.t_us;
        g_pps_hist[g_pps_hist_n % PPS_HISTORY] = e.t_us;
        g_pps_hist_n++;
        g.pps_count++;
    }
#if CONFIG_GPS_UBX_TIMEPULSE
    /* Correct the module's timepulse, but only while there is something to
     * correct and a module in a state to hear it.
     *
     * Gated on rmc_valid so a board with no antenna -- or no module -- never
     * transmits at all, and stopped by trustworthy() the moment the pulse is
     * right, so the normal case is a single frame. Retried because the module
     * may still be starting up when we first have a fix, and capped because a
     * module that will not listen (a TX wire on the wrong pin, say) must not
     * be talked at forever. */
    ubx_tx_pump();
    /* Gated on the module *talking*, not on it having a fix.
     *
     * This asked for rmc_valid at first, which meant the correction could only
     * be sent after the module locked -- and locking is precisely when it
     * starts emitting the 1 kHz that has to be corrected. So every boot with a
     * usable sky view produced a storm first and fixed it afterwards, which is
     * backwards: the whole point is that the wrong pulse never appears. A
     * module that is emitting sentences is present, powered and listening, and
     * that is the only precondition the write actually has.
     *
     * It still stays silent where there is no module at all, which was the
     * only thing rmc_valid was really buying. */
    /* A storm is direct evidence the module's *stored* configuration is still
     * wrong -- the boot-time correction below is volatile, so a pulse that is
     * fast enough to trip the guard means it arrived too late. */
    if (g.pps_storms != g_storms_seen) {
        g_storms_seen = g.pps_storms;
        g_tp5_needed = true;
        g_tp5_save_needed = true;
    }
    if (g_tp5_sent < 10u && g.sentences > 0u && g_tp5_needed) {
        uint64_t now_ms = time_get_ms();
        if (g_tp5_sent == 0u || now_ms - g_tp5_last_ms > 30000ull) {
            /* Both outputs. The M8 has TIMEPULSE and TIMEPULSE2, a breakout
             * may route either to its "PPS" pin, and configuring only tpIdx 0
             * produced an ACK and no change in behaviour -- which is exactly
             * what writing the wrong output looks like. Setting both costs one
             * extra frame and removes the ambiguity. */
            ubx_set_timepulse_1hz(0u);
            ubx_set_timepulse_1hz(1u);
            ubx_poll_timepulse(0u);
            g_tp5_last_ms = now_ms;
            g_tp5_sent++;
            g_tp5_needed = false;
            g_tp5_save_due = g_tp5_save_needed;
        }
    }
#if CONFIG_GPS_UBX_PERSIST
    /* The save goes in a later poll rather than straight after the frame
     * above: the module applies its input in order, and there is no value in
     * asking it to persist a setting in the same breath as receiving it. Half
     * a second is many frame periods here and costs nothing. */
    /* Flash gets written only on evidence that it needs writing.
     *
     * The first version saved after every correction, and indoors -- where
     * trustworthy() can never become true -- that meant ten configuration
     * frames and ten flash commits per boot, on somebody else's module. u-blox
     * flash is not infinite and nothing here justifies spending it.
     *
     * The volatile UBX-CFG-TP5 above is sufficient on its own: it is sent as
     * soon as the module speaks, which is well before it can lock and start
     * pulsing, so a module whose stored config is wrong still behaves
     * correctly for the whole session. The save exists only to stop the
     * correction being needed at all, so it is issued when a storm has proved
     * the stored config wrong -- and once that save takes, no storm follows,
     * and nothing writes flash again. */
    if (g_tp5_save_due && time_get_ms() - g_tp5_last_ms > 500ull) {
        ubx_save_config();
        g_tp5_save_due = false;
        g_tp5_save_needed = false;
        g_ubx_saves++;
    }
#endif
#endif
    g.pps_dropped = edgecap_dropped(&g_pps);
    g.pps_stormed = edgecap_stormed(&g_pps);
    g.pps_storms = edgecap_storms(&g_pps);
    g.pps_storm_rate = edgecap_storm_rate(&g_pps);
}

bool gps_pps_trustworthy(void) {
    if (!g.enabled || !g.pps_count) return false;
    if (!g.rmc_valid || g.fix_quality == 0) return false;
    /* Within a millisecond of a second. Generous on purpose: this is a
     * sanity gate against a free-running oscillator, not a precision claim --
     * the precision is what the measurement itself will report. */
    return g.pps_interval_us > 999000u && g.pps_interval_us < 1001000u;
}

#else  /* no GPS on this persona */

static gps_status_t g;
void gps_init(void) { }
void gps_poll(void) { }
bool gps_pps_trustworthy(void) { return false; }

#endif

/* Static buffers rather than fields in gps_status_t: the status struct is
 * copied onto a caller's stack on every read, including the clock app's frame
 * loop, and none of those callers wants a couple of hundred bytes of text they
 * will not look at. */
const char *gps_nmea_last(void) {
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_GPS
    return g_last;
#else
    return "";
#endif
}

void gps_nmea_types(char *buf, uint32_t cap) {
    if (!buf || !cap) return;
    buf[0] = '\0';
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_GPS
    uint32_t used = 0;
    for (uint8_t i = 0; i < g_ntypes; i++) {
        for (const char *p = g_types[i]; *p && used + 2u < cap; p++) buf[used++] = *p;
        if (i + 1u < g_ntypes && used + 2u < cap) buf[used++] = ' ';
    }
    buf[used] = '\0';
#else
    (void)cap;
#endif
}

bool gps_pps_offset_us(uint64_t t_us, int64_t *offset_us) {
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_GPS
    if (!offset_us || !gps_pps_trustworthy()) return false;

    /* The nearest pulse start, and it has to be *near*: more than half a
     * second away and there is no way to say which second it belonged to, so
     * the honest answer is no answer rather than a number that is wrong by
     * exactly one second. */
    uint32_t have = g_pps_hist_n < PPS_HISTORY ? g_pps_hist_n : PPS_HISTORY;
    bool found = false;
    int64_t best = 0;
    for (uint32_t i = 0; i < have; i++) {
        int64_t d = (int64_t)t_us - (int64_t)g_pps_hist[i];
        int64_t ad = d < 0 ? -d : d;
        if (ad > 500000) continue;
        if (!found || ad < (best < 0 ? -best : best)) { best = d; found = true; }
    }
    if (found) *offset_us = best;
    return found;
#else
    (void)t_us; (void)offset_us;
    return false;
#endif
}

void gps_status(gps_status_t *out) {
    if (!out) return;
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_GPS
    if (g.enabled) {
        g.rx_idle_ms = g.bytes ? (uint32_t)(time_get_ms() - g_last_rx_ms) : 0;
        g.rx_fr = REG(U_FR);
        g.rx_cr = REG(U_CR);
        g.ubx_tp5_sent = g_tp5_sent;
        g.ubx_saves = g_ubx_saves;
    }
#endif
    *out = g;
}
