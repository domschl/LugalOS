#include "drivers/i2c_rtc.h"
#include "drivers/at24c32.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "kernel/sched.h"
#include "kernel/chan.h"
#include "kernel/mem_domain.h"
#include "kernel/device.h"
#include "kernel/ipc.h"
#include "kernel/palloc.h"
#include "arch/umode.h"
#include "drivers/uart.h"
#include "lugalos_config.h"
#include <string.h>

#define DS1307_DS3231_I2C_ADDR 0x68

static bool g_rtc_detected = false;

#if defined(CONFIG_BOARD_RP2350)
/* Board-fact-driven (L3, plan/phase11_pico_clock_green.md) rather than the
 * GP4/GP5/I2C0 literals this file hardcoded before: the Pico-Clock-Green
 * baseboard's DS3231 is wired to GP6/GP7, which RP2350's GPIO-to-
 * controller mapping (alternates every 4 pins) puts on the I2C1
 * peripheral instance, not I2C0 -- a different base address, not just
 * different pins. cmake/board-rp2350.cmake keeps the original GP4/GP5/
 * I2C0 values (CONFIG_I2C_RTC_BASE == I2C0's 0x40090000); cmake/board-
 * rp2350-clock.cmake sets GP6/GP7/I2C1 (0x40098000) instead. */
#define I2C_SDA_PIN CONFIG_I2C_RTC_SDA_GPIO
#define I2C_SCL_PIN CONFIG_I2C_RTC_SCL_GPIO

#define RESETS_BASE            0x40020000UL
#define RESETS_RESET           (RESETS_BASE + 0x0000)
#define RESETS_RESET_SET       (RESETS_BASE + 0x2000) // Atomic Bit SET Alias
#define RESETS_RESET_CLR       (RESETS_BASE + 0x3000) // Atomic Bit CLR Alias
#define RESETS_RESET_DONE      (RESETS_BASE + 0x0008) // Reset Done Register --
    // was 0x000C (off by one register); verified against
    // ~/gith/pico/pico-sdk/src/rp2350/hardware_regs/include/hardware/regs/
    // resets.h's RESETS_RESET_DONE_OFFSET while researching L2's own ADC
    // reset sequence. Likely dormant rather than actually broken: the
    // 10000-iteration poll below still burns enough real time for the
    // peripheral's (near-instant) unreset to finish underneath it
    // regardless of which address it polled, so this was never observed
    // to misbehave -- but it's the wrong register, so fixed outright now
    // that it's been found, not left in place because it happened to work.

/* RESETS_RESET_I2C0 is bit 4, RESETS_RESET_I2C1 is bit 5 (resets.h) --
 * derived from CONFIG_I2C_RTC_BASE rather than added as its own board
 * fact, so a board file can't get this one wrong independently of the
 * base address it already has to get right. */
#if CONFIG_I2C_RTC_BASE == 0x40098000UL
#define I2C_RTC_RESET_BIT (1u << 5) // RESETS_RESET_I2C1
#else
#define I2C_RTC_RESET_BIT (1u << 4) // RESETS_RESET_I2C0
#endif

/* M5 Phase 3, plan/phase12_microkernel_migration.md: RP2350's Secure/
 * Non-secure split -- the same mechanism found for GPIO in M5 Phase 1
 * (drivers/uart_rp2350.c's ACCESSCTRL_GPIO_NSMASK0 comment has the full
 * datasheet citation, not repeated here) -- also gates I2C0/I2C1, but
 * through a differently-shaped register: one register per peripheral
 * (not one bit per GPIO), with SP/SU/NSP/NSU bits (Secure/Non-secure x
 * Privileged/Unprivileged). Checked directly against
 * ~/gith/pico/pico-sdk's accessctrl.h: reset value 0xfc, i.e. Secure
 * access enabled (SP=1) and Non-secure access disabled (NSP=NSU=0) by
 * default. U-mode is Non-secure+Unprivileged -- NSU -- and that header's
 * own comment notes NSU "is writable... if and only if NSP is set", so
 * both bits need setting together, from M-mode, before the task exists
 * (i2c_hw_init() below). Same conditional shape as I2C_RTC_RESET_BIT
 * above, for the same reason. */
#define ACCESSCTRL_BASE 0x40060000UL
#if CONFIG_I2C_RTC_BASE == 0x40098000UL
#define ACCESSCTRL_I2C_RTC (ACCESSCTRL_BASE + 0x88) // ACCESSCTRL_I2C1
#else
#define ACCESSCTRL_I2C_RTC (ACCESSCTRL_BASE + 0x84) // ACCESSCTRL_I2C0
#endif
#define ACCESSCTRL_I2C_NSP (1u << 1)
#define ACCESSCTRL_I2C_NSU (1u << 0)

/* Found the hard way, as a boot-time bus fault, immediately after adding
 * the write below without it: every ACCESSCTRL register *except*
 * GPIO_NSMASK0/1 (the two heartbeat's own fix, drivers/uart_rp2350.c,
 * happened to use) requires the 16-bit value 0xacce present in the
 * write's upper 16 bits, or the write both fails *and* raises a bus
 * fault rather than silently doing nothing -- straight from the
 * datasheet's own ACCESSCTRL overview section, not something either of
 * this tree's two prior ACCESSCTRL fixes (GPIO_NSMASK0, both exempt) had
 * ever needed to learn. */
#define ACCESSCTRL_WRITE_PASSWORD 0xacce0000UL

#define IO_BANK0_BASE          0x40028000UL
#define IO_BANK0_CTRL(n)       (IO_BANK0_BASE + 0x004 + (n) * 8)

#define PADS_BANK0_BASE        0x40038000UL
#define PADS_BANK0_PAD(n)      (PADS_BANK0_BASE + 0x004 + (n) * 4)

#define I2C_RTC_BASE            ((uintptr_t)CONFIG_I2C_RTC_BASE)
#define IC_CON                 (I2C_RTC_BASE + 0x00)
#define IC_TAR                 (I2C_RTC_BASE + 0x04)
#define IC_DATA_CMD            (I2C_RTC_BASE + 0x10)
#define IC_SS_SCL_HCNT         (I2C_RTC_BASE + 0x14)
#define IC_SS_SCL_LCNT         (I2C_RTC_BASE + 0x18)
#define IC_FS_SCL_HCNT         (I2C_RTC_BASE + 0x1C)
#define IC_FS_SCL_LCNT         (I2C_RTC_BASE + 0x20)
#define IC_INTR_STAT           (I2C_RTC_BASE + 0x2C)
#define IC_RAW_INTR_STAT       (I2C_RTC_BASE + 0x34)
#define IC_CLR_TX_ABRT         (I2C_RTC_BASE + 0x54)
#define IC_ENABLE              (I2C_RTC_BASE + 0x6C)
#define IC_STATUS              (I2C_RTC_BASE + 0x70)
#define IC_TXFLR               (I2C_RTC_BASE + 0x74)
#define IC_RXFLR               (I2C_RTC_BASE + 0x78)
#define IC_SDA_HOLD            (I2C_RTC_BASE + 0x7C)
#define IC_TX_ABRT_SOURCE      (I2C_RTC_BASE + 0x80)
#define IC_FS_SPKLEN           (I2C_RTC_BASE + 0xA0)

#define REG(addr) (*(volatile uint32_t *)(addr))

static void i2c_hw_init(void) {
    /* 1. Assert and then clear the peripheral's reset using RP2350 Atomic
     * Alias Registers (I2C0 or I2C1, whichever CONFIG_I2C_RTC_BASE says) */
    REG(RESETS_RESET_SET) = I2C_RTC_RESET_BIT;
    for (volatile int i = 0; i < 1000; i++);
    REG(RESETS_RESET_CLR) = I2C_RTC_RESET_BIT;
    int timeout = 10000;
    while (!(REG(RESETS_RESET_DONE) & I2C_RTC_RESET_BIT) && --timeout > 0);

    /* M5 Phase 3: the I2C controller needs to be Non-secure-accessible for
     * the U-mode task's own serve loop below to actually touch it -- must
     * happen here, from M-mode, before the task exists. See
     * ACCESSCTRL_I2C_RTC's own comment above -- including the write
     * password prefix that comment explains. */
    REG(ACCESSCTRL_I2C_RTC) = ACCESSCTRL_WRITE_PASSWORD | REG(ACCESSCTRL_I2C_RTC)
                              | ACCESSCTRL_I2C_NSP | ACCESSCTRL_I2C_NSU;

    /* 2. Configure GP4 (SDA) & GP5 (SCL) strictly as Function 3 (I2C) */
    REG(IO_BANK0_CTRL(I2C_SDA_PIN)) = 3;
    REG(IO_BANK0_CTRL(I2C_SCL_PIN)) = 3;

    /* 3. Enable Pull-ups, Input Enable, Schmitt Trigger on GP4 & GP5 pads (0x5A) */
    REG(PADS_BANK0_PAD(I2C_SDA_PIN)) = 0x5A;
    REG(PADS_BANK0_PAD(I2C_SCL_PIN)) = 0x5A;

    /* 4. Disable I2C0 before configuring */
    REG(IC_ENABLE) = 0;

    /* Master mode (bit 0), 7-bit addressing, Fast mode (2u << 1), Restart enable (bit 6), Slave disable (bit 5), TX_EMPTY_CTRL (bit 8) */
    REG(IC_CON) = (1u << 0) | (1u << 5) | (2u << 1) | (1u << 6) | (1u << 8);
    REG(IC_TAR) = DS1307_DS3231_I2C_ADDR;

    /* 100kHz Standard Mode Clock Dividers for 150MHz system clock */
    uint32_t freq_in = 150000000;
    uint32_t baudrate = 100000; // 100 kHz
    uint32_t period = (freq_in + baudrate / 2) / baudrate; // 1500 cycles
    uint32_t lcnt = period * 3 / 5; // 900
    uint32_t hcnt = period - lcnt;  // 600

    REG(IC_FS_SCL_HCNT) = hcnt;
    REG(IC_FS_SCL_LCNT) = lcnt;
    REG(IC_SS_SCL_HCNT) = hcnt;
    REG(IC_SS_SCL_LCNT) = lcnt;

    REG(IC_FS_SPKLEN) = lcnt < 16 ? 1 : lcnt / 16;

    /* Critical 300ns SDA Hold Time for 150MHz system clock (matching Pico SDK) */
    uint32_t sda_tx_hold_count = ((freq_in * 3) / 10000000) + 1; // 46 cycles = 307ns
    REG(IC_SDA_HOLD) = sda_tx_hold_count;

    /* Enable I2C0 */
    REG(IC_ENABLE) = 1;
}

static bool i2c_write_bytes(uint8_t addr, const uint8_t *src, int len) {
    REG(IC_ENABLE) = 0;
    REG(IC_TAR) = addr;
    REG(IC_ENABLE) = 1;
    (void)REG(IC_CLR_TX_ABRT);

    bool abort = false;
    for (int i = 0; i < len; i++) {
        bool last = (i == len - 1);
        uint32_t cmd = src[i];
        if (last) cmd |= (1u << 9); // STOP bit

        int timeout = 10000;
        while (!(REG(IC_STATUS) & (1u << 1)) && --timeout > 0); // Wait TX Not Full
        if (timeout == 0) return false;

        REG(IC_DATA_CMD) = cmd;

        timeout = 10000;
        do {
            if (REG(IC_RAW_INTR_STAT) & (1u << 6)) { // TX_ABRT (bit 6)
                abort = true;
                (void)REG(IC_CLR_TX_ABRT);
                break;
            }
        } while (--timeout > 0 && !(REG(IC_RAW_INTR_STAT) & (1u << 4))); // TX_EMPTY (bit 4)

        if (abort || timeout == 0) return false;
    }
    return !abort;
}

/* The read half, split out of i2c_read_bytes() so the generic transfer op
 * (I2C_OP_XFER, Q4) can reuse it byte for byte rather than growing a second
 * copy of the same controller sequence. No behaviour change: the RTC path
 * still reaches it through i2c_read_bytes() exactly as before. */
static bool i2c_read_phase(uint8_t addr, uint8_t *dst, int len) {
    REG(IC_ENABLE) = 0;
    REG(IC_TAR) = addr;
    REG(IC_ENABLE) = 1;
    (void)REG(IC_CLR_TX_ABRT);

    bool abort = false;
    for (int i = 0; i < len; i++) {
        bool last = (i == len - 1);
        uint32_t cmd = (1u << 8); // READ bit (bit 8)
        if (last) cmd |= (1u << 9); // STOP bit (bit 9)

        int timeout = 10000;
        while (!(REG(IC_STATUS) & (1u << 1)) && --timeout > 0); // Wait TX Not Full
        if (timeout == 0) return false;

        REG(IC_DATA_CMD) = cmd;

        timeout = 10000;
        do {
            if (REG(IC_RAW_INTR_STAT) & (1u << 6)) { // TX_ABRT (bit 6)
                abort = true;
                (void)REG(IC_CLR_TX_ABRT);
                break;
            }
        } while (--timeout > 0 && (REG(IC_STATUS) & (1u << 3)) == 0); // Wait RX Not Empty (bit 3)

        if (abort || timeout == 0) return false;

        dst[i] = (uint8_t)REG(IC_DATA_CMD);
    }
    return !abort;
}

static bool i2c_read_bytes(uint8_t addr, uint8_t reg, uint8_t *dst, int len) {
    if (!i2c_write_bytes(addr, &reg, 1)) return false;
    return i2c_read_phase(addr, dst, len);
}

/* Q4, plan/phase26_mqtt_and_environment_sensors.md: write `wlen` bytes, then
 * read `rlen`. Either half may be empty.
 *
 * This is every register access an I2C device generally needs, and that is
 * the point of having it: with one generic operation in the shared task, a
 * new part is a self-contained M-mode file rather than another opcode in a
 * U-mode dispatch that every future device would have to grow.
 *
 * It is not a loss of isolation. The U-mode `i2c` task exists to protect the
 * kernel from a bus driver's bugs, not the bus from its callers -- every
 * caller is kernel-side code in this same tree, and a generic opcode gives
 * them nothing they could not have by adding a case. The bound that does
 * matter, request and response size, is enforced by I2C_REQ_CAP/
 * I2C_RESP_CAP exactly as it is for every other op. */
static bool i2c_xfer_raw(uint8_t addr, const uint8_t *w, int wlen,
                         uint8_t *r, int rlen) {
    if (wlen > 0 && !i2c_write_bytes(addr, w, wlen)) return false;
    if (rlen > 0 && !i2c_read_phase(addr, r, rlen)) return false;
    return true;
}

static bool i2c_probe_addr(uint8_t addr);
static bool i2c_read_bytes(uint8_t addr, uint8_t reg, uint8_t *dst, int len);

/* --- The instrument (phase 30) -----------------------------------------
 *
 * Three address probes and three register reads of one part, each followed by
 * IC_TX_ABRT_SOURCE and IC_STATUS. Kept because it is what turned a day of
 * plausible theories into an answer in one line, and because the answer was
 * not in the software at all.
 *
 * **How to read it.** Three in a row, because one tells you nothing: 1,1,1 is
 * a working part, 0,0,0 with `abrt` bit 0 set is nothing at that address, and
 * 0,0,0 with `abrt == 0` is neither -- no device NACKed, so the transaction
 * simply never finished. That last pattern, with `status` showing
 * MST_ACTIVITY set and both FIFOs empty, means the bus is not being driven
 * the way the controller thinks it is.
 *
 * **What that turned out to be, 2026-09-11.** A marginal ground. Three parts
 * daisy-chained off a Pico 2 W read as present to `i2c scan` and failed every
 * register read; four software hypotheses were written, tested and discarded
 * against it. The fault was the GND jumper, and moving it fixed everything
 * with the driver untouched.
 *
 * The tell was there the whole time and is worth naming: **`i2c scan` worked
 * while everything else failed.** The scan emits a cprintf() between probes,
 * so its transactions are milliseconds apart, and a bus whose reference is
 * floating recovers in the gaps. Anything back-to-back -- the boot probes,
 * this diagnostic -- does not. If you see that split again, check the wiring
 * before reading any more of this file. */
void i2c_rp2350_diag(uint8_t addr, uint8_t reg) {
    cprintf("[I2Cdiag] CON=0x%08x TAR=0x%08x ENABLE=0x%08x STATUS=0x%08x RAW=0x%08x\n",
            (unsigned)REG(IC_CON), (unsigned)REG(IC_TAR),
            (unsigned)REG(IC_ENABLE), (unsigned)REG(IC_STATUS),
            (unsigned)REG(IC_RAW_INTR_STAT));
    for (int i = 0; i < 3; i++) {
        bool ok = i2c_probe_addr(addr);
        cprintf("[I2Cdiag] probe#%d 0x%02x -> %d  abrt=0x%08x status=0x%08x\n",
                i, addr, (int)ok, (unsigned)REG(IC_TX_ABRT_SOURCE),
                (unsigned)REG(IC_STATUS));
        (void)REG(IC_CLR_TX_ABRT);
    }
    for (int i = 0; i < 3; i++) {
        uint8_t v = 0xff;
        bool ok = i2c_read_bytes(addr, reg, &v, 1);
        cprintf("[I2Cdiag] read#%d 0x%02x reg 0x%02x -> %d val=0x%02x  "
                "abrt=0x%08x status=0x%08x\n",
                i, addr, reg, (int)ok, (unsigned)v,
                (unsigned)REG(IC_TX_ABRT_SOURCE), (unsigned)REG(IC_STATUS));
        (void)REG(IC_CLR_TX_ABRT);
    }
}

static bool i2c_probe_addr(uint8_t addr) {
    REG(IC_ENABLE) = 0;
    REG(IC_TAR) = addr;
    REG(IC_ENABLE) = 1;
    (void)REG(IC_CLR_TX_ABRT);

    // Send READ command + STOP bit (matching Pico SDK probe)
    uint32_t cmd = (1u << 8) | (1u << 9);
    REG(IC_DATA_CMD) = cmd;

    int timeout = 10000;
    bool abort = false;
    do {
        if (REG(IC_RAW_INTR_STAT) & (1u << 6)) { // TX_ABRT
            abort = true;
            (void)REG(IC_CLR_TX_ABRT);
            break;
        }
    } while (--timeout > 0 && (REG(IC_STATUS) & (1u << 3)) == 0); // RXFLR / RFNE

    if (!abort && timeout > 0) {
        (void)REG(IC_DATA_CMD); // Drain byte from RX FIFO
        return true; // ACK!
    }
    return false;
}
#elif defined(CONFIG_BOARD_ESP32P4)
/* ESP32-P4 I2C0 -- E7, plan/phase27_esp32p4_bringup.md.
 *
 * A different peripheral from RP2350's above, not a variant of it: that is a
 * Synopsys DW_apb_i2c driven by writing bytes into IC_DATA_CMD, this is
 * Espressif's own controller, where a transfer is assembled as a short
 * *command list* (up to 8 opcodes) and then started in one go. Nothing was
 * shared between the two beyond the five functions at this seam.
 *
 * Registers from TRM chapter 22 and IDF's generated i2c_reg.h (hw_ver1 --
 * this board is v1.3), and the timing/opcode constants from IDF's own
 * esp_hal_i2c/esp32p4 layer. That last part is not decoration: **the command
 * opcodes are not the same as on older ESP32 parts.** Here READ is 3 and STOP
 * is 2; on ESP32/S3 they are 2 and 3. Transcribing from an older chip's
 * driver would produce a controller that issues a STOP where a READ belongs
 * and reports a timeout, which is exactly the class of bug section 3.2 of the
 * phase plan exists to prevent.
 *
 * Pins are GPIO7 (SDA) and GPIO8 (SCL), which is what the board's own header
 * exposes and what the factory demo used. They reach the controller through
 * the **GPIO matrix**, not IO_MUX: I2C0 has no direct pad function on these
 * pins, so the signal indices (68 SCL, 69 SDA) are routed both ways.
 */
/* The RP2350 arm above defines its own REG() inside its register block, so
 * this arm needs one too -- and a cast through uintptr_t, since these bases
 * are unsigned long constants rather than pointers. */
#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

#define P4_I2C_BASE        0x500C4000UL
#define P4_I2C_SCL_LOW     (P4_I2C_BASE + 0x00)
#define P4_I2C_CTR         (P4_I2C_BASE + 0x04)
#define P4_I2C_SR          (P4_I2C_BASE + 0x08)
#define P4_I2C_TO          (P4_I2C_BASE + 0x0c)
#define P4_I2C_FIFO_ST     (P4_I2C_BASE + 0x14)
#define P4_I2C_FIFO_CONF   (P4_I2C_BASE + 0x18)
#define P4_I2C_DATA        (P4_I2C_BASE + 0x1c)
#define P4_I2C_INT_RAW     (P4_I2C_BASE + 0x20)
#define P4_I2C_INT_CLR     (P4_I2C_BASE + 0x24)
#define P4_I2C_SDA_HOLD    (P4_I2C_BASE + 0x30)
#define P4_I2C_SDA_SAMPLE  (P4_I2C_BASE + 0x34)
#define P4_I2C_SCL_HIGH    (P4_I2C_BASE + 0x38)
#define P4_I2C_SCL_START_HOLD   (P4_I2C_BASE + 0x40)
#define P4_I2C_SCL_RSTART_SETUP (P4_I2C_BASE + 0x44)
#define P4_I2C_SCL_STOP_HOLD    (P4_I2C_BASE + 0x48)
#define P4_I2C_SCL_STOP_SETUP   (P4_I2C_BASE + 0x4c)
#define P4_I2C_FILTER_CFG  (P4_I2C_BASE + 0x50)
#define P4_I2C_COMD(n)     (P4_I2C_BASE + 0x58 + 4u * (n))
#define P4_I2C_SCL_ST_TO        (P4_I2C_BASE + 0x78)
#define P4_I2C_SCL_MAIN_ST_TO   (P4_I2C_BASE + 0x7c)
#define P4_I2C_SCL_SP_CONF      (P4_I2C_BASE + 0x80)

#define P4_I2C_TIME_OUT_EN     (1u << 5)    /* TO: above the 5-bit value    */
#define P4_I2C_SCL_FILTER_EN   (1u << 8)    /* FILTER_CFG, one per line     */
#define P4_I2C_SDA_FILTER_EN   (1u << 9)
#define P4_I2C_SCL_RST_SLV_EN  (1u << 0)    /* SCL_SP_CONF: clear the bus   */
#define P4_I2C_SCL_RST_SLV_NUM(n) ((uint32_t)(n) << 1)

#define P4_I2C_SDA_FORCE_OUT  (1u << 0)
#define P4_I2C_SCL_FORCE_OUT  (1u << 1)
#define P4_I2C_MS_MODE        (1u << 4)
#define P4_I2C_TRANS_START    (1u << 5)
#define P4_I2C_CLK_EN         (1u << 8)
#define P4_I2C_FSM_RST        (1u << 10)
#define P4_I2C_CONF_UPGATE    (1u << 11)
#define P4_I2C_RX_FIFO_RST    (1u << 12)
#define P4_I2C_TX_FIFO_RST    (1u << 13)
#define P4_I2C_FIFO_PRT_EN    (1u << 14)

#define P4_I2C_INT_END_DETECT      (1u << 3)
#define P4_I2C_INT_ARB_LOST        (1u << 5)
#define P4_I2C_INT_TRANS_COMPLETE  (1u << 7)
#define P4_I2C_INT_TIME_OUT        (1u << 8)
#define P4_I2C_INT_NACK            (1u << 10)

/* Opcodes. See the header comment: these are the P4's, not ESP32's. */
#define P4_CMD_RSTART 6u
#define P4_CMD_WRITE  1u
#define P4_CMD_STOP   2u
#define P4_CMD_READ   3u
#define P4_CMD_END    4u

/* op<<11 | ack_check_en<<8 | ack_value<<10 | byte_num */
#define P4_CMD(op, ack_check, ack_val, n) \
    (((uint32_t)(op) << 11) | ((uint32_t)(ack_check) << 8) | \
     ((uint32_t)(ack_val) << 10) | (uint32_t)(n))

#define P4_CLKRST_BASE      0x500E6000UL
#define P4_SOC_CLK_CTRL2    (P4_CLKRST_BASE + 0x1c)   /* bit 12: I2C0 APB gate */
#define P4_PERI_CLK_CTRL10  (P4_CLKRST_BASE + 0x40)   /* b0 src sel, b1 clk en */
#define P4_HP_RST_EN1       (P4_CLKRST_BASE + 0xc4)   /* bit 22: I2C0 reset    */

#define P4_GPIO_BASE        0x500E0000UL
#define P4_GPIO_ENABLE_W1TS (P4_GPIO_BASE + 0x24)
#define P4_GPIO_PIN(n)      (P4_GPIO_BASE + 0x74 + 4u * (n))
#define P4_GPIO_IN_SEL(sig) (P4_GPIO_BASE + 0x158 + 4u * (sig))
#define P4_GPIO_OUT_SEL(n)  (P4_GPIO_BASE + 0x558 + 4u * (n))
#define P4_GPIO_PAD_DRIVER  (1u << 2)                 /* open drain           */
#define P4_IOMUX_PAD(n)     (0x500E1000UL + 0x4 + 4u * (n))
#define P4_IOMUX_FUN_GPIO   (1u << 12)                /* MCU_SEL = 1          */
#define P4_IOMUX_FUN_IE     (1u << 9)
#define P4_IOMUX_FUN_PU     (1u << 8)

#define P4_I2C_SDA_GPIO 7u
#define P4_I2C_SCL_GPIO 8u
#define P4_I2C_SDA_SIG  69u
#define P4_I2C_SCL_SIG  68u

static bool g_p4_i2c_ready;
static void i2c_p4_bringup_timing(void);
static bool i2c_xfer_raw(uint8_t addr, const uint8_t *w, int wlen,
                         uint8_t *r, int rlen);
static bool i2c_probe_addr(uint8_t addr);
static void p4_i2c_bus_clear(void);
static uint32_t g_p4_i2c_last_int;
static uint32_t g_p4_i2c_last_sr;

static void p4_pad_for_i2c(uint32_t gpio, uint32_t sig) {
    /* Open drain, input enabled, weak pull-up. The pull-up matters even with
     * a module that has its own: without it an absent module leaves the line
     * floating, and a floating SDA reads as a permanent ACK from every
     * address, which is a bus scan that finds 128 devices. */
    uint32_t pad = REG(P4_IOMUX_PAD(gpio));
    pad &= ~(7u << 12);
    REG(P4_IOMUX_PAD(gpio)) = pad | P4_IOMUX_FUN_GPIO | P4_IOMUX_FUN_IE | P4_IOMUX_FUN_PU;
    REG(P4_GPIO_PIN(gpio)) |= P4_GPIO_PAD_DRIVER;
    REG(P4_GPIO_ENABLE_W1TS) = (1u << gpio);
    /* Both directions: the controller drives the line and also samples it, so
     * a one-way route gives a bus that transmits and never sees an ACK. */
    REG(P4_GPIO_OUT_SEL(gpio)) = sig;
    REG(P4_GPIO_IN_SEL(sig))   = gpio | (1u << 7);   /* bit 7: take from matrix */
}

/* The controller's own bringup: clocks, reset, pads, master mode, timing.
 *
 * This ran from `i2cdiag` alone for most of E7, because doing it at boot
 * wedged the board mid-console-line -- the machine reached the shell and then
 * stopped inside a printk. That was never this sequence: it was the L2-cache
 * corruption at the top of E7 eating the frame of the task the UART interrupt
 * was trying to wake. With the memory map fixed the sequence runs at boot,
 * which is where a bus that the RTC and sensor probes need has to come up.
 *
 * `verbose` narrates each step through printk_critical() -- the diagnostic
 * shape kept from that hunt, because it costs one branch and it is the only
 * console path that survives a step which takes the peripheral bus down.
 * Idempotent: the second caller finds g_p4_i2c_ready and returns. */
static void i2c_p4_hw_bringup(bool verbose) {
    if (g_p4_i2c_ready) {
        if (verbose) printk_critical("[I2Cdiag] already up\n");
        return;
    }

    if (verbose) printk_critical("[I2Cdiag] 1: APB gate\n");
    REG(P4_SOC_CLK_CTRL2) |= (1u << 12);

    if (verbose) printk_critical("[I2Cdiag] 2: module clock, XTAL source\n");
    REG(P4_PERI_CLK_CTRL10) &= ~(1u << 0);
    REG(P4_PERI_CLK_CTRL10) |= (1u << 1);

    if (verbose) printk_critical("[I2Cdiag] 3: reset pulse\n");
    REG(P4_HP_RST_EN1) |=  (1u << 22);
    REG(P4_HP_RST_EN1) &= ~(1u << 22);

    if (verbose) printk_critical("[I2Cdiag] 4: pad GPIO7 (SDA)\n");
    p4_pad_for_i2c(P4_I2C_SDA_GPIO, P4_I2C_SDA_SIG);

    if (verbose) printk_critical("[I2Cdiag] 5: pad GPIO8 (SCL)\n");
    p4_pad_for_i2c(P4_I2C_SCL_GPIO, P4_I2C_SCL_SIG);

    if (verbose) printk_critical("[I2Cdiag] 6: CTR\n");
    REG(P4_I2C_CTR) = P4_I2C_MS_MODE | P4_I2C_CLK_EN |
                      P4_I2C_SDA_FORCE_OUT | P4_I2C_SCL_FORCE_OUT;

    if (verbose) printk_critical("[I2Cdiag] 7: timing\n");
    i2c_p4_bringup_timing();

    if (verbose) printk_critical("[I2Cdiag] 8: fifo + commit\n");
    REG(P4_I2C_FIFO_CONF) = P4_I2C_FIFO_PRT_EN;
    REG(P4_I2C_CTR) |= P4_I2C_CONF_UPGATE;
    g_p4_i2c_ready = true;

    /* The controller is fresh; the bus is not.
     *
     * Resetting this chip does not reset what is wired to it. A part left
     * mid-byte by the previous run -- or by the ROM, or by the factory image
     * -- is still holding SDA when we arrive, and the first transaction after
     * bringup then comes back wrong: E7 saw both halves of that, an address
     * scan reporting a phantom device at the first address it tried, and a
     * BME280 that is present NACKing its first read of the boot. Which of the
     * two it was depended only on which transaction happened to be first,
     * which is why it looked intermittent.
     *
     * Nine clocks here cost 90 microseconds once and make the first
     * transaction as trustworthy as the rest. */
    if (verbose) printk_critical("[I2Cdiag] 9: bus clear\n");
    p4_i2c_bus_clear();
    if (verbose) printk_critical("[I2Cdiag] bringup complete\n");
}

static void i2c_hw_init(void) { i2c_p4_hw_bringup(false); }

/* Every period below is in cycles of the controller's source clock, which
 * step 2 above selects as XTAL_CLK: 40 MHz on this board, undivided
 * (PERI_CLK_CTRL10's div_num field is 0, which is a divide by one). A half
 * cycle of 200 is therefore 100 kHz -- standard mode, which is what a BME280
 * on flying leads wants over the board's own pull-ups. Those are 2.2 kOhm to
 * 3V3 on both lines, fitted on the NANO itself (ESP32-P4-NANO-schematic.pdf,
 * the ESP_I2C_SDA/ESP_I2C_SCL nets) -- so the pads' internal pull-ups that
 * p4_pad_for_i2c() also enables are a backstop for a bare chip, not what is
 * holding this bus up.
 *
 * The numbers are IDF's own i2c_ll_master_cal_bus_clk() evaluated for that
 * pair, not a plausible-looking set: this file's first version halved four of
 * them and left two register fields unwritten, and what that produced was a
 * bus that answered one scan and then nothing. Three of those matter enough
 * to name:
 *
 *  * **SCL_WAIT_HIGH lives in the same register as SCL_HIGH**, bits [15:9]
 *    over bits [8:0]. Writing one number wrote SCL_HIGH and left the FSM's
 *    wait period at zero -- and the hardware's own documented assumption is
 *    scl_wait_high < sda_sample < scl_high.
 *  * **TIME_OUT_EN is bit 5 of the TO register**, above the 5-bit value.
 *    Writing the value alone leaves the SCL timeout *disabled*, so a slave
 *    that stretches forever wedges the FSM instead of ending the transaction
 *    with TIME_OUT set. That is the difference between an error this driver
 *    can recover from and one it cannot see.
 *  * **The SDA glitch filter has its own enable**, bit 9. Only bit 8 was set,
 *    so SCL was filtered and SDA was not, and the threshold that was meant
 *    for both was 15 on SCL and 7 on SDA.
 *
 * IDF subtracts one from the periods the TRM specifies that way, and
 * deliberately does not subtract it from SCL_HIGH/SCL_WAIT_HIGH; that
 * asymmetry is copied rather than tidied, because it is a measurement
 * ("according to practical measurement and some hardware behaviour") and not
 * an oversight. */
static void i2c_p4_bringup_timing(void) {
    const uint32_t half = 200u;                   /* 40 MHz / 100 kHz / 2 */
    const uint32_t wait_high = half / 2u - 2u;    /* 98, for >= 80 kHz    */
    const uint32_t high      = half - wait_high;  /* 102                  */
    const uint32_t sample    = half / 2u;         /* 100                  */

    REG(P4_I2C_SCL_LOW)          = half - 1u;
    REG(P4_I2C_SCL_HIGH)         = high | (wait_high << 9);
    REG(P4_I2C_SCL_START_HOLD)   = half - 1u;
    REG(P4_I2C_SCL_RSTART_SETUP) = half - 1u;
    REG(P4_I2C_SCL_STOP_HOLD)    = half - 1u;
    REG(P4_I2C_SCL_STOP_SETUP)   = half - 1u;
    REG(P4_I2C_SDA_HOLD)         = half / 4u - 1u;
    REG(P4_I2C_SDA_SAMPLE)       = sample - 1u;
    /* threshold 7 on each line, both filters enabled */
    REG(P4_I2C_FILTER_CFG)       = 7u | (7u << 4) | P4_I2C_SCL_FILTER_EN |
                                   P4_I2C_SDA_FILTER_EN;
    /* 2^12 source cycles, about 10 bus cycles, and the enable that makes it
     * mean anything. IDF's formula: ceil(log2(5 * half_cycle)) + 2. */
    REG(P4_I2C_TO)               = 12u | P4_I2C_TIME_OUT_EN;
    REG(P4_I2C_SCL_ST_TO)        = 0x10u;
    REG(P4_I2C_SCL_MAIN_ST_TO)   = 0x10u;

    REG(P4_I2C_FIFO_CONF) = P4_I2C_FIFO_PRT_EN;
    REG(P4_I2C_CTR) |= P4_I2C_CONF_UPGATE;
    g_p4_i2c_ready = true;
}

static void p4_i2c_reset_fifo(void) {
    REG(P4_I2C_FIFO_CONF) |= P4_I2C_TX_FIFO_RST | P4_I2C_RX_FIFO_RST;
    REG(P4_I2C_FIFO_CONF) &= ~(P4_I2C_TX_FIFO_RST | P4_I2C_RX_FIFO_RST);
    REG(P4_I2C_INT_CLR) = 0xffffffffu;
}

/* Every transaction starts here, and it starts by throwing away whatever the
 * last one left.
 *
 * The FIFO reset alone is not enough, and the board says so: with only that,
 * exactly one transaction after a NACKed one comes back wrong -- a false NACK
 * from a part that is present, or a false ACK from an address that is empty.
 * Three probes of the BME280 in a row read 0, 1, 1, and an address scan found
 * a phantom device at the first address it tried. The FIFO was clean each
 * time (FIFO_ST reads back zero); what was not clean was the master FSM,
 * which SR reports as having stopped mid-transfer.
 *
 * IDF's driver can leave this out because it owns the controller and tracks
 * its own state across calls. This one cannot: the same peripheral is entered
 * from an address scan, the RTC probe, the EEPROM and the sensor, in an order
 * nobody here decides, and "what did the previous caller leave behind" is not
 * a question any of them should have to answer. Two register writes at the
 * top of each transaction removes it. */
static void p4_i2c_begin(void) {
    REG(P4_I2C_CTR) |= P4_I2C_FSM_RST;
    p4_i2c_reset_fifo();
    REG(P4_I2C_CTR) |= P4_I2C_CONF_UPGATE;
}

/* What a failed transaction leaves behind, and why nothing works afterwards
 * until it is cleared.
 *
 * A NACK, a timeout or a lost arbitration stops the command list where it
 * stands: the master FSM is mid-transfer, SCL is wherever the abort left it,
 * and a slave that was clocking out a byte may still be holding SDA low
 * waiting for the ninth clock it never got. Starting the next command list
 * into that state does not begin a new transfer -- it inherits a broken one.
 *
 * This is precisely what an address scan looks like when it is missing: the
 * first NACKed address wedges the controller and every later address reports
 * absent, so a bus with one device on it scans as a bus with one device, then
 * as an empty bus, depending on where the wedge happened to land. On this
 * board the ES8311 codec at 0x18 made that visible -- a part that is soldered
 * down and cannot come and go.
 *
 * Two steps, in the order IDF's own recovery uses. FSM_RST (self-clearing)
 * returns the master to idle without disturbing timing or filter config.
 * SCL_RST_SLV_EN then clocks out up to nine SCL pulses and a STOP, which is
 * the standard way to walk a slave off a byte it is still transmitting; the
 * hardware clears the enable when it is done, so waiting on that bit is
 * waiting for the bus, not for a fixed delay. */
/* Nine SCL pulses and a STOP: the standard way to walk a slave off a byte it
 * is still transmitting. A device that was mid-transfer when the master went
 * away holds SDA low waiting for a clock that never comes, and no START the
 * master issues afterwards is seen -- the bus reads as busy and the first
 * address of the next transaction is NACKed, or worse, ACKed by the stuck
 * device.
 *
 * The hardware clears the enable when it has sent the pulses, so waiting on
 * that bit waits for the bus rather than for a guessed delay. */
static void p4_i2c_bus_clear(void) {
    REG(P4_I2C_SCL_SP_CONF) = P4_I2C_SCL_RST_SLV_NUM(9) | P4_I2C_SCL_RST_SLV_EN;
    REG(P4_I2C_CTR) |= P4_I2C_CONF_UPGATE;

    /* Bounded in wall time for the reason p4_i2c_run() is: nine pulses at
     * 100 kHz is 90 us, and a bus that has not finished them in 5 ms is not
     * going to. Give up rather than spin -- the enable is then cleared by
     * hand so the next transaction does not start into a pending clear. */
    uint64_t deadline = time_get_us() + 5000u;
    while ((REG(P4_I2C_SCL_SP_CONF) & P4_I2C_SCL_RST_SLV_EN) != 0) {
        if (time_get_us() >= deadline) {
            REG(P4_I2C_SCL_SP_CONF) = 0;
            break;
        }
    }
    REG(P4_I2C_CTR) |= P4_I2C_CONF_UPGATE;
    REG(P4_I2C_INT_CLR) = 0xffffffffu;
}

/* After a failure: the ordinary preparation, plus the bus clear.
 *
 * The clear is *here* and in bringup, and deliberately not in p4_i2c_begin().
 * Putting it before every transaction was tried and made things worse rather
 * than better -- a clear ends in a STOP, and a START issued straight after it
 * does not leave the bus-free time a slave needs (t_BUF, 4.7 us at 100 kHz),
 * so the first transfer of a burst started NACKing a part that was there. The
 * two places it belongs are the two where the bus may genuinely be held by
 * someone else: at bringup, when this kernel has just arrived and has no idea
 * what the previous firmware left mid-byte, and after an error, when the
 * transfer we just abandoned may have stopped a slave mid-byte ourselves. */
static void p4_i2c_recover(void) {
    p4_i2c_begin();
    p4_i2c_bus_clear();
}

/* Runs a command list that has already been written, and reports what the
 * bus said. Bounded: a stuck bus must not become a stuck kernel. */
/* g_p4_i2c_last_int / _sr (declared above): what the last transaction
 * ended on. */

static bool p4_i2c_run(void) {
    REG(P4_I2C_CTR) |= P4_I2C_CONF_UPGATE;
    REG(P4_I2C_CTR) |= P4_I2C_TRANS_START;

    /* Bounded in *wall time*, not in iterations.
     *
     * The first version counted loop passes, which is a bound on a fast core
     * and effectively none on this one: without a PLL the P4 runs at 40 MHz,
     * and 200000 MMIO reads at that speed took long enough that a 128-address
     * bus scan looked exactly like a hung board. A transaction of a few bytes
     * at 100 kHz is under a millisecond; 10 ms is generous and still leaves a
     * full scan of an empty bus well under two seconds. */
    uint64_t deadline = time_get_us() + 10000u;
    uint32_t st = 0;
    do {
        st = REG(P4_I2C_INT_RAW);
        if (st & (P4_I2C_INT_NACK | P4_I2C_INT_TIME_OUT | P4_I2C_INT_ARB_LOST)) break;
        if (st & (P4_I2C_INT_TRANS_COMPLETE | P4_I2C_INT_END_DETECT)) break;
    } while (time_get_us() < deadline);

    g_p4_i2c_last_int = st;
    g_p4_i2c_last_sr  = REG(P4_I2C_SR);

    bool ok = (st & (P4_I2C_INT_TRANS_COMPLETE | P4_I2C_INT_END_DETECT)) != 0 &&
              (st & (P4_I2C_INT_NACK | P4_I2C_INT_TIME_OUT | P4_I2C_INT_ARB_LOST)) == 0;
    /* Every failure, including the deadline expiring with no bit set at all,
     * leaves the FSM somewhere this driver did not put it. See
     * p4_i2c_recover(): without this, one absent address makes the whole bus
     * absent. */
    if (!ok) p4_i2c_recover();
    return ok;
}

/* Step-by-step through printk_critical(), because the failure
 * being chased is a machine that stops rather than a wrong answer, and the
 * ordinary console is part of what stops: printk() batches and reaches the
 * wire through the uart *task*, so a step that takes the scheduler or the
 * peripheral bus down takes the evidence with it. printk_critical() writes
 * straight at the UART FIFO with a bounded spin and no task involvement
 * (kernel/printk.c) -- which is exactly the case it was written for. Called
 * from the shell task directly rather than through the i2c endpoint, so the
 * i2c task is not in the picture either. */
void i2c_p4_diag(void) {
    i2c_p4_hw_bringup(true);
    printk_critical("[I2Cdiag] ready=%d\n", (int)g_p4_i2c_ready);
   
    printk_critical("[I2Cdiag] CTR=0x%08x\n", (unsigned)REG(P4_I2C_CTR));
   
    printk_critical("[I2Cdiag] SR=0x%08x FIFO_ST=0x%08x\n",
           (unsigned)REG(P4_I2C_SR), (unsigned)REG(P4_I2C_FIFO_ST));
   
    printk_critical("[I2Cdiag] clkrst: CTRL2=0x%08x CTRL10=0x%08x RST1=0x%08x\n",
           (unsigned)REG(P4_SOC_CLK_CTRL2), (unsigned)REG(P4_PERI_CLK_CTRL10),
           (unsigned)REG(P4_HP_RST_EN1));
   
    printk_critical("[I2Cdiag] pads: io7=0x%08x io8=0x%08x out7=0x%08x in69=0x%08x\n",
           (unsigned)REG(P4_IOMUX_PAD(7)), (unsigned)REG(P4_IOMUX_PAD(8)),
           (unsigned)REG(P4_GPIO_OUT_SEL(7)), (unsigned)REG(P4_GPIO_IN_SEL(69)));
   
    /* Both shapes, three times each, and the repetition is the measurement.
     *
     * One probe cannot tell "the part is there" from "the last transaction
     * left the controller somewhere". Three in a row can, and the reading is
     * mechanical: 1,1,1 is a part; 0,0,0 is an empty address; **0,1,1 is a
     * stale controller** -- the first transaction paying for what the
     * previous one left behind. That last pattern is what found
     * p4_i2c_begin() and the bringup bus clear, so the diagnostic keeps the
     * shape that found it rather than reporting a single verdict.
     *
     * The read is reported separately from the probe because they are
     * different command lists and it is possible for one to work while the
     * other does not -- E7 saw exactly that, an address-only probe NACKing
     * while a register read of the same part returned its chip id. Every
     * register access this driver makes is the read shape, so that is the one
     * that matters; the probe is what an address scan uses. */
    for (int i = 0; i < 3; i++) {
        bool pok = i2c_probe_addr(0x76u);
        printk_critical("[I2Cdiag] probe#%d 0x76 -> %d  INT_RAW=0x%08x SR=0x%08x\n",
               i, (int)pok, (unsigned)g_p4_i2c_last_int, (unsigned)g_p4_i2c_last_sr);
    }
    for (int i = 0; i < 3; i++) {
        uint8_t reg = 0xd0u, id = 0;
        bool rok = i2c_xfer_raw(0x76u, &reg, 1, &id, 1);
        printk_critical("[I2Cdiag] read#%d 0x76 reg 0xd0 -> %d id=0x%02x  INT_RAW=0x%08x SR=0x%08x\n",
               i, (int)rok, (unsigned)id, (unsigned)g_p4_i2c_last_int,
               (unsigned)g_p4_i2c_last_sr);
    }
}

/* What the last transaction ended on, for i2cdiag. Exposed rather than
 * printed here: this runs inside the i2c task, which may not printk(). */
void i2c_p4_last_status(uint32_t *int_raw, uint32_t *sr) {
    if (int_raw) *int_raw = g_p4_i2c_last_int;
    if (sr) *sr = g_p4_i2c_last_sr;
}

static bool i2c_write_bytes(uint8_t addr, const uint8_t *src, int len) {
    if (!g_p4_i2c_ready || len < 0 || len > 30) return false;
    p4_i2c_begin();
    REG(P4_I2C_DATA) = (uint32_t)(addr << 1);          /* address + write     */
    for (int i = 0; i < len; i++) REG(P4_I2C_DATA) = src[i];
    REG(P4_I2C_COMD(0)) = P4_CMD(P4_CMD_RSTART, 0, 0, 0);
    REG(P4_I2C_COMD(1)) = P4_CMD(P4_CMD_WRITE, 1, 0, 1 + len);
    REG(P4_I2C_COMD(2)) = P4_CMD(P4_CMD_STOP, 0, 0, 0);
    return p4_i2c_run();
}
/* Write-then-read with a repeated start, which is the shape every register
 * read on this bus takes: address+W, the register number, RESTART,
 * address+R, then the data. Building it as one command list is what makes it
 * a repeated start rather than two transfers with a STOP between -- and a
 * STOP there is what lets another master, or a device with an internal
 * pointer, lose the register selection. */
static bool i2c_xfer_raw(uint8_t addr, const uint8_t *w, int wlen,
                         uint8_t *r, int rlen) {
    if (!g_p4_i2c_ready) return false;
    if (wlen < 0 || rlen < 0 || wlen > 30 || rlen > 30) return false;
    if (rlen == 0) return i2c_write_bytes(addr, w, wlen);

    p4_i2c_begin();
    REG(P4_I2C_DATA) = (uint32_t)(addr << 1);
    for (int i = 0; i < wlen; i++) REG(P4_I2C_DATA) = w[i];
    REG(P4_I2C_DATA) = (uint32_t)((addr << 1) | 1u);

    unsigned c = 0;
    REG(P4_I2C_COMD(c++)) = P4_CMD(P4_CMD_RSTART, 0, 0, 0);
    REG(P4_I2C_COMD(c++)) = P4_CMD(P4_CMD_WRITE, 1, 0, 1 + wlen);
    REG(P4_I2C_COMD(c++)) = P4_CMD(P4_CMD_RSTART, 0, 0, 0);
    REG(P4_I2C_COMD(c++)) = P4_CMD(P4_CMD_WRITE, 1, 0, 1);
    if (rlen > 1) {
        /* All but the last byte are ACKed; the last is NACKed, which is how a
         * master tells the device the read is over. Sending ACK for the final
         * byte leaves the device driving the bus into the STOP. */
        REG(P4_I2C_COMD(c++)) = P4_CMD(P4_CMD_READ, 0, 0, rlen - 1);
    }
    REG(P4_I2C_COMD(c++)) = P4_CMD(P4_CMD_READ, 0, 1, 1);
    REG(P4_I2C_COMD(c++)) = P4_CMD(P4_CMD_STOP, 0, 0, 0);

    if (!p4_i2c_run()) return false;
    for (int i = 0; i < rlen; i++) r[i] = (uint8_t)(REG(P4_I2C_DATA) & 0xffu);
    return true;
}

static bool i2c_read_bytes(uint8_t addr, uint8_t reg, uint8_t *dst, int len) {
    return i2c_xfer_raw(addr, &reg, 1, dst, len);
}

/* Address-only transaction: a START, the address byte with ACK checking on,
 * and a STOP. Nothing is read, so the only thing that distinguishes a present
 * device from an absent one is whether the address was acknowledged -- which
 * is precisely what p4_i2c_run() returns false for. */
static bool i2c_probe_addr(uint8_t addr) {
    if (!g_p4_i2c_ready) return false;
    p4_i2c_begin();
    REG(P4_I2C_DATA) = (uint32_t)(addr << 1);
    REG(P4_I2C_COMD(0)) = P4_CMD(P4_CMD_RSTART, 0, 0, 0);
    REG(P4_I2C_COMD(1)) = P4_CMD(P4_CMD_WRITE, 1, 0, 1);
    REG(P4_I2C_COMD(2)) = P4_CMD(P4_CMD_STOP, 0, 0, 0);
    return p4_i2c_run();
}
#else
static void i2c_hw_init(void) {}
static bool i2c_probe_addr(uint8_t addr) { (void)addr; return false; }
static bool i2c_write_bytes(uint8_t addr, const uint8_t *src, int len) {
    (void)addr; (void)src; (void)len; return false;
}
static bool i2c_read_bytes(uint8_t addr, uint8_t reg, uint8_t *dst, int len) {
    (void)addr; (void)reg; (void)dst; (void)len; return false;
}
static bool i2c_xfer_raw(uint8_t addr, const uint8_t *w, int wlen,
                         uint8_t *r, int rlen) {
    (void)addr; (void)w; (void)wlen; (void)r; (void)rlen; return false;
}
#endif

static inline uint8_t bcd2dec(uint8_t val) {
    return ((val >> 4) * 10) + (val & 0x0F);
}

static inline uint8_t dec2bcd(uint8_t val) {
    return ((val / 10) << 4) | (val % 10);
}

/* Forward declarations: direct-hardware access, defined further down this
 * file. Only i2c_rtc_init() (once, at boot, before the task exists) and
 * i2c_task_body() (below) may call these -- every other caller goes through
 * the public i2c_rtc_read_time()/write_time()/read_temperature_c() facades,
 * which route via the shared "i2c" task when it is alive. */
static bool i2c_rtc_hw_read_time(rtc_time_t *tm);
static bool i2c_rtc_hw_write_time(const rtc_time_t *tm);
static bool i2c_rtc_hw_read_temperature_c(int *temp_c);

/* DS3231 status register, and the one bit in it that matters here.
 *
 * OSF is set by the chip whenever its oscillator has stopped -- which is what
 * happens when it loses power with no working backup cell -- and stays set
 * until something clears it. It is the chip saying, in the only way it can,
 * "the time in my registers is meaningless".
 *
 * It has to be read, because the meaningless value is *plausible*: a
 * DS3231 that has lost power reads 2000-01-01 00:00:00, and every range check
 * anyone would write -- month 1-12, day 1-31, hour under 24 -- passes it. A
 * clock face then shows midnight on New Year's Day 2000 with no indication
 * that anything is wrong, which is exactly what a Pico-Clock-Green did for a
 * day while the board's own kernel clock was correct to the millisecond.
 *
 * The DS1307 uses bit 7 of register 0 (CH, "clock halt") for the same job.
 * Both are checked; on a part that has neither, the register reads as
 * something without those bits set and nothing is lost. */
#define DS3231_REG_STATUS 0x0F
#define DS3231_STATUS_OSF 0x80
#define DS3231_REG_TEMP   0x11

/* Which of the two parts is on the bus, decided once at init.
 *
 * They share an address and a time-register layout, which is why one driver
 * has always served both -- but **the registers this file reaches beyond the
 * clock are not the same registers on the two parts**, and treating them as
 * equal is actively harmful rather than merely inaccurate:
 *
 *   0x0F : DS3231 status (OSF). On a DS1307 this is **user NVRAM** -- the
 *          part has 56 battery-backed bytes at 0x08-0x3F. Reading it returns
 *          whatever the owner stored, so a byte with bit 7 set would be
 *          reported as a stopped oscillator forever; and *clearing* OSF after
 *          a write would silently corrupt one of those bytes. That second one
 *          is data loss in someone else's data.
 *   0x11 : DS3231 temperature. NVRAM again on a DS1307, so the temperature
 *          this driver has always reported on such a board was two arbitrary
 *          bytes formatted as degrees. Pre-existing, and gated here too.
 *   0x00 bit 7 : DS1307 CH (clock halt). Unused and always 0 on a DS3231, so
 *          checking it is harmless on both and meaningful on one.
 *
 * Identified from invariants rather than from a part number, because neither
 * part has one to read. Two independent DS3231-only facts have to hold: the
 * status register's bits 6:4 are always zero, and the temperature fraction
 * byte's bits 5:0 are always zero. NVRAM satisfying both by chance is one
 * count in 2^12 per byte pair, and a DS1307 whose NVRAM happens to is left
 * doing exactly what it did before this check existed. */
static bool g_is_ds3231;

static void i2c_rtc_identify_part(void) {
    g_is_ds3231 = false;
    uint8_t st, temp[2];
    if (!i2c_read_bytes(DS1307_DS3231_I2C_ADDR, DS3231_REG_STATUS, &st, 1)) return;
    if (!i2c_read_bytes(DS1307_DS3231_I2C_ADDR, DS3231_REG_TEMP, temp, 2)) return;
    if (st & 0x70) return;          /* reserved in the DS3231's status */
    if (temp[1] & 0x3F) return;     /* only bits 7:6 of the fraction exist */
    g_is_ds3231 = true;
}

void i2c_rtc_init(void) {
    i2c_hw_init();
    i2c_rtc_identify_part();

    rtc_time_t tm;
    g_rtc_detected = false;

    /* A chip that answers but has lost its oscillator is a *detected* chip
     * with an unusable time -- a distinction the old code could not draw,
     * because it only ever asked for the time and a stopped DS3231 hands back
     * a plausible one. Detecting it here is what lets the clock be written
     * (which clears OSF) instead of the board deciding there is no RTC. */
    if (i2c_rtc_lost_power()) {
        /* The chip answered, so it is detected -- but its time is unverified
         * and does not become this system's clock. A DS3231 that lost its
         * oscillator reads 2000-01-01 00:00:00, which passes every range
         * check, so seeding from it would replace a known-unset clock with a
         * confidently wrong one. Nothing is worse to boot with.
         *
         * Not a diagnosis of the backup cell. OSF is sticky and this tree has
         * never cleared it, so on any board that has run before today it may
         * be reporting something old. Setting the clock -- by hand, from NTP,
         * or from the radio -- clears it, and from then on it means what it
         * says. */
        g_rtc_detected = true;
        printk("[I2C RTC] DS3231 at 0x68 answered, but OSF is set: its time is "
               "unverified and was not used. Set the clock to clear it "
               "(the flag is sticky and may be reporting an old event).\n");
        return;
    } else if (i2c_rtc_hw_read_time(&tm)) {
        if (tm.month >= 1 && tm.month <= 12 && tm.day >= 1 && tm.day <= 31 && tm.hour <= 23 && tm.min <= 59 && tm.sec <= 59) {
            g_rtc_detected = true;
            /* The DS3231 holds UTC, not local time (user, 2026-08-23). It is
             * storage for a clock that runs on UTC, and storing local time
             * there would make the hour that repeats every October
             * unrecoverable after a reset. A chip written by an older build
             * therefore reads an hour or two out until the next `date` or
             * (dcf-sync ... 1) rewrites it. */
            time_set_utc(&tm);
            char isostr[32];
            time_format_iso(&tm, isostr, sizeof(isostr));
#if defined(CONFIG_BOARD_RP2350)
            printk("[I2C RTC] %s detected at 0x68 (GP%d/GP%d)! Synced UTC: %s\n",
                   g_is_ds3231 ? "DS3231" : "DS1307-compatible",
                   CONFIG_I2C_RTC_SDA_GPIO, CONFIG_I2C_RTC_SCL_GPIO, isostr);
#else
            printk("[I2C RTC] %s detected at 0x68! Synced UTC: %s\n",
                   g_is_ds3231 ? "DS3231" : "DS1307-compatible", isostr);
#endif
            return;
        }
    }

    /* Two different absences, and saying which is the whole point.
     *
     * "Not found at 0x68" claims a probe. Where there is no controller
     * compiled in, i2c_probe_addr() and i2c_rtc_hw_read_time() are the stubs a
     * few hundred lines up, which return false without touching a wire, and
     * the sentence describes something that did not happen -- the same class
     * of line as the two the ESP32-P4's first boot caught in E2
     * (plan/phase27_esp32p4_bringup.md): drivers/at24c32.c announcing a "4KB
     * I2C EEPROM detected at 0x57!" from a build with no bus, and
     * drivers/usb_cdc.c naming two host device nodes from a stub.
     *
     * The gate was `#if defined(CONFIG_BOARD_RP2350)`, which was the same
     * thing as "has a controller" right up until E7 gave the P4 one -- after
     * which that board answered a full bus scan while printing "No I2C
     * controller on this target". Both sentences were true when written and
     * one of them stopped being true without changing. I2C_HAVE_CONTROLLER
     * (drivers/i2c_rtc.h) is the fact each site actually wanted.
     *
     * On the P4 the honest line still is "nothing at 0x68": that board has no
     * RTC chip at all. What it has is the LP domain's own counter and a
     * 32.768 kHz crystal, which is a real clock and nothing this driver has
     * ever heard of -- see E7 in the phase plan for why that is a different
     * device class rather than a second implementation of this one. */
    if (I2C_HAVE_CONTROLLER) {
#if defined(CONFIG_BOARD_RP2350)
        printk("[I2C RTC] No DS1307/DS3231 RTC module found at 0x68 (GP%d/GP%d); "
               "using the system software clock.\n",
               CONFIG_I2C_RTC_SDA_GPIO, CONFIG_I2C_RTC_SCL_GPIO);
#else
        printk("[I2C RTC] No DS1307/DS3231 RTC module answered at 0x68; "
               "using the system software clock.\n");
#endif
    } else {
        printk("[I2C RTC] No I2C controller on this target; the kernel clock is "
               "software-only until something sets it.\n");
    }
}

bool i2c_rtc_is_detected(void) {
    return g_rtc_detected;
}



static bool i2c_rtc_hw_read_time(rtc_time_t *tm) {
    if (!tm) return false;
    uint8_t buf[7];
    if (!i2c_read_bytes(DS1307_DS3231_I2C_ADDR, 0x00, buf, 7)) {
        return false;
    }

    /* CH is current state -- the DS1307's oscillator is halted *now* -- so a
     * read is genuinely meaningless and fails.
     *
     * OSF deliberately does NOT fail the read, and the distinction cost a
     * wrong conclusion before it was understood: **OSF is sticky**. The
     * DS3231 sets it when the oscillator stops and leaves it set until
     * software clears it, which nothing in this tree ever did -- so every
     * board here has it set, reporting an event that may be months old on a
     * chip that has kept perfect time ever since. Failing reads on it would
     * declare every working RTC in the fleet unusable.
     *
     * What it does mean is "this time has not been verified since the last
     * time the oscillator stopped", which is a caller's judgement to make:
     * i2c_rtc_init() declines to seed the *kernel* clock from it, and the
     * clock face lights a lamp. Writing the time clears the flag, so from
     * then on it means what it says. */
    if (buf[0] & 0x80) return false;              /* DS1307 CH: halted right now */

    tm->sec  = bcd2dec(buf[0] & 0x7F);
    tm->min  = bcd2dec(buf[1] & 0x7F);
    tm->hour = bcd2dec(buf[2] & 0x3F); // 24-hr mode
    // buf[3] is day of week (1-7)
    tm->day   = bcd2dec(buf[4] & 0x3F);
    tm->month = bcd2dec(buf[5] & 0x1F);
    tm->year  = 2000 + bcd2dec(buf[6]);
    tm->ms = 0;
    return true;
}

static bool i2c_rtc_hw_write_time(const rtc_time_t *tm) {
    if (!tm) return false;
    uint8_t reg_buf[8];
    reg_buf[0] = 0x00; // Register index
    reg_buf[1] = dec2bcd(tm->sec);
    reg_buf[2] = dec2bcd(tm->min);
    reg_buf[3] = dec2bcd(tm->hour);
    reg_buf[4] = 1; // Day of week default
    reg_buf[5] = dec2bcd(tm->day);
    reg_buf[6] = dec2bcd(tm->month);
    reg_buf[7] = dec2bcd((uint8_t)(tm->year >= 2000 ? (tm->year - 2000) : tm->year));

    if (!g_rtc_detected) return false;
    if (!i2c_write_bytes(DS1307_DS3231_I2C_ADDR, reg_buf, 8)) return false;

    /* Clear OSF now that the registers hold a real time again.
     *
     * The chip sets it and never clears it itself, so leaving it set would
     * make every subsequent read fail on a clock that is now correct -- the
     * flag would outlive the condition it reports. The datasheet prescribes
     * exactly this: write the time, then clear the flag.
     *
     * Best-effort: a part with no status register (a DS1307) NAKs the write
     * and the time is still set, which is the outcome that matters. */
    uint8_t st;
    if (g_is_ds3231 &&
        i2c_read_bytes(DS1307_DS3231_I2C_ADDR, DS3231_REG_STATUS, &st, 1) &&
        (st & DS3231_STATUS_OSF)) {
        uint8_t clear_buf[2] = { DS3231_REG_STATUS,
                                 (uint8_t)(st & (uint8_t)~DS3231_STATUS_OSF) };
        i2c_write_bytes(DS1307_DS3231_I2C_ADDR, clear_buf, 2);
    }
    return true;
}

static bool i2c_rtc_hw_read_temperature_c(int *temp_c) {
    if (!temp_c || !g_rtc_detected) return false;
    /* A DS1307 has no thermometer; 0x11 is two bytes of its NVRAM. This used
     * to format them as degrees. */
    if (!g_is_ds3231) return false;

    uint8_t buf[2];
    if (!i2c_read_bytes(DS1307_DS3231_I2C_ADDR, 0x11, buf, 2)) {
        return false;
    }

    /* buf[0] = signed integer part; buf[1] bits 7:6 = quarter-degree
     * fraction, always a non-negative offset from buf[0] (the DS3231's own
     * representation -- e.g. -0.25C is integer=-1, fraction=0.75, not a
     * separately-signed fraction), so no extra sign handling is needed
     * here. */
    int temp_x4 = (int)(int8_t)buf[0] * 4 + (buf[1] >> 6);
    /* Round to nearest whole degree, half-away-from-zero. */
    *temp_c = (temp_x4 >= 0) ? (temp_x4 + 2) / 4 : (temp_x4 - 2) / 4;
    return true;
}

void i2c_scan_bus(void) {
    /* An answer to a typed `i2c` command, so it goes to the console stream
     * (C0). Using printk() put the whole scan table into the kernel log
     * ring, where it turned up again in `cat /proc/kmsg`. */
#if defined(CONFIG_BOARD_RP2350)
    cprintf("\nI2C Bus Scan (GP%d SDA / GP%d SCL):\n", CONFIG_I2C_RTC_SDA_GPIO, CONFIG_I2C_RTC_SCL_GPIO);
#else
    cprintf("\nI2C Bus Scan:\n");
#endif
    cprintf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

    int found_count = 0;
    for (int row = 0; row < 128; row += 16) {
        cprintf("%02x: ", row);
        for (int col = 0; col < 16; col++) {
            uint8_t addr = row + col;
            if (addr < 0x08 || addr > 0x77) {
                cprintf("   ");
            } else {
                if (i2c_probe_addr(addr)) {
                    cprintf("%02x ", addr);
                    found_count++;
                } else {
                    cprintf("-- ");
                }
            }
        }
        cprintf("\n");
    }
    cprintf("Found %d I2C device(s).\n\n", found_count);
}

/* M4.5, plan/phase12_microkernel_migration.md, Part B: RTC and EEPROM share
 * one physical I2C bus, so this one "i2c" task serves both -- see i2c_rtc.h
 * for the fuller reasoning. Wire protocol, one opcode byte then a
 * fixed-shape payload per op; unlike drivers/spisd_rp2350.c's BLK_REQ_*
 * (a real wire onto an SD card), addr/len/temperature here are plain native
 * types, copied as-is between endpoint-owned buffers in the same address
 * space -- there is no external byte order to defend against.
 *
 *   'T' (read time)        req: [op]                    resp: [ok(1)] + rtc_time_t
 *   'S' (write time)       req: [op] + rtc_time_t        resp: [ok(1)]
 *   'C' (read temperature) req: [op]                     resp: [ok(1)] + int(4)
 *   'R' (EEPROM read)      req: [op] + addr(2) + len(2)  resp: [result:int32(4)] + data
 *   'X' (EEPROM write)     req: [op] + addr(2) + len(2) + data  resp: [result:int32(4)]
 *
 * The EEPROM ops' buffers were originally sized to the whole 4KB AT24C32
 * device; M5 Phase 3, plan/phase12_microkernel_migration.md, capped a
 * single EEPROM read/write at AT24C32_CHUNK_MAX (drivers/at24c32.h) bytes
 * instead -- far more than a syscall-sized U-mode buffer should carry, and
 * the reason i2c was deferred when tm1638 converted to U-mode in Phase 2.
 * drivers/at24c32.c's at24c32_read()/at24c32_write() loop internally in
 * chunks this size for anything larger, so this is invisible to every
 * existing caller. */
#define I2C_OP_RTC_READ_TIME  ((uint8_t)'T')
#define I2C_OP_RTC_WRITE_TIME ((uint8_t)'S')
#define I2C_OP_RTC_READ_TEMP  ((uint8_t)'C')
/* The DS3231's status register, for OSF. Its own op rather than a field
 * bolted onto the time read: the face reads the time once a second and only
 * consults the flag when deciding whether to trust it. */
#define I2C_OP_RTC_STATUS     ((uint8_t)'O')
#define I2C_OP_EE_READ        ((uint8_t)'R')
#define I2C_OP_EE_WRITE       ((uint8_t)'X')
/* Q4: the generic transfer -- write then read, for any address on this bus.
 * The last device-specific opcode this task should need: a new part
 * (drivers/bme280.c is the first) is M-mode code that builds requests, not
 * another case in here.
 *
 *   request:  'F', addr, wlen, rlen, w[wlen]
 *   response: ok, r[rlen]
 */
#define I2C_OP_XFER           ((uint8_t)'F')
#define I2C_XFER_WMAX 16u
#define I2C_XFER_RMAX 64u

#define I2C_REQ_CAP  (5u + AT24C32_CHUNK_MAX)
#define I2C_RESP_CAP (4u + AT24C32_CHUNK_MAX)

static uint8_t         g_i2c_req[I2C_REQ_CAP];
static uint8_t         g_i2c_resp[I2C_RESP_CAP];
static chan_endpoint_t *g_i2c_ep;
static int              g_i2c_task_pid = -1;

/* M4.5 verify: counts chan_call()s actually served -- see
 * drivers/uart_16550.c's g_uart_write_calls comment for the reasoning. */
static uint32_t g_i2c_calls;

uint32_t i2c_task_call_count(void) { return g_i2c_calls; }

bool i2c_task_alive(void) {
    if (g_i2c_task_pid < 0) return false;
    int st = sched_task_state(g_i2c_task_pid);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

/* --------------------------------------------------- the RTC wire ------ */

/*
 * Nine bytes, laid out explicitly:
 *
 *   [0..1] year, BIG-endian   [2] month  [3] day
 *   [4] hour   [5] min   [6] sec        [7..8] ms, big-endian
 *
 * Explicit because the U-mode server above cannot call anything outside
 * .utext and so decodes these bytes by hand -- which means the client has to
 * agree with a *byte layout*, not with a struct.
 *
 * It used to `memcpy()` an `rtc_time_t` instead, which agreed with neither
 * server. `rtc_time_t` is little-endian and carries a pad byte before `ms`,
 * so the U-mode server read the year byte-swapped: 2026 (0x07EA) arrived as
 * 0xEA07 = 59911, and `(uint8_t)(59911 - 2000)` is 55, written as BCD 0x55
 * and read back as the year 2055. Month and day, being single bytes, were
 * perfectly correct the whole time -- which is exactly what made it look like
 * a bad RTC chip rather than a bad wire format (found on hardware
 * 2026-08-23, on a clock the DCF-77 receiver had just set correctly).
 *
 * The read direction was broken differently and more quietly: the server
 * replies with 1 + 9 bytes and the client demanded 1 + sizeof(rtc_time_t) =
 * 1 + 10, so the check never passed and *every* read silently fell through to
 * direct hardware access from whatever task asked. It worked, which is why
 * nobody noticed, but it meant the display task was driving the I2C bus
 * itself and the i2c task was serving nothing.
 *
 * The lesson is the one i2c_usys_put_i32()'s comment already records from the
 * other direction: a wire format is a format, not a struct.
 */
#define RTC_WIRE_LEN 9u

static void rtc_to_wire(const rtc_time_t *tm, uint8_t *w) {
    w[0] = (uint8_t)(tm->year >> 8);
    w[1] = (uint8_t)tm->year;
    w[2] = tm->month;
    w[3] = tm->day;
    w[4] = tm->hour;
    w[5] = tm->min;
    w[6] = tm->sec;
    w[7] = (uint8_t)(tm->ms >> 8);
    w[8] = (uint8_t)tm->ms;
}

static void rtc_from_wire(const uint8_t *w, rtc_time_t *tm) {
    tm->year  = (uint16_t)(((uint16_t)w[0] << 8) | w[1]);
    tm->month = w[2];
    tm->day   = w[3];
    tm->hour  = w[4];
    tm->min   = w[5];
    tm->sec   = w[6];
    tm->ms    = (uint16_t)(((uint16_t)w[7] << 8) | w[8]);
}

/* Only RP2350 has real I2C hardware to isolate -- the #else branch below
 * (QEMU rv64/rv32) keeps the plain kernel-mode server every M4.5 driver
 * task had before M5, unchanged. Unlike drivers/uart_rp2350.c/

 * tm1638_rp2350.c (entirely separate, RP2350-only files), i2c_rtc.c is
 * shared across every target, so the split lives inside this one file. */
#if defined(CONFIG_BOARD_RP2350)

/* ---- U-mode implementation, M5 Phase 3, plan/phase12_microkernel_migration.md ----
 *
 * A second, independent implementation of the I2C register handshake
 * above (i2c_write_bytes/i2c_read_bytes) and drivers/at24c32.c's
 * (i2c_write_at24/i2c_read_at24, at24c32_hw_write()'s page-boundary
 * chunking), tagged I2C_UATTR and reachable only from the U-mode task's
 * own serve loop below -- not a refactor of the existing kernel-mode
 * ones into something shared. Those keep serving the direct-hardware
 * fallback path exactly as before, unreachable from U-mode. The two
 * copies never run concurrently -- the facade functions route to one or
 * the other depending on i2c_task_alive() -- so nothing needs to agree
 * between them beyond the wire protocol both sides already share.
 *
 * Unlike drivers/tm1638_rp2350.c's four separate bit-bang primitives,
 * RTC's and EEPROM's I2C transactions are the same DesignWare handshake
 * with different address-byte counts (RTC: 1-byte register pointer;
 * EEPROM: 2-byte memory address), so this is two generalized primitives
 * instead of four duplicated ones. */
#define I2C_UATTR __attribute__((section(".utext"))) __attribute__((no_sanitize("undefined")))

/* Hand-rolled per translation unit, not shared with drivers/uart_rp2350.c's
 * or drivers/tm1638_rp2350.c's own usys_*() stubs or user/progs/usys.h --
 * an I2C_UATTR function must not call anything the compiler might place
 * outside .utext, and a cross-file inline is not a guarantee. */
__attribute__((always_inline)) static inline void i2c_usys_delay_us(long us) {
    register long r_a0 __asm__("a0") = SYS_DELAY_US;
    register long r_a1 __asm__("a1") = us;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1) : "memory");
}
__attribute__((always_inline)) static inline long i2c_usys_chan_serve_wait(const char *name, uint8_t *buf, long buf_max) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_WAIT;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = buf_max;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}
__attribute__((always_inline)) static inline long i2c_usys_chan_serve_reply(const char *name, const uint8_t *buf, long len) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_REPLY;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = len;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}
/* Tiny one-liners, but still hand-rolled/always_inline rather than reused
 * from the kernel-mode bcd2dec()/dec2bcd() above (get_u16()/put_i32()
 * were only ever used by the kernel-mode i2c_task_body() this replaces,
 * and are gone with it) -- same reasoning as the syscall stubs, applied
 * to arithmetic helpers too:
 * "obviously inlined" is an optimizer heuristic, not a structural
 * guarantee (drivers/uart_rp2350.c's heartbeat_usleep_until() comment has
 * the fuller story of finding that out the hard way). */
__attribute__((always_inline)) static inline uint8_t i2c_usys_bcd2dec(uint8_t v) { return ((v >> 4) * 10) + (v & 0x0F); }
__attribute__((always_inline)) static inline uint8_t i2c_usys_dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
__attribute__((always_inline)) static inline uint16_t i2c_usys_get_u16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
__attribute__((always_inline)) static inline void i2c_usys_put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
/* Native (little-endian) byte order, NOT the big-endian i2c_usys_put_u16()
 * above uses -- matches the client side's get_i32() (drivers/at24c32.c),
 * which decodes via a raw memcpy() of the wire bytes into an int32_t and
 * so is native-endian by construction, same as the original kernel-mode
 * put_i32() this replaces (also a memcpy()). Found on real hardware, not
 * predicted: writing this field big-endian, matching put_u16() instead of
 * matching get_i32(), sent (eeprom-write)'s own 15-byte result back as
 * 251658240 -- the client reading 0x0F000000 where the wire held
 * 0x0000000F. */
__attribute__((always_inline)) static inline void i2c_usys_put_i32(uint8_t *p, int32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

I2C_UATTR static void i2c_usys_target(uint8_t addr) {
    REG(IC_ENABLE) = 0;
    REG(IC_TAR) = addr;
    REG(IC_ENABLE) = 1;
    (void)REG(IC_CLR_TX_ABRT);
}

/* Writes len bytes already TAR-targeted by the caller, STOP after the
 * last byte only if stop_at_end -- false is for a register/address
 * prefix that a read phase (i2c_usys_read_reg() below) continues past. */
I2C_UATTR static bool i2c_usys_write_raw(const uint8_t *data, int len, bool stop_at_end) {
    for (int i = 0; i < len; i++) {
        bool last = (i == len - 1);
        uint32_t cmd = data[i];
        if (last && stop_at_end) cmd |= (1u << 9);

        int timeout = 10000;
        while (!(REG(IC_STATUS) & (1u << 1)) && --timeout > 0);
        if (timeout == 0) return false;

        REG(IC_DATA_CMD) = cmd;

        timeout = 10000;
        bool abort = false;
        do {
            if (REG(IC_RAW_INTR_STAT) & (1u << 6)) {
                abort = true;
                (void)REG(IC_CLR_TX_ABRT);
                break;
            }
        } while (--timeout > 0 && !(REG(IC_RAW_INTR_STAT) & (1u << 4)));

        if (abort || timeout == 0) return false;
    }
    return true;
}

/* Sends len bytes to addr, STOP after the last -- RTC's register-pointer
 * + payload writes and EEPROM's 2-byte-address + payload writes alike. */
I2C_UATTR static bool i2c_usys_write_bytes(uint8_t addr, const uint8_t *data, int len) {
    i2c_usys_target(addr);
    return i2c_usys_write_raw(data, len, true);
}

/* Writes reg_len address/register bytes (no STOP), then re-targets and
 * reads len bytes -- RTC's 1-byte register reads (time at 0x00,
 * temperature at 0x11) and EEPROM's 2-byte address reads alike. */
I2C_UATTR static bool i2c_usys_read_reg(uint8_t addr, const uint8_t *reg, int reg_len, uint8_t *dst, int len) {
    i2c_usys_target(addr);
    if (!i2c_usys_write_raw(reg, reg_len, false)) return false;

    i2c_usys_target(addr);
    for (int i = 0; i < len; i++) {
        bool last = (i == len - 1);
        uint32_t cmd = (1u << 8);
        if (last) cmd |= (1u << 9);

        int timeout = 10000;
        while (!(REG(IC_STATUS) & (1u << 1)) && --timeout > 0);
        if (timeout == 0) return false;

        REG(IC_DATA_CMD) = cmd;

        timeout = 10000;
        bool abort = false;
        do {
            if (REG(IC_RAW_INTR_STAT) & (1u << 6)) {
                abort = true;
                (void)REG(IC_CLR_TX_ABRT);
                break;
            }
        } while (--timeout > 0 && (REG(IC_STATUS) & (1u << 3)) == 0);

        if (abort || timeout == 0) return false;

        dst[i] = (uint8_t)REG(IC_DATA_CMD);
    }
    return true;
}

/* EEPROM writes cross AT24C32_PAGE_SIZE (32-byte) boundaries in separate
 * transactions with a real ~10ms page-write cycle between them -- the
 * same chunking drivers/at24c32.c's at24c32_hw_write() already does, via
 * SYS_DELAY_US instead of time_delay_us() (which touches hardware no
 * U-mode domain has ever needed to be granted, and, on this build,
 * services usb_cdc_task() inline inside its own busy loop -- see
 * arch/riscv/common/trap.c's own SYS_DELAY_US comment). AT24C32_CHUNK_MAX
 * (drivers/at24c32.h) already bounds len to well under one stack frame's
 * worth of scratch space. */
I2C_UATTR static int i2c_usys_ee_write(uint16_t addr, const uint8_t *buf, int len) {
    int written = 0;
    while (written < len) {
        uint16_t curr_addr = (uint16_t)(addr + written);
        int page_offset = curr_addr % AT24C32_PAGE_SIZE;
        int chunk = AT24C32_PAGE_SIZE - page_offset;
        if (chunk > (len - written)) chunk = len - written;

        uint8_t wbuf[2 + AT24C32_PAGE_SIZE];
        wbuf[0] = (uint8_t)(curr_addr >> 8);
        wbuf[1] = (uint8_t)curr_addr;
        for (int i = 0; i < chunk; i++) wbuf[2 + i] = buf[written + i];

        if (!i2c_usys_write_bytes(AT24C32_I2C_ADDR, wbuf, 2 + chunk)) {
            return written > 0 ? written : -1;
        }
        written += chunk;
        i2c_usys_delay_us(10000);
    }
    return written;
}

I2C_UATTR static void i2c_umode_body(void) {
    /* Not a string literal: a literal lands in ordinary .rodata, outside
     * every region this task's domain grants -- the bug that hung the
     * board the first time this exact mechanism ran on real hardware in
     * M5 Phase 2 (see drivers/tm1638_rp2350.c's own comment on it).
     * volatile, for the reason kernel/shell.c's user_deputy() already
     * documents: gcc recognises a run of consecutive stores and turns it
     * back into a copy from a .rodata blob otherwise. */
    volatile char name[4];
    name[0] = 'i'; name[1] = '2'; name[2] = 'c'; name[3] = '\0';

    for (;;) {
        uint8_t req[I2C_REQ_CAP];
        long req_len = i2c_usys_chan_serve_wait((const char *)name, req, sizeof(req));
        if (req_len < 1) {
            i2c_usys_chan_serve_reply((const char *)name, NULL, 0);
            continue;
        }

        uint8_t op = req[0];
        uint8_t resp[I2C_RESP_CAP];
        uint32_t resp_len = 0;
        switch (op) {
        case I2C_OP_RTC_READ_TIME: {
            uint8_t buf[7];
            uint8_t reg = 0x00;
            bool ok = i2c_usys_read_reg(DS1307_DS3231_I2C_ADDR, &reg, 1, buf, 7);
            resp[0] = ok ? 1 : 0;
            if (ok) {
                /* Same field layout rtc_time_t's own decode uses
                 * (i2c_rtc_hw_read_time() above) -- written out field by
                 * field into the wire buffer rather than building a
                 * local rtc_time_t and copying it, so there is no
                 * struct-sized copy for the compiler to consider
                 * lowering into a call outside .utext. */
                uint8_t sec  = i2c_usys_bcd2dec(buf[0] & 0x7F);
                uint8_t min  = i2c_usys_bcd2dec(buf[1] & 0x7F);
                uint8_t hour = i2c_usys_bcd2dec(buf[2] & 0x3F);
                uint8_t day   = i2c_usys_bcd2dec(buf[4] & 0x3F);
                uint8_t month = i2c_usys_bcd2dec(buf[5] & 0x1F);
                uint16_t year = (uint16_t)(2000 + i2c_usys_bcd2dec(buf[6]));
                i2c_usys_put_u16(&resp[1], year);
                resp[3] = month; resp[4] = day; resp[5] = hour;
                resp[6] = min; resp[7] = sec;
                resp[8] = 0; resp[9] = 0; /* ms: always 0, see i2c_rtc_hw_read_time() */
                resp_len = 10;
            } else {
                resp_len = 1;
            }
            break;
        }
        case I2C_OP_RTC_WRITE_TIME: {
            bool ok = false;
            if (req_len >= 10) {
                uint16_t year = i2c_usys_get_u16(&req[1]);
                uint8_t reg_buf[8];
                reg_buf[0] = 0x00;
                reg_buf[1] = i2c_usys_dec2bcd(req[7]); /* sec */
                reg_buf[2] = i2c_usys_dec2bcd(req[6]); /* min */
                reg_buf[3] = i2c_usys_dec2bcd(req[5]); /* hour */
                reg_buf[4] = 1; /* day of week default */
                reg_buf[5] = i2c_usys_dec2bcd(req[4]); /* day */
                reg_buf[6] = i2c_usys_dec2bcd(req[3]); /* month */
                reg_buf[7] = i2c_usys_dec2bcd((uint8_t)(year >= 2000 ? (year - 2000) : year));
                ok = i2c_usys_write_bytes(DS1307_DS3231_I2C_ADDR, reg_buf, 8);

                /* Clear OSF, exactly as i2c_rtc_hw_write_time() does on the
                 * kernel-mode path. This handler writes the time registers
                 * itself rather than calling that function, so the clear had
                 * to be duplicated -- and was not, which meant that on every
                 * persona whose I2C driver runs U-mode (all the RP2350 ones)
                 * the flag was never cleared by anything. The panel's warning
                 * lamp therefore stayed lit for the life of the board, and no
                 * amount of setting the clock put it out.
                 *
                 * Only when the caller says this is a DS3231: on a DS1307
                 * 0x0F is user NVRAM. The facade knows which part is present
                 * and says so in the request, because this task cannot read
                 * the driver's globals from inside its own domain. */
                if (ok && req_len >= (long)(1 + RTC_WIRE_LEN + 1) && req[1 + RTC_WIRE_LEN]) {
                    uint8_t st = 0, sreg = DS3231_REG_STATUS;
                    if (i2c_usys_read_reg(DS1307_DS3231_I2C_ADDR, &sreg, 1, &st, 1) &&
                        (st & DS3231_STATUS_OSF)) {
                        uint8_t clr[2] = { DS3231_REG_STATUS,
                                           (uint8_t)(st & (uint8_t)~DS3231_STATUS_OSF) };
                        i2c_usys_write_bytes(DS1307_DS3231_I2C_ADDR, clr, 2);
                    }
                }
            }
            resp[0] = ok ? 1 : 0;
            resp_len = 1;
            break;
        }
        case I2C_OP_RTC_STATUS: {
            uint8_t st = 0, sreg = DS3231_REG_STATUS;
            bool ok = i2c_usys_read_reg(DS1307_DS3231_I2C_ADDR, &sreg, 1, &st, 1);
            resp[0] = ok ? 1 : 0;
            resp[1] = st;
            resp_len = 2;
            break;
        }
        case I2C_OP_RTC_READ_TEMP: {
            uint8_t buf[2];
            uint8_t reg = 0x11;
            bool ok = i2c_usys_read_reg(DS1307_DS3231_I2C_ADDR, &reg, 1, buf, 2);
            resp[0] = ok ? 1 : 0;
            if (ok) {
                int temp_x4 = (int)(int8_t)buf[0] * 4 + (buf[1] >> 6);
                int temp_c = (temp_x4 >= 0) ? (temp_x4 + 2) / 4 : (temp_x4 - 2) / 4;
                i2c_usys_put_i32(&resp[1], (int32_t)temp_c);
                resp_len = 5;
            } else {
                resp_len = 1;
            }
            break;
        }
        case I2C_OP_EE_READ: {
            int32_t result = -1;
            if (req_len >= 5) {
                uint16_t addr = i2c_usys_get_u16(&req[1]);
                uint16_t len  = i2c_usys_get_u16(&req[3]);
                if (len <= AT24C32_CHUNK_MAX) {
                    uint8_t reg[2] = { (uint8_t)(addr >> 8), (uint8_t)addr };
                    if (i2c_usys_read_reg(AT24C32_I2C_ADDR, reg, 2, &resp[4], len)) {
                        result = (int32_t)len;
                    }
                }
            }
            i2c_usys_put_i32(&resp[0], result);
            resp_len = (result > 0) ? 4u + (uint32_t)result : 4u;
            break;
        }
        case I2C_OP_EE_WRITE: {
            int32_t result = -1;
            if (req_len >= 5) {
                uint16_t addr = i2c_usys_get_u16(&req[1]);
                uint16_t len  = i2c_usys_get_u16(&req[3]);
                if (len <= AT24C32_CHUNK_MAX && (uint32_t)req_len >= 5u + (uint32_t)len) {
                    result = (int32_t)i2c_usys_ee_write(addr, &req[5], (int)len);
                }
            }
            i2c_usys_put_i32(&resp[0], result);
            resp_len = 4;
            break;
        }
        case I2C_OP_XFER: {
            /* i2c_usys_read_reg() already takes a write length, so the
             * generic op maps straight onto it with nothing new in U-mode. */
            bool ok = false;
            uint32_t rlen = 0;
            if (req_len >= 4) {
                uint8_t addr = req[1];
                uint32_t wlen = req[2];
                rlen = req[3];
                if (wlen <= I2C_XFER_WMAX && rlen <= I2C_XFER_RMAX &&
                    (long)(4u + wlen) <= req_len) {
                    if (rlen) {
                        ok = i2c_usys_read_reg(addr, &req[4], (int)wlen,
                                               &resp[1], (int)rlen);
                    } else {
                        ok = i2c_usys_write_bytes(addr, &req[4], (int)wlen);
                    }
                }
            }
            if (!ok) rlen = 0;
            resp[0] = ok ? 1 : 0;
            resp_len = 1u + rlen;
            break;
        }
        default:
            resp_len = 0;
            break;
        }
        i2c_usys_chan_serve_reply((const char *)name, resp, resp_len);
    }
}

/* M5 heap-reclaim, plan/phase12_microkernel_migration.md: 512 bytes, not
 * 4096 -- i2c_umode_body()'s deepest call chain (through
 * i2c_usys_read_reg -> i2c_usys_target/i2c_usys_write_raw) measures 384
 * bytes on the real disassembly. See drivers/uart_rp2350.c's
 * g_heartbeat_ustack comment and .ustacks512's in linker/rp2350.ld. */
static uint8_t      g_i2c_ustack[512] __attribute__((aligned(512)))
                                       __attribute__((section(".ustacks512")));
static mem_domain_t g_i2c_domain;


/* This task's own kernel-mode entry point: task_create_sized() calls this
 * (ordinary kernel stack, kernel privilege) to build the domain and make
 * the one-way jump into U-mode. Mirrors drivers/tm1638_rp2350.c's
 * tm1638_task_body() shape: the same 3-region domain (own stack, the
 * shared .utext page, one MMIO window -- I2C_RTC_BASE's controller
 * registers here instead of SIO), the same refuse-rather-than-claim-
 * unverified-isolation rule. */
static void i2c_task_body(void *arg) {
    (void)arg;
    while (!g_i2c_ep) sched_yield();

    mem_domain_init(&g_i2c_domain);
    mem_domain_add(&g_i2c_domain, (uintptr_t)g_i2c_ustack, sizeof(g_i2c_ustack),
                   MEM_R | MEM_W);

    uintptr_t tbase, tsize;
    board_text_region(&tbase, &tsize);
    mem_domain_add(&g_i2c_domain, tbase, tsize, MEM_R | MEM_X);

    mem_domain_add(&g_i2c_domain, I2C_RTC_BASE, 4096, MEM_R | MEM_W);

    if (task_set_domain(sched_current_pid(), &g_i2c_domain) != 0) {
        printk("[I2C] Refusing to enter U-mode: memory domain not enforceable; RTC/EEPROM stay on direct hardware access.\n");
        return;
    }
    arch_enter_user(i2c_umode_body, (uintptr_t)g_i2c_ustack + sizeof(g_i2c_ustack), 0, 0, 0);
}

#else /* !CONFIG_BOARD_RP2350: plain kernel-mode server, as every M4.5
       * driver task had it before M5 -- no real I2C hardware to isolate
       * on QEMU, so no reason to build a domain for it. */

/* This task, and only this task, may call the *_hw_* functions in this file
 * and drivers/at24c32.c while alive -- see uart_16550.c's uart_task_body()
 * for the fuller reasoning (never call back into anything that could
 * chan_call() this same endpoint; never take printk_lock() from here).
 *
 * Both of those are checked since phase 31: chan_call() refuses a call that
 * would close a wait-for cycle, and printk ownership became an edge in that
 * same graph in Y4. See kernel/lock.h. */
static void i2c_task_body(void *arg) {
    (void)arg;
    while (!g_i2c_ep) sched_yield();

    for (;;) {
        uint32_t req_len = chan_serve_wait(g_i2c_ep);
        if (req_len < 1) { chan_serve_reply(g_i2c_ep, 0); continue; }

        uint8_t op = g_i2c_req[0];
        switch (op) {
        case I2C_OP_RTC_READ_TIME: {
            rtc_time_t tm;
            bool ok = i2c_rtc_hw_read_time(&tm);
            g_i2c_resp[0] = ok ? 1 : 0;
            if (ok) rtc_to_wire(&tm, &g_i2c_resp[1]);
            chan_serve_reply(g_i2c_ep, ok ? 1u + RTC_WIRE_LEN : 1u);
            break;
        }
        case I2C_OP_RTC_WRITE_TIME: {
            bool ok = false;
            if (req_len >= 1 + RTC_WIRE_LEN) {
                rtc_time_t tm;
                rtc_from_wire(&g_i2c_req[1], &tm);
                ok = i2c_rtc_hw_write_time(&tm);
            }
            g_i2c_resp[0] = ok ? 1 : 0;
            chan_serve_reply(g_i2c_ep, 1);
            break;
        }
        case I2C_OP_RTC_STATUS: {
            uint8_t st = 0;
            bool ok = i2c_read_bytes(DS1307_DS3231_I2C_ADDR, DS3231_REG_STATUS, &st, 1);
            g_i2c_resp[0] = ok ? 1 : 0;
            g_i2c_resp[1] = st;
            chan_serve_reply(g_i2c_ep, 2);
            break;
        }
        case I2C_OP_RTC_READ_TEMP: {
            int temp_c = 0;
            bool ok = i2c_rtc_hw_read_temperature_c(&temp_c);
            g_i2c_resp[0] = ok ? 1 : 0;
            {
                int32_t v = (int32_t)temp_c;
                memcpy(&g_i2c_resp[1], &v, sizeof(v)); /* native byte order -- matches get_i32() */
            }
            chan_serve_reply(g_i2c_ep, ok ? 5u : 1u);
            break;
        }
        case I2C_OP_XFER: {
            bool ok = false;
            uint32_t rlen = 0;
            if (req_len >= 4) {
                uint8_t addr = g_i2c_req[1];
                uint32_t wlen = g_i2c_req[2];
                rlen = g_i2c_req[3];
                if (wlen <= I2C_XFER_WMAX && rlen <= I2C_XFER_RMAX &&
                    (uint32_t)req_len >= 4u + wlen) {
                    ok = i2c_xfer_raw(addr, &g_i2c_req[4], (int)wlen,
                                      &g_i2c_resp[1], (int)rlen);
                }
            }
            if (!ok) rlen = 0;
            g_i2c_resp[0] = ok ? 1 : 0;
            chan_serve_reply(g_i2c_ep, 1 + rlen);
            break;
        }
        case I2C_OP_EE_READ: {
            int32_t result = -1;
            if (req_len >= 5) {
                uint16_t addr = ((uint16_t)g_i2c_req[1] << 8) | g_i2c_req[2];
                uint16_t len  = ((uint16_t)g_i2c_req[3] << 8) | g_i2c_req[4];
                if (len <= AT24C32_CHUNK_MAX) {
                    result = (int32_t)at24c32_hw_read(addr, &g_i2c_resp[4], len);
                }
            }
            memcpy(&g_i2c_resp[0], &result, sizeof(result)); /* native byte order -- matches get_i32() */
            chan_serve_reply(g_i2c_ep, (result > 0) ? 4u + (uint32_t)result : 4u);
            break;
        }
        case I2C_OP_EE_WRITE: {
            int32_t result = -1;
            if (req_len >= 5) {
                uint16_t addr = ((uint16_t)g_i2c_req[1] << 8) | g_i2c_req[2];
                uint16_t len  = ((uint16_t)g_i2c_req[3] << 8) | g_i2c_req[4];
                if (len <= AT24C32_CHUNK_MAX && req_len >= 5u + len) {
                    result = (int32_t)at24c32_hw_write(addr, &g_i2c_req[5], len);
                }
            }
            memcpy(&g_i2c_resp[0], &result, sizeof(result)); /* native byte order -- matches get_i32() */
            chan_serve_reply(g_i2c_ep, 4);
            break;
        }
        default:
            chan_serve_reply(g_i2c_ep, 0);
            break;
        }
    }
}

#endif /* CONFIG_BOARD_RP2350 */


/* Called from kernel/main.c, after sched_init(). Not fatal if it fails:
 * i2c_rtc_read_time()/write_time()/read_temperature_c() and
 * at24c32_read()/write() all fall back to direct hardware access whenever
 * the task is not alive, same as every boot-time read before this ever
 * ran. */
int i2c_task_start(void) {
    int pid = task_create_driver("i2c", i2c_task_body, NULL, 1);
    if (pid < 0) {
        printk("[I2C] Could not start the i2c task; RTC/EEPROM stay on direct hardware access.\n");
        return -1;
    }
    if (chan_register_task("i2c", pid, g_i2c_req, sizeof(g_i2c_req),
                           g_i2c_resp, sizeof(g_i2c_resp)) != 0) {
        printk("[I2C] Could not register the i2c channel endpoint; falling back to direct hardware access.\n");
        return -1;
    }
    g_i2c_ep = chan_lookup("i2c");
    g_i2c_task_pid = pid;
    printk("[I2C] Driver running as task #%d, reachable via chan_call(\"i2c\", ...)\n", pid);
    return pid;
}

int i2c_task_call(const uint8_t *req, uint32_t req_len, uint8_t *resp, uint32_t resp_max) {
    for (int attempt = 0; attempt < 8; attempt++) {
        int n = chan_call(g_i2c_ep, req, req_len, resp, resp_max);
        /* M5 Phase 3: counted here, on the client side -- see
         * drivers/tm1638_rp2350.c's tm1638_call_with_retry() comment,
         * same reasoning: a U-mode server cannot touch g_i2c_calls, an
         * ordinary kernel .bss global no domain grants it. */
        if (n >= 0) { g_i2c_calls++; return n; }
        sched_yield();
    }
    return -1;
}

#if defined(CONFIG_BOARD_RP2350)
/* M5 Phase 3's own "Verify" deliverable: does the real i2c domain shape
 * (stack + .utext + a 4096-byte I2C_RTC_BASE window) actually confine the
 * task to the I2C controller, or does the grant's width accidentally
 * cover more? Modeled directly on drivers/tm1638_rp2350.c's
 * tm1638_isolation_test() -- same idea (a deliberate out-of-domain
 * store, asserted to fault), a separate canary rather than reaching into
 * another file's, for the same reason the syscall stubs above are
 * hand-rolled per file. Only meaningful where the "i2c" task actually runs
 * in U-mode -- see this file's own #if defined(CONFIG_BOARD_RP2350) split
 * above. */
static volatile uintptr_t g_i2c_canary = 0xC0FFEE;

I2C_UATTR static void i2c_intruder(void) {
    g_i2c_canary = 0xDEAD;
    for (;;) { } /* only reached if the store was NOT stopped */
}

static volatile bool g_i2c_intruder_entered;

/* `arg` is the U-mode stack -- allocated by i2c_isolation_test() below,
 * not here, so it can free it once the task is confirmed DEAD (same
 * shape as tm1638_isolation_test()'s own probe). */
static void i2c_intruder_task_body(void *arg) {
    uint8_t *ustack = (uint8_t *)arg;
    mem_domain_t dom;
    mem_domain_init(&dom);
    mem_domain_add(&dom, (uintptr_t)ustack, 4096, MEM_R | MEM_W);
    uintptr_t tbase, tsize;
    board_text_region(&tbase, &tsize);
    mem_domain_add(&dom, tbase, tsize, MEM_R | MEM_X);
    /* The exact grant real i2c runs under -- this is what's on trial. */
    mem_domain_add(&dom, I2C_RTC_BASE, 4096, MEM_R | MEM_W);

    if (task_set_domain(sched_current_pid(), &dom) != 0) {
        printk("[I2CIso] Refusing to enter U-mode: memory domain not enforceable\n");
        return;
    }
    g_i2c_intruder_entered = true;
    arch_enter_user(i2c_intruder, (uintptr_t)ustack + 4096, 0, 0, 0);
}

bool i2c_isolation_test(uintptr_t *out_canary, bool *out_exited_clean) {
    g_i2c_canary = 0xC0FFEE;
    g_i2c_intruder_entered = false;

    void *ustack = palloc_pages(1);
    if (!ustack) {
        *out_canary = g_i2c_canary;
        *out_exited_clean = true;
        return false;
    }

    int pid = task_create("i2c_intruder", i2c_intruder_task_body, ustack);
    if (pid < 0) {
        palloc_free(ustack, 1);
        *out_canary = g_i2c_canary;
        *out_exited_clean = true;
        return false;
    }
    for (int i = 0; i < 10000 && sched_task_state(pid) != TASK_DEAD; i++) {
        sched_yield();
    }
    long status;
    *out_exited_clean = sched_task_exited_cleanly(pid, &status);
    *out_canary = g_i2c_canary;
    palloc_free(ustack, 1);
    return g_i2c_intruder_entered;
}
#endif /* CONFIG_BOARD_RP2350 */

bool i2c_rtc_read_time(rtc_time_t *tm) {
    if (!tm) return false;
    if (i2c_task_alive()) {
        uint8_t req[1] = { I2C_OP_RTC_READ_TIME };
        uint8_t resp[1 + RTC_WIRE_LEN];
        int n = i2c_task_call(req, sizeof(req), resp, sizeof(resp));
        if (n >= 1) {
            if (resp[0] == 0) return false;
            if ((uint32_t)n >= 1 + RTC_WIRE_LEN) {
                rtc_from_wire(&resp[1], tm);
                return true;
            }
        }
        /* IPC failed -- fall through to direct access. */
    }
    return i2c_rtc_hw_read_time(tm);
}

bool i2c_rtc_lost_power(void) {
    /* DS3231 only. On a DS1307 the address is user NVRAM and the answer would
     * be whatever the owner put there. */
    if (!g_is_ds3231) return false;

    /* Through the driver task once it exists, like every other public
     * accessor in this file.
     *
     * This used to touch the bus directly, breaking the rule stated above
     * i2c_rtc_hw_read_time(): only i2c_rtc_init() (before the task exists)
     * and the task itself may do that. Called from the clock application's
     * own task it raced the driver for the bus. A rule only some callers
     * follow is not a rule. */
    if (i2c_task_alive()) {
        uint8_t req[1] = { I2C_OP_RTC_STATUS };
        uint8_t resp[2];
        int n = i2c_task_call(req, sizeof(req), resp, sizeof(resp));
        if (n >= 2) return resp[0] && (resp[1] & DS3231_STATUS_OSF);
        /* IPC failed -- fall through, as every other accessor here does. */
    }
    uint8_t st;
    if (!i2c_read_bytes(DS1307_DS3231_I2C_ADDR, DS3231_REG_STATUS, &st, 1)) return false;
    return (st & DS3231_STATUS_OSF) != 0;
}

bool i2c_rtc_write_time(const rtc_time_t *tm) {
    if (!tm) return false;
    if (i2c_task_alive()) {
        uint8_t req[1 + RTC_WIRE_LEN + 1];
        req[0] = I2C_OP_RTC_WRITE_TIME;
        rtc_to_wire(tm, &req[1]);
        /* Whether the task may touch 0x0F to clear OSF. Decided here because
         * the driver's globals are readable on this side and not inside the
         * task's domain. */
        req[1 + RTC_WIRE_LEN] = g_is_ds3231 ? 1u : 0u;
        uint8_t resp[1];
        int n = i2c_task_call(req, sizeof(req), resp, sizeof(resp));
        if (n >= 1) return resp[0] != 0;
        /* IPC failed -- fall through to direct access. */
    }
    return i2c_rtc_hw_write_time(tm);
}

/* Q4: the generic transfer, as every other public facade in this file works
 * -- through the shared task when it is alive, straight at the hardware when
 * it is not (during boot, before sched_init(), or on a target with no task).
 * That is what lets drivers/bme280.c be ordinary M-mode code with no
 * knowledge of the task at all. */
bool i2c_xfer(uint8_t addr, const uint8_t *w, uint32_t wlen,
              uint8_t *r, uint32_t rlen) {
    if (wlen > I2C_XFER_WMAX || rlen > I2C_XFER_RMAX) return false;
    if ((wlen && !w) || (rlen && !r)) return false;

    if (i2c_task_alive()) {
        uint8_t req[4 + I2C_XFER_WMAX];
        uint8_t resp[1 + I2C_XFER_RMAX];
        req[0] = I2C_OP_XFER;
        req[1] = addr;
        req[2] = (uint8_t)wlen;
        req[3] = (uint8_t)rlen;
        if (wlen) memcpy(&req[4], w, wlen);
        int n = i2c_task_call(req, 4u + wlen, resp, sizeof(resp));
        if (n < 1 || resp[0] != 1) return false;
        if (rlen) {
            if ((uint32_t)n < 1u + rlen) return false;
            memcpy(r, &resp[1], rlen);
        }
        return true;
    }
    return i2c_xfer_raw(addr, w, (int)wlen, r, (int)rlen);
}

/* The last successful temperature reading, and when it was taken.
 *
 * A cache rather than a second reader, because the interesting consumer is
 * /proc/clock and that is served by the 9P task -- which has no business
 * touching the I2C bus. Whoever already owns the bus refreshes this; readers
 * get a value and its age and can judge for themselves whether it is stale.
 *
 * The age matters as much as the value: a temperature from an hour ago is
 * evidence about an hour ago, and correlating a crystal's rate against a stale
 * reading is how a spurious tempco gets published. */
static int      g_temp_cached_c;
static bool     g_temp_cached_ok;
static uint64_t g_temp_cached_ms;

bool i2c_rtc_cached_temperature_c(int *temp_c, uint32_t *age_s) {
    if (!g_temp_cached_ok) return false;
    if (temp_c) *temp_c = g_temp_cached_c;
    if (age_s) {
        uint64_t now = time_get_ms();
        *age_s = (uint32_t)((now > g_temp_cached_ms ? now - g_temp_cached_ms : 0) / 1000u);
    }
    return true;
}

static bool temp_cache(bool ok, int v) {
    if (ok) { g_temp_cached_c = v; g_temp_cached_ok = true; g_temp_cached_ms = time_get_ms(); }
    return ok;
}

bool i2c_rtc_read_temperature_c(int *temp_c) {
    if (!temp_c) return false;
    if (i2c_task_alive()) {
        uint8_t req[1] = { I2C_OP_RTC_READ_TEMP };
        uint8_t resp[5];
        int n = i2c_task_call(req, sizeof(req), resp, sizeof(resp));
        if (n >= 1) {
            if (resp[0] == 0) return false;
            if ((uint32_t)n >= 5) {
                int32_t v;
                memcpy(&v, &resp[1], sizeof(v));
                *temp_c = (int)v;
                return temp_cache(true, *temp_c);
            }
        }
        /* IPC failed -- fall through to direct access. */
    }
    return temp_cache(i2c_rtc_hw_read_temperature_c(temp_c), *temp_c);
}
