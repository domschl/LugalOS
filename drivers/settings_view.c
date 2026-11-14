/* drivers/settings_view.c -- the Settings app (drivers/settings_view.h).
 *
 * Drawn 1-bpp, as the Lisp programs' `ui-` windows are: the panel maps it
 * to the ink and paper colours (drivers/paper.h), so a warmth change shows
 * on this window as it is being made. */

#include "drivers/settings_view.h"
#include "kernel/console.h"
#include "lugalos_config.h"

#if !(defined(CONFIG_BOARD_ESP32C6) || (defined(CONFIG_BOARD_RP2350) && !defined(CONFIG_LCD_PCLK_GPIO)))

#include "drivers/battery.h"
#include "drivers/lcdterm.h"
#include "drivers/paper.h"
#include "drivers/ramscreen.h"
#include "drivers/ribbon.h"
#include "drivers/screen.h"
#include "drivers/ugui.h"
#include "drivers/ui_sound.h"
#include "drivers/uxbench.h"
#include "fs/vfs.h"
#include "kernel/boardinfo.h"
#include "kernel/console.h"
#include "kernel/keyseq.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/settings.h"
#include "kernel/time.h"
#include "net/ip.h"
#include "net/netif.h"

#include <string.h>

enum {
    SEC_DISPLAY = SETTINGS_SECTION_DISPLAY, SEC_SOUND = SETTINGS_SECTION_SOUND, SEC_INPUT = SETTINGS_SECTION_INPUT,
    SEC_POWER = SETTINGS_SECTION_POWER, SEC_NETWORK = SETTINGS_SECTION_NETWORK, SEC_STORAGE = SETTINGS_SECTION_STORAGE,
    SEC_ABOUT = SETTINGS_SECTION_ABOUT, SEC_COUNT = SETTINGS_SECTION_COUNT
};
static const char *const k_sections[SEC_COUNT] = { "Display", "Sound", "Input", "Power", "Network", "Storage", "About" };

#define ID_SECTION   10          /* index: the section */
#define ID_BACK      20
#define ID_SCROLLBAR 21
#define ID_BRIGHT    100
#define ID_WARMTH    101
#define ID_WARM_INK  102
#define ID_ORIENT    110         /* + segment */
#define ID_TEXT      120         /* + segment */
#define ID_FONTSIZE  125
#define ID_VOLUME    130
#define ID_CLICK     131
#define ID_TEST      132
#define ID_OSK       140
#define ID_PTR_DEC   141
#define ID_PTR_INC   142
#define ID_NATURAL   143
#define ID_REFRESH   150

/* A warmth step on the P4 redraws the whole screen (about 100 ms): while a
 * finger drags the slider it is applied at most this often, and the last
 * value always when the finger lifts. */
#define WARMTH_DRAG_MS 150u

/* single task: views (drivers/view.h) -- every event and every draw; the
 * shell reaches it through view_call() only. */
static struct {
    bool       active;
    uint8_t    slot;
    int        w, h;
    uint32_t   seq;
    ugui_ctx_t ui;
    int        section;
    bool       page_open;       /* narrow: the section shown, not the list */
    bool       wide;            /* as last drawn: the list beside the page */
    int        scroll, content_h;
    ugui_rect_t page;           /* the page's viewport, as last drawn */
    ugui_rect_t sliders[4];     /* brightness, warmth, volume, font_size: their rects as last drawn */
    int        warm_drag;       /* the value under a dragging finger, -1: none */
    uint64_t   warm_ms;         /* when warmth was last applied */
    int        press_x, press_y;
    unsigned   told;            /* WIN_EV_SETTINGS received, for `settings view` */
    uint32_t   told_last;       /* the last one's setting index */
    char       storage[384];    /* Storage's lines, read on entering it (statfs can take a while) */
    char       about[384];      /* /proc/board's text */
} s_st BULK_BSS;

static canvas1_t *st_backing(void) {
#if defined(LCDTERM_PRESENT)
    canvas1_t *back = lcdterm_get_reader_backing(s_st.slot);
    if (back) return back;
#endif
    return ramscreen_get_reader_backing(s_st.slot);
}

/* --- Drawing ----------------------------------------------------------------- */

/* A label on the left of r and its value right-aligned. */
static void info_row(const ugui_surface_t *s, ugui_rect_t r, const char *label, const char *value) {
    int lh = ugui_serif_line_h(s);
    int ty = r.y + (r.h - lh) / 2;
    ugui_text_serif(s, r.x, ty, label, (uint32_t)strlen(label), false, false, false);
    if (!value || !*value) return;
    uint32_t vl = (uint32_t)strlen(value);
    int vw = ugui_text_serif_width(s, value, vl);
    int vx = r.x + r.w - vw;
    if (vx < r.x + r.w / 3) vx = r.x + r.w / 3;
    ugui_text_serif(s, vx, ty, value, vl, true, false, false);
}

/* A label over a slider, the value right of the label. */
static ugui_rect_t slider_row(const ugui_surface_t *s, ugui_rect_t *col, const char *label, const char *value,
                              int v, int min, int max, uint16_t id, uint8_t disabled) {
    const int sc = s->scale;
    info_row(s, ugui_cut_top(col, 24 * sc), label, value);
    ugui_rect_t r = ugui_cut_top(col, 30 * sc);
    ugui_disable_next(&s_st.ui, disabled);
    ugui_slider(s, &s_st.ui, r, v, min, max, id);
    (void)ugui_cut_top(col, 10 * sc);
    return r;
}

static void gap(ugui_rect_t *col, int px) { (void)ugui_cut_top(col, px); }

static void header(const ugui_surface_t *s, ugui_rect_t *col, const char *title) {
    const int sc = s->scale;
    ugui_rect_t h = ugui_cut_top(col, 26 * sc);
    ugui_section_header(s, h.x, h.y, h.w, title);
}

static void page_display(const ugui_surface_t *s, ugui_rect_t *col) {
    const int sc = s->scale;
    char v[24];
    header(s, col, "BRIGHTNESS");
    int b = settings_get("display.brightness");
    ksnprintf(v, sizeof v, "%d%%", b);
    s_st.sliders[0] = slider_row(s, col, "Backlight", v, b, 10, 100, ID_BRIGHT, 0);

    header(s, col, "PAPER");
    int w = s_st.warm_drag >= 0 ? s_st.warm_drag : settings_get("display.warmth");
    if (w == 0) ksnprintf(v, sizeof v, "white");
    else ksnprintf(v, sizeof v, "%d of %d", w, PAPER_WARMTH_MAX);
    s_st.sliders[1] = slider_row(s, col, "Warmth", v, w, 0, PAPER_WARMTH_MAX, ID_WARMTH, 0);
    ugui_toggle(s, &s_st.ui, ugui_cut_top(col, 34 * sc), "Warm ink with the paper",
                settings_get("display.warm_ink") == 1, ID_WARM_INK);
    gap(col, 10 * sc);

    header(s, col, "TEXT SIZE");
    static const char *const k_size[3] = { "Small", "Medium", "Large" };
    ugui_segmented(s, &s_st.ui, ugui_cut_top(col, 30 * sc), k_size, 3, settings_get("ui.text_size"), ID_TEXT);
#if defined(CONFIG_PSRAM_BYTES) || defined(CONFIG_BOARD_VISIONFIVE2)
    {
        int fpx = settings_get("ui.font_size");
        int ts = settings_get("ui.text_size");
#if defined(CONFIG_FONT_CELL_W) && (CONFIG_FONT_CELL_W == 16)
        int def_px = (ts == 0) ? 24 : (ts == 2) ? 40 : 32;
#else
        int def_px = (ts == 0) ? 13 : (ts == 2) ? 24 : 16;
#endif
        int eff = (fpx >= 16) ? fpx : def_px;
        if (fpx >= 16) ksnprintf(v, sizeof v, "%d px", eff);
        else ksnprintf(v, sizeof v, "%d px (preset)", eff);
        gap(col, 6 * sc);
        s_st.sliders[3] = slider_row(s, col, "Reader font", v, eff < 16 ? 16 : eff, 16, 64, ID_FONTSIZE, 0);
    }
#endif
    gap(col, 10 * sc);
#if defined(CONFIG_BOARD_TAB5)
    header(s, col, "ORIENTATION");
    static const char *const k_orient[3] = { "Landscape", "Portrait", "Auto" };
    ugui_segmented(s, &s_st.ui, ugui_cut_top(col, 30 * sc), k_orient, 3, settings_get("display.orient"), ID_ORIENT);
    gap(col, 10 * sc);
#endif
}

static void page_sound(const ugui_surface_t *s, ugui_rect_t *col) {
    const int sc = s->scale;
    char v[24];
    header(s, col, "VOLUME");
#if defined(CONFIG_BOARD_ESP32P4)
    int vol = settings_get("audio.volume");
    ksnprintf(v, sizeof v, "%d%%", vol);
    s_st.sliders[2] = slider_row(s, col, "Speaker", v, vol, 0, 100, ID_VOLUME, 0);
#elif defined(CONFIG_BOARD_VISIONFIVE2)
    int vol = settings_get("audio.volume");
    ksnprintf(v, sizeof v, "%d%%", vol);
    s_st.sliders[2] = slider_row(s, col, "Headphones", v, vol, 0, 100, ID_VOLUME, 0);
#else
    (void)v;
    s_st.sliders[2] = slider_row(s, col, "Speaker", "none on this board", 0, 0, 100, ID_VOLUME, 1);
#endif
    ugui_toggle(s, &s_st.ui, ugui_cut_top(col, 34 * sc), "Click on each tap", settings_get("ui.click_sound") == 1, ID_CLICK);
    gap(col, 10 * sc);
    ugui_rect_t row = ugui_cut_top(col, 32 * sc);
#if !defined(CONFIG_BOARD_ESP32P4) && !defined(CONFIG_BOARD_VISIONFIVE2)
    ugui_disable_next(&s_st.ui, 1);
#endif
    ugui_button(s, &s_st.ui, (ugui_rect_t){ row.x, row.y, (int16_t)(120 * sc), row.h }, "Test sound", ID_TEST, false);
    gap(col, 10 * sc);
}

static void page_input(const ugui_surface_t *s, ugui_rect_t *col) {
    const int sc = s->scale;
    uint32_t m = console_input_mode();
    header(s, col, "ATTACHED");
    info_row(s, ugui_cut_top(col, 24 * sc), "Keyboard", (m & WIN_INPUT_KEYBOARD) ? "connected" : "none");
    info_row(s, ugui_cut_top(col, 24 * sc), "Mouse", (m & WIN_INPUT_POINTER) ? "connected" : "none");
    info_row(s, ugui_cut_top(col, 24 * sc), "Touch", (m & WIN_INPUT_TOUCH) ? "yes" : "no");
    gap(col, 10 * sc);

    header(s, col, "POINTER");
    char v[16];
    int sp = settings_get("input.pointer_speed");
    ksnprintf(v, sizeof v, "%d%%", sp);
    const setting_def_t *d = settings_find("input.pointer_speed");
    ugui_disable_next(&s_st.ui, (uint8_t)((d && sp <= d->min ? 1u : 0u) | (d && sp >= d->max ? 2u : 0u)));
    ugui_stepper(s, &s_st.ui, ugui_cut_top(col, 30 * sc), "Speed", v, ID_PTR_DEC, ID_PTR_INC);
    gap(col, 8 * sc);
    ugui_toggle(s, &s_st.ui, ugui_cut_top(col, 34 * sc), "Natural scrolling", settings_get("input.natural_scroll") == 1,
                ID_NATURAL);
    gap(col, 10 * sc);

    header(s, col, "ON-SCREEN KEYBOARD");
    ugui_toggle(s, &s_st.ui, ugui_cut_top(col, 34 * sc), "Show the key pressed", settings_get("ui.osk_preview") == 1, ID_OSK);
    gap(col, 10 * sc);
}

static void page_power(const ugui_surface_t *s, ugui_rect_t *col) {
    const int sc = s->scale;
    header(s, col, "BATTERY");
    /* accepted: the power task updates the battery once a second; a value
     * read halfway through an update is one wrong line until the next draw. */
    battery_t *b = battery_board();
    int32_t pm = 0;
    if (!b || !battery_soc(b, &pm)) {
        info_row(s, ugui_cut_top(col, 24 * sc), "Battery", "none on this board");
        return;
    }
    char v[32];
    ksnprintf(v, sizeof v, "%ld%%", (long)((pm + 5) / 10));
    info_row(s, ugui_cut_top(col, 24 * sc), "Charge", v);
    info_row(s, ugui_cut_top(col, 24 * sc), "Charger", battery_board_charging() ? "charging" : b->low ? "low" : "not charging");
    int32_t mins = battery_minutes_left(b);
    if (mins > 0) {
        ksnprintf(v, sizeof v, "%ld h %02ld min", (long)(mins / 60), (long)(mins % 60));
        info_row(s, ugui_cut_top(col, 24 * sc), "Time left", v);
    }
    gap(col, 10 * sc);
    header(s, col, "MEASURED");
    ksnprintf(v, sizeof v, "%ld.%03ld V", (long)(b->last_mv / 1000), (long)(b->last_mv % 1000));
    info_row(s, ugui_cut_top(col, 24 * sc), "Pack voltage", v);
    ksnprintf(v, sizeof v, "%ld mA", (long)b->last_ma);
    info_row(s, ugui_cut_top(col, 24 * sc), "Current (+ in)", v);
    int32_t mw = (int32_t)((int64_t)b->last_mv * b->last_ma / 1000);
    ksnprintf(v, sizeof v, "%ld mW", (long)mw);
    info_row(s, ugui_cut_top(col, 24 * sc), "Power", v);
}

static void page_network(const ugui_surface_t *s, ugui_rect_t *col) {
    const int sc = s->scale;
    char name[16] = "", ip[20] = "";
    bool configured = false, up = false;
    net_lock();
    const net_state_t *ns = net_state();
    if (ns && ns->nif) {
        strncpy(name, ns->nif->name, sizeof name - 1);
        up = netif_link_up(ns->nif);
        configured = ns->configured;
        ksnprintf(ip, sizeof ip, "%u.%u.%u.%u", ns->ip[0], ns->ip[1], ns->ip[2], ns->ip[3]);
    }
    net_unlock();
    header(s, col, "NETWORK");
    if (!name[0]) {
        info_row(s, ugui_cut_top(col, 24 * sc), "Interface", "none");
        return;
    }
    info_row(s, ugui_cut_top(col, 24 * sc), "Interface", strcmp(name, "wlan0") == 0 ? "Wi-Fi (wlan0)" : name);
    info_row(s, ugui_cut_top(col, 24 * sc), "Link", up ? "up" : "down");
    info_row(s, ugui_cut_top(col, 24 * sc), "Address", configured ? ip : "not configured");
}

/* Lines of text, one row each ("label<TAB>value" or plain). */
static void text_rows(const ugui_surface_t *s, ugui_rect_t *col, const char *text) {
    const int sc = s->scale;
    char line[96];
    while (text && *text) {
        unsigned n = 0;
        while (text[n] && text[n] != '\n') n++;
        unsigned c = n < sizeof line - 1 ? n : (unsigned)sizeof line - 1;
        memcpy(line, text, c);
        line[c] = '\0';
        char *tab = strchr(line, '\t');
        if (tab) *tab = '\0';
        if (line[0]) info_row(s, ugui_cut_top(col, 24 * sc), line, tab ? tab + 1 : "");
        text += n;
        if (*text) text++;
    }
}

static void page_storage(const ugui_surface_t *s, ugui_rect_t *col) {
    const int sc = s->scale;
    ugui_rect_t h = ugui_cut_top(col, 26 * sc);
    ugui_rect_t btn = ugui_cut_right(&h, 80 * sc);
    ugui_section_header(s, h.x, h.y, h.w - 6 * sc, "VOLUMES");
    /* Inside its row: the page is clipped at its top, where this row is. */
    ugui_toolbar_button(s, &s_st.ui, (ugui_rect_t){ btn.x, h.y, btn.w, (int16_t)(20 * sc) }, "Refresh", ID_REFRESH, false);
    text_rows(s, col, s_st.storage[0] ? s_st.storage : "none mounted");
}

static void page_about(const ugui_surface_t *s, ugui_rect_t *col) {
    header(s, col, "THIS MACHINE");
    text_rows(s, col, s_st.about);
}

/* blocks of 512 bytes as text in a unit that keeps two or three digits:
 * KB below 10 MB, MB below 10 GB, GB above. */
static void size_text(uint32_t blocks, char *out, unsigned cap) {
    uint64_t kb = (uint64_t)blocks / 2u;
    if (kb < 10u * 1024u) ksnprintf(out, cap, "%lu KB", (unsigned long)kb);
    else if (kb < 10u * 1024u * 1024u) ksnprintf(out, cap, "%lu MB", (unsigned long)(kb / 1024u));
    else ksnprintf(out, cap, "%lu.%lu GB", (unsigned long)(kb / (1024u * 1024u)),
                   (unsigned long)(kb * 10u / (1024u * 1024u) % 10u));
}

/* /proc/df's sizes, in 512-byte blocks, as "volume<TAB>X free of Y". */
static void read_storage(void) {
    char df[512];
    int n = vfs_read("/proc/df", df, sizeof df - 1);
    s_st.storage[0] = '\0';
    if (n <= 0) return;
    df[n] = '\0';
    unsigned o = 0;
    const char *p = strchr(df, '\n');                    /* past the header */
    while (p && *++p) {
        char vol[16];
        unsigned k = 0;
        while (*p == '/' ) p++;
        while (*p && *p != '/' && *p != ' ' && k + 1 < sizeof vol) vol[k++] = *p++;
        vol[k] = '\0';
        uint32_t num[3] = { 0, 0, 0 };
        for (int f = 0; f < 3; f++) {
            while (*p && (*p < '0' || *p > '9') && *p != '\n') p++;
            while (*p >= '0' && *p <= '9') num[f] = num[f] * 10u + (uint32_t)(*p++ - '0');
        }
        if (vol[0] && o < sizeof s_st.storage) {
            char fr[16], tot[16];
            size_text(num[2], fr, sizeof fr);
            size_text(num[0], tot, sizeof tot);
            o += (unsigned)ksnprintf(s_st.storage + o, sizeof s_st.storage - o, "/%s\t%s free of %s\n", vol, fr, tot);
        }
        p = strchr(p, '\n');
    }
    if (o >= sizeof s_st.storage) s_st.storage[sizeof s_st.storage - 1] = '\0';
}

/* /proc/board's key=value lines, as rows. */
static void read_about(void) {
    char b[384];
    uint32_t n = board_info_text(b, sizeof b);
    if (n >= sizeof b) n = sizeof b - 1;
    b[n] = '\0';
    unsigned o = 0;
    for (char *p = b; *p && o < sizeof s_st.about;) {
        char *e = strchr(p, '\n');
        if (e) *e = '\0';
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            o += (unsigned)ksnprintf(s_st.about + o, sizeof s_st.about - o, "%s\t%s\n", p, eq + 1);
        }
        if (!e) break;
        p = e + 1;
    }
    if (o >= sizeof s_st.about) s_st.about[sizeof s_st.about - 1] = '\0';
}

static void enter_section(int sec) {
    s_st.section = sec;
    s_st.scroll = 0;
    if (sec == SEC_STORAGE) read_storage();
    if (sec == SEC_ABOUT && !s_st.about[0]) read_about();
}

static void draw_page(const ugui_surface_t *s, ugui_rect_t page) {
    s_st.page = page;
    ugui_surface_t ps = ugui_clip(s, page);
    ugui_rect_t col = { .x = page.x, .y = (int16_t)(page.y - s_st.scroll), .w = page.w, .h = 0x3fff };
    int top = col.y;
    switch (s_st.section) {
    case SEC_DISPLAY: page_display(&ps, &col); break;
    case SEC_SOUND:   page_sound(&ps, &col); break;
    case SEC_INPUT:   page_input(&ps, &col); break;
    case SEC_POWER:   page_power(&ps, &col); break;
    case SEC_NETWORK: page_network(&ps, &col); break;
    case SEC_STORAGE: page_storage(&ps, &col); break;
    default:          page_about(&ps, &col); break;
    }
    s_st.content_h = col.y - top;
    int most = s_st.content_h - page.h;
    if (most > 0) {
        const int sc = s->scale;
        ugui_rect_t track = { (int16_t)(page.x + page.w + 4 * sc), page.y, (int16_t)(8 * sc), page.h };
        ugui_scrollbar(s, track, s_st.scroll, s_st.content_h, page.h);
        ugui_add_drag_hit(&s_st.ui, track, ID_SCROLLBAR);
    }
}

static void st_draw(void) {
    canvas1_t *back = st_backing();
    if (!s_st.active || !back || s_st.w <= 0 || s_st.h <= 0) return;
    const int sc = CHROME_SCALE;
    uint64_t t_draw = time_get_us();
    ugui_surface_t surf = { .c1 = back, .w = s_st.w, .h = s_st.h, .scale = sc };
    ugui_ctx_t *ctx = &s_st.ui;
    ugui_begin(ctx);
    ugui_set_min_target(ctx, ugui_target_px());
    ugui_fill(&surf, (ugui_rect_t){ 0, 0, (int16_t)s_st.w, (int16_t)s_st.h }, UGUI_GRAY_PAPER, CANVAS1_WHITE);

    ugui_rect_t area = ugui_inset((ugui_rect_t){ 0, 0, (int16_t)s_st.w, (int16_t)s_st.h }, 12 * sc, 10 * sc);
    int side_w = 150 * sc, row_h = 32 * sc, sb_w = 14 * sc;
    s_st.wide = area.w >= side_w + 12 * sc + 260 * sc;
    if (s_st.wide) s_st.page_open = false;

    if (s_st.wide || !s_st.page_open) {
        ugui_rect_t side = s_st.wide ? ugui_cut_left(&area, side_w) : area;
        if (s_st.wide) (void)ugui_cut_left(&area, 12 * sc);
        for (int i = 0; i < SEC_COUNT; i++) {
            ugui_rect_t r = ugui_cut_top(&side, row_h);
            if (r.h < row_h) break;
            ugui_list_row(&surf, ctx, r, NULL, k_sections[i], s_st.wide ? NULL : ">", ID_SECTION, (int16_t)i,
                          s_st.wide && i == s_st.section);
            (void)ugui_cut_top(&side, 2 * sc);
        }
    }
    if (s_st.wide || s_st.page_open) {
        if (!s_st.wide) {
            ugui_rect_t bar = ugui_cut_top(&area, 28 * sc);
            ugui_toolbar_button(&surf, ctx, (ugui_rect_t){ bar.x, bar.y, (int16_t)(70 * sc), (int16_t)(22 * sc) }, "< Back",
                                ID_BACK, false);
            const char *t = k_sections[s_st.section];
            ugui_text_serif(&surf, bar.x + 80 * sc, bar.y + 2 * sc, t, (uint32_t)strlen(t), true, false, false);
            (void)ugui_cut_top(&area, 6 * sc);
        }
        (void)ugui_cut_right(&area, sb_w);
        draw_page(&surf, area);
    }
    ugui_end(&surf, ctx);
    s_st.seq++;
    uint64_t t_copy = time_get_us();
    uint8_t req[2] = { 'W', s_st.slot }, reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 2, reply);
    uxbench_view_note(t_draw, t_draw, t_copy, time_get_us());
}

/* --- Input ------------------------------------------------------------------ */

static void scroll_to(int px) {
    int most = s_st.content_h - s_st.page.h;
    if (px > most) px = most;
    if (px < 0) px = 0;
    if (px == s_st.scroll) return;
    s_st.scroll = px;
    st_draw();
}

static void warmth_apply(int v, bool final) {
    uint64_t now = time_get_ms();
    if (final || now - s_st.warm_ms >= WARMTH_DRAG_MS) {
        s_st.warm_ms = now;
        (void)settings_set("display.warmth", v);
    }
}

/* Slider id moved to x (a finger or the mouse); final: it let go. */
static void slider_to(uint16_t id, int x, bool final) {
    ugui_surface_t ss = { .scale = CHROME_SCALE };
    if (id == ID_BRIGHT) {
        (void)settings_set("display.brightness", ugui_slider_value(&ss, s_st.sliders[0], x, 10, 100));
    } else if (id == ID_WARMTH) {
        int v = ugui_slider_value(&ss, s_st.sliders[1], x, 0, PAPER_WARMTH_MAX);
        s_st.warm_drag = final ? -1 : v;
        warmth_apply(v, final);
    } else if (id == ID_VOLUME) {
        (void)settings_set("audio.volume", ugui_slider_value(&ss, s_st.sliders[2], x, 0, 100));
    } else if (id == ID_FONTSIZE) {
        (void)settings_set("ui.font_size", ugui_slider_value(&ss, s_st.sliders[3], x, 16, 64));
    } else {
        return;
    }
    st_draw();
}

/* A slider's key step: Left / Right on its focus. */
static bool slider_key(uint16_t id, uint32_t key) {
    int dir = key == KEY_RIGHT ? 1 : -1;
    if (id == ID_BRIGHT) return settings_set("display.brightness", settings_get("display.brightness") + 10 * dir);
    if (id == ID_WARMTH) return settings_set("display.warmth", settings_get("display.warmth") + dir);
    if (id == ID_VOLUME) return settings_set("audio.volume", settings_get("audio.volume") + 10 * dir);
    if (id == ID_FONTSIZE) {
        int cur = settings_get("ui.font_size");
        if (cur < 16) cur = 32;
        int next = cur + 2 * dir;
        if (next < 16) next = 16;
        if (next > 64) next = 64;
        return settings_set("ui.font_size", next);
    }
    return false;
}

static void toggle(const char *key) {
    (void)settings_set(key, settings_get(key) == 1 ? 0 : 1);
}

/* A widget acted on: a tap at x (Enter on the focus: x < 0). */
static void st_act(uint16_t id, int16_t idx, int x) {
    if (id == ID_SECTION && idx >= 0 && idx < SEC_COUNT) {
        enter_section(idx);
        if (!s_st.wide) {
            s_st.page_open = true;
            s_st.ui.focus_id = 0;                        /* the page's first widget next */
        }
    } else if (id == ID_BACK) {
        s_st.page_open = false;
        ugui_set_focus(&s_st.ui, ID_SECTION, (int16_t)s_st.section);
    } else if (id == ID_BRIGHT || id == ID_WARMTH || id == ID_VOLUME || id == ID_FONTSIZE) {
        if (x >= 0) { slider_to(id, x, true); return; }
    } else if (id == ID_WARM_INK) {
        toggle("display.warm_ink");
    } else if (id >= ID_TEXT && id < ID_TEXT + 3) {
        (void)settings_set("ui.font_size", 0);
        (void)settings_set("ui.text_size", id - ID_TEXT);
    } else if (id >= ID_ORIENT && id < ID_ORIENT + 3) {
        (void)settings_set("display.orient", id - ID_ORIENT);
    } else if (id == ID_CLICK) {
        toggle("ui.click_sound");
    } else if (id == ID_TEST) {
        ui_click_now();
    } else if (id == ID_OSK) {
        toggle("ui.osk_preview");
    } else if (id == ID_NATURAL) {
        toggle("input.natural_scroll");
    } else if (id == ID_PTR_DEC || id == ID_PTR_INC) {
        (void)settings_set("input.pointer_speed", settings_get("input.pointer_speed") + (id == ID_PTR_INC ? 25 : -25));
    } else if (id == ID_REFRESH) {
        read_storage();
    } else {
        return;
    }
    st_draw();
}

/* The scrollbar pressed or dragged at y: the thumb's middle there. */
static void scrollbar_to(int y) {
    int range = s_st.content_h - s_st.page.h;
    if (range <= 0 || s_st.page.h <= 0) return;
    scroll_to((int)((long)(y - s_st.page.y) * s_st.content_h / s_st.page.h) - s_st.page.h / 2);
}

static void st_key(uint32_t key, uint8_t mods) {
    ugui_ctx_t *ctx = &s_st.ui;
    if (ctx->focus_shown && (key == KEY_LEFT || key == KEY_RIGHT) && slider_key(ctx->focus_id, key)) {
        st_draw();
        return;
    }
    if (key == KEY_PGDN || key == KEY_PGUP) {
        scroll_to(s_st.scroll + (key == KEY_PGDN ? 1 : -1) * (s_st.page.h * 3 / 4));
        return;
    }
    uint16_t id = 0;
    int16_t idx = 0;
    switch (ugui_nav(ctx, key, mods, &id, &idx)) {
    case UGUI_NAV_MOVED:
        st_draw();
        return;
    case UGUI_NAV_ACTIVATE: {
        uint32_t seq = s_st.seq;
        st_act(id, idx, -1);
        if (s_st.seq == seq) st_draw();
        return;
    }
    case UGUI_NAV_BACK:
        if (!s_st.wide && s_st.page_open) st_act(ID_BACK, 0, -1);
        return;
    case UGUI_NAV_EDGE:
        if (key == KEY_UP || key == KEY_DOWN) scroll_to(s_st.scroll + (key == KEY_DOWN ? 1 : -1) * 40 * CHROME_SCALE);
        return;
    default:
        return;
    }
}

/* --- The view ---------------------------------------------------------------- */

static bool st_active(uint8_t inst) { return s_st.active && inst == s_st.slot; }

static bool st_is_slider(uint16_t id) { return id == ID_BRIGHT || id == ID_WARMTH || id == ID_VOLUME || id == ID_FONTSIZE; }

static void st_event(uint8_t inst, const win_event_t *ev) {
    if (!st_active(inst)) return;
    ugui_ctx_t *ctx = &s_st.ui;
    switch (ev->type) {
    case WIN_EV_POINTER_DOWN:
        if (ev->button == 1 && (ev->flags & WIN_EVF_PRESS)) {
            s_st.press_x = ev->x;
            s_st.press_y = ev->y;
            bool changed = ugui_arm(ctx, ev->x, ev->y);
            if (st_is_slider(ctx->armed_id)) slider_to(ctx->armed_id, ev->x, false);
            else if (ctx->armed_id == ID_SCROLLBAR) scrollbar_to(ev->y);
            else if (changed) st_draw();
        }
        return;
    case WIN_EV_POINTER_UP: {
        if (ev->button == 2) return;
        uint16_t armed = ctx->armed_id;
        if (ev->flags & WIN_EVF_TAP) {
            bool was_armed = ugui_disarm(ctx);
            uint32_t seq = s_st.seq;
            int16_t idx = 0;
            uint16_t id = ugui_hit_test(ctx, ev->x, ev->y, &idx);
            if (id) st_act(id, idx, ev->x);
            if (was_armed && s_st.active && s_st.seq == seq) st_draw();
        } else if (ev->flags & WIN_EVF_PRESS) {
            bool was = ugui_disarm(ctx);
            if (armed == ID_WARMTH && s_st.warm_drag >= 0) {
                int v = s_st.warm_drag;
                s_st.warm_drag = -1;
                warmth_apply(v, true);                   /* the last value, always */
                st_draw();
            } else if (was) {
                st_draw();
            }
        }
        return;
    }
    case WIN_EV_POINTER_MOVE:
        if (ev->button == 0) {
            if (ugui_hover(ctx, ev->x, ev->y)) st_draw();
        } else if (st_is_slider(ctx->armed_id)) {
            slider_to(ctx->armed_id, ev->x, false);
        } else if (ctx->armed_id == ID_SCROLLBAR) {
            scrollbar_to(ev->y);
        }
        return;
    case WIN_EV_DRAG:                                    /* the page follows the finger */
        if (ugui_disarm(ctx)) st_draw();
        scroll_to(s_st.scroll - ev->dy);
        return;
    case WIN_EV_SCROLL: {
        int notches = ev->dy < 0 ? -ev->dy : ev->dy;
        if (!(ev->flags & WIN_EVF_WHEEL)) notches = 3;
        if (notches > 4) notches = 4;
        scroll_to(s_st.scroll + (ev->dy > 0 ? -1 : 1) * notches * 32 * CHROME_SCALE);
        return;
    }
    case WIN_EV_KEY_DOWN:
        st_key(ev->key, ev->mods);
        return;
    case WIN_EV_SETTINGS:                                /* from here or elsewhere: show it */
        s_st.told++;
        s_st.told_last = ev->key;
        if (s_st.warm_drag < 0) st_draw();
        return;
    case WIN_EV_POWER:
    case WIN_EV_INPUT_MODE:
        if (s_st.warm_drag < 0) st_draw();
        return;
    default:
        return;
    }
}

static bool st_captures(uint8_t inst, int x, int y) {
    /* accepted: read on the touch path while the views task may draw, as
     * the launcher's (a bounded array; a stale answer costs one contact). */
    return st_active(inst) && ugui_drag_at(&s_st.ui, x, y);
}

static void st_resize(uint8_t inst, int w, int h) {
    if (!st_active(inst) || (w == s_st.w && h == s_st.h)) return;
    s_st.w = w;
    s_st.h = h;
    st_draw();
}

static void st_close(uint8_t inst) {
    if (st_active(inst)) s_st.active = false;
}

static uint8_t *st_surface(uint8_t inst, uint32_t *stride, int *w, int *h) {
    (void)inst; (void)stride; (void)w; (void)h;
    return NULL;                                         /* 1-bpp: the compositor converts it */
}

static uint32_t st_seq(uint8_t inst) {
    (void)inst;
    return s_st.seq;
}

const view_ops_t g_settings_view = {
    .name = "settings",
    .active = st_active,
    .event = st_event,
    .captures = st_captures,
    .resize = st_resize,
    .close = st_close,
    .surface = st_surface,
    .seq = st_seq,
};

/* --- From other tasks ------------------------------------------------------------ */

typedef struct { int op; int section; bool ok; } st_call_t;

static void st_report_now(void);

static void st_call(void *arg) {
    st_call_t *c = arg;
    if (c->op == 1) { st_report_now(); return; }
    static const uint8_t k_title[] = "Settings";
    uint8_t req[3 + sizeof k_title] = { 'v', RIBBON_WIN_SETTINGS, SCREEN_VIEW_SINGLETON };
    memcpy(req + 3, k_title, sizeof k_title);
    uint8_t reply[SCREEN_REPLY_LEN];
    if (!console_canvas(req, sizeof req, reply) || reply[0] != 0) { c->ok = false; return; }
    bool fresh = !s_st.active || s_st.slot != reply[1];
    if (fresh) {                                         /* a new window, at the section last left */
        memset(&s_st.ui, 0, sizeof s_st.ui);
        s_st.page_open = false;
        s_st.warm_drag = -1;
        s_st.told = 0;
    }
    if (c->section >= 0 && c->section < SEC_COUNT) {     /* asked for one: shown, in a narrow window too */
        enter_section(c->section);
        s_st.page_open = true;
        s_st.ui.focus_id = 0;
    } else if (fresh) {
        enter_section(s_st.section >= 0 && s_st.section < SEC_COUNT ? s_st.section : SEC_DISPLAY);
    }
    s_st.active = true;
    s_st.slot = reply[1];
    s_st.w = (int)(reply[2] | (reply[3] << 8));
    s_st.h = (int)(reply[4] | (reply[5] << 8));
    console_set_active_win_type(RIBBON_WIN_SETTINGS);
    st_draw();
    c->ok = true;
}

bool settings_view_open_at(int section) {
    st_call_t c = { 0, section, false };
    view_call(st_call, &c);
    return c.ok;
}

bool settings_view_open(void) {
    return settings_view_open_at(-1);
}

/* The report, written on the views task and printed by the caller after
 * view_call() returns (view_call()'s fn would print to the views task's
 * console, not the shell's). single task: views writes, then the one
 * waiting caller reads. */
static char s_report[2048] BULK_BSS;

static void st_report_now(void) {
    unsigned o = 0;
    char *b = s_report;
    unsigned cap = sizeof s_report;
    o += (unsigned)ksnprintf(b + o, cap - o, "settings view: %s, slot %u, %d x %d, %s, section %s%s\n",
                             s_st.active ? "open" : "closed", (unsigned)s_st.slot, s_st.w, s_st.h,
                             s_st.wide ? "wide" : "narrow", k_sections[s_st.section],
                             !s_st.wide && !s_st.page_open ? " (list shown)" : "");
    const setting_def_t *last = settings_at(s_st.told_last);
    o += (unsigned)ksnprintf(b + o, cap - o, "  told %u setting change%s%s%s\n", s_st.told, s_st.told == 1 ? "" : "s",
                             s_st.told && last ? ", the last " : "", s_st.told && last ? last->key : "");
    for (int i = 0; i < s_st.ui.count && o < cap; i++) {
        const ugui_hit_t *h = &s_st.ui.hits[i];
        o += (unsigned)ksnprintf(b + o, cap - o, "  hit id %u index %d: x %d..%d y %d..%d%s\n", (unsigned)h->id,
                                 (int)h->index, (int)h->rect.x, (int)(h->rect.x + h->rect.w - 1), (int)h->rect.y,
                                 (int)(h->rect.y + h->rect.h - 1), (h->flags & UGUI_HIT_DISABLED) ? " (dimmed)" : "");
    }
    if (o >= cap) b[cap - 1] = '\0';
}

bool settings_view_bench_stepper(bool inc, uint8_t *slot, int *x, int *y) {
    if (!s_st.active) return false;
    for (int i = 0; i < s_st.ui.count; i++) {
        const ugui_hit_t *h = &s_st.ui.hits[i];
        if (h->id != (inc ? ID_PTR_INC : ID_PTR_DEC)) continue;
        *slot = s_st.slot;
        *x = h->rect.x + h->rect.w / 2;
        *y = h->rect.y + h->rect.h / 2;
        return true;
    }
    return false;
}

void settings_view_report(void) {
    st_call_t c = { 1, -1, false };
    view_call(st_call, &c);
    cprintf("%s", s_report);
    uint16_t ink, paper;
    paper_current(&ink, &paper);
    cprintf("  colours: ink 0x%04x paper 0x%04x\n", (unsigned)ink, (unsigned)paper);
}

#else

static bool st_none_active(uint8_t inst) { (void)inst; return false; }
static void st_none_event(uint8_t inst, const win_event_t *ev) { (void)inst; (void)ev; }
static void st_none_resize(uint8_t inst, int w, int h) { (void)inst; (void)w; (void)h; }
static void st_none_close(uint8_t inst) { (void)inst; }
static uint8_t *st_none_surface(uint8_t inst, uint32_t *s, int *w, int *h) { (void)inst; (void)s; (void)w; (void)h; return 0; }
static uint32_t st_none_seq(uint8_t inst) { (void)inst; return 0; }
const view_ops_t g_settings_view = { .name = "settings", .active = st_none_active, .event = st_none_event,
                                     .captures = 0, .resize = st_none_resize, .close = st_none_close,
                                     .surface = st_none_surface, .seq = st_none_seq };

bool settings_view_open(void) { return false; }
bool settings_view_bench_stepper(bool inc, uint8_t *slot, int *x, int *y) {
    (void)inc; (void)slot; (void)x; (void)y;
    return false;
}
bool settings_view_open_at(int section) { (void)section; return false; }
void settings_view_report(void) {
    cprintf("settings view: no screen on this board\n");
}

#endif
