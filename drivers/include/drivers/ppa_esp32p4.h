#ifndef LUGALOS_DRIVERS_PPA_ESP32P4_H
#define LUGALOS_DRIVERS_PPA_ESP32P4_H

#include <stdint.h>
#include <stdbool.h>

/* Initialize the ESP32-P4 Pixel Processing Accelerator (PPA) SRM engine and
 * 2D-DMA channel 0 (TX + RX). Idempotent and safe to call from any task. */
bool ppa_init(void);

/* Hardware-accelerated Scale-Rotate-Mirror (SRM) on an RGB565 rectangular block
 * via the ESP32-P4 PPA SRM engine and 2D-DMA channel 0.
 *
 * Preconditions & Contracts:
 * - `src` and `dst` must point to 16-bit RGB565 buffers in internal SRAM or PSRAM.
 * - `src_x + blk_w <= src_pic_w` and `src_y + blk_h <= src_pic_h`.
 * - `scale_x_int`, `scale_y_int` in 0..255; `scale_x_frag`, `scale_y_frag` in 0..15
 *   (effective scale factor = scale_int + scale_frag / 16).
 * - `rot_ccw_deg` must be 0, 90, 180, or 270 (counter-clockwise rotation in degrees:
 *   Note that physical display rotation `g_lcd_rot == 270` corresponds to `rot_ccw_deg == 90`,
 *   and `g_lcd_rot == 90` corresponds to `rot_ccw_deg == 270`).
 * - If `flush_src` is true, flushes the source window in `src` from CPU L1/L2 cache
 *   to PSRAM before starting DMA.
 * - If `inv_dst` is true, invalidates the destination window in `dst` in CPU L1/L2 cache
 *   before starting DMA so no dirty cache line can overwrite DMA output.
 * - Returns true when DMA2D RX SUC_EOF completes without error, false on invalid
 *   parameters, hardware error, or timeout. */
bool ppa_srm_rgb565(const uint16_t *src, uint32_t src_pic_w, uint32_t src_pic_h,
                    uint32_t src_x, uint32_t src_y, uint32_t blk_w, uint32_t blk_h,
                    uint16_t *dst, uint32_t dst_pic_w, uint32_t dst_pic_h,
                    uint32_t dst_x, uint32_t dst_y,
                    uint32_t scale_x_int, uint32_t scale_x_frag,
                    uint32_t scale_y_int, uint32_t scale_y_frag,
                    unsigned rot_ccw_deg, bool flush_src, bool inv_dst);

/* Query cumulative PPA SRM operation count, error count, and last transfer duration (us). */
void ppa_get_stats(uint32_t *out_ops, uint32_t *out_errs, uint32_t *out_last_us);

#endif /* LUGALOS_DRIVERS_PPA_ESP32P4_H */
