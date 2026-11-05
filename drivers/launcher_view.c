/* drivers/launcher_view.c — Classic Mac Touch Launcher Window
 *
 * Implements the touch-first Launcher ribbon window (RIBBON_WIN_LAUNCHER)
 * using the stateless Classic-Mac micro-toolkit (ugui):
 * - Application cards: Reader, Moonbase, Terminal
 * - Document Library browser: scans /sd0/pub, /sd0/help, /flash0/help
 * - Quick Settings: Volume stepper, Brightness stepper, Orientation selector
 * - Freestanding C, zero heap allocations, zero FPU in M-mode.
 */

#include "drivers/launcher_view.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "lugalos_config.h"

/* Headless sensor/gateway/clock boards without an LCD panel have no touch
 * launcher window; keep zero static RAM overhead on those presets. */
#if defined(CONFIG_BOARD_ESP32C6) || (defined(CONFIG_BOARD_RP2350) && !defined(CONFIG_LCD_PCLK_GPIO))

void launcher_init(void) {}
bool launcher_open(void) { return false; }
void launcher_close(void) {}
bool launcher_is_active(void) { return false; }
uint8_t launcher_get_slot_id(void) { return 0; }
bool launcher_set_target(uint8_t slot_id, int w, int h) { (void)slot_id; (void)w; (void)h; return false; }
void launcher_draw(const canvas1_t *c, int w, int h) { (void)c; (void)w; (void)h; }
void launcher_update(void) {}
bool launcher_tap(int client_x, int client_y) { (void)client_x; (void)client_y; return false; }
void launcher_scroll(int delta_y) { (void)delta_y; }
void launcher_feed_key(int c) { (void)c; }
bool launcher_poll_exec(char *out_cmd, size_t max_len) { (void)out_cmd; (void)max_len; return false; }
uint8_t *launcher_get_aa_backing(uint32_t *out_stride, int *out_w, int *out_h) { (void)out_stride; (void)out_w; (void)out_h; return NULL; }
uint32_t launcher_get_seq(void) { return 0; }
void launcher_report(void) { cprintf("launcher: not present on headless board\n"); }

#else

#include "drivers/ugui.h"
#include "drivers/reader_view.h"
#include "drivers/ribbon.h"
#include "drivers/screen.h"
#include "drivers/lcdterm.h"
#include "drivers/ramscreen.h"
#include "drivers/battery.h"
#include "drivers/usbkbd_task.h"
#include "kernel/vterm.h"
#include "fs/vfs.h"
#include "kernel/console.h"
#include "kernel/orient.h"
#include "kernel/palloc.h"
#include <string.h>

#if defined(CONFIG_BOARD_ESP32P4)
#include "drivers/audio_esp32p4.h"
#endif

#if defined(CONFIG_BOARD_ESP32P4) && (defined(CONFIG_DSI_LCD_RST_GPIO) || defined(CONFIG_BOARD_TAB5))
#include "drivers/dsi_esp32p4.h"
#endif

#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_LCD_PCLK_GPIO)
#include "drivers/lcd7.h"
#endif

#define LAUNCHER_MAX_DOCS       24

#define LAUNCHER_ID_APP_READER   1
#define LAUNCHER_ID_APP_CHESS    2
#define LAUNCHER_ID_APP_MOONBASE 3
#define LAUNCHER_ID_APP_TERMINAL 4
#define LAUNCHER_ID_VOL_STEP     10
#define LAUNCHER_ID_BL_STEP      20
#define LAUNCHER_ID_ORIENT_SEG   30
#define LAUNCHER_ID_DOC_REFRESH  40
#define LAUNCHER_ID_DOC_BASE     100

/* Crisp 16x16 1-bpp Classic Mac application icons in .rodata */
static const uint16_t k_icon_reader[16] = {
    0x0000, 0x7FE0, 0x4030, 0x5FF8,
    0x4008, 0x57E8, 0x4008, 0x57E8,
    0x4008, 0x57C8, 0x4008, 0x57E8,
    0x4008, 0x4008, 0x7FF8, 0x0000
};

static const uint16_t k_icon_chess[16] = {
    0x0000, 0x03C0, 0x0FE0, 0x1FE0,
    0x3DE0, 0x3BE0, 0x3C00, 0x3F00,
    0x3FC0, 0x1FF0, 0x0FF8, 0x0FF8,
    0x1FFC, 0x3FFE, 0x3FFE, 0x0000
};

static const uint16_t k_icon_moonbase[16] = {
    0x0180, 0x03C0, 0x07E0, 0x07E0,
    0x0DB0, 0x0FF0, 0x1FF8, 0x1BD8,
    0x3FFC, 0x3C3C, 0x67E6, 0x4242,
    0x05A0, 0x0990, 0x1008, 0x0000
};

static const uint16_t k_icon_terminal[16] = {
    0xFFFF, 0x8001, 0xFFFF, 0x8001,
    0x9001, 0x8801, 0x8401, 0x8801,
    0x9001, 0x8079, 0x8001, 0x8001,
    0x8001, 0x8001, 0xFFFF, 0x0000
};

typedef struct {
    char path[96];
    char label[48];
    char badge[12];
} launcher_doc_t;

typedef struct {
    bool           active;
    uint8_t        slot_id;
    int            win_w;
    int            win_h;
    int            scroll_idx;
    int            visible_docs;
    int            doc_count;
    launcher_doc_t docs[LAUNCHER_MAX_DOCS];
    ugui_ctx_t     ui_ctx;
    uint8_t       *aa_buf;
    uint32_t       aa_stride;
    int            aa_w;
    int            aa_h;
    uint32_t       aa_pages;
    uint32_t       seq;
    char           pending_cmd[64];
} launcher_state_t;

/* guarded by: console_lock for opening, closing, and surface allocation, which
 * the display compositor reads under it. accepted: interactive taps and redraws
 * update the live surface from the input path. */
static launcher_state_t s_launcher BULK_BSS;

static bool str_ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s);
    size_t xl = strlen(suffix);
    if (sl < xl) return false;
    return strcmp(s + sl - xl, suffix) == 0;
}

static const char *find_last_char(const char *s, char ch) {
    const char *last = NULL;
    for (; *s; s++) {
        if (*s == ch) last = s;
    }
    return last;
}

static bool doc_already_listed(const char *basename) {
    for (int i = 0; i < s_launcher.doc_count; i++) {
        const char *p = s_launcher.docs[i].path;
        const char *slash = find_last_char(p, '/');
        const char *b = slash ? (slash + 1) : p;
        if (strcmp(b, basename) == 0) return true;
    }
    return false;
}

static void add_doc_entry(const char *full_path, const char *name, const char *badge) {
    if (s_launcher.doc_count >= LAUNCHER_MAX_DOCS) return;
    if (doc_already_listed(name)) return;
    launcher_doc_t *d = &s_launcher.docs[s_launcher.doc_count++];
    strncpy(d->path, full_path, sizeof(d->path) - 1);
    d->path[sizeof(d->path) - 1] = '\0';

    /* Friendly label: strip extension and capitalize first letter */
    strncpy(d->label, name, sizeof(d->label) - 1);
    d->label[sizeof(d->label) - 1] = '\0';
    for (int i = (int)strlen(d->label) - 1; i >= 0; i--) {
        if (d->label[i] == '.') {
            d->label[i] = '\0';
            break;
        }
    }
    if (d->label[0] >= 'a' && d->label[0] <= 'z') {
        d->label[0] = (char)(d->label[0] - ('a' - 'A'));
    }
    for (int i = 0; d->label[i]; i++) {
        if (d->label[i] == '_') d->label[i] = ' ';
    }

    strncpy(d->badge, badge ? badge : "", sizeof(d->badge) - 1);
    d->badge[sizeof(d->badge) - 1] = '\0';
}

static void scan_dir_docs(const char *dir_path, const char *badge) {
    int fd = vfs_open(dir_path, VFS_O_READ);
    if (fd < 0) return;
    char name[64];
    vfs_stat_t st;
    for (uint32_t idx = 0; idx < 64 && s_launcher.doc_count < LAUNCHER_MAX_DOCS; idx++) {
        if (vfs_readdir(fd, idx, name, sizeof(name), &st) < 0) break;
        if (st.is_dir) continue;
        if (str_ends_with(name, ".md") || str_ends_with(name, ".gmi")) {
            char full[96];
            ksnprintf(full, sizeof(full), "%s/%s", dir_path, name);
            add_doc_entry(full, name, badge);
        }
    }
    vfs_close(fd);
}

static void launcher_scan_docs(void) {
    s_launcher.doc_count = 0;

    /* Curated primary documentation first so new users see key guides at the top */
    static const struct {
        const char *file;
        const char *title;
        const char *badge;
    } k_curated[] = {
        { "index.md",   "Documentation Index", "Manual" },
        { "about.md",   "About LugalOS",       "Manual" },
        { "ribbon.md",  "Ribbon & Touch UX",   "Guide"  },
        { "reader.md",  "Reader & Typography", "Guide"  },
        { "shell.md",   "Shell & Commands",    "Ref"    },
        { "cas.md",     "Computer Algebra",    "Math"   },
        { "chess.md",   "Chess Engine",        "Game"   },
        { "sensors.md", "Sensors & Telemetry", "Ref"    },
    };

    for (unsigned i = 0; i < sizeof(k_curated) / sizeof(k_curated[0]); i++) {
        char path[96];
        ksnprintf(path, sizeof(path), "/sd0/help/%s", k_curated[i].file);
        int fd = vfs_open(path, VFS_O_READ);
        if (fd < 0) {
            ksnprintf(path, sizeof(path), "/flash0/help/%s", k_curated[i].file);
            fd = vfs_open(path, VFS_O_READ);
        }
        if (fd >= 0) {
            vfs_close(fd);
            if (s_launcher.doc_count < LAUNCHER_MAX_DOCS) {
                launcher_doc_t *d = &s_launcher.docs[s_launcher.doc_count++];
                strncpy(d->path, path, sizeof(d->path) - 1);
                d->path[sizeof(d->path) - 1] = '\0';
                strncpy(d->label, k_curated[i].title, sizeof(d->label) - 1);
                d->label[sizeof(d->label) - 1] = '\0';
                strncpy(d->badge, k_curated[i].badge, sizeof(d->badge) - 1);
                d->badge[sizeof(d->badge) - 1] = '\0';
            }
        }
    }

    scan_dir_docs("/sd0/pub", "Pub");
    scan_dir_docs("/sd0/help", "Help");
    scan_dir_docs("/flash0/help", "Flash");
}

static unsigned get_backlight_percent(void) {
#if defined(CONFIG_BOARD_ESP32P4) && (defined(CONFIG_DSI_LCD_RST_GPIO) || defined(CONFIG_BOARD_TAB5))
    return dsi_lcd_get_backlight();
#else
    return 100u;
#endif
}

static void set_backlight_percent(int pct) {
    if (pct < 10) pct = 10;
    if (pct > 100) pct = 100;
#if defined(CONFIG_BOARD_ESP32P4) && (defined(CONFIG_DSI_LCD_RST_GPIO) || defined(CONFIG_BOARD_TAB5))
    dsi_lcd_set_backlight((unsigned)pct);
#elif defined(CONFIG_BOARD_RP2350) && defined(CONFIG_LCD_PCLK_GPIO)
    lcd7_set_backlight((unsigned)pct);
#endif
}

static int get_volume_percent(void) {
#if defined(CONFIG_BOARD_ESP32P4)
    return audio_p4_get_volume();
#else
    return 70;
#endif
}

static void set_volume_percent(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
#if defined(CONFIG_BOARD_ESP32P4)
    audio_p4_set_volume(pct);
#endif
}

#if defined(CONFIG_BOARD_TAB5)
static int get_orient_mode(void) {
    if (orient_is_auto()) return 2;
    int rot = dsi_lcd_get_orientation();
    return (rot == 90 || rot == 270) ? 0 : 1;
}
#endif

static bool focus_window_by_title(const char *prefix) {
    if (!prefix || !*prefix) return false;
    uint8_t rreq[2] = { 'R', 0 }, rreply[SCREEN_REPLY_LEN];
    if (console_canvas(rreq, 1, rreply) && rreply[0] == 0) {
        uint8_t count = rreply[1];
        for (uint8_t i = 0; i < count; i++) {
            rreq[1] = i;
            if (console_canvas(rreq, 2, rreply) && rreply[0] == 0) {
                const char *wtitle = (const char *)rreply + 12;
                if (strncmp(wtitle, prefix, strlen(prefix)) == 0) {
                    uint8_t jreq[2] = { 'J', i };
                    (void)console_canvas(jreq, 2, rreply);
                    if (rreply[10] != 0) {
                        console_set_active_win_type(rreply[10]);
                    } else {
                        console_set_active_win_type(rreply[1]);
                    }
                    vterm_set_active(rreply[1]);
                    return true;
                }
            }
        }
    }
    return false;
}

static void open_doc_in_reader(const char *doc_path) {
    if (!doc_path || !*doc_path) doc_path = "index.md";

    /* Check if a Reader window is already open with this path */
    int existing_rid = reader_find_by_path(doc_path);
    if (existing_rid >= 0) {
        uint8_t rreq[2] = { 'R', 0 }, rreply[SCREEN_REPLY_LEN];
        if (console_canvas(rreq, 1, rreply) && rreply[0] == 0) {
            uint8_t wcount = rreply[1];
            for (uint8_t w = 0; w < wcount; w++) {
                rreq[1] = w;
                if (console_canvas(rreq, 2, rreply) && rreply[0] == 0) {
                    if (rreply[1] == RIBBON_WIN_READER && rreply[2] == (uint8_t)existing_rid) {
                        uint8_t jreq[2] = { 'J', w };
                        (void)console_canvas(jreq, 2, rreply);
                        reader_set_active_instance((uint8_t)existing_rid);
                        console_set_active_win_type(RIBBON_WIN_READER);
                        return;
                    }
                }
            }
        }
    }

    /* Open a new Reader window in the ribbon */
    uint8_t req[128];
    req[0] = 'O';
    req[1] = 0;
    uint32_t len = (uint32_t)strlen(doc_path);
    if (len > sizeof(req) - 3) len = sizeof(req) - 3;
    memcpy(req + 2, doc_path, len);
    req[2 + len] = '\0';
    uint8_t reply[SCREEN_REPLY_LEN];
    if (console_canvas(req, 3 + len, reply) && reply[0] == 0) {
        uint8_t rid = reply[1];
        int w = (int)(reply[2] | (reply[3] << 8));
        int h = (int)(reply[4] | (reply[5] << 8));
        if (reader_open_instance(rid, doc_path) >= 0) {
            reader_set_target(rid, w, h);
            reader_update_instance(rid);
            reader_set_active_instance(rid);
            console_set_active_win_type(RIBBON_WIN_READER);
        }
    }
}

void launcher_init(void) {
    memset(&s_launcher, 0, sizeof(s_launcher));
}

bool launcher_is_active(void) {
    return s_launcher.active;
}

uint8_t launcher_get_slot_id(void) {
    return s_launcher.slot_id;
}

bool launcher_set_target(uint8_t slot_id, int w, int h) {
    bool changed = (s_launcher.slot_id != slot_id || s_launcher.win_w != w || s_launcher.win_h != h);
    s_launcher.slot_id = slot_id;
    s_launcher.win_w = w;
    s_launcher.win_h = h;
    return changed;
}

void launcher_draw(const canvas1_t *c, int w, int h) {
    if ((!c && !s_launcher.aa_buf) || w <= 0 || h <= 0) return;

    const int s = CHROME_SCALE;
    ugui_surface_t surf;
    surf.c1 = c;
    surf.aa_fb = s_launcher.aa_buf;
    surf.aa_stride = s_launcher.aa_stride;
    surf.w = w;
    surf.h = h;
    surf.scale = s;

    ugui_begin(&s_launcher.ui_ctx);
    ugui_fill(&surf, (ugui_rect_t){ 0, 0, (int16_t)w, (int16_t)h }, UGUI_GRAY_PAPER, CANVAS1_WHITE);

    int pad = 12 * s;
    int gap = 10 * s;
    bool landscape = (w >= h);

    if (landscape) {
        /* Two-column layout: Left = Applications + Quick Settings, Right = Document Library */
        int left_w = (w * 46) / 100;
        int right_x = pad + left_w + gap;
        int right_w = w - right_x - pad;

        /* --- Left Column: Applications --- */
        int y = pad;
        ugui_section_header(&surf, pad, y, left_w, "APPLICATIONS");
        y += 24 * s;

        int card_gap = 8 * s;
        int card_w = (left_w - card_gap) / 2;
        int card_h = 56 * s;
        ugui_card(&surf, &s_launcher.ui_ctx,
                  (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)card_w, (int16_t)card_h },
                  k_icon_reader, "Reader", "Docs", LAUNCHER_ID_APP_READER, false);
        ugui_card(&surf, &s_launcher.ui_ctx,
                  (ugui_rect_t){ (int16_t)(pad + card_w + card_gap), (int16_t)y, (int16_t)card_w, (int16_t)card_h },
                  k_icon_chess, "Chess", "Game", LAUNCHER_ID_APP_CHESS, false);
        y += card_h + card_gap;
        ugui_card(&surf, &s_launcher.ui_ctx,
                  (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)card_w, (int16_t)card_h },
                  k_icon_moonbase, "Moonbase", "Game", LAUNCHER_ID_APP_MOONBASE, false);
        ugui_card(&surf, &s_launcher.ui_ctx,
                  (ugui_rect_t){ (int16_t)(pad + card_w + card_gap), (int16_t)y, (int16_t)card_w, (int16_t)card_h },
                  k_icon_terminal, "Terminal", "Shell", LAUNCHER_ID_APP_TERMINAL, false);
        y += card_h + 14 * s;

        /* --- Left Column: Quick Settings --- */
        ugui_section_header(&surf, pad, y, left_w, "QUICK SETTINGS");
        y += 26 * s;

        int row_h = 30 * s;
        char vol_str[16];
        ksnprintf(vol_str, sizeof(vol_str), "%d%%", get_volume_percent());
        ugui_stepper(&surf, &s_launcher.ui_ctx,
                     (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)left_w, (int16_t)row_h },
                     "Volume", vol_str, LAUNCHER_ID_VOL_STEP, LAUNCHER_ID_VOL_STEP + 1);
        y += row_h + 8 * s;

        char bl_str[16];
        ksnprintf(bl_str, sizeof(bl_str), "%u%%", get_backlight_percent());
        ugui_stepper(&surf, &s_launcher.ui_ctx,
                     (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)left_w, (int16_t)row_h },
                     "Bright", bl_str, LAUNCHER_ID_BL_STEP, LAUNCHER_ID_BL_STEP + 1);
        y += row_h + 10 * s;

#if defined(CONFIG_BOARD_TAB5)
        static const char *const k_orient_labels[3] = { "Landscape", "Portrait", "Auto" };
        ugui_segmented(&surf, &s_launcher.ui_ctx,
                       (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)left_w, (int16_t)(28 * s) },
                       k_orient_labels, 3, get_orient_mode(), LAUNCHER_ID_ORIENT_SEG);
        y += 28 * s + 10 * s;
#endif

        /* Status summary box at bottom-left */
        int status_y = h - pad - 20 * s;
        if (status_y > y) {
            char batt[16] = "";
            (void)battery_indicator(batt, sizeof(batt));
            bool sd_ok = vfs_volume_mounted("sd0");
            char status_line[64];
            if (batt[0]) {
                ksnprintf(status_line, sizeof(status_line), "Batt: %s   SD: %s",
                          batt, sd_ok ? "Mounted" : "Absent");
            } else {
                ksnprintf(status_line, sizeof(status_line), "SD: %s   Touch UX Ready",
                          sd_ok ? "Mounted" : "Absent");
            }
            ugui_text_serif(&surf, pad, status_y, status_line, (uint32_t)strlen(status_line),
                            false, false, false);
        }

        /* --- Right Column: Document Library --- */
        int ry = pad;
        int btn_w = 66 * s;
        ugui_section_header(&surf, right_x, ry, right_w - btn_w - 6 * s, "DOCUMENTS");
        ugui_toolbar_button(&surf, &s_launcher.ui_ctx,
                            (ugui_rect_t){ (int16_t)(right_x + right_w - btn_w), (int16_t)(ry - 2 * s), (int16_t)btn_w, (int16_t)(20 * s) },
                            "Refresh", LAUNCHER_ID_DOC_REFRESH, false);
        ry += 24 * s;

        int list_h = h - ry - pad;
        int item_h = 30 * s;
        int sb_w = 10 * s;
        int list_w = right_w - sb_w - 4 * s;
        int vis_rows = list_h / item_h;
        if (vis_rows < 1) vis_rows = 1;
        s_launcher.visible_docs = vis_rows;

        if (s_launcher.scroll_idx > s_launcher.doc_count - vis_rows) {
            s_launcher.scroll_idx = s_launcher.doc_count - vis_rows;
        }
        if (s_launcher.scroll_idx < 0) s_launcher.scroll_idx = 0;

        /* List frame */
        ugui_box_shadow(&surf, (ugui_rect_t){ (int16_t)right_x, (int16_t)ry, (int16_t)list_w, (int16_t)(vis_rows * item_h) }, false);
        for (int i = 0; i < vis_rows; i++) {
            int doc_i = s_launcher.scroll_idx + i;
            if (doc_i >= s_launcher.doc_count) break;
            ugui_list_row(&surf, &s_launcher.ui_ctx,
                          (ugui_rect_t){ (int16_t)(right_x + 2 * s), (int16_t)(ry + i * item_h), (int16_t)(list_w - 4 * s), (int16_t)item_h },
                          NULL, s_launcher.docs[doc_i].label, s_launcher.docs[doc_i].badge,
                          LAUNCHER_ID_DOC_BASE, (int16_t)doc_i, false);
        }

        int total_list_h = s_launcher.doc_count * item_h;
        int view_list_h = vis_rows * item_h;
        if (total_list_h < view_list_h) total_list_h = view_list_h;
        ugui_scrollbar(&surf,
                       (ugui_rect_t){ (int16_t)(right_x + list_w + 4 * s), (int16_t)ry, (int16_t)sb_w, (int16_t)view_list_h },
                       s_launcher.scroll_idx * item_h, total_list_h, view_list_h);
    } else {
        /* Portrait layout: Vertical stack (Applications -> Quick Settings -> Documents) */
        int inner_w = w - 2 * pad;
        int y = pad;
        ugui_section_header(&surf, pad, y, inner_w, "APPLICATIONS");
        y += 24 * s;

        int card_gap = 8 * s;
        int card_w = (inner_w - card_gap) / 2;
        int card_h = 56 * s;
        ugui_card(&surf, &s_launcher.ui_ctx,
                  (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)card_w, (int16_t)card_h },
                  k_icon_reader, "Reader", "Docs", LAUNCHER_ID_APP_READER, false);
        ugui_card(&surf, &s_launcher.ui_ctx,
                  (ugui_rect_t){ (int16_t)(pad + card_w + card_gap), (int16_t)y, (int16_t)card_w, (int16_t)card_h },
                  k_icon_chess, "Chess", "Game", LAUNCHER_ID_APP_CHESS, false);
        y += card_h + card_gap;
        ugui_card(&surf, &s_launcher.ui_ctx,
                  (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)card_w, (int16_t)card_h },
                  k_icon_moonbase, "Moonbase", "Game", LAUNCHER_ID_APP_MOONBASE, false);
        ugui_card(&surf, &s_launcher.ui_ctx,
                  (ugui_rect_t){ (int16_t)(pad + card_w + card_gap), (int16_t)y, (int16_t)card_w, (int16_t)card_h },
                  k_icon_terminal, "Terminal", "Shell", LAUNCHER_ID_APP_TERMINAL, false);
        y += card_h + 12 * s;

        ugui_section_header(&surf, pad, y, inner_w, "QUICK SETTINGS");
        y += 24 * s;

        int row_h = 28 * s;
        char vol_str[16];
        ksnprintf(vol_str, sizeof(vol_str), "%d%%", get_volume_percent());
        ugui_stepper(&surf, &s_launcher.ui_ctx,
                     (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)inner_w, (int16_t)row_h },
                     "Volume", vol_str, LAUNCHER_ID_VOL_STEP, LAUNCHER_ID_VOL_STEP + 1);
        y += row_h + 6 * s;

        char bl_str[16];
        ksnprintf(bl_str, sizeof(bl_str), "%u%%", get_backlight_percent());
        ugui_stepper(&surf, &s_launcher.ui_ctx,
                     (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)inner_w, (int16_t)row_h },
                     "Brightness", bl_str, LAUNCHER_ID_BL_STEP, LAUNCHER_ID_BL_STEP + 1);
        y += row_h + 8 * s;

#if defined(CONFIG_BOARD_TAB5)
        static const char *const k_orient_labels[3] = { "Landscape", "Portrait", "Auto" };
        ugui_segmented(&surf, &s_launcher.ui_ctx,
                       (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)inner_w, (int16_t)(26 * s) },
                       k_orient_labels, 3, get_orient_mode(), LAUNCHER_ID_ORIENT_SEG);
        y += 26 * s + 12 * s;
#endif

        int btn_w = 66 * s;
        ugui_section_header(&surf, pad, y, inner_w - btn_w - 6 * s, "DOCUMENTS");
        ugui_toolbar_button(&surf, &s_launcher.ui_ctx,
                            (ugui_rect_t){ (int16_t)(pad + inner_w - btn_w), (int16_t)(y - 2 * s), (int16_t)btn_w, (int16_t)(20 * s) },
                            "Refresh", LAUNCHER_ID_DOC_REFRESH, false);
        y += 24 * s;

        int footer_h = 20 * s;
        int list_h = h - y - pad - footer_h;
        int item_h = 30 * s;
        int sb_w = 10 * s;
        int list_w = inner_w - sb_w - 4 * s;
        int vis_rows = list_h / item_h;
        if (vis_rows < 1) vis_rows = 1;
        s_launcher.visible_docs = vis_rows;

        if (s_launcher.scroll_idx > s_launcher.doc_count - vis_rows) {
            s_launcher.scroll_idx = s_launcher.doc_count - vis_rows;
        }
        if (s_launcher.scroll_idx < 0) s_launcher.scroll_idx = 0;

        ugui_box_shadow(&surf, (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)list_w, (int16_t)(vis_rows * item_h) }, false);
        for (int i = 0; i < vis_rows; i++) {
            int doc_i = s_launcher.scroll_idx + i;
            if (doc_i >= s_launcher.doc_count) break;
            ugui_list_row(&surf, &s_launcher.ui_ctx,
                          (ugui_rect_t){ (int16_t)(pad + 2 * s), (int16_t)(y + i * item_h), (int16_t)(list_w - 4 * s), (int16_t)item_h },
                          NULL, s_launcher.docs[doc_i].label, s_launcher.docs[doc_i].badge,
                          LAUNCHER_ID_DOC_BASE, (int16_t)doc_i, false);
        }

        int total_list_h = s_launcher.doc_count * item_h;
        int view_list_h = vis_rows * item_h;
        if (total_list_h < view_list_h) total_list_h = view_list_h;
        ugui_scrollbar(&surf,
                       (ugui_rect_t){ (int16_t)(pad + list_w + 4 * s), (int16_t)y, (int16_t)sb_w, (int16_t)view_list_h },
                       s_launcher.scroll_idx * item_h, total_list_h, view_list_h);

        int status_y = h - pad - 16 * s;
        char batt[16] = "";
        (void)battery_indicator(batt, sizeof(batt));
        bool sd_ok = vfs_volume_mounted("sd0");
        char status_line[64];
        if (batt[0]) {
            ksnprintf(status_line, sizeof(status_line), "Batt: %s   SD: %s",
                      batt, sd_ok ? "Mounted" : "Absent");
        } else {
            ksnprintf(status_line, sizeof(status_line), "SD: %s   Touch UX Ready",
                      sd_ok ? "Mounted" : "Absent");
        }
        ugui_text_serif(&surf, pad, status_y, status_line, (uint32_t)strlen(status_line),
                        false, false, false);
    }

    s_launcher.seq++;
}

void launcher_update(void) {
    if (!s_launcher.active) return;
    if (s_launcher.win_w <= 0 || s_launcher.win_h <= 0) return;

#if defined(LCDTERM_PRESENT)
    canvas1_t *back = lcdterm_get_reader_backing(s_launcher.slot_id);
#else
    canvas1_t *back = NULL;
#endif
    if (!back) {
        back = ramscreen_get_reader_backing(s_launcher.slot_id);
    }

    launcher_draw(back, s_launcher.win_w, s_launcher.win_h);

    uint8_t ureq[2] = { 'W', s_launcher.slot_id }, sreply[SCREEN_REPLY_LEN];
    (void)console_canvas(ureq, 2, sreply);
}

bool launcher_open(void) {
    /* Allocate AA buffer on PSRAM-equipped P4 boards if not yet allocated */
#if defined(CONFIG_BOARD_ESP32P4) && defined(CONFIG_PSRAM_BYTES)
    if (!s_launcher.aa_buf) {
        int max_w = 1280;
        int max_h = 1280;
        uint32_t bytes = (uint32_t)(max_w * max_h);
        uint32_t pages = (bytes + 4095u) / 4096u;
        uint8_t *buf = (uint8_t *)palloc_pages_bulk(pages);
        if (buf) {
            memset(buf, UGUI_GRAY_PAPER, bytes);
            console_lock();
            s_launcher.aa_buf = buf;
            s_launcher.aa_stride = (uint32_t)max_w;
            s_launcher.aa_w = max_w;
            s_launcher.aa_h = max_h;
            s_launcher.aa_pages = pages;
            console_unlock();
        }
    }
#endif

    launcher_scan_docs();

    uint8_t req[2] = { 'U', 0 };
    uint8_t reply[SCREEN_REPLY_LEN];
    if (!console_canvas(req, 2, reply) || reply[0] != 0) {
        return false;
    }

    uint8_t slot_id = reply[1];
    int w = (int)(reply[2] | (reply[3] << 8));
    int h = (int)(reply[4] | (reply[5] << 8));

    console_lock();
    s_launcher.active = true;
    s_launcher.slot_id = slot_id;
    s_launcher.win_w = w;
    s_launcher.win_h = h;
    console_unlock();

    console_set_active_win_type(RIBBON_WIN_LAUNCHER);
    launcher_update();
    return true;
}

void launcher_close(void) {
    console_lock();
    s_launcher.active = false;
    uint8_t *buf = s_launcher.aa_buf;
    uint32_t pages = s_launcher.aa_pages;
    s_launcher.aa_buf = NULL;
    s_launcher.aa_pages = 0;
    console_unlock();

    if (buf) {
        palloc_free(buf, pages);
    }
}

bool launcher_tap(int client_x, int client_y) {
    if (!s_launcher.active) return false;

    int16_t hit_idx = -1;
    uint16_t id = ugui_hit_test(&s_launcher.ui_ctx, client_x, client_y, &hit_idx);
    if (id == 0) return false;

    if (id == LAUNCHER_ID_APP_READER) {
        const char *doc = (s_launcher.doc_count > 0) ? s_launcher.docs[0].path : "index.md";
        for (int i = 0; i < s_launcher.doc_count; i++) {
            if (reader_find_by_path(s_launcher.docs[i].path) < 0) {
                doc = s_launcher.docs[i].path;
                break;
            }
        }
        open_doc_in_reader(doc);
        return true;
    }
    if (id == LAUNCHER_ID_APP_CHESS) {
        if (focus_window_by_title("Chess")) {
            return true;
        }
        strncpy(s_launcher.pending_cmd, "chess touch", sizeof(s_launcher.pending_cmd) - 1);
        s_launcher.pending_cmd[sizeof(s_launcher.pending_cmd) - 1] = '\0';
        return true;
    }
    if (id == LAUNCHER_ID_APP_MOONBASE) {
        if (focus_window_by_title("Moonbase")) {
            return true;
        }
        strncpy(s_launcher.pending_cmd, "moonbase", sizeof(s_launcher.pending_cmd) - 1);
        s_launcher.pending_cmd[sizeof(s_launcher.pending_cmd) - 1] = '\0';
        return true;
    }
    if (id == LAUNCHER_ID_APP_TERMINAL) {
        console_hotkey(CONSOLE_HOTKEY_NEW_TERM);
        return true;
    }
    if (id == LAUNCHER_ID_VOL_STEP) {
        set_volume_percent(get_volume_percent() - 10);
        launcher_update();
        return true;
    }
    if (id == LAUNCHER_ID_VOL_STEP + 1) {
        set_volume_percent(get_volume_percent() + 10);
        launcher_update();
        return true;
    }
    if (id == LAUNCHER_ID_BL_STEP) {
        set_backlight_percent((int)get_backlight_percent() - 10);
        launcher_update();
        return true;
    }
    if (id == LAUNCHER_ID_BL_STEP + 1) {
        set_backlight_percent((int)get_backlight_percent() + 10);
        launcher_update();
        return true;
    }
#if defined(CONFIG_BOARD_TAB5)
    if (id == LAUNCHER_ID_ORIENT_SEG) {
        if (hit_idx == 0) {
            orient_set_auto(false);
            dsi_lcd_set_orientation(270);
            console_sync_readers();
            return true;
        }
        if (hit_idx == 1) {
            orient_set_auto(false);
            dsi_lcd_set_orientation(0);
            console_sync_readers();
            return true;
        }
        if (hit_idx == 2) {
            orient_set_auto(true);
            launcher_update();
            return true;
        }
    }
#endif
    if (id == LAUNCHER_ID_DOC_REFRESH) {
        launcher_scan_docs();
        launcher_update();
        return true;
    }
    if (id == LAUNCHER_ID_DOC_BASE && hit_idx >= 0 && hit_idx < s_launcher.doc_count) {
        open_doc_in_reader(s_launcher.docs[hit_idx].path);
        return true;
    }
    return false;
}

void launcher_scroll(int delta_y) {
    if (!s_launcher.active) return;
    int step = (delta_y > 0) ? 2 : -2;
    s_launcher.scroll_idx += step;
    if (s_launcher.scroll_idx < 0) s_launcher.scroll_idx = 0;
    launcher_update();
}

void launcher_feed_key(int c) {
    if (!s_launcher.active) return;
    if (c == 'j' || c == 'J' || c == ' ') {
        launcher_scroll(1);
    } else if (c == 'k' || c == 'K') {
        launcher_scroll(-1);
    } else if (c == 'r' || c == 'R') {
        launcher_scan_docs();
        launcher_update();
    }
}

bool launcher_poll_exec(char *out_cmd, size_t max_len) {
    if (!s_launcher.pending_cmd[0] || !out_cmd || max_len == 0) return false;
    strncpy(out_cmd, s_launcher.pending_cmd, max_len - 1);
    out_cmd[max_len - 1] = '\0';
    s_launcher.pending_cmd[0] = '\0';
    return true;
}

uint8_t *launcher_get_aa_backing(uint32_t *out_stride, int *out_w, int *out_h) {
    if (!s_launcher.active || !s_launcher.aa_buf) return NULL;
    if (out_stride) *out_stride = s_launcher.aa_stride;
    if (out_w) *out_w = s_launcher.win_w;
    if (out_h) *out_h = s_launcher.win_h;
    return s_launcher.aa_buf;
}

uint32_t launcher_get_seq(void) {
    return s_launcher.seq;
}

void launcher_report(void) {
    cprintf("launcher: %s (slot=%u, size=%dx%d, docs=%d, widgets=%d)\n",
            s_launcher.active ? "open" : "closed",
            (unsigned)s_launcher.slot_id,
            s_launcher.win_w, s_launcher.win_h,
            s_launcher.doc_count, s_launcher.ui_ctx.count);
}

#endif
