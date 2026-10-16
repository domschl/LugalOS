#ifndef LUGALOS_DRIVERS_LCDTERM_H
#define LUGALOS_DRIVERS_LCDTERM_H

/* The panel terminal, portable:
 * the screen_t (drivers/screen.h) on a 1-bpp frame, run by the U-mode
 * `lcdterm` task, fed in batches by the console and the kernel log, with the
 * menu bar's title and status, the tee to a host, and the console's screen
 * hooks. Until 47.6 this lived inside drivers/lcd7_rp2350.c; the panel
 * drivers now only scan a frame out and describe it here.
 *
 * A panel driver brings its glass up, then hands lcdterm_init() a backend:
 * the 1-bpp frame (leftmost pixel in bit 0 of each byte, the order the
 * RP2350's PIO scans and font8x16 is stored in), its size, the naturally
 * aligned pieces of it the task may write (a PMP domain's regions), and
 * optionally a flush for frames that are not scanned out directly. */

#include <stdbool.h>
#include <stdint.h>

#include "drivers/canvas1.h"
#include "kernel/console.h"
#include "lugalos_config.h"

#if (defined(CONFIG_BOARD_RP2350) && defined(CONFIG_LCD_PCLK_GPIO)) || \
    (defined(CONFIG_BOARD_ESP32P4) && defined(CONFIG_DSI_LCD_RST_GPIO))
#define LCDTERM_PRESENT 1
#endif

/* Where the terminal can run as the U-mode `lcdterm` task: boards whose
 * linker script gives its code a region (.lcdtermtext, board_lcdterm_text_
 * region()). The RP2350 since 36.6a. Elsewhere -- the LCD-7B, 47.7 -- the
 * terminal draws from the kernel, the fallback it always had; a P4 text
 * region (32 KB of SRAM, or PMP over the XIP window) is a later step. */
#if defined(LCDTERM_PRESENT) && defined(CONFIG_BOARD_RP2350)
#define LCDTERM_UMODE 1
#endif

#define LCDTERM_MAX_FB_REGIONS 2u

typedef struct {
    const char *name;               /* the panel, for reports: "RP2350-LCD-7" */
    const char *text_test_title;    /* `lcd test text`'s first line */
    uint8_t    *fb;                 /* 1-bpp, `stride` bytes per line, 1 = foreground */
    unsigned    w, h, stride;       /* pixels, pixels, bytes; w a multiple of 8 */
    struct { uintptr_t base; uint32_t size; } fb_regions[LCDTERM_MAX_FB_REGIONS];
    unsigned    fb_region_count;    /* the frame as the task's domain grants it */
    /* Rows [y0, y1) of the frame changed. NULL when the scan-out reads the
     * frame itself (RP2350); a converting backend (47.7) does its work here. */
    void      (*flush)(unsigned y0, unsigned y1);
    /* 47.11: the backlight off, or back at its level (idle blanking). */
    void      (*blank)(bool off);
    /* Display synchronization hooks: sync_pre before rendering intermediate
     * frame (e.g. wait for VBLANK to eliminate tearing on RP2350); sync_post
     * after rendering intermediate frame (e.g. convert & flush to PSRAM on P4). */
    void      (*sync_pre)(void);
    void      (*sync_post)(void);
} lcdterm_backend_t;

/* Takes the backend (kept by pointer: it must outlive the terminal), finds
 * memory for the screen's state and canvas stores, and initialises the
 * screen on the frame. 0, or -1 if there is no memory for the state. */
int lcdterm_init(const lcdterm_backend_t *be);

/* The U-mode task. Until it runs, and if it never does, the terminal draws
 * from the kernel. */
int lcdterm_task_start(void);

/* The console stream and the kernel log's `lcd` sink. */
void lcdterm_screen_putc(char c);
void lcdterm_screen_putc_vterm(int vid, char c);
void lcdterm_console_putc(char c);           /* the screen, plus the tee */
void lcdterm_screen_flush(void);

/* Where the `lcd` console device copies its output for a host. */
#define LCD_TEE_UART 0u
#define LCD_TEE_USB  1u
#define LCD_TEE_OFF  2u
void lcdterm_set_tee(unsigned mode);
unsigned lcdterm_tee(void);

const console_screen_t *lcdterm_console_screen(void);
void lcdterm_set_title(const char *title);
void lcdterm_repaint(void);
uint32_t lcdterm_unknown_sequences(void);
uint32_t lcdterm_task_call_count(void);

/* `lcdtermisotest`, `lcd test text`, `lcd scroll N`, and the terminal half
 * of `lcd`. */
bool lcdterm_isolation_test(uintptr_t *out_canary, bool *out_exited_clean);
int lcdterm_text_test(void);
void lcdterm_scroll_test(unsigned n);
void lcdterm_report(void);
canvas1_t *lcdterm_get_canvas_backing(uint8_t cid);
canvas1_t *lcdterm_get_reader_backing(uint8_t rid);
#if defined(LCDTERM_PRESENT)
bool lcdterm_is_animating(void);
void lcdterm_sync_pre(void);
void lcdterm_sync_post(void);
#else
static inline bool lcdterm_is_animating(void) { return false; }
static inline void lcdterm_sync_pre(void) { }
static inline void lcdterm_sync_post(void) { }
#endif

#endif /* LUGALOS_DRIVERS_LCDTERM_H */
