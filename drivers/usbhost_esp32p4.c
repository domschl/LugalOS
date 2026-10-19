/* The ESP32-P4's USB 2.0 OTG HS controller as a host, polled, and the LCD-7B's
 * Type-A port on it.
 *
 * This is a *port* for drivers/usbkbd_host.c (the interface is
 * drivers/include/drivers/usbkbd_task.h): it does one USB transaction at a
 * time -- SETUP, IN, OUT, or a root port reset -- and the shared host above it
 * enumerates, follows a hub and polls the boot keyboard, exactly as on the
 * RP2350, whose port is the PIO-USB engine.
 *
 * Provenance. The controller is a Synopsys DWC2 ("DWC_otg"), v4.x. Register
 * offsets and bits are the DWC2 databook's, as TinyUSB 0.18.0 spells them in
 * src/portable/synopsys/dwc2/dwc2_type.h (MIT, Copyright (c) 2024 Ha Thach,
 * tinyusb.org), cross-checked against IDF's esp_hal_usb/esp32p4/include/hal/
 * usb_dwc_ll.h. The bring-up order is IDF's, which is the known-good on this
 * chip: hal/usb_utmi_ll.h and usb_utmi_hal.c for the PHY's clocks, reset and
 * control bits, usb_dwc_hal.c's set_defaults() for the core. The channel
 * handling follows TinyUSB's hcd_dwc2.c in slave (FIFO) mode -- not the
 * core's DMA, which would need cache maintenance on every transfer. The P4
 * system registers (clocks, resets, SUSPENDM, pulldowns) are generated from
 * IDF's headers into drivers/include/drivers/esp32p4_usb_regs.h.
 *
 * ## What is not obvious
 *
 * **1. A channel per transaction.** A DWC2 channel can run a whole transfer
 * (many packets, NAK retries by itself); the shared host wants one
 * transaction and the handshake that ended it, because that is what the
 * RP2350's engine gives. So every transaction here is a channel programmed
 * for one packet (PKTCNT 1) with the PID the host asks for, and stopped
 * (CHDIS) at the first outcome: XferCompl (ACK/DATA), NAK, STALL, a
 * transaction error, babble or a toggle error. Channel 0 does all of them:
 * the shared host is one task and has one transaction in flight.
 *
 * **2. Interrupt endpoints go through the non-periodic queue.** The keyboard
 * is polled every bInterval ms by the task's own clock, so the channel is
 * programmed as bulk: the core sends the IN token at once instead of
 * scheduling it for an odd or even frame. On the wire an IN is an IN.
 *
 * **3. Full and low speed on the HS root port.** Nothing to set: the core
 * runs the UTMI+ PHY at the speed the port reset found (HPRT.PrtSpd), and
 * IDF leaves HCFG/HFIR alone for this PHY. LSPDDEV is only for a low-speed
 * device behind a *full-speed* hub (IDF's ls_via_fs_hub). Devices behind a
 * *high-speed* hub need split transactions, which this port does not do
 * (47.10's stretch goal).
 *
 * **4. v3 silicon wants two PHY bits set by software**: SUSPENDM (else a
 * detach is not seen, IDF-9953) and the 15k D+/D- pulldowns of host mode,
 * which on v3 are LP_SYSTEM's and not the controller's. */

#include "drivers/usbkbd.h"
#include "drivers/usbkbd_task.h"
#include "kernel/sysreg.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "lugalos_config.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(CONFIG_BOARD_ESP32P4) && defined(CONFIG_USBHOST_DWC2)

#include "drivers/esp32p4_usb_regs.h"

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* --- The OTG HS core (IDF ld/esp32p4.peripherals.ld: USB_DWC_HS, USB_UTMI) - */

#define OTG_BASE        0x50000000u
#define UTMI_BASE       0x5009C000u
#define UTMI_FC_06      (UTMI_BASE + 0x18u)     /* usb_utmi_struct.h: fc_06 */
#define UTMI_LS_PAR_EN  (1u << 0)
#define UTMI_LS_KPALV_EN (1u << 3)

#define OTG(off)        REG(OTG_BASE + (off))
#define GOTGCTL         0x000u
#define GAHBCFG         0x008u
#define GUSBCFG         0x00Cu
#define GRSTCTL         0x010u
#define GINTSTS         0x014u
#define GINTMSK         0x018u
#define GRXSTSP         0x020u
#define GRXFSIZ         0x024u
#define GNPTXFSIZ       0x028u
#define GNPTXSTS        0x02Cu
#define GSNPSID         0x040u
#define GHWCFG2         0x048u
#define GHWCFG3         0x04Cu
#define GHWCFG4         0x050u
#define HPTXFSIZ        0x100u
#define HCFG            0x400u
#define HFIR            0x404u
#define HFNUM           0x408u
#define HPRT            0x440u
#define HCCHAR(n)       (0x500u + 0x20u * (n))
#define HCSPLT(n)       (0x504u + 0x20u * (n))
#define HCINT(n)        (0x508u + 0x20u * (n))
#define HCINTMSK(n)     (0x50Cu + 0x20u * (n))
#define HCTSIZ(n)       (0x510u + 0x20u * (n))
#define PCGCCTL         0xE00u
#define FIFO(n)         (0x1000u * ((n) + 1u))

#define GOTGCTL_BVALIDOVEN      (1u << 6)
#define GAHBCFG_GINT            (1u << 0)
#define GAHBCFG_DMAEN           (1u << 5)
#define GUSBCFG_TOCAL_MASK      0x7u
#define GUSBCFG_PHYIF16         (1u << 3)
#define GUSBCFG_ULPI_UTMI_SEL   (1u << 4)
#define GUSBCFG_PHYSEL          (1u << 6)
#define GUSBCFG_SRPCAP          (1u << 8)
#define GUSBCFG_HNPCAP          (1u << 9)
#define GUSBCFG_FHMOD           (1u << 29)
#define GUSBCFG_FDMOD           (1u << 30)
#define GRSTCTL_CSRST           (1u << 0)
#define GRSTCTL_RXFFLSH         (1u << 4)
#define GRSTCTL_TXFFLSH         (1u << 5)
#define GRSTCTL_TXFNUM_ALL      (0x10u << 6)
#define GRSTCTL_CSRST_DONE      (1u << 29)
#define GRSTCTL_AHBIDL          (1u << 31)
#define GINTSTS_CMOD            (1u << 0)
#define GINTSTS_RXFLVL          (1u << 4)
#define GSNPSID_4_20A           0x4F54420Au
#define HCFG_FSLS_ONLY          (1u << 2)
#define HPRT_CONN_STATUS        (1u << 0)
#define HPRT_CONN_DETECT        (1u << 1)   /* W1C */
#define HPRT_ENABLE             (1u << 2)   /* W1C -- writing 1 disables the port */
#define HPRT_ENABLE_CHANGE      (1u << 3)   /* W1C */
#define HPRT_OVER_CURRENT       (1u << 4)
#define HPRT_OVER_CURRENT_CHANGE (1u << 5)  /* W1C */
#define HPRT_RESET              (1u << 8)
#define HPRT_POWER              (1u << 12)
#define HPRT_SPEED(v)           (((v) >> 17) & 3u)
#define HPRT_W1C                (HPRT_CONN_DETECT | HPRT_ENABLE | HPRT_ENABLE_CHANGE | HPRT_OVER_CURRENT_CHANGE)
#define HPRT_SPEED_HIGH         0u
#define HPRT_SPEED_FULL         1u
#define HPRT_SPEED_LOW          2u
#define HCCHAR_EPNUM_POS        11u
#define HCCHAR_EPDIR_IN         (1u << 15)
#define HCCHAR_LSDEV            (1u << 17)
#define HCCHAR_EPTYP_POS        18u
#define HCCHAR_MC_1             (1u << 20)
#define HCCHAR_DAD_POS          22u
#define HCCHAR_CHDIS            (1u << 30)
#define HCCHAR_CHENA            (1u << 31)
#define EPTYP_CONTROL           0u
#define EPTYP_BULK              2u
#define HCSPLT_SPLITEN          (1u << 31)
#define HCSPLT_COMPSPLT         (1u << 16)
#define HCSPLT_XACTPOS_ALL      (3u << 14)
#define HCSPLT_HUBADDR_POS      7u
#define HCTSIZ_PKTCNT_1         (1u << 19)
#define HCTSIZ_PID_POS          29u
#define PID_DATA0               0u
#define PID_DATA1               2u
#define PID_SETUP               3u
#define HCINT_XFRC              (1u << 0)
#define HCINT_HALTED            (1u << 1)
#define HCINT_AHBERR            (1u << 2)
#define HCINT_STALL             (1u << 3)
#define HCINT_NAK               (1u << 4)
#define HCINT_ACK               (1u << 5)
#define HCINT_NYET              (1u << 6)
#define HCINT_XACTERR           (1u << 7)
#define HCINT_BBLERR            (1u << 8)
#define HCINT_FRMOR             (1u << 9)
#define HCINT_DTERR             (1u << 10)
#define HCINT_OUTCOME           (HCINT_XFRC | HCINT_STALL | HCINT_NAK | HCINT_XACTERR | HCINT_BBLERR | \
                                 HCINT_FRMOR | HCINT_DTERR | HCINT_AHBERR)
#define GRXSTS_CH(v)            ((v) & 0xFu)
#define GRXSTS_BCNT(v)          (((v) >> 4) & 0x7FFu)
#define GRXSTS_PKTSTS(v)        (((v) >> 17) & 0xFu)
#define PKTSTS_IN_DATA          2u

/* FIFO RAM, in 32-bit words (TinyUSB's dfifo_host_init() rule: RX holds two
 * of the largest packets plus status words and one per channel; the TX
 * FIFOs at least the largest packet). Full/low speed only: 64-byte packets. */
#define FIFO_RX_WORDS   128u
#define FIFO_NPTX_WORDS 64u
#define FIFO_PTX_WORDS  64u

#define CH              0u          /* every transaction runs on channel 0 */
#define MPS             64u         /* the most any FS/LS packet carries */

typedef struct {
    bool     up;                    /* the core answered and is in host mode */
    uint32_t snpsid, hwcfg2, hwcfg3, hwcfg4;
    uint32_t speed;                 /* HPRT_SPEED_* of the last port reset */
    uint32_t resets, xfers, naks, stalls, xacterrs, toggles, babbles, stuck, halts_late;
    uint32_t splits, nyets;
    uint32_t last_hcint;
    uint64_t busy_us;               /* spent inside transactions (they poll) */
} usbhost_t;

/* single task: the kbd task; host_init() runs at boot (usbkbd_start()) before
 * it, and `usb` only reads. */
static usbhost_t g_host;

/* single task: the keyboard host (`kbd` starts and stops it). */
static union {
    usbkbd_state_t s;
    uint8_t        raw[1024];
} g_kbd_mem __attribute__((aligned(8)));
#define g_kbd g_kbd_mem.s

static bool wait_set(uint32_t off, uint32_t mask, uint32_t us) {
    return time_poll_until_us(OTG(off) & mask, time_get_us() + us);
}

static bool wait_clear(uint32_t off, uint32_t mask, uint32_t us) {
    return time_poll_until_us(!(OTG(off) & mask), time_get_us() + us);
}

/* --- Bring-up ------------------------------------------------------------- */

/* IDF usb_utmi_hal.c _usb_utmi_hal_init() + usb_phy.c's host mode: the core's
 * bus clock and the PHY's 48 MHz reference on, both out of reset, SUSPENDM
 * forced high, LS in parallel mode with keep-alives, pulldowns on. */
static void phy_init(void) {
    sysreg_set(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, 1u << HP_SYS_CLKRST_REG_USB_OTG20_SYS_CLK_EN_S);
    sysreg_set(LP_CLKRST_HP_USB_CLKRST_CTRL1_REG, 1u << LP_CLKRST_USB_OTG20_PHYREF_CLK_EN_S);
    sysreg_set(LP_CLKRST_HP_USB_CLKRST_CTRL1_REG, 1u << LP_CLKRST_RST_EN_USB_OTG20_S);
    sysreg_set(LP_CLKRST_HP_USB_CLKRST_CTRL1_REG, 1u << LP_CLKRST_RST_EN_USB_OTG20_PHY_S);
    sysreg_clear(LP_CLKRST_HP_USB_CLKRST_CTRL1_REG, 1u << LP_CLKRST_RST_EN_USB_OTG20_PHY_S);
    sysreg_clear(LP_CLKRST_HP_USB_CLKRST_CTRL1_REG, 1u << LP_CLKRST_RST_EN_USB_OTG20_S);
    REG(HP_SYSTEM_SYS_USBOTG20_CTRL_REG) |= 1u << HP_SYSTEM_SYS_OTG_SUSPENDM_S;
    REG(UTMI_FC_06) |= UTMI_LS_PAR_EN | UTMI_LS_KPALV_EN;
    REG(LP_SYSTEM_REG_HP_USB_OTGHS_PHY_CTRL_REG) |= (1u << LP_SYSTEM_REG_HP_UTMIOTG_DPPULLDOWN_S) |
                                                    (1u << LP_SYSTEM_REG_HP_UTMIOTG_DMPULLDOWN_S);
}

/* IDF usb_dwc_ll_grstctl_core_soft_reset(): from v4.20a CSRST does not
 * clear itself; CSRST_DONE says it is over, and both are cleared by hand. */
static bool core_reset(void) {
    if (!wait_set(GRSTCTL, GRSTCTL_AHBIDL, 100000)) return false;
    OTG(GRSTCTL) |= GRSTCTL_CSRST;
    if (g_host.snpsid < GSNPSID_4_20A) {
        if (!wait_clear(GRSTCTL, GRSTCTL_CSRST, 100000)) return false;
    } else {
        if (!wait_set(GRSTCTL, GRSTCTL_CSRST_DONE, 100000)) return false;
        OTG(GRSTCTL) = (OTG(GRSTCTL) & ~GRSTCTL_CSRST) | GRSTCTL_CSRST_DONE;
    }
    return wait_set(GRSTCTL, GRSTCTL_AHBIDL, 100000);
}

static void fifo_flush(void) {
    OTG(GRSTCTL) = GRSTCTL_TXFFLSH | GRSTCTL_TXFNUM_ALL;
    (void)wait_clear(GRSTCTL, GRSTCTL_TXFFLSH, 10000);
    OTG(GRSTCTL) = GRSTCTL_RXFFLSH;
    (void)wait_clear(GRSTCTL, GRSTCTL_RXFFLSH, 10000);
}

static bool host_init(void) {
    phy_init();
    g_host.snpsid = OTG(GSNPSID);
    if ((g_host.snpsid & 0xFFFF0000u) != 0x4F540000u) {    /* "OT" */
        printk("[USB] OTG HS core does not answer (GSNPSID %08lx)\n", (unsigned long)g_host.snpsid);
        return false;
    }
    g_host.hwcfg2 = OTG(GHWCFG2);
    g_host.hwcfg3 = OTG(GHWCFG3);
    g_host.hwcfg4 = OTG(GHWCFG4);
    OTG(GAHBCFG) &= ~GAHBCFG_GINT;
    if (!core_reset()) {
        printk("[USB] OTG HS core soft reset did not complete\n");
        return false;
    }
    /* IDF set_defaults(), P4 part: no B-valid override, PHY clock running. */
    OTG(GOTGCTL) &= ~GOTGCTL_BVALIDOVEN;
    OTG(PCGCCTL) = 0;
    OTG(GAHBCFG) &= ~(GAHBCFG_DMAEN | GAHBCFG_GINT);   /* slave mode, polled */
    uint32_t c = OTG(GUSBCFG);
    c &= ~(GUSBCFG_SRPCAP | GUSBCFG_HNPCAP | GUSBCFG_PHYSEL | GUSBCFG_ULPI_UTMI_SEL | GUSBCFG_TOCAL_MASK | GUSBCFG_FDMOD);
    c |= GUSBCFG_PHYIF16 | 5u;      /* UTMI+, 16 bits; 5 PHY clocks of timeout calibration */
    c |= GUSBCFG_FHMOD;
    OTG(GUSBCFG) = c;
    OTG(GINTMSK) = 0;
    OTG(GINTSTS) = 0xFFFFFFFFu;
    /* Forcing host mode takes effect after up to 25 ms (databook). */
    if (!time_poll_until_us(OTG(GINTSTS) & GINTSTS_CMOD, time_get_us() + 100000u)) {
        printk("[USB] core did not enter host mode\n");
        return false;
    }
    OTG(HCFG) &= ~HCFG_FSLS_ONLY;
    OTG(GRXFSIZ) = FIFO_RX_WORDS;
    OTG(GNPTXFSIZ) = (FIFO_NPTX_WORDS << 16) | FIFO_RX_WORDS;
    OTG(HPTXFSIZ) = (FIFO_PTX_WORDS << 16) | (FIFO_RX_WORDS + FIFO_NPTX_WORDS);
    fifo_flush();
    OTG(HPRT) = (OTG(HPRT) & ~HPRT_W1C) | HPRT_POWER;
    g_host.up = true;
    return true;
}

/* --- One transaction ------------------------------------------------------ */

/* Pops what the RX FIFO holds for the channel: IN data into in[] (what does
 * not fit is read and dropped -- the FIFO must be drained either way). */
static void rx_drain(uint8_t *in, uint32_t in_max, uint32_t *got) {
    while (OTG(GINTSTS) & GINTSTS_RXFLVL) {
        uint32_t st = OTG(GRXSTSP);
        uint32_t n = GRXSTS_BCNT(st);
        if (GRXSTS_PKTSTS(st) != PKTSTS_IN_DATA || n == 0) continue;
        for (uint32_t i = 0; i < n; i += 4) {
            uint32_t w = REG(OTG_BASE + FIFO(0));
            for (uint32_t b = 0; b < 4 && i + b < n; b++)
                if (in && *got < in_max) in[(*got)++] = (uint8_t)(w >> (8 * b));
        }
    }
}

/* Stops the channel if the core has not, and waits for HALTED. */
static void channel_stop(uint8_t *in, uint32_t in_max, uint32_t *got) {
    if (OTG(HCCHAR(CH)) & HCCHAR_CHENA) OTG(HCCHAR(CH)) |= HCCHAR_CHENA | HCCHAR_CHDIS;
    uint64_t end = time_get_us() + 5000;
    for (;;) {
        bool late = time_get_us() >= end;   /* before the test: kernel/time.h */
        if ((OTG(HCINT(CH)) & HCINT_HALTED) || !(OTG(HCCHAR(CH)) & HCCHAR_CHENA)) break;
        rx_drain(in, in_max, got);
        if (late) { g_host.stuck++; break; }
    }
    rx_drain(in, in_max, got);
}

/* One phase on channel 0 -- a whole transaction, or one half of a split --
 * programmed, an OUT's data pushed, run to the first of `until`, stopped.
 * Returns what HCINT said (0: nothing within 20 ms). */
static uint32_t phase(uint32_t hcchar, uint32_t hcsplt, uint32_t size, uint32_t pid, const uint8_t *out,
                      uint8_t *in, uint32_t in_max, uint32_t *got, uint32_t until) {
    if (OTG(HCCHAR(CH)) & HCCHAR_CHENA) channel_stop(0, 0, 0);   /* left over: should not happen */
    OTG(HCINT(CH)) = 0xFFFFFFFFu;
    OTG(HCINTMSK(CH)) = HCINT_OUTCOME | HCINT_HALTED | HCINT_ACK | HCINT_NYET;
    OTG(HCSPLT(CH)) = hcsplt;
    OTG(HCCHAR(CH)) = hcchar;
    OTG(HCTSIZ(CH)) = size | HCTSIZ_PKTCNT_1 | (pid << HCTSIZ_PID_POS);
    OTG(HCCHAR(CH)) = hcchar | HCCHAR_CHENA;
    if (out && size) {
        /* A free non-periodic TX FIFO has room for a whole packet. */
        for (uint32_t i = 0; i < size; i += 4) {
            uint32_t w = 0;
            for (uint32_t b = 0; b < 4 && i + b < size; b++) w |= (uint32_t)out[i + b] << (8 * b);
            REG(OTG_BASE + FIFO(CH)) = w;
        }
    }
    uint32_t hcint = 0;
    uint64_t end = time_get_us() + 20000;
    for (;;) {
        rx_drain(in, in_max, got);
        hcint = OTG(HCINT(CH));
        if (hcint & until) break;
        if (time_get_us() > end) break;
    }
    channel_stop(in, in_max, got);
    hcint |= OTG(HCINT(CH));
    OTG(HCINT(CH)) = 0xFFFFFFFFu;
    return hcint;
}

uint32_t usbkbd_port_xfer(usbkbd_state_t *k, uint32_t op, uint32_t addr, uint32_t ep, uint32_t data1,
                          const uint8_t *data, uint32_t len, uint8_t *in, uint32_t in_max, uint32_t *in_len) {
    if (in_len) *in_len = 0;
    if (!g_host.up) return PIOUSB_ENGINE;
    uint32_t hprt = OTG(HPRT);
    if (op == PIOUSB_OP_RESET) {
        if (!(hprt & HPRT_CONN_STATUS)) return PIOUSB_NODEV;
        OTG(HPRT) = (hprt & ~HPRT_W1C) | HPRT_RESET;
        task_sleep_ms(50);                      /* TDRSTR: 50 ms at the root */
        OTG(HPRT) = OTG(HPRT) & ~(HPRT_W1C | HPRT_RESET);
        uint64_t end = time_get_ms() + 200;
        for (;;) {
            bool late = time_get_ms() >= end;   /* before the test: kernel/time.h */
            if (OTG(HPRT) & HPRT_ENABLE) break;
            if (late) return PIOUSB_NODEV;
            task_sleep_ms(1);
        }
        hprt = OTG(HPRT);
        OTG(HPRT) = (hprt & ~HPRT_W1C) | (hprt & (HPRT_CONN_DETECT | HPRT_ENABLE_CHANGE | HPRT_OVER_CURRENT_CHANGE));
        g_host.speed = HPRT_SPEED(hprt);
        g_host.resets++;
        k->ls = (uint8_t)(g_host.speed == HPRT_SPEED_LOW);
        k->hs = (uint8_t)(g_host.speed == HPRT_SPEED_HIGH);
        return PIOUSB_OK;
    }
    if (!(hprt & HPRT_CONN_STATUS) || !(hprt & HPRT_ENABLE)) return PIOUSB_NODEV;

    uint64_t t0 = time_get_us();
    bool is_in = op == PIOUSB_OP_IN;
    uint32_t pid = op == PIOUSB_OP_SETUP ? PID_SETUP : (data1 ? PID_DATA1 : PID_DATA0);
    uint32_t type = ep == 0 ? EPTYP_CONTROL : EPTYP_BULK;
    /* LSPDDEV: a low-speed device the root port does not already run at low
     * speed -- behind a full-speed hub (PRE) or a high-speed one (the TT). */
    bool lsdev = k->ls && g_host.speed != HPRT_SPEED_LOW;
    bool split = k->tt_port && !k->hs && g_host.speed == HPRT_SPEED_HIGH;
    uint32_t hcchar = MPS | ((ep & 0xFu) << HCCHAR_EPNUM_POS) | (is_in ? HCCHAR_EPDIR_IN : 0u) |
                      (lsdev ? HCCHAR_LSDEV : 0u) | (type << HCCHAR_EPTYP_POS) | HCCHAR_MC_1 |
                      ((addr & 0x7Fu) << HCCHAR_DAD_POS);
    uint32_t size = is_in ? MPS : (len > MPS ? MPS : len);
    uint32_t got = 0, hcint;
    bool ok = false;

    if (!split) {
        hcint = phase(hcchar, 0, size, pid, is_in ? 0 : data, in, in_max, &got, HCINT_OUTCOME | HCINT_NYET);
        /* A high-speed OUT answered NYET was accepted (the endpoint just
         * may not take the next one yet: PING, which we do not need). */
        ok = (hcint & HCINT_XFRC) || (!is_in && (hcint & HCINT_NYET));
    } else {
        /* Start split: the hub's TT takes the token (and an OUT's data) and
         * ACKs; NAK means its buffer is busy. Complete split: the device's
         * answer, or NYET while the TT is still waiting for it. Size 0 for an
         * OUT's complete split -- the data went with the start (Linux dwc2). */
        uint32_t sp = HCSPLT_SPLITEN | HCSPLT_XACTPOS_ALL | ((uint32_t)k->hub_addr << HCSPLT_HUBADDR_POS) |
                      (k->tt_port & 0x7Fu);
        g_host.splits++;
        hcint = phase(hcchar, sp, size, pid, is_in ? 0 : data, 0, 0, &got,
                      HCINT_OUTCOME | HCINT_ACK | HCINT_NYET);
        if ((hcint & HCINT_ACK) && !(hcint & (HCINT_STALL | HCINT_NAK | HCINT_XACTERR | HCINT_BBLERR))) {
            uint64_t end = time_get_us() + 10000;
            uint32_t errs = 0;
            for (;;) {
                hcint = phase(hcchar, sp | HCSPLT_COMPSPLT, is_in ? MPS : 0, pid, 0, in, in_max, &got,
                              HCINT_OUTCOME | HCINT_NYET);
                if (hcint & (HCINT_XFRC | HCINT_STALL | HCINT_NAK | HCINT_BBLERR | HCINT_DTERR)) break;
                if ((hcint & HCINT_XACTERR) && ++errs >= 3) break;
                if (hcint & HCINT_NYET) g_host.nyets++;
                if (time_get_us() > end) break;
            }
            ok = (hcint & HCINT_XFRC) != 0 ||
                 (!is_in && (hcint & HCINT_ACK) && !(hcint & (HCINT_STALL | HCINT_NAK)));
        }
    }

    g_host.last_hcint = hcint;
    g_host.xfers++;
    k->xfers++;
    g_host.busy_us += time_get_us() - t0;
    if (in_len) *in_len = got;
    if (hcint & HCINT_STALL)   { g_host.stalls++;  return PIOUSB_STALL; }
    if (hcint & HCINT_BBLERR)  { g_host.babbles++; return PIOUSB_PROTOCOL; }
    if (hcint & HCINT_DTERR)   { g_host.toggles++; return PIOUSB_TOGGLE; }
    if (ok)                    return PIOUSB_OK;
    if (hcint & HCINT_NAK)     { g_host.naks++;    return PIOUSB_NAK; }
    if (hcint & (HCINT_XACTERR | HCINT_FRMOR)) { g_host.xacterrs++; return PIOUSB_TIMEOUT; }
    if (hcint & HCINT_AHBERR)  return PIOUSB_ENGINE;
    return PIOUSB_TIMEOUT;
}

uint32_t usbkbd_port_ms(void) { return (uint32_t)time_get_ms(); }
void usbkbd_port_sleep(uint32_t ms) { task_sleep_ms(ms); }

bool usbkbd_port_up(usbkbd_state_t *k) {
    uint32_t hprt = OTG(HPRT);
    return (hprt & HPRT_CONN_STATUS) && (hprt & HPRT_ENABLE) && !k->off_request;
}

/* --- The task ------------------------------------------------------------- */

/* single task: the boot, before any reader runs; constant after. */
static int g_kbd_pid = -1;

static void kbd_task_body(void *arg) {
    usbkbd_state_t *k = (usbkbd_state_t *)arg;
    for (;;) {
        usbkbd_host_forget(k);
        if (k->off_request) {
            k->phase = USBKBD_PH_OFF;
            while (k->off_request) task_sleep_ms(50);
        }
        k->phase = USBKBD_PH_WAIT;
        while (!(OTG(HPRT) & HPRT_CONN_STATUS) && !k->off_request) task_sleep_ms(100);
        if (k->off_request) continue;
        task_sleep_ms(100);                     /* attach debounce */
        if (usbkbd_host_session(k)) k->root_detaches++;
        /* Acknowledge the detach (or whatever ended the session). */
        OTG(HPRT) = (OTG(HPRT) & ~HPRT_W1C) | (OTG(HPRT) & (HPRT_CONN_DETECT | HPRT_ENABLE_CHANGE | HPRT_OVER_CURRENT_CHANGE));
    }
}

int usbkbd_start(void) {
    if (!host_init()) return -1;
    int pid = task_create_driver("kbd", kbd_task_body, &g_kbd, 2);
    if (pid < 0) {
        printk("[KBD] could not start the kbd task\n");
        return -1;
    }
    g_kbd_pid = pid;
    usbkbd_host_attach(&g_kbd);
    printk("[KBD] USB keyboard task #%d on the OTG HS port (core %x.%03x, %lu channels)\n", pid,
           (unsigned)((g_host.snpsid >> 12) & 0xF), (unsigned)(g_host.snpsid & 0xFFF),
           (unsigned long)(((g_host.hwcfg2 >> 14) & 0xFu) + 1u));
    return pid;
}

bool usbkbd_set_off(bool off) {
    if (g_kbd_pid < 0) return false;
    g_kbd.off_request = off ? 1u : 0u;
    uint64_t end = time_get_ms() + 3000;
    while (time_get_ms() < end) {
        if (off == (g_kbd.phase == USBKBD_PH_OFF)) return true;
        task_sleep_ms(10);
    }
    return false;
}

/* --- `usb` ---------------------------------------------------------------- */

static const char *speed_name(uint32_t s) {
    return s == HPRT_SPEED_HIGH ? "high" : s == HPRT_SPEED_FULL ? "full" : s == HPRT_SPEED_LOW ? "low" : "?";
}

static void print_config(const uint8_t *cfg, uint32_t len) {
    usb_cfg_info_t ci;
    if (len < 9 || !usb_parse_config(cfg, len, &ci)) {
        cprintf("usb:   configuration descriptor: none read, or malformed\n");
        return;
    }
    cprintf("usb:   configuration %u: %u bytes, %u interface(s), %s%s, %u mA\n", ci.cfg_value,
            (unsigned)(cfg[2] | (cfg[3] << 8)), cfg[4], (cfg[7] & 0x40) ? "self-powered" : "bus-powered",
            (cfg[7] & 0x20) ? ", remote wakeup" : "", cfg[8] * 2u);
    if (ci.kbd_found)
        cprintf("usb:   boot keyboard: interface %u, interrupt IN endpoint %u, %u bytes every %u ms\n",
                ci.kbd_iface, ci.kbd_ep, ci.kbd_mps, ci.kbd_interval);
    if (ci.is_hub) cprintf("usb:   hub: status-change endpoint %u\n", ci.hub_ep);
    /* Every descriptor in it, one line each: length, type, the bytes. */
    for (uint32_t i = 0; i + 2 <= len && cfg[i] >= 2 && i + cfg[i] <= len; i += cfg[i]) {
        static const char *const names[] = { "?", "device", "config", "string", "interface", "endpoint" };
        uint32_t t = cfg[i + 1];
        cprintf("usb:     %-9s", t < 6 ? names[t] : t == 0x21 ? "HID" : t == 0x29 ? "hub" : "other");
        for (uint32_t j = 0; j < cfg[i]; j++) cprintf(" %02x", cfg[i + j]);
        cprintf("\n");
    }
}

void usbhost_command(const char *args) {
    while (*args == ' ') args++;
    if (!g_host.up) { cprintf("usb: the OTG HS controller is not up\n"); return; }
    if (args[0] == 'r') {                       /* `usb regs`: for diffs against IDF */
        static const uint16_t offs[] = { GOTGCTL, GAHBCFG, GUSBCFG, GRSTCTL, GINTSTS, GINTMSK, GRXFSIZ,
                                         GNPTXFSIZ, GNPTXSTS, GSNPSID, GHWCFG2, GHWCFG3, GHWCFG4, HPTXFSIZ,
                                         HCFG, HFIR, HFNUM, HPRT, HCCHAR(0), HCINT(0), HCTSIZ(0), PCGCCTL };
        for (uint32_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
            cprintf("usb: +%03x = %08lx\n", offs[i], (unsigned long)OTG(offs[i]));
        cprintf("usb: clk ctrl1 %08lx  lp usb ctrl1 %08lx  suspendm %08lx  pulldowns %08lx  utmi fc_06 %08lx\n",
                (unsigned long)REG(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG), (unsigned long)REG(LP_CLKRST_HP_USB_CLKRST_CTRL1_REG),
                (unsigned long)REG(HP_SYSTEM_SYS_USBOTG20_CTRL_REG),
                (unsigned long)REG(LP_SYSTEM_REG_HP_USB_OTGHS_PHY_CTRL_REG), (unsigned long)REG(UTMI_FC_06));
        return;
    }
    uint32_t hprt = OTG(HPRT);
    cprintf("usb: OTG HS core %x.%03x, %lu host channels, FIFO %lu words; slave mode, polled\n",
            (unsigned)((g_host.snpsid >> 12) & 0xF), (unsigned)(g_host.snpsid & 0xFFF),
            (unsigned long)(((g_host.hwcfg2 >> 14) & 0xFu) + 1u), (unsigned long)(g_host.hwcfg3 >> 16));
    cprintf("usb: port %s%s%s, %s speed; frame %lu\n", (hprt & HPRT_CONN_STATUS) ? "connected" : "empty",
            (hprt & HPRT_ENABLE) ? ", enabled" : "", (hprt & HPRT_OVER_CURRENT) ? ", OVER CURRENT" : "",
            speed_name(HPRT_SPEED(hprt)), (unsigned long)(OTG(HFNUM) & 0xFFFFu));
    cprintf("usb: %lu port resets, %lu transactions: %lu NAK, %lu STALL, %lu errors, %lu toggle, %lu babble, %lu stuck\n",
            (unsigned long)g_host.resets, (unsigned long)g_host.xfers, (unsigned long)g_host.naks,
            (unsigned long)g_host.stalls, (unsigned long)g_host.xacterrs, (unsigned long)g_host.toggles,
            (unsigned long)g_host.babbles, (unsigned long)g_host.stuck);
    if (g_host.splits)
        cprintf("usb: %lu split transactions through the hub's TT, %lu NYETs\n",
                (unsigned long)g_host.splits, (unsigned long)g_host.nyets);
    if (g_host.xfers) {
        uint64_t up = time_get_us();
        cprintf("usb: %lu us per transaction; %lu.%02lu%% of the time since boot spent in them\n",
                (unsigned long)(g_host.busy_us / g_host.xfers), (unsigned long)(g_host.busy_us * 100u / up),
                (unsigned long)(g_host.busy_us * 10000u / up % 100u));
    }
    usbkbd_report();
    const usbkbd_state_t *k = &g_kbd;
    const uint8_t *d = k->desc18;
    if (d[0] == 18 && d[1] == 1) {
        cprintf("usb: device descriptor of the last device enumerated:\n");
        cprintf("usb:   USB %x.%02x, class %02x/%02x/%02x, EP0 %u bytes, %04x:%04x release %x.%02x, %u configuration(s)\n",
                d[3], d[2], d[4], d[5], d[6], d[7], (unsigned)(d[8] | (d[9] << 8)),
                (unsigned)(d[10] | (d[11] << 8)), d[13], d[12], d[17]);
        print_config(k->buf, (uint32_t)(k->buf[2] | (k->buf[3] << 8)) <= sizeof(k->buf)
                                 ? (uint32_t)(k->buf[2] | (k->buf[3] << 8)) : sizeof(k->buf));
    } else {
        cprintf("usb: no device enumerated yet\n");
    }
}

#else

void usbhost_command(const char *args) { (void)args; cprintf("usb: this board has no USB host port\n"); }

#endif
