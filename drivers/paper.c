/* drivers/paper.c -- the screen's ink and paper colours (drivers/paper.h). */

#include "drivers/paper.h"
#include "kernel/settings.h"
#include "lugalos_config.h"

#if defined(CONFIG_BOARD_ESP32P4) && (defined(CONFIG_DSI_LCD_RST_GPIO) || defined(CONFIG_BOARD_TAB5))
#include "drivers/dsi_esp32p4.h"
#define PAPER_P4 1
#elif defined(CONFIG_BOARD_RP2350) && defined(CONFIG_LCD_PCLK_GPIO)
#include "drivers/lcd7.h"
#define PAPER_LCD7 1
#elif defined(CONFIG_BOARD_VISIONFIVE2)
#include "drivers/dc8200_jh7110.h"
#define PAPER_VF2 1
#endif

/* The curve's points, 8 bits a channel (owner, 2026-11-11): the paper goes
 * from white to a light warm paper at PAPER_MID, then on to a deep, aged
 * sepia at the top; the warm ink reaches its brown at PAPER_MID and keeps it. */
#define PAPER_MID 5
#define MID_R   0xF4
#define MID_G   0xE8
#define MID_B   0xD0
#define DEEP_R  0xE3
#define DEEP_G  0xCC
#define DEEP_B  0xA0
#define INK_R   0x3B
#define INK_G   0x2A
#define INK_B   0x1A

#if defined(PAPER_P4) || defined(PAPER_LCD7) || defined(PAPER_VF2)
#define PAPER_PANEL 1
/* accepted: written by the settings store's apply (one caller at a time in
 * practice: the shell or the views task), read for reports; a torn read
 * costs one wrong status line. Only where there is a panel: no SRAM on the
 * headless boards. */
static uint16_t g_ink = 0x0000, g_paper = 0xffff;
#endif

static uint16_t rgb565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* from + (to - from) * w / n, in integers: exact at both ends. */
static unsigned lerp8(unsigned from, unsigned to, int w, int n) {
    return (unsigned)((int)from + ((int)to - (int)from) * w / n);
}

void paper_pair(int warmth, bool warm_ink, uint16_t *ink, uint16_t *paper) {
    int w = warmth < 0 ? 0 : warmth > PAPER_WARMTH_MAX ? PAPER_WARMTH_MAX : warmth;
    if (paper) {
        if (w <= PAPER_MID)
            *paper = rgb565(lerp8(0xFF, MID_R, w, PAPER_MID), lerp8(0xFF, MID_G, w, PAPER_MID),
                            lerp8(0xFF, MID_B, w, PAPER_MID));
        else
            *paper = rgb565(lerp8(MID_R, DEEP_R, w - PAPER_MID, PAPER_WARMTH_MAX - PAPER_MID),
                            lerp8(MID_G, DEEP_G, w - PAPER_MID, PAPER_WARMTH_MAX - PAPER_MID),
                            lerp8(MID_B, DEEP_B, w - PAPER_MID, PAPER_WARMTH_MAX - PAPER_MID));
    }
    int iw = !warm_ink ? 0 : w < PAPER_MID ? w : PAPER_MID;
    if (ink) *ink = rgb565(lerp8(0, INK_R, iw, PAPER_MID), lerp8(0, INK_G, iw, PAPER_MID), lerp8(0, INK_B, iw, PAPER_MID));
}

void paper_apply(void) {
#if defined(PAPER_PANEL)
    int w = settings_get("display.warmth");
    bool warm_ink = settings_get("display.warm_ink") == 1;
    uint16_t ink, paper;
    paper_pair(w, warm_ink, &ink, &paper);
    if (ink == g_ink && paper == g_paper) return;        /* a P4 change redraws the whole screen */
    g_ink = ink;
    g_paper = paper;
#if defined(PAPER_P4)
    dsi_lcd_set_colours(ink, paper);
#elif defined(PAPER_LCD7)
    if (lcd7_is_running()) lcd7_set_colours(ink, paper);
#elif defined(PAPER_VF2)
    jh7110_dc8200_set_colours(ink, paper);
#endif
#endif
}

void paper_current(uint16_t *ink, uint16_t *paper) {
#if defined(PAPER_PANEL)
    if (ink) *ink = g_ink;
    if (paper) *paper = g_paper;
#else
    if (ink) *ink = 0x0000;
    if (paper) *paper = 0xffff;
#endif
}

bool paper_selftest(void) {
    uint16_t ink, paper, last_ink = 0, last_paper = 0xffff;
    paper_pair(0, true, &ink, &paper);
    if (ink != 0x0000 || paper != 0xffff) return false;
    paper_pair(PAPER_MID, true, &ink, &paper);
    if (ink != 0x3943 || paper != 0xf75a) return false;
    paper_pair(PAPER_WARMTH_MAX, true, &ink, &paper);
    if (ink != 0x3943 || paper != 0xe674) return false;
    paper_pair(PAPER_WARMTH_MAX, false, &ink, &paper);
    if (ink != 0x0000 || paper != 0xe674) return false;
    paper_pair(-3, true, &ink, &paper);                  /* clamped */
    if (ink != 0x0000 || paper != 0xffff) return false;
    /* Each step no lighter paper and no darker ink than the one before:
     * every channel moves one way only. */
    for (int w = 0; w <= PAPER_WARMTH_MAX; w++) {
        paper_pair(w, true, &ink, &paper);
        if ((paper >> 11) > (last_paper >> 11) || ((paper >> 5) & 0x3f) > ((last_paper >> 5) & 0x3f) ||
            (paper & 0x1f) > (last_paper & 0x1f))
            return false;
        if ((ink >> 11) < (last_ink >> 11) || ((ink >> 5) & 0x3f) < ((last_ink >> 5) & 0x3f) || (ink & 0x1f) < (last_ink & 0x1f))
            return false;
        last_ink = ink;
        last_paper = paper;
    }
    return true;
}
