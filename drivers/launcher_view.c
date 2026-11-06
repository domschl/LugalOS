/* drivers/launcher_view.c — Classic Mac Touch Launcher Window
 *
 * Implements the touch-first Launcher ribbon window (RIBBON_WIN_LAUNCHER)
 * using the stateless Classic-Mac micro-toolkit (ugui):
 * - Application cards: Reader, Moonbase, Terminal
 * - Document Library browser: scans /sd0/pub, /sd0/help, /flash0/help
 * - Quick Settings: Volume stepper, Brightness stepper, Orientation selector
 * - Freestanding C, zero heap allocations, zero FPU in M-mode.
 */

#include "kernel/keyseq.h"
#include "kernel/apps.h"
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
void launcher_press(int client_x, int client_y) { (void)client_x; (void)client_y; }
void launcher_release(void) {}
void launcher_scroll(int delta_y) { (void)delta_y; }
void launcher_feed_key(int c) { (void)c; }
void launcher_gallery(bool on) { (void)on; }
bool launcher_drag_target(int client_x, int client_y) { (void)client_x; (void)client_y; return false; }
void launcher_pointer_move(int client_x, int client_y) { (void)client_x; (void)client_y; }
void launcher_drag(int dy) { (void)dy; }
void launcher_drag_end(int dy, uint32_t ms) { (void)dy; (void)ms; }
void launcher_feed_end(void) {}
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

#define LAUNCHER_MAX_DOCS       128     /* in BULK_BSS: ~20 KB, PSRAM on the panel boards */

#define LAUNCHER_ID_APP_READER   1
#define LAUNCHER_ID_APP_CHESS    2
#define LAUNCHER_ID_APP_MOONBASE 3
#define LAUNCHER_ID_APP_TERMINAL 4
#define LAUNCHER_ID_VOL_STEP     10
#define LAUNCHER_ID_BL_STEP      20
#define LAUNCHER_ID_ORIENT_SEG   30
#define LAUNCHER_ID_DOC_REFRESH  40
#define LAUNCHER_ID_DOC_BASE     100
/* The control gallery (`launcher gallery`): every ugui control, to try by
 * touch and keys before an app uses it. */
#define GAL_ID_TAB       500     /* + tab */
#define GAL_ID_DONE      510
#define GAL_ID_TOGGLE    511
#define GAL_ID_CHECK     512
#define GAL_ID_SLIDER    513
#define GAL_ID_PATH      514
#define GAL_ID_TEXT      515
#define GAL_ID_SHOW_DLG  516
#define GAL_ID_SHOW_SHT  517
#define GAL_ID_DLG       520     /* + button */
#define GAL_ID_SHEET     530     /* + item */

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
    struct {                                   /* the control gallery's state */
        bool        on, toggle, check, dialog, sheet;
        int         tab, slider;
        char        text[64];
        ugui_text_t field;
        char        result[64];
    } gal;
    uint8_t        slot_id;
    int            win_w;
    int            win_h;
    int            scroll_px;      /* the document list's scroll, pixels */
    int            item_h, view_h; /* its row and viewport heights, as last drawn */
    int            list_x, list_y, list_w, list_avail;   /* draw_doc_list()'s place, as last drawn */
    int            doc_count;
    launcher_doc_t docs[LAUNCHER_MAX_DOCS];
    ugui_ctx_t     ui_ctx;
    uint8_t       *aa_buf;         /* GRAY8: what the panel composites (the front) */
    uint8_t       *aa_back;        /* the one a full draw goes into, then swapped in; NULL: draw in place */
    uint8_t       *aa_target;      /* the one launcher_draw() is drawing into */
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

/* ASCII case folded: the FAT volumes answer 8.3 names in capitals. */
static bool same_name(const char *a, const char *b) {
    for (;; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return false;
        if (!ca) return true;
    }
}

static bool str_ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s);
    size_t xl = strlen(suffix);
    if (sl < xl) return false;
    return same_name(s + sl - xl, suffix);
}

static const char *find_last_char(const char *s, char ch) {
    const char *last = NULL;
    for (; *s; s++) {
        if (*s == ch) last = s;
    }
    return last;
}

/* Whether a and b name the same document: equal up to their extensions,
 * without case (ADMIN.MD and ADMIN.GMI are one document in two forms). */
static bool same_stem(const char *a, const char *b) {
    for (;; a++, b++) {
        char ca = *a == '.' ? '\0' : (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = *b == '.' ? '\0' : (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return false;
        if (!ca) return true;
    }
}

/* The listed document with basename's stem, or -1. */
static int doc_listed(const char *basename) {
    for (int i = 0; i < s_launcher.doc_count; i++) {
        const char *p = s_launcher.docs[i].path;
        const char *slash = find_last_char(p, '/');
        const char *b = slash ? (slash + 1) : p;
        if (same_stem(b, basename)) return i;
    }
    return -1;
}

/* A document once: a second form of one already listed (the same name with
 * another extension) only replaces a Gemtext path by the Markdown one, the
 * reader's richer form. */
static void add_doc_entry(const char *full_path, const char *name, const char *badge) {
    int dup = doc_listed(name);
    if (dup >= 0) {
        launcher_doc_t *d = &s_launcher.docs[dup];
        if (str_ends_with(d->path, ".gmi") && str_ends_with(name, ".md")) {
            strncpy(d->path, full_path, sizeof(d->path) - 1);
            d->path[sizeof(d->path) - 1] = '\0';
        }
        return;
    }
    if (s_launcher.doc_count >= LAUNCHER_MAX_DOCS) return;
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
    /* An 8.3 name comes in capitals: shown as "Shell", not "SHELL". */
    bool lower = false;
    for (int i = 0; d->label[i]; i++)
        if (d->label[i] >= 'a' && d->label[i] <= 'z') lower = true;
    if (!lower)
        for (int i = 1; d->label[i]; i++)
            if (d->label[i] >= 'A' && d->label[i] <= 'Z') d->label[i] = (char)(d->label[i] + 32);
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
    for (uint32_t idx = 0; idx < 256 && s_launcher.doc_count < LAUNCHER_MAX_DOCS; idx++) {
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

    scan_dir_docs("/sd0/docs", "Docs");                  /* the user's own, before the bundled */
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

/* The steppers' parts that cannot act (ugui_disable_next(): bit 0 the -,
 * bit 1 the +): at a limit, or both where the board has no such control. */
static uint8_t volume_limits(void) {
#if defined(CONFIG_BOARD_ESP32P4)
    int v = get_volume_percent();
    return (uint8_t)((v <= 0 ? 1u : 0u) | (v >= 100 ? 2u : 0u));
#else
    return 3u;
#endif
}

static uint8_t backlight_limits(void) {
    unsigned b = get_backlight_percent();
    return (uint8_t)((b <= 10u ? 1u : 0u) | (b >= 100u ? 2u : 0u));
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

/* The window of the canvas called `name` (a game's), focused: its title is
 * the generic one, so it is found by the canvas. False when it is not open. */
static bool focus_canvas_by_name(const char *name) {
#if defined(LCDTERM_PRESENT)
    int cid = lcdterm_get_canvas_cid_by_name(name);
    if (cid < 0) return false;
    uint8_t rreq[2] = { 'R', 0 }, rreply[SCREEN_REPLY_LEN];
    if (!console_canvas(rreq, 1, rreply) || rreply[0] != 0) return false;
    uint8_t count = rreply[1];
    for (uint8_t i = 0; i < count; i++) {
        rreq[1] = i;
        if (!console_canvas(rreq, 2, rreply) || rreply[0] != 0) continue;
        if (rreply[1] != RIBBON_WIN_CANVAS || rreply[2] != (uint8_t)cid) continue;
        uint8_t jreq[2] = { 'J', i };
        (void)console_canvas(jreq, 2, rreply);
        if (rreply[10] != 0) console_set_active_win_type(rreply[10]);
        vterm_set_active(rreply[1]);
        return true;
    }
#else
    (void)name;
#endif
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

static void doc_row(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t row, int i, void *user) {
    (void)user;
    int sc = s->scale;
    ugui_list_row(s, ctx, (ugui_rect_t){ (int16_t)(row.x + 2 * sc), row.y, (int16_t)(row.w - 4 * sc), row.h },
                  NULL, s_launcher.docs[i].label, s_launcher.docs[i].badge, LAUNCHER_ID_DOC_BASE, (int16_t)i, false);
}

/* The document list in x, y, w x avail_h: a framed virtual list of whole
 * rows' height, scrolled by pixels (s_launcher.scroll_px), and its
 * scrollbar. `frame` false: a scroll step -- the rows are drawn over what
 * shows (each fills its own background, so no frame shows them cleared),
 * only the space below the last one cleared. */
static void draw_doc_list(const ugui_surface_t *surf, int x, int y, int w, int avail_h, bool frame) {
    const int s = surf->scale;
    s_launcher.list_x = x;
    s_launcher.list_y = y;
    s_launcher.list_w = w;
    s_launcher.list_avail = avail_h;
    int item_h = 30 * s, sb_w = 10 * s;
    int list_w = w - sb_w - 4 * s;
    int vis_rows = avail_h / item_h;
    if (vis_rows < 1) vis_rows = 1;
    int view_h = vis_rows * item_h;
    s_launcher.item_h = item_h;
    s_launcher.view_h = view_h - 2 * s;
    if (frame) ugui_box_shadow(surf, (ugui_rect_t){ (int16_t)x, (int16_t)y, (int16_t)list_w, (int16_t)view_h }, false);
    ugui_rect_t view = { (int16_t)(x + s), (int16_t)(y + s), (int16_t)(list_w - 2 * s), (int16_t)(view_h - 2 * s) };
    ugui_vlist(surf, &s_launcher.ui_ctx, view, s_launcher.doc_count, item_h, &s_launcher.scroll_px, doc_row, NULL);
    int rows_end = view.y + s_launcher.doc_count * item_h - s_launcher.scroll_px;
    if (!frame && rows_end < view.y + view.h)
        ugui_fill(surf, (ugui_rect_t){ view.x, (int16_t)rows_end, view.w, (int16_t)(view.y + view.h - rows_end) },
                  UGUI_GRAY_CARD, CANVAS1_WHITE);
    int total = s_launcher.doc_count * item_h;
    if (total < view.h) total = view.h;
    ugui_scrollbar(surf, (ugui_rect_t){ (int16_t)(x + list_w + 4 * s), (int16_t)y, (int16_t)sb_w, (int16_t)view_h },
                   s_launcher.scroll_px, total, view.h);
}

static void draw_status(const ugui_surface_t *surf, ugui_rect_t r) {
    char batt[16] = "";
    (void)battery_indicator(batt, sizeof(batt));
    bool sd_ok = vfs_volume_mounted("sd0");
    char line[64];
    if (batt[0]) ksnprintf(line, sizeof(line), "Batt: %s   SD: %s", batt, sd_ok ? "Mounted" : "Absent");
    else ksnprintf(line, sizeof(line), "SD: %s   Touch UX Ready", sd_ok ? "Mounted" : "Absent");
    ugui_text_serif(surf, r.x, r.y, line, (uint32_t)strlen(line), false, false, false);
}

/* The applications and quick settings, cut off the top of *col (which is
 * left with what remains). One description for both orientations. */
static void draw_apps_and_settings(const ugui_surface_t *surf, ugui_rect_t *col) {
    const int s = surf->scale;
    ugui_ctx_t *ctx = &s_launcher.ui_ctx;
    ugui_rect_t hdr = ugui_cut_top(col, 24 * s);
    ugui_section_header(surf, hdr.x, hdr.y, hdr.w, "APPLICATIONS");
    ugui_rect_t cards = ugui_cut_top(col, 2 * 56 * s + 8 * s);
    ugui_card(surf, ctx, ugui_grid_cell(cards, 2, 2, 8 * s, 0), k_icon_reader, "Reader", "Docs", LAUNCHER_ID_APP_READER, false);
    ugui_card(surf, ctx, ugui_grid_cell(cards, 2, 2, 8 * s, 1), k_icon_chess, "Chess", "Game", LAUNCHER_ID_APP_CHESS, false);
    ugui_card(surf, ctx, ugui_grid_cell(cards, 2, 2, 8 * s, 2), k_icon_moonbase, "Moonbase", "Game", LAUNCHER_ID_APP_MOONBASE, false);
    ugui_card(surf, ctx, ugui_grid_cell(cards, 2, 2, 8 * s, 3), k_icon_terminal, "Terminal", "Shell", LAUNCHER_ID_APP_TERMINAL, false);
    (void)ugui_cut_top(col, 14 * s);

    hdr = ugui_cut_top(col, 26 * s);
    ugui_section_header(surf, hdr.x, hdr.y, hdr.w, "QUICK SETTINGS");
    char vol_str[16];
    ksnprintf(vol_str, sizeof(vol_str), "%d%%", get_volume_percent());
    ugui_disable_next(ctx, volume_limits());
    ugui_stepper(surf, ctx, ugui_cut_top(col, 30 * s), "Volume", vol_str, LAUNCHER_ID_VOL_STEP, LAUNCHER_ID_VOL_STEP + 1);
    (void)ugui_cut_top(col, 8 * s);
    char bl_str[16];
    ksnprintf(bl_str, sizeof(bl_str), "%u%%", get_backlight_percent());
    ugui_disable_next(ctx, backlight_limits());
    ugui_stepper(surf, ctx, ugui_cut_top(col, 30 * s), col->w >= 320 * s ? "Brightness" : "Bright", bl_str,
                 LAUNCHER_ID_BL_STEP, LAUNCHER_ID_BL_STEP + 1);
    (void)ugui_cut_top(col, 10 * s);
#if defined(CONFIG_BOARD_TAB5)
    static const char *const k_orient_labels[3] = { "Landscape", "Portrait", "Auto" };
    ugui_segmented(surf, ctx, ugui_cut_top(col, 28 * s), k_orient_labels, 3, get_orient_mode(), LAUNCHER_ID_ORIENT_SEG);
    (void)ugui_cut_top(col, 10 * s);
#endif
}

/* The documents: a header with Refresh, the list under it. */
static void draw_documents(const ugui_surface_t *surf, ugui_rect_t r) {
    const int s = surf->scale;
    ugui_rect_t hdr = ugui_cut_top(&r, 24 * s);
    ugui_rect_t btn = ugui_cut_right(&hdr, 66 * s);
    (void)ugui_cut_right(&hdr, 6 * s);
    ugui_section_header(surf, hdr.x, hdr.y, hdr.w, "DOCUMENTS");
    ugui_toolbar_button(surf, &s_launcher.ui_ctx, (ugui_rect_t){ btn.x, (int16_t)(btn.y - 2 * s), btn.w, (int16_t)(20 * s) },
                        "Refresh", LAUNCHER_ID_DOC_REFRESH, false);
    draw_doc_list(surf, r.x, r.y, r.w, r.h, true);
}

static const char *const k_gal_tabs[3] = { "Controls", "Text", "Dialogs" };
static const char *const k_gal_dlg[2] = { "Cancel", "Delete" };
static const char *const k_gal_sheet[5] = { "Open", "Rename", "Add to Shortcuts", "Delete", "Cancel" };
static const char k_gal_path[] = "/sd0/docs/books/german/goethe";

static void gal_line(const ugui_surface_t *surf, int x, int y, const char *text, bool muted) {
    ugui_text_serif(surf, x, y, text, (uint32_t)strlen(text), false, muted, false);
}

static void launcher_draw_gallery(const ugui_surface_t *surf, int w, int h) {
    const int s = surf->scale;
    ugui_ctx_t *ctx = &s_launcher.ui_ctx;
    int pad = 12 * s, col = w - 2 * pad > 440 * s ? 440 * s : w - 2 * pad, row = 32 * s, y = pad;
    ugui_tab_bar(surf, ctx, (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)(w - 2 * pad - 90 * s), (int16_t)(26 * s) },
                 k_gal_tabs, 3, s_launcher.gal.tab, GAL_ID_TAB);
    ugui_toolbar_button(surf, ctx, (ugui_rect_t){ (int16_t)(w - pad - 80 * s), (int16_t)(y + 2 * s), (int16_t)(80 * s), (int16_t)(22 * s) },
                        "Done", GAL_ID_DONE, false);
    y += 40 * s;
    if (s_launcher.gal.tab == 0) {
        ugui_toggle(surf, ctx, (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)col, (int16_t)row },
                    "Wi-Fi", s_launcher.gal.toggle, GAL_ID_TOGGLE);
        y += row + 6 * s;
        ugui_checkbox(surf, ctx, (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)col, (int16_t)row },
                      "Show hidden files", s_launcher.gal.check, GAL_ID_CHECK);
        y += row + 6 * s;
        ugui_slider(surf, ctx, (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)(col - 70 * s), (int16_t)row },
                    s_launcher.gal.slider, 0, 100, GAL_ID_SLIDER);
        char v[16];
        ksnprintf(v, sizeof v, "%d", s_launcher.gal.slider);
        gal_line(surf, pad + col - 60 * s, y + (row - ugui_serif_line_h(surf)) / 2, v, false);
        y += row + 6 * s;
        ugui_progress(surf, (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)(col - 70 * s), (int16_t)(14 * s) },
                      s_launcher.gal.slider * 10);
        y += 28 * s;
        ugui_path_bar(surf, ctx, (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)(w - 2 * pad), (int16_t)(26 * s) },
                      k_gal_path, GAL_ID_PATH);
        y += 34 * s;
    } else if (s_launcher.gal.tab == 1) {
        gal_line(surf, pad, y, "Name:", false);
        y += ugui_serif_line_h(surf) + 4 * s;
        ugui_textfield(surf, ctx, (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)col, (int16_t)(28 * s) },
                       &s_launcher.gal.field, GAL_ID_TEXT);
        y += 36 * s;
        gal_line(surf, pad, y, "Tap the field (or Tab to it) and type on a USB keyboard;", true);
        y += ugui_serif_line_h(surf);
        gal_line(surf, pad, y, "Shift+arrows select, Enter submits, Esc cancels.", true);
        y += ugui_serif_line_h(surf) + 8 * s;
    } else {
        ugui_button(surf, ctx, (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)(180 * s), (int16_t)(28 * s) },
                    "Show Dialog", GAL_ID_SHOW_DLG, false);
        ugui_button(surf, ctx, (ugui_rect_t){ (int16_t)(pad + 196 * s), (int16_t)y, (int16_t)(180 * s), (int16_t)(28 * s) },
                    "Show Action Sheet", GAL_ID_SHOW_SHT, false);
        y += 44 * s;
    }
    if (s_launcher.gal.result[0]) gal_line(surf, pad, y, s_launcher.gal.result, false);
    (void)h;
    if (s_launcher.gal.dialog)
        (void)ugui_dialog(surf, ctx, "Delete \"notes.md\"?", "The file will be removed from /sd0/docs.\nThis cannot be undone.",
                          k_gal_dlg, 2, GAL_ID_DLG);
    else if (s_launcher.gal.sheet)
        (void)ugui_action_sheet(surf, ctx, "notes.md", k_gal_sheet, 5, GAL_ID_SHEET);
}

/* A gallery control acted on (a tap at x, or Enter: x < 0). */
static bool launcher_gallery_act(uint16_t id, int16_t idx, int x) {
    char *res = s_launcher.gal.result;
    if (id >= GAL_ID_TAB && id < GAL_ID_TAB + 3) { s_launcher.gal.tab = id - GAL_ID_TAB; res[0] = '\0'; }
    else if (id == GAL_ID_DONE) { s_launcher.gal.on = false; s_launcher.gal.dialog = s_launcher.gal.sheet = false; }
    else if (id == GAL_ID_TOGGLE) s_launcher.gal.toggle = !s_launcher.gal.toggle;
    else if (id == GAL_ID_CHECK) s_launcher.gal.check = !s_launcher.gal.check;
    else if (id == GAL_ID_SLIDER) {
        if (x < 0) return false;
        for (int i = 0; i < s_launcher.ui_ctx.count; i++)
            if (s_launcher.ui_ctx.hits[i].id == GAL_ID_SLIDER) {
                ugui_surface_t ss = { .scale = CHROME_SCALE };
                s_launcher.gal.slider = ugui_slider_value(&ss, s_launcher.ui_ctx.hits[i].rect, x, 0, 100);
            }
    }
    else if (id == GAL_ID_PATH) {
        char pre[sizeof k_gal_path];
        int n = ugui_path_prefix(k_gal_path, idx);
        memcpy(pre, k_gal_path, (size_t)n);
        pre[n] = '\0';
        ksnprintf(res, 64, "Path: %s", pre);
    }
    else if (id == GAL_ID_TEXT) ugui_set_focus(&s_launcher.ui_ctx, GAL_ID_TEXT, 0);
    else if (id == GAL_ID_SHOW_DLG) s_launcher.gal.dialog = true;
    else if (id == GAL_ID_SHOW_SHT) s_launcher.gal.sheet = true;
    else if (id >= GAL_ID_DLG && id < GAL_ID_DLG + 2) { s_launcher.gal.dialog = false; ksnprintf(res, 64, "Dialog: %s", k_gal_dlg[id - GAL_ID_DLG]); }
    else if (id >= GAL_ID_SHEET && id < GAL_ID_SHEET + 5) { s_launcher.gal.sheet = false; ksnprintf(res, 64, "Action sheet: %s", k_gal_sheet[id - GAL_ID_SHEET]); }
    else return false;
    launcher_update();
    return true;
}

/* A key on the gallery, before the focus moves: the text field's while it
 * has the focus, the slider's Left / Right, Esc closing a modal. */
static bool launcher_gallery_key(key_event_t k) {
    ugui_ctx_t *ctx = &s_launcher.ui_ctx;
    if (k.key == 0x1b && (s_launcher.gal.dialog || s_launcher.gal.sheet)) {
        s_launcher.gal.dialog = s_launcher.gal.sheet = false;
        ksnprintf(s_launcher.gal.result, 64, "Cancelled");
        launcher_update();
        return true;
    }
    if (s_launcher.gal.tab == 1 && ctx->focus_id == GAL_ID_TEXT && !s_launcher.gal.dialog && !s_launcher.gal.sheet) {
        ugui_text_ev_t e = ugui_text_key(&s_launcher.gal.field, k.key, k.mods);
        if (e == UGUI_TEXT_SUBMIT) ksnprintf(s_launcher.gal.result, 64, "Submitted: %s", s_launcher.gal.text);
        if (e == UGUI_TEXT_CANCEL) { ctx->focus_id = 0; ctx->focus_shown = false; }
        if (e != UGUI_TEXT_NONE) { launcher_update(); return true; }
    }
    if (s_launcher.gal.tab == 0 && ctx->focus_shown && ctx->focus_id == GAL_ID_SLIDER &&
        (k.key == KEY_LEFT || k.key == KEY_RIGHT)) {
        s_launcher.gal.slider += k.key == KEY_RIGHT ? 5 : -5;
        if (s_launcher.gal.slider < 0) s_launcher.gal.slider = 0;
        if (s_launcher.gal.slider > 100) s_launcher.gal.slider = 100;
        launcher_update();
        return true;
    }
    return false;
}

/* accepted: read by the touch task (console_touch_captures()) while the
 * input path may be redrawing: the hit-boxes are a bounded array, and a
 * stale answer costs one contact taken as a gesture or a drag. */
bool launcher_drag_target(int client_x, int client_y) {
    return s_launcher.active && ugui_drag_at(&s_launcher.ui_ctx, client_x, client_y);
}

/* A captured finger at (client_x, client_y): the armed slider follows it. */
void launcher_pointer_move(int client_x, int client_y) {
    (void)client_y;
    if (!s_launcher.active || !s_launcher.gal.on || s_launcher.ui_ctx.armed_id != GAL_ID_SLIDER) return;
    (void)launcher_gallery_act(GAL_ID_SLIDER, 0, client_x);   /* sets it from x, draws */
}

void launcher_gallery(bool on) {
    if (on && !s_launcher.gal.field.buf) {
        strncpy(s_launcher.gal.text, "notes.md", sizeof s_launcher.gal.text);
        ugui_text_init(&s_launcher.gal.field, s_launcher.gal.text, (uint16_t)sizeof s_launcher.gal.text);
        s_launcher.gal.slider = 40;
    }
    s_launcher.gal.on = on;
    launcher_update();
}

void launcher_draw(const canvas1_t *c, int w, int h) {
    if ((!c && !s_launcher.aa_buf) || w <= 0 || h <= 0) return;

    const int s = CHROME_SCALE;
    ugui_surface_t surf;
    surf.c1 = c;
    surf.aa_fb = s_launcher.aa_target ? s_launcher.aa_target : s_launcher.aa_buf;
    surf.aa_stride = s_launcher.aa_stride;
    surf.w = w;
    surf.h = h;
    surf.scale = s;
    surf.clipped = false;

    ugui_begin(&s_launcher.ui_ctx);
    ugui_set_min_target(&s_launcher.ui_ctx, ugui_target_px());   /* touch areas: at least 7 mm */
    ugui_fill(&surf, (ugui_rect_t){ 0, 0, (int16_t)w, (int16_t)h }, UGUI_GRAY_PAPER, CANVAS1_WHITE);
    if (s_launcher.gal.on) {
        launcher_draw_gallery(&surf, w, h);
        ugui_end(&surf, &s_launcher.ui_ctx);
        s_launcher.seq++;
        return;
    }

    int pad = 12 * s;
    ugui_rect_t area = ugui_inset((ugui_rect_t){ 0, 0, (int16_t)w, (int16_t)h }, pad, pad);
    if (w >= h) {
        /* Landscape: apps and settings in a column on the left, the
         * status at its foot; the documents beside it. */
        ugui_rect_t col = ugui_cut_left(&area, w * 46 / 100);
        (void)ugui_cut_left(&area, 10 * s);
        ugui_rect_t status = ugui_cut_bottom(&col, 20 * s);
        draw_apps_and_settings(&surf, &col);
        if (col.h > 0) draw_status(&surf, status);
        draw_documents(&surf, area);
    } else {
        /* Portrait: the same column on top, the documents under it, the
         * status at the foot. */
        ugui_rect_t status = ugui_cut_bottom(&area, 16 * s);
        (void)ugui_cut_bottom(&area, 4 * s);
        draw_apps_and_settings(&surf, &area);
        draw_documents(&surf, area);
        draw_status(&surf, status);
    }

    ugui_end(&surf, &s_launcher.ui_ctx);
    s_launcher.seq++;
}

static canvas1_t *launcher_backing(void) {
#if defined(LCDTERM_PRESENT)
    canvas1_t *back = lcdterm_get_reader_backing(s_launcher.slot_id);
#else
    canvas1_t *back = NULL;
#endif
    return back ? back : ramscreen_get_reader_backing(s_launcher.slot_id);
}

/* A scroll step: only the document list drawn again, and shown. The rest
 * of the launcher stays as it is on the surface the panel composites from,
 * so it does not flicker; its rows' hit-boxes (the pass's last) are
 * replaced. With the focus ring showing, the whole launcher instead. */
static void launcher_update_list(void) {
    if (!s_launcher.active || s_launcher.win_w <= 0 || s_launcher.win_h <= 0) return;
    ugui_ctx_t *ctx = &s_launcher.ui_ctx;
    canvas1_t *back = launcher_backing();
    if (ctx->focus_shown || s_launcher.list_avail <= 0 || (!back && !s_launcher.aa_buf)) {
        launcher_update();
        return;
    }
    ugui_surface_t surf = {
        .c1 = back, .aa_fb = s_launcher.aa_buf, .aa_stride = s_launcher.aa_stride,
        .w = s_launcher.win_w, .h = s_launcher.win_h, .scale = CHROME_SCALE,
    };
    int keep = 0;
    for (int i = 0; i < ctx->count; i++)
        if (ctx->hits[i].id != LAUNCHER_ID_DOC_BASE) ctx->hits[keep++] = ctx->hits[i];
    ctx->count = keep;
    draw_doc_list(&surf, s_launcher.list_x, s_launcher.list_y, s_launcher.list_w, s_launcher.list_avail, false);
    s_launcher.seq++;
    uint8_t ureq[2] = { 'W', s_launcher.slot_id }, sreply[SCREEN_REPLY_LEN];
    (void)console_canvas(ureq, 2, sreply);
}

/* A full draw. On the P4 the panel composites from the launcher's GRAY8
 * surface while it is drawn, so a draw in place showed it half-cleared (the
 * gallery's tab bar flickered at every tap): it goes into the back buffer,
 * swapped in whole under console_lock (term_snapshot() reads the pointer
 * under it), and only then is seq moved, so no snapshot pairs the new seq
 * with the old picture. Before drawing, a conversion still reading the
 * back buffer (the front of the last swap) is waited for: the frame lock. */
void launcher_update(void) {
    if (!s_launcher.active) return;
    if (s_launcher.win_w <= 0 || s_launcher.win_h <= 0) return;

    canvas1_t *back = launcher_backing();
    if (s_launcher.aa_back) {
        console_lock();
        console_frame_lock();
        console_frame_unlock();
        console_unlock();
        s_launcher.aa_target = s_launcher.aa_back;
    }
    launcher_draw(back, s_launcher.win_w, s_launcher.win_h);
    if (s_launcher.aa_target) {
        console_lock();
        uint8_t *front = s_launcher.aa_buf;
        s_launcher.aa_buf = s_launcher.aa_target;
        s_launcher.aa_back = front;
        s_launcher.aa_target = NULL;
        s_launcher.seq++;
        console_unlock();
    }

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
        uint8_t *buf2 = buf ? (uint8_t *)palloc_pages_bulk(pages) : NULL;   /* the back: optional */
        if (buf) {
            memset(buf, UGUI_GRAY_PAPER, bytes);
            console_lock();
            s_launcher.aa_buf = buf;
            s_launcher.aa_back = buf2;
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
    console_frame_lock();                                /* no conversion reads them past here */
    uint8_t *buf = s_launcher.aa_buf, *buf2 = s_launcher.aa_back;
    uint32_t pages = s_launcher.aa_pages;
    s_launcher.aa_buf = NULL;
    s_launcher.aa_back = NULL;
    s_launcher.aa_pages = 0;
    console_frame_unlock();
    console_unlock();

    if (buf) palloc_free(buf, pages);
    if (buf2) palloc_free(buf2, pages);
}

void launcher_press(int client_x, int client_y) {
    if (!s_launcher.active) return;
    if (ugui_arm(&s_launcher.ui_ctx, client_x, client_y)) launcher_update();
}

void launcher_release(void) {
    if (!s_launcher.active) return;
    if (ugui_disarm(&s_launcher.ui_ctx)) launcher_update();
}

static bool launcher_tap_act(int client_x, int client_y);

/* Application `id` (kernel/apps.h) on path (NULL: none): a single-instance
 * one that is running is focused instead; otherwise its command line is
 * left for the shell (shell_check_launcher_exec()). */
static bool launch_app(const char *id, const char *path) {
    const app_desc_t *a = app_find(id);
    if (!a) return false;
    if ((a->caps & APP_SINGLETON) && a->window &&
        (focus_canvas_by_name(a->window) || focus_window_by_title(a->window)))
        return true;                                     /* running: back to it */
    return app_command(a, path, s_launcher.pending_cmd, sizeof(s_launcher.pending_cmd));
}
static bool launcher_activate(uint16_t id, int16_t hit_idx);

/* A tap: the pressed control lets go first, then acts; drawn once, by the
 * action if it redraws, here if it does not (the release that follows finds
 * nothing armed). */
bool launcher_tap(int client_x, int client_y) {
    if (!s_launcher.active) return false;
    bool was_armed = ugui_disarm(&s_launcher.ui_ctx);
    uint32_t seq = s_launcher.seq;
    bool acted = launcher_tap_act(client_x, client_y);
    if (was_armed && s_launcher.active && s_launcher.seq == seq) launcher_update();
    return acted;
}

static bool launcher_tap_act(int client_x, int client_y) {
    int16_t hit_idx = -1;
    uint16_t id = ugui_hit_test(&s_launcher.ui_ctx, client_x, client_y, &hit_idx);
    if (s_launcher.gal.on) return id && launcher_gallery_act(id, hit_idx, client_x);
    return launcher_activate(id, hit_idx);
}

/* What a tap on widget id (hit_idx: its index) does; the keyboard's Enter
 * does the same. An app card leaves its command in pending_cmd for the
 * shell (shell_check_launcher_exec()). */
static bool launcher_activate(uint16_t id, int16_t hit_idx) {
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
    if (id == LAUNCHER_ID_APP_CHESS) return launch_app("chess", NULL);
    if (id == LAUNCHER_ID_APP_MOONBASE) return launch_app("moonbase", NULL);
    if (id == LAUNCHER_ID_APP_TERMINAL) {
        console_hotkey(CONSOLE_HOTKEY_NEW_TERM);         /* beside the launcher, focused */
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
    /* ugui_segmented() gives each segment its own id, base + index. */
    if (id >= LAUNCHER_ID_ORIENT_SEG && id < LAUNCHER_ID_ORIENT_SEG + 3) {
        hit_idx = (int16_t)(id - LAUNCHER_ID_ORIENT_SEG);
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
        const app_desc_t *a = app_for_path(s_launcher.docs[hit_idx].path);
        if (a && strcmp(a->id, "reader") == 0) {
            open_doc_in_reader(s_launcher.docs[hit_idx].path);   /* the reader in the ribbon, directly */
            return true;
        }
        return a && launch_app(a->id, s_launcher.docs[hit_idx].path);
    }
    return false;
}

void launcher_scroll(int delta_y) {
    if (!s_launcher.active || s_launcher.gal.on) return;
    s_launcher.scroll_px += (delta_y > 0 ? 2 : -2) * (s_launcher.item_h > 0 ? s_launcher.item_h : 30);
    if (s_launcher.scroll_px < 0) s_launcher.scroll_px = 0;
    launcher_update();
}

/* A finger dragging dy px (down positive): the document list follows it. */
void launcher_drag(int dy) {
    if (!s_launcher.active || dy == 0 || s_launcher.gal.on) return;
    s_launcher.scroll_px -= dy;
    if (s_launcher.scroll_px < 0) s_launcher.scroll_px = 0;
    launcher_update_list();
}

/* The drag's end: a finger still moving (dy over its last ms) glides on,
 * its speed halving at each of up to four steps -- in whole pixels, drawn
 * as it goes. */
void launcher_drag_end(int dy, uint32_t ms) {
    if (!s_launcher.active || ms == 0 || dy == 0) return;
    int step = (int)((long)dy * 48 / (long)ms);          /* px per ~48 ms */
    for (int i = 0; i < 4 && (step > 1 || step < -1); i++) {
        int before = s_launcher.scroll_px;
        launcher_drag(step);
        if (s_launcher.scroll_px == before) break;       /* at an end */
        step /= 2;
    }
}

/* The document list under the keyboard: Up / Down from a row moves to the
 * row before / after it, scrolling it into view, so the focus walks the
 * whole list rather than jumping out of it at the visible edge. */
static bool launcher_doc_step(uint32_t key) {
    ugui_ctx_t *ctx = &s_launcher.ui_ctx;
    if (!ctx->focus_shown || ctx->focus_id != LAUNCHER_ID_DOC_BASE) return false;
    int to = ctx->focus_index + (key == KEY_DOWN ? 1 : -1);
    if (to < 0 || to >= s_launcher.doc_count) return false;
    ctx->focus_index = (int16_t)to;
    s_launcher.scroll_px = ugui_vlist_reveal(s_launcher.scroll_px, s_launcher.view_h, s_launcher.item_h, to);
    return true;
}

static void launcher_key(key_event_t k) {
    if (s_launcher.gal.on && launcher_gallery_key(k)) return;
    if (k.key == KEY_PGDN || k.key == KEY_PGUP) {
        s_launcher.scroll_px += k.key == KEY_PGDN ? s_launcher.view_h : -s_launcher.view_h;
        if (s_launcher.scroll_px < 0) s_launcher.scroll_px = 0;
        launcher_update();
        return;
    }
    if ((k.key == KEY_UP || k.key == KEY_DOWN) && launcher_doc_step(k.key)) {
        launcher_update();
        return;
    }
    uint16_t id = 0;
    int16_t idx = 0;
    switch (ugui_nav(&s_launcher.ui_ctx, k.key, k.mods, &id, &idx)) {
    case UGUI_NAV_MOVED:
        launcher_update();
        return;
    case UGUI_NAV_ACTIVATE: {
        uint32_t seq = s_launcher.seq;
        if (s_launcher.gal.on) (void)launcher_gallery_act(id, idx, -1);
        else (void)launcher_activate(id, idx);
        if (s_launcher.active && s_launcher.seq == seq) launcher_update();
        return;
    }
    case UGUI_NAV_EDGE:
        if (k.key == KEY_DOWN || k.key == KEY_UP) launcher_scroll(k.key == KEY_DOWN ? 1 : -1);
        return;
    default:
        break;
    }
    if (k.key == 'r' || k.key == 'R') {
        launcher_scan_docs();
        launcher_update();
    }
}

/* guarded by: g_input_lock (kernel/console.c): the console pump is the only
 * caller of launcher_feed_key() / launcher_feed_end(). */
static keyseq_push_t s_key_push;

void launcher_feed_key(int c) {
    if (!s_launcher.active) return;
    key_event_t k;
    if (keyseq_push(&s_key_push, c, &k)) launcher_key(k);
}

void launcher_feed_end(void) {
    key_event_t k;
    if (keyseq_push_flush(&s_key_push, &k) && s_launcher.active) launcher_key(k);
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
    /* The hit-boxes, window-relative, for `touch tap` in tests. */
    for (int i = 0; i < s_launcher.ui_ctx.count; i++) {
        const ugui_hit_t *h = &s_launcher.ui_ctx.hits[i];
        ugui_rect_t t = ugui_touch_rect(&s_launcher.ui_ctx, i);
        cprintf("  hit id %u index %d: x %d..%d y %d..%d, touched over %dx%d px%s\n", (unsigned)h->id, (int)h->index,
                (int)h->rect.x, (int)(h->rect.x + h->rect.w - 1), (int)h->rect.y, (int)(h->rect.y + h->rect.h - 1),
                (int)t.w, (int)t.h, (h->flags & UGUI_HIT_DISABLED) ? " (dimmed)" : "");
    }
}

#endif
