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
 * ## Interrupt-driven since E3, with the polling still underneath
 *
 * E2 shipped this file entirely polled, because there was no route from a
 * peripheral to a handler until the CLIC came up. E3 built that route, and
 * this file is its first user: RX and TX both block on an interrupt now, and
 * the sched_yield() spins survive as the fallback for the cases that cannot
 * block -- before sched_init(), or when the single waiter slot in a
 * direction is already taken. Same two-path shape as
 * drivers/uart_16550.c, which is now literally true rather than aspirational.
 *
 * The consequence to know about: there is still no preemption timer (E4), so
 * a task that blocks on a keystroke really does sleep until the key arrives
 * -- but nothing periodically re-examines a task that got stuck some other
 * way. Interrupts fixed the console's idle behaviour, not the scheduler's.
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
#include "drivers/uart_proto.h"
#include "drivers/driver_task.h"
#include "drivers/uart_net.h"
#include "kernel/devirq.h"
#include "kernel/irq.h"
#include "kernel/sched.h"
#include "kernel/hart.h"
#include "kernel/lock.h"
#include "kernel/chan.h"
#include "kernel/printk.h"
#include "arch/trap.h"
#include "arch/esp32p4_intr.h"
#include "lugalos_config.h"
#include <string.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* --- UART0 registers (TRM chapter 45, Register Summary) ----------------- */

static uintptr_t g_uart_base = CONFIG_UART0_BASE;

#define UART_FIFO(base)         ((base) + 0x00)  /* RO/WO FIFO data port     */
#define UART_INT_RAW(base)      ((base) + 0x04)  /* raw interrupt status     */
#define UART_INT_ST(base)       ((base) + 0x08)  /* masked interrupt status  */
#define UART_INT_ENA(base)      ((base) + 0x0c)  /* interrupt enable         */
#define UART_INT_CLR(base)      ((base) + 0x10)  /* write 1 to clear         */
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

/* The interrupt bits, identical across INT_RAW / INT_ST / INT_ENA / INT_CLR
 * (TRM Registers 45.2-45.5). Only three of the twenty are used here.
 *
 * INT_ST is INT_RAW masked by INT_ENA, which is the property the ISR relies
 * on: clearing a bit in INT_ENA takes the line down at the source even while
 * the underlying condition persists. Since RXFIFO_FULL and TXFIFO_EMPTY are
 * both *conditions* rather than events -- they re-assert as soon as they are
 * cleared, for as long as the FIFO stays that way -- INT_ENA is the only
 * thing standing between an enabled console interrupt and an interrupt
 * storm. Every path below therefore disables before it clears, never the
 * other way round.
 *
 * RXFIFO_TOUT is enabled alongside RXFIFO_FULL and not instead of it. The
 * TRM says only "Configures the threshold for RX FIFO being full"; IDF's
 * generated header is the one that says "when receiver receives more data
 * than this register value", i.e. strictly greater, which would make a
 * threshold of 1 need two bytes to fire and lose the first keystroke of
 * every line. The timeout closes that: it fires when the receiver has been
 * idle for RX_TOUT_THRHD byte-times with anything at all in the FIFO. IDF's
 * own driver enables the pair together for the same reason
 * (esp_driver_uart/src/uart.c), which is the corroboration for reading the
 * threshold as exclusive rather than assuming the friendlier one. */
#define UART_RXFIFO_FULL_INT    (1u << 0)
#define UART_TXFIFO_EMPTY_INT   (1u << 1)
#define UART_RXFIFO_TOUT_INT    (1u << 8)
#define UART_RX_INTS            (UART_RXFIFO_FULL_INT | UART_RXFIFO_TOUT_INT)

/* UART_CONF1_REG (TRM Register 45.10). Not a _SYNC register -- it takes
 * effect without the UART_REG_UPDATE handshake, which IDF's uart_ll.h
 * confirms by writing it directly. */
#define UART_RXFIFO_FULL_THRHD_SHIFT   0
#define UART_RXFIFO_FULL_THRHD_MASK    (0xffu << UART_RXFIFO_FULL_THRHD_SHIFT)
#define UART_TXFIFO_EMPTY_THRHD_SHIFT  8
#define UART_TXFIFO_EMPTY_THRHD_MASK   (0xffu << UART_TXFIFO_EMPTY_THRHD_SHIFT)

/* UART_TOUT_CONF_SYNC_REG (TRM Register 45.17) -- this one *is* _SYNC and
 * needs the commit. RX_TOUT_THRHD is in byte-times, [11:2]. */
#define UART_TOUT_CONF_SYNC(base) ((base) + 0x64)
#define UART_RX_TOUT_EN         (1u << 0)
#define UART_RX_TOUT_THRHD_SHIFT 2
#define UART_RX_TOUT_THRHD_MASK  (0x3ffu << UART_RX_TOUT_THRHD_SHIFT)

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

/* --- the ISR, E3 --------------------------------------------------------
 *
 * One waiter slot per direction, held by whichever task is about to block,
 * and the corresponding INT_ENA bit is set only for the duration of that
 * wait. Structurally the same as drivers/uart_16550.c's, and for the same
 * reason: the conditions are level, so a source left enabled with nobody
 * waiting on it fires on every return from interrupt for as long as the
 * condition holds. A second concurrent waiter in the same direction falls
 * back to polling rather than being queued -- the single-slot, busy-refuses
 * shape chan_call() itself uses.
 *
 * A note on what this is *not*: the ISR does not touch the FIFO. It wakes
 * the task that was waiting and lets that task do the read or the write.
 * Draining the FIFO here would put console bytes somewhere the demux
 * (drivers/uart_net.c) cannot see them. */
static volatile int g_rx_waiter = -1;
static volatile int g_tx_waiter = -1;
static volatile uint32_t g_uart_irq_count;    /* see uart_irq_count()    */
static volatile uint32_t g_uart_irq_rx_wakes; /* see uart_irq_rx_wakes() */
static volatile uint32_t g_uart_irq_tx_arms;  /* see uart_irq_tx_arms()  */
static volatile uint32_t g_uart_irq_tx_wakes; /* see uart_irq_tx_wakes() */
static volatile uint32_t g_uart_irq_tx_seen;  /* see uart_irq_tx_seen()  */

static void uart_isr(void *ctx) {
    (void)ctx;
    g_uart_irq_count++;
    uintptr_t base = g_uart_base;
    uint32_t st = REG(UART_INT_ST(base));

    /* Everything that fired gets disabled, whether or not there was a waiter
     * for it. That is deliberate belt-and-braces: only a task about to block
     * ever re-enables a bit, so masking unconditionally here means no
     * sequence of events -- a race with the fast path, a spurious source
     * sharing this line, an ISR that ran twice -- can leave a level
     * condition enabled with nothing to serve it. The cost of being wrong in
     * this direction is one interrupt not taken; the cost of being wrong in
     * the other is a board that only prints. */
    uint32_t served = st & (UART_RX_INTS | UART_TXFIFO_EMPTY_INT);
    if (served) REG(UART_INT_ENA(base)) &= ~served;

    if ((st & UART_RX_INTS) && g_rx_waiter >= 0) {
        g_uart_irq_rx_wakes++;
        int pid = g_rx_waiter;
        g_rx_waiter = -1;
        task_unblock(pid);
    }
    if (st & UART_TXFIFO_EMPTY_INT) g_uart_irq_tx_seen++;
    if ((st & UART_TXFIFO_EMPTY_INT) && g_tx_waiter >= 0) {
        g_uart_irq_tx_wakes++;
        int pid = g_tx_waiter;
        g_tx_waiter = -1;
        task_unblock(pid);
    }

    /* Acknowledge last. This is also what takes the CLIC's pending bit down:
     * the line is configured level-triggered (arch/riscv/common/trap.c), so
     * clicintip follows the peripheral and "cleared from source" means
     * exactly this store. */
    REG(UART_INT_CLR(base)) = st;
}

/* The blocking primitives. Whoever calls these owns the hardware -- normally
 * the uart task, exclusively, once it is up.
 *
 * Two paths, in this order: block on the interrupt if there is a task to
 * block and the slot is free, otherwise spin with sched_yield(). The spin is
 * not a leftover -- it is the only thing that works before sched_init(), and
 * every line of boot output before the uart task exists goes through it. */
static void uart_hw_putc_blocking_ex(char c, bool may_block) {
    if (!g_uart_base) return;
    if (hw_uart_tx_room()) {
        REG(UART_FIFO(g_uart_base)) = (uint8_t)c;
        return;
    }
    /* Nothing between the fast-path miss and task_block() may restore
     * interrupts early: a TX interrupt landing in that gap would find this
     * task still RUNNING rather than BLOCKED, task_unblock() would no-op,
     * and the ISR would have "served" a wakeup nobody was asleep for -- a
     * silent, permanent lost wakeup. So the whole decision runs under one
     * irq_save(). Copied in shape from drivers/uart_16550.c, where the same
     * comment is the record of having got it wrong first. */
    /* The caller decides whether blocking is even allowed here.
     *
     * uart_flush() reaches this function by two very different routes. Through
     * the uart *task*, blocking is right: the task exists to sleep until the
     * FIFO drains. Through the **fallback** -- taken when the endpoint is busy
     * or the task is not running -- blocking is wrong, and not subtly so. That
     * path exists precisely because the task machinery is unavailable, and
     * task_block()ing there re-introduces the dependency the fallback was
     * written to escape: the caller sleeps waiting for a TX interrupt while
     * holding whatever it was in the middle of, and on this board that is a
     * printk() that never returns.
     *
     * Kept on its own merits, and the record of how it was arrived at is worth
     * preserving because the reasoning was right and the diagnosis was wrong.
     * E7 saw a boot that stopped mid-console-line once E6's /flash0 added
     * output after the shell starts reading the console, and this looked like
     * the cause: the shell's pending blocking read occupies the single-slot
     * uart endpoint, every later printk falls back to direct hardware, and the
     * first one to find a full FIFO would block for good. The TX interrupt was
     * blamed for never arriving.
     *
     * It arrives. uart_irq_tx_arms() and uart_irq_tx_wakes() are equal on this
     * board, and enabling the source with a drained FIFO delivers an interrupt
     * immediately -- both measured. The hang was the L2-cache memory
     * corruption (esp32p4_l2_cache_shrink(), arch/riscv/common/trap.c) eating
     * task context frames, and it presented as a lost wakeup because the task
     * that should have been woken no longer had a frame to return to.
     *
     * The split stays regardless: a fallback whose whole reason for existing is
     * that the task machinery is unavailable must not call task_block(). That
     * argument never depended on the bug. */
    uintptr_t flags = irq_save();
    /* Acknowledge before re-testing, not after. Both orderings look right;
     * only this one is. INT_CLR takes down a raw bit that may be left over
     * from an earlier condition -- without it the enable below would deliver
     * an immediate stale interrupt and this function would return having
     * written to a full FIFO. But clearing it *after* the test would instead
     * discard the notification for a condition that became true in between,
     * and the task would sleep on an event that had already happened. Clear
     * first, test second: hardware re-asserts the raw bit for as long as the
     * condition holds, so anything that arrives from here on is either seen
     * by the test or delivered by the enable. */
    REG(UART_INT_CLR(g_uart_base)) = UART_TXFIFO_EMPTY_INT;
    if (!hw_uart_tx_room()) {
        if (may_block && g_tx_waiter < 0 && sched_has_task()) {
            g_tx_waiter = sched_current_pid();
            g_uart_irq_tx_arms++;
            REG(UART_INT_ENA(g_uart_base)) |= UART_TXFIFO_EMPTY_INT;
            task_block();
        } else {
            irq_restore(flags);
            while (!hw_uart_tx_room()) sched_yield();
            flags = irq_save();
        }
    }
    irq_restore(flags);
    REG(UART_FIFO(g_uart_base)) = (uint8_t)c;
}

/* The two entry points, so the choice is made by name at each call site
 * rather than by a flag someone has to remember to pass. */
static void uart_hw_putc_blocking(char c) { uart_hw_putc_blocking_ex(c, true); }
static void uart_hw_putc_polling(char c)  { uart_hw_putc_blocking_ex(c, false); }

static uint8_t uart_hw_getc_blocking(void) {
    if (hw_uart_has_char()) return hw_uart_getc();
    uintptr_t flags = irq_save();
    /* Clear first, test second -- see uart_hw_putc_blocking() above for why
     * the other order loses a keystroke rather than merely being untidy. */
    REG(UART_INT_CLR(g_uart_base)) = UART_RX_INTS;
    if (!hw_uart_has_char()) {
        if (g_rx_waiter < 0 && sched_has_task()) {
            g_rx_waiter = sched_current_pid();
            REG(UART_INT_ENA(g_uart_base)) |= UART_RX_INTS;
            task_block();
        } else {
            irq_restore(flags);
            while (!hw_uart_has_char()) sched_yield();
            flags = irq_save();
        }
    }
    irq_restore(flags);
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

#define UART_TX_BATCH_CAP 256
#define UART_REQ_CAP (UART_TX_BATCH_CAP + 1)

static uint8_t          g_uart_req[UART_REQ_CAP];
static uint8_t          g_uart_resp[1];
static driver_task_t g_uart_task;
static uint32_t         g_uart_write_calls;
static volatile bool    g_uart_write_in_flight;

uint32_t uart_write_call_count(void) { return g_uart_write_calls; }
uint32_t uart_irq_count(void) { return g_uart_irq_count; }
uint32_t uart_irq_rx_wakes(void) { return g_uart_irq_rx_wakes; }
uint32_t uart_irq_tx_arms(void) { return g_uart_irq_tx_arms; }
uint32_t uart_irq_tx_wakes(void) { return g_uart_irq_tx_wakes; }
uint32_t uart_irq_tx_seen(void) { return g_uart_irq_tx_seen; }


static bool uart_task_alive(void) { return driver_task_alive(&g_uart_task); }

/* printk() from inside this loop is fine; cprintf()/printk_debug() are not.
 *
 * The rule used to cover printk() too, because printk() reached the console
 * through this very endpoint: a caller blocked here while holding the output
 * lock, and this task taking the same lock to log, deadlocks against itself.
 * Y5c (plan/phase31_concurrency_hierarchy.md) made printk() a ring append
 * that reaches no blocking primitive, so that half is gone. The console
 * stream still ends at the wire, so the other half stands, and
 * drivers/driver_task.c's bracket plus console_lock() check it rather than
 * leaving it to this comment. uart_debug_putc() remains the escape hatch. */
static uint32_t uart_serve(void *ctx, const uint8_t *req, uint32_t req_len,
                          uint8_t *resp, uint32_t resp_cap) {
    (void)ctx; (void)resp_cap;
    switch (req[0]) {
        case UART_REQ_HASCHAR:
            resp[0] = hw_uart_has_char() ? 1 : 0;
            return 1;
        case UART_REQ_READ:
            /* Blocking, and it may be: this server runs in kernel mode.
             * drivers/uart_proto.h has the contrast with the RP2350's U-mode
             * server, which cannot block and so answers a different 'R'. */
            resp[0] = uart_hw_getc_blocking();
            return 1;
        case UART_REQ_WRITE:
            g_uart_write_calls++;
            g_uart_write_in_flight = true;
            for (uint32_t i = 1; i < req_len; i++) {
                uart_hw_putc_blocking((char)req[i]);
            }
            g_uart_write_in_flight = false;
            return 0;
        default:
            return 0;
    }
}

int uart_task_start(void) {
    /* Unmask the console's interrupt line here rather than in uart_init(),
     * for the ordering reason that function's step 8 sets out: trap_init()
     * masks every CLIC line as its first act, and it runs in between. From
     * this point a task blocking on the console really sleeps. */
    arch_irq_enable(ESP32P4_CLIC_IRQ_UART0);

    /* G4, plan/phase30_driver_framework.md: the same spec uart_16550.c uses,
     * because these two are the same driver in every respect the framework
     * cares about -- a kernel-mode task, serving the same three opcodes, at
     * the same priority, for the same reason. */
    const driver_task_spec_t spec = {
        .name        = "uart",
        .serve       = uart_serve,
        .ctx         = NULL,
        .req         = g_uart_req,  .req_cap  = sizeof(g_uart_req),
        .resp        = g_uart_resp, .resp_cap = sizeof(g_uart_resp),
        .min_req_len = 1,
        .stack_pages = 1,
        .priority    = TASK_PRIO_INTERRUPT,
    };
    return driver_task_start(&g_uart_task, &spec);
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

    /* 7. Interrupts (E3).
     *
     * Everything masked and acknowledged first. INT_ENA has no reset value
     * this code can rely on -- the ROM's own download loader uses this UART
     * -- and TXFIFO_EMPTY_INT_RAW is *set at reset* (TRM Register 45.2's
     * reset row has bit 1 high, because an empty TX FIFO is what the
     * condition means), so an enable without a preceding clear would take an
     * interrupt for a condition that was true before the kernel existed. */
    REG(UART_INT_ENA(base)) = 0;
    REG(UART_INT_CLR(base)) = 0xffffffffu;

    /* Thresholds. RX: 1, the lowest useful value -- one byte in the FIFO
     * should wake a reader, and the timeout below covers the reading of that
     * threshold as exclusive. TX: 64, half the 128-byte FIFO, so a writer
     * that filled the FIFO is woken with room for a useful batch rather than
     * for a single byte. */
    uint32_t conf1 = REG(UART_CONF1(base));
    conf1 &= ~(UART_RXFIFO_FULL_THRHD_MASK | UART_TXFIFO_EMPTY_THRHD_MASK);
    conf1 |= (1u << UART_RXFIFO_FULL_THRHD_SHIFT) |
             (64u << UART_TXFIFO_EMPTY_THRHD_SHIFT);
    REG(UART_CONF1(base)) = conf1;

    /* RX idle timeout: 2 byte-times. Short enough that a keystroke is not
     * perceptibly delayed (at 115200 a byte-time is 87 us), long enough not
     * to fire in the middle of a paste. */
    uint32_t tout = REG(UART_TOUT_CONF_SYNC(base));
    tout &= ~UART_RX_TOUT_THRHD_MASK;
    tout |= (2u << UART_RX_TOUT_THRHD_SHIFT) | UART_RX_TOUT_EN;
    REG(UART_TOUT_CONF_SYNC(base)) = tout;
    uart_commit(base);

    /* The A3b demux (drivers/uart_net.c) gets the raw accessors, same as on
     * every other target: when `p9share` is on, some of what is waiting in
     * the FIFO is 9P frame bytes and the console may not read the register
     * directly. */
    uart_demux_init(hw_uart_has_char, hw_uart_getc);

    /* 8. Route the peripheral to a CPU interrupt line, and attach the
     * handler to it.
     *
     * UART0's interrupt signal is source 31 in the interrupt matrix (TRM
     * Table 13.4-1 -- and its mapping register's offset 0x7C is 4*31, which
     * is the same fact stated twice). Nothing reaches the CPU until it has
     * been routed to one of the 32 lines this core has. *Which* line is an
     * allocation rather than a hardware fact, so it is written down in
     * arch/esp32p4_intr.h, where a second driver picking the same number
     * would be a visible edit instead of a coincidence.
     *
     * Handler before enable, as arch/trap.h asks. The enable itself is not
     * here: main.c calls uart_init() long before trap_init(), and
     * trap_init()'s CLIC bring-up begins by masking every line -- including
     * one enabled from here, which would then never be re-enabled. So
     * arch_irq_enable() lives in uart_task_start() instead, which runs after
     * both. Nothing is lost by waiting: until sched_init() there is no task
     * for the ISR to wake, and every blocking primitive above already falls
     * back to polling in exactly that case. */
    if (esp32p4_intmtx_route(31u, ESP32P4_CLIC_IRQ_UART0) == 0) {
        devirq_attach(ESP32P4_CLIC_IRQ_UART0, uart_isr, NULL);
    }
}

/* --- the facade (identical in shape to drivers/uart_16550.c's) ----------- */

static char       g_tx_batch[MAX_HARTS][UART_TX_BATCH_CAP];
static uint32_t   g_tx_batch_len[MAX_HARTS];
static spinlock_t g_tx_batch_lock;

/* The bounded retry is driver_task_call()'s (G2), shared with every other
 * driver task. uart_flush() below still does not use it -- see its own
 * comment for the one policy that is this driver's rather than the
 * framework's. */

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
        for (uint32_t i = 0; i < len; i++) uart_hw_putc_polling(local[i]);
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
        int n = chan_call(driver_task_endpoint(&g_uart_task), req, 1 + len, resp, sizeof(resp));
        if (n >= 0) return;
        if (!g_uart_write_in_flight) break;
        sched_yield();
    }
    for (uint32_t i = 0; i < len; i++) uart_hw_putc_polling(local[i]);
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
        if (driver_task_call(&g_uart_task, req, 1, resp, 1) == 1) return resp[0] != 0;
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
        if (driver_task_call(&g_uart_task, req, 1, resp, 1) == 1) return (char)resp[0];
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
/* See drivers/uart.h. Bounded spin on TX FIFO room, then drop. No yield, no
 * block -- in particular this never touches g_tx_waiter or INT_ENA, so it is
 * safe to call from inside the ISR that owns them. */
void uart_critical_putc(char c) {
    if (!g_uart_base) return;
    for (unsigned i = 0; i < 200000u; i++) {
        if (hw_uart_tx_room()) {
            REG(UART_FIFO(g_uart_base)) = (uint8_t)c;
            return;
        }
    }
}

void uart_critical_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_critical_putc('\r');
        uart_critical_putc(*s++);
    }
}

/* See drivers/uart.h. Same batch as uart_flush(), written the bounded way. */
void uart_flush_critical(void) {
    char local[UART_TX_BATCH_CAP];
    uintptr_t flags = spin_lock_irqsave(&g_tx_batch_lock);
    unsigned h = hart_id();
    uint32_t len = g_tx_batch_len[h];
    if (len > 0) {
        memcpy(local, g_tx_batch[h], len);
        g_tx_batch_len[h] = 0;
    }
    spin_unlock_irqrestore(&g_tx_batch_lock, flags);
    for (uint32_t i = 0; i < len; i++) uart_critical_putc(local[i]);
}

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
