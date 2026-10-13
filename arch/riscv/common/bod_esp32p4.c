/* The ESP32-P4's brown-out detector, armed to reset the chip.
 *
 * Its reset enable defaults to 0 (LP_ANALOG_PERI_BOD_MODE0_CNTL bit 31), and
 * nothing but an application turns it on -- ESP-IDF does in every app
 * (esp_brownout_init()). Left off, a sagging supply does not stop the chip: it
 * runs on below its rated voltage, with whatever the SD card, the flash and
 * the C6 on the same 3.3 V rail make of that. Armed, the dip ends in a clean
 * system reset, and the next boot says why ("[Chip] last reset: BROWN-OUT").
 *
 * The sequence and values are IDF's (esp_hal_pmu brownout_hal.c,
 * brownout_ll.h): the analog mode-1 reset off, a two-cycle interrupt wait, the
 * flash suspended and the RF powered down on a dip, the counter cleared, a
 * system-level reset after 0x3FF cycles, the threshold at level 7 (about
 * 2.6 V, IDF's default) and detection on. The threshold is two 3-bit fields
 * of register 9 of the analog bias block (slave 0x6A, regi2c_brownout.h),
 * reached through the clock driver's analog I2C master.
 *
 * Registers: IDF soc/esp32p4/register/hw_ver3/soc/lp_analog_peri_reg.h (the
 * same addresses on v1.3). */

#include "arch/clk_esp32p4.h"
#include "kernel/printk.h"
#include <stdint.h>
#include <stdbool.h>

#if defined(CONFIG_BOARD_ESP32P4)

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define LP_ANA_BASE           0x50113000u
#define LP_ANA_BOD_MODE0      (LP_ANA_BASE + 0x00u)
#define  BOD0_CLOSE_FLASH        (1u << 6)
#define  BOD0_PD_RF              (1u << 7)
#define  BOD0_INTR_WAIT_S        8
#define  BOD0_INTR_WAIT_M        (0x3FFu << BOD0_INTR_WAIT_S)
#define  BOD0_RESET_WAIT_S       18
#define  BOD0_RESET_WAIT_M       (0x3FFu << BOD0_RESET_WAIT_S)
#define  BOD0_CNT_CLR            (1u << 28)
#define  BOD0_INTR_ENA           (1u << 29)   /* the detector itself, despite the name */
#define  BOD0_RESET_SEL          (1u << 30)   /* 1 = system reset, 0 = chip reset */
#define  BOD0_RESET_ENA          (1u << 31)
#define LP_ANA_BOD_MODE1      (LP_ANA_BASE + 0x04u)
#define  BOD1_RESET_ENA          (1u << 31)
#define LP_ANA_FIB_ENABLE     (LP_ANA_BASE + 0x1Cu)
#define  FIB_BOD                 (1u << 1)    /* 1: mode 1 follows the eFuse, 0: software */

#define I2C_BIAS_SLAVE        0x6Au
#define I2C_BIAS_MST_SEL      (1u << 12)
#define I2C_BOD_THRESHOLD_REG 9u
#define BOD_LEVEL             7u              /* ~2.6 V */

static bool g_armed;

void esp32p4_brownout_arm(void) {
    REG(LP_ANA_FIB_ENABLE) &= ~FIB_BOD;
    REG(LP_ANA_BOD_MODE1) &= ~BOD1_RESET_ENA;

    uint32_t v = REG(LP_ANA_BOD_MODE0);
    v &= ~(BOD0_INTR_WAIT_M | BOD0_RESET_WAIT_M | BOD0_INTR_ENA | BOD0_RESET_ENA);
    v |= (2u << BOD0_INTR_WAIT_S) | BOD0_CLOSE_FLASH | BOD0_PD_RF;
    REG(LP_ANA_BOD_MODE0) = v | BOD0_CNT_CLR;
    REG(LP_ANA_BOD_MODE0) = v;
    v |= (0x3FFu << BOD0_RESET_WAIT_S) | BOD0_RESET_ENA | BOD0_RESET_SEL;
    REG(LP_ANA_BOD_MODE0) = v;

    bool lo = esp32p4_regi2c_write_field(I2C_BIAS_SLAVE, I2C_BIAS_MST_SEL, I2C_BOD_THRESHOLD_REG, 4, 2, BOD_LEVEL);
    bool hi = esp32p4_regi2c_write_field(I2C_BIAS_SLAVE, I2C_BIAS_MST_SEL, I2C_BOD_THRESHOLD_REG, 7, 5, BOD_LEVEL);
    if (!lo || !hi) {
        /* Without the threshold the level is whatever the analog block holds;
         * arming at an unknown level could reset a healthy board. */
        REG(LP_ANA_BOD_MODE0) = v & ~BOD0_RESET_ENA;
        printk("[BOD] threshold not written (analog I2C master busy or gated); detector left off\n");
        return;
    }
    REG(LP_ANA_BOD_MODE0) = v | BOD0_INTR_ENA;
    g_armed = true;
    printk("[BOD] brown-out reset armed at level %u (~2.6 V)\n", BOD_LEVEL);
}

bool esp32p4_brownout_armed(void) { return g_armed; }

#endif
