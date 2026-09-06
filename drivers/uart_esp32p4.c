/*
 * LugalOS driver: UART0 on the ESP32-P4 -- E2,
 * plan/phase27_esp32p4_bringup.md.
 *
 * The console, and in E2 the only wire this board has.
 *
 * ## Why this configures hardware the boot ROM already configured
 *
 * E1 established that the ROM hands over a fully working UART0: clocked,
 * muxed onto GPIO37/38, 115200 8N1. tools/minimal_esp32p4.c therefore wrote
 * bytes and nothing else, which was exactly right for a program whose job
 * was to prove the chip was alive.
 *
 * A kernel may not do that. "It works because of what the loader left
 * behind" is a property of one boot path, and this image is meant to acquire
 * more of them: E6 boots it from flash through a second-stage bootloader,
 * which arrives with different peripheral state, and anything that touches
 * the clock tree later can change the divisor under a console that never
 * asked for one. So this file states every value it depends on.
 *
 * ## Why the source clock is the crystal
 *
 * XTAL_CLK, not the 80 MHz PLL. The board's crystal is 40 MHz
 * (CONFIG_XTAL_HZ, and esptool reports it on every connect), and unlike the
 * PLL it does not move when the CPU clock does. Selecting it means the
 * console's baud rate does not depend on the clock tree at all -- so E2 gets
 * a shell without bringing the PLL up, and no later milestone can take the
 * console away by changing a CPU frequency. The cost is that 115200 comes
 * out of a 40 MHz divisor rather than an 80 MHz one, which changes the error
 * from 0.00% to 0.008%: about 1/10000th of a bit period across a 10-bit
 * frame, which is four orders of magnitude inside what a UART tolerates.
 *
 * ## Polled, deliberately
 *
 * No interrupts anywhere in this file. The P4's interrupt controller is a
 * non-standard CLIC (E0 section 5) and E3 is the milestone that brings it
 * up; until then there is no route from a peripheral to a handler, and a
 * driver that pretended otherwise would be untestable code sitting in the
 * boot path. So both blocking primitives spin with sched_yield(), which is
 * what drivers/uart_16550.c already falls back to when its own ISR slots are
 * taken. E3 adds the ISR here beside them, and the two paths will then have
 * the same shape as that file's.
 *
 * The consequence to know about: with no preemption timer either (E4), a
 * task that blocks on a keystroke yields to whatever else is READY, and if
 * nothing else is, it spins. That is the cooperative behaviour E2 asks for,
 * not an oversight.
 *
 * ## Register provenance
 *
 * Every offset and bit position below was read from the ESP32-P4 TRM
 * chapters 16 and 45 and cross-checked against ESP-IDF's generated
 * components/soc/esp32p4/register/hw_ver1/soc/uart_reg.h -- hw_ver1 because
 * this board is chip revision v1.3. Nothing here is carried over from
 * another Espressif part; section 3.2 of the phase plan says why, and phase
 * 24's 0x88888888 is what it cost the last time that rule was skipped.
 */

#include "drivers/uart.h"
#include "drivers/uart_net.h"
#include "kernel/sched.h"
#include "kernel/hart.h"
#include "kernel/lock.h"
#include "kernel/chan.h"
#include "kernel/printk.h"
#include "lugalos_config.h"
#include <string.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* --- UART0 registers (TRM chapter 45, Register Summary) ----------------- */

static uintptr_t g_uart_base = CONFIG_UART0_BASE;

#define UART_FIFO(base)         ((base) + 0x00)  /* RO/WO FIFO data port     */
#define UART_CLKDIV_SYNC(base)  ((base) + 0x14)  /* baud divisor, int + frac */
#define UART_STATUS(base)       ((base) + 0x1c)  /* RXFIFO_CNT / TXFIFO_CNT  */
#define UART_CONF0_SYNC(base)   ((base) + 0x20)  /* frame format, FIFO reset */
#define UART_CONF1(base)        ((base) + 0x24)
#define UART_CLK_CONF(base)     ((base) + 0x88)  /* core clock enable/reset  */
#define UART_REG_UPDATE(base)   ((base) + 0x98)  /* commit the _SYNC regs    */

/* UART_STATUS_REG (TRM Register 45.21). Both counters are 8 bits, and the
 * FIFO is 128 bytes deep (SOC_UART_FIFO_LEN), so neither can wrap.
 *
 * Note the TRM's own text for TXFIFO_CNT reads "the number of valid data
 * bytes in RX FIFO" -- a copy-paste slip in the manual, not a second RX
 * counter; the field name, the bit range and IDF's header all agree it is
 * the TX side. Worth writing down because E1 left "a TXFIFO_CNT field this
 * tree has not yet verified against hardware" as an open suspect for a
 * garbled-output report, and this is that field, now verified. */
#define UART_RXFIFO_CNT_SHIFT   0
#define UART_RXFIFO_CNT_MASK    0xffu
#define UART_TXFIFO_CNT_SHIFT   16
#define UART_TXFIFO_CNT_MASK    0xffu
#define UART_FIFO_DEPTH         128u

/* UART_CONF0_SYNC_REG (TRM Register 45.9). */
#define UART_PARITY_EN          (1u << 1)
#define UART_BIT_NUM_SHIFT      2       /* 0=5b 1=6b 2=7b 3=8b */
#define UART_BIT_NUM_MASK       (3u << UART_BIT_NUM_SHIFT)
#define UART_BIT_NUM_8          (3u << UART_BIT_NUM_SHIFT)
#define UART_STOP_BIT_NUM_SHIFT 4       /* 1=1b 2=1.5b 3=2b; 0 is invalid */
#define UART_STOP_BIT_NUM_MASK  (3u << UART_STOP_BIT_NUM_SHIFT)
#define UART_STOP_BIT_NUM_1     (1u << UART_STOP_BIT_NUM_SHIFT)
#define UART_LOOPBACK           (1u << 12)
#define UART_TX_FLOW_EN         (1u << 13)
#define UART_RXFIFO_RST         (1u << 22)
#define UART_TXFIFO_RST         (1u << 23)

/* UART_CLKDIV_SYNC_REG (TRM Register 45.7): a 12-bit integer part and a
 * 4-bit fraction in sixteenths, i.e. the divisor is CLKDIV + FRAG/16. */
#define UART_CLKDIV_MASK        0xfffu
#define UART_CLKDIV_FRAG_SHIFT  20
#define UART_CLKDIV_FRAG_MASK   (0xfu << UART_CLKDIV_FRAG_SHIFT)

/* UART_CLK_CONF_REG (TRM Register 45.20). */
#define UART_TX_SCLK_EN         (1u << 24)
#define UART_RX_SCLK_EN         (1u << 25)
#define UART_TX_RST_CORE        (1u << 26)
#define UART_RX_RST_CORE        (1u << 27)

/* --- Clock and reset control (TRM chapter 8, HP_SYS_CLKRST) -------------
 *
 * 0x500E6000 = DR_REG_HPPERIPH1_BASE (0x500C0000) + 0x26000, TRM Table
 * 9.3-2. UART0's three knobs are spread across three registers, and the
 * split is not intuitive enough to guess at: the source select and the gate
 * are in PERI_CLK_CTRL110, while the *divider for the same UART* is in
 * PERI_CLK_CTRL111 alongside UART1's source select. Transcribed field by
 * field rather than reasoned about. */
#define HP_SYS_CLKRST_BASE      0x500E6000UL
#define SOC_CLK_CTRL1           (HP_SYS_CLKRST_BASE + 0x18)
#define SOC_CLK_CTRL2           (HP_SYS_CLKRST_BASE + 0x1c)
#define PERI_CLK_CTRL110        (HP_SYS_CLKRST_BASE + 0x68)
#define PERI_CLK_CTRL111        (HP_SYS_CLKRST_BASE + 0x6c)

/* SOC_CLK_CTRL1 bit 18 and SOC_CLK_CTRL2 bit 7. Both were read off the TRM's
 * own bit diagrams (Registers 11.7 and 11.8, pages 1108 and 1113) by
 * counting the field labels down from bit 31, then checked against IDF's
 * generated hp_sys_clkrst_reg.h -- which is the only reason they are right.
 * The first draft of this file had them at 24 and 25, reasoned from where
 * UART0 sits in the peripheral list, and they are nowhere near: the two
 * registers order their fields differently from each other, so there is no
 * position to infer. Exactly the failure section 3.2 of the phase plan
 * forbids. */
#define UART0_SYS_CLK_EN        (1u << 18)      /* SOC_CLK_CTRL1  */
#define UART0_APB_CLK_EN        (1u << 7)       /* SOC_CLK_CTRL2  */
#define UART0_CLK_SRC_SEL_SHIFT 24              /* PERI_CLK_CTRL110: 0=XTAL */
#define UART0_CLK_SRC_SEL_MASK  (3u << UART0_CLK_SRC_SEL_SHIFT)
#define UART0_CLK_SRC_XTAL      (0u << UART0_CLK_SRC_SEL_SHIFT)
#define UART0_CLK_EN            (1u << 26)      /* PERI_CLK_CTRL110 */
#define UART0_SCLK_DIV_NUM_MASK 0xffu           /* PERI_CLK_CTRL111, [7:0] */

/* --- IO_MUX (TRM chapter 10) --------------------------------------------
 *
 * 0x500E1000 = DR_REG_HPPERIPH1_BASE + 0x21000, and the per-pad register is
 * base + 0x4 + 4*n -- the same two facts tools/minimal_esp32p4.c already
 * used to toggle GPIO20 in E1, so they are confirmed on this silicon and not
 * only on paper.
 *
 * MCU_SEL selects which peripheral owns the pad. Function 0 on GPIO37 is
 * UART0_TXD and function 0 on GPIO38 is UART0_RXD (IDF's io_mux_reg.h:
 * FUNC_GPIO37_UART0_TXD_PAD 0, FUNC_GPIO38_UART0_RXD_PAD 0) -- a direct
 * IO_MUX route, not a GPIO-matrix one, so there is no signal index to
 * program and no GPIO_FUNCn_OUT_SEL_CFG write. Function 1 on both is plain
 * GPIO, which is what E1's toggle needed and this does not.
 *
 * MCU_SEL resets to 0, which is why the ROM console works before anyone has
 * configured anything; writing it anyway is the point of this file. */
#define IOMUX_BASE              0x500E1000UL
#define IOMUX_PAD(n)            (IOMUX_BASE + 0x4 + 4u * (n))
#define IOMUX_MCU_SEL_SHIFT     12
#define IOMUX_MCU_SEL_MASK      (7u << IOMUX_MCU_SEL_SHIFT)
#define IOMUX_FUN_UART0         (0u << IOMUX_MCU_SEL_SHIFT)
#define IOMUX_FUN_IE            (1u << 9)
#define IOMUX_FUN_PU            (1u << 8)

/* --- the register-update handshake --------------------------------------
 *
 * The registers named *_SYNC live in the UART core's clock domain rather
 * than the APB one, and a write to them does nothing until
 * UART_REG_UPDATE_REG bit 0 is written and self-clears. Forgetting it does
 * not fail loudly: the readback shows the value you wrote, and the hardware
 * keeps using the old one. That is a whole afternoon if it is not written
 * down, so it is a named function and every caller goes through it.
 *
 * Bounded, because this runs before the console exists and a hang here has
 * nothing to print with. The bound is arbitrary-but-generous rather than
 * calibrated -- the synchroniser takes a handful of core-clock cycles, and
 * at the worst plausible clock ratio that is far under 100000 spins. */
static void uart_commit(uintptr_t base) {
    REG(UART_REG_UPDATE(base)) = 1u;
    for (unsigned i = 0; i < 100000u && (REG(UART_REG_UPDATE(base)) & 1u); i++) {
        /* spin */
    }
}

/* --- raw hardware access ------------------------------------------------ */

static bool hw_uart_has_char(void) {
    if (!g_uart_base) return false;
    uint32_t st = REG(UART_STATUS(g_uart_base));
    return ((st >> UART_RXFIFO_CNT_SHIFT) & UART_RXFIFO_CNT_MASK) != 0;
}

static uint8_t hw_uart_getc(void) {
    /* The FIFO is a single address; reading it pops one byte. Only the low
     * 8 bits carry data (TRM Register 45.1). */
    return (uint8_t)(REG(UART_FIFO(g_uart_base)) & 0xffu);
}

/* Whether the TX FIFO has room, asked by counting rather than by trusting a
 * status flag.
 *
 * E1 left open whether a saturating writer could overrun this FIFO, having
 * seen a stream of 0x05 that decoded at no baud rate; the suspicion at the
 * time was that TXFIFO_CNT was being read wrongly. It was not being read at
 * all -- minimal_esp32p4.c gated on it, but that program's own bound was the
 * open question. The field is now confirmed against TRM Register 45.21
 * (bits [23:16]) and the depth against SOC_UART_FIFO_LEN (128), so this is a
 * real count against a real capacity and cannot silently overrun. */
static bool hw_uart_tx_room(void) {
    uint32_t st = REG(UART_STATUS(g_uart_base));
    uint32_t used = (st >> UART_TXFIFO_CNT_SHIFT) & UART_TXFIFO_CNT_MASK;
    return used < UART_FIFO_DEPTH;
}

/* The blocking primitives. Whoever calls these owns the hardware -- normally
 * the uart task, exclusively, once it is up.
 *
 * sched_yield() rather than a bare spin: it is a no-op when nothing else is
 * runnable (and before sched_init(), when there is no task table at all), so
 * the same code serves the boot path and the running system. Same structure
 * as drivers/uart_16550.c's polling fallback, minus the ISR fast path that
 * has no controller to attach to until E3. */
static void uart_hw_putc_blocking(char c) {
    if (!g_uart_base) return;
    while (!hw_uart_tx_room()) sched_yield();
    REG(UART_FIFO(g_uart_base)) = (uint8_t)c;
}

static uint8_t uart_hw_getc_blocking(void) {
    while (!hw_uart_has_char()) sched_yield();
    return hw_uart_getc();
}

/* --- the uart driver task (M4, plan/phase12_microkernel_migration.md) ----
 *
 * The same wire protocol drivers/uart_16550.c serves, byte for byte:
 *
 *   'H'      -> has-char query.  resp: 1 byte, 0 or 1.
 *   'R'      -> read, blocking.  resp: 1 byte, the char read.
 *   'W', ... -> write the req_len-1 bytes that follow. resp: empty.
 *
 * This is the third copy of this machinery in the tree (16550, RP2350, and
 * now here) and that is worth naming rather than leaving to be noticed. Two
 * copies were defensible: the RP2350 one carries a USB CDC mirror and a
 * demux bypass the QEMU one does not. A third makes the shared part large
 * enough to be worth factoring, but E2 is the wrong moment -- extracting it
 * now would change two drivers that work, on two boards, to make room for a
 * third that has never run. E3 adds this file's interrupt path, at which
 * point all three have the same shape and the comparison is real; the debt
 * is recorded there rather than paid here on speculation. */
#define UART_REQ_HASCHAR ((uint8_t)'H')
#define UART_REQ_READ    ((uint8_t)'R')
#define UART_REQ_WRITE   ((uint8_t)'W')

#define UART_TX_BATCH_CAP 256
#define UART_REQ_CAP (UART_TX_BATCH_CAP + 1)

static uint8_t          g_uart_req[UART_REQ_CAP];
static uint8_t          g_uart_resp[1];
static chan_endpoint_t *g_uart_ep;
static int              g_uart_task_pid = -1;
static uint32_t         g_uart_write_calls;
static volatile bool    g_uart_write_in_flight;

uint32_t uart_write_call_count(void) { return g_uart_write_calls; }

static bool uart_task_alive(void) {
    if (g_uart_task_pid < 0) return false;
    int st = sched_task_state(g_uart_task_pid);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

/* Must never printk() from inside this loop -- a caller can be blocked on
 * this very endpoint while holding printk_lock(), and taking that lock here
 * would deadlock against it. uart_debug_putc() exists for that case. */
static void uart_task_body(void *arg) {
    (void)arg;
    while (!g_uart_ep) sched_yield();

    for (;;) {
        uint32_t req_len = chan_serve_wait(g_uart_ep);
        if (req_len == 0) { chan_serve_reply(g_uart_ep, 0); continue; }
        switch (g_uart_req[0]) {
            case UART_REQ_HASCHAR:
                g_uart_resp[0] = hw_uart_has_char() ? 1 : 0;
                chan_serve_reply(g_uart_ep, 1);
                break;
            case UART_REQ_READ:
                g_uart_resp[0] = uart_hw_getc_blocking();
                chan_serve_reply(g_uart_ep, 1);
                break;
            case UART_REQ_WRITE:
                g_uart_write_calls++;
                g_uart_write_in_flight = true;
                for (uint32_t i = 1; i < req_len; i++) {
                    uart_hw_putc_blocking((char)g_uart_req[i]);
                }
                g_uart_write_in_flight = false;
                chan_serve_reply(g_uart_ep, 0);
                break;
            default:
                chan_serve_reply(g_uart_ep, 0);
                break;
        }
    }
}

int uart_task_start(void) {
    int pid = task_create_driver("uart", uart_task_body, NULL, 1);
    if (pid < 0) {
        printk("[UART] Could not start the uart task; console stays on direct hardware access.\n");
        return -1;
    }
    task_set_priority(pid, TASK_PRIO_INTERRUPT);
    if (chan_register_task("uart", pid, g_uart_req, sizeof(g_uart_req),
                           g_uart_resp, sizeof(g_uart_resp)) != 0) {
        printk("[UART] Could not register the uart channel endpoint; falling back to direct hardware access.\n");
        return -1;
    }
    g_uart_ep = chan_lookup("uart");
    g_uart_task_pid = pid;
    printk("[UART] Driver running as task #%d, reachable via chan_call(\"uart\", ...)\n", pid);
    return pid;
}

/* --- bring-up ----------------------------------------------------------- */

void uart_init(uintptr_t base_addr) {
    if (base_addr != 0) g_uart_base = base_addr;
    uintptr_t base = g_uart_base;

    /* 1. Bus and core clocks on.
     *
     * All three of these reset to enabled, and the ROM has certainly turned
     * them on already -- this is not fixing anything today. It is the part
     * that stops being redundant the first time this image is entered by
     * something other than the ROM's download loader. */
    REG(SOC_CLK_CTRL1) |= UART0_SYS_CLK_EN;
    REG(SOC_CLK_CTRL2) |= UART0_APB_CLK_EN;

    /* 2. Source clock: XTAL, undivided.
     *
     * SCLK_DIV_NUM is the pre-divider in front of the UART's own 12.4-bit
     * divisor, and it is written as (divisor - 1). Zero means divide by one,
     * which is what a 40 MHz source and a 115200 target want -- the whole
     * ratio fits in CLKDIV's 12 integer bits with room to spare
     * (40e6/115200 = 347, against a 4095 ceiling), so the pre-divider has
     * nothing to do. Written explicitly rather than assumed zero. */
    uint32_t c110 = REG(PERI_CLK_CTRL110);
    c110 &= ~UART0_CLK_SRC_SEL_MASK;
    REG(PERI_CLK_CTRL110) = c110 | UART0_CLK_SRC_XTAL | UART0_CLK_EN;
    REG(PERI_CLK_CTRL111) &= ~UART0_SCLK_DIV_NUM_MASK;

    REG(UART_CLK_CONF(base)) |= UART_TX_SCLK_EN | UART_RX_SCLK_EN;

    /* 3. Baud rate.
     *
     * The divisor is CLKDIV + FRAG/16, so compute it in sixteenths and split
     * it. 40 MHz / 115200 = 347.222..., i.e. 5555.55 sixteenths -> 5555 ->
     * CLKDIV 347, FRAG 3. That divisor gives 115209.5 baud, 0.008% fast.
     *
     * Integer arithmetic throughout: this file has no float, the ABI is
     * soft-float, and a rounding constant would be one more number to
     * justify. Truncation costs at most 1/16 of a divisor step, which at
     * these rates is under 0.05%.
     *
     * If CONFIG_UART0_BAUD were ever set so low that CLKDIV overflowed its
     * 12 bits, the mask below would silently produce a wildly wrong rate --
     * so the case is a compile-time error instead. At 40 MHz the floor is
     * 9773 baud, which is under every rate this project uses. */
#if (CONFIG_UART0_SCLK_HZ / CONFIG_UART0_BAUD) > 4095
#error "CONFIG_UART0_BAUD is too low for a 12-bit UART_CLKDIV at CONFIG_UART0_SCLK_HZ -- the pre-divider in PERI_CLK_CTRL111 would have to be used"
#endif
    uint32_t sixteenths = ((uint32_t)CONFIG_UART0_SCLK_HZ * 16u) / (uint32_t)CONFIG_UART0_BAUD;
    uint32_t clkdiv = (sixteenths >> 4) & UART_CLKDIV_MASK;
    uint32_t frag   = sixteenths & 0xfu;
    REG(UART_CLKDIV_SYNC(base)) = clkdiv | (frag << UART_CLKDIV_FRAG_SHIFT);

    /* 4. Frame format: 8N1, no flow control, no loopback.
     *
     * Read-modify-write rather than a whole-register store: CONF0 also holds
     * MEM_CLK_EN and ERR_WR_MASK, whose reset values are correct and which
     * this file has no opinion about. Zeroing the register would clear them
     * along with everything else. */
    uint32_t conf0 = REG(UART_CONF0_SYNC(base));
    conf0 &= ~(UART_BIT_NUM_MASK | UART_STOP_BIT_NUM_MASK | UART_PARITY_EN |
               UART_TX_FLOW_EN | UART_LOOPBACK);
    conf0 |= UART_BIT_NUM_8 | UART_STOP_BIT_NUM_1;
    REG(UART_CONF0_SYNC(base)) = conf0;

    uart_commit(base);

    /* 5. Empty both FIFOs.
     *
     * After the format and rate are committed, not before: anything already
     * queued was framed by the ROM's settings, and on the RX side it is
     * whatever the host said while esptool still owned the port -- E1
     * recorded that merely *opening* the host port puts a stray byte in the
     * RX FIFO, and that stray byte is what made an earlier heartbeat gate
     * useless. Starting from empty is how the shell's first prompt is not
     * answered by a character nobody typed.
     *
     * Both reset bits are level, not pulses: written, then written back to
     * zero. Leaving RXFIFO_RST asserted holds the FIFO empty forever, which
     * presents as a keyboard that does nothing. */
    conf0 = REG(UART_CONF0_SYNC(base));
    REG(UART_CONF0_SYNC(base)) = conf0 | UART_RXFIFO_RST | UART_TXFIFO_RST;
    uart_commit(base);
    REG(UART_CONF0_SYNC(base)) = conf0 & ~(UART_RXFIFO_RST | UART_TXFIFO_RST);
    uart_commit(base);

    /* 6. The pads, last.
     *
     * Last on purpose: until MCU_SEL is written the pad still carries
     * whatever the ROM left driving it, and doing this first would put a
     * half-configured UART on the wire for the duration of the steps above.
     *
     * TX needs only the function select. RX needs FUN_IE, or the input
     * buffer stays off and the pin reads as permanently idle -- and a weak
     * pull-up, so an unplugged console line idles high (a UART's idle level)
     * rather than floating and framing noise into the RX FIFO. */
    uint32_t pad = REG(IOMUX_PAD(CONFIG_UART0_TX_GPIO));
    pad &= ~IOMUX_MCU_SEL_MASK;
    REG(IOMUX_PAD(CONFIG_UART0_TX_GPIO)) = pad | IOMUX_FUN_UART0;

    pad = REG(IOMUX_PAD(CONFIG_UART0_RX_GPIO));
    pad &= ~IOMUX_MCU_SEL_MASK;
    REG(IOMUX_PAD(CONFIG_UART0_RX_GPIO)) = pad | IOMUX_FUN_UART0 |
                                           IOMUX_FUN_IE | IOMUX_FUN_PU;

    /* The A3b demux (drivers/uart_net.c) gets the raw accessors, same as on
     * every other target: when `p9share` is on, some of what is waiting in
     * the FIFO is 9P frame bytes and the console may not read the register
     * directly. */
    uart_demux_init(hw_uart_has_char, hw_uart_getc);

    /* No devirq_attach()/arch_irq_enable() here, and no mie bit. E3 is where
     * this file learns about the CLIC; see the header comment. */
}

/* --- the facade (identical in shape to drivers/uart_16550.c's) ----------- */

static char       g_tx_batch[MAX_HARTS][UART_TX_BATCH_CAP];
static uint32_t   g_tx_batch_len[MAX_HARTS];
static spinlock_t g_tx_batch_lock;

static int uart_call_with_retry(const uint8_t *req, uint32_t req_len,
                                uint8_t *resp, uint32_t resp_max) {
    for (int attempt = 0; attempt < 8; attempt++) {
        int n = chan_call(g_uart_ep, req, req_len, resp, resp_max);
        if (n >= 0) return n;
        sched_yield();
    }
    return -1;
}

void uart_flush(void) {
    char local[UART_TX_BATCH_CAP];
    uintptr_t flags = spin_lock_irqsave(&g_tx_batch_lock);
    unsigned h = hart_id();
    uint32_t len = g_tx_batch_len[h];
    if (len > 0) {
        memcpy(local, g_tx_batch[h], len);
        g_tx_batch_len[h] = 0;
    }
    spin_unlock_irqrestore(&g_tx_batch_lock, flags);
    if (len == 0) return;

    if (uart_demux_is_enabled() || !uart_task_alive()) {
        for (uint32_t i = 0; i < len; i++) uart_hw_putc_blocking(local[i]);
        return;
    }
    uint8_t req[1 + UART_TX_BATCH_CAP];
    req[0] = UART_REQ_WRITE;
    memcpy(&req[1], local, len);
    uint8_t resp[1];
    /* Never fall back to direct hardware access while another WRITE is
     * actually in flight -- that is what produced byte-level interleaved
     * console garbage on RP2350, and this board's UART is real hardware with
     * real transmit time, so the same race exists here. A pending READ can
     * run for as long as a human takes to press a key and must not be waited
     * out; g_uart_write_in_flight is what tells the two apart. */
    for (;;) {
        int n = chan_call(g_uart_ep, req, 1 + len, resp, sizeof(resp));
        if (n >= 0) return;
        if (!g_uart_write_in_flight) break;
        sched_yield();
    }
    for (uint32_t i = 0; i < len; i++) uart_hw_putc_blocking(local[i]);
}

void uart_putc(char c) {
    uintptr_t flags = spin_lock_irqsave(&g_tx_batch_lock);
    while (g_tx_batch_len[hart_id()] >= UART_TX_BATCH_CAP) {
        spin_unlock_irqrestore(&g_tx_batch_lock, flags);
        uart_flush();
        flags = spin_lock_irqsave(&g_tx_batch_lock);
    }
    g_tx_batch[hart_id()][g_tx_batch_len[hart_id()]++] = c;
    spin_unlock_irqrestore(&g_tx_batch_lock, flags);
}

/* A plain UART FIFO cannot be inspected without consuming, so there is
 * nothing to peek and kernel/console.c's pump keeps the behaviour it has on
 * QEMU. Same answer, same reason, as drivers/uart_16550.c. */
bool uart_peek_interrupt(void) {
    return false;
}

bool uart_has_char(void) {
    uart_flush();
    if (uart_demux_is_enabled()) return uart_demux_console_has_char();
    if (uart_task_alive()) {
        uint8_t req[1] = { UART_REQ_HASCHAR };
        uint8_t resp[1];
        if (uart_call_with_retry(req, 1, resp, 1) == 1) return resp[0] != 0;
    }
    return hw_uart_has_char();
}

char uart_getc(void) {
    uart_flush();
    if (uart_demux_is_enabled()) {
        while (!uart_demux_console_has_char()) sched_yield();
        return uart_demux_console_getc();
    }
    if (uart_task_alive()) {
        uint8_t req[1] = { UART_REQ_READ };
        uint8_t resp[1];
        if (uart_call_with_retry(req, 1, resp, 1) == 1) return (char)resp[0];
    }
    return (char)uart_hw_getc_blocking();
}

void uart_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

/* Low-level driver diagnostics only: never routed through the task or the
 * batch above, so tracing that might be used to debug either mechanism does
 * not depend on them. */
void uart_debug_putc(char c) {
    uart_hw_putc_blocking(c);
}

void uart_debug_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_debug_putc('\r');
        uart_debug_putc(*s++);
    }
}
