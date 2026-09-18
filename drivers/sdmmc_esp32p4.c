/* The ESP32-P4's SD/MMC host controller, and the microSD slot the
 * ESP32-P4-NANO wires to it. 35.1-35.5, plan/phase35_esp32p4_sdmmc.md.
 *
 * Provenance, because on this chip it is the whole ballgame (the rule is
 * plan/phase27_esp32p4_bringup.md §3.2, and drivers/emac_esp32p4.c's header
 * says why): every register offset and bit position below was taken from the
 * ESP32-P4 TRM and cross-checked against ESP-IDF's generated headers under
 * ~/gith/esp/esp-idf/components/soc/esp32p4/register/hw_ver1/soc/ -- the
 * hw_ver1 set, because this board is silicon revision v1.3. Where a fact is
 * this board's wiring rather than the chip's, the source is the schematic at
 * ~/gith/esp/datasheet/ESP32-P4-NANO-schematic.pdf and it is named as such.
 *
 * ## Three things about this peripheral that are not obvious
 *
 * **1. The card's power is not a given, and neither is the pads'.** The SD
 * signals live in the VDDPST_5 IO domain, and on this board VDDPST_5 is fed
 * by the chip's own LDO channel 4 (net ESP_LDO_VO4, schematic sheet
 * "MicroSD Card" and the ESP32-P4 symbol's pin 84). The same rail reaches the
 * card's VDD through a 0R link and an AO3401 P-channel switch whose gate is
 * GPIO45. So a driver that configures the controller perfectly and never
 * touches the PMU gets a dead bus with no error anywhere: the pads have no
 * supply. This is what SOC_SDMMC_IO_POWER_EXTERNAL means on this part, and
 * it is why sdmmc_power_on() runs before anything else here.
 *
 * **2. The transfers must be done by the controller's DMA, and the TRM says
 * otherwise.** §57.6 documents a CPU path -- read a block out of
 * SDHOST_BUFFIFO_REG, no descriptors, no cache maintenance -- and this driver
 * was written to use it. It does not work: on this silicon that read returns
 * the head of the FIFO forever and never pops it. The measurements that
 * establish that, and what the DMA path costs instead, are above
 * sdmmc_dma_arm() further down; they are the expensive finding of this phase
 * and they are recorded where the code that pays for them lives.
 *
 * **3. There is no card-detect switch to read.** The socket has one (pin 9,
 * CD) and the schematic pulls it up to SD1_VDD through R10 -- and routes it
 * nowhere. No GPIO sees it. So "is there a card" is answered by asking the
 * card, not by reading a pin, and the controller's own CDETECT input is tied
 * to a constant so that it never refuses a command for a card it cannot see.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "drivers/sdmmc.h"
#include "drivers/driver_task.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "arch/esp32p4_intr.h"   /* esp32p4_dcache_* */
#include "lugalos_config.h"

/* Stubbed rather than gated out of CMakeLists.txt's source list, the same
 * shape as drivers/spisd_rp2350.c: fs/vfs_server.c and kernel/board.c call
 * sdmmc_get_device() without a matching #if, and kernel/shell.c's `blkstats`
 * calls blk_task_call_count() the same way. The stubs are at the end. */
#if CONFIG_ENABLE_SDMMC

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* --- Peripheral bases (TRM Table 9.3-2, IDF soc/reg_base.h) -------------- */

#define HPPERIPH1_BASE      0x500C0000UL
#define GPIO_BASE           (HPPERIPH1_BASE + 0x20000UL)   /* 0x500E0000 */
#define IOMUX_BASE          (HPPERIPH1_BASE + 0x21000UL)   /* 0x500E1000 */
#define HP_SYS_CLKRST_BASE  (HPPERIPH1_BASE + 0x26000UL)   /* 0x500E6000 */
#define LPAON_BASE          0x50110000UL
#define LP_CLKRST_BASE      (LPAON_BASE + 0x1000UL)        /* 0x50111000 */
#define PMU_BASE            (LPAON_BASE + 0x5000UL)        /* 0x50115000 */

/* --- The card's power rail: LDO channel 4 (VO4) --------------------------
 *
 * TRM §15.4.2.9 ("Output Regulator Control"). Its Table 15.4-3 and Table
 * 15.4-4 are what tie the register names below to VO4 specifically, and they
 * are worth quoting because the mapping is not guessable: the four channels
 * are spread across two register pairs and VO4 is the *_1 half of the 0P2A
 * pair, while VO3 is the *_0 half of the same one.
 *
 *     VO3   PMU_0P2A_FORCE_TIEH_SEL_0 / TIEH_SEL_0 / TIEH_0 / XPD_0
 *     VO4   PMU_0P2A_FORCE_TIEH_SEL_1 / TIEH_SEL_1 / TIEH_1 / XPD_1
 *
 * The TRM's own worked example is this exact case -- §15.4.2.9.2, "Example 2:
 * VO4 powering VDD_IO_5 (SD card voltage switching) [...] 3.3 V output: use
 * bypass mode (for example, set PMU_0P2A_TIEH_1=1)". Bypass, not LDO mode:
 * the rail is already 3.3 V, so the regulator passes it through and DREF/MUL
 * mean nothing. That is also what IDF computes for a 3300 mV request
 * (ldo_ll_voltage_to_dref_mul()'s use_rail_voltage).
 *
 * The register pair is PMU_EXT_LDO_P1_0P2A_REG / _ANA_REG at PMU + 0x1d8 /
 * 0x1dc, which is ext_ldo[4] of the PMU's six -- IDF's ldo_ll.h maps LDO
 * unit 3 (channel 4) through index_array[] to exactly that index, and the
 * TRM's register list names 0x01D8 "VO4 regulator control register" outright.
 *
 * Both bit diagrams were rendered and counted (TRM Registers 15.64 and 15.65,
 * pages 1503-1504), because a wrong bit here is not a peripheral that fails to
 * answer -- it is a voltage on a card:
 *
 *     0x1D8  [30:23] TARGET0_1  [22:15] TARGET1_1  14 TIEH_1  13,12 reserved
 *            [11:9] TIEH_SEL_1  8 XPD_1  7 FORCE_TIEH_SEL_1  [6:0] reserved
 *     0x1DC  [31:28] DREF_1  27 EN_CUR_LIM_1  26 EN_VDET_1  [25:23] MUL_1
 */
#define PMU_EXT_LDO_VO4      (PMU_BASE + 0x1d8)
#define  LDO_FORCE_TIEH_SEL     (1u << 7)    /* 1 = software owns the mode */
#define  LDO_XPD                (1u << 8)    /* 1 = regulator enabled */
#define  LDO_TIEH_SEL_S         9            /* [11:9]; 0 = use TIEH below */
#define  LDO_TIEH_SEL_M         (7u << LDO_TIEH_SEL_S)
#define  LDO_TIEH               (1u << 14)   /* 1 = bypass, i.e. 3.3 V rail */
#define PMU_EXT_LDO_VO4_ANA  (PMU_BASE + 0x1dc)
#define  LDO_ANA_EN_VDET        (1u << 26)   /* ripple suppression */
#define  LDO_ANA_EN_CUR_LIM     (1u << 27)   /* inrush limit, on then off */

/* The power switch between VO4 and the card's VDD pin.
 *
 * Schematic: Q1, an AO3401 P-channel MOSFET. Source on the VO4 rail, gate on
 * GPIO45 with R27 (10K) to ground and R23 (the pull-up to the source) NOT
 * FITTED. A P-channel switch conducts when its gate is *low*, so the default
 * state of this board is "SD powered as soon as VO4 is up" and the only way
 * to break it is to drive GPIO45 high.
 *
 * Which is exactly why this driver drives it low explicitly, and does so
 * *before* enabling the regulator rather than after: if the ROM (or anything
 * else that ran first) left GPIO45 as an output driving high, the switch
 * would open the moment the rail it is switching came up, and the symptom
 * would be a card that never answers CMD0 on a board whose PMU registers all
 * read back correctly. */
#define SD_PWR_GPIO             ((uint32_t)CONFIG_SDMMC_PWR_GPIO)

/* --- Clock and reset -----------------------------------------------------
 *
 * Spread across three controllers, exactly like the EMAC's (see
 * drivers/emac_esp32p4.c's own note on this, which is where the shape was
 * first paid for): the bus clock gate is in HP_SYS_CLKRST_SOC_CLK_CTRL1, the
 * module's source select, enable and divider are in
 * HP_SYS_CLKRST_PERI_CLK_CTRL01/02, and the peripheral reset is in LP_CLKRST
 * -- a different peripheral in a different power domain.
 *
 * The one register this file shares with the EMAC driver is that last one:
 * LP_CLKRST_HP_SDMMC_EMAC_RST_CTRL_REG holds both parts' resets, and phase
 * 28 already read its bit diagram (bit 31 FORCE_NORST_EMAC, 30 RST_EN_EMAC,
 * 29 FORCE_NORST_SDMMC, 28 RST_EN_SDMMC). This is the SDMMC half of it. */
#define SOC_CLK_CTRL1       (HP_SYS_CLKRST_BASE + 0x18)
#define  SDMMC_SYS_CLK_EN       (1u << 14)   /* EMAC's is bit 13, right beside it */

#define REF_CLK_CTRL2       (HP_SYS_CLKRST_BASE + 0x2c)
#define  REF_160M_CLK_EN        (1u << 0)    /* reset value 1; written anyway */

#define PERI_CLK_CTRL01     (HP_SYS_CLKRST_BASE + 0x34)
#define  SDIO_HS_MODE           (1u << 22)
#define  SDIO_LS_CLK_SRC_SEL    (1u << 23)   /* 0 = PLL_F160M, 1 = SDIO_PLL 200M */
#define  SDIO_LS_CLK_EN         (1u << 24)

/* The host-side divider, and it is not a plain divisor field: the hardware
 * takes three *edge* numbers (TRM's SDIO_LS_CLK_EDGE_L/H/N) that describe one
 * period of the divided clock, and a write-1 update bit that commits them.
 * For a division by N: N = div-1, L = div-1, H = div/2-1, which is IDF's own
 * sdmmc_ll_set_clock_div(). div == 1 is not expressible that way and has its
 * own bit (SDIO_HS_MODE above); this driver never uses it. */
#define PERI_CLK_CTRL02     (HP_SYS_CLKRST_BASE + 0x38)
#define  SDIO_LS_DIV_NUM_M      0xffu        /* [7:0] */
#define  SDIO_LS_EDGE_UPDATE    (1u << 8)
#define  SDIO_LS_EDGE_L_S       9            /* [12:9] */
#define  SDIO_LS_EDGE_H_S       13           /* [16:13] */
#define  SDIO_LS_EDGE_N_S       17           /* [20:17] */
#define  SDIO_LS_SLF_EDGE_SEL_S 21           /* [22:21] */
#define  SDIO_LS_DRV_EDGE_SEL_S 23           /* [24:23] */
#define  SDIO_LS_SAM_EDGE_SEL_S 25           /* [26:25] */
#define  SDIO_LS_SLF_CLK_EN     (1u << 27)
#define  SDIO_LS_DRV_CLK_EN     (1u << 28)
#define  SDIO_LS_SAM_CLK_EN     (1u << 29)

#define LP_SDMMC_EMAC_RST   (LP_CLKRST_BASE + 0x4c)
#define  RST_EN_SDMMC           (1u << 28)

/* PLL_F160M. Named as a number because it is one: the two-stage divider below
 * is arithmetic on this, and a wrong value here is a card that initialises at
 * the wrong bus clock rather than one that fails. */
#define SDMMC_SRC_HZ            160000000u

/* --- IO_MUX and the GPIO matrix (TRM chapter 10) ------------------------
 *
 * Same register shapes drivers/emac_esp32p4.c documents; repeated here rather
 * than shared because a register half is never shared across drivers
 * (plan/hardware_seams.md §1), and because the pads this file touches are a
 * different set with different requirements.
 *
 * SDMMC **slot 0 has no GPIO-matrix option**: its six signals are IO_MUX pads
 * and nothing else (IDF's SDMMC_LL_SLOT_SUPPORT_GPIO_MATRIX(0) is 0, and
 * soc/sdmmc_pins.h fixes CLK=43 CMD=44 D0=39 D1=40 D2=41 D3=42 at function
 * 0). That is a piece of luck this board cashes in: the Waveshare wiring is
 * exactly slot 0's pad set, so there is no routing to get wrong. Card detect
 * and write protect are the exception -- those are never IO_MUX'd and always
 * arrive through the matrix, which is why the two constants below exist. */
#define IOMUX_PAD(n)        (IOMUX_BASE + 0x4 + 4u * (uint32_t)(n))
#define  IOMUX_MCU_SEL_S        12
#define  IOMUX_MCU_SEL_M        (7u << IOMUX_MCU_SEL_S)
#define  IOMUX_FUN_IE           (1u << 9)
#define  IOMUX_FUN_PU           (1u << 8)
#define  IOMUX_FUN_PD           (1u << 7)
#define  IOMUX_FUN_DRV_S        10           /* [11:10] */
#define  IOMUX_FUN_DRV_M        (3u << IOMUX_FUN_DRV_S)
#define  IOMUX_FUNC_SDMMC       0u
#define  IOMUX_FUNC_GPIO        1u

#define GPIO_IN_SEL(sig)    (GPIO_BASE + 0x158 + 4u * (uint32_t)(sig))
#define  GPIO_IN_SEL_ROUTE      (1u << 7)
#define GPIO_OUT_SEL(n)     (GPIO_BASE + 0x558 + 4u * (uint32_t)(n))
#define  GPIO_OUT_SEL_GPIO      256u
#define GPIO_OUT_W1TS_HI    (GPIO_BASE + 0x14)
#define GPIO_OUT_W1TC_HI    (GPIO_BASE + 0x18)
#define GPIO_ENABLE_W1TS_HI (GPIO_BASE + 0x30)

/* The matrix's two constant sources (IDF soc/gpio_pins.h) and the three
 * per-slot inputs that are never pads on this board. Slot 0's numbers, from
 * IDF's gpio_sig_map.h: SD_CARD_DETECT_N_1 = 126, SD_CARD_INT_N_1 = 128,
 * SD_CARD_WRITE_PRT_1 = 130. */
#define GPIO_MATRIX_CONST_ZERO  0x3Eu
#define GPIO_MATRIX_CONST_ONE   0x3Fu
#define SIG_SD_CARD_DETECT_N    126u
#define SIG_SD_CARD_INT_N       128u
#define SIG_SD_CARD_WRITE_PRT   130u

/* --- The controller itself (TRM chapter 57) -----------------------------
 *
 * A Synopsys DesignWare mobile-storage host, so the register set is the
 * familiar one; the offsets are the P4's, from IDF's sdmmc_reg.h. */
#define SD_BASE             ((uintptr_t)CONFIG_SDMMC_BASE)
#define SD_CTRL             (SD_BASE + 0x000)
#define  CTRL_RESET             (1u << 0)
#define  CTRL_FIFO_RESET        (1u << 1)
#define  CTRL_DMA_RESET         (1u << 2)
#define  CTRL_INT_ENABLE        (1u << 4)
/* Bits 5 and 25. **Not in the P4's generated *_reg.h at all** -- that header
 * stops naming CTRL fields at bit 11 -- but present in its sdmmc_struct.h as
 * `dma_enable` and `use_internal_dma`, and written by IDF's
 * sdmmc_ll_enable_dma() on this exact chip. A field that one Espressif header
 * documents and another omits is worth the sentence. */
#define  CTRL_DMA_ENABLE        (1u << 5)
#define  CTRL_USE_INTERNAL_DMA  (1u << 25)
#define SD_CLKDIV           (SD_BASE + 0x008)   /* [7:0] divider 0 */
#define SD_CLKSRC           (SD_BASE + 0x00c)   /* [1:0] which divider card 0 uses */
#define SD_CLKENA           (SD_BASE + 0x010)
#define  CLKENA_CCLK_EN_0       (1u << 0)
#define SD_TMOUT            (SD_BASE + 0x014)   /* [7:0] response, [31:8] data */
#define SD_CTYPE            (SD_BASE + 0x018)
#define  CTYPE_WIDTH4_0         (1u << 0)
#define SD_BLKSIZ           (SD_BASE + 0x01c)
#define SD_BYTCNT           (SD_BASE + 0x020)
#define SD_INTMASK          (SD_BASE + 0x024)
#define SD_CMDARG           (SD_BASE + 0x028)
#define SD_CMD              (SD_BASE + 0x02c)
#define SD_RESP(n)          (SD_BASE + 0x030 + 4u * (uint32_t)(n))
#define SD_RINTSTS          (SD_BASE + 0x044)
#define SD_STATUS           (SD_BASE + 0x048)
#define  STATUS_DATA_BUSY       (1u << 9)
#define  STATUS_FIFO_COUNT_S    17           /* [29:17], in 32-bit words */
#define  STATUS_FIFO_COUNT_M    0x1fffu
#define SD_FIFOTH           (SD_BASE + 0x04c)
#define SD_CDETECT          (SD_BASE + 0x050)
#define SD_WRTPRT           (SD_BASE + 0x054)
#define SD_RST_N            (SD_BASE + 0x078)
#define  RST_N_CARD_RESET_0     (1u << 0)
#define SD_BMOD             (SD_BASE + 0x080)
#define  BMOD_SWR               (1u << 0)
#define  BMOD_FB                (1u << 1)    /* fixed burst */
#define  BMOD_DE                (1u << 7)    /* the IDMAC's enable */
#define SD_PLDMND           (SD_BASE + 0x084)
#define SD_DBADDR           (SD_BASE + 0x088)
#define SD_IDSTS            (SD_BASE + 0x08c)
#define  IDSTS_FBE              (1u << 2)    /* fatal bus error */
#define  IDSTS_DU               (1u << 4)    /* descriptor unavailable */
#define  IDSTS_CES              (1u << 5)    /* card error summary */
#define  IDSTS_ERRORS           (IDSTS_FBE | IDSTS_DU | IDSTS_CES)
#define SD_IDINTEN          (SD_BASE + 0x090)
#define SD_BUFFIFO          (SD_BASE + 0x200)

/* SDHOST_CMD_REG's fields. The ones this driver does not use (CEATA, CCS,
 * stop/abort, the auto-stop for multi-block) are omitted rather than defined
 * and left unreferenced. */
#define  CMD_INDEX_M            0x3fu
#define  CMD_RESP_EXPECT        (1u << 6)
#define  CMD_RESP_LONG          (1u << 7)
#define  CMD_CHECK_RESP_CRC     (1u << 8)
#define  CMD_DATA_EXPECTED      (1u << 9)
#define  CMD_WRITE              (1u << 10)   /* 0 = read from card */
#define  CMD_WAIT_PRVDATA       (1u << 13)
#define  CMD_SEND_INIT          (1u << 15)   /* the 80 clocks before CMD0 */
#define  CMD_UPDATE_CLK_ONLY    (1u << 21)
#define  CMD_USE_HOLD_REG       (1u << 29)
#define  CMD_START              (1u << 31)

/* SDHOST_RINTSTS_REG. Raw, write-1-to-clear, and set regardless of INTMASK --
 * which is what makes a polled driver possible without ever routing an
 * interrupt (INTMASK stays 0 and CTRL_INT_ENABLE stays clear). */
#define  INT_CD                 (1u << 0)
#define  INT_RESP_ERR           (1u << 1)
#define  INT_CMD_DONE           (1u << 2)
#define  INT_DATA_OVER          (1u << 3)
#define  INT_RCRC               (1u << 6)
#define  INT_DCRC               (1u << 7)
#define  INT_RTO                (1u << 8)
#define  INT_DRTO               (1u << 9)
#define  INT_HTO                (1u << 10)
#define  INT_FRUN               (1u << 11)
#define  INT_HLE                (1u << 12)
#define  INT_SBE                (1u << 13)
#define  INT_EBE                (1u << 15)
#define  INT_CMD_ERRORS         (INT_RESP_ERR | INT_RCRC | INT_RTO)
#define  INT_DATA_ERRORS        (INT_DCRC | INT_DRTO | INT_HTO | INT_FRUN | \
                                 INT_SBE | INT_EBE)

/* --- What this driver remembers about the card in the slot --------------- */

static bool     g_present;        /* a card answered and is initialised */
static bool     g_block_addressed;/* SDHC/SDXC: LBA in blocks, not bytes */
static uint32_t g_rca;            /* the card's relative address, as published */
static uint32_t g_blocks;         /* capacity in 512-byte blocks, from the CSD */
static uint32_t g_bus_width = 1;  /* what the bus actually ended up at */
static uint32_t g_bus_khz;        /* what the card clock actually ended up at */
static uint32_t g_cid[4];         /* kept for `sdinfo` -- who this card is */

/* --- Power ---------------------------------------------------------------
 *
 * The order here is the point; see PMU_EXT_LDO_VO4's comment above for why
 * the switch is settled before the rail it switches.
 *
 * The regulator sequence itself is TRM §15.4.2.9.3's, minus the two steps
 * that only make sense with interrupts (it suggests waiting on the waiting
 * counter's "target reached" interrupts between enabling and dropping the
 * inrush limit). This driver has no interrupts, so it does what IDF's
 * esp_ldo_acquire_channel() does in the same situation: current limit on,
 * mode and owner set, ripple suppression on, XPD, current limit off, then a
 * delay long enough for the rail and the card's own 10 µF to settle. */
static void sdmmc_power_on(void) {
    /* The switch first, and as a plain GPIO output driving low. */
    uint32_t pad = REG(IOMUX_PAD(SD_PWR_GPIO));
    pad &= ~(IOMUX_MCU_SEL_M | IOMUX_FUN_PU | IOMUX_FUN_PD);
    pad |= (IOMUX_FUNC_GPIO << IOMUX_MCU_SEL_S);
    REG(IOMUX_PAD(SD_PWR_GPIO)) = pad;
    REG(GPIO_OUT_SEL(SD_PWR_GPIO)) = GPIO_OUT_SEL_GPIO;
    REG(GPIO_OUT_W1TC_HI) = 1u << (SD_PWR_GPIO - 32u);      /* gate low = ON */
    REG(GPIO_ENABLE_W1TS_HI) = 1u << (SD_PWR_GPIO - 32u);

    /* Then the rail. */
    REG(PMU_EXT_LDO_VO4_ANA) |= LDO_ANA_EN_CUR_LIM;

    uint32_t ctrl = REG(PMU_EXT_LDO_VO4);
    ctrl &= ~LDO_TIEH_SEL_M;               /* TIEH_SEL = 0: TIEH picks the mode */
    ctrl |= LDO_TIEH;                      /* bypass: pass the 3.3 V rail through */
    ctrl |= LDO_FORCE_TIEH_SEL;            /* and software, not eFuse, decides */
    REG(PMU_EXT_LDO_VO4) = ctrl;

    REG(PMU_EXT_LDO_VO4_ANA) |= LDO_ANA_EN_VDET;
    REG(PMU_EXT_LDO_VO4) |= LDO_XPD;
    REG(PMU_EXT_LDO_VO4_ANA) &= ~LDO_ANA_EN_CUR_LIM;

    /* The card's VDD pin has 10 µF + 0.1 µF on it (schematic C7/C8) and the
     * SD physical-layer spec gives a card 1 ms of supply ramp before it will
     * look at anything. 10 ms is that with room to spare, paid once. */
    time_delay_us(10000);
}

/* --- Pads ---------------------------------------------------------------
 *
 * Six IO_MUX pads at function 0, plus the internal pull-ups.
 *
 * The board already fits 51K externals on CMD and D0-D3 (schematic R5-R9,
 * pulled to SD1_VDD), which is weaker than the 10K the SD spec asks for. The
 * internal pull-ups are roughly 45K, so enabling them puts about 24K on each
 * line -- still not 10K, but a great deal closer, and it costs nothing. IDF's
 * own example enables them for the same reason and with the same caveat.
 *
 * CLK is driven by the host at both ends of every edge and wants no pull at
 * all. Every pad gets the strongest drive (3): these are 20 MHz edges into a
 * connector, and the P4's default of 2 is chosen for pins that are not. */
static void sdmmc_pad_init(uint32_t gpio, bool pull_up) {
    uint32_t pad = REG(IOMUX_PAD(gpio));
    pad &= ~(IOMUX_MCU_SEL_M | IOMUX_FUN_PU | IOMUX_FUN_PD | IOMUX_FUN_DRV_M);
    pad |= (IOMUX_FUNC_SDMMC << IOMUX_MCU_SEL_S);
    pad |= IOMUX_FUN_IE;                   /* CMD and D0-D3 are bidirectional */
    pad |= (3u << IOMUX_FUN_DRV_S);
    if (pull_up) pad |= IOMUX_FUN_PU;
    REG(IOMUX_PAD(gpio)) = pad;
}

static void sdmmc_pads_init(void) {
    sdmmc_pad_init((uint32_t)CONFIG_SDMMC_CLK_GPIO, false);
    sdmmc_pad_init((uint32_t)CONFIG_SDMMC_CMD_GPIO, true);
    sdmmc_pad_init((uint32_t)CONFIG_SDMMC_D0_GPIO, true);
    sdmmc_pad_init((uint32_t)CONFIG_SDMMC_D1_GPIO, true);
    sdmmc_pad_init((uint32_t)CONFIG_SDMMC_D2_GPIO, true);
    sdmmc_pad_init((uint32_t)CONFIG_SDMMC_D3_GPIO, true);

    /* Card detect, write protect and the SDIO interrupt line: none of them is
     * a pad on this board (see the file header's point 3), so each is tied to
     * a constant instead of being left at whatever the matrix defaults to.
     *
     * CD is active low, so a constant 0 means "a card is present" -- without
     * it the controller refuses every command with no error this driver could
     * report. WP is active high, so a constant 0 means "not write protected".
     * The SDIO interrupt is constant 1 because that input is
     * ~(int_n | card_int | card_detect) and a 0 there would assert it
     * permanently; this driver never enables the interrupt, but leaving a
     * peripheral input floating on a matrix default is how phase 28's RX pad
     * lists became a warning in the board file. */
    REG(GPIO_IN_SEL(SIG_SD_CARD_DETECT_N)) = GPIO_MATRIX_CONST_ZERO | GPIO_IN_SEL_ROUTE;
    REG(GPIO_IN_SEL(SIG_SD_CARD_WRITE_PRT)) = GPIO_MATRIX_CONST_ZERO | GPIO_IN_SEL_ROUTE;
    REG(GPIO_IN_SEL(SIG_SD_CARD_INT_N)) = GPIO_MATRIX_CONST_ONE | GPIO_IN_SEL_ROUTE;
}

/* --- Clocks -------------------------------------------------------------- */

/* The host-side divider: PLL_F160M / div, expressed as the three edge numbers
 * the hardware actually takes. div must be 2..16. */
static void sdmmc_set_host_div(uint32_t div) {
    uint32_t v = REG(PERI_CLK_CTRL02);
    v &= ~((0xfu << SDIO_LS_EDGE_L_S) | (0xfu << SDIO_LS_EDGE_H_S) |
           (0xfu << SDIO_LS_EDGE_N_S));
    v |= ((div - 1u) & 0xfu) << SDIO_LS_EDGE_L_S;
    v |= ((div / 2u - 1u) & 0xfu) << SDIO_LS_EDGE_H_S;
    v |= ((div - 1u) & 0xfu) << SDIO_LS_EDGE_N_S;
    REG(PERI_CLK_CTRL02) = v;
    REG(PERI_CLK_CTRL02) = v | SDIO_LS_EDGE_UPDATE;
    REG(PERI_CLK_CTRL02) = v;
}

static void sdmmc_clk_init(void) {
    /* The module's bus clock, and its reset. RST_EN is 1 = held in reset. */
    REG(SOC_CLK_CTRL1) |= SDMMC_SYS_CLK_EN;
    REG(LP_SDMMC_EMAC_RST) |= RST_EN_SDMMC;
    REG(LP_SDMMC_EMAC_RST) &= ~RST_EN_SDMMC;

    /* The 160 MHz reference this peripheral divides down. Its gate resets to
     * 1, so this write normally changes nothing -- which is exactly the
     * argument drivers/emac_esp32p4.c makes for writing its own always-on
     * gate anyway: a gate nobody writes is a gate nobody notices when
     * something else clears it. */
    REG(REF_CLK_CTRL2) |= REF_160M_CLK_EN;

    uint32_t v = REG(PERI_CLK_CTRL01);
    v &= ~(SDIO_LS_CLK_SRC_SEL | SDIO_HS_MODE);   /* PLL_F160M, divided */
    v |= SDIO_LS_CLK_EN;
    REG(PERI_CLK_CTRL01) = v;

    /* The drive/sample/self clock phases. IDF's sdmmc_ll_init_phase_delay():
     * drive on the falling edge (sel 1), sample and self on the rising one
     * (sel 0). These are the default-speed settings and this driver never
     * leaves default speed -- a DDR or SDR104 mode would have to revisit
     * them, and would have a good deal more to revisit besides. */
    v = REG(PERI_CLK_CTRL02);
    v |= SDIO_LS_SLF_CLK_EN | SDIO_LS_DRV_CLK_EN | SDIO_LS_SAM_CLK_EN;
    v &= ~((3u << SDIO_LS_SLF_EDGE_SEL_S) | (3u << SDIO_LS_DRV_EDGE_SEL_S) |
           (3u << SDIO_LS_SAM_EDGE_SEL_S));
    v |= (1u << SDIO_LS_DRV_EDGE_SEL_S);
    REG(PERI_CLK_CTRL02) = v;
    REG(PERI_CLK_CTRL02) = v | SDIO_LS_EDGE_UPDATE;
    REG(PERI_CLK_CTRL02) = v;

    /* **A divider, before the controller is reset.** MEASURED, 2026-09-18:
     * without this line the very first hardware run printed
     *
     *     [SDMMC] controller reset never completed -- no module clock?
     *
     * and it was telling the truth. The three edge fields reset to zero, and
     * all-zero is NOT "divide by one" -- division by one has its own bit
     * (SDIO_HS_MODE), which is why sdmmc_set_host_div() is documented as
     * taking 2..16 and why IDF's own set_clock_div() branches on div == 1
     * rather than encoding it. All-zero edges with HS_MODE clear is a stopped
     * clock, and a stopped module clock means CTRL's three reset bits are
     * never cleared by hardware, because nothing is clocking the logic that
     * would clear them.
     *
     * So the module needs a working divider before it is reset, not merely
     * before it is used. Two, which is IDF's SDMMC_LL_DEFAULT_DIV at exactly
     * this point in its own bring-up; sdmmc_set_bus_clock() replaces it with
     * the identification-mode value moments later. */
    sdmmc_set_host_div(2);
}

/* --- Commands ------------------------------------------------------------
 *
 * Every command goes through here, including the "command" that is not one:
 * a write to SDHOST_CMD_REG with UPDATE_CLOCK_REGISTERS_ONLY set sends no
 * command to the card at all, it commits the clock registers to the card
 * interface unit. Changing the bus clock means three of them around the
 * writes, which is why it is not a special case below but the ordinary path.
 *
 * START_CMD self-clears when the unit accepts the command, so "has it been
 * taken" is a poll on that bit rather than on an interrupt. */
static int sdmmc_cmd_start(uint32_t cmd, uint32_t arg) {
    uint64_t deadline = time_get_us() + 100000u;      /* 100 ms */

    while (REG(SD_CMD) & CMD_START) {
        if (time_get_us() > deadline) return -1;
    }
    REG(SD_CMDARG) = arg;
    REG(SD_CMD) = cmd | CMD_START | CMD_USE_HOLD_REG;
    while (REG(SD_CMD) & CMD_START) {
        if (time_get_us() > deadline) return -1;
    }
    return 0;
}

static int sdmmc_clock_update(void) {
    return sdmmc_cmd_start(CMD_UPDATE_CLK_ONLY | CMD_WAIT_PRVDATA, 0);
}

/* Sets the card clock as close to `want_khz` as the two dividers allow, and
 * returns what was actually achieved.
 *
 * Two stages, as TRM §57.5 and IDF both describe them: a host divider of
 * 2..16 on PLL_F160M, then the controller's own divider, which divides by
 * 2*n (n = 0 bypasses it). The search below takes the smallest host divider
 * that lets the second stage reach the target, which keeps the intermediate
 * clock high and the quantisation fine: 400 kHz comes out as 160/2/(2*100)
 * and 20 MHz as 160/8 with the second stage bypassed. Both are exact. */
static uint32_t sdmmc_set_bus_clock(uint32_t want_khz) {
    uint32_t want_hz = want_khz * 1000u;
    uint32_t host_div = 2, card_div = 0;

    uint32_t total = (SDMMC_SRC_HZ + want_hz - 1u) / want_hz;
    if (total <= 16u) {
        host_div = (total < 2u) ? 2u : total;
        card_div = 0;
    } else {
        for (uint32_t h = 2; h <= 16u; h++) {
            uint32_t inter = SDMMC_SRC_HZ / h;
            uint32_t c = (inter + 2u * want_hz - 1u) / (2u * want_hz);
            if (c < 1u) c = 1u;
            if (c <= 255u) { host_div = h; card_div = c; break; }
        }
    }

    REG(SD_CLKENA) = 0;
    if (sdmmc_clock_update() != 0) return 0;

    REG(SD_CLKSRC) = 0;                    /* card 0 takes divider 0 */
    REG(SD_CLKDIV) = card_div;
    sdmmc_set_host_div(host_div);
    if (sdmmc_clock_update() != 0) return 0;

    REG(SD_CLKENA) = CLKENA_CCLK_EN_0;
    if (sdmmc_clock_update() != 0) return 0;

    uint32_t real_hz = SDMMC_SRC_HZ / host_div / (card_div ? (2u * card_div) : 1u);

    /* Timeouts, in card-clock cycles, so they follow the clock rather than
     * being a constant that means different things at 400 kHz and 20 MHz.
     * 100 ms of data timeout is what IDF uses and is far longer than any
     * single-block read; the response timeout field is only 8 bits, and 255
     * cycles is generous for a response that must arrive within 64. */
    uint32_t data_timeout = (real_hz / 1000u) * 100u;
    if (data_timeout > 0xffffffu) data_timeout = 0xffffffu;
    REG(SD_TMOUT) = (data_timeout << 8) | 0xffu;

    return real_hz / 1000u;
}

/* Resets the controller, the FIFO and the (unused) internal DMA, and waits
 * for the hardware to clear the three bits. Any of them still set after this
 * means the module has no clock -- which is the failure this function exists
 * to turn into a return value rather than a hang. */
static int sdmmc_host_reset(void) {
    REG(SD_CTRL) |= CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET;
    uint64_t deadline = time_get_us() + 100000u;
    while (REG(SD_CTRL) & (CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET)) {
        if (time_get_us() > deadline) return -1;
    }
    return 0;
}

/* --- The SD protocol on top of it ---------------------------------------
 *
 * The response kinds this driver needs. R7 (CMD8's voltage echo) and R6
 * (CMD3's published address) are both short CRC-checked responses and so
 * share R1's shape; R3 (the OCR, from ACMD41) is the one that must NOT be
 * CRC-checked, because the card does not compute one for it -- asking the
 * controller to check it turns every ACMD41 into an RCRC error. */
typedef enum {
    RESP_NONE = 0,
    RESP_R1,      /* and R6, R7 */
    RESP_R1B,     /* R1 plus the card holding DAT0 low while it works */
    RESP_R2,      /* 136-bit: CID, CSD */
    RESP_R3,      /* OCR, no CRC */
} sd_resp_t;

/* Sends one command and collects its response. Returns 0, or -1 with the raw
 * status left in *out_status for the caller to report -- this function
 * deliberately prints nothing, because its callers know which failures are
 * expected (CMD8 timing out is how an SDv1 card announces itself) and which
 * are not. */
static int sd_command(uint8_t index, uint32_t arg, sd_resp_t rt,
                      uint32_t data_flags, uint32_t *resp, uint32_t *out_status) {
    uint32_t cmd = (uint32_t)index & CMD_INDEX_M;

    switch (rt) {
        case RESP_NONE: break;
        case RESP_R1:
        case RESP_R1B:  cmd |= CMD_RESP_EXPECT | CMD_CHECK_RESP_CRC; break;
        case RESP_R2:   cmd |= CMD_RESP_EXPECT | CMD_RESP_LONG | CMD_CHECK_RESP_CRC; break;
        case RESP_R3:   cmd |= CMD_RESP_EXPECT; break;
    }
    cmd |= data_flags | CMD_WAIT_PRVDATA;

    REG(SD_RINTSTS) = 0xffffffffu;     /* raw status is write-1-to-clear */
    if (sdmmc_cmd_start(cmd, arg) != 0) {
        if (out_status) *out_status = 0;
        return -1;
    }

    uint64_t deadline = time_get_us() + 500000u;
    uint32_t status;
    for (;;) {
        status = REG(SD_RINTSTS);
        if (status & (INT_CMD_DONE | INT_CMD_ERRORS)) break;
        if (time_get_us() > deadline) {
            if (out_status) *out_status = status;
            return -1;
        }
    }
    if (out_status) *out_status = status;
    if (status & INT_CMD_ERRORS) return -1;

    if (resp) {
        if (rt == RESP_R2) {
            /* RESP0 is the LOW word of the 128-bit response, so resp[0] holds
             * CSD/CID bits [31:0] and resp[3] holds [127:96]. That is the
             * order the bit numbers in sd_bits() below assume, and it is the
             * opposite of the convention Linux's MMC core uses -- worth being
             * explicit about, since both are "the obvious one" to someone who
             * has read the other. */
            resp[0] = REG(SD_RESP(0));
            resp[1] = REG(SD_RESP(1));
            resp[2] = REG(SD_RESP(2));
            resp[3] = REG(SD_RESP(3));
        } else if (rt != RESP_NONE) {
            resp[0] = REG(SD_RESP(0));
        }
    }

    if (rt == RESP_R1B) {
        /* The card pulls DAT0 low while it finishes. Busy-polled rather than
         * yielded: this is CMD7 during init and nothing else. */
        deadline = time_get_us() + 1000000u;
        while (REG(SD_STATUS) & STATUS_DATA_BUSY) {
            if (time_get_us() > deadline) return -1;
        }
    }
    return 0;
}

/* An application command: CMD55 with the card's address, then the ACMD. */
static int sd_acommand(uint8_t index, uint32_t arg, sd_resp_t rt,
                       uint32_t *resp, uint32_t *out_status) {
    if (sd_command(55, g_rca << 16, RESP_R1, 0, NULL, out_status) != 0) return -1;
    return sd_command(index, arg, rt, 0, resp, out_status);
}

/* Bit field out of a 128-bit response, numbered the way the SD spec numbers
 * the CSD -- bit 0 is the low bit of resp[0], which is where the CRC lives,
 * and CSD_STRUCTURE is at 126. Same arithmetic as IDF's MMC_RSP_BITS(); the
 * field positions below are the spec's and IDF's alike. */
static uint32_t sd_bits(const uint32_t *r, uint32_t start, uint32_t len) {
    uint32_t mask = (len % 32u == 0u) ? 0xffffffffu : (0xffffffffu >> (32u - (len % 32u)));
    uint32_t word = start / 32u;
    uint32_t shift = start % 32u;
    if (word > 3u) return 0;
    uint32_t right = r[word] >> shift;
    uint32_t left = (len + shift <= 32u || word >= 3u) ? 0u : (r[word + 1u] << ((32u - shift) % 32u));
    return (left | right) & mask;
}

/* Capacity in 512-byte blocks, which is what block_dev_t wants.
 *
 * Two CSD layouts, and the difference is not cosmetic: a v2 CSD (every SDHC
 * and SDXC card) states the capacity directly in 512 KB units, while a v1 one
 * states it as a count of blocks whose size the card also chooses -- a 1 GB
 * card with READ_BL_LEN 10 reports half as many blocks of twice the size, so
 * the last step below is a conversion, not a rounding. Returns 0 for a CSD
 * version this driver does not know, which is an honest "no capacity" rather
 * than a plausible wrong number. */
static uint32_t sd_csd_blocks(const uint32_t *csd) {
    uint32_t ver = sd_bits(csd, 126, 2);
    if (ver == 1u) {
        return (sd_bits(csd, 48, 22) + 1u) << 10;
    }
    if (ver == 0u) {
        uint32_t c_size = sd_bits(csd, 62, 12);
        uint32_t c_mult = sd_bits(csd, 47, 3);
        uint32_t bl_len = sd_bits(csd, 80, 4);
        uint32_t blocks = (c_size + 1u) << (c_mult + 2u);
        if (bl_len > 9u) blocks <<= (bl_len - 9u);   /* into 512-byte units */
        return blocks;
    }
    return 0;
}

/* --- Data transfers ------------------------------------------------------
 *
 * Through the controller's own DMA, and **not** through the CPU FIFO path the
 * TRM documents. That is a correction, and it is the expensive finding of this
 * phase, so it is recorded here rather than in a commit message.
 *
 * TRM §57.6 says a block can be moved by reading SDHOST_BUFFIFO_REG, and
 * §57.9.2 step 7 repeats it. **On this silicon that read never pops the
 * FIFO.** Measured on the bench, 2026-09-18, on a card that was otherwise
 * working perfectly:
 *
 *   - CMD17 completed, DATA_OVER was set, no error bit anywhere.
 *   - STATUS.fifo_count read 128 -- exactly one block of 32-bit words, so the
 *     data had arrived in the FIFO.
 *   - Reading SDHOST_BUFFIFO_REG 128 times returned the *same* word 128 times
 *     (the block's real first word, 0x6d9058eb), and fifo_count stayed at 128
 *     afterwards. Reading through the rest of the 0x200-0x7FF window behaved
 *     identically. Console output between reads rules out any load merging.
 *   - The write side is fine: fifo_reset takes the count to 0, and reading
 *     block 1 afterwards puts *that* block's first word at the head
 *     (0x41615252, "RRaA" -- the FAT32 FSInfo signature, which is exactly what
 *     sector 1 of this card should contain).
 *   - Setting RX_WMARK to 0, so the FIFO request is raised continuously,
 *     changed nothing.
 *
 * So the FIFO's read port simply is not wired to the CPU here, whatever the
 * TRM says. ESP-IDF's agreement is retrospective evidence: its SDMMC driver
 * has only ever used the IDMAC, on every ESP32 part, with no FIFO-mode branch
 * to fall back to. This is the same class of error the project already
 * recorded for this chip's UART, where the TRM describes TXFIFO_CNT as an RX
 * count -- the register diagrams are reliable, the prose around them is not.
 *
 * What that costs, and how it is paid:
 *
 *   - **A descriptor**, padded to a full 64-byte cache line for the reason
 *     drivers/emac_esp32p4.c gives for its rings: the CPU and the DMA both
 *     write it, so it needs cache maintenance, and an invalidate discards a
 *     whole line.
 *   - **A bounce buffer**, because cache maintenance on a caller's buffer is
 *     only safe when that buffer is cache-line aligned and a whole number of
 *     lines long, and a `block_dev_t` caller promises neither. Invalidating a
 *     partial line would discard whatever else shares it. 512 bytes and one
 *     memcpy per block is the price of not having that bug.
 *
 * Multi-block is still deliberately absent -- see the phase plan's §5. The
 * descriptor below would extend to it by growing into a small ring.
 */

/* DES0, TRM Table 57.8-1. */
#define DESC_OWN            (1u << 31)   /* 1 = the DMA's, 0 = the CPU's */
#define DESC_CH             (1u << 4)    /* DES3 is the next descriptor */
#define DESC_FS             (1u << 3)    /* first descriptor of a transfer */
#define DESC_LD             (1u << 2)    /* last descriptor of a transfer */
#define DESC_DIC            (1u << 1)    /* no interrupt on completion */

typedef struct {
    volatile uint32_t des0;
    volatile uint32_t des1;   /* buffer sizes: [12:0] and [25:13] */
    volatile uint32_t des2;   /* buffer 1 address */
    volatile uint32_t des3;   /* chained: the next descriptor's address */
    uint32_t pad[12];         /* one descriptor per cache line, see above */
} sdmmc_desc_t;

static sdmmc_desc_t g_desc  __attribute__((aligned(ESP32P4_L1_CACHE_LINE)));
static uint8_t g_dma_buf[512] __attribute__((aligned(ESP32P4_L1_CACHE_LINE)));

static uint32_t sdmmc_fifo_count(void) {
    return (REG(SD_STATUS) >> STATUS_FIFO_COUNT_S) & STATUS_FIFO_COUNT_M;
}

/* Arms the IDMAC on the bounce buffer for one 512-byte transfer. The caller
 * has already put the data there (a write) or is about to read it out (a
 * read); this only hands the buffer over. */
static void sdmmc_dma_arm(void) {
    /* The bounded wait below reports nothing if it expires, and that is
     * deliberate: the only way these bits stay set is a module with no clock,
     * which sdmmc_host_init() has already refused to proceed past. If one
     * ever did hang here, the transfer that follows fails and says so with
     * both status registers in hand, which is a better message than this
     * function could write. */
    REG(SD_CTRL) |= CTRL_DMA_RESET | CTRL_FIFO_RESET;
    uint64_t deadline = time_get_us() + 10000u;
    while ((REG(SD_CTRL) & (CTRL_DMA_RESET | CTRL_FIFO_RESET)) &&
           time_get_us() < deadline) { }

    REG(SD_BMOD) = BMOD_SWR;
    REG(SD_IDSTS) = 0xffffffffu;      /* write-1-to-clear, like RINTSTS */
    REG(SD_IDINTEN) = 0;              /* polled; nothing reaches the CLIC */

    g_desc.des0 = DESC_OWN | DESC_CH | DESC_FS | DESC_LD | DESC_DIC;
    g_desc.des1 = 512u;
    g_desc.des2 = (uint32_t)(uintptr_t)g_dma_buf;
    g_desc.des3 = (uint32_t)(uintptr_t)&g_desc;   /* chained back to itself */
    esp32p4_dcache_writeback((uintptr_t)&g_desc, sizeof(g_desc));

    REG(SD_CTRL) |= CTRL_DMA_ENABLE | CTRL_USE_INTERNAL_DMA;
    REG(SD_DBADDR) = (uint32_t)(uintptr_t)&g_desc;
    REG(SD_BMOD) = BMOD_DE | BMOD_FB;
    REG(SD_PLDMND) = 1;

    REG(SD_BLKSIZ) = 512;
    REG(SD_BYTCNT) = 512;
}

/* Waits for the data phase and reports what went wrong if it did. Both status
 * registers matter: RINTSTS carries the card-facing errors (CRC, timeouts)
 * and IDSTS the bus-facing ones (a descriptor the DMA could not fetch, a bus
 * error), and a transfer can fail in either without the other noticing. */
static int sdmmc_dma_wait(uint32_t lba, const char *what) {
    uint64_t deadline = time_get_us() + 500000u;
    for (;;) {
        uint32_t st = REG(SD_RINTSTS);
        uint32_t id = REG(SD_IDSTS);
        if ((st & INT_DATA_ERRORS) || (id & IDSTS_ERRORS)) {
            printk("[SDMMC] %s lba %u failed (RINTSTS 0x%x IDSTS 0x%x)\n",
                   what, (unsigned)lba, (unsigned)st, (unsigned)id);
            return -1;
        }
        if (st & INT_DATA_OVER) return 0;
        if (time_get_us() > deadline) {
            printk("[SDMMC] %s lba %u timed out (RINTSTS 0x%x IDSTS 0x%x FIFO %u)\n",
                   what, (unsigned)lba, (unsigned)st, (unsigned)id,
                   (unsigned)sdmmc_fifo_count());
            return -1;
        }
    }
}

static int sdmmc_hw_read_block(uint32_t lba, uint8_t *dst) {
    /* Invalidate before handing the buffer over, not only after: the CPU may
     * hold dirty lines over it from the last transfer, and a writeback landing
     * after the DMA has filled it would overwrite the card's data with stale
     * bytes. drivers/emac_esp32p4.c's receive ring makes the same argument. */
    esp32p4_dcache_invalidate((uintptr_t)g_dma_buf, sizeof(g_dma_buf));
    sdmmc_dma_arm();

    uint32_t addr = g_block_addressed ? lba : (lba * 512u);
    uint32_t status = 0;
    if (sd_command(17, addr, RESP_R1, CMD_DATA_EXPECTED, NULL, &status) != 0) {
        printk("[SDMMC] CMD17 lba %u failed (status 0x%x)\n",
               (unsigned)lba, (unsigned)status);
        return -1;
    }
    if (sdmmc_dma_wait(lba, "read") != 0) return -1;

    esp32p4_dcache_invalidate((uintptr_t)g_dma_buf, sizeof(g_dma_buf));
    memcpy(dst, g_dma_buf, 512);
    return 0;
}

static int sdmmc_hw_write_block(uint32_t lba, const uint8_t *src) {
    memcpy(g_dma_buf, src, 512);
    esp32p4_dcache_writeback((uintptr_t)g_dma_buf, sizeof(g_dma_buf));
    sdmmc_dma_arm();

    uint32_t addr = g_block_addressed ? lba : (lba * 512u);
    uint32_t status = 0;
    if (sd_command(24, addr, RESP_R1, CMD_DATA_EXPECTED | CMD_WRITE, NULL, &status) != 0) {
        printk("[SDMMC] CMD24 lba %u failed (status 0x%x)\n",
               (unsigned)lba, (unsigned)status);
        return -1;
    }
    if (sdmmc_dma_wait(lba, "write") != 0) return -1;

    /* The card programs its flash with DAT0 held low, and may hold it for
     * milliseconds. Busy-polled, like the RP2350 driver's own write path:
     * yielding here would mean a serve callback rescheduling inside a
     * transfer, and the block driver's contract is that only it touches the
     * hardware while one is in flight. */
    uint64_t deadline = time_get_us() + 1000000u;
    while (REG(SD_STATUS) & STATUS_DATA_BUSY) {
        if (time_get_us() > deadline) {
            printk("[SDMMC] write lba %u: card still busy after 1 s\n", (unsigned)lba);
            return -1;
        }
    }
    return 0;
}

/* --- Bringing a card up --------------------------------------------------
 *
 * The host half (power, pads, clocks, controller reset) is idempotent and is
 * done once; the card half runs again on every probe, because a slot is
 * allowed to be empty at boot and full later.
 *
 * An empty slot is cheap to discover and that is deliberate: CMD8 and then
 * CMD55 both come back as response timeouts within a few dozen card clocks,
 * so the whole probe costs microseconds rather than the 1.5 s the ACMD41 loop
 * is budgeted. The budget only ever applies to a card that is answering and
 * still initialising. */
static bool g_hw_ready;

static void sdmmc_host_init(void) {
    if (g_hw_ready) return;

    sdmmc_power_on();
    sdmmc_pads_init();
    sdmmc_clk_init();

    if (sdmmc_host_reset() != 0) {
        printk("[SDMMC] controller reset never completed -- no module clock?\n");
        return;
    }

    /* Polled, start to finish: no interrupt is routed, INTMASK stays clear so
     * nothing reaches the interrupt controller, and RINTSTS (which is raw and
     * set regardless) is what this driver reads. The EMAC made the same call
     * for the same reason -- see plan/phase28's Z6. */
    REG(SD_INTMASK) = 0;
    REG(SD_CTRL) &= ~CTRL_INT_ENABLE;
    REG(SD_RINTSTS) = 0xffffffffu;

    /* The internal DMA, explicitly off. BMOD.DE is the only enable it has on
     * this chip -- SDHOST_CTRL_REG here has no USE_INTERNAL_DMA bit at all,
     * unlike the S3's -- so clearing BMOD is what selects the CPU path. */
    REG(SD_BMOD) = 0;

    /* FIFO watermarks: the half-depth values, and a note about how much they
     * matter. They decide when the IDMAC is asked to move data mid-transfer;
     * a transfer also moves at end of packet "regardless of threshold
     * programming" (the TRM's own words on SDHOST_RX_WMARK), which is why a
     * 128-word block completes whatever these say. IDF never writes this
     * register at all, leaving it at its reset value of 0 -- so these are a
     * considered choice rather than a copied one, exercised at 20 MHz on real
     * transfers. If a multi-block path ever lands (phase plan §5) this is one
     * of the first registers to revisit, because there the mid-transfer
     * behaviour is the whole game. */
    REG(SD_FIFOTH) = (255u << 16) | 256u;

    /* The card's reset output, which the NANO does not wire to anything --
     * the socket has no reset pin. Left released, which is its reset value. */
    REG(SD_RST_N) |= RST_N_CARD_RESET_0;

    REG(SD_CTYPE) = 0;                       /* 1-bit until ACMD6 says otherwise */
    g_bus_width = 1;
    g_bus_khz = sdmmc_set_bus_clock(400);    /* identification mode */
    g_hw_ready = true;
}

/* Switches the bus to 4 bits and proves it, or stays at 1.
 *
 * The proof is a read of block 0 at the final clock, and it is the only
 * honest way to make this claim: ACMD6 returns R1 with no error whether or
 * not the data lines are actually connected, so a driver that trusts the
 * response reports a 4-bit bus and then fails on its first real read. D1 and
 * D2 in particular do nothing at all in 1-bit mode, so nothing before this
 * point has ever driven them.
 *
 * `probe` is 512 bytes of .bss rather than a local because this runs on
 * whatever stack the first caller happens to have -- vfs_server_init()'s,
 * during boot -- and half a kilobyte is a great deal to ask of one. */
static uint8_t g_probe_block[512];

static void sdmmc_set_bus_width(void) {
    uint32_t want = (uint32_t)CONFIG_SDMMC_BUS_WIDTH;
    uint32_t status = 0;

    if (want == 4u) {
        if (sd_acommand(6, 2, RESP_R1, NULL, &status) == 0) {
            REG(SD_CTYPE) = CTYPE_WIDTH4_0;
            g_bus_width = 4;
        } else {
            printk("[SDMMC] ACMD6 refused 4-bit (status 0x%x); staying 1-bit\n",
                   (unsigned)status);
        }
    }

    g_bus_khz = sdmmc_set_bus_clock((uint32_t)CONFIG_SDMMC_FREQ_KHZ);

    if (sdmmc_hw_read_block(0, g_probe_block) == 0) return;

    if (g_bus_width == 4u) {
        printk("[SDMMC] 4-bit bus did not read back; falling back to 1-bit\n");
        if (sd_acommand(6, 0, RESP_R1, NULL, &status) == 0) {
            REG(SD_CTYPE) = 0;
            g_bus_width = 1;
            if (sdmmc_hw_read_block(0, g_probe_block) == 0) return;
        }
    }

    /* Neither width could read block 0 at the target clock. Say so and leave
     * the card marked present anyway: the filesystem mount below will fail
     * next and report it in the ordinary way, which is a better error than a
     * card that silently is not there. */
    printk("[SDMMC] block 0 unreadable at %u kHz, %u-bit\n",
           (unsigned)g_bus_khz, (unsigned)g_bus_width);
}

static int sdmmc_card_init(void) {
    uint32_t resp[4] = {0, 0, 0, 0};
    uint32_t status = 0;

    sdmmc_host_init();
    if (!g_hw_ready) return -1;

    g_present = false;
    g_block_addressed = false;
    g_rca = 0;
    g_blocks = 0;

    /* Back to identification mode for the retry case: a second probe on a
     * card that was already initialised must start where the first one did. */
    REG(SD_CTYPE) = 0;
    g_bus_width = 1;
    g_bus_khz = sdmmc_set_bus_clock(400);

    /* CMD0 with SEND_INITIALIZATION, which is the controller sending the 80
     * clocks the spec requires before the first command rather than this
     * driver bit-banging them. */
    if (sd_command(0, 0, RESP_NONE, CMD_SEND_INIT, NULL, &status) != 0) {
        printk("[SDMMC] CMD0 was not accepted (status 0x%x)\n", (unsigned)status);
        return -1;
    }

    /* CMD8: does the card understand SD 2.0's argument, and does it echo the
     * check pattern back? A timeout here is not an error -- it is how an SDv1
     * card answers -- so it is not reported as one. */
    bool v2 = false;
    if (sd_command(8, 0x1AAu, RESP_R1, 0, resp, &status) == 0 &&
        (resp[0] & 0xffu) == 0xAAu) {
        v2 = true;
    }

    /* ACMD41 until the card leaves idle. Bounded by the clock, not by a retry
     * count, for the reason drivers/spisd_rp2350.c gives at length: the SD
     * spec's budget is one second, iterations are not seconds, and the
     * substitution silently changes meaning when the clock moves. */
    uint32_t ocr = 0;
    uint64_t deadline = time_get_ms() + 1500u;
    unsigned tries = 0;
    for (;;) {
        uint32_t arg = 0x00FF8000u | (v2 ? 0x40000000u : 0u);  /* 2.7-3.6 V, HCS */
        if (sd_acommand(41, arg, RESP_R3, resp, &status) != 0) {
            if (tries == 0) {
                /* The ordinary empty-slot path. Not an error line: `sdinfo`
                 * and the mount message both say "no card" perfectly well. */
                return -1;
            }
            printk("[SDMMC] ACMD41 stopped answering after %u tries (status 0x%x)\n",
                   tries, (unsigned)status);
            return -1;
        }
        tries++;
        ocr = resp[0];
        if (ocr & 0x80000000u) break;        /* card is no longer busy */
        if (time_get_ms() > deadline) {
            printk("[SDMMC] ACMD41 did not leave idle within 1500 ms "
                   "(OCR 0x%x, %u tries, card is %s)\n",
                   (unsigned)ocr, tries, v2 ? "SDv2/SDHC" : "SDv1");
            return -1;
        }
    }
    g_block_addressed = v2 && (ocr & 0x40000000u) != 0u;   /* OCR's CCS bit */

    /* CMD2: the CID, which is what `sdinfo` prints. CMD3: the card publishes
     * an address for everything after this point to use. */
    if (sd_command(2, 0, RESP_R2, 0, g_cid, &status) != 0) {
        printk("[SDMMC] CMD2 (CID) failed (status 0x%x)\n", (unsigned)status);
        return -1;
    }
    if (sd_command(3, 0, RESP_R1, 0, resp, &status) != 0) {
        printk("[SDMMC] CMD3 (publish RCA) failed (status 0x%x)\n", (unsigned)status);
        return -1;
    }
    g_rca = resp[0] >> 16;

    /* CMD9: the CSD, and with it the real capacity. Must be asked while the
     * card is still deselected -- CMD9 is a stand-by-state command. */
    if (sd_command(9, g_rca << 16, RESP_R2, 0, resp, &status) != 0) {
        printk("[SDMMC] CMD9 (CSD) failed (status 0x%x)\n", (unsigned)status);
        return -1;
    }
    g_blocks = sd_csd_blocks(resp);
    if (g_blocks == 0) {
        printk("[SDMMC] CSD structure %u not understood; capacity unknown\n",
               (unsigned)sd_bits(resp, 126, 2));
        return -1;
    }

    /* CMD7: select it. R1b -- the card may hold the bus while it switches. */
    if (sd_command(7, g_rca << 16, RESP_R1B, 0, NULL, &status) != 0) {
        printk("[SDMMC] CMD7 (select) failed (status 0x%x)\n", (unsigned)status);
        return -1;
    }

    /* CMD16: 512-byte blocks. A block-addressed card is fixed at 512 already
     * and ignores this; a byte-addressed one needs it, and sending it to both
     * is one less state to reason about than sending it to one. */
    if (sd_command(16, 512, RESP_R1, 0, NULL, &status) != 0) {
        printk("[SDMMC] CMD16 (512-byte blocks) failed (status 0x%x)\n", (unsigned)status);
        return -1;
    }

    sdmmc_set_bus_width();

    g_present = true;
    printk("[SDMMC] MicroSD %u MB (%s, %u-bit, %u kHz) on SDMMC slot 0 "
           "(CLK=GPIO%d CMD=GPIO%d D0-D3=GPIO%d-%d)\n",
           (unsigned)(g_blocks / 2048u),
           g_block_addressed ? "SDHC/SDXC block-addressed" : "SD byte-addressed",
           (unsigned)g_bus_width, (unsigned)g_bus_khz,
           CONFIG_SDMMC_CLK_GPIO, CONFIG_SDMMC_CMD_GPIO,
           CONFIG_SDMMC_D0_GPIO, CONFIG_SDMMC_D3_GPIO);
    return 0;
}

/* --- The "sdblk" driver task --------------------------------------------
 *
 * The same endpoint name, wire protocol and four-block batch as
 * drivers/spisd_rp2350.c's and drivers/virtio_blk.c's, deliberately: /proc,
 * `blkstats` and the hardware tests all ask the same questions of whichever
 * of the three a board happens to build, and a third dialect would mean a
 * third case in each of them.
 *
 * Kernel-mode, not U-mode, and that is the one difference worth stating.
 * spisd's task drops to U-mode under a PMP domain covering SIO and SPI1;
 * this one does not, because on this board nothing else does yet -- the P4
 * has no .utext region wired up and no driver running under a domain, so a
 * single driver claiming isolation here would be claiming something no test
 * on this board can check. drivers/driver_task.h's own warning applies:
 * real isolation or a kernel-mode task, and it is a choice. */
#define BLK_REQ_READ  ((uint8_t)'R')
#define BLK_REQ_WRITE ((uint8_t)'W')
#define BLK_MAX_COUNT 4u
#define BLK_HDR_LEN   9u /* opcode + lba(4) + count(4) */
#define BLK_REQ_CAP   (BLK_HDR_LEN + BLK_MAX_COUNT * 512u)
#define BLK_RESP_CAP  (1u + BLK_MAX_COUNT * 512u)

static uint8_t       g_blk_req[BLK_REQ_CAP];
static uint8_t       g_blk_resp[BLK_RESP_CAP];
static driver_task_t g_blk_task;

uint32_t blk_task_call_count(void) { return driver_task_call_count(&g_blk_task); }

static bool blk_task_alive(void) { return driver_task_alive(&g_blk_task); }

static uint32_t be32_load(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void be32_store(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int sdmmc_hw_read_blocks(uint32_t lba, uint32_t count, uint8_t *dst) {
    if (!g_present) return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (sdmmc_hw_read_block(lba + i, dst + (size_t)i * 512u) != 0) return -1;
    }
    return 0;
}

static int sdmmc_hw_write_blocks(uint32_t lba, uint32_t count, const uint8_t *src) {
    if (!g_present) return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (sdmmc_hw_write_block(lba + i, src + (size_t)i * 512u) != 0) return -1;
    }
    return 0;
}

/* This callback, and only this callback, touches the controller while the
 * task is alive. drivers/driver_task.h's three invariants are what it runs
 * under; the framework owns the loop, and what is left here is the wire
 * protocol, which is this driver's. */
static uint32_t blk_serve(void *ctx, const uint8_t *req, uint32_t req_len,
                          uint8_t *resp, uint32_t resp_cap) {
    (void)ctx; (void)resp_cap;

    uint8_t  op    = req[0];
    uint32_t lba   = be32_load(&req[1]);
    uint32_t count = be32_load(&req[5]);

    if (op == BLK_REQ_READ && count >= 1 && count <= BLK_MAX_COUNT) {
        int rc = sdmmc_hw_read_blocks(lba, count, &resp[1]);
        resp[0] = (rc == 0) ? 0 : 1;
        return (rc == 0) ? (1u + count * 512u) : 1u;
    }
    if (op == BLK_REQ_WRITE && count >= 1 && count <= BLK_MAX_COUNT &&
        req_len >= BLK_HDR_LEN + count * 512u) {
        int rc = sdmmc_hw_write_blocks(lba, count, &req[BLK_HDR_LEN]);
        resp[0] = (rc == 0) ? 0 : 1;
        return 1;
    }
    resp[0] = 1;
    return 1;
}

int sdmmc_task_start(void) {
    if (!g_present) return -1;         /* nothing to serve */
    const driver_task_spec_t spec = {
        .name        = "sdblk",
        .serve       = blk_serve,
        .ctx         = NULL,
        .req         = g_blk_req,  .req_cap  = sizeof(g_blk_req),
        .resp        = g_blk_resp, .resp_cap = sizeof(g_blk_resp),
        .min_req_len = BLK_HDR_LEN,
        .stack_pages = 1,
        .priority    = DRIVER_PRIO_DEFAULT,
    };
    return driver_task_start(&g_blk_task, &spec);
}

/* One batched request through the endpoint. Returns the blocks transferred,
 * or -1 if the channel itself failed -- which the caller answers by falling
 * back to direct access rather than by propagating a failure that might be
 * about the IPC and not about the card. Same split, and the same reasoning,
 * as spisd's and virtio_blk's. */
static int blk_read_chunk(uint32_t lba, uint32_t count, uint8_t *dst) {
    uint8_t req[BLK_HDR_LEN];
    req[0] = BLK_REQ_READ;
    be32_store(&req[1], lba);
    be32_store(&req[5], count);
    uint8_t resp[BLK_RESP_CAP];
    int n = driver_task_call(&g_blk_task, req, sizeof(req), resp, sizeof(resp));
    if (n >= 1 && resp[0] == 0 && (uint32_t)n >= 1u + count * 512u) {
        memcpy(dst, &resp[1], count * 512u);
        return (int)count;
    }
    return -1;
}

static int blk_write_chunk(uint32_t lba, uint32_t count, const uint8_t *src) {
    uint8_t req[BLK_REQ_CAP];
    req[0] = BLK_REQ_WRITE;
    be32_store(&req[1], lba);
    be32_store(&req[5], count);
    memcpy(&req[BLK_HDR_LEN], src, count * 512u);
    uint8_t resp[1];
    int n = driver_task_call(&g_blk_task, req, BLK_HDR_LEN + count * 512u,
                             resp, sizeof(resp));
    if (n >= 1 && resp[0] == 0) return (int)count;
    return -1;
}

static int sdmmc_read_blocks(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    if (!buf || count == 0 || lba + count < lba || lba + count > dev->num_blocks) return -1;

    uint32_t done = 0;
    uint8_t *dst = (uint8_t *)buf;
    if (blk_task_alive()) {
        while (done < count) {
            uint32_t chunk = (count - done) > BLK_MAX_COUNT ? BLK_MAX_COUNT : (count - done);
            int n = blk_read_chunk(lba + done, chunk, dst + (size_t)done * 512u);
            if (n < 0) break;
            done += (uint32_t)n;
        }
        if (done >= count) return 0;
    }
    return sdmmc_hw_read_blocks(lba + done, count - done, dst + (size_t)done * 512u);
}

static int sdmmc_write_blocks(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    if (!buf || count == 0 || lba + count < lba || lba + count > dev->num_blocks) return -1;

    uint32_t done = 0;
    const uint8_t *src = (const uint8_t *)buf;
    if (blk_task_alive()) {
        while (done < count) {
            uint32_t chunk = (count - done) > BLK_MAX_COUNT ? BLK_MAX_COUNT : (count - done);
            int n = blk_write_chunk(lba + done, chunk, src + (size_t)done * 512u);
            if (n < 0) break;
            done += (uint32_t)n;
        }
        if (done >= count) return 0;
    }
    return sdmmc_hw_write_blocks(lba + done, count - done, src + (size_t)done * 512u);
}

static block_dev_t g_sdmmc_dev = {
    .name = "sdmmc0",
    .block_size = 512,
    .num_blocks = 0,          /* the CSD's number, filled in by the probe */
    .read_blocks = sdmmc_read_blocks,
    .write_blocks = sdmmc_write_blocks,
};

block_dev_t *sdmmc_get_device(void) {
    if (!g_present) {
        if (sdmmc_card_init() != 0) return NULL;
        g_sdmmc_dev.num_blocks = g_blocks;
    }
    return &g_sdmmc_dev;
}

/* --- `sdinfo` and `sdbench` ---------------------------------------------- */

/* Sequential read throughput, measured on the board's own clock.
 *
 * A rate without a sample size is not a measurement, so this reads a whole
 * megabyte by default rather than timing one block, and it prints the count it
 * actually achieved. Read-only and bounded: it walks from LBA 0, which on a
 * mounted volume is the filesystem's own metadata, and writes nothing.
 *
 * It goes through block_dev_t rather than sdmmc_hw_read_block(), deliberately:
 * what matters is the rate a filesystem sees, which includes the sdblk task's
 * IPC and its four-block batching, not the rate the bus could manage with
 * nothing in the way. */
void sdmmc_bench_report(uint32_t kilobytes) {
    block_dev_t *dev = sdmmc_get_device();
    if (!dev) {
        cprintf("sdbench: no card\n");
        return;
    }
    if (kilobytes == 0 || kilobytes > 8192u) kilobytes = 1024u;

    uint32_t blocks = kilobytes * 2u;
    if (blocks > dev->num_blocks) blocks = dev->num_blocks;

    static uint8_t buf[512 * 4];
    uint32_t done = 0;
    uint64_t t0 = time_get_us();
    while (done < blocks) {
        uint32_t chunk = (blocks - done) > 4u ? 4u : (blocks - done);
        if (dev->read_blocks(dev, buf, done, chunk) != 0) {
            cprintf("sdbench: read failed at lba %u\n", (unsigned)done);
            return;
        }
        done += chunk;
    }
    uint64_t us = time_get_us() - t0;
    if (us == 0) us = 1;

    uint32_t kb = done / 2u;
    uint32_t kb_per_s = (uint32_t)(((uint64_t)kb * 1000000u) / us);
    cprintf("sdbench: %u KB in %u ms = %u KB/s (%u-bit at %u kHz, %u blocks, "
            "4 per request)\n",
            (unsigned)kb, (unsigned)(us / 1000u), (unsigned)kb_per_s,
            (unsigned)g_bus_width, (unsigned)g_bus_khz, (unsigned)done);
}



void sdmmc_info_report(void) {
    if (!g_present) {
        /* Probe rather than report a stale "no card": the slot may have been
         * filled since boot, and a command that answers "no" without looking
         * is worth less than the second it takes to look. */
        (void)sdmmc_get_device();
    }
    if (!g_present) {
        cprintf("SDMMC slot 0: no card (CLK=GPIO%d CMD=GPIO%d D0-D3=GPIO%d-%d, "
                "rail VO4 %s)\n",
                CONFIG_SDMMC_CLK_GPIO, CONFIG_SDMMC_CMD_GPIO,
                CONFIG_SDMMC_D0_GPIO, CONFIG_SDMMC_D3_GPIO,
                (REG(PMU_EXT_LDO_VO4) & LDO_XPD) ? "on" : "OFF");
        return;
    }

    char pnm[6];
    for (int i = 0; i < 5; i++) {
        pnm[i] = (char)sd_bits(g_cid, (uint32_t)(96 - 8 * i), 8);
        if (pnm[i] < 0x20 || pnm[i] > 0x7e) pnm[i] = '?';
    }
    pnm[5] = '\0';

    uint32_t mdt = sd_bits(g_cid, 8, 12);
    cprintf("SDMMC slot 0: %s, %u MB (%u blocks of 512 B)\n",
            g_block_addressed ? "SDHC/SDXC" : "SD", (unsigned)(g_blocks / 2048u),
            (unsigned)g_blocks);
    cprintf("  card   MID 0x%02x OID '%c%c' name '%s' rev %u.%u serial 0x%08x %u/%u\n",
            (unsigned)sd_bits(g_cid, 120, 8),
            (char)sd_bits(g_cid, 112, 8), (char)sd_bits(g_cid, 104, 8), pnm,
            (unsigned)sd_bits(g_cid, 60, 4), (unsigned)sd_bits(g_cid, 56, 4),
            (unsigned)sd_bits(g_cid, 24, 32),
            (unsigned)(mdt & 0xfu), (unsigned)(2000u + (mdt >> 4)));
    cprintf("  bus    %u-bit at %u kHz, RCA 0x%04x, addressing %s\n",
            (unsigned)g_bus_width, (unsigned)g_bus_khz, (unsigned)g_rca,
            g_block_addressed ? "block" : "byte");
    cprintf("  host   CDETECT 0x%x WRTPRT 0x%x STATUS 0x%x, sdblk task %s "
            "(%u calls)\n",
            (unsigned)REG(SD_CDETECT), (unsigned)REG(SD_WRTPRT),
            (unsigned)REG(SD_STATUS),
            blk_task_alive() ? "serving" : "not running",
            (unsigned)blk_task_call_count());

    /* Block 0, re-read now, and the reason this is in a status command rather
     * than in a debugging patch somebody deleted afterwards: "the card
     * answered but /sd0 did not mount" has two very different causes -- a
     * card with no filesystem on it, and a data path returning the wrong
     * bytes -- and they are indistinguishable from every other line above.
     * Sixteen bytes and the signature separate them: a blank card reads as
     * zeros or 0xff with no 55 aa, and a broken path reads as neither. */
    if (sdmmc_hw_read_block(0, g_probe_block) != 0) {
        cprintf("  block0 UNREADABLE\n");
        return;
    }
    cprintf("  block0 ");
    for (int i = 0; i < 16; i++) cprintf("%02x ", g_probe_block[i]);
    cprintf("... sig %02x %02x %s\n",
            g_probe_block[510], g_probe_block[511],
            (g_probe_block[510] == 0x55 && g_probe_block[511] == 0xAA)
                ? "(MBR/VBR)" : "(no boot signature)");

}

#else /* !CONFIG_ENABLE_SDMMC */

block_dev_t *sdmmc_get_device(void) { return NULL; }
int sdmmc_task_start(void) { return -1; }
uint32_t blk_task_call_count(void) { return 0; }
void sdmmc_info_report(void) {
    cprintf("SDMMC: built out of this image (LUGALOS_ENABLE_SDMMC=OFF)\n");
}
void sdmmc_bench_report(uint32_t kilobytes) { (void)kilobytes; sdmmc_info_report(); }

#endif /* CONFIG_ENABLE_SDMMC */
