/* ESP32-P4 Pixel Processing Accelerator (PPA) Scale-Rotate-Mirror (SRM) driver
 * and 2D-DMA channel 0 (TX + RX) interface.
 *
 * Drives the PPA SRM hardware block (0x50087000) coupled to 2D-DMA channel 0
 * (0x50088000) in descriptor-port (`dscr_port`) mode to offload 2D image
 * scaling and 0/90/180/270-degree rotation of RGB565 framebuffers in PSRAM.
 *
 * Concurrency & Cache Coherency:
 * - All hardware state and the two non-cacheable L2MEM DMA2D descriptors are
 *   serialized under `g_ppa_lock`.
 * - Descriptors reside in 64-byte aligned internal SRAM and are written through
 *   the non-cached alias (`SRAM_NONCACHE_OFFSET`), with their initial cached
 *   lines flushed and invalidated once during `ppa_init()`.
 * - Source and destination PSRAM windows are synchronized through
 *   `esp32p4_extmem_writeback()` / `esp32p4_extmem_invalidate()`. */

#include "drivers/ppa_esp32p4.h"
#include "lugalos_config.h"

#if defined(CONFIG_BOARD_ESP32P4)

#include "drivers/esp32p4_ppa_regs.h"
#include "arch/esp32p4_intr.h"
#include "kernel/lock.h"
#include "kernel/sysreg.h"
#include "kernel/time.h"

#include <stddef.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

static inline void field_set(uintptr_t reg, unsigned s, uint32_t v_mask, uint32_t val) {
    if (sysreg_is_shared(reg)) {
        sysreg_update(reg, v_mask << s, (val & v_mask) << s);
        return;
    }
    uint32_t x = REG(reg);
    x &= ~(v_mask << s);
    x |= (val & v_mask) << s;
    REG(reg) = x;
}
#define FSET(reg, f, val) field_set((reg), f##_S, f##_V, (uint32_t)(val))
#define FGET(reg, f)      ((REG(reg) >> f##_S) & f##_V)

#define SRAM_NONCACHE_OFFSET             0x40000000u
#define DMA2D_MAX_2D_DIM                 16383u
#define DMA2D_PBYTE_RGB565               3u  /* 2.0 bytes/pixel */
#define DMA2D_PERI_SEL_PPA_SRM           1u  /* SOC_DMA2D_TRIG_PERIPH_PPA_SRM_TX/RX */
#define DMA2D_BURST_128B                 4u  /* 128-byte AXI burst */
#define DMA2D_MACRO_BLOCK_NONE           3u
#define PPA_SRM_CM_RGB565                2u

/* ESP32-P4 rev v3.2 defaults to 32x32 macroblocks (dscr_port block 34x34 for RGB565);
 * rev v1.3 has fixed 16x16 macroblocks (dscr_port block 18x18 for RGB565). */
#if defined(CONFIG_ESP32P4_REV) && (CONFIG_ESP32P4_REV >= 300)
#define PPA_MB_H                         32u
#define PPA_PORT_BLK                     34u
#else
#define PPA_MB_H                         16u
#define PPA_PORT_BLK                     18u
#endif

/* 2D-DMA 24-byte hardware descriptor padded to a 64-byte L1D cache line so no
 * adjacent variable shares the cache line accessed via SRAM_NONCACHE_OFFSET. */
typedef struct {
    uint32_t vb_size    : 14;
    uint32_t hb_length  : 14;
    uint32_t err_eof    : 1;
    uint32_t dma2d_en   : 1;
    uint32_t suc_eof    : 1;
    uint32_t owner      : 1;
    uint32_t va_size    : 14;
    uint32_t ha_length  : 14;
    uint32_t pbyte      : 4;
    uint32_t y          : 14;
    uint32_t x          : 14;
    uint32_t mode       : 1;
    uint32_t reserved29 : 3;
    uint32_t buffer;
    uint32_t next;
    uint32_t pad[11];
} dma2d_dscr_t;
_Static_assert(sizeof(dma2d_dscr_t) == 64, "dma2d_dscr_t must be 64-byte cache-line sized");

/* guarded by: g_ppa_lock */
static ylock_t      g_ppa_lock;
static bool         g_ppa_ready;
static dma2d_dscr_t g_tx_dscr_mem __attribute__((aligned(64)));
static dma2d_dscr_t g_rx_dscr_mem __attribute__((aligned(64)));
static uint32_t     g_ppa_ops;
static uint32_t     g_ppa_errs;
static uint32_t     g_ppa_last_us;

#define TX_DSCR_NC ((volatile dma2d_dscr_t *)((uintptr_t)&g_tx_dscr_mem + SRAM_NONCACHE_OFFSET))
#define RX_DSCR_NC ((volatile dma2d_dscr_t *)((uintptr_t)&g_rx_dscr_mem + SRAM_NONCACHE_OFFSET))

static void ppa_init_locked(void) {
    if (g_ppa_ready) return;

    /* Evict and invalidate the L1D cache lines of the descriptor storage before
     * touching them exclusively through the non-cacheable SRAM alias. */
    esp32p4_dcache_writeback((uintptr_t)&g_tx_dscr_mem, sizeof g_tx_dscr_mem);
    esp32p4_dcache_invalidate((uintptr_t)&g_tx_dscr_mem, sizeof g_tx_dscr_mem);
    esp32p4_dcache_writeback((uintptr_t)&g_rx_dscr_mem, sizeof g_rx_dscr_mem);
    esp32p4_dcache_invalidate((uintptr_t)&g_rx_dscr_mem, sizeof g_rx_dscr_mem);

    /* Enable 2D-DMA and PPA system clocks and pulse their module resets. */
    FSET(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_DMA2D_SYS_CLK_EN, 1);
    FSET(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_PPA_SYS_CLK_EN, 1);

    FSET(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_DMA2D, 1);
    FSET(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_DMA2D, 0);

    FSET(HP_SYS_CLKRST_HP_RST_EN1_REG, HP_SYS_CLKRST_REG_RST_EN_PPA, 1);
    FSET(HP_SYS_CLKRST_HP_RST_EN1_REG, HP_SYS_CLKRST_REG_RST_EN_PPA, 0);

    /* Enable internal register clocks and pulse AXI master read/write FIFO resets. */
    FSET(DMA2D_RST_CONF_REG, DMA2D_CLK_EN, 1);
    FSET(DMA2D_RST_CONF_REG, DMA2D_AXIM_RD_RST, 1);
    FSET(DMA2D_RST_CONF_REG, DMA2D_AXIM_RD_RST, 0);
    FSET(DMA2D_RST_CONF_REG, DMA2D_AXIM_WR_RST, 1);
    FSET(DMA2D_RST_CONF_REG, DMA2D_AXIM_WR_RST, 0);

    FSET(PPA_REG_CONF_REG, PPA_CLK_EN, 1);

    g_ppa_ready = true;
}

bool ppa_init(void) {
    ylock_acquire(&g_ppa_lock);
    ppa_init_locked();
    ylock_release(&g_ppa_lock);
    return true;
}

bool ppa_srm_rgb565(const uint16_t *src, uint32_t src_pic_w, uint32_t src_pic_h,
                    uint32_t src_x, uint32_t src_y, uint32_t blk_w, uint32_t blk_h,
                    uint16_t *dst, uint32_t dst_pic_w, uint32_t dst_pic_h,
                    uint32_t dst_x, uint32_t dst_y,
                    uint32_t scale_x_int, uint32_t scale_x_frag,
                    uint32_t scale_y_int, uint32_t scale_y_frag,
                    unsigned rot_ccw_deg, bool flush_src, bool inv_dst) {
    if (!src || !dst || blk_w == 0 || blk_h == 0) return false;
    if (src_pic_w > DMA2D_MAX_2D_DIM || src_pic_h > DMA2D_MAX_2D_DIM ||
        dst_pic_w > DMA2D_MAX_2D_DIM || dst_pic_h > DMA2D_MAX_2D_DIM) return false;
    if (src_x >= src_pic_w || blk_w > src_pic_w - src_x ||
        src_y >= src_pic_h || blk_h > src_pic_h - src_y) return false;

    scale_x_frag &= 0xfu;
    scale_y_frag &= 0xfu;
    if (scale_x_int == 0 && scale_x_frag == 0) return false;
    if (scale_y_int == 0 && scale_y_frag == 0) return false;

    uint32_t rot_code;
    switch (rot_ccw_deg) {
    case 0:   rot_code = 0u; break;
    case 90:  rot_code = 1u; break;
    case 180: rot_code = 2u; break;
    case 270: rot_code = 3u; break;
    default:  return false;
    }

    uint32_t scaled_w = blk_w * scale_x_int + (blk_w * scale_x_frag) / 16u;
    uint32_t scaled_h = blk_h * scale_y_int + (blk_h * scale_y_frag) / 16u;
    uint32_t out_blk_w = (rot_code == 0u || rot_code == 2u) ? scaled_w : scaled_h;
    uint32_t out_blk_h = (rot_code == 0u || rot_code == 2u) ? scaled_h : scaled_w;
    if (out_blk_w == 0 || out_blk_h == 0) return false;
    if (dst_x >= dst_pic_w || out_blk_w > dst_pic_w - dst_x ||
        dst_y >= dst_pic_h || out_blk_h > dst_pic_h - dst_y) return false;

    /* Synchronize PSRAM caches outside the spinlock so interrupts remain enabled
     * during potentially large cache range operations. */
    if (flush_src) {
        uintptr_t in_win = (uintptr_t)src + (uintptr_t)src_y * src_pic_w * sizeof(uint16_t);
        uint32_t in_len  = blk_h * src_pic_w * sizeof(uint16_t);
        esp32p4_extmem_writeback(in_win, in_len);
    }
    if (inv_dst) {
        uintptr_t out_win = (uintptr_t)dst + (uintptr_t)dst_y * dst_pic_w * sizeof(uint16_t);
        uint32_t out_len  = out_blk_h * dst_pic_w * sizeof(uint16_t);
        esp32p4_extmem_invalidate(out_win, out_len);
    }

    uint64_t t0 = time_get_us();
    ylock_acquire(&g_ppa_lock);
    ppa_init_locked();

    /* 1. Reset PPA SRM engine */
    FSET(PPA_SR_SCAL_ROTATE_REG, PPA_SCAL_ROTATE_RST, 1);
    FSET(PPA_SR_SCAL_ROTATE_REG, PPA_SCAL_ROTATE_RST, 0);

    /* 2. Stop and reset DMA2D RX channel 0 and TX channel 0 */
    FSET(DMA2D_IN_LINK_CONF_CH0_REG, DMA2D_INLINK_STOP_CH0, 1);
    FSET(DMA2D_IN_CONF0_CH0_REG, DMA2D_IN_CMD_DISABLE_CH0, 1);
    (void)time_poll_until_us(FGET(DMA2D_IN_STATE_CH0_REG, DMA2D_IN_RESET_AVAIL_CH0) != 0,
                             time_get_us() + 1000u);
    FSET(DMA2D_IN_CONF0_CH0_REG, DMA2D_IN_RST_CH0, 1);
    FSET(DMA2D_IN_CONF0_CH0_REG, DMA2D_IN_RST_CH0, 0);
    FSET(DMA2D_IN_CONF0_CH0_REG, DMA2D_IN_CMD_DISABLE_CH0, 0);

    FSET(DMA2D_OUT_LINK_CONF_CH0_REG, DMA2D_OUTLINK_STOP_CH0, 1);
    FSET(DMA2D_OUT_CONF0_CH0_REG, DMA2D_OUT_CMD_DISABLE_CH0, 1);
    (void)time_poll_until_us(FGET(DMA2D_OUT_STATE_CH0_REG, DMA2D_OUT_RESET_AVAIL_CH0) != 0,
                             time_get_us() + 1000u);
    FSET(DMA2D_OUT_CONF0_CH0_REG, DMA2D_OUT_RST_CH0, 1);
    FSET(DMA2D_OUT_CONF0_CH0_REG, DMA2D_OUT_RST_CH0, 0);
    FSET(DMA2D_OUT_CONF0_CH0_REG, DMA2D_OUT_CMD_DISABLE_CH0, 0);

    /* 3. Populate non-cacheable TX and RX 2D-DMA descriptors */
    volatile dma2d_dscr_t *tx = TX_DSCR_NC;
    tx->vb_size    = blk_h;
    tx->hb_length  = blk_w;
    tx->err_eof    = 0;
    tx->dma2d_en   = 1;
    tx->suc_eof    = 1;
    tx->owner      = 1;
    tx->va_size    = src_pic_h;
    tx->ha_length  = src_pic_w;
    tx->pbyte      = DMA2D_PBYTE_RGB565;
    tx->y          = src_y;
    tx->x          = src_x;
    tx->mode       = 0;
    tx->reserved29 = 0;
    tx->buffer     = (uint32_t)(uintptr_t)src;
    tx->next       = 0;

    volatile dma2d_dscr_t *rx = RX_DSCR_NC;
    rx->vb_size    = 2; /* Non-zero even placeholder required in dscr_port mode */
    rx->hb_length  = 2;
    rx->err_eof    = 0;
    rx->dma2d_en   = 1;
    rx->suc_eof    = 1;
    rx->owner      = 1;
    rx->va_size    = dst_pic_h;
    rx->ha_length  = dst_pic_w;
    rx->pbyte      = DMA2D_PBYTE_RGB565;
    rx->y          = dst_y;
    rx->x          = dst_x;
    rx->mode       = 0;
    rx->reserved29 = 0;
    rx->buffer     = (uint32_t)(uintptr_t)dst;
    rx->next       = 0;

    /* 4. Configure DMA2D RX channel 0 and TX channel 0 for PPA SRM dscr_port mode */
    FSET(DMA2D_IN_PERI_SEL_CH0_REG, DMA2D_IN_PERI_SEL_CH0, DMA2D_PERI_SEL_PPA_SRM);
    REG(DMA2D_IN_COLOR_CONVERT_CH0_REG) = (7u << DMA2D_IN_COLOR_INPUT_SEL_CH0_S) |
                                          (1u << DMA2D_IN_COLOR_OUTPUT_SEL_CH0_S);
    REG(DMA2D_IN_SCRAMBLE_CH0_REG) = 0;
    REG(DMA2D_IN_CONF0_CH0_REG) = (1u << DMA2D_INDSCR_BURST_EN_CH0_S) |
                                  (DMA2D_BURST_128B << DMA2D_IN_MEM_BURST_LENGTH_CH0_S) |
                                  (DMA2D_MACRO_BLOCK_NONE << DMA2D_IN_MACRO_BLOCK_SIZE_CH0_S) |
                                  (1u << DMA2D_IN_DSCR_PORT_EN_CH0_S) |
                                  (1u << DMA2D_IN_PAGE_BOUND_EN_CH0_S);

    FSET(DMA2D_OUT_PERI_SEL_CH0_REG, DMA2D_OUT_PERI_SEL_CH0, DMA2D_PERI_SEL_PPA_SRM);
    REG(DMA2D_OUT_COLOR_CONVERT_CH0_REG) = (7u << DMA2D_OUT_COLOR_INPUT_SEL_CH0_S) |
                                           (2u << DMA2D_OUT_COLOR_OUTPUT_SEL_CH0_S);
    REG(DMA2D_OUT_SCRAMBLE_CH0_REG) = 0;
    REG(DMA2D_OUT_CONF0_CH0_REG) = (1u << DMA2D_OUT_EOF_MODE_CH0_S) |
                                   (1u << DMA2D_OUTDSCR_BURST_EN_CH0_S) |
                                   (DMA2D_BURST_128B << DMA2D_OUT_MEM_BURST_LENGTH_CH0_S) |
                                   (DMA2D_MACRO_BLOCK_NONE << DMA2D_OUT_MACRO_BLOCK_SIZE_CH0_S) |
                                   (1u << DMA2D_OUT_DSCR_PORT_EN_CH0_S) |
                                   (1u << DMA2D_OUT_PAGE_BOUND_EN_CH0_S);
    REG(DMA2D_OUT_DSCR_PORT_BLK_CH0_REG) = (PPA_PORT_BLK << DMA2D_OUT_DSCR_PORT_BLK_V_CH0_S) |
                                           (PPA_PORT_BLK << DMA2D_OUT_DSCR_PORT_BLK_H_CH0_S);

    /* 5. Configure PPA SRM color mode, alpha bypass, and DIG-734 macroblock reorder workaround */
    REG(PPA_SR_COLOR_MODE_REG) = (PPA_SRM_CM_RGB565 << PPA_SR_TX_CM_S) |
                                 (PPA_SRM_CM_RGB565 << PPA_SR_RX_CM_S);
    REG(PPA_SR_FIX_ALPHA_REG) = 0x80u;

    uint32_t w_left = (scaled_w % 64u == 0u) ? 64u : (scaled_w % 64u);
    uint32_t h_in_left = (blk_h % PPA_MB_H == 0u) ? PPA_MB_H : (blk_h % PPA_MB_H);
    uint32_t h_left = h_in_left * scale_y_int + (h_in_left * scale_y_frag) / 16u;
    uint32_t bypass_mb = (((scaled_w > 64u) || (blk_h > PPA_MB_H)) &&
                          ((w_left * h_left * 16u) < (12u * 128u))) ? 1u : 0u;
    REG(PPA_SR_BYTE_ORDER_REG) = (bypass_mb << PPA_SR_MACRO_BK_RO_BYPASS_S);

    /* 6. Clear status flags, arm DMA2D RX/TX channels, and trigger PPA SRM */
    REG(DMA2D_IN_INT_CLR_CH0_REG) = 0x3fffu;
    REG(DMA2D_OUT_INT_CLR_CH0_REG) = 0x1fffu;
    REG(PPA_INT_CLR_REG) = 0xfu;

    __asm__ volatile ("fence" ::: "memory");

    REG(DMA2D_IN_LINK_ADDR_CH0_REG) = (uint32_t)(uintptr_t)&g_rx_dscr_mem;
    REG(DMA2D_OUT_LINK_ADDR_CH0_REG) = (uint32_t)(uintptr_t)&g_tx_dscr_mem;
    FSET(DMA2D_IN_LINK_CONF_CH0_REG, DMA2D_INLINK_START_CH0, 1);
    FSET(DMA2D_OUT_LINK_CONF_CH0_REG, DMA2D_OUTLINK_START_CH0, 1);

    uint32_t sr_cfg =
        ((scale_x_int & PPA_SR_SCAL_X_INT_V) << PPA_SR_SCAL_X_INT_S) |
        ((scale_x_frag & PPA_SR_SCAL_X_FRAG_V) << PPA_SR_SCAL_X_FRAG_S) |
        ((scale_y_int & PPA_SR_SCAL_Y_INT_V) << PPA_SR_SCAL_Y_INT_S) |
        ((scale_y_frag & PPA_SR_SCAL_Y_FRAG_V) << PPA_SR_SCAL_Y_FRAG_S) |
        ((rot_code & PPA_SR_ROTATE_ANGLE_V) << PPA_SR_ROTATE_ANGLE_S);
    REG(PPA_SR_SCAL_ROTATE_REG) = sr_cfg;
    REG(PPA_SR_SCAL_ROTATE_REG) = sr_cfg | (1u << PPA_SCAL_ROTATE_START_S);

    /* 7. Wait for DMA2D RX SUC_EOF or any DMA/PPA error bit (100 ms timeout) */
    const uint32_t rx_done_mask = (1u << DMA2D_IN_SUC_EOF_CH0_INT_RAW_S) |
                                  (1u << DMA2D_IN_ERR_EOF_CH0_INT_RAW_S) |
                                  (1u << DMA2D_IN_DSCR_ERR_CH0_INT_RAW_S);
    const uint32_t tx_err_mask  = (1u << DMA2D_OUT_DSCR_ERR_CH0_INT_RAW_S);
    const uint32_t ppa_err_mask = (1u << PPA_SR_PARAM_CFG_ERR_INT_RAW_S);

    bool finished = time_poll_until_us(
        ((REG(DMA2D_IN_INT_RAW_CH0_REG) & rx_done_mask) != 0u) ||
        ((REG(DMA2D_OUT_INT_RAW_CH0_REG) & tx_err_mask) != 0u) ||
        ((REG(PPA_INT_RAW_REG) & ppa_err_mask) != 0u),
        time_get_us() + 100000u);

    uint32_t rx_raw  = REG(DMA2D_IN_INT_RAW_CH0_REG);
    uint32_t tx_raw  = REG(DMA2D_OUT_INT_RAW_CH0_REG);
    uint32_t ppa_raw = REG(PPA_INT_RAW_REG);

    bool ok = finished &&
              ((rx_raw & (1u << DMA2D_IN_SUC_EOF_CH0_INT_RAW_S)) != 0u) &&
              ((rx_raw & ((1u << DMA2D_IN_ERR_EOF_CH0_INT_RAW_S) | (1u << DMA2D_IN_DSCR_ERR_CH0_INT_RAW_S))) == 0u) &&
              ((tx_raw & tx_err_mask) == 0u) &&
              ((ppa_raw & ppa_err_mask) == 0u);

    g_ppa_ops++;
    if (!ok) g_ppa_errs++;
    g_ppa_last_us = (uint32_t)(time_get_us() - t0);

    ylock_release(&g_ppa_lock);
    return ok;
}

void ppa_get_stats(uint32_t *out_ops, uint32_t *out_errs, uint32_t *out_last_us) {
    ylock_acquire(&g_ppa_lock);
    if (out_ops)     *out_ops     = g_ppa_ops;
    if (out_errs)    *out_errs    = g_ppa_errs;
    if (out_last_us) *out_last_us = g_ppa_last_us;
    ylock_release(&g_ppa_lock);
}

#else /* !CONFIG_BOARD_ESP32P4 */

bool ppa_init(void) { return false; }

bool ppa_srm_rgb565(const uint16_t *src, uint32_t src_pic_w, uint32_t src_pic_h,
                    uint32_t src_x, uint32_t src_y, uint32_t blk_w, uint32_t blk_h,
                    uint16_t *dst, uint32_t dst_pic_w, uint32_t dst_pic_h,
                    uint32_t dst_x, uint32_t dst_y,
                    uint32_t scale_x_int, uint32_t scale_x_frag,
                    uint32_t scale_y_int, uint32_t scale_y_frag,
                    unsigned rot_ccw_deg, bool flush_src, bool inv_dst) {
    (void)src; (void)src_pic_w; (void)src_pic_h;
    (void)src_x; (void)src_y; (void)blk_w; (void)blk_h;
    (void)dst; (void)dst_pic_w; (void)dst_pic_h;
    (void)dst_x; (void)dst_y;
    (void)scale_x_int; (void)scale_x_frag;
    (void)scale_y_int; (void)scale_y_frag;
    (void)rot_ccw_deg; (void)flush_src; (void)inv_dst;
    return false;
}

void ppa_get_stats(uint32_t *out_ops, uint32_t *out_errs, uint32_t *out_last_us) {
    if (out_ops)     *out_ops     = 0;
    if (out_errs)    *out_errs    = 0;
    if (out_last_us) *out_last_us = 0;
}

#endif /* CONFIG_BOARD_ESP32P4 */
