/* The ESP32-P4's in-package PSRAM. 47.3, plan/phase47_esp32p4_lcd7b_ribbon.md.
 *
 * Both P4 boards carry an ESP32-P4NRW32: 32 MB of AP Memory hex-mode PSRAM in
 * the package, on its own MSPI controller (SPI_MEM_S, "MSPI2/3") -- not the
 * flash's (SPI_MEM_C). Nothing brings it up before us: the boot ROM loads our
 * image straight from flash, without the ESP-IDF second-stage bootloader that
 * would. So everything ESP-IDF's esp_psram_impl_enable() relies on having
 * happened is done here, in IDF's order:
 *
 *   1. power   LDO channel 2 at 1.8 V. Both schematics tie VDDO_PSRAM (pin
 *              72, VFB/VO2) to VDD_PSRAM_0/1 (pins 59, 67), and nothing else
 *              feeds them: the PSRAM is unpowered until this runs (its PMU
 *              register read 0x40200000, XPD clear, on both boards).
 *   2. clock   MPLL at 400 MHz -- its PHY power and output gate, then the
 *              analog programming, which differs by silicon revision
 *              (clk_ll_mpll_set_config_v1 / _v3).
 *   3. ctrl    the PSRAM controller's clocks from MPLL, its reset pulsed, pad
 *              drive 2 and DQS on, CS timing, page size, bus clock, DLL.
 *   4. device  mode registers MR0/MR4/MR8 written through the register
 *              port (MSPI3) with the ROM's user-command routines, a
 *              write/read check, then MR1/MR2 read for vendor and density.
 *   5. cache   the cache port (MSPI2) set up for hex-line DDR reads and
 *              writes, and the PSRAM MMU mapping the chip at 0x48000000.
 *
 * Speed: **20 MHz** (MPLL 400 / 20), IDF's SPIRAM_SPEED_20M settings, which is
 * the one speed IDF runs without timing tuning. Raising it is a separate
 * milestone step (80/200 MHz need mspi_timing_psram_tuning()).
 *
 * Every register is in drivers/include/drivers/esp32p4_psram_regs.h, which is
 * generated from IDF's hw_ver1 and hw_ver3 headers and fails to generate if
 * they disagree; the ROM routines used are at identical addresses in
 * esp32p4.rom.ld and esp32p4.rom.eco0_4.ld. The two places the revisions
 * differ -- the MPLL programming and v3's extra bias writes -- are #if'd on
 * CONFIG_ESP32P4_REV, with IDF's code named at each.
 *
 * Unlike the RP2350, where PSRAM and flash share the QMI and a flash write
 * must not touch PSRAM, the two are on separate controllers here and
 * drivers/flash_esp32p4.c never turns the cache off: PSRAM stays usable
 * across a flash erase. */

#include "lugalos_config.h"
#include "drivers/psram_esp32p4.h"   /* outside the guard: never an empty unit */

#if defined(CONFIG_BOARD_ESP32P4) && defined(CONFIG_PSRAM_BYTES)

#include "drivers/esp32p4_psram_regs.h"
#include "arch/esp32p4_intr.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/palloc.h"
#include "kernel/sched.h"
#include "kernel/time.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

static inline void field_set(uintptr_t reg, unsigned s, uint32_t v_mask, uint32_t val) {
    uint32_t x = REG(reg);
    x &= ~(v_mask << s);
    x |= (val & v_mask) << s;
    REG(reg) = x;
}
#define FSET(reg, f, val) field_set((reg), f##_S, f##_V, (uint32_t)(val))
#define FGET(reg, f)      ((REG(reg) >> f##_S) & f##_V)

static void spin_us(uint32_t us) {
    uint64_t t0 = time_get_us();
    while (time_get_us() - t0 < us) { }
}

/* ---- state -------------------------------------------------------------- */

extern char _bulk_bss_start[], _bulk_bss_end[];

static bool        g_up;
static const char *g_reason = "not initialised";
static uint32_t    g_bytes;
static uint8_t     g_mr[10];         /* MR0..MR9, read back in pairs; MR8 is the last that matters */
static uint8_t     g_ldo_dref, g_ldo_mul;
static bool        g_ldo_from_efuse;

/* ---- 1. power: LDO channel 2 ----------------------------------------------
 *
 * ESP-IDF esp_ldo_acquire_channel() for chan 2 (ldo_unit 1), whose registers
 * are PMU_EXT_LDO_P1_0P1A{,_ANA} (ldo_ll.h's index_array {0,3,1,4}). The
 * voltage: if eFuse BLK1 carries this chip's calibrated LDO_VO2_DREF/MUL for
 * 1.8 V (block version >= 0.1 and both non-zero), those; otherwise IDF's
 * uncalibrated search, which for 1800 mV is dref 8 (Vref 18/20 V) x mul 4
 * (x 8/4... i.e. (4000 + 4*1000)/4000 = 2) = 1.8 V exactly. Both from
 * ldo_ll_voltage_to_dref_mul(). */
#define EFUSE_RD_MAC_SYS_2   0x5012d04cu   /* BLK1 bits 95..64 */
#define EFUSE_RD_MAC_SYS_3   0x5012d050u   /* BLK1 bits 127..96 */

static void ldo2_on(void) {
    uint32_t w2 = REG(EFUSE_RD_MAC_SYS_2), w3 = REG(EFUSE_RD_MAC_SYS_3);
    unsigned blk_minor = (w2 >> 8) & 0x7u;      /* BLK_VERSION_MINOR, BLK1 72..74 */
    unsigned blk_major = (w2 >> 11) & 0x3u;     /* BLK_VERSION_MAJOR, BLK1 75..76 */
    uint8_t  e_dref    = (uint8_t)((w2 >> 28) & 0xFu);   /* LDO_VO2_DREF, BLK1 92..95 */
    uint8_t  e_mul     = (uint8_t)((w3 >> 3) & 0x7u);    /* LDO_VO2_MUL,  BLK1 99..101 */

    g_ldo_dref = 8; g_ldo_mul = 4; g_ldo_from_efuse = false;
    if ((blk_major * 100u + blk_minor) >= 1u && e_dref && e_mul) {
        g_ldo_dref = e_dref; g_ldo_mul = e_mul; g_ldo_from_efuse = true;
    }

    /* IDF's order: current limit on, voltage, software owner, ripple
     * suppression, enable, current limit off, then let it settle. */
    REG(PMU_EXT_LDO_P1_0P1A_ANA_REG) |= (1u << 27);                 /* EN_CUR_LIM */
    FSET(PMU_EXT_LDO_P1_0P1A_REG, PMU_0P1A_TIEH_1, 0);              /* Vref x mul */
    FSET(PMU_EXT_LDO_P1_0P1A_ANA_REG, PMU_ANA_0P1A_DREF_1, g_ldo_dref);
    FSET(PMU_EXT_LDO_P1_0P1A_ANA_REG, PMU_ANA_0P1A_MUL_1, g_ldo_mul);
    FSET(PMU_EXT_LDO_P1_0P1A_REG, PMU_0P1A_FORCE_TIEH_SEL_1, 1);    /* by software */
    FSET(PMU_EXT_LDO_P1_0P1A_REG, PMU_0P1A_TIEH_SEL_1, 0);
    REG(PMU_EXT_LDO_P1_0P1A_ANA_REG) |= (1u << 26);                 /* EN_VDET */
    FSET(PMU_EXT_LDO_P1_0P1A_REG, PMU_0P1A_XPD_1, 1);
    REG(PMU_EXT_LDO_P1_0P1A_ANA_REG) &= ~(1u << 27);
    spin_us(200);    /* CONFIG_ESP_LDO_VOLTAGE_STABLE_DELAY_US */
}

/* ---- 2. clock: MPLL at 400 MHz -------------------------------------------- */

extern bool esp32p4_regi2c_read(uint8_t blk, uint32_t mst_sel, uint8_t reg, uint8_t *out);
extern bool esp32p4_regi2c_write_mask(uint8_t blk, uint32_t mst_sel, uint8_t reg,
                                      unsigned msb, unsigned lsb, uint8_t val);

/* soc/regi2c_mpll.h and esp_hal_regi2c/esp32p4/regi2c_impl.c (block 0x63,
 * "MSPI_XTAL", master select BIT9); soc/regi2c_bias.h (0x6A, BIT12). */
#define I2C_MPLL         0x63u
#define I2C_MPLL_SEL     (1u << 9)
#define I2C_BIAS         0x6Au
#define I2C_BIAS_SEL     (1u << 12)

#define MPLL_MHZ         400u

static bool mpll_cal_wait(void) {
    uint64_t t0 = time_get_us();
    while (!FGET(HP_SYS_CLKRST_ANA_PLL_CTRL0_REG, HP_SYS_CLKRST_REG_MSPI_CAL_END)) {
        if (time_get_us() - t0 > 10000u) return false;
    }
    return true;
}

static bool mpll_on(void) {
    /* clk_ll_mpll_enable(): the MSPI PHY's power and the 500M gate. */
    FSET(PMU_RF_PWC_REG, PMU_MSPI_PHY_XPD, 1);
    FSET(LP_CLKRST_HP_CLK_CTRL_REG, LP_CLKRST_HP_MPLL_500M_CLK_EN, 1);

    uint8_t div = (uint8_t)(MPLL_MHZ / 20u - 1u);   /* MPLL = 40 * (div+1) / 2 */
    bool ok = true;
#if CONFIG_ESP32P4_REV >= 300
    /* pmu_init()'s v3-only analog writes -- MSPI PHY bias and the MPLL's
     * external cap -- which a stock IDF boot has made by now. */
    ok &= esp32p4_regi2c_write_mask(I2C_BIAS, I2C_BIAS_SEL, 0, 7, 4, 12);  /* DREG_1P1 */
    ok &= esp32p4_regi2c_write_mask(I2C_BIAS, I2C_BIAS_SEL, 1, 3, 0, 12);  /* DREG_1P1_PVT */
    /* clk_ll_mpll_set_config_v3() */
    ok &= esp32p4_regi2c_write_mask(I2C_MPLL, I2C_MPLL_SEL, 1, 1, 0, 3);   /* IR_CAL_EXT_CAP */
    ok &= esp32p4_regi2c_write_mask(I2C_MPLL, I2C_MPLL_SEL, 1, 2, 2, 1);   /* IR_CAL_ENX_CAP */
    ok &= esp32p4_regi2c_write_mask(I2C_MPLL, I2C_MPLL_SEL, 2, 7, 3, 9);   /* DIV */
    ok &= esp32p4_regi2c_write_mask(I2C_MPLL, I2C_MPLL_SEL, 3, 5, 4, 3);   /* DHREF */
    ok &= esp32p4_regi2c_write_mask(I2C_MPLL, I2C_MPLL_SEL, 1, 2, 2, 0);   /* IR_CAL_ENX_CAP */
    FSET(HP_SYS_CLKRST_ANA_PLL_CTRL0_REG, HP_SYS_CLKRST_REG_MSPI_CAL_STOP, 0);
    ok &= esp32p4_regi2c_write_mask(I2C_MPLL, I2C_MPLL_SEL, 2, 7, 3, div);
#else
    /* clk_ll_mpll_set_config_v1() */
    FSET(HP_SYS_CLKRST_ANA_PLL_CTRL0_REG, HP_SYS_CLKRST_REG_MSPI_CAL_STOP, 0);
    uint8_t v = 0;
    ok &= esp32p4_regi2c_read(I2C_MPLL, I2C_MPLL_SEL, 3, &v);
    ok &= esp32p4_regi2c_write_mask(I2C_MPLL, I2C_MPLL_SEL, 3, 7, 0, (uint8_t)(v | (3u << 4)));
    ok &= esp32p4_regi2c_read(I2C_MPLL, I2C_MPLL_SEL, 1, &v);
    ok &= esp32p4_regi2c_write_mask(I2C_MPLL, I2C_MPLL_SEL, 1, 7, 0, (uint8_t)(v & 0xdfu));
    ok &= esp32p4_regi2c_write_mask(I2C_MPLL, I2C_MPLL_SEL, 1, 7, 0, (uint8_t)(v | (1u << 5)));
    ok &= esp32p4_regi2c_write_mask(I2C_MPLL, I2C_MPLL_SEL, 2, 7, 0, (uint8_t)((div << 3) | 1u));
#endif
    if (!ok) return false;
    ok = mpll_cal_wait();
    FSET(HP_SYS_CLKRST_ANA_PLL_CTRL0_REG, HP_SYS_CLKRST_REG_MSPI_CAL_STOP, 1);
    return ok;
}

/* 47.3b: the target speed, a board-file choice (CONFIG_PSRAM_SPEED_MHZ): 20,
 * IDF's untuned SPIRAM_SPEED_20M, or 200, SPIRAM_SPEED_200M, reached through
 * the DQS timing tuning below. Both from the 400 MHz MPLL. Bring-up and the
 * tuning's reference write always run at 20; the cache port is switched to
 * the target only after the sweep has found where to sample. */
#ifndef CONFIG_PSRAM_SPEED_MHZ
#define CONFIG_PSRAM_SPEED_MHZ 20
#endif
#if CONFIG_PSRAM_SPEED_MHZ != 20 && CONFIG_PSRAM_SPEED_MHZ != 200
#error "CONFIG_PSRAM_SPEED_MHZ must be 20 or 200 (IDF's SPIRAM_SPEED_20M / _200M)"
#endif
#define PSRAM_INIT_MHZ    20u
#define BUS_DIV_FOR(mhz)  (MPLL_MHZ / (mhz))
static uint32_t g_speed_mhz = PSRAM_INIT_MHZ;   /* what the bus runs at now */

/* ---- 3. controller -------------------------------------------------------- */

/* psram_ctrlr_ll_set_bus_clock() for both ports: N = div-1, H = div/2-1,
 * L = div-1 (mspi_timing_config_set_psram_clock(): core clock div 1). */
static void bus_clock(uint32_t mhz) {
    uint32_t div = BUS_DIV_FOR(mhz);
    REG(SPI_MEM_S_SRAM_CLK_REG) = ((div - 1u) << SPI_MEM_S_SCLKCNT_N_S) |
                                  ((div / 2u - 1u) << SPI_MEM_S_SCLKCNT_H_S) |
                                  ((div - 1u) << SPI_MEM_S_SCLKCNT_L_S);
    REG(SPI1_MEM_S_CLOCK_REG)   = ((div - 1u) << SPI1_MEM_S_CLKCNT_N_S) |
                                  ((div / 2u - 1u) << SPI1_MEM_S_CLKCNT_H_S) |
                                  ((div - 1u) << SPI1_MEM_S_CLKCNT_L_S);
    g_speed_mhz = mhz;
}


static void ctrl_setup(void) {
    /* psram_ctrlr_ll_enable_module_clock / _reset_module_clock /
     * _select_clk_source (1 = MPLL) / _set_core_clock_div (1). */
    FSET(HP_SYS_CLKRST_SOC_CLK_CTRL0_REG,   HP_SYS_CLKRST_REG_PSRAM_SYS_CLK_EN, 1);
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL00_REG, HP_SYS_CLKRST_REG_PSRAM_PLL_CLK_EN, 1);
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL00_REG, HP_SYS_CLKRST_REG_PSRAM_CORE_CLK_EN, 1);
    FSET(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_DUAL_MSPI_AXI, 1);
    FSET(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_DUAL_MSPI_APB, 1);
    FSET(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_DUAL_MSPI_APB, 0);
    FSET(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_DUAL_MSPI_AXI, 0);
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL00_REG, HP_SYS_CLKRST_REG_PSRAM_CLK_SRC_SEL, 1);
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL00_REG, HP_SYS_CLKRST_REG_PSRAM_CORE_CLK_DIV_NUM, 0);

    /* mspi_timing_ll_pin_drv_set(2), mspi_timing_ll_enable_dqs(true). */
    static const uint32_t pins[][2] = {
        { IOMUX_MSPI_PIN_PSRAM_D_PIN0_REG,    IOMUX_MSPI_PIN_REG_PSRAM_D_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_Q_PIN0_REG,    IOMUX_MSPI_PIN_REG_PSRAM_Q_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_WP_PIN0_REG,   IOMUX_MSPI_PIN_REG_PSRAM_WP_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_HOLD_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_HOLD_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ4_PIN0_REG,  IOMUX_MSPI_PIN_REG_PSRAM_DQ4_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ5_PIN0_REG,  IOMUX_MSPI_PIN_REG_PSRAM_DQ5_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ6_PIN0_REG,  IOMUX_MSPI_PIN_REG_PSRAM_DQ6_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ7_PIN0_REG,  IOMUX_MSPI_PIN_REG_PSRAM_DQ7_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ8_PIN0_REG,  IOMUX_MSPI_PIN_REG_PSRAM_DQ8_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ9_PIN0_REG,  IOMUX_MSPI_PIN_REG_PSRAM_DQ9_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ10_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQ10_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ11_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQ11_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ12_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQ12_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ13_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQ13_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ14_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQ14_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ15_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQ15_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_DQS_0_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQS_0_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_DQS_1_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQS_1_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_CK_PIN0_REG,   IOMUX_MSPI_PIN_REG_PSRAM_CK_DRV_S },
        { IOMUX_MSPI_PIN_PSRAM_CS_PIN0_REG,   IOMUX_MSPI_PIN_REG_PSRAM_CS_DRV_S },
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        field_set(pins[i][0], pins[i][1], 0x3u, 2u);
    FSET(IOMUX_MSPI_PIN_PSRAM_DQS_0_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQS_0_XPD, 1);
    FSET(IOMUX_MSPI_PIN_PSRAM_DQS_1_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQS_1_XPD, 1);

    /* s_set_psram_cs_timing(): setup 4, hold 4, hold delay 3. */
    FSET(SPI_MEM_S_SMEM_AC_REG, SPI_MEM_S_SMEM_CS_SETUP, 1);
    FSET(SPI_MEM_S_SMEM_AC_REG, SPI_MEM_S_SMEM_CS_SETUP_TIME, 4 - 1);
    FSET(SPI_MEM_S_SMEM_AC_REG, SPI_MEM_S_SMEM_CS_HOLD, 1);
    FSET(SPI_MEM_S_SMEM_AC_REG, SPI_MEM_S_SMEM_CS_HOLD_TIME, 4 - 1);
    FSET(SPI_MEM_S_SMEM_AC_REG, SPI_MEM_S_SMEM_CS_HOLD_DELAY, 3 - 1);
    FSET(SPI_MEM_S_SMEM_AC_REG, SPI_MEM_S_SMEM_SPLIT_TRANS_EN, 1);
    FSET(SPI_MEM_S_SMEM_ECC_CTRL_REG, SPI_MEM_S_SMEM_PAGE_SIZE, 3);   /* 2048 */

    bus_clock(PSRAM_INIT_MHZ);

    /* psram_ctrlr_ll_enable_dll() for both ports (both fields are SPIMEM2's). */
    FSET(SPI_MEM_S_SMEM_TIMING_CALI_REG, SPI_MEM_S_SMEM_DLL_TIMING_CALI, 1);
    FSET(SPI_MEM_S_TIMING_CALI_REG,      SPI_MEM_S_DLL_TIMING_CALI, 1);
}

/* ---- 4. device: the ROM's user-command routines on MSPI3 -------------------
 *
 * esp32p4/rom/opi_flash.h's esp_rom_spi_cmd_t and the three routines
 * psram_ctrlr_ll_common_transaction() calls, in OPI DTR mode (7 in
 * esp_rom_spiflash_read_mode_t) with chip select 1. ROM addresses are the same
 * in both revisions' linker scripts (0x4fc00108/10c/110). */
typedef struct {
    uint16_t  cmd;
    uint16_t  cmd_bitlen;
    uint32_t *addr;
    uint32_t  addr_bitlen;
    uint32_t *tx;
    uint32_t  tx_bitlen;
    uint32_t *rx;
    uint32_t  rx_bitlen;
    uint32_t  dummy_bitlen;
} rom_spi_cmd_t;

#define ROM_SPI_CMD_CONFIG   0x4fc00108u
#define ROM_SPI_CMD_START    0x4fc0010cu
#define ROM_SPI_SET_OP_MODE  0x4fc00110u
#define ROM_OPI_DTR_MODE     7
#define MSPI3                3
#define PSRAM_CS_MASK        (1u << 1)

/* AP hex PSRAM commands, and the latencies for the *target* speed
 * (esp_psram_impl_ap_hex.c): latency is counted in clocks, so the 200 MHz
 * settings are equally valid while bring-up runs the bus at 20. */
#define AP_SYNC_READ      0x0000u
#define AP_SYNC_WRITE     0x8080u
#define AP_REG_READ       0x4040u
#define AP_REG_WRITE      0xC0C0u
#define AP_CMD_BITLEN     16u
#define AP_ADDR_BITLEN    32u
#if CONFIG_PSRAM_SPEED_MHZ == 200
#define AP_RD_DUMMY       (2u * (14u - 1u))
#define AP_RD_REG_DUMMY   (2u * (7u - 1u))
#define AP_WR_DUMMY       (2u * (7u - 1u))
#define AP_RD_LATENCY     4u
#define AP_WR_LATENCY     1u
#else
#define AP_RD_DUMMY       (2u * (10u - 1u))
#define AP_RD_REG_DUMMY   (2u * (5u - 1u))
#define AP_WR_DUMMY       (2u * (5u - 1u))
#define AP_RD_LATENCY     2u
#define AP_WR_LATENCY     2u
#endif
#define AP_REF_DATA       0x5a6b7c8du

static void psram_xfer(uint16_t cmd, uint32_t addr, uint32_t dummy,
                       uint32_t *tx, uint32_t tx_bits, uint32_t *rx, uint32_t rx_bits) {
    void (*set_mode)(int, int) = (void (*)(int, int))(uintptr_t)ROM_SPI_SET_OP_MODE;
    void (*config)(int, rom_spi_cmd_t *) = (void (*)(int, rom_spi_cmd_t *))(uintptr_t)ROM_SPI_CMD_CONFIG;
    void (*start)(int, uint8_t *, uint16_t, uint8_t, bool) =
        (void (*)(int, uint8_t *, uint16_t, uint8_t, bool))(uintptr_t)ROM_SPI_CMD_START;
    rom_spi_cmd_t c = {
        .cmd = cmd, .cmd_bitlen = AP_CMD_BITLEN,
        .addr = &addr, .addr_bitlen = AP_ADDR_BITLEN,
        .tx = tx, .tx_bitlen = tx_bits,
        .rx = rx, .rx_bitlen = rx_bits,
        .dummy_bitlen = dummy,
    };
    set_mode(MSPI3, ROM_OPI_DTR_MODE);
    config(MSPI3, &c);
    start(MSPI3, (uint8_t *)rx, (uint16_t)(rx_bits / 8u), PSRAM_CS_MASK, false);
}

static uint8_t mr_read8(uint32_t addr) {
    uint32_t v = 0;
    psram_xfer(AP_REG_READ, addr, AP_RD_REG_DUMMY, NULL, 0, &v, 8);
    return (uint8_t)v;
}
static uint16_t mr_read16(uint32_t addr) {
    uint32_t v = 0;
    psram_xfer(AP_REG_READ, addr, AP_RD_REG_DUMMY, NULL, 0, &v, 16);
    return (uint16_t)v;
}
static void mr_write16(uint32_t addr, uint16_t val) {
    uint32_t v = val;
    psram_xfer(AP_REG_WRITE, addr, 0, &v, 16, NULL, 0);
}

/* s_init_psram_mode_reg(): fixed latency, read latency, drive strength full;
 * write latency; 2 KB wrapped burst with RBX, x16 (hex) data. */
static void mode_regs_init(void) {
    uint16_t r01 = mr_read16(0x0);
    uint8_t mr0 = (uint8_t)r01;
    mr0 = (uint8_t)((mr0 & ~0x3Fu) | (0u /* drive_str */) | (AP_RD_LATENCY << 2) | (1u << 5) /* lt */);
    mr_write16(0x0, (uint16_t)((r01 & 0xFF00u) | mr0));

    uint16_t r48 = mr_read16(0x4);
    uint8_t mr4 = (uint8_t)r48;
    mr4 = (uint8_t)((mr4 & ~0xE0u) | (AP_WR_LATENCY << 5));
    mr_write16(0x4, (uint16_t)((r48 & 0xFF00u) | mr4));

    uint8_t mr8 = mr_read8(0x8);
    mr8 = (uint8_t)((mr8 & ~0x4Fu) | 3u /* bl 2K */ | (0u << 2) /* bt */ | (1u << 3) /* rbx */ | (1u << 6) /* x16 */);
    mr_write16(0x8, mr8);
}

static bool connected_check(void) {
    uint32_t ref = AP_REF_DATA, got = 0;
    psram_xfer(AP_SYNC_WRITE, 0x0, AP_WR_DUMMY, &ref, 32, NULL, 0);
    psram_xfer(AP_SYNC_READ, 0x0, AP_RD_DUMMY, NULL, 0, &got, 32);
    return got == ref;
}

/* ---- 4b. timing tuning (47.3b) ---------------------------------------------
 *
 * IDF's mspi_timing_psram_tuning() for the P4, i.e. the DQS scheme
 * (SOC_MEMSPI_TIMING_TUNING_BY_DQS; tuning_scheme_impl/mspi_timing_by_dqs.c):
 *
 *   - write 128 bytes of IDF's reference pattern at 0x80 at the safe speed;
 *   - at the target speed, read it back under each of the 4 DQS phases
 *     (67.5, 78.75, 90, 101.25 degrees); the best phase is the FIRST of the
 *     longest run that read back intact (mspi_timing_psram_select_best_
 *     tuning_phase: end - length + 1);
 *   - at that phase, sweep the 31 delay-line pairs {data, dqs} from {0,15}
 *     down to {0,0} and up to {15,0}, 100 reads each; a config passes only if
 *     all 100 do; the best is the MIDDLE of the longest passing run
 *     (end - length/2).
 *
 * The delay goes into every PSRAM pad's DLC field (data, CK and CS take the
 * data value) and both DQS pads' DELAY_90 and DELAY_270 (mspi_timing_ll_set_
 * delayline). IDF disables the cache around its speed changes because they
 * also move the *flash* clock; this moves only the PSRAM's, before anything
 * has used it, while the kernel keeps executing from flash. */
static const uint32_t s_ref[32] = {
    0x7f786655, 0xa5ff005a, 0x3f3c33aa, 0xa5ff5a00, 0x1f1e9955, 0xa5005aff, 0x0f0fccaa, 0xa55a00ff,
    0x07876655, 0xffa55a00, 0x03c333aa, 0xff00a55a, 0x01e19955, 0xff005aa5, 0x00f0ccaa, 0xff5a00a5,
    0x80786655, 0x00a5ff5a, 0xc03c33aa, 0x00a55aff, 0xe01e9355, 0x00ff5aa5, 0xf00fccaa, 0x005affa5,
    0xf8876655, 0x5aa5ff00, 0xfcc333aa, 0x5affa500, 0xfee19955, 0x5a00a5ff, 0x11f0ccaa, 0x5a00ffa5,
};
#define TUNE_ADDR        0x80u
#define TUNE_DL_NUM      31u
#define TUNE_DL_REPEATS  100u

static uint8_t  g_tune_phase_ok;           /* bit i: phase i passed */
static uint32_t g_tune_dl_ok;              /* bit i: delay-line config i passed 100/100 */
static int8_t   g_tune_phase = -1, g_tune_dl = -1;

static void dqs_phase(unsigned ph) {
    FSET(IOMUX_MSPI_PIN_PSRAM_DQS_0_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQS_0_PHASE, ph);
    FSET(IOMUX_MSPI_PIN_PSRAM_DQS_1_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQS_1_PHASE, ph);
}

static void delaylines(unsigned data, unsigned dqs) {
    static const uint32_t pads[][2] = {
        { IOMUX_MSPI_PIN_PSRAM_D_PIN0_REG,    IOMUX_MSPI_PIN_REG_PSRAM_D_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_Q_PIN0_REG,    IOMUX_MSPI_PIN_REG_PSRAM_Q_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_WP_PIN0_REG,   IOMUX_MSPI_PIN_REG_PSRAM_WP_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_HOLD_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_HOLD_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ4_PIN0_REG,  IOMUX_MSPI_PIN_REG_PSRAM_DQ4_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ5_PIN0_REG,  IOMUX_MSPI_PIN_REG_PSRAM_DQ5_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ6_PIN0_REG,  IOMUX_MSPI_PIN_REG_PSRAM_DQ6_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ7_PIN0_REG,  IOMUX_MSPI_PIN_REG_PSRAM_DQ7_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_CK_PIN0_REG,   IOMUX_MSPI_PIN_REG_PSRAM_CK_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_CS_PIN0_REG,   IOMUX_MSPI_PIN_REG_PSRAM_CS_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ8_PIN0_REG,  IOMUX_MSPI_PIN_REG_PSRAM_DQ8_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ9_PIN0_REG,  IOMUX_MSPI_PIN_REG_PSRAM_DQ9_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ10_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQ10_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ11_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQ11_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ12_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQ12_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ13_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQ13_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ14_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQ14_DLC_S },
        { IOMUX_MSPI_PIN_PSRAM_DQ15_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQ15_DLC_S },
    };
    for (unsigned i = 0; i < sizeof(pads) / sizeof(pads[0]); i++)
        field_set(pads[i][0], pads[i][1], 0xFu, data);
    FSET(IOMUX_MSPI_PIN_PSRAM_DQS_0_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQS_0_DELAY_90, dqs);
    FSET(IOMUX_MSPI_PIN_PSRAM_DQS_0_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQS_0_DELAY_270, dqs);
    FSET(IOMUX_MSPI_PIN_PSRAM_DQS_1_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQS_1_DELAY_90, dqs);
    FSET(IOMUX_MSPI_PIN_PSRAM_DQS_1_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQS_1_DELAY_270, dqs);
}

/* IDF's s_test_delayline_config: {0,15} .. {0,1}, {0,0}, {1,0} .. {15,0}. */
static void dl_config(unsigned i, unsigned *data, unsigned *dqs) {
    if (i < 15u) { *data = 0; *dqs = 15u - i; }
    else         { *data = i - 15u; *dqs = 0; }
}

/* 128 bytes through the register port in the controller's 64-byte FIFO
 * chunks (PSRAM_CTRLR_LL_FIFO_MAX_BYTES). */
static bool tune_read_ok(void) {
    uint32_t buf[32];
    for (unsigned off = 0; off < 128u; off += 64u)
        psram_xfer(AP_SYNC_READ, TUNE_ADDR + off, AP_RD_DUMMY, NULL, 0, &buf[off / 4u], 512);
    return memcmp(buf, s_ref, sizeof(buf)) == 0;
}

/* Longest run of set bits in `mask` over `n` positions: its length and the
 * index of its last bit (s_find_max_consecutive_success_points). */
static void longest_run(uint32_t mask, unsigned n, unsigned *len, unsigned *end) {
    unsigned best = 0, bend = 0, run = 0;
    for (unsigned i = 0; i < n; i++) {
        if (mask & (1u << i)) { run++; if (run > best) { best = run; bend = i; } }
        else run = 0;
    }
    *len = best; *end = bend;
}

static bool tune(uint32_t target_mhz) {
    for (unsigned off = 0; off < 128u; off += 64u)
        psram_xfer(AP_SYNC_WRITE, TUNE_ADDR + off, AP_WR_DUMMY,
                   (uint32_t *)&s_ref[off / 4u], 512, NULL, 0);

    /* mspi_timing_psram_init(): fixed dummy on the register port, target clock. */
    FSET(SPI1_MEM_S_DDR_REG, SPI1_MEM_S_FMEM_VAR_DUMMY, 0);
    bus_clock(target_mhz);

    delaylines(0, 0);
    g_tune_phase_ok = 0;
    for (unsigned ph = 0; ph < 4u; ph++) {
        dqs_phase(ph);
        if (tune_read_ok()) g_tune_phase_ok |= (uint8_t)(1u << ph);
    }
    unsigned len, end;
    longest_run(g_tune_phase_ok, 4, &len, &end);
    if (len == 0) return false;
    g_tune_phase = (int8_t)(end - len + 1u);
    dqs_phase((unsigned)g_tune_phase);

    g_tune_dl_ok = 0;
    for (unsigned i = 0; i < TUNE_DL_NUM; i++) {
        unsigned d, q;
        dl_config(i, &d, &q);
        delaylines(d, q);
        unsigned good = 0;
        for (unsigned r = 0; r < TUNE_DL_REPEATS; r++) good += tune_read_ok() ? 1u : 0u;
        if (good == TUNE_DL_REPEATS) g_tune_dl_ok |= 1u << i;
    }
    longest_run(g_tune_dl_ok, TUNE_DL_NUM, &len, &end);
    /* select_best_tuning_delayline(): a run of one is no margin at all. */
    g_tune_dl = (int8_t)(len <= 1u ? 0u : end - len / 2u);
    if (len <= 1u) return false;
    unsigned d, q;
    dl_config((unsigned)g_tune_dl, &d, &q);
    delaylines(d, q);
    return true;
}

/* ---- 5. cache port and MMU -------------------------------------------------- */

static void cache_port_setup(void) {
    /* s_config_mspi_for_psram(), for MSPI2. */
    FSET(SPI_MEM_S_CACHE_SCTRL_REG, SPI_MEM_S_CACHE_SRAM_USR_WCMD, 1);
    FSET(SPI_MEM_S_SRAM_DWR_CMD_REG, SPI_MEM_S_CACHE_SRAM_USR_WR_CMD_BITLEN, AP_CMD_BITLEN - 1);
    FSET(SPI_MEM_S_SRAM_DWR_CMD_REG, SPI_MEM_S_CACHE_SRAM_USR_WR_CMD_VALUE, AP_SYNC_WRITE);
    FSET(SPI_MEM_S_CACHE_SCTRL_REG, SPI_MEM_S_CACHE_SRAM_USR_RCMD, 1);
    FSET(SPI_MEM_S_SRAM_DRD_CMD_REG, SPI_MEM_S_CACHE_SRAM_USR_RD_CMD_BITLEN, AP_CMD_BITLEN - 1);
    FSET(SPI_MEM_S_SRAM_DRD_CMD_REG, SPI_MEM_S_CACHE_SRAM_USR_RD_CMD_VALUE, AP_SYNC_READ);
    FSET(SPI_MEM_S_CACHE_SCTRL_REG, SPI_MEM_S_SRAM_ADDR_BITLEN, AP_ADDR_BITLEN - 1);
    FSET(SPI_MEM_S_CACHE_SCTRL_REG, SPI_MEM_S_CACHE_USR_SADDR_4BYTE, 1);
    FSET(SPI_MEM_S_CACHE_SCTRL_REG, SPI_MEM_S_USR_WR_SRAM_DUMMY, 1);
    FSET(SPI_MEM_S_CACHE_SCTRL_REG, SPI_MEM_S_SRAM_WDUMMY_CYCLELEN, AP_WR_DUMMY - 1);
    FSET(SPI_MEM_S_CACHE_SCTRL_REG, SPI_MEM_S_USR_RD_SRAM_DUMMY, 1);
    FSET(SPI_MEM_S_CACHE_SCTRL_REG, SPI_MEM_S_SRAM_RDUMMY_CYCLELEN, AP_RD_DUMMY - 1);
    FSET(SPI_MEM_S_SMEM_DDR_REG, SPI_MEM_S_SMEM_VAR_DUMMY, 1);
    FSET(SPI_MEM_S_SRAM_CMD_REG, SPI_MEM_S_SDUMMY_WOUT, 1);
    FSET(SPI_MEM_S_SMEM_DDR_REG, SPI_MEM_S_SMEM_DDR_WDAT_SWP, 0);
    FSET(SPI_MEM_S_SMEM_DDR_REG, SPI_MEM_S_SMEM_DDR_RDAT_SWP, 0);
    FSET(SPI_MEM_S_SMEM_DDR_REG, SPI_MEM_S_SMEM_DDR_EN, 1);
    FSET(SPI_MEM_S_CACHE_SCTRL_REG, SPI_MEM_S_SRAM_OCT, 1);
    FSET(SPI_MEM_S_SRAM_CMD_REG, SPI_MEM_S_SCMD_OCT, 1);
    FSET(SPI_MEM_S_SRAM_CMD_REG, SPI_MEM_S_SADDR_OCT, 1);
    FSET(SPI_MEM_S_SRAM_CMD_REG, SPI_MEM_S_SDOUT_OCT, 1);
    FSET(SPI_MEM_S_SRAM_CMD_REG, SPI_MEM_S_SDIN_OCT, 1);
    FSET(SPI_MEM_S_SRAM_CMD_REG, SPI_MEM_S_SDIN_HEX, 1);
    FSET(SPI_MEM_S_SRAM_CMD_REG, SPI_MEM_S_SDOUT_HEX, 1);
    FSET(SPI_MEM_S_CACHE_FCTRL_REG, SPI_MEM_S_AXI_REQ_EN, 1);
    FSET(SPI_MEM_S_CACHE_FCTRL_REG, SPI_MEM_S_CLOSE_AXI_INF_EN, 0);
    FSET(SPI_MEM_S_CTRL1_REG, SPI_MEM_S_AW_SPLICE_EN, 1);
    FSET(SPI_MEM_S_CTRL1_REG, SPI_MEM_S_AR_SPLICE_EN, 1);
    /* After (the absent) tuning: variable dummy on both ports. */
    FSET(SPI_MEM_S_SMEM_DDR_REG, SPI_MEM_S_SMEM_VAR_DUMMY, 1);
    FSET(SPI1_MEM_S_DDR_REG, SPI1_MEM_S_FMEM_VAR_DUMMY, 1);
}

/* mmu_ll_write_entry() for the PSRAM MMU: 64 KB pages, entry i maps vaddr
 * 0x48000000 + i*64K to paddr i*64K. SOC_MMU_PSRAM_VALID BIT(11),
 * SOC_MMU_ACCESS_PSRAM BIT(10) (soc/ext_mem_defs.h). */
static void mmu_map(uint32_t bytes) {
    uint32_t pages = bytes >> 16;
    for (uint32_t i = 0; i < pages; i++) {
        REG(SPI_MEM_S_MMU_ITEM_INDEX_REG)   = i;
        REG(SPI_MEM_S_MMU_ITEM_CONTENT_REG) = i | (1u << 11) | (1u << 10);
    }
    esp32p4_extmem_invalidate(PSRAM_CACHED_BASE, bytes);
}

/* ---- bring-up -------------------------------------------------------------- */

void psram_init(void) {
    uint64_t t0 = time_get_us();
    ldo2_on();
    if (!mpll_on()) { g_reason = "the MPLL did not calibrate"; goto fail; }
    ctrl_setup();
    mode_regs_init();
    if (!connected_check()) { g_reason = "no answer from the PSRAM (write/read check)"; goto fail; }
    for (uint32_t a = 0; a <= 8; a += 2) {
        uint16_t v = mr_read16(a);
        g_mr[a] = (uint8_t)v;
        g_mr[a + 1] = (uint8_t)(v >> 8);
    }
    switch (g_mr[2] & 0x7u) {               /* MR2 density */
    case 0x1: g_bytes =  4u << 20; break;
    case 0x3: g_bytes =  8u << 20; break;
    case 0x5: g_bytes = 16u << 20; break;
    case 0x7: g_bytes = 32u << 20; break;
    default:  g_bytes = 0;         break;
    }
    if (g_bytes < (uint32_t)CONFIG_PSRAM_BYTES) {
        g_reason = g_bytes ? "the PSRAM is smaller than the board file says" : "unknown MR2 density";
        goto fail;
    }
    g_bytes = (uint32_t)CONFIG_PSRAM_BYTES;   /* what the linker was told */
    if (CONFIG_PSRAM_SPEED_MHZ != PSRAM_INIT_MHZ && !tune(CONFIG_PSRAM_SPEED_MHZ)) {
        /* No window, or no margin: stay at the speed that needs none, and
         * say so -- a board quietly at 20 MHz is a thing to find in the log. */
        printk("[PSRAM] timing tuning found no safe window at %u MHz (phases 0x%x, "
               "delay lines 0x%08lx); staying at %u MHz\n",
               (unsigned)CONFIG_PSRAM_SPEED_MHZ, (unsigned)g_tune_phase_ok,
               (unsigned long)g_tune_dl_ok, (unsigned)PSRAM_INIT_MHZ);
        dqs_phase(0);
        delaylines(0, 0);
        bus_clock(PSRAM_INIT_MHZ);
    }
    cache_port_setup();
    mmu_map(g_bytes);

    /* BULK_BSS is NOLOAD in PSRAM: nobody else zeroes it (kernel/palloc.h). */
    memset(_bulk_bss_start, 0, (size_t)(_bulk_bss_end - _bulk_bss_start));
    esp32p4_extmem_writeback((uintptr_t)_bulk_bss_start, (uint32_t)(_bulk_bss_end - _bulk_bss_start));

    g_up = true;
    g_reason = "up";
    printk("[PSRAM] %lu MB at 0x%08lx, vendor 0x%02x, hex DDR %u MHz, LDO2 dref %u mul %u (%s), "
           "%lu us\n", (unsigned long)(g_bytes >> 20), (unsigned long)PSRAM_CACHED_BASE,
           (unsigned)(g_mr[1] & 0x1Fu), (unsigned)g_speed_mhz,
           (unsigned)g_ldo_dref, (unsigned)g_ldo_mul, g_ldo_from_efuse ? "eFuse" : "default",
           (unsigned long)(time_get_us() - t0));
    return;
fail:
    printk("[PSRAM] not up: %s\n", g_reason);
}

bool psram_is_up(void)     { return g_up; }
uint32_t psram_bytes(void) { return g_up ? g_bytes : 0u; }

void psram_require(void) {
    if (g_up) return;
    for (;;) {
        cprintf("\n[PSRAM] HALTED: this persona is built for its %lu KB PSRAM, and %s. "
                "Nothing else will start (plan/phase38_psram.md, S1). "
                "The board can still be reflashed over USB.\n",
                (unsigned long)(CONFIG_PSRAM_BYTES / 1024), g_reason);
        task_sleep_ms(5000);
    }
}

int psram_meminfo(char *buf, uint32_t cap) {
    if (!g_up) return ksnprintf(buf, cap, "PSRAM: not up (%s)\n", g_reason);
    return ksnprintf(buf, cap, "PSRAM: %lu KB at 0x%08lx, hex DDR %u MHz, BULK_BSS %lu KB\n",
                     (unsigned long)(g_bytes / 1024), (unsigned long)PSRAM_CACHED_BASE,
                     (unsigned)g_speed_mhz,
                     (unsigned long)((uintptr_t)(_bulk_bss_end - _bulk_bss_start) / 1024));
}

/* ---- psram test / bench ---------------------------------------------------- */

static uint32_t pat(uint32_t i, uint32_t salt) {
    uint32_t x = i * 2654435761u + salt;
    return x ^ (x >> 15);
}

/* Memory for the test and the bench comes from the bulk zone like anyone
 * else's -- /ram0 and the Lisp pools live there and must survive a `psram
 * test`. NULL (and the reason printed) if the zone cannot give `n` pages of
 * PSRAM; a fallback into SRAM is handed straight back. */
static void *bulk_claim(uint32_t n, const char *who) {
    void *p = palloc_pages_bulk(n);
    if (p && !palloc_is_bulk(p)) { palloc_free(p, n); p = NULL; }
    if (!p) cprintf("%s: the bulk zone has no free run of %lu pages\n", who, (unsigned long)n);
    return p;
}

/* Address-in-data patterns over the largest free run of the bulk zone (or
 * `mb` MB of it), twice with different salts; each read pass follows a
 * write-back and invalidate of the range, so it comes from the chip and not
 * from L1/L2. */
static void psram_test(uint32_t mb) {
    palloc_zone_stats_t st;
    if (!palloc_bulk_stats(&st)) { cprintf("psram test: no bulk zone\n"); return; }
    uint32_t n = st.largest_free_run;
    if (mb && (mb << 8) < n) n = mb << 8;           /* 256 pages per MB */
    uint8_t *mem = bulk_claim(n, "psram test");
    if (!mem) return;
    uintptr_t lo = (uintptr_t)mem;
    uint32_t bytes = n * 4096u, words = bytes / 4u, bad = 0;
    for (uint32_t salt = 0x1234u; salt <= 0x1235u; salt++) {
        uint64_t t0 = time_get_us();
        volatile uint32_t *p = (volatile uint32_t *)lo;
        for (uint32_t i = 0; i < words; i++) p[i] = pat(i, salt);
        esp32p4_extmem_writeback(lo, bytes);
        esp32p4_extmem_invalidate(lo, bytes);
        for (uint32_t i = 0; i < words; i++) {
            uint32_t v = p[i], e = pat(i, salt);
            if (v != e && bad++ < 4)
                cprintf("  0x%08lx: 0x%08lx, expected 0x%08lx\n",
                        (unsigned long)(lo + 4u * i), (unsigned long)v, (unsigned long)e);
        }
        cprintf("psram test: %lu KB at 0x%08lx, pass %u, %lu ms\n",
                (unsigned long)(bytes / 1024), (unsigned long)lo, (unsigned)(salt - 0x1233u),
                (unsigned long)((time_get_us() - t0) / 1000u));
    }
    palloc_free(mem, n);
    cprintf("psram test: %s (%lu bad words)\n", bad ? "FAIL" : "PASS", (unsigned long)bad);
}

static void psram_bench(void) {
    enum { PAGES = 256u, BYTES = PAGES * 4096u };
    uint8_t *mem = bulk_claim(PAGES, "psram bench");
    if (!mem) return;
    uintptr_t base = (uintptr_t)mem;
    volatile uint32_t *p = (volatile uint32_t *)base;
    uint64_t t0 = time_get_us();
    for (uint32_t i = 0; i < BYTES / 4u; i++) p[i] = i;
    esp32p4_extmem_writeback(base, BYTES);
    uint64_t t1 = time_get_us();
    esp32p4_extmem_invalidate(base, BYTES);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < BYTES / 4u; i++) sum += p[i];
    uint64_t t2 = time_get_us();
    palloc_free(mem, PAGES);
    cprintf("psram bench: 1 MB write+writeback %lu us (%lu KB/s), read %lu us (%lu KB/s), sum %08lx\n",
            (unsigned long)(t1 - t0), (unsigned long)(1024ull * 1000000ull / (t1 - t0 + 1)),
            (unsigned long)(t2 - t1), (unsigned long)(1024ull * 1000000ull / (t2 - t1 + 1)),
            (unsigned long)sum);
}

/* 47.4b: the flash-fill-versus-dirty-PSRAM-eviction stress. Each round
 * dirties 256 KB of PSRAM (twice the L2) without a write-back, then reads the
 * whole XIP image one cache line at a time, so every flash fill has to evict
 * a dirty PSRAM line. The flash checksum must be the same every round: a
 * fault traps ("[Trap Cache]"), a wrong fill shows as a different sum. */
extern char _xip_start[], _xip_end[];

static void psram_evict(uint32_t rounds) {
    enum { PAGES = 64u, BYTES = PAGES * 4096u };
    uint8_t *mem = bulk_claim(PAGES, "psram evict");
    if (!mem) return;
    volatile uint32_t *p = (volatile uint32_t *)mem;
    const uintptr_t f0 = (uintptr_t)_xip_start, f1 = (uintptr_t)_xip_end;
    uint32_t first = 0, bad = 0;
    uint64_t t0 = time_get_us();
    for (uint32_t r = 0; r < rounds; r++) {
        for (uint32_t i = 0; i < BYTES / 4u; i += 16u) p[i] = r + i;   /* one store per 64 B line */
        uint32_t sum = 0;
        for (uintptr_t a = f0; a < f1; a += 64u) sum += *(volatile uint32_t *)a;
        if (r == 0) first = sum;
        else if (sum != first && bad++ < 4)
            cprintf("  round %lu: flash sum %08lx, expected %08lx\n", (unsigned long)r,
                    (unsigned long)sum, (unsigned long)first);
    }
    esp32p4_extmem_writeback((uintptr_t)mem, BYTES);
    palloc_free(mem, PAGES);
    cprintf("psram evict: %lu rounds, %lu KB flash per round, %lu ms: %s (%lu bad rounds)\n",
            (unsigned long)rounds, (unsigned long)((f1 - f0) / 1024u),
            (unsigned long)((time_get_us() - t0) / 1000u), bad ? "FAIL" : "PASS", (unsigned long)bad);
}

void psram_command(const char *args) {
    while (*args == ' ') args++;
    if (!g_up) {
        cprintf("psram: not up -- %s\n", g_reason);
        return;
    }
    if (strncmp(args, "test", 4) == 0) {
        const char *q = args + 4;
        uint32_t mb = 0;
        while (*q == ' ') q++;
        while (*q >= '0' && *q <= '9') mb = mb * 10u + (uint32_t)(*q++ - '0');
        psram_test(mb);
    } else if (strcmp(args, "bench") == 0) {
        psram_bench();
    } else if (strncmp(args, "evict", 5) == 0) {
        const char *q = args + 5;
        uint32_t n = 0;
        while (*q == ' ') q++;
        while (*q >= '0' && *q <= '9') n = n * 10u + (uint32_t)(*q++ - '0');
        psram_evict(n ? n : 100u);
    } else if (*args == '\0') {
        cprintf("psram: %lu KB, vendor 0x%02x, MR0-8 %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                (unsigned long)(g_bytes / 1024), (unsigned)(g_mr[1] & 0x1Fu),
                g_mr[0], g_mr[1], g_mr[2], g_mr[3], g_mr[4], g_mr[5], g_mr[6], g_mr[7], g_mr[8]);
        cprintf("       hex DDR, %u MHz (MPLL %u / %u), LDO2 dref %u mul %u (%s)\n",
                (unsigned)g_speed_mhz, (unsigned)MPLL_MHZ, (unsigned)BUS_DIV_FOR(g_speed_mhz),
                (unsigned)g_ldo_dref, (unsigned)g_ldo_mul, g_ldo_from_efuse ? "eFuse" : "default");
        if (g_tune_phase >= 0) {
            unsigned d = 0, q = 0;
            if (g_tune_dl >= 0) dl_config((unsigned)g_tune_dl, &d, &q);
            cprintf("       tuning: phases ok 0x%x -> phase %d; delay lines ok 0x%08lx -> #%d {data %u, dqs %u}\n",
                    (unsigned)g_tune_phase_ok, (int)g_tune_phase,
                    (unsigned long)g_tune_dl_ok, (int)g_tune_dl, d, q);
        }
        cprintf("       cached 0x%08lx; BULK_BSS %lu KB at 0x%08lx\n",
                (unsigned long)PSRAM_CACHED_BASE,
                (unsigned long)((uintptr_t)(_bulk_bss_end - _bulk_bss_start) / 1024),
                (unsigned long)(uintptr_t)_bulk_bss_start);
        palloc_zone_stats_t st;
        if (palloc_bulk_stats(&st)) {
            cprintf("       bulk zone %lu pages at 0x%08lx: %lu free (largest run %lu), peak %lu used, "
                    "%lu SRAM fallbacks\n", (unsigned long)st.total_pages, (unsigned long)st.base,
                    (unsigned long)st.free_pages, (unsigned long)st.largest_free_run,
                    (unsigned long)st.peak_used_pages, (unsigned long)st.fallbacks);
        }
    } else {
        cprintf("usage: psram [test [MB]|bench]\n");
    }
}

#endif /* CONFIG_BOARD_ESP32P4 && CONFIG_PSRAM_BYTES */
