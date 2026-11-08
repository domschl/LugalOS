#include "lugalos_config.h"
#include "drivers/emac_jh7110.h"
#include "net/netif.h"
#include "net/ip.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include <string.h>

/* StarFive JH7110 Synopsys DesignWare Ethernet QoS v5.20 (DWC_ether_qos)
 * Gigabit Ethernet MAC driver (`gmac1@16040000` and `gmac0@16030000`) and
 * Motorcomm YT8531C RGMII-ID PHY driver for StarFive VisionFive 2.
 *
 * Architectural notes:
 * 1. U-Boot `booti` tears down active driver-model devices (`DM_REMOVE_ACTIVE_ALL`),
 *    gating `gmac5_clk_rx`/`tx` and asserting resets. `emac_probe()` explicitly
 *    configures SYSCON/AON_SYSCON RGMII mode, enables all CRG clocks, and releases
 *    AHB/AXI resets before touching any EQOS register.
 * 2. DWC_ether_qos `DMA_MODE.SWR` requires both TX and RX RGMII clocks to tick in
 *    order to self-clear, whereas the Clause-22 MDIO controller runs purely off the
 *    CSR clock (`clk_ahb`). We therefore configure the YT8531C PHY over MDIO first
 *    (disabling PHY auto-sleep in extended reg 0x27 so RXC stays active even without
 *    a cable) before issuing `DMA_MODE.SWR`.
 * 3. The JH7110 EQOS DMA engine is non-coherent with the U74 L1/L2 caches. All
 *    descriptor rings and packet buffers live in 64-byte-aligned `.bss` and are
 *    accessed by the CPU through the JH7110 uncached System Port DRAM alias
 *    (`pa + 0x4_00000000`, mapped `PTE_R | PTE_W` in Sv39), while the 32-bit physical
 *    addresses are handed to the EQOS DMA engine.
 */

#define SYS_CRG_BASE        0x13020000UL
#define SYS_SYSCON_BASE     0x13030000UL
#define AON_CRG_BASE        0x17000000UL
#define AON_SYSCON_BASE     0x17010000UL

#define GMAC0_BASE          0x16030000UL
#define GMAC1_BASE          0x16040000UL

#define REG32(addr)         (*(volatile uint32_t *)(uintptr_t)(addr))

static inline void jh7110_mb(void) {
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}


#include "arch/jh7110_cache.h"

/* --- EQOS v5.20 MAC registers (offset from GMAC base) ------------------- */
#define EQOS_MAC_CFG                0x0000UL
#define  MAC_CFG_IPC                (1u << 27)
#define  MAC_CFG_CST                (1u << 21)  /* CRC stripping for Type frames */
#define  MAC_CFG_ACS                (1u << 20)  /* Automatic Pad/CRC stripping */
#define  MAC_CFG_WD                 (1u << 19)  /* Watchdog disable */
#define  MAC_CFG_BE                 (1u << 18)  /* Packet burst enable */
#define  MAC_CFG_JD                 (1u << 17)  /* Jabber disable */
#define  MAC_CFG_JE                 (1u << 16)  /* Jumbo packet enable */
#define  MAC_CFG_PS                 (1u << 15)  /* Port select: 0=1000M GMII, 1=10/100M MII */
#define  MAC_CFG_FES                (1u << 14)  /* Speed: 1=100M, 0=10M (when PS=1) */
#define  MAC_CFG_DM                 (1u << 13)  /* Duplex mode: 1=full, 0=half */
#define  MAC_CFG_LM                 (1u << 12)  /* Loopback mode */
#define  MAC_CFG_TE                 (1u << 1)   /* Transmitter enable */
#define  MAC_CFG_RE                 (1u << 0)   /* Receiver enable */

#define EQOS_MAC_PKT_FILTER         0x0008UL
#define  PKT_FILTER_RA              (1u << 31)  /* Receive all */
#define  PKT_FILTER_PM              (1u << 4)   /* Pass all multicast */
#define  PKT_FILTER_PR              (1u << 0)   /* Promiscuous mode */

#define EQOS_MAC_Q0_TX_FLOW_CTRL    0x0070UL
#define  TX_FLOW_CTRL_PT_SHIFT      16
#define  TX_FLOW_CTRL_TFE           (1u << 1)

#define EQOS_MAC_RX_FLOW_CTRL       0x0090UL
#define  RX_FLOW_CTRL_RFE           (1u << 0)

#define EQOS_MAC_TXQ_PRTY_MAP0      0x0098UL
#define EQOS_MAC_RXQ_CTRL0          0x00A0UL
#define  RXQ0_EN_MASK               0x3u
#define  RXQ0_EN_DCB                0x2u        /* Enable RX Queue 0 for DCB/generic */
#define EQOS_MAC_RXQ_CTRL1          0x00A4UL
#define EQOS_MAC_RXQ_CTRL2          0x00A8UL

#define EQOS_MAC_US_TIC_COUNTER     0x00DCUL
#define EQOS_MAC_VERSION            0x0110UL
#define EQOS_MAC_HW_FEATURE0        0x011CUL
#define EQOS_MAC_HW_FEATURE1        0x0120UL
#define  HW_FEAT1_TXFIFOSIZE_SHIFT  6
#define  HW_FEAT1_TXFIFOSIZE_MASK   0x1fu
#define  HW_FEAT1_RXFIFOSIZE_SHIFT  0
#define  HW_FEAT1_RXFIFOSIZE_MASK   0x1fu

#define EQOS_MAC_MDIO_ADDR          0x0200UL
#define  MDIO_ADDR_PA_SHIFT         21
#define  MDIO_ADDR_RDA_SHIFT        16
#define  MDIO_ADDR_CR_SHIFT         8
#define  MDIO_ADDR_CR_150_250       4u          /* CSR clock 150-250 MHz -> MDC /102 */
#define  MDIO_ADDR_GOC_READ         (3u << 2)
#define  MDIO_ADDR_GOC_WRITE        (1u << 2)
#define  MDIO_ADDR_GB               (1u << 0)
#define EQOS_MAC_MDIO_DATA          0x0204UL

#define EQOS_MAC_ADDR0_HIGH         0x0300UL
#define  MAC_ADDR0_HIGH_AE          (1u << 31)
#define EQOS_MAC_ADDR0_LOW          0x0304UL

/* --- EQOS v5.20 MTL registers ------------------------------------------- */
#define EQOS_MTL_TXQ0_OP_MODE       0x0D00UL
#define  MTL_TXQ0_TQS_SHIFT         16
#define  MTL_TXQ0_TQS_MASK          0x1ffu
#define  MTL_TXQ0_TXQEN_SHIFT       2
#define  MTL_TXQ0_TXQEN_MASK        0x3u
#define  MTL_TXQ0_TXQEN_EN          2u
#define  MTL_TXQ0_TSF               (1u << 1)
#define  MTL_TXQ0_FTQ               (1u << 0)

#define EQOS_MTL_TXQ0_DEBUG         0x0D08UL
#define EQOS_MTL_TXQ0_QUANTUM_W     0x0D18UL

#define EQOS_MTL_RXQ0_OP_MODE       0x0D30UL
#define  MTL_RXQ0_RQS_SHIFT         20
#define  MTL_RXQ0_RQS_MASK          0x3ffu
#define  MTL_RXQ0_RFD_SHIFT         14
#define  MTL_RXQ0_RFD_MASK          0x3fu
#define  MTL_RXQ0_RFA_SHIFT         8
#define  MTL_RXQ0_RFA_MASK          0x3fu
#define  MTL_RXQ0_EHFC              (1u << 7)
#define  MTL_RXQ0_RSF               (1u << 5)
#define  MTL_RXQ0_FEP               (1u << 4)
#define  MTL_RXQ0_FUP               (1u << 3)

#define EQOS_MTL_RXQ0_MISSED_OVF    0x0D34UL
#define EQOS_MTL_RXQ0_DEBUG         0x0D38UL

/* --- EQOS v5.20 DMA registers ------------------------------------------- */
#define EQOS_DMA_MODE               0x1000UL
#define  DMA_MODE_SWR               (1u << 0)

#define EQOS_DMA_SYSBUS_MODE        0x1004UL
#define  SYSBUS_RD_OSR_LMT_SHIFT    16
#define  SYSBUS_EAME                (1u << 11)
#define  SYSBUS_BLEN16              (1u << 3)
#define  SYSBUS_BLEN8               (1u << 2)
#define  SYSBUS_BLEN4               (1u << 1)

#define EQOS_DMA_CH0_CONTROL        0x1100UL
#define  CH0_CTRL_DSL_SHIFT         18
#define  CH0_CTRL_PBLX8             (1u << 16)

#define EQOS_DMA_CH0_TX_CONTROL     0x1104UL
#define  CH0_TX_PBL_SHIFT           16
#define  CH0_TX_PBL_MASK            0x3fu
#define  CH0_TX_OSP                 (1u << 4)
#define  CH0_TX_ST                  (1u << 0)

#define EQOS_DMA_CH0_RX_CONTROL     0x1108UL
#define  CH0_RX_PBL_SHIFT           16
#define  CH0_RX_PBL_MASK            0x3fu
#define  CH0_RX_RBSZ_SHIFT          1
#define  CH0_RX_RBSZ_MASK           0x3fffu
#define  CH0_RX_SR                  (1u << 0)

#define EQOS_DMA_CH0_TXDESC_HADDR   0x1110UL
#define EQOS_DMA_CH0_TXDESC_ADDR    0x1114UL
#define EQOS_DMA_CH0_RXDESC_HADDR   0x1118UL
#define EQOS_DMA_CH0_RXDESC_ADDR    0x111CUL
#define EQOS_DMA_CH0_TXDESC_TAIL    0x1120UL
#define EQOS_DMA_CH0_RXDESC_TAIL    0x1128UL
#define EQOS_DMA_CH0_TXDESC_RLEN    0x112CUL
#define EQOS_DMA_CH0_RXDESC_RLEN    0x1130UL
#define EQOS_DMA_CH0_CUR_TX_DESC    0x1144UL
#define EQOS_DMA_CH0_CUR_RX_DESC    0x114cUL
#define EQOS_DMA_CH0_CUR_TX_BUF     0x1154UL
#define EQOS_DMA_CH0_CUR_RX_BUF     0x115cUL
#define EQOS_DMA_CH0_STATUS         0x1160UL

/* --- Standard Clause-22 & Motorcomm YT8531C PHY registers --------------- */
#define PHY_BMCR                    0u
#define  BMCR_RESET                 (1u << 15)
#define  BMCR_LOOPBACK              (1u << 14)
#define  BMCR_SPEED_100             (1u << 13)
#define  BMCR_AN_ENABLE             (1u << 12)
#define  BMCR_POWERDOWN             (1u << 11)
#define  BMCR_ISOLATE               (1u << 10)
#define  BMCR_AN_RESTART            (1u << 9)
#define  BMCR_FULL_DUPLEX           (1u << 8)
#define  BMCR_SPEED_1000            (1u << 6)

#define PHY_BMSR                    1u
#define  BMSR_AN_COMPLETE           (1u << 5)
#define  BMSR_LINK_UP               (1u << 2)

#define PHY_IDR1                    2u
#define PHY_IDR2                    3u
#define PHY_ANAR                    4u
#define  AN_100BASE_TX_FD           (1u << 8)
#define  AN_100BASE_TX_HD           (1u << 7)
#define  AN_10BASE_T_FD             (1u << 6)
#define  AN_10BASE_T_HD             (1u << 5)
#define  AN_SELECTOR_802_3          0x0001u
#define PHY_ANLPAR                  5u
#define PHY_CTRL1000                9u
#define  ADVERTISE_1000FULL         (1u << 9)
#define  ADVERTISE_1000HALF         (1u << 8)
#define PHY_STAT1000                10u

/* Motorcomm YT8531C specific registers */
#define YT8531_PHY_ID1              0x4f51u
#define YT8531_PHY_ID2              0xe91bu
#define YT8531_SPEC_STATUS          0x11u
#define  YT8531_SPEED_MASK          (3u << 14)
#define  YT8531_SPEED_1000          (2u << 14)
#define  YT8531_SPEED_100           (1u << 14)
#define  YT8531_SPEED_10            (0u << 14)
#define  YT8531_DUPLEX_FULL         (1u << 13)

#define YT8531_DEBUG_ADDR           0x1eu
#define YT8531_DEBUG_DATA           0x1fu
#define YT8531_EXT_SLEEP_CTRL1      0x0027u
#define  YT8531_SLEEP_AUTO_EN       (1u << 15)
#define YT8531_EXT_CHIP_CONFIG      0xa001u
#define  YT8531_RXC_DLY_EN          (1u << 8)
#define YT8531_EXT_RGMII_CFG1       0xa003u
#define  YT8531_TX_CLK_SEL_INV      (1u << 14)
#define YT8531_EXT_PAD_DRIVE        0xa010u

/* --- Descriptor layout (64-byte cacheline aligned) ---------------------- */
#define U74_CACHE_LINE              64u
#define EMAC_RX_DESC_COUNT          8u
#define EMAC_TX_DESC_COUNT          8u
#define EMAC_BUF_SIZE               1536u

typedef struct {
    volatile uint32_t des0;
    volatile uint32_t des1;
    volatile uint32_t des2;
    volatile uint32_t des3;
    uint8_t pad[U74_CACHE_LINE - 16];
} emac_desc_t;

_Static_assert(sizeof(emac_desc_t) == U74_CACHE_LINE,
               "an EQOS descriptor must occupy one 64-byte U74 cache line");

#define DESC3_OWN                   (1u << 31)
#define DESC3_IOC                   (1u << 30)
#define DESC3_FD                    (1u << 29)
#define DESC3_LD                    (1u << 28)
#define DESC3_BUF1V                 (1u << 24)
#define DESC3_ES                    (1u << 15)
#define DESC3_PL_MASK               0x7fffu

#define CACHE_ALIGNED __attribute__((aligned(U74_CACHE_LINE)))

/* guarded by: net_lock() for the active interface; loopback diagnostics take
 * the rings with g_diag_owns_rings under the same lock. */
static CACHE_ALIGNED emac_desc_t g_rx_desc[EMAC_RX_DESC_COUNT];
static CACHE_ALIGNED emac_desc_t g_tx_desc[EMAC_TX_DESC_COUNT];
static CACHE_ALIGNED uint8_t     g_rx_buf[EMAC_RX_DESC_COUNT][EMAC_BUF_SIZE];
static CACHE_ALIGNED uint8_t     g_tx_buf[EMAC_TX_DESC_COUNT][EMAC_BUF_SIZE];

/* guarded by: net_lock() for the active interface; loopback diagnostics take
 * the rings with g_diag_owns_rings under the same lock. */
static uint32_t g_rx_next;
static uint32_t g_tx_next;
static volatile bool g_diag_owns_rings;

/* Active GMAC controller selection:
 * unit 1 = gmac1@16040000 (primary LAN port on VisionFive 2)
 * unit 0 = gmac0@16030000 (secondary port)
 * single task: set during emac_probe() before netsrv starts, or from shell diagnostics. */
static uintptr_t g_gmac_base = GMAC1_BASE;
static int       g_gmac_unit = 1;
static uint8_t   g_phy_addr  = 0;

/* guarded by: net_lock() for the interface. */
static emac_link_t g_link;
static uint64_t    g_link_checked_ms;
static bool        g_link_valid;

/* single task: set at boot in emac_netif_init() before netsrv starts. */
static uint8_t g_station_mac[NETIF_MAC_LEN];
static bool    g_station_mac_valid;
static netif_t g_netif;
static bool    g_netif_registered;

/* guarded by: net_lock() for the interface. */
static const volatile uint8_t *g_rx_held;
static uint32_t                g_rx_held_len;
static uint8_t                 g_last_hdr[14];
static bool                    g_last_hdr_valid;

/* accepted: error statistics for `emac stats`. */
static uint32_t g_rx_hw_errors;
static uint32_t g_rx_err_last_des3;
static uint32_t g_rx_err_seen_bits;
static uint32_t g_tx_hw_errors;
static uint32_t g_tx_err_last_des3;
static uint32_t g_tx_err_seen_bits;


/* --- JH7110 Clock, Reset, and SYSCON bring-up for GMAC0 & GMAC1 ---------- */

static void jh7110_gmac_unit_clks_on(int unit) {
    /* Ensure root GMAC/USB clock and source divider are enabled in SYS_CRG */
    REG32(SYS_CRG_BASE + 0x0180UL) |= (1u << 31);
    REG32(SYS_CRG_BASE + 0x018CUL) = (REG32(SYS_CRG_BASE + 0x018CUL) & ~0x7u) | 2u;

    if (unit == 1) {
        /* SYS_SYSCON + 0x90 (syscfg_36): bits [4:2] = 1 (RGMII mode for GMAC1) */
        uint32_t sc = REG32(SYS_SYSCON_BASE + 0x0090UL);
        sc = (sc & ~(0x7u << 2)) | (0x1u << 2);
        REG32(SYS_SYSCON_BASE + 0x0090UL) = sc;

        /* Enable GMAC1 AHB, AXI, GTXCLK (div=8 -> 125 MHz), RMII_RTX (div=1),
         * PTP, RX, TX (mux=1 -> rmii_rtx), and GTXC clocks in SYS_CRG. */
        REG32(SYS_CRG_BASE + 0x0184UL) |= (1u << 31);   /* gmac5_clk_ahb */
        REG32(SYS_CRG_BASE + 0x0188UL) |= (1u << 31);   /* gmac5_clk_axi */
        REG32(SYS_CRG_BASE + 0x0190UL) = (REG32(SYS_CRG_BASE + 0x0190UL) & ~0xfu) | 8u;
        REG32(SYS_CRG_BASE + 0x0194UL) = (REG32(SYS_CRG_BASE + 0x0194UL) & ~0x1fu) | 1u;
        REG32(SYS_CRG_BASE + 0x0198UL) = 0x8000000au;   /* gmac5_clk_ptp */
        REG32(SYS_CRG_BASE + 0x019CUL) = 0x80000020u;   /* gmac5_clk_rx */
        REG32(SYS_CRG_BASE + 0x01A0UL) = 0x40000000u;   /* gmac5_clk_rx_inv */
        REG32(SYS_CRG_BASE + 0x01A4UL) = 0x81000000u;   /* gmac5_clk_tx (enable + mux=1) */
        REG32(SYS_CRG_BASE + 0x01A8UL) = 0x40000000u;   /* gmac5_clk_tx_inv */
        REG32(SYS_CRG_BASE + 0x01ACUL) = 0x80000020u;   /* gmac1_gtxc */
        jh7110_mb();

        /* Pulse and release GMAC1 AXI (id 66, bit 2) and AHB (id 67, bit 3) resets
         * in SYSCRG_RESET_ASSERT2 (0x13020300) / STATUS2 (0x13020310). */
        const uint32_t rst_mask = (1u << 2) | (1u << 3);
        REG32(SYS_CRG_BASE + 0x0300UL) |= rst_mask;
        time_delay_us(10);
        REG32(SYS_CRG_BASE + 0x0300UL) &= ~rst_mask;
        for (unsigned i = 0; i < 10000u; i++) {
            if ((REG32(SYS_CRG_BASE + 0x0310UL) & rst_mask) == rst_mask) break;
            time_delay_us(1);
        }
        time_delay_us(10);
    } else {
        /* AON_SYSCON + 0x0C (aon_syscfg_12): bits [20:18] = 1 (RGMII mode for GMAC0) */
        uint32_t sc = REG32(AON_SYSCON_BASE + 0x000CUL);
        sc = (sc & ~(0x7u << 18)) | (0x1u << 18);
        REG32(AON_SYSCON_BASE + 0x000CUL) = sc;

        /* Enable GMAC0 clocks in SYS_CRG and AON_CRG */
        REG32(SYS_CRG_BASE + 0x01B0UL) = 0x80000008u;   /* gmac0_gtxclk (en + div=8) */
        REG32(SYS_CRG_BASE + 0x01B4UL) = 0x8000000au;   /* gmac0_ptp */
        REG32(SYS_CRG_BASE + 0x01BCUL) = 0x80000020u;   /* gmac0_gtxc */

        REG32(AON_CRG_BASE + 0x0008UL) |= (1u << 31);   /* u0_gmac5_clk_ahb */
        REG32(AON_CRG_BASE + 0x000CUL) |= (1u << 31);   /* u0_gmac5_clk_axi */
        REG32(AON_CRG_BASE + 0x0010UL) = (REG32(AON_CRG_BASE + 0x0010UL) & ~0x1fu) | 1u;
        REG32(AON_CRG_BASE + 0x0014UL) = 0x81000000u;   /* u0_gmac5_clk_tx (en + mux=1) */
        REG32(AON_CRG_BASE + 0x0018UL) = 0x40000000u;   /* u0_gmac5_clk_tx_inv */
        REG32(AON_CRG_BASE + 0x001CUL) = 0x80000020u;   /* u0_gmac5_clk_rx */
        REG32(AON_CRG_BASE + 0x0020UL) = 0x40000000u;   /* u0_gmac5_clk_rx_inv */
        jh7110_mb();

        /* Pulse and release GMAC0 AXI (bit 0) and AHB (bit 1) resets in AON_CRG */
        const uint32_t rst_mask = (1u << 0) | (1u << 1);
        REG32(AON_CRG_BASE + 0x0038UL) |= rst_mask;
        time_delay_us(10);
        REG32(AON_CRG_BASE + 0x0038UL) &= ~rst_mask;
        for (unsigned i = 0; i < 10000u; i++) {
            if ((REG32(AON_CRG_BASE + 0x003CUL) & rst_mask) == rst_mask) break;
            time_delay_us(1);
        }
        time_delay_us(10);
    }
}

static void jh7110_gmac_set_tx_clk_speed(int unit, uint16_t speed_mbit) {
    /* Parent of gmac_rmii_rtx is 50 MHz RGMII/RMII ref; parent of gtxclk is 1000 MHz PLL0.
     * In U-Boot clk-jh7110.c / dwc_eth_qos.c:
     *   1000 Mbps -> rmii_rtx div = 1,  gtxclk div = 8  (125 MHz)
     *    100 Mbps -> rmii_rtx div = 2,  gtxclk div = 40 (25 MHz)
     *     10 Mbps -> rmii_rtx div = 20, gtxclk div = 40 (2.5 MHz) */
    uint32_t rtx_div = (speed_mbit == 1000u) ? 1u : (speed_mbit == 100u) ? 2u : 20u;
    if (unit == 1) {
        REG32(SYS_CRG_BASE + 0x0194UL) = (REG32(SYS_CRG_BASE + 0x0194UL) & ~0x1fu) | rtx_div;
    } else {
        REG32(AON_CRG_BASE + 0x0010UL) = (REG32(AON_CRG_BASE + 0x0010UL) & ~0x1fu) | rtx_div;
    }
    jh7110_mb();
}

/* --- Clause-22 MDIO & Motorcomm YT8531C Extended Registers -------------- */

static int eqos_mdio_wait_idle(uintptr_t base) {
    for (unsigned i = 0; i < 20000u; i++) {
        if (!(REG32(base + EQOS_MAC_MDIO_ADDR) & MDIO_ADDR_GB)) return 0;
        time_delay_us(1);
    }
    return -1;
}

static int eqos_mdio_read_at(uintptr_t base, uint8_t phy_addr, uint8_t reg) {
    if (eqos_mdio_wait_idle(base) != 0) return -1;
    uint32_t cmd = ((uint32_t)(phy_addr & 0x1fu) << MDIO_ADDR_PA_SHIFT) |
                   ((uint32_t)(reg & 0x1fu) << MDIO_ADDR_RDA_SHIFT) |
                   (MDIO_ADDR_CR_150_250 << MDIO_ADDR_CR_SHIFT) |
                   MDIO_ADDR_GOC_READ | MDIO_ADDR_GB;
    REG32(base + EQOS_MAC_MDIO_ADDR) = cmd;
    if (eqos_mdio_wait_idle(base) != 0) return -1;
    return (int)(REG32(base + EQOS_MAC_MDIO_DATA) & 0xffffu);
}

static int eqos_mdio_write_at(uintptr_t base, uint8_t phy_addr, uint8_t reg, uint16_t val) {
    if (eqos_mdio_wait_idle(base) != 0) return -1;
    REG32(base + EQOS_MAC_MDIO_DATA) = (uint32_t)val;
    uint32_t cmd = ((uint32_t)(phy_addr & 0x1fu) << MDIO_ADDR_PA_SHIFT) |
                   ((uint32_t)(reg & 0x1fu) << MDIO_ADDR_RDA_SHIFT) |
                   (MDIO_ADDR_CR_150_250 << MDIO_ADDR_CR_SHIFT) |
                   MDIO_ADDR_GOC_WRITE | MDIO_ADDR_GB;
    REG32(base + EQOS_MAC_MDIO_ADDR) = cmd;
    return eqos_mdio_wait_idle(base);
}

int emac_mdio_read(uint8_t phy_addr, uint8_t reg) {
    return eqos_mdio_read_at(g_gmac_base, phy_addr, reg);
}

int emac_mdio_write(uint8_t phy_addr, uint8_t reg, uint16_t val) {
    return eqos_mdio_write_at(g_gmac_base, phy_addr, reg, val);
}

static int yt8531_read_ext(uintptr_t base, uint8_t phy_addr, uint16_t ext_reg) {
    if (eqos_mdio_write_at(base, phy_addr, YT8531_DEBUG_ADDR, ext_reg) != 0) return -1;
    return eqos_mdio_read_at(base, phy_addr, YT8531_DEBUG_DATA);
}

static int yt8531_write_ext(uintptr_t base, uint8_t phy_addr, uint16_t ext_reg, uint16_t val) {
    if (eqos_mdio_write_at(base, phy_addr, YT8531_DEBUG_ADDR, ext_reg) != 0) return -1;
    return eqos_mdio_write_at(base, phy_addr, YT8531_DEBUG_DATA, val);
}

static int yt8531_modify_ext(uintptr_t base, uint8_t phy_addr, uint16_t ext_reg,
                             uint16_t clr_mask, uint16_t set_bits) {
    int v = yt8531_read_ext(base, phy_addr, ext_reg);
    if (v < 0) return -1;
    uint16_t nv = (uint16_t)(((uint16_t)v & ~clr_mask) | set_bits);
    return yt8531_write_ext(base, phy_addr, ext_reg, nv);
}

/* Configures Motorcomm YT8531C RGMII-ID delays and drive strengths per
 * starfive_visionfive2.dts (Rev B silicon) and disables PHY auto-sleep so RXC
 * continues ticking even when no cable is attached (required for DMA_MODE.SWR). */
static int yt8531_config_init(uintptr_t base, int unit, uint8_t phy_addr) {
    /* Disable auto-sleep in EXT_SLEEP_CTRL1 (0x27) so RXC is never gated */
    (void)yt8531_modify_ext(base, phy_addr, YT8531_EXT_SLEEP_CTRL1, YT8531_SLEEP_AUTO_EN, 0);

    /* Clear RXC_DLY_EN (bit 8) in EXT_CHIP_CONFIG (0xa001) */
    if (yt8531_modify_ext(base, phy_addr, YT8531_EXT_CHIP_CONFIG, YT8531_RXC_DLY_EN, 0) != 0) {
        return -1;
    }

    /* Configure RGMII pad drive strengths in 0xa010:
     * rgmii_sw_dr_rxc [15:13] = 0x6, rgmii_sw_dr_2 [12] = 0, rgmii_sw_dr [5:4] = 0x3 */
    if (yt8531_modify_ext(base, phy_addr, YT8531_EXT_PAD_DRIVE,
                          0xf030u, (0x6u << 13) | (0x3u << 4)) != 0) {
        return -1;
    }

    /* Configure RGMII internal delays in 0xa003:
     * GMAC1 (unit 1): rx_delay=0x2, tx_delay_fe=0x5, tx_delay=0x0, tx_inv=0 (at 1000M)
     * GMAC0 (unit 0): rx_delay=0xa, tx_delay_fe=0x5, tx_delay=0xa, tx_inv=1 */
    uint16_t rgmii_cfg1 = (unit == 1)
        ? ((0x2u << 10) | (0x5u << 4) | 0x0u)
        : (YT8531_TX_CLK_SEL_INV | (0xau << 10) | (0x5u << 4) | 0xau);
    if (yt8531_modify_ext(base, phy_addr, YT8531_EXT_RGMII_CFG1, 0x7cffu, rgmii_cfg1) != 0) {
        return -1;
    }
    return 0;
}

static void yt8531_update_tx_inv(uintptr_t base, int unit, uint8_t phy_addr, uint16_t speed_mbit) {
    /* Per starfive_visionfive2.dts:
     * On GMAC1: tx_inverted_1000 = 0, tx_inverted_100 = 1, tx_inverted_10 = 1.
     * On GMAC0: tx_inverted = 1 for all speeds. */
    uint16_t inv = (unit == 0 || speed_mbit != 1000u) ? YT8531_TX_CLK_SEL_INV : 0u;
    (void)yt8531_modify_ext(base, phy_addr, YT8531_EXT_RGMII_CFG1, YT8531_TX_CLK_SEL_INV, inv);
}

static int find_phy_on_bus(uintptr_t base, uint8_t *addr_out) {
    for (uint8_t a = 0; a < 32u; a++) {
        int id1 = eqos_mdio_read_at(base, a, PHY_IDR1);
        int id2 = eqos_mdio_read_at(base, a, PHY_IDR2);
        if (id1 > 0 && id1 != 0xffff && id2 > 0 && id2 != 0xffff) {
            if (addr_out) *addr_out = a;
            return 0;
        }
    }
    return -1;
}

int emac_probe(void) {
    /* Bring up clocks and resets on both GMAC1 (primary LAN port on VF2) and GMAC0 */
    jh7110_gmac_unit_clks_on(1);
    jh7110_gmac_unit_clks_on(0);

    uint8_t phy1 = 0, phy0 = 0;
    bool has1 = (find_phy_on_bus(GMAC1_BASE, &phy1) == 0);
    bool has0 = (find_phy_on_bus(GMAC0_BASE, &phy0) == 0);

    if (has1) (void)yt8531_config_init(GMAC1_BASE, 1, phy1);
    if (has0) (void)yt8531_config_init(GMAC0_BASE, 0, phy0);

    /* Select GMAC1 by default; if GMAC1 has no carrier and GMAC0 has carrier, use GMAC0 */
    int chosen_unit = 1;
    uintptr_t chosen_base = GMAC1_BASE;
    uint8_t chosen_phy = phy1;

    if (has1) {
        (void)eqos_mdio_read_at(GMAC1_BASE, phy1, PHY_BMSR);
        int bmsr1 = eqos_mdio_read_at(GMAC1_BASE, phy1, PHY_BMSR);
        if ((bmsr1 < 0 || !(bmsr1 & BMSR_LINK_UP)) && has0) {
            (void)eqos_mdio_read_at(GMAC0_BASE, phy0, PHY_BMSR);
            int bmsr0 = eqos_mdio_read_at(GMAC0_BASE, phy0, PHY_BMSR);
            if (bmsr0 >= 0 && (bmsr0 & BMSR_LINK_UP)) {
                chosen_unit = 0;
                chosen_base = GMAC0_BASE;
                chosen_phy  = phy0;
            }
        }
    } else if (has0) {
        chosen_unit = 0;
        chosen_base = GMAC0_BASE;
        chosen_phy  = phy0;
    } else {
        return -2;  /* No PHY answered on either MDIO bus */
    }

    g_gmac_unit = chosen_unit;
    g_gmac_base = chosen_base;
    g_phy_addr  = chosen_phy;

    /* Issue EQOS DMA software reset (SWR) and wait up to 100 ms for it to clear. */
    REG32(g_gmac_base + EQOS_DMA_MODE) |= DMA_MODE_SWR;
    bool swr_cleared = false;
    for (unsigned i = 0; i < 10000u; i++) {
        if (!(REG32(g_gmac_base + EQOS_DMA_MODE) & DMA_MODE_SWR)) {
            swr_cleared = true;
            break;
        }
        time_delay_us(10);
    }
    if (!swr_cleared) return -1;

    /* Set 1 us tick counter for CSR clock (100 MHz -> 99) */
    REG32(g_gmac_base + EQOS_MAC_US_TIC_COUNTER) = 99u;
    return 0;
}

void emac_phy_scan(void) {
    int rc = emac_probe();
    if (rc != 0) {
        cprintf("[EMAC] probe failed (%d): EQOS DMA reset did not complete.\n", rc);
        return;
    }

    uint32_t ver = REG32(g_gmac_base + EQOS_MAC_VERSION);
    cprintf("EMAC%d at 0x%lx (Synopsys EQOS SNPSVER 0x%02x USERVER 0x%02x). Scanning 0..31:\n",
            g_gmac_unit, (unsigned long)g_gmac_base,
            (unsigned)(ver & 0xffu), (unsigned)((ver >> 8) & 0xffu));

    unsigned found = 0;
    for (uint8_t a = 0; a < 32u; a++) {
        int id1 = emac_mdio_read(a, PHY_IDR1);
        int id2 = emac_mdio_read(a, PHY_IDR2);
        if (id1 < 0 || id2 < 0) {
            cprintf("  %2u: MDIO stayed busy\n", (unsigned)a);
            continue;
        }
        if ((id1 == 0xffff && id2 == 0xffff) || (id1 == 0 && id2 == 0)) continue;
        uint32_t oui = ((uint32_t)id1 << 6) | (((uint32_t)id2 >> 10) & 0x3fu);
        cprintf("  %2u: ID 0x%04x/0x%04x OUI %02x-%02x-%02x model %u rev %u%s\n",
                (unsigned)a, (unsigned)id1, (unsigned)id2,
                (unsigned)((oui >> 16) & 0xffu), (unsigned)((oui >> 8) & 0xffu),
                (unsigned)(oui & 0xffu),
                (unsigned)((id2 >> 4) & 0x3fu), (unsigned)(id2 & 0xfu),
                (id1 == YT8531_PHY_ID1 && id2 == YT8531_PHY_ID2) ? " (Motorcomm YT8531C)" : "");
        found++;
    }

    if (found == 0) {
        cprintf("  nothing answered on MDIO\n");
    } else {
        cprintf("  %u PHY%s at addr %u on GMAC%d. OK.\n",
                found, found == 1 ? "" : "s", (unsigned)g_phy_addr, g_gmac_unit);
    }
}

/* --- Station MAC Address & Link Configuration --------------------------- */

static void emac_set_mac(const uint8_t mac[NETIF_MAC_LEN]) {
    if (mac != g_station_mac) {
        memcpy(g_station_mac, mac, NETIF_MAC_LEN);
        g_station_mac_valid = true;
    }
    uint32_t hi = MAC_ADDR0_HIGH_AE | ((uint32_t)mac[5] << 8) | (uint32_t)mac[4];
    uint32_t lo = ((uint32_t)mac[3] << 24) | ((uint32_t)mac[2] << 16) |
                  ((uint32_t)mac[1] << 8)  |  (uint32_t)mac[0];
    REG32(g_gmac_base + EQOS_MAC_ADDR0_HIGH) = hi;
    REG32(g_gmac_base + EQOS_MAC_ADDR0_LOW)  = lo;
    jh7110_mb();
}

static void emac_apply_link(const emac_link_t *l) {
    uint16_t spd = l->up ? l->speed_mbit : 1000u;
    jh7110_gmac_set_tx_clk_speed(g_gmac_unit, spd);
    yt8531_update_tx_inv(g_gmac_base, g_gmac_unit, g_phy_addr, spd);

    uint32_t cfg = REG32(g_gmac_base + EQOS_MAC_CFG);
    cfg &= ~(MAC_CFG_PS | MAC_CFG_FES | MAC_CFG_DM);
    if (spd == 100u) {
        cfg |= MAC_CFG_PS | MAC_CFG_FES;
    } else if (spd == 10u) {
        cfg |= MAC_CFG_PS;
    }
    if (!l->up || l->full_duplex) {
        cfg |= MAC_CFG_DM;
    }
    REG32(g_gmac_base + EQOS_MAC_CFG) = cfg;
    jh7110_mb();
}

int emac_phy_autoneg_start(void) {
    int bmcr = emac_mdio_read(g_phy_addr, PHY_BMCR);
    if (bmcr < 0) return -1;

    /* Advertise 10/100/1000 full/half duplex */
    (void)emac_mdio_write(g_phy_addr, PHY_ANAR,
                          AN_SELECTOR_802_3 | AN_100BASE_TX_FD | AN_100BASE_TX_HD |
                          AN_10BASE_T_FD | AN_10BASE_T_HD);
    (void)emac_mdio_write(g_phy_addr, PHY_CTRL1000,
                          ADVERTISE_1000FULL | ADVERTISE_1000HALF);

    uint16_t new_bmcr = (uint16_t)((bmcr & ~(BMCR_ISOLATE | BMCR_POWERDOWN | BMCR_LOOPBACK)) |
                                   BMCR_AN_ENABLE | BMCR_AN_RESTART);
    if (emac_mdio_write(g_phy_addr, PHY_BMCR, new_bmcr) != 0) return -1;
    g_link_valid = false;
    return 0;
}

bool emac_link_poll(emac_link_t *out) {
    uint64_t now = time_get_ms();
    if (g_link_valid && (now - g_link_checked_ms) < 200u) {
        if (out) *out = g_link;
        return g_link.up;
    }
    g_link_checked_ms = now;

    /* Read BMSR twice since link-down latches low */
    (void)emac_mdio_read(g_phy_addr, PHY_BMSR);
    int bmsr = emac_mdio_read(g_phy_addr, PHY_BMSR);

    emac_link_t l = { 0 };
    if (bmsr >= 0 && (bmsr & BMSR_LINK_UP)) {
        l.up = true;
        l.an_complete = (bmsr & BMSR_AN_COMPLETE) != 0;

        /* Read Motorcomm YT8531C PHY-specific status register (0x11) for resolved
         * 10/100/1000 speed and duplex. */
        int spec = emac_mdio_read(g_phy_addr, YT8531_SPEC_STATUS);
        if (spec >= 0) {
            uint32_t spd_bits = (uint32_t)spec & YT8531_SPEED_MASK;
            if (spd_bits == YT8531_SPEED_1000)     l.speed_mbit = 1000u;
            else if (spd_bits == YT8531_SPEED_100) l.speed_mbit = 100u;
            else                                   l.speed_mbit = 10u;
            l.full_duplex = ((uint32_t)spec & YT8531_DUPLEX_FULL) != 0;
        } else {
            l.speed_mbit  = 1000u;
            l.full_duplex = true;
        }
    }

    bool changed = !g_link_valid ||
                   l.up != g_link.up ||
                   l.speed_mbit != g_link.speed_mbit ||
                   l.full_duplex != g_link.full_duplex;
    g_link = l;
    g_link_valid = true;
    if (changed && l.up) emac_apply_link(&l);
    if (l.up && g_station_mac_valid) emac_set_mac(g_station_mac);

    if (out) *out = l;
    return l.up;
}

/* --- EQOS v5.20 Descriptor Rings & Uncached DMA ------------------------- */

static void emac_netif_restarted(void) {
    g_rx_held = NULL;
    g_rx_held_len = 0;
}

static void emac_rings_init(void) {
    for (uint32_t i = 0; i < EMAC_TX_DESC_COUNT; i++) {
        volatile emac_desc_t *td = &g_tx_desc[i];
        td->des0 = 0;
        td->des1 = 0;
        td->des2 = 0;
        td->des3 = 0;
    }
    for (uint32_t i = 0; i < EMAC_RX_DESC_COUNT; i++) {
        volatile emac_desc_t *rd = &g_rx_desc[i];
        rd->des0 = (uint32_t)(uintptr_t)&g_rx_buf[i][0];
        rd->des1 = 0;
        rd->des2 = 0;
        rd->des3 = DESC3_OWN | DESC3_BUF1V;
    }
    jh7110_mb();

    /* Flush initial descriptors and clean all buffer cache lines to DDR */
    jh7110_flush_l2_range((uintptr_t)&g_tx_desc[0], (uintptr_t)&g_tx_desc[0] + sizeof(g_tx_desc));
    jh7110_flush_l2_range((uintptr_t)&g_rx_desc[0], (uintptr_t)&g_rx_desc[0] + sizeof(g_rx_desc));
    jh7110_flush_l2_range((uintptr_t)&g_tx_buf[0][0], (uintptr_t)&g_tx_buf[0][0] + sizeof(g_tx_buf));
    jh7110_flush_l2_range((uintptr_t)&g_rx_buf[0][0], (uintptr_t)&g_rx_buf[0][0] + sizeof(g_rx_buf));

    g_rx_next = 0;
    g_tx_next = 0;

    REG32(g_gmac_base + EQOS_DMA_CH0_TXDESC_HADDR) = 0;
    REG32(g_gmac_base + EQOS_DMA_CH0_TXDESC_ADDR)  = (uint32_t)(uintptr_t)&g_tx_desc[0];
    REG32(g_gmac_base + EQOS_DMA_CH0_TXDESC_RLEN)  = EMAC_TX_DESC_COUNT - 1u;

    REG32(g_gmac_base + EQOS_DMA_CH0_RXDESC_HADDR) = 0;
    REG32(g_gmac_base + EQOS_DMA_CH0_RXDESC_ADDR)  = (uint32_t)(uintptr_t)&g_rx_desc[0];
    REG32(g_gmac_base + EQOS_DMA_CH0_RXDESC_RLEN)  = EMAC_RX_DESC_COUNT - 1u;
    jh7110_mb();
}

static void emac_start(bool loopback) {
    /* Stop DMA TX/RX and MAC TX/RX before reconfiguring rings */
    REG32(g_gmac_base + EQOS_DMA_CH0_TX_CONTROL) &= ~CH0_TX_ST;
    REG32(g_gmac_base + EQOS_DMA_CH0_RX_CONTROL) &= ~CH0_RX_SR;
    REG32(g_gmac_base + EQOS_MAC_CFG) &= ~(MAC_CFG_TE | MAC_CFG_RE);
    jh7110_mb();

    /* Configure MTL TX/RX queues and FIFO sizes from HW_FEATURE1 */
    uint32_t hw_feat1 = REG32(g_gmac_base + EQOS_MAC_HW_FEATURE1);
    uint32_t tx_fifo_sz = (hw_feat1 >> HW_FEAT1_TXFIFOSIZE_SHIFT) & HW_FEAT1_TXFIFOSIZE_MASK;
    uint32_t rx_fifo_sz = (hw_feat1 >> HW_FEAT1_RXFIFOSIZE_SHIFT) & HW_FEAT1_RXFIFOSIZE_MASK;
    uint32_t tqs = (128u << tx_fifo_sz) / 256u - 1u;
    uint32_t rqs = (128u << rx_fifo_sz) / 256u - 1u;

    REG32(g_gmac_base + EQOS_MTL_TXQ0_OP_MODE) =
        ((tqs & MTL_TXQ0_TQS_MASK) << MTL_TXQ0_TQS_SHIFT) |
        (MTL_TXQ0_TXQEN_EN << MTL_TXQ0_TXQEN_SHIFT) |
        MTL_TXQ0_TSF;
    REG32(g_gmac_base + EQOS_MTL_TXQ0_QUANTUM_W) = 0x10u;

    uint32_t rxq_op = ((rqs & MTL_RXQ0_RQS_MASK) << MTL_RXQ0_RQS_SHIFT) | MTL_RXQ0_RSF;
    if (rqs >= ((4096u / 256u) - 1u)) {
        rxq_op |= MTL_RXQ0_EHFC | (0x3u << MTL_RXQ0_RFD_SHIFT) | (0x1u << MTL_RXQ0_RFA_SHIFT);
    }
    REG32(g_gmac_base + EQOS_MTL_RXQ0_OP_MODE) = rxq_op;

    /* Enable MAC RX Queue 0 */
    REG32(g_gmac_base + EQOS_MAC_RXQ_CTRL0) =
        (REG32(g_gmac_base + EQOS_MAC_RXQ_CTRL0) & ~RXQ0_EN_MASK) | RXQ0_EN_DCB;
    /* Multicast and Broadcast Queue Enable */
    REG32(g_gmac_base + EQOS_MAC_RXQ_CTRL1) |= 0x00100000u;

    /* Flow control configuration */
    REG32(g_gmac_base + EQOS_MAC_Q0_TX_FLOW_CTRL) = (0xffffu << TX_FLOW_CTRL_PT_SHIFT) | TX_FLOW_CTRL_TFE;
    REG32(g_gmac_base + EQOS_MAC_TXQ_PRTY_MAP0)   = 0;
    REG32(g_gmac_base + EQOS_MAC_RXQ_CTRL2)       = 0;
    REG32(g_gmac_base + EQOS_MAC_RX_FLOW_CTRL)    = RX_FLOW_CTRL_RFE;

    /* Configure DMA bus mode and channel 0 parameters:
     * axi_bus_width = 8 bytes (64-bit), desc_size = 64 bytes -> desc_pad = (64 - 16) / 8 = 6 */
    uint32_t desc_pad = (sizeof(emac_desc_t) - 16u) / 8u;
    REG32(g_gmac_base + EQOS_DMA_SYSBUS_MODE) =
        (2u << SYSBUS_RD_OSR_LMT_SHIFT) | SYSBUS_EAME |
        SYSBUS_BLEN16 | SYSBUS_BLEN8 | SYSBUS_BLEN4;
    REG32(g_gmac_base + EQOS_DMA_CH0_CONTROL) =
        CH0_CTRL_PBLX8 | (desc_pad << CH0_CTRL_DSL_SHIFT);

    uint32_t pbl = (tqs + 1u > 32u) ? 32u : (tqs + 1u);
    REG32(g_gmac_base + EQOS_DMA_CH0_TX_CONTROL) =
        ((pbl & CH0_TX_PBL_MASK) << CH0_TX_PBL_SHIFT) | CH0_TX_OSP;
    REG32(g_gmac_base + EQOS_DMA_CH0_RX_CONTROL) =
        (8u << CH0_RX_PBL_SHIFT) | ((EMAC_BUF_SIZE & CH0_RX_RBSZ_MASK) << CH0_RX_RBSZ_SHIFT);

    /* Initialize descriptor rings */
    emac_rings_init();

    /* Configure MAC speed/duplex/CRC stripping and filter */
    emac_link_t cur_link = { .up = true, .speed_mbit = g_link.up ? g_link.speed_mbit : 1000u, .full_duplex = true };
    emac_apply_link(&cur_link);

    uint32_t mac_cfg = REG32(g_gmac_base + EQOS_MAC_CFG);
    mac_cfg &= ~(MAC_CFG_WD | MAC_CFG_JD | MAC_CFG_JE | MAC_CFG_LM);
    mac_cfg |= MAC_CFG_CST | MAC_CFG_ACS | MAC_CFG_DM;
    if (loopback) mac_cfg |= MAC_CFG_LM;
    REG32(g_gmac_base + EQOS_MAC_CFG) = mac_cfg;

    if (g_station_mac_valid) emac_set_mac(g_station_mac);
    REG32(g_gmac_base + EQOS_MAC_PKT_FILTER) = loopback ? (PKT_FILTER_RA | PKT_FILTER_PR) : PKT_FILTER_PM;

    /* Start DMA TX/RX and MAC TX/RX, then arm RX tail pointer */
    REG32(g_gmac_base + EQOS_DMA_CH0_TX_CONTROL) |= CH0_TX_ST;
    REG32(g_gmac_base + EQOS_DMA_CH0_RX_CONTROL) |= CH0_RX_SR;
    REG32(g_gmac_base + EQOS_MAC_CFG) |= MAC_CFG_TE | MAC_CFG_RE;
    jh7110_mb();

    REG32(g_gmac_base + EQOS_DMA_CH0_RXDESC_TAIL) =
        (uint32_t)(uintptr_t)&g_rx_desc[EMAC_RX_DESC_COUNT - 1u];
    jh7110_mb();

    emac_netif_restarted();
}

/* --- Zero-copy TX & RX primitives over Cache-Flushed DRAM rings -------- */

static volatile uint8_t *emac_tx_buffer(void) {
    /* Invalidate descriptor before inspecting OWN bit */
    jh7110_flush_l2_range((uintptr_t)&g_tx_desc[g_tx_next],
                          (uintptr_t)(&g_tx_desc[g_tx_next] + 1));
    volatile emac_desc_t *d = &g_tx_desc[g_tx_next];
    uint32_t d3 = d->des3;
    if (d3 & DESC3_OWN) return NULL;
    if (d3 & DESC3_ES) {
        g_tx_err_last_des3 = d3;
        g_tx_err_seen_bits |= d3;
        g_tx_hw_errors++;
    }
    return &g_tx_buf[g_tx_next][0];
}

static int emac_tx_submit(uint32_t len) {
    if (len == 0 || len > EMAC_BUF_SIZE) return -1;
    uint32_t idx = g_tx_next;
    volatile emac_desc_t *d = &g_tx_desc[idx];

    /* Flush payload bytes to DDR */
    jh7110_flush_l2_range((uintptr_t)&g_tx_buf[idx][0],
                          (uintptr_t)&g_tx_buf[idx][0] + len);

    d->des0 = (uint32_t)(uintptr_t)&g_tx_buf[idx][0];
    d->des1 = 0;
    d->des2 = len & 0x3fffu;
    jh7110_mb();
    d->des3 = DESC3_OWN | DESC3_FD | DESC3_LD | (len & DESC3_PL_MASK);
    jh7110_mb();

    /* Flush descriptor to DDR before ringing the doorbell */
    jh7110_flush_l2_range((uintptr_t)&g_tx_desc[idx],
                          (uintptr_t)(&g_tx_desc[idx] + 1));

    g_tx_next = (idx + 1u) % EMAC_TX_DESC_COUNT;
    REG32(g_gmac_base + EQOS_DMA_CH0_TXDESC_TAIL) =
        (uint32_t)(uintptr_t)&g_tx_desc[g_tx_next];
    return 0;
}

static const volatile uint8_t *emac_rx_peek(uint32_t *len_out) {
    /* Invalidate descriptor line so CPU observes DMA status writeback */
    jh7110_flush_l2_range((uintptr_t)&g_rx_desc[g_rx_next],
                          (uintptr_t)(&g_rx_desc[g_rx_next] + 1));
    volatile emac_desc_t *d = &g_rx_desc[g_rx_next];
    uint32_t d3 = d->des3;
    if (d3 & DESC3_OWN) return NULL;

    uint32_t len = d3 & DESC3_PL_MASK;
    if (len == 0 || (d3 & DESC3_ES) || !(d3 & DESC3_FD) || !(d3 & DESC3_LD)) {
        return NULL;
    }
    *len_out = len;
    /* Invalidate receive buffer so CPU reads packet bytes written to DDR */
    jh7110_flush_l2_range((uintptr_t)&g_rx_buf[g_rx_next][0],
                          (uintptr_t)&g_rx_buf[g_rx_next][0] + len);
    return &g_rx_buf[g_rx_next][0];
}

static void emac_rx_release(void) {
    uint32_t idx = g_rx_next;
    volatile emac_desc_t *d = &g_rx_desc[idx];
    d->des0 = (uint32_t)(uintptr_t)&g_rx_buf[idx][0];
    d->des1 = 0;
    d->des2 = 0;
    jh7110_mb();
    d->des3 = DESC3_OWN | DESC3_BUF1V;
    jh7110_mb();

    /* Flush rearmed descriptor to DDR */
    jh7110_flush_l2_range((uintptr_t)&g_rx_desc[idx],
                          (uintptr_t)(&g_rx_desc[idx] + 1));

    REG32(g_gmac_base + EQOS_DMA_CH0_RXDESC_TAIL) =
        (uint32_t)(uintptr_t)&g_rx_desc[idx];
    g_rx_next = (idx + 1u) % EMAC_RX_DESC_COUNT;
}

/* --- Loopback & Link Diagnostics ---------------------------------------- */

static const uint16_t g_loopback_sizes[] = { 60, 64, 65, 128, 512, 1500, 1514 };

static uint8_t loopback_byte(uint32_t off, uint32_t len) {
    static const uint8_t head[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    if (off < 6u)   return head[off];
    if (off == 12u) return 0x08;
    if (off == 13u) return 0x00;
    return (uint8_t)(off + len);
}

static void loopback_run(uint32_t rounds, uint32_t *p_sent, uint32_t *p_ok,
                         uint32_t *p_corrupt, uint32_t *p_refused) {
    if (rounds == 0) rounds = 100;
    uint32_t sent = 0, ok = 0, corrupt = 0, refused = 0;

    for (uint32_t r = 0; r < rounds; r++) {
        for (unsigned si = 0; si < sizeof(g_loopback_sizes) / sizeof(g_loopback_sizes[0]); si++) {
            uint32_t len = g_loopback_sizes[si];
            volatile uint8_t *tx = NULL;
            for (unsigned i = 0; i < 1000u && !tx; i++) {
                tx = emac_tx_buffer();
                if (!tx) time_delay_us(10);
            }
            if (!tx) { refused++; continue; }
            for (uint32_t i = 0; i < len; i++) tx[i] = loopback_byte(i, len);
            if (emac_tx_submit(len) != 0) { refused++; continue; }
            sent++;

            for (unsigned guard = 0; guard < EMAC_RX_DESC_COUNT + 1u; guard++) {
                bool owned = false;
                unsigned patience = (guard == 0) ? 2000u : 1u;
                for (unsigned i = 0; i < patience && !owned; i++) {
                    jh7110_flush_l2_range((uintptr_t)&g_rx_desc[g_rx_next],
                                          (uintptr_t)(&g_rx_desc[g_rx_next] + 1));
                    volatile emac_desc_t *rd = &g_rx_desc[g_rx_next];
                    if (!(rd->des3 & DESC3_OWN)) { owned = true; break; }
                    time_delay_us(1);
                }
                if (!owned) break;

                uint32_t got = 0;
                const volatile uint8_t *rx = emac_rx_peek(&got);
                if (!rx) { corrupt++; emac_rx_release(); continue; }

                uint32_t bad = 0;
                for (uint32_t i = 0; i < got; i++) {
                    if (rx[i] != loopback_byte(i, got)) { bad++; break; }
                }
                emac_rx_release();
                if (bad) corrupt++; else ok++;
            }
        }
    }
    *p_sent = sent; *p_ok = ok; *p_corrupt = corrupt; *p_refused = refused;
}

static void loopback_report(const char *what, uint32_t sent, uint32_t ok,
                            uint32_t corrupt, uint32_t refused) {
    cprintf("EMAC %s loopback: %lu sent, %lu verified, %lu corrupt, "
            "%lu refused, %lu unaccounted\n", what,
            (unsigned long)sent, (unsigned long)ok, (unsigned long)corrupt,
            (unsigned long)refused, (unsigned long)(sent - ok - corrupt));
}

void emac_loopback_stress(uint32_t rounds) {
    if (emac_probe() != 0) {
        cprintf("[EMAC] probe failed -- see `emac scan`.\n");
        return;
    }
    net_lock();
    g_diag_owns_rings = true;
    net_unlock();
    emac_start(true);

    uint32_t sent, ok, corrupt, refused;
    loopback_run(rounds, &sent, &ok, &corrupt, &refused);

    emac_start(false);
    g_diag_owns_rings = false;
    loopback_report("MAC-internal", sent, ok, corrupt, refused);
}

void emac_loopback_test(void) {
    if (emac_probe() != 0) {
        cprintf("[EMAC] probe failed -- see `emac scan`.\n");
        return;
    }
    net_lock();
    g_diag_owns_rings = true;
    net_unlock();
    emac_start(true);

    cprintf("EMAC loopback: %u RX + %u TX descriptors of %u B, buffers %u B\n",
            (unsigned)EMAC_RX_DESC_COUNT, (unsigned)EMAC_TX_DESC_COUNT,
            (unsigned)sizeof(emac_desc_t), (unsigned)EMAC_BUF_SIZE);
    cprintf("  rings cost %u B of .bss; U74 cache line %u B (uncached alias +0x4_00000000)\n",
            (unsigned)(sizeof(g_rx_desc) + sizeof(g_tx_desc) +
                       sizeof(g_rx_buf) + sizeof(g_tx_buf)),
            (unsigned)U74_CACHE_LINE);

    unsigned passed = 0, failed = 0;

    for (unsigned s = 0; s < sizeof(g_loopback_sizes) / sizeof(g_loopback_sizes[0]); s++) {
        uint32_t len = g_loopback_sizes[s];
        volatile uint8_t *tx = emac_tx_buffer();
        if (!tx) {
            cprintf("  %4u B: no free transmit descriptor\n", (unsigned)len);
            failed++;
            continue;
        }
        for (uint32_t i = 0; i < len; i++) tx[i] = loopback_byte(i, len);

        int rc = emac_tx_submit(len);
        if (rc != 0) {
            cprintf("  %4u B: send refused (%d)\n", (unsigned)len, rc);
            failed++;
            continue;
        }

        const volatile uint8_t *rx = NULL;
        uint32_t got = 0;
        bool owned = false;
        for (unsigned i = 0; i < 2000u && !owned; i++) {
            jh7110_flush_l2_range((uintptr_t)&g_rx_desc[g_rx_next],
                                  (uintptr_t)(&g_rx_desc[g_rx_next] + 1));
            volatile emac_desc_t *rd = &g_rx_desc[g_rx_next];
            if (!(rd->des3 & DESC3_OWN)) {
                owned = true;
                rx = emac_rx_peek(&got);
                break;
            }
            time_delay_us(1);
        }

        if (!owned) {
            cprintf("  %4u B: nothing came back, DMA status 0x%08lx\n",
                    (unsigned)len, (unsigned long)REG32(g_gmac_base + EQOS_DMA_CH0_STATUS));
            failed++;
            continue;
        }
        if (!rx) {
            cprintf("  %4u B: the frame came back with an error\n", (unsigned)len);
            emac_rx_release();
            failed++;
            continue;
        }
        if (got != len) {
            cprintf("  %4u B: came back %u B\n", (unsigned)len, (unsigned)got);
            emac_rx_release();
            failed++;
            continue;
        }
        uint32_t bad = 0;
        for (uint32_t i = 0; i < len; i++) {
            if (rx[i] != loopback_byte(i, len)) bad++;
        }
        emac_rx_release();
        if (bad) {
            cprintf("  %4u B: %u byte%s differ\n", (unsigned)len,
                    (unsigned)bad, bad == 1 ? "" : "s");
            failed++;
            continue;
        }
        cprintf("  %4u B: ok\n", (unsigned)len);
        passed++;
    }

    emac_start(false);
    g_diag_owns_rings = false;
    cprintf("EMAC loopback: %u passed, %u failed\n", passed, failed);
}

void emac_loopback_phy(uint32_t rounds) {
    if (emac_probe() != 0) {
        cprintf("[EMAC] probe failed -- see `emac scan`.\n");
        return;
    }
    net_lock();
    g_diag_owns_rings = true;
    net_unlock();
    emac_start(false);
    REG32(g_gmac_base + EQOS_MAC_PKT_FILTER) = PKT_FILTER_RA | PKT_FILTER_PR;

    /* Force YT8531C to 100 Mbit/s full-duplex PHY loopback */
    emac_link_t phy_lb_link = { .up = true, .speed_mbit = 100u, .full_duplex = true };
    emac_apply_link(&phy_lb_link);
    if (emac_mdio_write(g_phy_addr, PHY_BMCR,
                        (uint16_t)(BMCR_LOOPBACK | BMCR_SPEED_100 | BMCR_FULL_DUPLEX)) != 0) {
        cprintf("[EMAC] PHY at %u did not accept the loopback write\n", (unsigned)g_phy_addr);
        g_diag_owns_rings = false;
        return;
    }
    time_delay_us(50000);

    uint32_t sent, ok, corrupt, refused;
    loopback_run(rounds, &sent, &ok, &corrupt, &refused);

    emac_phy_autoneg_start();
    g_link_valid = false;
    emac_start(false);
    g_diag_owns_rings = false;
    loopback_report("PHY", sent, ok, corrupt, refused);
}

void emac_link_report(void) {
    if (emac_probe() != 0) {
        cprintf("[EMAC] probe failed -- see `emac scan`.\n");
        return;
    }
    emac_start(false);
    if (emac_phy_autoneg_start() != 0) {
        cprintf("[EMAC] could not start auto-negotiation (PHY at %u not answering?)\n",
                (unsigned)g_phy_addr);
        return;
    }

    cprintf("EMAC%d: negotiating...\n", g_gmac_unit);
    emac_link_t l = { 0 };
    for (unsigned i = 0; i < 50u; i++) {
        g_link_valid = false;
        if (emac_link_poll(&l) && l.an_complete) break;
        time_delay_us(100000);
    }

    if (!l.up) {
        cprintf("EMAC%d: link DOWN (no carrier)\n", g_gmac_unit);
        return;
    }
    cprintf("EMAC%d: link UP, %u Mbit/s %s duplex%s, MAC_CFG 0x%08lx\n",
            g_gmac_unit, (unsigned)l.speed_mbit, l.full_duplex ? "full" : "half",
            l.an_complete ? "" : " (parallel detect)",
            (unsigned long)REG32(g_gmac_base + EQOS_MAC_CFG));
}

void emac_link_updown_test(void) {
    if (emac_probe() != 0) {
        cprintf("[EMAC] probe failed -- see `emac scan`.\n");
        return;
    }
    emac_start(false);
    emac_link_t l = { 0 };
    for (unsigned i = 0; i < 50u && !(emac_link_poll(&l) && l.an_complete); i++) {
        g_link_valid = false;
        time_delay_us(100000);
    }
    if (!l.up) {
        cprintf("EMAC linktest: SKIPPED -- no carrier to begin with.\n");
        return;
    }
    cprintf("EMAC linktest: up at %u Mbit/s %s duplex; dropping the PHY...\n",
            (unsigned)l.speed_mbit, l.full_duplex ? "full" : "half");

    int bmcr = emac_mdio_read(g_phy_addr, PHY_BMCR);
    emac_mdio_write(g_phy_addr, PHY_BMCR, (uint16_t)(bmcr | BMCR_POWERDOWN));

    uint64_t t0 = time_get_ms();
    bool went_down = false;
    for (unsigned i = 0; i < 300u; i++) {
        g_link_valid = false;
        if (!emac_link_poll(&l)) { went_down = true; break; }
        time_delay_us(10000);
    }
    uint64_t down_ms = time_get_ms() - t0;

    emac_mdio_write(g_phy_addr, PHY_BMCR, (uint16_t)(bmcr & ~BMCR_POWERDOWN));
    emac_phy_autoneg_start();

    t0 = time_get_ms();
    bool came_back = false;
    for (unsigned i = 0; i < 100u; i++) {
        g_link_valid = false;
        if (emac_link_poll(&l) && l.an_complete) { came_back = true; break; }
        time_delay_us(100000);
    }
    uint64_t up_ms = time_get_ms() - t0;

    if (!went_down || !came_back) {
        cprintf("EMAC linktest: FAILED (went_down=%d, came_back=%d)\n",
                (int)went_down, (int)came_back);
        return;
    }
    cprintf("EMAC linktest: down noticed in %u ms, back up in %u ms at %u Mbit/s %s duplex\n",
            (unsigned)down_ms, (unsigned)up_ms, (unsigned)l.speed_mbit,
            l.full_duplex ? "full" : "half");
    cprintf("EMAC linktest: PASSED\n");
}

/* --- LugalOS netif_t Glue (`eth0`) -------------------------------------- */

static int emac_netif_poll(netif_t *nif) {
    (void)nif;
    if (!g_netif_registered) return -1;
    if (g_diag_owns_rings) return 0;

    (void)emac_link_poll(NULL);
    if (g_rx_held) return 1;

    for (unsigned i = 0; i < EMAC_RX_DESC_COUNT; i++) {
        jh7110_flush_l2_range((uintptr_t)&g_rx_desc[g_rx_next],
                              (uintptr_t)(&g_rx_desc[g_rx_next] + 1));
        volatile emac_desc_t *d = &g_rx_desc[g_rx_next];
        jh7110_mb();
        uint32_t d3 = d->des3;
        if (d3 & DESC3_OWN) return 0;

        uint32_t len = 0;
        const volatile uint8_t *p = emac_rx_peek(&len);
        if (p) {
            if (len > NETIF_FRAME_MAX) {
                g_rx_hw_errors++;
                emac_rx_release();
                continue;
            }
            if (len >= sizeof(g_last_hdr)) {
                for (unsigned k = 0; k < sizeof(g_last_hdr); k++) g_last_hdr[k] = p[k];
                g_last_hdr_valid = true;
            }
            g_rx_held = p;
            g_rx_held_len = len;
            return 1;
        }
        g_rx_err_last_des3 = d3;
        g_rx_err_seen_bits |= d3;
        g_rx_hw_errors++;
        emac_rx_release();
    }
    return 0;
}

static int emac_netif_recv_frame(netif_t *nif, uint8_t *buf, uint32_t max_len) {
    (void)nif;
    if (!g_rx_held || !buf) return -1;
    if (g_rx_held_len > max_len) {
        g_rx_hw_errors++;
        g_rx_held = NULL;
        g_rx_held_len = 0;
        emac_rx_release();
        return -1;
    }
    uint32_t n = g_rx_held_len;
    for (uint32_t i = 0; i < n; i++) buf[i] = g_rx_held[i];
    g_rx_held = NULL;
    g_rx_held_len = 0;
    emac_rx_release();
    return (int)n;
}

static int emac_netif_send_frame(netif_t *nif, const uint8_t *buf, uint32_t len) {
    (void)nif;
    if (!g_netif_registered || !buf) return -1;
    if (len == 0 || len > NETIF_FRAME_MAX) return -1;

    volatile uint8_t *dst = NULL;
    for (unsigned i = 0; i < 1000u; i++) {
        dst = emac_tx_buffer();
        if (dst) break;
        time_delay_us(10);
    }
    if (!dst) return -1;

    for (uint32_t i = 0; i < len; i++) dst[i] = buf[i];
    if (emac_tx_submit(len) != 0) return -1;
    return (int)len;
}

static bool emac_netif_link_up(netif_t *nif) {
    (void)nif;
    if (!g_netif_registered) return false;
    emac_link_t l = { 0 };
    return emac_link_poll(&l);
}

int emac_netif_init(void) {
    if (g_netif_registered) return 0;

    int rc = emac_probe();
    if (rc != 0) {
        printk("[EMAC] probe failed (%d) -- no interface registered\n", rc);
        return -1;
    }

    memset(&g_netif, 0, sizeof(g_netif));
    g_netif.name       = "eth0";
    g_netif.poll       = emac_netif_poll;
    g_netif.send_frame = emac_netif_send_frame;
    g_netif.recv_frame = emac_netif_recv_frame;
    g_netif.link_up    = emac_netif_link_up;

    /* VisionFive 2 StarFive OUI MAC (matches factory EEPROM eth1addr/eth0addr) */
    g_netif.mac[0] = 0x6cu;
    g_netif.mac[1] = 0xcfu;
    g_netif.mac[2] = 0x39u;
    g_netif.mac[3] = 0x00u;
    g_netif.mac[4] = 0xb9u;
    g_netif.mac[5] = (g_gmac_unit == 1) ? 0x8fu : 0x8eu;

    if (netif_register(&g_netif) != 0) {
        printk("[EMAC] netif_register() failed\n");
        return -1;
    }
    g_netif_registered = true;

    emac_set_mac(g_netif.mac);
    emac_start(false);

    /* Wait briefly for link if already negotiated by U-Boot, otherwise restart autoneg */
    g_link_valid = false;
    emac_link_t l = { 0 };
    bool up = emac_link_poll(&l);
    if (!up) {
        (void)emac_phy_autoneg_start();
        for (unsigned i = 0; i < 35u; i++) {
            g_link_valid = false;
            if (emac_link_poll(&l) && l.an_complete) { up = true; break; }
            time_delay_us(100000);
        }
    }

    char macstr[18];
    netif_mac_str(g_netif.mac, macstr);
    if (up) {
        printk("[EMAC] eth0 (gmac%d@0x%lx) registered, mac %s, link UP (%u Mbit/s %s duplex)\n",
               g_gmac_unit, (unsigned long)g_gmac_base, macstr,
               (unsigned)l.speed_mbit, l.full_duplex ? "full" : "half");
    } else {
        printk("[EMAC] eth0 (gmac%d@0x%lx) registered, mac %s, link negotiating\n",
               g_gmac_unit, (unsigned long)g_gmac_base, macstr);
    }
    return 0;
}

struct netif *emac_get_netif(void) {
    return g_netif_registered ? &g_netif : NULL;
}

void emac_stats_report(void) {
    if (!g_netif_registered) {
        cprintf("EMAC: no interface registered (`emac link` for the PHY).\n");
        return;
    }
    uint32_t lo = REG32(g_gmac_base + EQOS_MAC_ADDR0_LOW);
    uint32_t hi = REG32(g_gmac_base + EQOS_MAC_ADDR0_HIGH);
    uint32_t missed = REG32(g_gmac_base + EQOS_MTL_RXQ0_MISSED_OVF);
    char macstr[18];
    netif_mac_str(g_netif.mac, macstr);

    cprintf("EMAC %s (gmac%d@0x%lx): mac %s\n",
            g_netif.name, g_gmac_unit, (unsigned long)g_gmac_base, macstr);
    cprintf("  filter   0x%08lx  %s\n",
            (unsigned long)REG32(g_gmac_base + EQOS_MAC_PKT_FILTER),
            (REG32(g_gmac_base + EQOS_MAC_PKT_FILTER) & (PKT_FILTER_RA | PKT_FILTER_PR))
                ? "PROMISCUOUS (receive all)" : "our unicast + broadcast/multicast");
    cprintf("  addr0    hi 0x%08lx lo 0x%08lx -> %02x:%02x:%02x:%02x:%02x:%02x\n",
            (unsigned long)hi, (unsigned long)lo,
            (unsigned)(lo & 0xffu), (unsigned)((lo >> 8) & 0xffu),
            (unsigned)((lo >> 16) & 0xffu), (unsigned)((lo >> 24) & 0xffu),
            (unsigned)(hi & 0xffu), (unsigned)((hi >> 8) & 0xffu));
    cprintf("  maccfg   0x%08lx   dma ch0 status 0x%08lx   mtl rxq0 0x%08lx\n",
            (unsigned long)REG32(g_gmac_base + EQOS_MAC_CFG),
            (unsigned long)REG32(g_gmac_base + EQOS_DMA_CH0_STATUS),
            (unsigned long)REG32(g_gmac_base + EQOS_MTL_RXQ0_OP_MODE));
    cprintf("  dma ch0  ctrl 0x%08lx txctrl 0x%08lx rxctrl 0x%08lx\n",
            (unsigned long)REG32(g_gmac_base + EQOS_DMA_CH0_CONTROL),
            (unsigned long)REG32(g_gmac_base + EQOS_DMA_CH0_TX_CONTROL),
            (unsigned long)REG32(g_gmac_base + EQOS_DMA_CH0_RX_CONTROL));
    cprintf("  dma desc tx_head 0x%08lx tx_tail 0x%08lx cur_tx 0x%08lx cur_txb 0x%08lx\n",
            (unsigned long)REG32(g_gmac_base + EQOS_DMA_CH0_TXDESC_ADDR),
            (unsigned long)REG32(g_gmac_base + EQOS_DMA_CH0_TXDESC_TAIL),
            (unsigned long)REG32(g_gmac_base + EQOS_DMA_CH0_CUR_TX_DESC),
            (unsigned long)REG32(g_gmac_base + EQOS_DMA_CH0_CUR_TX_BUF));
    cprintf("  dma desc rx_head 0x%08lx rx_tail 0x%08lx cur_rx 0x%08lx cur_rxb 0x%08lx\n",
            (unsigned long)REG32(g_gmac_base + EQOS_DMA_CH0_RXDESC_ADDR),
            (unsigned long)REG32(g_gmac_base + EQOS_DMA_CH0_RXDESC_TAIL),
            (unsigned long)REG32(g_gmac_base + EQOS_DMA_CH0_CUR_RX_DESC),
            (unsigned long)REG32(g_gmac_base + EQOS_DMA_CH0_CUR_RX_BUF));
    jh7110_flush_l2_range((uintptr_t)&g_tx_desc[0], (uintptr_t)(&g_tx_desc[0] + 1));
    jh7110_flush_l2_range((uintptr_t)&g_rx_desc[0], (uintptr_t)(&g_rx_desc[0] + 1));
    cprintf("  tx0 des0 0x%08lx des1 0x%08lx des2 0x%08lx des3 0x%08lx\n",
            (unsigned long)g_tx_desc[0].des0, (unsigned long)g_tx_desc[0].des1,
            (unsigned long)g_tx_desc[0].des2, (unsigned long)g_tx_desc[0].des3);
    cprintf("  rx0 des0 0x%08lx des1 0x%08lx des2 0x%08lx des3 0x%08lx\n",
            (unsigned long)g_rx_desc[0].des0, (unsigned long)g_rx_desc[0].des1,
            (unsigned long)g_rx_desc[0].des2, (unsigned long)g_rx_desc[0].des3);
    cprintf("  missed   %lu overflow, %lu missed\n",
            (unsigned long)(missed & 0x7ffu),
            (unsigned long)((missed >> 16) & 0x7ffu));
    cprintf("  rx       %lu frames, %lu bytes, %lu hardware errors/oversize\n",
            (unsigned long)g_netif.rx_frames, (unsigned long)g_netif.rx_bytes,
            (unsigned long)g_rx_hw_errors);
    cprintf("  tx       %lu frames, %lu bytes, %lu refused, %lu failed on the wire\n",
            (unsigned long)g_netif.tx_frames, (unsigned long)g_netif.tx_bytes,
            (unsigned long)g_netif.tx_errors, (unsigned long)g_tx_hw_errors);
    if (g_last_hdr_valid) {
        cprintf("  last rx  dst %02x:%02x:%02x:%02x:%02x:%02x  src "
                "%02x:%02x:%02x:%02x:%02x:%02x  type 0x%02x%02x\n",
                g_last_hdr[0], g_last_hdr[1], g_last_hdr[2],
                g_last_hdr[3], g_last_hdr[4], g_last_hdr[5],
                g_last_hdr[6], g_last_hdr[7], g_last_hdr[8],
                g_last_hdr[9], g_last_hdr[10], g_last_hdr[11],
                g_last_hdr[12], g_last_hdr[13]);
    } else {
        cprintf("  last rx  (nothing received yet)\n");
    }
}

void emac_set_promiscuous(bool on) {
    if (!g_netif_registered) {
        cprintf("EMAC: no interface registered.\n");
        return;
    }
    REG32(g_gmac_base + EQOS_MAC_PKT_FILTER) =
        on ? (PKT_FILTER_RA | PKT_FILTER_PR) : PKT_FILTER_PM;
    cprintf("EMAC: receive filter %s (0x%08lx)\n",
            on ? "OFF -- accepting every frame on the wire"
               : "on -- our unicast and broadcast only",
            (unsigned long)REG32(g_gmac_base + EQOS_MAC_PKT_FILTER));
}
