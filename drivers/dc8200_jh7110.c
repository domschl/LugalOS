#include "drivers/dc8200_jh7110.h"
#include "drivers/include/drivers/lcdterm.h"
#include "drivers/xhci.h"
#include "drivers/hdmi_jh7110.h"
#include "arch/jh7110_cache.h"
#include "kernel/cursor.h"
#include "kernel/pointer.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/palloc.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/lock.h"
#include <string.h>

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* Register Offsets relative to DC8200_BASE_ADDR (0x29400000) */
#define AQ_INTR_ACKNOWLEDGE             0x0010u
#define AQ_INTR_ENBL                    0x0014u
#define DC_HW_REVISION                  0x0024u
#define DC_HW_CHIP_CID                  0x0030u

#define DC_DISPLAY_PANEL_CONFIG         0x1418u
#define DC_DISPLAY_H                    0x1430u
#define DC_DISPLAY_H_SYNC               0x1438u
#define DC_DISPLAY_V                    0x1440u
#define DC_DISPLAY_V_SYNC               0x1448u
#define DC_DISPLAY_CURRENT_LOCATION     0x1450u
#define DC_DISPLAY_INT                  0x147Cu
#define DC_DISPLAY_INT_ENABLE           0x1480u
#define DC_DISPLAY_DPI_CONFIG           0x14B8u
#define DC_DISPLAY_PANEL_START          0x1CCCu

#define DC_FRAMEBUFFER_ADDRESS          0x1400u
#define DC_FRAMEBUFFER_STRIDE           0x1408u
#define DC_FRAMEBUFFER_CONFIG           0x1518u
#define DC_FRAMEBUFFER_CONFIG_EX        0x1CC0u
#define DC_FRAMEBUFFER_SCALE_CONFIG     0x1520u
#define DC_FRAMEBUFFER_BG_COLOR         0x1528u
#define DC_FRAMEBUFFER_SIZE             0x1810u
#define DC_FRAMEBUFFER_H_FILTER_INDEX   0x1838u
#define DC_FRAMEBUFFER_H_FILTER_DATA    0x1A00u
#define DC_FRAMEBUFFER_V_FILTER_INDEX   0x1A08u
#define DC_FRAMEBUFFER_V_FILTER_DATA    0x1A10u
#define DC_FRAMEBUFFER_INIT_OFFSET      0x1A20u
#define DC_FRAMEBUFFER_TOP_LEFT         0x24D8u
#define DC_FRAMEBUFFER_BOTTOM_RIGHT     0x24E0u
#define DC_QOS_CONFIG                   0x1A38u

#define DC_FRAMEBUFFER_RGBTORGB_COEF0   0x1E20u
#define DC_FRAMEBUFFER_RGBTORGB_COEF1   0x1E28u
#define DC_FRAMEBUFFER_RGBTORGB_COEF2   0x1E30u
#define DC_FRAMEBUFFER_RGBTORGB_COEF3   0x1E38u
#define DC_FRAMEBUFFER_RGBTORGB_COEF4   0x1E40u

#define DC_DISPLAY_RGBTOYUV_COEF0       0x1E48u
#define DC_DISPLAY_RGBTOYUV_COEF1       0x1E50u
#define DC_DISPLAY_RGBTOYUV_COEF2       0x1E58u
#define DC_DISPLAY_RGBTOYUV_COEF3       0x1E60u
#define DC_DISPLAY_RGBTOYUV_COEF4       0x1E68u
#define DC_DISPLAY_RGBTOYUV_COEFD0      0x1E70u
#define DC_DISPLAY_RGBTOYUV_COEFD1      0x1E78u
#define DC_DISPLAY_RGBTOYUV_COEFD2      0x1E80u

/* Filter and color conversion tables */
static const uint32_t k_hor_kernel[16] = {
    0x00000000, 0x20000000, 0x00002000, 0x00000000,
    0x00000000, 0x00000000, 0x23fd1c03, 0x00000000,
    0x00000000, 0x00000000, 0x181f0000, 0x000027e1,
    0x00000000, 0x00000000, 0x00000000, 0x2b981468
};

static const uint32_t k_ver_kernel[16] = {
    0x00000000, 0x20000000, 0x00002000, 0x00000000,
    0x00000000, 0x00000000, 0x23fd1c03, 0x00000000,
    0x00000000, 0x00000000, 0x181f0000, 0x000027e1,
    0x00000000, 0x00000000, 0x00000000, 0x2b981468
};

static const uint16_t k_rgb2rgb[9] = {
    10279, 5395, 709,
    1132, 15065, 187,
    269, 1442, 14674
};

static const int16_t k_rgb2yuv[12] = {
    230, 594, 52,
    -125, -323, 448,
    448, -412, -36,
    64, 512, 512
};

/* Uncached alias offset in Sv39 paging: 0x400000000UL */
#define UNCACHED_DRAM_OFFSET 0x400000000UL

/* single task: driver initialization and flip engine; guarded by: g_dc_lock */
static dc8200_state_t g_state;
static ylock_t g_dc_lock;

/* 1-bpp text terminal frame buffer and shadows for dirty tracking */
/* single task: lcdterm flush task; guarded by: console frame lock */
static uint8_t *g_t1;
static uint8_t *g_t1_shadow[2];
static uint32_t g_t1_bytes;
static uint16_t g_lut565[256][8];
static lcdterm_backend_t g_backend;
static bool g_term_task_running = false;
static bool g_term_dirty = false;
/* accepted: set by the console's pointer hook (any task), taken by the
 * pacer; a move seen a frame late is drawn a frame late. */
static volatile bool g_cursor_moved;

/* lcdterm_backend_t.pointer: the pacer draws the cursor at its next frame. */
static void dc8200_pointer(void) {
    g_cursor_moved = true;
}
static bool g_force[2] = { true, true };

static inline void dc_write(uint32_t offset, uint32_t val) {
    REG32(DC8200_BASE_ADDR + offset) = val;
}

static inline uint32_t dc_read(uint32_t offset) {
    return REG32(DC8200_BASE_ADDR + offset);
}

static inline void dc_set_clear(uint32_t offset, uint32_t set_bits, uint32_t clr_bits) {
    uint32_t val = dc_read(offset);
    val = (val & ~clr_bits) | set_bits;
    dc_write(offset, val);
}

static void *pa_to_uncached(uint32_t pa) {
    return (void *)(uintptr_t)(UNCACHED_DRAM_OFFSET + (uintptr_t)pa);
}

static void dc8200_build_lut(uint16_t fg, uint16_t bg) {
    for (unsigned v = 0; v < 256u; v++) {
        for (unsigned bit = 0; bit < 8u; bit++) {
            g_lut565[v][bit] = ((v >> bit) & 1u) ? fg : bg;
        }
    }
}

int jh7110_dc8200_init(hdmi_resolution_t res) {
    if (g_state.initialized) return 0;

    memset(&g_state, 0, sizeof(g_state));

    /* 1. Read hardware revision and chip CID */
    g_state.revision = dc_read(DC_HW_REVISION);
    g_state.chip_cid = dc_read(DC_HW_CHIP_CID);

    /* 2. Determine display resolution */
    if (res == HDMI_RES_1280_720P_60HZ) {
        g_state.width = 1280;
        g_state.height = 720;
    } else {
        g_state.width = 1920;
        g_state.height = 1080;
    }
    g_state.stride_bytes = (uint16_t)(g_state.width * 2u); /* RGB565 */

    /* 3. Allocate double-buffered scanout memory in DRAM */
    uint32_t fb_bytes = (uint32_t)g_state.stride_bytes * g_state.height;
    uint32_t pages = (fb_bytes + 4095u) / 4096u;

    void *p1 = palloc_pages_bulk(pages);
    void *p2 = palloc_pages_bulk(pages);
    if (!p1 || !p2) {
        printk("[DC8200] Failed to allocate %u bulk pages for scanout\n", pages * 2u);
        return -1;
    }

    g_state.front_fb_pa = (uint32_t)(uintptr_t)p1;
    g_state.back_fb_pa = (uint32_t)(uintptr_t)p2;
    g_state.front_idx = 0;
    g_state.back_idx = 1;
    g_state.front_fb = (uint16_t *)pa_to_uncached(g_state.front_fb_pa);
    g_state.back_fb = (uint16_t *)pa_to_uncached(g_state.back_fb_pa);

    /* Drop all L1/L2 cache lines populated during palloc's zone_zero so stale
     * dirty zeroes can never be evicted to overwrite DDR scanout buffers. */
    jh7110_flush_l2_range(g_state.front_fb_pa, g_state.front_fb_pa + fb_bytes);
    jh7110_flush_l2_range(g_state.back_fb_pa, g_state.back_fb_pa + fb_bytes);

    /* Clear both framebuffers to clean white background */
    memset(g_state.front_fb, 0xFF, fb_bytes);
    memset(g_state.back_fb, 0xFF, fb_bytes);

    /* 4. Load filter coefficients and color conversion tables */
    dc_write(DC_FRAMEBUFFER_SCALE_CONFIG, 0x33u);
    dc_write(DC_FRAMEBUFFER_INIT_OFFSET, 0x80008000u);
    dc_write(DC_FRAMEBUFFER_H_FILTER_INDEX, 0x00u);
    for (size_t i = 0; i < 16; i++) {
        dc_write(DC_FRAMEBUFFER_H_FILTER_DATA, k_hor_kernel[i]);
    }
    dc_write(DC_FRAMEBUFFER_V_FILTER_INDEX, 0x00u);
    for (size_t i = 0; i < 16; i++) {
        dc_write(DC_FRAMEBUFFER_V_FILTER_DATA, k_ver_kernel[i]);
    }

    /* RGB to RGB matrix */
    dc_write(DC_FRAMEBUFFER_RGBTORGB_COEF0, k_rgb2rgb[0] | ((uint32_t)k_rgb2rgb[1] << 16));
    dc_write(DC_FRAMEBUFFER_RGBTORGB_COEF1, k_rgb2rgb[2] | ((uint32_t)k_rgb2rgb[3] << 16));
    dc_write(DC_FRAMEBUFFER_RGBTORGB_COEF2, k_rgb2rgb[4] | ((uint32_t)k_rgb2rgb[5] << 16));
    dc_write(DC_FRAMEBUFFER_RGBTORGB_COEF3, k_rgb2rgb[6] | ((uint32_t)k_rgb2rgb[7] << 16));
    dc_write(DC_FRAMEBUFFER_RGBTORGB_COEF4, k_rgb2rgb[8]);

    /* RGB to YUV matrix */
    dc_write(DC_DISPLAY_RGBTOYUV_COEF0, (uint16_t)k_rgb2yuv[0] | ((uint32_t)(uint16_t)k_rgb2yuv[1] << 16));
    dc_write(DC_DISPLAY_RGBTOYUV_COEF1, (uint16_t)k_rgb2yuv[2] | ((uint32_t)(uint16_t)k_rgb2yuv[3] << 16));
    dc_write(DC_DISPLAY_RGBTOYUV_COEF2, (uint16_t)k_rgb2yuv[4] | ((uint32_t)(uint16_t)k_rgb2yuv[5] << 16));
    dc_write(DC_DISPLAY_RGBTOYUV_COEF3, (uint16_t)k_rgb2yuv[6] | ((uint32_t)(uint16_t)k_rgb2yuv[7] << 16));
    dc_write(DC_DISPLAY_RGBTOYUV_COEF4, (uint16_t)k_rgb2yuv[8]);
    dc_write(DC_DISPLAY_RGBTOYUV_COEFD0, (uint32_t)k_rgb2yuv[9]);
    dc_write(DC_DISPLAY_RGBTOYUV_COEFD1, (uint32_t)k_rgb2yuv[10]);
    dc_write(DC_DISPLAY_RGBTOYUV_COEFD2, (uint32_t)k_rgb2yuv[11]);

    /* 5. Program VTG video timing */
    uint16_t h_active, h_total, h_sync_start, h_sync_end;
    uint16_t v_active, v_total, v_sync_start, v_sync_end;

    if (res == HDMI_RES_1280_720P_60HZ) {
        h_active = 1280; h_total = 1650; h_sync_start = 1390; h_sync_end = 1430;
        v_active = 720;  v_total = 750;  v_sync_start = 725;  v_sync_end = 730;
    } else {
        h_active = 1920; h_total = 2200; h_sync_start = 2008; h_sync_end = 2052;
        v_active = 1080; v_total = 1125; v_sync_start = 1084; v_sync_end = 1089;
    }

    dc_write(DC_DISPLAY_H, (uint32_t)h_active | ((uint32_t)h_total << 16));
    dc_write(DC_DISPLAY_H_SYNC, (uint32_t)h_sync_start | ((uint32_t)h_sync_end << 15) | (1u << 30));
    dc_write(DC_DISPLAY_V, (uint32_t)v_active | ((uint32_t)v_total << 16));
    dc_write(DC_DISPLAY_V_SYNC, (uint32_t)v_sync_start | ((uint32_t)v_sync_end << 15) | (1u << 30));

    /* 6. Configure display bridge and DPI/DP formats for Innosilicon HDMI TX */
    /* DSSCTRL (0x295B0000): enable display subsystem routing gates */
    REG32(0x295B0004UL) |= (1u << 20);
    REG32(0x295B0008UL) |= (1u << 3);

    /* DC8200 output configuration (matching U-Boot sf_vop.c):
     * 0x14B8 (DPI_CONFIG) = 5 (RGB888 24-bit)
     * 0x1CD0 (DP_CONFIG)  = 0xD2 (DP output config)
     * 0x14D0 = 0x52
     * 0x1CE8 = 0x0F61
     * 0x2510 = 0x2042 (BLEND_CONFIG)
     * 0x2508 = 0x808A3156 (DST_GLOBAL_COLOR)
     * 0x2500 = 0x8008E1B2 (SRC_GLOBAL_COLOR)
     * Hardware cursor plane disabled and clock-gated (0x1468/0x24E8 = 0)
     * to eliminate random 2cm horizontal scanout line artifacts.
     */
    dc_write(DC_DISPLAY_DPI_CONFIG, 5u);
    dc_write(0x1CD0u, 0xD2u);
    dc_write(0x14D0u, 0x52u);
    dc_write(0x1CE8u, 0x0F61u);
    dc_write(0x2510u, 0x2042u);
    dc_write(0x2508u, 0x808A3156u);
    dc_write(0x2500u, 0x8008E1B2u);
    /* High AXI QoS (0xE / 0x8) prevents scanout DMA starvation */
    dc_write(DC_QOS_CONFIG, 0x000000E8u);
    /* Un-gate hardware cursor clocks so the DC8200 mixer pipeline runs cleanly */
    dc_write(0x1484u, 0x00000000u); /* Cursor 0 clock un-gated */
    dc_write(0x24FCu, 0x00000000u); /* Cursor 1 clock un-gated */

    /* Bypass/disable hardware cursors (bit 3 = bypass, bits 0..1 = 0) */
    dc_write(0x1468u, 0x00000008u); /* Cursor 0 bypass */
    dc_write(0x24E8u, 0x00000008u); /* Cursor 1 bypass */

    /* Position hardware cursors off-screen */
    dc_write(0x1470u, 0x0FFF0FFFu);
    dc_write(0x24F0u, 0x0FFF0FFFu);
    dc_write(0x146Cu, 0x00000000u);
    dc_write(0x24ECu, 0x00000000u);

    /* Disable/clear overlay planes 0..3 */
    dc_write(0x1540u, 0x00060000u);
    dc_write(0x2540u, 0x00000001u);
    dc_write(0x1540u, 0x80060000u);
    dc_write(0x1544u, 0x00060000u);
    dc_write(0x2544u, 0x00000002u);
    dc_write(0x1544u, 0x80060000u);
    dc_write(0x1548u, 0x00060000u);
    dc_write(0x2548u, 0x0000000Cu);
    dc_write(0x1548u, 0x80060000u);
    dc_write(0x154Cu, 0x00060000u);
    dc_write(0x254Cu, 0x0000000Du);
    dc_write(0x154Cu, 0x80060000u);

    /* 7. Configure Primary Plane (Layer 0) DMA scanout */
    dc_write(DC_FRAMEBUFFER_ADDRESS, g_state.front_fb_pa);
    dc_write(DC_FRAMEBUFFER_STRIDE, g_state.stride_bytes);
    dc_write(DC_FRAMEBUFFER_SIZE, (uint32_t)g_state.width | ((uint32_t)g_state.height << 15));
    dc_write(DC_FRAMEBUFFER_TOP_LEFT, 0u);
    dc_write(DC_FRAMEBUFFER_BOTTOM_RIGHT, (uint32_t)g_state.width | ((uint32_t)g_state.height << 15));

    /* Format 4 = FORMAT_R5G6B5 (bits 26..30) */
    dc_write(DC_FRAMEBUFFER_CONFIG, (4u << 26));

    /* Enable plane (bit 13) and shadow registers (bit 12) */
    dc_write(DC_FRAMEBUFFER_CONFIG_EX, (1u << 13) | (1u << 12));
    dc_write(DC_FRAMEBUFFER_BG_COLOR, 0x00000000u);

    /* 8. Enable Panel Start */
    dc_write(DC_DISPLAY_PANEL_CONFIG, 0x1111u);
    dc_write(0x1410u, 0x00000000u);
    dc_write(0x2518u, 0x00000000u); /* Unmute */
    dc_write(DC_DISPLAY_PANEL_START, 0x00000001u);

    /* Enable VBLANK interrupt in AQ_INTR_ENBL */
    dc_write(AQ_INTR_ENBL, 1u << 0);

    g_state.initialized = true;
    g_state.panel_started = true;

    printk("[DC8200] VeriSilicon DC8200 Online (Rev 0x%04x, CID 0x%04x): %ux%u RGB565 @ 60 Hz\n",
           g_state.revision, g_state.chip_cid, g_state.width, g_state.height);
    printk("[DC8200] Front FB: 0x%08x (PA), Back FB: 0x%08x (PA), Size: %u KB x 2\n",
           g_state.front_fb_pa, g_state.back_fb_pa, fb_bytes / 1024u);

    return 0;
}

void jh7110_dc8200_flip(void) {
    if (!g_state.initialized) return;

    ylock_acquire(&g_dc_lock);

    /* Render mouse cursor onto back buffer before flip */
    cursor_render(g_state.back_fb, g_state.width, g_state.height);

    /* Hardware shadow registers latch the new address at VBLANK.
     * Follow Linux dc_hw_enable_shadow_register() to latch smoothly: */
    dc_set_clear(DC_FRAMEBUFFER_CONFIG_EX, 0, (1u << 12));
    dc_set_clear(0x2518u, (1u << 0), 0);

    dc_write(DC_FRAMEBUFFER_ADDRESS, g_state.back_fb_pa);

    dc_set_clear(DC_FRAMEBUFFER_CONFIG_EX, (1u << 12), 0);
    dc_set_clear(0x2518u, 0, (1u << 0));
    g_state.vsync_count++;

    /* Swap front and back pointers and indices */
    uint32_t tmp_pa = g_state.front_fb_pa;
    g_state.front_fb_pa = g_state.back_fb_pa;
    g_state.back_fb_pa = tmp_pa;

    uint16_t *tmp_ptr = g_state.front_fb;
    g_state.front_fb = g_state.back_fb;
    g_state.back_fb = tmp_ptr;

    uint8_t tmp_idx = g_state.front_idx;
    g_state.front_idx = g_state.back_idx;
    g_state.back_idx = tmp_idx;

    g_state.frames_flipped++;
    ylock_release(&g_dc_lock);
}

void jh7110_dc8200_test_pattern(void) {
    if (!g_state.initialized) return;

    /* 8 standard SMPTE color bars in RGB565 */
    static const uint16_t k_colors[8] = {
        0xFFFF, /* White */
        0xFFE0, /* Yellow */
        0x07FF, /* Cyan */
        0x07E0, /* Green */
        0xF81F, /* Magenta */
        0xF800, /* Red */
        0x001F, /* Blue */
        0x0000  /* Black */
    };

    uint16_t *fb = g_state.back_fb;
    unsigned bar_w = g_state.width / 8u;

    for (unsigned y = 0; y < g_state.height; y++) {
        uint16_t *row = &fb[y * g_state.width];
        for (unsigned bar = 0; bar < 8; bar++) {
            uint16_t color = k_colors[bar];
            unsigned x_end = (bar == 7) ? g_state.width : (bar + 1) * bar_w;
            for (unsigned x = bar * bar_w; x < x_end; x++) {
                row[x] = color;
            }
        }
    }
    jh7110_dc8200_flip();
}

/* Ground-truth cursor erase: re-render 16x16 footprint directly from 1-bpp g_t1 */
static void dc8200_cursor_restore(uint16_t *fb, unsigned fb_w, unsigned fb_h, int x, int y) {
    if (!g_t1 || !fb) return;
    unsigned bytes_per_row = fb_w / 8u;
    int b_start = x < 0 ? 0 : x / 8;
    int b_end = (x + 15) / 8;
    if (b_end < 0) return;
    if ((unsigned)b_end >= bytes_per_row) b_end = (int)bytes_per_row - 1;

    for (int r = 0; r < 16; r++) {
        int py = y + r;
        if (py < 0 || (unsigned)py >= fb_h) continue;

        const uint8_t *src_row = &g_t1[py * bytes_per_row];
        uint16_t *dst_row = &fb[py * fb_w];

        for (int b = b_start; b <= b_end; b++) {
            uint8_t val = src_row[b];
            const uint16_t *lut = g_lut565[val];
            uint64_t *d64 = (uint64_t *)(dst_row + b * 8);
            const uint64_t *s64 = (const uint64_t *)lut;
            d64[0] = s64[0];
            d64[1] = s64[1];
        }
    }
}

/* Fast conversion of 1-bpp text rows [y0, y1) into RGB565 back buffer */
static void jh7110_dc8200_flush(unsigned y0, unsigned y1) {
    if (!g_state.initialized || !g_t1 || !g_t1_shadow[0] || !g_t1_shadow[1]) return;
    if (y1 > g_state.height) y1 = g_state.height;
    if (y0 >= y1) return;

    ylock_acquire(&g_dc_lock);

    /* Restore background under cursor on back buffer before modifying lines */
    cursor_restore(g_state.back_fb, g_state.width, g_state.height);

    unsigned bytes_per_row = g_state.width / 8u;
    uint8_t b_idx = g_state.back_idx;
    uint8_t *shadow = g_t1_shadow[b_idx];
    uint16_t *back = g_state.back_fb;
    bool force = g_force[b_idx];
    g_force[b_idx] = false;
    bool dirty = false;

    for (unsigned y = y0; y < y1; y++) {
        const uint8_t *src = &g_t1[y * bytes_per_row];
        uint8_t *sh = &shadow[y * bytes_per_row];

        /* Check if row changed relative to this buffer */
        if (!force && memcmp(src, sh, bytes_per_row) == 0) continue;
        memcpy(sh, src, bytes_per_row);
        dirty = true;

        uint16_t *dst_b = &back[y * g_state.width];
        for (unsigned b = 0; b < bytes_per_row; b++) {
            uint8_t val = src[b];
            const uint16_t *lut = g_lut565[val];
            uint64_t *d64_b = (uint64_t *)(dst_b + b * 8);
            const uint64_t *s64 = (const uint64_t *)lut;
            d64_b[0] = s64[0];
            d64_b[1] = s64[1];
        }
    }
    if (dirty) {
        g_term_dirty = true;
    }

    ylock_release(&g_dc_lock);
}

static void jh7110_dc8200_sync_post(void) {
    jh7110_dc8200_flush(0, g_state.height);
    if (g_term_dirty) {
        jh7110_dc8200_flip();
        g_term_dirty = false;
    }
}

static bool jh7110_dc8200_capture(const uint8_t **fb, unsigned *w, unsigned *h,
                                  unsigned *stride, unsigned *format) {
    if (!g_state.initialized) return false;
    if (fb) *fb = (const uint8_t *)g_state.front_fb;
    if (w) *w = g_state.width;
    if (h) *h = g_state.height;
    if (stride) *stride = g_state.stride_bytes;
    if (format) *format = SCREEN_FORMAT_RGB565;
    return true;
}

static bool jh7110_dc8200_is_double_buffered(void) {
    return true;
}

int jh7110_dc8200_term_start(void) {
    const hdmi_monitor_info_t *mon = jh7110_hdmi_get_monitor_info();
    hdmi_resolution_t res = mon ? mon->target_res : HDMI_RES_1920_1080P_60HZ;

    if (jh7110_dc8200_init(res) != 0) {
        return -1;
    }

    /* Allocate 1-bpp frame and dual shadows (one per buffer) for lcdterm */
    g_t1_bytes = (uint32_t)(g_state.width / 8u) * g_state.height;
    uint32_t pages = ((g_t1_bytes * 3u) + 4095u) / 4096u;

    uint8_t *mem = (uint8_t *)palloc_pages_bulk(pages);
    if (!mem) {
        printk("[DC8200] Failed to allocate memory for 1-bpp terminal frame\n");
        return -1;
    }
    memset(mem, 0, g_t1_bytes);
    g_t1 = mem;
    g_t1_shadow[0] = mem + g_t1_bytes;
    g_t1_shadow[1] = mem + 2u * g_t1_bytes;
    memset(g_t1_shadow[0], 0, g_t1_bytes);
    memset(g_t1_shadow[1], 0, g_t1_bytes);
    g_force[0] = true;
    g_force[1] = true;

    dc8200_build_lut(0x0000, 0xFFFF); /* Black text on white background default */
    cursor_set_restore_cb(dc8200_cursor_restore);

    /* Configure backend structure */
    memset(&g_backend, 0, sizeof(g_backend));
    g_backend.name = "JH7110-HDMI";
    g_backend.text_test_title = "StarFive VisionFive 2 HDMI 1080p60 Workstation";
    g_backend.fb = g_t1;
    g_backend.w = g_state.width;
    g_backend.h = g_state.height;
    g_backend.stride = g_state.width / 8u;
    g_backend.flush = jh7110_dc8200_flush;
    g_backend.sync_post = jh7110_dc8200_sync_post;
    g_backend.pointer = dc8200_pointer;
    g_backend.dpi = mon && mon->dpi ? mon->dpi : 96u;   /* the monitor's EDID size; 96 without one */
    g_backend.capture = jh7110_dc8200_capture;
    g_backend.is_double_buffered = jh7110_dc8200_is_double_buffered;

    if (lcdterm_init(&g_backend) != 0) {
        printk("[DC8200] lcdterm_init failed\n");
        return -1;
    }

    /* Synchronize both scanout buffers to identical initial clean state */
    g_state.back_idx = 0;
    g_state.back_fb = (uint16_t *)pa_to_uncached(g_state.front_fb_pa);
    jh7110_dc8200_flush(0, g_state.height);

    g_state.back_idx = 1;
    g_state.back_fb = (uint16_t *)pa_to_uncached(g_state.back_fb_pa);
    jh7110_dc8200_flush(0, g_state.height);

    printk("[DC8200] Workstation Desktop Online: %ux%u on HDMI\n", g_state.width, g_state.height);
    return 0;
}

static void dc8200_pacer_task(void *arg) {
    (void)arg;
    bool mouse_was = false;

    while (1) {
        /* 60 Hz frame pacer: ~16.6 ms */
        time_delay_us(16666);

        /* Poll USB host controller and mouse */
        xhci_poll();

        /* The mouse's motion to the console's pointer (kernel/pointer.c:
         * speed, the screen's bounds); this task is its one caller here. */
        bool mouse_now = xhci_is_mouse_present();
        int mdx = 0, mdy = 0, mwheel = 0;
        uint8_t mbtn = 0;
        if (mouse_now && xhci_take_mouse(&mdx, &mdy, &mbtn, &mwheel)) pointer_relative(mdx, mdy, mbtn, mwheel);
        if (mouse_was && !mouse_now) {
            pointer_release();
            console_pointer_hide();
        }
        mouse_was = mouse_now;
        bool mouse_changed = __atomic_exchange_n(&g_cursor_moved, false, __ATOMIC_ACQ_REL);

        /* Redraw terminal dirty regions and synchronize back buffer */
        lcdterm_screen_flush();
        jh7110_dc8200_flush(0, g_state.height);

        if (mouse_changed || g_term_dirty) {
            jh7110_dc8200_flip();
            g_term_dirty = false;
        }
    }
}

int jh7110_dc8200_term_task_start(void) {
    if (g_term_task_running) return 0;
    int pid = task_create("lcdflush", dc8200_pacer_task, NULL);
    if (pid < 0) return -1;
    g_term_task_running = true;
    return pid;
}

uint16_t *jh7110_dc8200_get_front_fb(void) {
    return g_state.front_fb;
}

uint16_t *jh7110_dc8200_get_back_fb(void) {
    return g_state.back_fb;
}

void jh7110_dc8200_get_res(unsigned *w, unsigned *h) {
    if (w) *w = g_state.width;
    if (h) *h = g_state.height;
}

const dc8200_state_t *jh7110_dc8200_get_state(void) {
    return &g_state;
}

void jh7110_dc8200_cmd_status(void) {
    ylock_acquire(&g_dc_lock);
    dc8200_state_t st = g_state;
    ylock_release(&g_dc_lock);

    uint32_t loc = dc_read(DC_DISPLAY_CURRENT_LOCATION);

    cprintf("VeriSilicon DC8200 Display Controller (0x%lx):\n", DC8200_BASE_ADDR);
    cprintf("  Driver State:     %s (Panel %s)\n",
            st.initialized ? "ONLINE" : "OFFLINE",
            st.panel_started ? "RUNNING" : "STOPPED");
    cprintf("  Hardware Rev:     0x%04x (CID: 0x%04x)\n", st.revision, st.chip_cid);
    cprintf("  Active Scanout:   %ux%u @ 60 Hz (RGB565, stride %u B)\n",
            st.width, st.height, st.stride_bytes);
    cprintf("  Front Buffer PA:  0x%08x (VA: 0x%lx, Slot %u)\n",
            st.front_fb_pa, (unsigned long)(uintptr_t)st.front_fb, st.front_idx);
    cprintf("  Back Buffer PA:   0x%08x (VA: 0x%lx, Slot %u)\n",
            st.back_fb_pa, (unsigned long)(uintptr_t)st.back_fb, st.back_idx);
    cprintf("  Current Scanline: Y=%u, X=%u (reg 0x%08x)\n",
            (unsigned)(loc >> 16), (unsigned)(loc & 0xFFFF), (unsigned)loc);
    cprintf("  Performance:      %u frames flipped, %u VSYNC pulses\n",
            st.frames_flipped, st.vsync_count);
}

void jh7110_dc8200_repaint(void) {
    if (!g_state.initialized || !g_t1 || !g_t1_shadow[0] || !g_t1_shadow[1]) return;
    ylock_acquire(&g_dc_lock);
    g_force[0] = true;
    g_force[1] = true;
    ylock_release(&g_dc_lock);

    /* Flush and flip both buffers so neither has stale data */
    jh7110_dc8200_flush(0, g_state.height);
    jh7110_dc8200_flip();
    jh7110_dc8200_flush(0, g_state.height);
    jh7110_dc8200_flip();
}

void jh7110_dc8200_cmd_test(void) {
    cprintf("DC8200: Rendering 8-color SMPTE test bars to HDMI display...\n");
    jh7110_dc8200_test_pattern();
    cprintf("DC8200: Test pattern presented. Restoring terminal in 3 seconds...\n");
    time_delay_us(3000000);
    jh7110_dc8200_repaint();
    cprintf("DC8200: Terminal restored.\n");
}
