#include "drivers/hdmi_jh7110.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include <string.h>

#define SYS_CRG_BASE        0x13020000UL
#define SYS_SYSCON_BASE     0x13030000UL
#define SYS_IOMUX_BASE      0x13040000UL

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* Innosilicon HDMI TX register indices (each spaced by 4 bytes) */
#define HDMI_SYS_CTRL       0x00u
#define HDMI_VIDEO_CTRL1    0x01u
#define HDMI_VIDEO_CTRL2    0x02u
#define HDMI_VIDEO_CTRL3    0x04u
#define HDMI_DDC_BUS_FREQ_L 0x4Bu
#define HDMI_DDC_BUS_FREQ_H 0x4Cu
#define HDMI_EDID_SEGMENT   0x4Du
#define HDMI_EDID_WORD_ADDR 0x4Eu
#define HDMI_EDID_FIFO_OFF  0x4Fu
#define HDMI_EDID_FIFO_DATA 0x50u
#define HDMI_INT_MASK1      0xC0u
#define HDMI_INT_STATUS1    0xC1u
#define HDMI_STATUS         0xC8u
#define HDMI_CTRL_9F        0x9Fu
#define HDMI_CTRL_A0        0xA0u
#define HDMI_CTRL_A2        0xA2u
#define HDMI_CTRL_A3        0xA3u
#define HDMI_CTRL_A4        0xA4u
#define HDMI_CTRL_A5        0xA5u
#define HDMI_CTRL_A6        0xA6u
#define HDMI_CTRL_A7        0xA7u
#define HDMI_CTRL_C9        0xC9u
#define HDMI_CTRL_CE        0xCEu
#define HDMI_PHY_1A0        0x1A0u
#define HDMI_PHY_1A1        0x1A1u
#define HDMI_PHY_1A2        0x1A2u
#define HDMI_PHY_1A3        0x1A3u
#define HDMI_PHY_1A4        0x1A4u
#define HDMI_PHY_1A5        0x1A5u
#define HDMI_PHY_1A6        0x1A6u
#define HDMI_PHY_1A9        0x1A9u
#define HDMI_PHY_1AA        0x1AAu
#define HDMI_PHY_1AB        0x1ABu
#define HDMI_PHY_1AC        0x1ACu
#define HDMI_PHY_1AD        0x1ADu
#define HDMI_PHY_1AF        0x1AFu
#define HDMI_PHY_1B0        0x1B0u
#define HDMI_PHY_1B2        0x1B2u
#define HDMI_PHY_1B4        0x1B4u
#define HDMI_PHY_1BE        0x1BEu
#define HDMI_PHY_1CC        0x1CCu

#define HDMI_STATUS_HOTPLUG (1u << 7)
#define HDMI_INT_EDID_READY (1u << 2)

typedef struct {
    uint16_t reg;
    uint8_t  val;
} hdmi_phy_reg_t;

/* PLL configuration for 1080p60 (148.5 MHz pixel clock) */
static const hdmi_phy_reg_t k_pll_cfg_1080p[] = {
    {0x1a0, 0x01},
    {0x1aa, 0x0f},
    {0x1a1, 0x01},
    {0x1a2, 0xf0},
    {0x1a3, 0x63},
    {0x1a4, 0x15},
    {0x1a5, 0x41},
    {0x1a6, 0x42},
    {0x1ab, 0x01},
    {0x1ac, 0x14},
    {0x1ad, 0x01},
    {0x1aa, 0x0e},
    {0x1a0, 0x00},
};

/* PLL configuration for 720p60 (74.25 MHz pixel clock) */
static const hdmi_phy_reg_t k_pll_cfg_720p[] = {
    {0x1a0, 0x01},
    {0x1aa, 0x0f},
    {0x1a1, 0x01},
    {0x1a2, 0xf0},
    {0x1a3, 0x63},
    {0x1a4, 0x1a},
    {0x1a5, 0x41},
    {0x1a6, 0x64},
    {0x1ab, 0x01},
    {0x1ac, 0x14},
    {0x1ad, 0x01},
    {0x1aa, 0x0e},
    {0x1a0, 0x00},
};

/* single task: driver initialization and shell diagnostics; guarded by: lockless read after init */
static hdmi_monitor_info_t g_hdmi_info;
static bool g_hdmi_initialized = false;

static inline uint8_t hdmi_readb(uint16_t offset) {
    return (uint8_t)(REG32(JH7110_HDMI_BASE + ((uint32_t)offset * 4u)) & 0xFFu);
}

static inline void hdmi_writeb(uint16_t offset, uint8_t val) {
    REG32(JH7110_HDMI_BASE + ((uint32_t)offset * 4u)) = val;
}

static inline void hdmi_modb(uint16_t offset, uint8_t mask, uint8_t val) {
    uint8_t cur = hdmi_readb(offset);
    cur = (uint8_t)((cur & ~mask) | (val & mask));
    hdmi_writeb(offset, cur);
}

/* Turn on Power Domain 4 (VOUT) via AON PMU */
static int jh7110_pmu_vout_on(void) {
    volatile uint32_t *pmu = (volatile uint32_t *)JH7110_AON_PMU_BASE;
    uint32_t domain_id = 4u; /* PMU_DOMAIN_VOUT */
    uint32_t bit = (1u << domain_id);

    /* Check if already powered on */
    if (pmu[0x80u / 4u] & bit) {
        return 0;
    }

    /* SW_TURN_ON_POWER_MODE (0x0C) */
    pmu[0x0Cu / 4u] = bit;

    /* SW_ENCOURAGE (0x44) handshake: 0xFF -> 0x05 -> 0x50 */
    pmu[0x44u / 4u] = 0xFFu;
    pmu[0x44u / 4u] = 0x05u;
    pmu[0x44u / 4u] = 0x50u;

    uint64_t deadline = time_get_us() + 100000ULL; /* 100 ms */
    while (!(pmu[0x80u / 4u] & bit)) {
        if (time_get_us() > deadline) {
            printk("[HDMI] AON PMU VOUT power domain 4 turn-on timed out!\n");
            return -1;
        }
        time_delay_us(50);
    }
    return 0;
}

/* Configure SYS_CRG and VOUT_CRG clocks and deassert resets */
static void jh7110_vout_clocks_and_resets(void) {
    /* 1. SYS_CRG clocks for VOUT:
     *    0x0E8: VOUT_SRC
     *    0x0EC: VOUT_AXI
     *    0x0F0: NOC_BUS_CLK_DISP_AXI
     *    0x0F4: VOUT_TOP_CLK_VOUT_AHB
     *    0x0F8: VOUT_TOP_CLK_VOUT_AXI
     *    0x0FC: VOUT_TOP_CLK_HDMITX0_MCLK */
    REG32(SYS_CRG_BASE + 0x0E8u) |= (1u << 31);
    REG32(SYS_CRG_BASE + 0x0ECu) |= (1u << 31);
    REG32(SYS_CRG_BASE + 0x0F0u) |= (1u << 31);
    REG32(SYS_CRG_BASE + 0x0F4u) |= (1u << 31);
    REG32(SYS_CRG_BASE + 0x0F8u) |= (1u << 31);
    REG32(SYS_CRG_BASE + 0x0FCu) |= (1u << 31);

    /* 2. Deassert SYS_CRG resets for VOUT:
     *    0x2F8 (ASSERT0): clear bit 26 (RSTN_U0_NOC_BUS_DISP_AXI_N)
     *    0x2FC (ASSERT1): clear bit 11 (RSTN_U0_DOM_VOUT_TOP_SRC) */
    REG32(SYS_CRG_BASE + 0x2F8u) &= ~(1u << 26);
    REG32(SYS_CRG_BASE + 0x2FCu) &= ~(1u << 11);

    time_delay_us(1000);

    /* 3. VOUT_CRG clocks (0x295C0000):
     *    0x04: DC8200_PIX0 divider: 1188 MHz / 8 = 148.5 MHz (for 1080p60)
     *    0x10: U0_DC8200_CLK_AXI
     *    0x14: U0_DC8200_CLK_CORE
     *    0x18: U0_DC8200_CLK_AHB
     *    0x1C: U0_DC8200_CLK_PIX0 (Parent 0 = DC8200_PIX0 @ 148.5 MHz, Gate=1)
     *    0x20: U0_DC8200_CLK_PIX1
     *    0x24: DOM_VOUT_TOP_LCD_CLK (Parent 0 = PIX0_OUT, Gate=1)
     *    0x3C: U0_HDMI_TX_CLK_MCLK
     *    0x40: U0_HDMI_TX_CLK_BCLK
     *    0x44: U0_HDMI_TX_CLK_SYS */
    REG32(JH7110_VOUTCRG_BASE + 0x04u) = 8u; /* 1188 MHz / 8 = 148.5 MHz */
    REG32(JH7110_VOUTCRG_BASE + 0x10u) |= (1u << 31);
    REG32(JH7110_VOUTCRG_BASE + 0x14u) |= (1u << 31);
    REG32(JH7110_VOUTCRG_BASE + 0x18u) |= (1u << 31);
    REG32(JH7110_VOUTCRG_BASE + 0x1Cu) = (1u << 31); /* Parent 0, Gate 1 */
    REG32(JH7110_VOUTCRG_BASE + 0x20u) = (1u << 31);
    REG32(JH7110_VOUTCRG_BASE + 0x24u) = (1u << 31); /* Parent 0, Gate 1 */
    REG32(JH7110_VOUTCRG_BASE + 0x3Cu) |= (1u << 31);
    REG32(JH7110_VOUTCRG_BASE + 0x40u) |= (1u << 31);
    REG32(JH7110_VOUTCRG_BASE + 0x44u) |= (1u << 31);

    /* 4. Deassert VOUT_CRG resets (0x295C0048):
     *    bit 0: RSTN_U0_DC8200_AXI
     *    bit 1: RSTN_U0_DC8200_AHB
     *    bit 2: RSTN_U0_DC8200_CORE
     *    bit 9: RSTN_U0_HDMI_TX_HDMI */
    REG32(JH7110_VOUTCRG_BASE + 0x48u) &= ~((1u << 0) | (1u << 1) | (1u << 2) | (1u << 9));

    time_delay_us(2000);

    /* 5. Configure dssctrl video mux (0x295B0000) for DC8200 -> HDMI TX */
    REG32(JH7110_DSSCTRL_BASE + 0x04u) |= (1u << 20);
    REG32(JH7110_DSSCTRL_BASE + 0x08u) |= (1u << 3);

    /* 6. Configure SYS_IOMUX pinmux for HDMI DDC (GPIO 0/1) and HPD (GPIO 15) */
    /* Pad configuration: GPIO 0 (SCL) IE=1 PU=1, GPIO 1 (SDA) IE=1 PU=1, GPIO 15 (HPD) IE=1 */
    REG32(SYS_IOMUX_BASE + 0x120u + 0u * 4u) = 0x09u;
    REG32(SYS_IOMUX_BASE + 0x120u + 1u * 4u) = 0x09u;
    REG32(SYS_IOMUX_BASE + 0x120u + 15u * 4u) = 0x01u;

    /* Output enable: GPIO 0 = 3 (SCL), GPIO 1 = 4 (SDA), GPIO 15 = 1 (OEN_HIGH) */
    uint32_t doen0 = REG32(SYS_IOMUX_BASE + 0x00u);
    doen0 &= ~((0x3Fu << 0) | (0x3Fu << 8));
    doen0 |= (3u << 0) | (4u << 8);
    REG32(SYS_IOMUX_BASE + 0x00u) = doen0;

    uint32_t doen3 = REG32(SYS_IOMUX_BASE + 0x0Cu);
    doen3 &= ~(0x3Fu << 24);
    doen3 |= (1u << 24);
    REG32(SYS_IOMUX_BASE + 0x0Cu) = doen3;

    /* Output data: GPIO 0 = 11 (SCL OUT), GPIO 1 = 12 (SDA OUT) */
    uint32_t dout0 = REG32(SYS_IOMUX_BASE + 0x40u);
    dout0 &= ~((0x7Fu << 0) | (0x7Fu << 8));
    dout0 |= (11u << 0) | (12u << 8);
    REG32(SYS_IOMUX_BASE + 0x40u) = dout0;

    /* Input signal routing: SCL_IN (sig 6) <- GPIO 0 (2), SDA_IN (sig 7) <- GPIO 1 (3), HPD (sig 8) <- GPIO 15 (17) */
    uint32_t din1 = REG32(SYS_IOMUX_BASE + 0x84u);
    din1 &= ~((0x7Fu << 16) | (0x7Fu << 24));
    din1 |= (2u << 16) | (3u << 24);
    REG32(SYS_IOMUX_BASE + 0x84u) = din1;

    uint32_t din2 = REG32(SYS_IOMUX_BASE + 0x88u);
    din2 &= ~(0x7Fu << 0);
    din2 |= (17u << 0);
    REG32(SYS_IOMUX_BASE + 0x88u) = din2;
}

/* Read 128-byte EDID block from connected monitor via DDC I2C */
static int jh7110_hdmi_read_edid(uint8_t *out_edid) {
    /* Setup DDC bus frequency (100 kHz I2C clock from ~51.2-148.5 MHz clock) */
    hdmi_writeb(HDMI_DDC_BUS_FREQ_L, 0x80u);
    hdmi_writeb(HDMI_DDC_BUS_FREQ_H, 0x00u);

    /* Clear and unmute interrupt */
    hdmi_writeb(HDMI_INT_MASK1, HDMI_INT_EDID_READY);
    hdmi_writeb(HDMI_INT_STATUS1, HDMI_INT_EDID_READY);

    /* Trigger EDID read: Block 0, offset 0 */
    hdmi_writeb(HDMI_EDID_FIFO_OFF, 0x00u);
    hdmi_writeb(HDMI_EDID_WORD_ADDR, 0x00u);
    hdmi_writeb(HDMI_EDID_SEGMENT, 0x00u);

    /* Poll for EDID Ready (timeout 150 ms) */
    uint64_t deadline = time_get_us() + 150000ULL;
    while (!(hdmi_readb(HDMI_INT_STATUS1) & HDMI_INT_EDID_READY)) {
        if (time_get_us() > deadline) {
            hdmi_writeb(HDMI_INT_MASK1, 0x00u);
            return -1;
        }
        time_delay_us(500);
    }

    /* Clear interrupt flag and mute */
    hdmi_writeb(HDMI_INT_STATUS1, HDMI_INT_EDID_READY);
    hdmi_writeb(HDMI_INT_MASK1, 0x00u);

    /* Read 128 bytes from FIFO */
    for (int i = 0; i < 128; i++) {
        out_edid[i] = hdmi_readb(HDMI_EDID_FIFO_DATA);
    }

    return 0;
}

/* Parse 128-byte EDID block into structured monitor info */
static void jh7110_hdmi_parse_edid(const uint8_t *edid, hdmi_monitor_info_t *info) {
    /* Check EDID header: 00 FF FF FF FF FF FF 00 */
    static const uint8_t k_header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    if (memcmp(edid, k_header, 8) != 0) {
        info->edid_valid = false;
        return;
    }

    /* Verify checksum (sum of all 128 bytes mod 256 == 0) */
    uint8_t csum = 0;
    for (int i = 0; i < 128; i++) {
        csum = (uint8_t)(csum + edid[i]);
    }
    if (csum != 0) {
        info->edid_valid = false;
        return;
    }
    info->edid_valid = true;

    /* Manufacturer ID: 2 bytes, 3 compressed 5-bit characters (ASCII 'A' - 1 + val) */
    uint16_t mfg = (uint16_t)((edid[8] << 8) | edid[9]);
    info->mfg_id[0] = (char)(((mfg >> 10) & 0x1f) + 'A' - 1);
    info->mfg_id[1] = (char)(((mfg >> 5) & 0x1f) + 'A' - 1);
    info->mfg_id[2] = (char)((mfg & 0x1f) + 'A' - 1);
    info->mfg_id[3] = '\0';

    info->product_code = (uint16_t)(edid[10] | (edid[11] << 8));
    info->serial_number = (uint32_t)(edid[12] | (edid[13] << 8) | (edid[14] << 16) | (edid[15] << 24));

    /* Physical dimensions in cm */
    info->width_cm = edid[21];
    info->height_cm = edid[22];

    /* Detailed Timing Descriptor 1 (bytes 54..71) */
    uint16_t pclk_10khz = (uint16_t)(edid[54] | (edid[55] << 8));
    if (pclk_10khz > 0) {
        info->pixel_clock_khz = (uint32_t)pclk_10khz * 10u;
        info->active_w = (uint16_t)(edid[56] | ((edid[58] & 0xF0u) << 4));
        info->active_h = (uint16_t)(edid[59] | ((edid[61] & 0xF0u) << 4));
    } else {
        /* Fallback if no DTD1 */
        info->active_w = 1920;
        info->active_h = 1080;
        info->pixel_clock_khz = 148500;
    }

    /* Compute DPI: (width_pixels * 25.4) / (width_cm * 10) */
    if (info->width_cm > 0 && info->active_w > 0) {
        info->dpi = (uint16_t)(((uint32_t)info->active_w * 254u) / ((uint32_t)info->width_cm * 100u));
    } else {
        info->dpi = 96; /* standard desktop display default */
    }

    /* Target resolution selection: 1080p if native is >= 1920, else 720p */
    if (info->active_w >= 1920 && info->active_h >= 1080) {
        info->target_res = HDMI_RES_1920_1080P_60HZ;
    } else {
        info->target_res = HDMI_RES_1280_720P_60HZ;
    }
}

/* Configure Innosilicon HDMI TX PHY and serializers */
static int jh7110_hdmi_configure_phy(hdmi_resolution_t res) {
    /* 1. Detect sequence */
    uint8_t val = hdmi_readb(HDMI_PHY_1B0);
    val |= 0x04u;
    hdmi_writeb(HDMI_PHY_1B0, val);
    hdmi_writeb(HDMI_PHY_1CC, 0x0Fu);

    /* 2. Power down PHY */
    hdmi_writeb(HDMI_SYS_CTRL, 0x63u);

    /* 3. Program PLL registers */
    const hdmi_phy_reg_t *cfg = (res == HDMI_RES_1920_1080P_60HZ) ? k_pll_cfg_1080p : k_pll_cfg_720p;
    size_t count = (res == HDMI_RES_1920_1080P_60HZ)
        ? (sizeof(k_pll_cfg_1080p) / sizeof(k_pll_cfg_1080p[0]))
        : (sizeof(k_pll_cfg_720p) / sizeof(k_pll_cfg_720p[0]));

    for (size_t i = 0; i < count; i++) {
        hdmi_writeb(cfg[i].reg, cfg[i].val);
    }

    /* 4. Turn on pre-PLL and post-PLL */
    val = (uint8_t)(hdmi_readb(HDMI_PHY_1A0) & ~0x01u);
    hdmi_writeb(HDMI_PHY_1A0, val);
    val = (uint8_t)(hdmi_readb(HDMI_PHY_1AA) & ~0x01u);
    hdmi_writeb(HDMI_PHY_1AA, val);

    /* 5. Wait for PLL locks */
    uint64_t deadline = time_get_us() + 100000ULL; /* 100 ms */
    while (!(hdmi_readb(HDMI_PHY_1A9) & 0x01u)) {
        if (time_get_us() > deadline) {
            printk("[HDMI] Pre-PLL lock timed out!\n");
            return -1;
        }
        time_delay_us(100);
    }

    deadline = time_get_us() + 100000ULL;
    while (!(hdmi_readb(HDMI_PHY_1AF) & 0x01u)) {
        if (time_get_us() > deadline) {
            printk("[HDMI] Post-PLL lock timed out!\n");
            return -1;
        }
        time_delay_us(100);
    }

    /* 6. Turn on LDO and serializer */
    hdmi_writeb(HDMI_PHY_1B4, 0x07u);
    hdmi_writeb(HDMI_PHY_1BE, 0x70u);

    /* Eye diagram improvement (VIC 16 / 31) */
    hdmi_writeb(0x1BFu, 0x02u);
    hdmi_writeb(0x1C0u, 0x22u);

    /* Set DC8200 pixel clock divider: 1188 MHz / 8 = 148.5 MHz for 1080p, / 16 for 720p */
    REG32(JH7110_VOUTCRG_BASE + 0x04u) = (res == HDMI_RES_1920_1080P_60HZ) ? 8u : 16u;

    /* Video input and color space control:
     * 0x01: DE_EXTERNAL (bit 0 = 1)
     * 0x02: 8-bit input RGB (bits 5:4 = 3)
     * 0x03: C0/C2 normal (bit 0 = 1)
     * 0x04: SOF disable (bit 3 = 1, bit 4 = 1)
     * 0x05: AV_MUTE clear (all bits 0)
     * 0x52: DVI mode (bit 1 = 0)
     */
    hdmi_writeb(0x01u, 0x01u);
    hdmi_writeb(0x02u, 0x30u);
    hdmi_writeb(0x03u, 0x01u);
    hdmi_writeb(0x04u, 0x10u);
    hdmi_writeb(0x05u, 0x00u);
    hdmi_writeb(0x52u, 0x00u);

    /* 7. Configure external video receiver timing (accepts parallel stream from DC8200) */
    if (res == HDMI_RES_1920_1080P_60HZ) {
        hdmi_writeb(0x08u, 0x0Du); /* EXTERNAL_VIDEO(1) | HSYNC_POS | VSYNC_POS */
        hdmi_writeb(0x09u, 0x98u); /* HTOTAL_L (2200) */
        hdmi_writeb(0x0Au, 0x08u); /* HTOTAL_H */
        hdmi_writeb(0x0Bu, 0x18u); /* HBLANK_L (280) */
        hdmi_writeb(0x0Cu, 0x01u); /* HBLANK_H */
        hdmi_writeb(0x0Du, 0xC0u); /* HDELAY_L (192) */
        hdmi_writeb(0x0Eu, 0x00u); /* HDELAY_H */
        hdmi_writeb(0x0Fu, 0x2Cu); /* HDURATION_L (44) */
        hdmi_writeb(0x10u, 0x00u); /* HDURATION_H */
        hdmi_writeb(0x11u, 0x65u); /* VTOTAL_L (1125) */
        hdmi_writeb(0x12u, 0x04u); /* VTOTAL_H */
        hdmi_writeb(0x13u, 0x2Du); /* VBLANK (45) */
        hdmi_writeb(0x14u, 0x29u); /* VDELAY (41) */
        hdmi_writeb(0x15u, 0x05u); /* VDURATION (5) */
    } else {
        hdmi_writeb(0x08u, 0x0Du); /* EXTERNAL_VIDEO(1) | HSYNC_POS | VSYNC_POS */
        hdmi_writeb(0x09u, 0x72u); /* HTOTAL_L (1650) */
        hdmi_writeb(0x0Au, 0x06u); /* HTOTAL_H */
        hdmi_writeb(0x0Bu, 0x72u); /* HBLANK_L (370) */
        hdmi_writeb(0x0Cu, 0x01u); /* HBLANK_H */
        hdmi_writeb(0x0Du, 0x04u); /* HDELAY_L (260) */
        hdmi_writeb(0x0Eu, 0x01u); /* HDELAY_H */
        hdmi_writeb(0x0Fu, 0x28u); /* HDURATION_L (40) */
        hdmi_writeb(0x10u, 0x00u); /* HDURATION_H */
        hdmi_writeb(0x11u, 0xEEu); /* VTOTAL_L (750) */
        hdmi_writeb(0x12u, 0x02u); /* VTOTAL_H */
        hdmi_writeb(0x13u, 0x1Eu); /* VBLANK (30) */
        hdmi_writeb(0x14u, 0x19u); /* VDELAY (25) */
        hdmi_writeb(0x15u, 0x05u); /* VDURATION (5) */
    }

    /* 8. Configure video format (VIC) */
    uint8_t vic = (res == HDMI_RES_1920_1080P_60HZ) ? 16u : 4u; /* CEA-861 VIC 16 = 1080p60, 4 = 720p60 */
    hdmi_writeb(HDMI_CTRL_9F, 0x06u);
    hdmi_writeb(HDMI_CTRL_A0, 0x82u);
    hdmi_writeb(HDMI_CTRL_A2, 0x0Du);
    hdmi_writeb(HDMI_CTRL_A3, 0x00u);
    hdmi_writeb(HDMI_CTRL_A4, 0x00u);
    hdmi_writeb(HDMI_CTRL_A5, 0x08u);
    hdmi_writeb(HDMI_CTRL_A6, 0x70u);
    hdmi_writeb(HDMI_CTRL_A7, vic);
    hdmi_writeb(HDMI_CTRL_C9, 0x10u); /* Normal mode */

    /* 9. Power on PHY */
    hdmi_writeb(HDMI_SYS_CTRL, 0x61u); /* Power on */

    /* 10. Turn on TMDS drivers */
    hdmi_writeb(HDMI_PHY_1B2, 0x8Fu);
    time_delay_us(50000); /* 50 ms stabilization */

    /* 11. Data sync */
    hdmi_writeb(HDMI_CTRL_CE, 0x00u);
    hdmi_writeb(HDMI_CTRL_CE, 0x01u);

    return 0;
}

int jh7110_hdmi_init(void) {
    if (g_hdmi_initialized) return 0;

    memset(&g_hdmi_info, 0, sizeof(g_hdmi_info));

    /* 1. Turn on VOUT Power Domain */
    if (jh7110_pmu_vout_on() != 0) {
        printk("[HDMI] Failed to power on VOUT domain\n");
        return -1;
    }

    /* 2. Configure VOUT Clocks and Resets */
    jh7110_vout_clocks_and_resets();

    /* 3. Check Hot Plug Detect (HPD) */
    uint8_t status = hdmi_readb(HDMI_STATUS);
    uint32_t gpio_din = REG32(SYS_IOMUX_BASE + 0x118u);
    bool gpio_hpd = (gpio_din & (1u << 15)) != 0;
    g_hdmi_info.connected = ((status & HDMI_STATUS_HOTPLUG) || gpio_hpd);

    if (!g_hdmi_info.connected) {
        printk("[HDMI] No display cable detected (status=0x%02x, gpio15=%u)\n", status, gpio_hpd ? 1 : 0);
        /* Default to 1080p60 anyway so the display pipeline is fully configured */
        g_hdmi_info.active_w = 1920;
        g_hdmi_info.active_h = 1080;
        g_hdmi_info.dpi = 96;
        g_hdmi_info.target_res = HDMI_RES_1920_1080P_60HZ;
    } else {
        g_hdmi_info.active_w = 1920;
        g_hdmi_info.active_h = 1080;
        g_hdmi_info.dpi = 96;
        g_hdmi_info.target_res = HDMI_RES_1920_1080P_60HZ;
    }

    /* 4. Configure HDMI Transmitter PHY & Serializer (powers clock for DDC) */
    if (jh7110_hdmi_configure_phy(g_hdmi_info.target_res) != 0) {
        printk("[HDMI] PHY PLL lock failed\n");
        return -1;
    }

    if (g_hdmi_info.connected) {
        /* 5. Probe EDID via DDC now that transmitter clock is running */
        if (jh7110_hdmi_read_edid(g_hdmi_info.raw_edid) == 0) {
            jh7110_hdmi_parse_edid(g_hdmi_info.raw_edid, &g_hdmi_info);
            printk("[HDMI] Monitor detected: %s (PID 0x%04x), %ux%u @ %u MHz, %u DPI\n",
                   g_hdmi_info.edid_valid ? g_hdmi_info.mfg_id : "Generic",
                   g_hdmi_info.product_code,
                   g_hdmi_info.active_w, g_hdmi_info.active_h,
                   g_hdmi_info.pixel_clock_khz / 1000u,
                   g_hdmi_info.dpi);
            if (g_hdmi_info.target_res == HDMI_RES_1280_720P_60HZ) {
                (void)jh7110_hdmi_configure_phy(g_hdmi_info.target_res);
            }
        } else {
            printk("[HDMI] Cable connected, but DDC EDID read timed out; defaulting to 1080p60\n");
        }
    }

    g_hdmi_initialized = true;
    printk("[HDMI] Innosilicon HDMI 2.0 TX Online (%s, TMDS PLL locked)\n",
           (g_hdmi_info.target_res == HDMI_RES_1920_1080P_60HZ) ? "1080p60" : "720p60");

    return 0;
}

bool jh7110_hdmi_is_connected(void) {
    return g_hdmi_info.connected;
}

const hdmi_monitor_info_t *jh7110_hdmi_get_monitor_info(void) {
    return &g_hdmi_info;
}

void jh7110_hdmi_cmd_status(void) {
    uint8_t st = hdmi_readb(HDMI_STATUS);
    uint32_t gpio_din = REG32(SYS_IOMUX_BASE + 0x118u);
    bool gpio_hpd = (gpio_din & (1u << 15)) != 0;
    cprintf("Innosilicon HDMI 2.0 Transmitter (JH7110 VOUT @ 0x%lx):\n", JH7110_HDMI_BASE);
    cprintf("  Driver State:     %s\n", g_hdmi_initialized ? "ONLINE" : "OFFLINE");
    cprintf("  Hot Plug Detect:  %s (status=0x%02x, gpio15=%u)\n",
            ((st & HDMI_STATUS_HOTPLUG) || gpio_hpd) ? "CONNECTED" : "DISCONNECTED", st, gpio_hpd ? 1 : 0);
    cprintf("  Resolution:       %ux%u @ 60 Hz (%s)\n",
            g_hdmi_info.active_w, g_hdmi_info.active_h,
            (g_hdmi_info.target_res == HDMI_RES_1920_1080P_60HZ) ? "1080p60" : "720p60");
    cprintf("  Pixel Clock:      %u.%03u MHz\n",
            g_hdmi_info.pixel_clock_khz / 1000u, g_hdmi_info.pixel_clock_khz % 1000u);
    cprintf("  Monitor Size:     %u cm x %u cm (%u DPI)\n",
            g_hdmi_info.width_cm, g_hdmi_info.height_cm, g_hdmi_info.dpi);
    if (g_hdmi_info.edid_valid) {
        cprintf("  Manufacturer:     %s (Product ID: 0x%04x, SN: 0x%08x)\n",
                g_hdmi_info.mfg_id, g_hdmi_info.product_code, g_hdmi_info.serial_number);
    }
}

void jh7110_hdmi_cmd_edid(void) {
    if (!g_hdmi_info.connected) {
        cprintf("HDMI: No monitor attached (HPD disconnected).\n");
        return;
    }
    if (!g_hdmi_info.edid_valid) {
        cprintf("HDMI: No valid EDID block received from monitor.\n");
        return;
    }

    cprintf("Monitor EDID Block 0 (128 bytes):\n");
    cprintf("  Manufacturer ID:  %s\n", g_hdmi_info.mfg_id);
    cprintf("  Product Code:     0x%04x\n", g_hdmi_info.product_code);
    cprintf("  Serial Number:    0x%08x (%u)\n", g_hdmi_info.serial_number, g_hdmi_info.serial_number);
    cprintf("  Physical Size:    %u cm x %u cm (Calculated DPI: %u)\n",
            g_hdmi_info.width_cm, g_hdmi_info.height_cm, g_hdmi_info.dpi);
    cprintf("  Native Timing:    %ux%u @ %u.%03u MHz\n",
            g_hdmi_info.active_w, g_hdmi_info.active_h,
            g_hdmi_info.pixel_clock_khz / 1000u, g_hdmi_info.pixel_clock_khz % 1000u);
    cprintf("Raw EDID Hex Dump:\n");
    for (int r = 0; r < 128; r += 16) {
        cprintf("  %02x: ", r);
        for (int c = 0; c < 16; c++) {
            cprintf("%02x ", g_hdmi_info.raw_edid[r + c]);
        }
        cprintf("\n");
    }
}
