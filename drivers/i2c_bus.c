#include "drivers/i2c_bus.h"
#include "drivers/driver_task.h"
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
#include "arch/rp2350_clocks.h"
#include <string.h>

/* See drivers/include/drivers/i2c_bus.h for what this file is and why it is
 * not part of the RTC driver any more. */

#if defined(CONFIG_BOARD_RP2350)
/* Two I2C buses on RP2350.
 *
 * Bus 0 is the board's own bus, where the board file puts it
 * (CONFIG_I2C_RTC_BASE/SDA/SCL -- names historical): I2C0 on GP4/GP5 on most
 * personas, but I2C1 on GP6/GP7 on rp2350-clock (the Pico-Clock-Green's
 * DS3231) and rp2350-terminal. The RTC and the EEPROM sit on it and only
 * ever use bus 0, so it must stay wherever the board file says.
 *
 * Bus 1 is I2C1 on GP6/GP7, and exists only where those pins are free: bus 0
 * must be I2C0 (otherwise I2C1 already *is* bus 0), and the TM1638 must not
 * have GP6/GP7 (rp2350-chess). Everywhere else there is one bus. */
#define I2C0_BASE              0x40090000UL
#define I2C1_BASE              0x40098000UL

#define I2C_BUS0_BASE          ((uintptr_t)CONFIG_I2C_RTC_BASE)
#define I2C_BUS0_SDA           CONFIG_I2C_RTC_SDA_GPIO
#define I2C_BUS0_SCL           CONFIG_I2C_RTC_SCL_GPIO
#if CONFIG_I2C_RTC_BASE == 0x40098000UL
#define I2C_BUS0_RESET_BIT     (1u << 5) // RESETS_RESET_I2C1
#define I2C_BUS0_ACCESSCTRL    (ACCESSCTRL_BASE + 0x88) // ACCESSCTRL_I2C1
#else
#define I2C_BUS0_RESET_BIT     (1u << 4) // RESETS_RESET_I2C0
#define I2C_BUS0_ACCESSCTRL    (ACCESSCTRL_BASE + 0x84) // ACCESSCTRL_I2C0
#endif

#if CONFIG_I2C_RTC_BASE == 0x40090000UL && !(defined(CONFIG_ENABLE_TM1638) && CONFIG_ENABLE_TM1638)
#define RP2350_I2C1_AVAILABLE  1
#define I2C_BUS1_BASE          I2C1_BASE
#define I2C_BUS1_SDA           6
#define I2C_BUS1_SCL           7
#define I2C_BUS1_RESET_BIT     (1u << 5) // RESETS_RESET_I2C1
#define I2C_BUS1_ACCESSCTRL    (ACCESSCTRL_BASE + 0x88) // ACCESSCTRL_I2C1
#else
#define RP2350_I2C1_AVAILABLE  0
#define I2C_BUS1_BASE          I2C_BUS0_BASE
#endif

#define RESETS_BASE            0x40020000UL
#define RESETS_RESET           (RESETS_BASE + 0x0000)
#define RESETS_RESET_SET       (RESETS_BASE + 0x2000)
#define RESETS_RESET_CLR       (RESETS_BASE + 0x3000)
#define RESETS_RESET_DONE      (RESETS_BASE + 0x0008)

#define ACCESSCTRL_BASE        0x40060000UL
#define ACCESSCTRL_I2C_NSP     (1u << 1)
#define ACCESSCTRL_I2C_NSU     (1u << 0)
#define ACCESSCTRL_WRITE_PASSWORD 0xacce0000UL

#define IO_BANK0_BASE          0x40028000UL
#define IO_BANK0_CTRL(n)       (IO_BANK0_BASE + 0x004 + (n) * 8)

#define PADS_BANK0_BASE        0x40038000UL
#define PADS_BANK0_PAD(n)      (PADS_BANK0_BASE + 0x004 + (n) * 4)

#define IC_CON_OFFSET          0x00
#define IC_TAR_OFFSET          0x04
#define IC_DATA_CMD_OFFSET     0x10
#define IC_SS_SCL_HCNT_OFFSET  0x14
#define IC_SS_SCL_LCNT_OFFSET  0x18
#define IC_FS_SCL_HCNT_OFFSET  0x1C
#define IC_FS_SCL_LCNT_OFFSET  0x20
#define IC_INTR_STAT_OFFSET    0x2C
#define IC_RAW_INTR_STAT_OFFSET 0x34
#define IC_CLR_TX_ABRT_OFFSET  0x54
#define IC_CLR_STOP_DET_OFFSET 0x60
#define IC_ENABLE_OFFSET       0x6C
#define IC_STATUS_OFFSET       0x70
#define IC_TXFLR_OFFSET        0x74
#define IC_RXFLR_OFFSET        0x78
#define IC_SDA_HOLD_OFFSET     0x7C
#define IC_TX_ABRT_SOURCE_OFFSET 0x80
#define IC_FS_SPKLEN_OFFSET    0xA0

#define IC_RAW_STOP_DET        (1u << 9)

#define I2C_RTC_BASE           ((uintptr_t)CONFIG_I2C_RTC_BASE)
#define IC_CON                 (I2C_RTC_BASE + IC_CON_OFFSET)
#define IC_TAR                 (I2C_RTC_BASE + IC_TAR_OFFSET)
#define IC_DATA_CMD            (I2C_RTC_BASE + IC_DATA_CMD_OFFSET)
#define IC_SS_SCL_HCNT         (I2C_RTC_BASE + IC_SS_SCL_HCNT_OFFSET)
#define IC_SS_SCL_LCNT         (I2C_RTC_BASE + IC_SS_SCL_LCNT_OFFSET)
#define IC_FS_SCL_HCNT         (I2C_RTC_BASE + IC_FS_SCL_HCNT_OFFSET)
#define IC_FS_SCL_LCNT         (I2C_RTC_BASE + IC_FS_SCL_LCNT_OFFSET)
#define IC_INTR_STAT           (I2C_RTC_BASE + IC_INTR_STAT_OFFSET)
#define IC_RAW_INTR_STAT       (I2C_RTC_BASE + IC_RAW_INTR_STAT_OFFSET)
#define IC_CLR_TX_ABRT         (I2C_RTC_BASE + IC_CLR_TX_ABRT_OFFSET)
#define IC_CLR_STOP_DET        (I2C_RTC_BASE + IC_CLR_STOP_DET_OFFSET)
#define IC_ENABLE              (I2C_RTC_BASE + IC_ENABLE_OFFSET)
#define IC_STATUS              (I2C_RTC_BASE + IC_STATUS_OFFSET)
#define IC_TXFLR               (I2C_RTC_BASE + IC_TXFLR_OFFSET)
#define IC_RXFLR               (I2C_RTC_BASE + IC_RXFLR_OFFSET)
#define IC_SDA_HOLD            (I2C_RTC_BASE + IC_SDA_HOLD_OFFSET)
#define IC_TX_ABRT_SOURCE      (I2C_RTC_BASE + IC_TX_ABRT_SOURCE_OFFSET)
#define IC_FS_SPKLEN           (I2C_RTC_BASE + IC_FS_SPKLEN_OFFSET)

#define REG(addr) (*(volatile uint32_t *)(addr))

static inline uintptr_t rp2350_i2c_base(uint8_t bus) {
    return (bus == 1) ? I2C_BUS1_BASE : I2C_BUS0_BASE;
}

static void rp2350_i2c_init_controller(uintptr_t base) {
    REG(base + IC_ENABLE_OFFSET) = 0;
    /* Master mode (bit 0), 7-bit addressing, Fast mode (2u << 1), Restart enable (bit 6), Slave disable (bit 5), TX_EMPTY_CTRL (bit 8) */
    REG(base + IC_CON_OFFSET) = (1u << 0) | (1u << 5) | (2u << 1) | (1u << 6) | (1u << 8);
    REG(base + IC_TAR_OFFSET) = 0x00;

    /* 100kHz Standard Mode clock dividers from clk_sys (CONFIG_CLK_SYS_HZ) */
    uint32_t freq_in = CONFIG_CLK_SYS_HZ;
    uint32_t baudrate = 100000; // 100 kHz
    uint32_t period = (freq_in + baudrate / 2) / baudrate;
    uint32_t lcnt = period * 3 / 5;
    uint32_t hcnt = period - lcnt;

    REG(base + IC_FS_SCL_HCNT_OFFSET) = hcnt;
    REG(base + IC_FS_SCL_LCNT_OFFSET) = lcnt;
    REG(base + IC_SS_SCL_HCNT_OFFSET) = hcnt;
    REG(base + IC_SS_SCL_LCNT_OFFSET) = lcnt;
    REG(base + IC_FS_SPKLEN_OFFSET) = lcnt < 16 ? 1 : lcnt / 16;

    /* 300ns SDA Hold Time matching Pico SDK */
    uint32_t sda_tx_hold_count = ((freq_in * 3) / 10000000) + 1;
    REG(base + IC_SDA_HOLD_OFFSET) = sda_tx_hold_count;

    REG(base + IC_ENABLE_OFFSET) = 1;
}

void i2c_bus_init(void) {
    /* 1. Assert and clear the controllers' resets */
    uint32_t rst_mask = I2C_BUS0_RESET_BIT;
#if RP2350_I2C1_AVAILABLE
    rst_mask |= I2C_BUS1_RESET_BIT;
#endif
    REG(RESETS_RESET_SET) = rst_mask;
    for (volatile int i = 0; i < 1000; i++);
    REG(RESETS_RESET_CLR) = rst_mask;
    int timeout = 10000;
    while ((REG(RESETS_RESET_DONE) & rst_mask) != rst_mask && --timeout > 0);

    /* M5 Phase 3: the controllers need to be Non-secure-accessible for the
     * U-mode task's serve loop to touch them -- from M-mode, before the task
     * exists. Every ACCESSCTRL register except GPIO_NSMASK0/1 needs 0xacce
     * in the write's upper 16 bits, or the write bus-faults. */
    REG(I2C_BUS0_ACCESSCTRL) = ACCESSCTRL_WRITE_PASSWORD | REG(I2C_BUS0_ACCESSCTRL)
                             | ACCESSCTRL_I2C_NSP | ACCESSCTRL_I2C_NSU;
#if RP2350_I2C1_AVAILABLE
    REG(I2C_BUS1_ACCESSCTRL) = ACCESSCTRL_WRITE_PASSWORD | REG(I2C_BUS1_ACCESSCTRL)
                             | ACCESSCTRL_I2C_NSP | ACCESSCTRL_I2C_NSU;
#endif

    /* 2. Pins as Function 3 (I2C); pull-ups, input enable, Schmitt (0x5A) */
    REG(IO_BANK0_CTRL(I2C_BUS0_SDA)) = 3;
    REG(IO_BANK0_CTRL(I2C_BUS0_SCL)) = 3;
    REG(PADS_BANK0_PAD(I2C_BUS0_SDA)) = 0x5A;
    REG(PADS_BANK0_PAD(I2C_BUS0_SCL)) = 0x5A;
#if RP2350_I2C1_AVAILABLE
    REG(IO_BANK0_CTRL(I2C_BUS1_SDA)) = 3;
    REG(IO_BANK0_CTRL(I2C_BUS1_SCL)) = 3;
    REG(PADS_BANK0_PAD(I2C_BUS1_SDA)) = 0x5A;
    REG(PADS_BANK0_PAD(I2C_BUS1_SCL)) = 0x5A;
#endif

    /* 3. Timing & master mode */
    rp2350_i2c_init_controller(I2C_BUS0_BASE);
#if RP2350_I2C1_AVAILABLE
    rp2350_i2c_init_controller(I2C_BUS1_BASE);
#endif
}

static uint32_t g_rp2350_last_abrt_source;

static bool rp2350_i2c_write_bytes_stop(uintptr_t base, uint8_t addr, const uint8_t *src, int len, bool stop) {
    REG(base + IC_ENABLE_OFFSET) = 0;
    REG(base + IC_TAR_OFFSET) = addr;
    REG(base + IC_ENABLE_OFFSET) = 1;
    (void)REG(base + IC_CLR_TX_ABRT_OFFSET);

    bool abort = false;
    for (int i = 0; i < len; i++) {
        bool last = (i == len - 1);
        uint32_t cmd = src[i];
        if (last && stop) cmd |= (1u << 9); // STOP bit

        int timeout = 10000;
        while (!(REG(base + IC_STATUS_OFFSET) & (1u << 1)) && --timeout > 0);
        if (timeout == 0) return false;

        REG(base + IC_DATA_CMD_OFFSET) = cmd;

        timeout = 10000;
        do {
            if (REG(base + IC_RAW_INTR_STAT_OFFSET) & (1u << 6)) {
                abort = true;
                g_rp2350_last_abrt_source = REG(base + IC_TX_ABRT_SOURCE_OFFSET);
                (void)REG(base + IC_CLR_TX_ABRT_OFFSET);
                break;
            }
        } while (--timeout > 0 && !(REG(base + IC_RAW_INTR_STAT_OFFSET) & (1u << 4)));

        if (abort || timeout == 0) return false;
    }
    return !abort;
}

static bool rp2350_i2c_read_phase_cont(uintptr_t base, uint8_t addr, uint8_t *dst, int len, bool continuing) {
    if (!continuing) {
        REG(base + IC_ENABLE_OFFSET) = 0;
        REG(base + IC_TAR_OFFSET) = addr;
        REG(base + IC_ENABLE_OFFSET) = 1;
        (void)REG(base + IC_CLR_TX_ABRT_OFFSET);
    }

    bool abort = false;
    for (int i = 0; i < len; i++) {
        bool last = (i == len - 1);
        uint32_t cmd = (1u << 8); // READ bit
        if (last) cmd |= (1u << 9); // STOP bit

        int timeout = 10000;
        while (!(REG(base + IC_STATUS_OFFSET) & (1u << 1)) && --timeout > 0);
        if (timeout == 0) return false;

        REG(base + IC_DATA_CMD_OFFSET) = cmd;

        timeout = 10000;
        do {
            if (REG(base + IC_RAW_INTR_STAT_OFFSET) & (1u << 6)) {
                abort = true;
                g_rp2350_last_abrt_source = REG(base + IC_TX_ABRT_SOURCE_OFFSET);
                (void)REG(base + IC_CLR_TX_ABRT_OFFSET);
                break;
            }
        } while (--timeout > 0 && (REG(base + IC_STATUS_OFFSET) & (1u << 3)) == 0);

        if (abort || timeout == 0) return false;

        dst[i] = (uint8_t)REG(base + IC_DATA_CMD_OFFSET);
    }
    return !abort;
}

static bool i2c_xfer_raw_bus(uint8_t bus, uint8_t addr, const uint8_t *w, int wlen,
                             uint8_t *r, int rlen) {
    if (bus >= i2c_bus_count()) return false;
    uintptr_t base = rp2350_i2c_base(bus);
    if (wlen > 0 && !rp2350_i2c_write_bytes_stop(base, addr, w, wlen, rlen == 0)) return false;
    if (rlen > 0 && !rp2350_i2c_read_phase_cont(base, addr, r, rlen, wlen > 0)) return false;
    return true;
}

static bool __attribute__((unused)) i2c_xfer_raw(uint8_t addr, const uint8_t *w, int wlen,
                         uint8_t *r, int rlen) {
    return i2c_xfer_raw_bus(0, addr, w, wlen, r, rlen);
}

static bool rp2350_i2c_probe_addr(uintptr_t base, uint8_t addr) {
    REG(base + IC_ENABLE_OFFSET) = 0;
    REG(base + IC_TAR_OFFSET) = addr;
    REG(base + IC_ENABLE_OFFSET) = 1;
    (void)REG(base + IC_CLR_TX_ABRT_OFFSET);
    g_rp2350_last_abrt_source = 0;

    // Send READ command + STOP bit
    uint32_t cmd = (1u << 8) | (1u << 9);
    REG(base + IC_DATA_CMD_OFFSET) = cmd;

    int timeout = 10000;
    bool abort = false;
    do {
        if (REG(base + IC_RAW_INTR_STAT_OFFSET) & (1u << 6)) { // TX_ABRT
            abort = true;
            g_rp2350_last_abrt_source = REG(base + IC_TX_ABRT_SOURCE_OFFSET);
            (void)REG(base + IC_CLR_TX_ABRT_OFFSET);
            break;
        }
    } while (--timeout > 0 && (REG(base + IC_STATUS_OFFSET) & (1u << 3)) == 0);

    if (!abort && timeout > 0) {
        (void)REG(base + IC_DATA_CMD_OFFSET); // Drain byte from RX FIFO
        return true;
    }
    return false;
}

static bool i2c_probe_addr_bus(uint8_t bus, uint8_t addr) {
    if (bus >= i2c_bus_count()) return false;
    return rp2350_i2c_probe_addr(rp2350_i2c_base(bus), addr);
}

static bool __attribute__((unused)) i2c_probe_addr(uint8_t addr) {
    return i2c_probe_addr_bus(0, addr);
}

void i2c_rp2350_diag_bus(uint8_t bus, uint8_t addr, uint8_t reg) {
    if (bus >= i2c_bus_count()) {
        cprintf("[I2Cdiag] Bus %u not available on this board\n", bus);
        return;
    }
    uintptr_t base = rp2350_i2c_base(bus);
    uint32_t gpio_in = REG(0xd0000004); // SIO GPIO_IN
    cprintf("[I2Cdiag] Bus %u GPIO: GP2=%d GP3=%d GP4=%d GP5=%d GP6=%d GP7=%d (raw=0x%08x)\n",
            bus,
            (int)((gpio_in >> 2) & 1), (int)((gpio_in >> 3) & 1),
            (int)((gpio_in >> 4) & 1), (int)((gpio_in >> 5) & 1),
            (int)((gpio_in >> 6) & 1), (int)((gpio_in >> 7) & 1),
            (unsigned)gpio_in);
    cprintf("[I2Cdiag] Bus %u: CON=0x%08x TAR=0x%08x ENABLE=0x%08x STATUS=0x%08x RAW=0x%08x\n",
            bus,
            (unsigned)REG(base + IC_CON_OFFSET), (unsigned)REG(base + IC_TAR_OFFSET),
            (unsigned)REG(base + IC_ENABLE_OFFSET), (unsigned)REG(base + IC_STATUS_OFFSET),
            (unsigned)REG(base + IC_RAW_INTR_STAT_OFFSET));
    for (int i = 0; i < 3; i++) {
        bool ok = rp2350_i2c_probe_addr(base, addr);
        cprintf("[I2Cdiag] probe#%d 0x%02x -> %d  abrt=0x%08x status=0x%08x\n",
                i, addr, (int)ok, (unsigned)g_rp2350_last_abrt_source,
                (unsigned)REG(base + IC_STATUS_OFFSET));
        (void)REG(base + IC_CLR_TX_ABRT_OFFSET);
    }
    for (int i = 0; i < 3; i++) {
        uint8_t v = 0xff;
        bool ok = false;
        g_rp2350_last_abrt_source = 0;
        if (rp2350_i2c_write_bytes_stop(base, addr, &reg, 1, false)) {
            ok = rp2350_i2c_read_phase_cont(base, addr, &v, 1, true);
        }
        cprintf("[I2Cdiag] read#%d 0x%02x reg 0x%02x -> %d val=0x%02x  "
                "abrt=0x%08x status=0x%08x\n",
                i, addr, reg, (int)ok, (unsigned)v,
                (unsigned)g_rp2350_last_abrt_source, (unsigned)REG(base + IC_STATUS_OFFSET));
        (void)REG(base + IC_CLR_TX_ABRT_OFFSET);
    }

    (void)REG(base + IC_CLR_TX_ABRT_OFFSET);
    (void)REG(base + IC_CLR_STOP_DET_OFFSET);
    uint8_t rv = 0xff;
    bool wok = rp2350_i2c_write_bytes_stop(base, addr, &reg, 1, false);
    bool open_after_write = (REG(base + IC_RAW_INTR_STAT_OFFSET) & IC_RAW_STOP_DET) == 0;
    bool rok = rp2350_i2c_read_phase_cont(base, addr, &rv, 1, true);
    bool stopped_after_read = false;
    for (uint64_t end = time_get_us() + 1000u; time_get_us() < end; ) {
        if (REG(base + IC_RAW_INTR_STAT_OFFSET) & IC_RAW_STOP_DET) { stopped_after_read = true; break; }
    }
    (void)REG(base + IC_CLR_STOP_DET_OFFSET);
    cprintf("[I2Cdiag] repeated-start: write=%d open_after_write=%d read=%d "
            "val=0x%02x stop_after_read=%d -> %s\n",
            (int)wok, (int)open_after_write, (int)rok, (unsigned)rv,
            (int)stopped_after_read,
            (wok && open_after_write && rok && stopped_after_read)
                ? "ONE TRANSACTION" : "SPLIT");
}

void i2c_rp2350_diag(uint8_t addr, uint8_t reg) {
    i2c_rp2350_diag_bus(0, addr, reg);
}
#elif defined(CONFIG_BOARD_ESP32P4) || defined(CONFIG_BOARD_ESP32C6)
/* Espressif's I2C controller: the ESP32-P4's I2C0 (E7, plan/phase27_esp32p4_bringup.md) and, since
 * 45.10, the ESP32-C6's (plan/phase45_esp32c6.md). The C6's register map, bit positions and command
 * opcodes are the P4's to the bit (checked against IDF's soc/esp32c6/register/soc/i2c_reg.h and
 * esp_hal_i2c/esp32c6 i2c_ll.h); what differs is in the per-chip block below -- the base, the clock
 * and reset, the GPIO matrix's offsets, the pins and signal numbers. Everything else -- the command
 * lists, the timing, the bus clear and the recovery -- is one implementation, learned once on the P4.
 *
 * ESP32-P4 I2C0 -- E7.
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

#if defined(CONFIG_BOARD_ESP32P4)
#define ESP_I2C_BASE        0x500C4000UL
#else   /* ESP32-C6: I2C0 at DR_REG_I2C_EXT_BASE */
#define ESP_I2C_BASE        0x60004000UL
#endif
#define ESP_I2C_SCL_LOW     (ESP_I2C_BASE + 0x00)
#define ESP_I2C_CTR         (ESP_I2C_BASE + 0x04)
#define ESP_I2C_SR          (ESP_I2C_BASE + 0x08)
#define ESP_I2C_SR_BUS_BUSY (1u << 4)   /* I2C_BUS_BUSY, hw_ver1 and hw_ver3 alike */
#define ESP_I2C_TO          (ESP_I2C_BASE + 0x0c)
#define ESP_I2C_FIFO_ST     (ESP_I2C_BASE + 0x14)
#define ESP_I2C_FIFO_CONF   (ESP_I2C_BASE + 0x18)
#define ESP_I2C_DATA        (ESP_I2C_BASE + 0x1c)
#define ESP_I2C_INT_RAW     (ESP_I2C_BASE + 0x20)
#define ESP_I2C_INT_CLR     (ESP_I2C_BASE + 0x24)
#define ESP_I2C_SDA_HOLD    (ESP_I2C_BASE + 0x30)
#define ESP_I2C_SDA_SAMPLE  (ESP_I2C_BASE + 0x34)
#define ESP_I2C_SCL_HIGH    (ESP_I2C_BASE + 0x38)
#define ESP_I2C_SCL_START_HOLD   (ESP_I2C_BASE + 0x40)
#define ESP_I2C_SCL_RSTART_SETUP (ESP_I2C_BASE + 0x44)
#define ESP_I2C_SCL_STOP_HOLD    (ESP_I2C_BASE + 0x48)
#define ESP_I2C_SCL_STOP_SETUP   (ESP_I2C_BASE + 0x4c)
#define ESP_I2C_FILTER_CFG  (ESP_I2C_BASE + 0x50)
#define ESP_I2C_COMD(n)     (ESP_I2C_BASE + 0x58 + 4u * (n))
#define ESP_I2C_SCL_ST_TO        (ESP_I2C_BASE + 0x78)
#define ESP_I2C_SCL_MAIN_ST_TO   (ESP_I2C_BASE + 0x7c)
#define ESP_I2C_SCL_SP_CONF      (ESP_I2C_BASE + 0x80)

#define ESP_I2C_TIME_OUT_EN     (1u << 5)    /* TO: above the 5-bit value    */
#define ESP_I2C_SCL_FILTER_EN   (1u << 8)    /* FILTER_CFG, one per line     */
#define ESP_I2C_SDA_FILTER_EN   (1u << 9)
#define ESP_I2C_SCL_RST_SLV_EN  (1u << 0)    /* SCL_SP_CONF: clear the bus   */
#define ESP_I2C_SCL_RST_SLV_NUM(n) ((uint32_t)(n) << 1)

#define ESP_I2C_SDA_FORCE_OUT  (1u << 0)
#define ESP_I2C_SCL_FORCE_OUT  (1u << 1)
#define ESP_I2C_MS_MODE        (1u << 4)
#define ESP_I2C_TRANS_START    (1u << 5)
#define ESP_I2C_CLK_EN         (1u << 8)
#define ESP_I2C_FSM_RST        (1u << 10)
#define ESP_I2C_CONF_UPGATE    (1u << 11)
#define ESP_I2C_RX_FIFO_RST    (1u << 12)
#define ESP_I2C_TX_FIFO_RST    (1u << 13)
#define ESP_I2C_FIFO_PRT_EN    (1u << 14)

#define ESP_I2C_INT_END_DETECT      (1u << 3)
#define ESP_I2C_INT_ARB_LOST        (1u << 5)
#define ESP_I2C_INT_TRANS_COMPLETE  (1u << 7)
#define ESP_I2C_INT_TIME_OUT        (1u << 8)
#define ESP_I2C_INT_NACK            (1u << 10)

/* Opcodes. See the header comment: these are the P4's, not ESP32's. */
#define ESP_CMD_RSTART 6u
#define ESP_CMD_WRITE  1u
#define ESP_CMD_STOP   2u
#define ESP_CMD_READ   3u
#define ESP_CMD_END    4u

/* op<<11 | ack_check_en<<8 | ack_value<<10 | byte_num */
#define ESP_CMD(op, ack_check, ack_val, n) \
    (((uint32_t)(op) << 11) | ((uint32_t)(ack_check) << 8) | \
     ((uint32_t)(ack_val) << 10) | (uint32_t)(n))

#if defined(CONFIG_BOARD_ESP32P4)
#define P4_CLKRST_BASE      0x500E6000UL
#define P4_SOC_CLK_CTRL2    (P4_CLKRST_BASE + 0x1c)   /* bit 12: I2C0 APB gate */
#define P4_PERI_CLK_CTRL10  (P4_CLKRST_BASE + 0x40)   /* b0 src sel, b1 clk en */
#define P4_HP_RST_EN1       (P4_CLKRST_BASE + 0xc4)   /* bit 22: I2C0 reset    */
#define ESP_GPIO_BASE        0x500E0000UL
#define ESP_GPIO_IN_OFF      0x158u
#define ESP_GPIO_OUT_OFF     0x558u
#define ESP_IOMUX_BASE       0x500E1000UL
#define ESP_I2C_SDA_GPIO 7u
#define ESP_I2C_SCL_GPIO 8u
#define ESP_I2C_SDA_SIG  69u
#define ESP_I2C_SCL_SIG  68u
#else   /* ESP32-C6 */
/* PCR: I2C_CONF (bit 0 APB clock, bit 1 reset) and I2C_SCLK_CONF (bit 20 source: 0 = XTAL, bits
 * [19:12] divider, bit 22 enable) -- IDF's soc/esp32c6/register/soc/pcr_reg.h. */
#define C6_PCR_I2C_CONF      0x60096020UL
#define C6_PCR_I2C_SCLK_CONF 0x60096024UL
#define ESP_GPIO_BASE        0x60091000UL
#define ESP_GPIO_IN_OFF      0x154u
#define ESP_GPIO_OUT_OFF     0x554u
#define ESP_IOMUX_BASE       0x60090000UL
/* The ESP32-C6-Zero's GPIO0 (SDA) and GPIO1 (SCL) -- also the 32 kHz crystal's pins, which this
 * board does not fit, so their IO_MUX function 1 is plain GPIO. Signals I2CEXT0_SDA/SCL (46/45),
 * IDF's soc/esp32c6/include/soc/gpio_sig_map.h. */
#define ESP_I2C_SDA_GPIO 0u
#define ESP_I2C_SCL_GPIO 1u
#define ESP_I2C_SDA_SIG  46u
#define ESP_I2C_SCL_SIG  45u
#endif
#define ESP_GPIO_ENABLE_W1TS (ESP_GPIO_BASE + 0x24)
#define ESP_GPIO_PIN(n)      (ESP_GPIO_BASE + 0x74 + 4u * (n))
#define ESP_GPIO_IN_SEL(sig) (ESP_GPIO_BASE + ESP_GPIO_IN_OFF + 4u * (sig))
#define ESP_GPIO_OUT_SEL(n)  (ESP_GPIO_BASE + ESP_GPIO_OUT_OFF + 4u * (n))
#define ESP_GPIO_PAD_DRIVER  (1u << 2)                 /* open drain           */
#define ESP_IOMUX_PAD(n)     (ESP_IOMUX_BASE + 0x4 + 4u * (n))
#define ESP_IOMUX_FUN_GPIO   (1u << 12)                /* MCU_SEL = 1          */
#define ESP_IOMUX_FUN_IE     (1u << 9)
#define ESP_IOMUX_FUN_PU     (1u << 8)

static bool g_esp_i2c_ready;
static void esp_i2c_bringup_timing(void);
static bool i2c_xfer_raw(uint8_t addr, const uint8_t *w, int wlen,
                         uint8_t *r, int rlen);
static bool i2c_probe_addr(uint8_t addr);
static void esp_i2c_bus_clear(void);
static uint32_t g_esp_i2c_last_int;
static uint32_t g_esp_i2c_last_sr;

static void esp_pad_for_i2c(uint32_t gpio, uint32_t sig) {
    /* Open drain, input enabled, weak pull-up. The pull-up matters even with
     * a module that has its own: without it an absent module leaves the line
     * floating, and a floating SDA reads as a permanent ACK from every
     * address, which is a bus scan that finds 128 devices. */
    uint32_t pad = REG(ESP_IOMUX_PAD(gpio));
    pad &= ~(7u << 12);
    REG(ESP_IOMUX_PAD(gpio)) = pad | ESP_IOMUX_FUN_GPIO | ESP_IOMUX_FUN_IE | ESP_IOMUX_FUN_PU;
    REG(ESP_GPIO_PIN(gpio)) |= ESP_GPIO_PAD_DRIVER;
    REG(ESP_GPIO_ENABLE_W1TS) = (1u << gpio);
    /* Both directions: the controller drives the line and also samples it, so
     * a one-way route gives a bus that transmits and never sees an ACK. */
    REG(ESP_GPIO_OUT_SEL(gpio)) = sig;
    REG(ESP_GPIO_IN_SEL(sig))   = gpio | (1u << 7);   /* bit 7: take from matrix */
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
 * Idempotent: the second caller finds g_esp_i2c_ready and returns. */
static void esp_i2c_hw_bringup(bool verbose) {
    if (g_esp_i2c_ready) {
        if (verbose) printk_critical("[I2Cdiag] already up\n");
        return;
    }

#if defined(CONFIG_BOARD_ESP32P4)
    if (verbose) printk_critical("[I2Cdiag] 1: APB gate\n");
    REG(P4_SOC_CLK_CTRL2) |= (1u << 12);

    if (verbose) printk_critical("[I2Cdiag] 2: module clock, XTAL source\n");
    REG(P4_PERI_CLK_CTRL10) &= ~(1u << 0);
    REG(P4_PERI_CLK_CTRL10) |= (1u << 1);

    if (verbose) printk_critical("[I2Cdiag] 3: reset pulse\n");
    REG(P4_HP_RST_EN1) |=  (1u << 22);
    REG(P4_HP_RST_EN1) &= ~(1u << 22);
#else   /* ESP32-C6: the same three steps in the PCR -- APB clock, function clock from the 40 MHz
         * crystal undivided (so the timing below holds as it is), a reset pulse */
    REG(C6_PCR_I2C_CONF) |= (1u << 0);
    REG(C6_PCR_I2C_SCLK_CONF) = (REG(C6_PCR_I2C_SCLK_CONF) & ~((1u << 20) | (0xffu << 12))) | (1u << 22);
    REG(C6_PCR_I2C_CONF) |=  (1u << 1);
    REG(C6_PCR_I2C_CONF) &= ~(1u << 1);
#endif

    if (verbose) printk_critical("[I2Cdiag] 4: pad (SDA)\n");
    esp_pad_for_i2c(ESP_I2C_SDA_GPIO, ESP_I2C_SDA_SIG);

    if (verbose) printk_critical("[I2Cdiag] 5: pad (SCL)\n");
    esp_pad_for_i2c(ESP_I2C_SCL_GPIO, ESP_I2C_SCL_SIG);

    if (verbose) printk_critical("[I2Cdiag] 6: CTR\n");
    REG(ESP_I2C_CTR) = ESP_I2C_MS_MODE | ESP_I2C_CLK_EN |
                      ESP_I2C_SDA_FORCE_OUT | ESP_I2C_SCL_FORCE_OUT;

    if (verbose) printk_critical("[I2Cdiag] 7: timing\n");
    esp_i2c_bringup_timing();

    if (verbose) printk_critical("[I2Cdiag] 8: fifo + commit\n");
    REG(ESP_I2C_FIFO_CONF) = ESP_I2C_FIFO_PRT_EN;
    REG(ESP_I2C_CTR) |= ESP_I2C_CONF_UPGATE;
    g_esp_i2c_ready = true;

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
    esp_i2c_bus_clear();
    if (verbose) printk_critical("[I2Cdiag] bringup complete\n");
}

void i2c_bus_init(void) { esp_i2c_hw_bringup(false); }

/* Every period below is in cycles of the controller's source clock, which
 * step 2 above selects as XTAL_CLK: 40 MHz on this board, undivided
 * (PERI_CLK_CTRL10's div_num field is 0, which is a divide by one). A half
 * cycle of 200 is therefore 100 kHz -- standard mode, which is what a BME280
 * on flying leads wants over the board's own pull-ups. Those are 2.2 kOhm to
 * 3V3 on both lines, fitted on the NANO itself (ESP32-P4-NANO-schematic.pdf,
 * the ESP_I2C_SDA/ESP_I2C_SCL nets) -- so the pads' internal pull-ups that
 * esp_pad_for_i2c() also enables are a backstop for a bare chip, not what is
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
static void esp_i2c_bringup_timing(void) {
    const uint32_t half = 200u;                   /* 40 MHz / 100 kHz / 2 */
    const uint32_t wait_high = half / 2u - 2u;    /* 98, for >= 80 kHz    */
    const uint32_t high      = half - wait_high;  /* 102                  */
    const uint32_t sample    = half / 2u;         /* 100                  */

    REG(ESP_I2C_SCL_LOW)          = half - 1u;
    REG(ESP_I2C_SCL_HIGH)         = high | (wait_high << 9);
    REG(ESP_I2C_SCL_START_HOLD)   = half - 1u;
    REG(ESP_I2C_SCL_RSTART_SETUP) = half - 1u;
    REG(ESP_I2C_SCL_STOP_HOLD)    = half - 1u;
    REG(ESP_I2C_SCL_STOP_SETUP)   = half - 1u;
    REG(ESP_I2C_SDA_HOLD)         = half / 4u - 1u;
    REG(ESP_I2C_SDA_SAMPLE)       = sample - 1u;
    /* threshold 7 on each line, both filters enabled */
    REG(ESP_I2C_FILTER_CFG)       = 7u | (7u << 4) | ESP_I2C_SCL_FILTER_EN |
                                   ESP_I2C_SDA_FILTER_EN;
    /* 2^12 source cycles, about 10 bus cycles, and the enable that makes it
     * mean anything. IDF's formula: ceil(log2(5 * half_cycle)) + 2. */
    REG(ESP_I2C_TO)               = 12u | ESP_I2C_TIME_OUT_EN;
    REG(ESP_I2C_SCL_ST_TO)        = 0x10u;
    REG(ESP_I2C_SCL_MAIN_ST_TO)   = 0x10u;

    REG(ESP_I2C_FIFO_CONF) = ESP_I2C_FIFO_PRT_EN;
    REG(ESP_I2C_CTR) |= ESP_I2C_CONF_UPGATE;
    g_esp_i2c_ready = true;
}

static void esp_i2c_reset_fifo(void) {
    REG(ESP_I2C_FIFO_CONF) |= ESP_I2C_TX_FIFO_RST | ESP_I2C_RX_FIFO_RST;
    REG(ESP_I2C_FIFO_CONF) &= ~(ESP_I2C_TX_FIFO_RST | ESP_I2C_RX_FIFO_RST);
    REG(ESP_I2C_INT_CLR) = 0xffffffffu;
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
static void esp_i2c_begin(void) {
    REG(ESP_I2C_CTR) |= ESP_I2C_FSM_RST;
    esp_i2c_reset_fifo();
    REG(ESP_I2C_CTR) |= ESP_I2C_CONF_UPGATE;
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
static void esp_i2c_bus_clear(void) {
    REG(ESP_I2C_SCL_SP_CONF) = ESP_I2C_SCL_RST_SLV_NUM(9) | ESP_I2C_SCL_RST_SLV_EN;
    REG(ESP_I2C_CTR) |= ESP_I2C_CONF_UPGATE;

    /* Bounded in wall time for the reason esp_i2c_run() is: nine pulses at
     * 100 kHz is 90 us, and a bus that has not finished them in 5 ms is not
     * going to. Give up rather than spin -- the enable is then cleared by
     * hand so the next transaction does not start into a pending clear. */
    uint64_t deadline = time_get_us() + 5000u;
    while ((REG(ESP_I2C_SCL_SP_CONF) & ESP_I2C_SCL_RST_SLV_EN) != 0) {
        if (time_get_us() >= deadline) {
            REG(ESP_I2C_SCL_SP_CONF) = 0;
            break;
        }
    }
    REG(ESP_I2C_CTR) |= ESP_I2C_CONF_UPGATE;
    REG(ESP_I2C_INT_CLR) = 0xffffffffu;
}

/* After a failure: the ordinary preparation, plus the bus clear.
 *
 * The clear is *here* and in bringup, and deliberately not in esp_i2c_begin().
 * Putting it before every transaction was tried and made things worse rather
 * than better -- a clear ends in a STOP, and a START issued straight after it
 * does not leave the bus-free time a slave needs (t_BUF, 4.7 us at 100 kHz),
 * so the first transfer of a burst started NACKing a part that was there. The
 * two places it belongs are the two where the bus may genuinely be held by
 * someone else: at bringup, when this kernel has just arrived and has no idea
 * what the previous firmware left mid-byte, and after an error, when the
 * transfer we just abandoned may have stopped a slave mid-byte ourselves. */
static void esp_i2c_recover(void) {
    esp_i2c_begin();
    esp_i2c_bus_clear();
}

/* Runs a command list that has already been written, and reports what the
 * bus said. Bounded: a stuck bus must not become a stuck kernel. */
/* g_esp_i2c_last_int / _sr (declared above): what the last transaction
 * ended on. */

static bool esp_i2c_run(void) {
    REG(ESP_I2C_CTR) |= ESP_I2C_CONF_UPGATE;
    REG(ESP_I2C_CTR) |= ESP_I2C_TRANS_START;

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
        st = REG(ESP_I2C_INT_RAW);
        if (st & (ESP_I2C_INT_NACK | ESP_I2C_INT_TIME_OUT | ESP_I2C_INT_ARB_LOST)) break;
        if (st & (ESP_I2C_INT_TRANS_COMPLETE | ESP_I2C_INT_END_DETECT)) break;
    } while (time_get_us() < deadline);

    g_esp_i2c_last_int = st;
    g_esp_i2c_last_sr  = REG(ESP_I2C_SR);

    /* 47.2, plan/phase47_esp32p4_lcd7b_ribbon.md: a NACK is *reported* before
     * the transfer is over. The command list still runs on to its STOP, and
     * the bus stays busy until that has gone out; IDF's master waits for
     * exactly this (`while (i2c_ll_is_bus_busy(...))` after an event) before
     * it touches the controller again. Resetting the FSM inside that window
     * -- which esp_i2c_recover() did, immediately -- leaves the next
     * transaction to inherit a half-sent STOP. On the v3.2 board that showed
     * as a *false ACK* every other read of an empty address (id 0xff,
     * TRANS_COMPLETE without NACK), which is how the boot announced a DS1307
     * at 0x68 and an EEPROM at 0x57 on a bus with neither. Every failure
     * captured SR with BUS_BUSY set; the phantom success had it clear. */
    {
        uint64_t idle_by = time_get_us() + 2000u;
        while ((REG(ESP_I2C_SR) & ESP_I2C_SR_BUS_BUSY) && time_get_us() < idle_by) { }
    }

    bool ok = (st & (ESP_I2C_INT_TRANS_COMPLETE | ESP_I2C_INT_END_DETECT)) != 0 &&
              (st & (ESP_I2C_INT_NACK | ESP_I2C_INT_TIME_OUT | ESP_I2C_INT_ARB_LOST)) == 0;
    /* A failure that is not a clean NACK -- a timeout, lost arbitration, the
     * deadline expiring with no bit set, or a bus still busy after the wait
     * above -- leaves the FSM, and possibly a slave, somewhere this driver did
     * not put it. See esp_i2c_recover(): without it, one wedged transfer makes
     * the whole bus absent.
     *
     * 47.2: a *clean* NACK (bus idle again) gets nothing more. That is IDF's
     * policy too -- its master resets and clears the bus only on a timeout or
     * a bus found busy, never for a NACK -- and the reason it matters showed
     * on the LCD-7B: running the nine-pulse bus clear after every NACK, i.e.
     * at every empty address of a scan, left the GT911 touch controller
     * NACKing its own address until some other part's transfer went through,
     * so `i2c scan` reported no touch controller on a board that has one. The
     * next transaction's esp_i2c_begin() still resets the FSM and FIFOs, which
     * is what the NANO's stale-controller fix needed. */
    if (!ok) {
        bool clean_nack = (st & ESP_I2C_INT_NACK) &&
                          !(st & (ESP_I2C_INT_TIME_OUT | ESP_I2C_INT_ARB_LOST)) &&
                          !(REG(ESP_I2C_SR) & ESP_I2C_SR_BUS_BUSY);
        if (!clean_nack) esp_i2c_recover();
    }
    return ok;
}

#if defined(CONFIG_BOARD_ESP32P4)
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
    esp_i2c_hw_bringup(true);
    printk_critical("[I2Cdiag] ready=%d\n", (int)g_esp_i2c_ready);
   
    printk_critical("[I2Cdiag] CTR=0x%08x\n", (unsigned)REG(ESP_I2C_CTR));
   
    printk_critical("[I2Cdiag] SR=0x%08x FIFO_ST=0x%08x\n",
           (unsigned)REG(ESP_I2C_SR), (unsigned)REG(ESP_I2C_FIFO_ST));
   
    printk_critical("[I2Cdiag] clkrst: CTRL2=0x%08x CTRL10=0x%08x RST1=0x%08x\n",
           (unsigned)REG(P4_SOC_CLK_CTRL2), (unsigned)REG(P4_PERI_CLK_CTRL10),
           (unsigned)REG(P4_HP_RST_EN1));
   
    printk_critical("[I2Cdiag] pads: io7=0x%08x io8=0x%08x out7=0x%08x in69=0x%08x\n",
           (unsigned)REG(ESP_IOMUX_PAD(7)), (unsigned)REG(ESP_IOMUX_PAD(8)),
           (unsigned)REG(ESP_GPIO_OUT_SEL(7)), (unsigned)REG(ESP_GPIO_IN_SEL(69)));
   
    /* Both shapes, three times each, and the repetition is the measurement.
     *
     * One probe cannot tell "the part is there" from "the last transaction
     * left the controller somewhere". Three in a row can, and the reading is
     * mechanical: 1,1,1 is a part; 0,0,0 is an empty address; **0,1,1 is a
     * stale controller** -- the first transaction paying for what the
     * previous one left behind. That last pattern is what found
     * esp_i2c_begin() and the bringup bus clear, so the diagnostic keeps the
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
               i, (int)pok, (unsigned)g_esp_i2c_last_int, (unsigned)g_esp_i2c_last_sr);
    }
    for (int i = 0; i < 3; i++) {
        uint8_t reg = 0xd0u, id = 0;
        bool rok = i2c_xfer_raw(0x76u, &reg, 1, &id, 1);
        printk_critical("[I2Cdiag] read#%d 0x76 reg 0xd0 -> %d id=0x%02x  INT_RAW=0x%08x SR=0x%08x\n",
               i, (int)rok, (unsigned)id, (unsigned)g_esp_i2c_last_int,
               (unsigned)g_esp_i2c_last_sr);
    }
}

/* What the last transaction ended on, for i2cdiag. Exposed rather than
 * printed here: this runs inside the i2c task, which may not printk(). */
void i2c_p4_last_status(uint32_t *int_raw, uint32_t *sr) {
    if (int_raw) *int_raw = g_esp_i2c_last_int;
    if (sr) *sr = g_esp_i2c_last_sr;
}

#endif /* CONFIG_BOARD_ESP32P4: i2cdiag */

static bool i2c_write_bytes(uint8_t addr, const uint8_t *src, int len) {
    if (!g_esp_i2c_ready || len < 0 || len > 30) return false;
    esp_i2c_begin();
    REG(ESP_I2C_DATA) = (uint32_t)(addr << 1);          /* address + write     */
    for (int i = 0; i < len; i++) REG(ESP_I2C_DATA) = src[i];
    REG(ESP_I2C_COMD(0)) = ESP_CMD(ESP_CMD_RSTART, 0, 0, 0);
    REG(ESP_I2C_COMD(1)) = ESP_CMD(ESP_CMD_WRITE, 1, 0, 1 + len);
    REG(ESP_I2C_COMD(2)) = ESP_CMD(ESP_CMD_STOP, 0, 0, 0);
    return esp_i2c_run();
}
/* Write-then-read with a repeated start, which is the shape every register
 * read on this bus takes: address+W, the register number, RESTART,
 * address+R, then the data. Building it as one command list is what makes it
 * a repeated start rather than two transfers with a STOP between -- and a
 * STOP there is what lets another master, or a device with an internal
 * pointer, lose the register selection. */
static bool i2c_xfer_raw(uint8_t addr, const uint8_t *w, int wlen,
                         uint8_t *r, int rlen) {
    if (!g_esp_i2c_ready) return false;
    if (wlen < 0 || rlen < 0 || wlen > 30 || rlen > 30) return false;
    if (rlen == 0) return i2c_write_bytes(addr, w, wlen);

    esp_i2c_begin();
    REG(ESP_I2C_DATA) = (uint32_t)(addr << 1);
    for (int i = 0; i < wlen; i++) REG(ESP_I2C_DATA) = w[i];
    REG(ESP_I2C_DATA) = (uint32_t)((addr << 1) | 1u);

    unsigned c = 0;
    REG(ESP_I2C_COMD(c++)) = ESP_CMD(ESP_CMD_RSTART, 0, 0, 0);
    REG(ESP_I2C_COMD(c++)) = ESP_CMD(ESP_CMD_WRITE, 1, 0, 1 + wlen);
    REG(ESP_I2C_COMD(c++)) = ESP_CMD(ESP_CMD_RSTART, 0, 0, 0);
    REG(ESP_I2C_COMD(c++)) = ESP_CMD(ESP_CMD_WRITE, 1, 0, 1);
    if (rlen > 1) {
        /* All but the last byte are ACKed; the last is NACKed, which is how a
         * master tells the device the read is over. Sending ACK for the final
         * byte leaves the device driving the bus into the STOP. */
        REG(ESP_I2C_COMD(c++)) = ESP_CMD(ESP_CMD_READ, 0, 0, rlen - 1);
    }
    REG(ESP_I2C_COMD(c++)) = ESP_CMD(ESP_CMD_READ, 0, 1, 1);
    REG(ESP_I2C_COMD(c++)) = ESP_CMD(ESP_CMD_STOP, 0, 0, 0);

    if (!esp_i2c_run()) return false;
    for (int i = 0; i < rlen; i++) r[i] = (uint8_t)(REG(ESP_I2C_DATA) & 0xffu);
    return true;
}

/* Address-only transaction: a START, the address byte with ACK checking on,
 * and a STOP. Nothing is read, so the only thing that distinguishes a present
 * device from an absent one is whether the address was acknowledged -- which
 * is precisely what esp_i2c_run() returns false for. */
static bool i2c_probe_addr(uint8_t addr) {
    if (!g_esp_i2c_ready) return false;
    esp_i2c_begin();
    REG(ESP_I2C_DATA) = (uint32_t)(addr << 1);
    REG(ESP_I2C_COMD(0)) = ESP_CMD(ESP_CMD_RSTART, 0, 0, 0);
    REG(ESP_I2C_COMD(1)) = ESP_CMD(ESP_CMD_WRITE, 1, 0, 1);
    REG(ESP_I2C_COMD(2)) = ESP_CMD(ESP_CMD_STOP, 0, 0, 0);
    return esp_i2c_run();
}

static bool i2c_xfer_raw_bus(uint8_t bus, uint8_t addr, const uint8_t *w, int wlen,
                             uint8_t *r, int rlen) {
    if (bus != 0) return false;
    return i2c_xfer_raw(addr, w, wlen, r, rlen);
}

static bool i2c_probe_addr_bus(uint8_t bus, uint8_t addr) {
    if (bus != 0) return false;
    return i2c_probe_addr(addr);
}
#else
void i2c_bus_init(void) {}
static bool __attribute__((unused)) i2c_probe_addr_bus(uint8_t bus, uint8_t addr) { (void)bus; (void)addr; return false; }
static bool __attribute__((unused)) i2c_probe_addr(uint8_t addr) { (void)addr; return false; }
static bool __attribute__((unused)) i2c_xfer_raw_bus(uint8_t bus, uint8_t addr, const uint8_t *w, int wlen,
                             uint8_t *r, int rlen) {
    (void)bus; (void)addr; (void)w; (void)wlen; (void)r; (void)rlen; return false;
}
static bool __attribute__((unused)) i2c_xfer_raw(uint8_t addr, const uint8_t *w, int wlen,
                         uint8_t *r, int rlen) {
    (void)addr; (void)w; (void)wlen; (void)r; (void)rlen; return false;
}
#endif

uint8_t i2c_bus_count(void) {
#if defined(CONFIG_BOARD_RP2350)
#if RP2350_I2C1_AVAILABLE
    return 2;
#else
    return 1;
#endif
#else
    return 1;
#endif
}

void i2c_scan_bus_id(uint8_t bus) {
    if (bus >= i2c_bus_count()) {
        cprintf("I2C bus %u not available on this board.\n", bus);
        return;
    }
#if defined(CONFIG_BOARD_RP2350)
#if RP2350_I2C1_AVAILABLE
    int sda = (bus == 1) ? I2C_BUS1_SDA : I2C_BUS0_SDA;
    int scl = (bus == 1) ? I2C_BUS1_SCL : I2C_BUS0_SCL;
#else
    int sda = I2C_BUS0_SDA, scl = I2C_BUS0_SCL;
#endif
    if (bus == 0) {
        cprintf("\nI2C Bus Scan (GP%d SDA / GP%d SCL):\n", sda, scl);
    } else {
        cprintf("\nI2C Bus %u Scan (GP%d SDA / GP%d SCL):\n", bus, sda, scl);
    }
#else
    if (bus == 0) {
        cprintf("\nI2C Bus Scan:\n");
    } else {
        cprintf("\nI2C Bus %u Scan:\n", bus);
    }
#endif
    cprintf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

    int found_count = 0;
    for (int row = 0; row < 128; row += 16) {
        cprintf("%02x: ", row);
        for (int col = 0; col < 16; col++) {
            uint8_t addr = row + col;
            if (addr < 0x03 || addr > 0x77) {
                cprintf("   ");
            } else {
                if (i2c_probe_addr_bus(bus, addr)) {
                    cprintf("%02x ", addr);
                    found_count++;
                } else {
                    cprintf("-- ");
                }
            }
        }
        cprintf("\n");
    }
    cprintf("Found %d I2C device(s) on bus %u.\n\n", found_count, bus);
}

void i2c_scan_bus(void) {
    uint8_t n = i2c_bus_count();
    if (n == 0) {
        cprintf("\nNo I2C controller on this target.\n\n");
        return;
    }
    for (uint8_t b = 0; b < n; b++) {
        i2c_scan_bus_id(b);
    }
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
/* The DS3231's status register, for OSF. Its own op rather than a field
 * bolted onto the time read: the face reads the time once a second and only
 * consults the flag when deciding whether to trust it. */
/* Q4: the generic transfer -- write then read, for any address on this bus.
 * The last device-specific opcode this task should need: a new part
 * (drivers/bme280.c is the first) is M-mode code that builds requests, not
 * another case in here.
 *
 *   request:  'F', addr, wlen, rlen, w[wlen]
 *   response: ok, r[rlen]
 */
#define I2C_OP_XFER           ((uint8_t)'F')
#define I2C_OP_XFER_BUS       ((uint8_t)'B')


/* Sized by the generic transfer operations. Sized to hold opcode, bus,
 * addr, wlen, rlen and payload up to I2C_XFER_WMAX. */
#define I2C_REQ_CAP  (5u + I2C_XFER_WMAX)
#define I2C_RESP_CAP (1u + I2C_XFER_RMAX)

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

I2C_UATTR static void i2c_usys_target(uintptr_t base, uint8_t addr) {
    REG(base + IC_ENABLE_OFFSET) = 0;
    REG(base + IC_TAR_OFFSET) = addr;
    REG(base + IC_ENABLE_OFFSET) = 1;
    (void)REG(base + IC_CLR_TX_ABRT_OFFSET);
}

/* Writes len bytes already TAR-targeted by the caller, STOP after the
 * last byte only if stop_at_end -- false is for a register/address
 * prefix that a read phase (i2c_usys_read_reg() below) continues past. */
I2C_UATTR static bool i2c_usys_write_raw(uintptr_t base, const uint8_t *data, int len, bool stop_at_end) {
    for (int i = 0; i < len; i++) {
        bool last = (i == len - 1);
        uint32_t cmd = data[i];
        if (last && stop_at_end) cmd |= (1u << 9);

        int timeout = 10000;
        while (!(REG(base + IC_STATUS_OFFSET) & (1u << 1)) && --timeout > 0);
        if (timeout == 0) return false;

        REG(base + IC_DATA_CMD_OFFSET) = cmd;

        timeout = 10000;
        bool abort = false;
        do {
            if (REG(base + IC_RAW_INTR_STAT_OFFSET) & (1u << 6)) {
                abort = true;
                (void)REG(base + IC_CLR_TX_ABRT_OFFSET);
                break;
            }
        } while (--timeout > 0 && !(REG(base + IC_RAW_INTR_STAT_OFFSET) & (1u << 4)));

        if (abort || timeout == 0) return false;
    }
    return true;
}

/* Sends len bytes to addr, STOP after the last -- RTC's register-pointer
 * + payload writes and EEPROM's 2-byte-address + payload writes alike. */
I2C_UATTR static bool i2c_usys_write_bytes(uintptr_t base, uint8_t addr, const uint8_t *data, int len) {
    i2c_usys_target(base, addr);
    return i2c_usys_write_raw(base, data, len, true);
}

/* Writes reg_len address/register bytes (no STOP), then re-targets and
 * reads len bytes -- RTC's 1-byte register reads (time at 0x00,
 * temperature at 0x11) and EEPROM's 2-byte address reads alike. */
I2C_UATTR static bool i2c_usys_read_reg(uintptr_t base, uint8_t addr, const uint8_t *reg, int reg_len, uint8_t *dst, int len) {
    i2c_usys_target(base, addr);
    if (!i2c_usys_write_raw(base, reg, reg_len, false)) return false;

    for (int i = 0; i < len; i++) {
        bool last = (i == len - 1);
        uint32_t cmd = (1u << 8);
        if (last) cmd |= (1u << 9);

        int timeout = 10000;
        while (!(REG(base + IC_STATUS_OFFSET) & (1u << 1)) && --timeout > 0);
        if (timeout == 0) return false;

        REG(base + IC_DATA_CMD_OFFSET) = cmd;

        timeout = 10000;
        bool abort = false;
        do {
            if (REG(base + IC_RAW_INTR_STAT_OFFSET) & (1u << 6)) {
                abort = true;
                (void)REG(base + IC_CLR_TX_ABRT_OFFSET);
                break;
            }
        } while (--timeout > 0 && (REG(base + IC_STATUS_OFFSET) & (1u << 3)) == 0);

        if (abort || timeout == 0) return false;

        dst[i] = (uint8_t)REG(base + IC_DATA_CMD_OFFSET);
    }
    return true;
}

I2C_UATTR static void i2c_umode_body(void) {
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
        case I2C_OP_XFER: {
            uintptr_t base = I2C_BUS0_BASE;
            bool ok = false;
            uint32_t rlen = 0;
            if (req_len >= 4) {
                uint8_t addr = req[1];
                uint32_t wlen = req[2];
                rlen = req[3];
                if (wlen <= I2C_XFER_WMAX && rlen <= I2C_XFER_RMAX &&
                    (long)(4u + wlen) <= req_len) {
                    if (rlen) {
                        ok = i2c_usys_read_reg(base, addr, &req[4], (int)wlen,
                                               &resp[1], (int)rlen);
                    } else {
                        ok = i2c_usys_write_bytes(base, addr, &req[4], (int)wlen);
                    }
                }
            }
            if (!ok) rlen = 0;
            resp[0] = ok ? 1 : 0;
            resp_len = 1u + rlen;
            break;
        }
        case I2C_OP_XFER_BUS: {
            bool ok = false;
            uint32_t rlen = 0;
            if (req_len >= 5) {
                uint8_t bus = req[1];
                uint8_t addr = req[2];
                uint32_t wlen = req[3];
                rlen = req[4];
                uintptr_t base = (bus == 1) ? I2C_BUS1_BASE : I2C_BUS0_BASE;
                if (bus < 1u + RP2350_I2C1_AVAILABLE &&
                    wlen <= I2C_XFER_WMAX && rlen <= I2C_XFER_RMAX &&
                    (long)(5u + wlen) <= req_len) {
                    if (rlen) {
                        ok = i2c_usys_read_reg(base, addr, &req[5], (int)wlen,
                                               &resp[1], (int)rlen);
                    } else {
                        ok = i2c_usys_write_bytes(base, addr, &req[5], (int)wlen);
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

/* This task's own kernel-mode entry point: task_create_sized() calls this
 * (ordinary kernel stack, kernel privilege) to build the domain and make
 * the one-way jump into U-mode. */
static void i2c_task_body(void *arg) {
    (void)arg;
    while (!g_i2c_ep) sched_yield();

    /* G3, plan/phase30_driver_framework.md. I2C controller windows. */
    const driver_umode_spec_t spec = {
        .name         = "I2C",
        .fallback     = "RTC/EEPROM stay on direct hardware access.",
        .body         = i2c_umode_body,
        .stack_base   = (uintptr_t)g_i2c_ustack,
        .stack_size   = sizeof(g_i2c_ustack),
        .regions      = {
            { I2C_BUS0_BASE, 4096, MEM_R | MEM_W },
#if RP2350_I2C1_AVAILABLE
            { I2C_BUS1_BASE, 4096, MEM_R | MEM_W },
#endif
        },
#if RP2350_I2C1_AVAILABLE
        .region_count = 2,
#else
        .region_count = 1,
#endif
    };
    (void)driver_umode_enter(&spec);
}

#else /* !CONFIG_BOARD_RP2350: plain kernel-mode server, as every M4.5
       * driver task had it before M5 -- no real I2C hardware to isolate
       * on QEMU, so no reason to build a domain for it. */

/* This task, and only this task, may call the *_hw_* functions in this file
 * and drivers/at24c32.c while alive -- see uart_16550.c's uart_task_body()
 * for the fuller reasoning (never call back into anything that could
 * chan_call() this same endpoint; never write the console from here --
 * printk() is safe, cprintf() is not, see kernel/lock.h).
 *
 * Both are checked: chan_call() refuses a call that would close a wait-for
 * cycle (Y3), and console_lock() reports a console write from inside a serve
 * callback (Y5). See kernel/lock.h. */
static void i2c_task_body(void *arg) {
    (void)arg;
    while (!g_i2c_ep) sched_yield();

    for (;;) {
        uint32_t req_len = chan_serve_wait(g_i2c_ep);
        if (req_len < 1) { chan_serve_reply(g_i2c_ep, 0); continue; }

        uint8_t op = g_i2c_req[0];
        switch (op) {
        case I2C_OP_XFER: {
            bool ok = false;
            uint32_t rlen = 0;
            if (req_len >= 4) {
                uint8_t addr = g_i2c_req[1];
                uint32_t wlen = g_i2c_req[2];
                rlen = g_i2c_req[3];
                if (wlen <= I2C_XFER_WMAX && rlen <= I2C_XFER_RMAX &&
                    (uint32_t)req_len >= 4u + wlen) {
                    ok = i2c_xfer_raw_bus(0, addr, &g_i2c_req[4], (int)wlen,
                                          &g_i2c_resp[1], (int)rlen);
                }
            }
            if (!ok) rlen = 0;
            g_i2c_resp[0] = ok ? 1 : 0;
            chan_serve_reply(g_i2c_ep, 1 + rlen);
            break;
        }
        case I2C_OP_XFER_BUS: {
            bool ok = false;
            uint32_t rlen = 0;
            if (req_len >= 5) {
                uint8_t bus = g_i2c_req[1];
                uint8_t addr = g_i2c_req[2];
                uint32_t wlen = g_i2c_req[3];
                rlen = g_i2c_req[4];
                if (wlen <= I2C_XFER_WMAX && rlen <= I2C_XFER_RMAX &&
                    (uint32_t)req_len >= 5u + wlen) {
                    ok = i2c_xfer_raw_bus(bus, addr, &g_i2c_req[5], (int)wlen,
                                          &g_i2c_resp[1], (int)rlen);
                }
            }
            if (!ok) rlen = 0;
            g_i2c_resp[0] = ok ? 1 : 0;
            chan_serve_reply(g_i2c_ep, 1 + rlen);
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


bool i2c_xfer_bus(uint8_t bus, uint8_t addr, const uint8_t *w, uint32_t wlen,
                  uint8_t *r, uint32_t rlen) {
    if (bus >= i2c_bus_count()) return false;
    if (wlen > I2C_XFER_WMAX || rlen > I2C_XFER_RMAX) return false;
    if ((wlen && !w) || (rlen && !r)) return false;

    if (i2c_task_alive()) {
        uint8_t req[5 + I2C_XFER_WMAX];
        uint8_t resp[1 + I2C_XFER_RMAX];
        req[0] = I2C_OP_XFER_BUS;
        req[1] = bus;
        req[2] = addr;
        req[3] = (uint8_t)wlen;
        req[4] = (uint8_t)rlen;
        if (wlen) memcpy(&req[5], w, wlen);
        int n = i2c_task_call(req, 5u + wlen, resp, sizeof(resp));
        if (n < 1 || resp[0] != 1) return false;
        if (rlen) {
            if ((uint32_t)n < 1u + rlen) return false;
            memcpy(r, &resp[1], rlen);
        }
        return true;
    }
    return i2c_xfer_raw_bus(bus, addr, w, (int)wlen, r, (int)rlen);
}

bool i2c_xfer(uint8_t addr, const uint8_t *w, uint32_t wlen,
              uint8_t *r, uint32_t rlen) {
    return i2c_xfer_bus(0, addr, w, wlen, r, rlen);
}

