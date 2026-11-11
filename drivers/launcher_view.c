/* drivers/launcher_view.c — Classic Mac Touch Launcher Window
 *
 * Implements the touch-first Launcher ribbon window (RIBBON_WIN_LAUNCHER)
 * using the stateless Classic-Mac micro-toolkit (ugui):
 * - Application cards: Reader, Moonbase, Terminal
 * - Document Library browser: scans /sd0/pub, /sd0/help, /flash0/help
 * - Quick Settings: Volume stepper, Brightness stepper, Orientation selector,
 *   and More... for the Settings app
 * - Freestanding C, zero heap allocations, zero FPU in M-mode.
 */

#include "kernel/keyseq.h"
#include "drivers/view.h"
#include "kernel/apps.h"
#include "kernel/settings.h"
#include "drivers/icons.h"
#include "drivers/launcher_view.h"
#include "drivers/winclose.h"
#include "kernel/console.h"
#include "kernel/lock.h"
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
void launcher_gallery(bool on) { (void)on; }
void launcher_alert(const char *title, const char *msg) { (void)title; (void)msg; }
void launcher_ask(const char *icon, const char *title, const char *msg, const char *const *buttons, int count,
                  int cancel, launcher_ask_fn done, void *arg) {
    (void)icon; (void)title; (void)msg; (void)buttons; (void)count; (void)cancel; (void)done; (void)arg;
}
void launcher_ask_dismiss(launcher_ask_fn done, void *arg) { (void)done; (void)arg; }
bool launcher_drag_target(int client_x, int client_y) { (void)client_x; (void)client_y; return false; }
void launcher_pointer_move(int client_x, int client_y) { (void)client_x; (void)client_y; }
void launcher_drag(int dy) { (void)dy; }
void launcher_drag_end(int dy, uint32_t ms) { (void)dy; (void)ms; }
static void launcher_view_none(uint8_t inst, const win_event_t *ev) { (void)inst; (void)ev; }
static void launcher_view_noop(uint8_t inst) { (void)inst; }
static void launcher_view_noresize(uint8_t inst, int w, int h) { (void)inst; (void)w; (void)h; }
static uint8_t *launcher_view_nosurface(uint8_t inst, uint32_t *s, int *w, int *h) { (void)inst; (void)s; (void)w; (void)h; return NULL; }
static uint32_t launcher_view_noseq(uint8_t inst) { (void)inst; return 0; }
static bool launcher_view_active(uint8_t inst) { (void)inst; return false; }
const view_ops_t g_launcher_view = { .name = "launcher", .active = launcher_view_active, .event = launcher_view_none,
                                     .captures = NULL, .resize = launcher_view_noresize, .close = launcher_view_noop,
                                     .surface = launcher_view_nosurface, .seq = launcher_view_noseq };
bool launcher_poll_exec(char *out_cmd, size_t max_len) { (void)out_cmd; (void)max_len; return false; }
uint8_t *launcher_get_aa_backing(uint32_t *out_stride, int *out_w, int *out_h) { (void)out_stride; (void)out_w; (void)out_h; return NULL; }
uint32_t launcher_get_seq(void) { return 0; }
void launcher_report(void) { cprintf("launcher: not present on headless board\n"); }

#else

#include "drivers/ugui.h"
#include "drivers/reader_view.h"
#include "drivers/settings_view.h"
#include "drivers/ribbon.h"
#include "drivers/screen.h"
#include "drivers/lcdterm.h"
#include "drivers/ramscreen.h"
#include "drivers/battery.h"
#include "drivers/usbkbd_task.h"
#include "kernel/vterm.h"
#include "fs/vfs.h"
#include "kernel/console.h"
#include "kernel/lock.h"
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
#define LAUNCHER_PATH_MAX       96      /* the help folders' paths; a longer one is left out */

#define LAUNCHER_ID_APP_READER   1
#define LAUNCHER_ID_APP_CHESS    2
#define LAUNCHER_ID_APP_MOONBASE 3
#define LAUNCHER_ID_APP_TERMINAL 4
#define LAUNCHER_ID_VOL_STEP     10
#define LAUNCHER_ID_BL_STEP      20
#define LAUNCHER_ID_ORIENT_SEG   30
#define LAUNCHER_ID_DOC_REFRESH  40
#define LAUNCHER_ID_SCROLLBAR    41     /* the document list's: press or drag to scroll there */
#define LAUNCHER_ID_SETTINGS     42     /* the Settings app (drivers/settings_view.h) */
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
#define GAL_ID_EDIT      560     /* + item: the text field's edit menu */
#define GAL_ID_ICONS     570     /* the icon grid (index: the icon) */
#define LAUNCHER_ID_ALERT_OK 540     /* + button */
#define LAUNCHER_ID_SHEET    550     /* + item: a document's action sheet */
#define LAUNCHER_ALERT_MAX   4

typedef struct {
    char path[LAUNCHER_PATH_MAX];
    char label[48];
    char badge[12];
} launcher_doc_t;

typedef struct {
    bool           active;
    char           alert_title[48];          /* a dialog over the launcher ("" none) */
    char           alert_msg[200];
    const char    *alert_icon;               /* its icon's name, NULL: none */
    const char *const *alert_buttons;        /* from the right, the last the default */
    int            alert_count, alert_cancel; /* Esc's choice */
    launcher_ask_fn alert_done;              /* told the choice, NULL: none */
    void          *alert_arg;
    struct {                                   /* the control gallery's state */
        bool        on, toggle, check, dialog, sheet, edit;
        int         tab, slider, icon_sel;
        char        text[64];
        ugui_text_t field;
        char        result[64];
    } gal;
    uint8_t        slot_id;
    int            win_w;
    int            win_h;
    int            scroll_px;      /* the document list's scroll, pixels */
    int            item_h, view_h; /* its row and viewport heights, as last drawn */
    int            sb_y, sb_h;     /* its scrollbar's track, as last drawn (sb_h 0: none) */
    int            list_x, list_y, list_w, list_avail;   /* draw_doc_list()'s place, as last drawn */
    int            doc_count;
    launcher_doc_t docs[LAUNCHER_MAX_DOCS];
    int            sheet_doc;      /* a document's action sheet is up: its index + 1 (0: none) */
    bool           osk_want;       /* a text field has the focus: the on-screen keyboard asked for */
    const char    *sheet_items[3]; /* Open, Open in <the other app>, Cancel */
    ugui_ctx_t     ui_ctx;
    uint8_t       *aa_buf;         /* GRAY8: what the panel composites (the front) */
    uint8_t       *aa_back;        /* the one a full draw goes into, then swapped in; NULL: draw in place */
    uint8_t       *aa_target;      /* the one launcher_draw() is drawing into */
    uint32_t       aa_stride;
    int            aa_w;
    int            aa_h;
    uint32_t       aa_pages;
    uint32_t       seq;
} launcher_state_t;

/* single task: views (drivers/view.h) -- every change and every draw. Also
 * under console_lock: opening, closing and the surfaces' pointers, which the
 * compositor reads under it. */
static launcher_state_t s_launcher BULK_BSS;

/* An application card's command line, for the shell (launcher_poll_exec()). */
static char       s_pending_cmd[64] BULK_BSS;           /* guarded by: s_pending_lock */
static spinlock_t s_pending_lock = SPINLOCK_INIT;

/* ASCII case folded: FAT names match in any case, and 8.3 names may come in capitals. */
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
    char name[VFS_NAME_MAX];
    vfs_stat_t st;
    for (uint32_t idx = 0; idx < 256 && s_launcher.doc_count < LAUNCHER_MAX_DOCS; idx++) {
        if (vfs_readdir(fd, idx, name, sizeof(name), &st) < 0) break;
        if (st.is_dir) continue;
        if (str_ends_with(name, ".md") || str_ends_with(name, ".gmi")) {
            char full[LAUNCHER_PATH_MAX];
            /* A path cut short would open nothing, or another file: left out. */
            if (strlen(dir_path) + 1u + strlen(name) >= sizeof(full)) continue;
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

/* The quick settings are the settings store's (kernel/settings.h): applied
 * there, kept across reboots. */
static unsigned get_backlight_percent(void) {
    return (unsigned)settings_get("display.brightness");
}

static void set_backlight_percent(int pct) {
    (void)settings_set("display.brightness", pct);
}

static int get_volume_percent(void) {
    return settings_get("audio.volume");
}

static void set_volume_percent(int pct) {
    (void)settings_set("audio.volume", pct);
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

    /* A window of its own, or a reader with room; else say why. */
    if (reader_open_window(doc_path, NULL) < 0)
        launcher_alert("The document did not open.",
                       "It may be missing, or there is no room for another window:\nclose one first (its close box, or Cmd-W).");
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
    ugui_rect_t track = { (int16_t)(x + list_w + 4 * s), (int16_t)y, (int16_t)sb_w, (int16_t)view_h };
    ugui_scrollbar(surf, track, s_launcher.scroll_px, total, view.h);
    s_launcher.sb_y = y;
    s_launcher.sb_h = total > view.h ? view_h : 0;
    if (s_launcher.sb_h) ugui_add_drag_hit(&s_launcher.ui_ctx, track, LAUNCHER_ID_SCROLLBAR);
}

static void launcher_update_list(void);

/* The scrollbar pressed or dragged at y: the thumb's middle there. */
static void launcher_scrollbar_to(int y) {
    if (s_launcher.sb_h <= 0 || s_launcher.item_h <= 0) return;
    int total = s_launcher.doc_count * s_launcher.item_h, view = s_launcher.view_h;
    int range = total - view;
    if (range <= 0) return;
    int thumb = s_launcher.sb_h * view / total, min_thumb = 16 * (int)CHROME_SCALE;
    if (thumb < min_thumb) thumb = min_thumb;
    if (thumb > s_launcher.sb_h) thumb = s_launcher.sb_h;
    int travel = s_launcher.sb_h - thumb;
    int pos = travel > 0 ? (y - s_launcher.sb_y - thumb / 2) * range / travel : 0;
    if (pos < 0) pos = 0;
    if (pos > range) pos = range;
    if (pos == s_launcher.scroll_px) return;
    s_launcher.scroll_px = pos;
    launcher_update_list();
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

static const char *const k_alert_ok[1] = { "OK" };

/* The launcher's alert, if one is up: modal over the rest, its icon (an
 * alert's 32 units) left of the text. */
static void launcher_draw_alert(const ugui_surface_t *surf) {
    if (!s_launcher.alert_title[0]) return;
    int sc = 1;
    ugui_image_t img = icon_image(s_launcher.alert_icon ? icon_find(s_launcher.alert_icon) : NULL,
                                  32 * CHROME_SCALE, &sc);
    (void)ugui_alert(surf, &s_launcher.ui_ctx, img.px ? &img : NULL, sc, s_launcher.alert_title,
                     s_launcher.alert_msg, s_launcher.alert_buttons, s_launcher.alert_count, LAUNCHER_ID_ALERT_OK);
}

void launcher_ask(const char *icon, const char *title, const char *msg, const char *const *buttons, int count,
                  int cancel, launcher_ask_fn done, void *arg) {
    if (s_launcher.active) {                             /* brought to the front, not scanned again */
        uint8_t req[2] = { 'U', 0 }, reply[SCREEN_REPLY_LEN];
        (void)console_canvas(req, 2, reply);
        console_set_active_win_type(RIBBON_WIN_LAUNCHER);
    } else if (!launcher_open()) {
        return;
    }
    strncpy(s_launcher.alert_title, title ? title : "", sizeof s_launcher.alert_title - 1);
    s_launcher.alert_title[sizeof s_launcher.alert_title - 1] = '\0';
    strncpy(s_launcher.alert_msg, msg ? msg : "", sizeof s_launcher.alert_msg - 1);
    s_launcher.alert_msg[sizeof s_launcher.alert_msg - 1] = '\0';
    s_launcher.alert_icon = icon;
    s_launcher.alert_buttons = buttons;
    s_launcher.alert_count = count > 0 && count <= LAUNCHER_ALERT_MAX ? count : 1;
    s_launcher.alert_cancel = cancel;
    s_launcher.alert_done = done;
    s_launcher.alert_arg = arg;
    if (!buttons) s_launcher.alert_buttons = k_alert_ok;
    launcher_update();
}

void launcher_alert(const char *title, const char *msg) {
    if (!s_launcher.active) return;
    launcher_ask(NULL, title, msg, k_alert_ok, 1, 0, NULL, NULL);
}

void launcher_ask_dismiss(launcher_ask_fn done, void *arg) {
    if (!s_launcher.alert_title[0] || s_launcher.alert_done != done || s_launcher.alert_arg != arg) return;
    s_launcher.alert_title[0] = '\0';
    s_launcher.alert_done = NULL;
    launcher_update();
}

/* The alert answered with button `choice`: gone, then its asker told. */
static void launcher_alert_answer(int choice) {
    launcher_ask_fn done = s_launcher.alert_done;
    void *arg = s_launcher.alert_arg;
    s_launcher.alert_title[0] = '\0';
    s_launcher.alert_done = NULL;
    launcher_update();
    if (done) done(choice, arg);
}

/* An application's card: its registered title and icon (32 units). */
static void app_card(const ugui_surface_t *surf, ugui_rect_t r, const char *app_id, const char *subtitle, uint16_t id) {
    const app_desc_t *a = app_find(app_id);
    int scale = 1;
    ugui_image_t img = icon_image(icon_or(a ? a->icon : NULL, "app"), 32 * surf->scale, &scale);
    ugui_card_img(surf, &s_launcher.ui_ctx, r, &img, scale, a ? a->title : app_id, subtitle, id, false);
}

/* The applications and quick settings, cut off the top of *col (which is
 * left with what remains). One description for both orientations. */
static void draw_apps_and_settings(const ugui_surface_t *surf, ugui_rect_t *col) {
    const int s = surf->scale;
    ugui_ctx_t *ctx = &s_launcher.ui_ctx;
    ugui_rect_t hdr = ugui_cut_top(col, 24 * s);
    ugui_section_header(surf, hdr.x, hdr.y, hdr.w, "APPLICATIONS");
    ugui_rect_t cards = ugui_cut_top(col, 2 * 56 * s + 8 * s);
    app_card(surf, ugui_grid_cell(cards, 2, 2, 8 * s, 0), "reader", "Docs", LAUNCHER_ID_APP_READER);
    app_card(surf, ugui_grid_cell(cards, 2, 2, 8 * s, 1), "chess", "Game", LAUNCHER_ID_APP_CHESS);
    app_card(surf, ugui_grid_cell(cards, 2, 2, 8 * s, 2), "moonbase", "Game", LAUNCHER_ID_APP_MOONBASE);
    app_card(surf, ugui_grid_cell(cards, 2, 2, 8 * s, 3), "term", "Shell", LAUNCHER_ID_APP_TERMINAL);
    (void)ugui_cut_top(col, 14 * s);

    hdr = ugui_cut_top(col, 26 * s);
    ugui_rect_t more = ugui_cut_right(&hdr, 66 * s);
    (void)ugui_cut_right(&hdr, 6 * s);
    ugui_section_header(surf, hdr.x, hdr.y, hdr.w, "QUICK SETTINGS");
    ugui_toolbar_button(surf, ctx, (ugui_rect_t){ more.x, (int16_t)(more.y - 2 * s), more.w, (int16_t)(20 * s) },
                        "More...", LAUNCHER_ID_SETTINGS, false);
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

static const char *const k_gal_tabs[4] = { "Controls", "Text", "Dialogs", "Icons" };
static const char *const k_gal_dlg[2] = { "Cancel", "Delete" };
static const char *const k_gal_sheet[5] = { "Open", "Rename", "Add to Shortcuts", "Delete", "Cancel" };
static const char *const k_gal_edit[5] = { "Cut", "Copy", "Paste", "Select All", "Cancel" };
static const char k_gal_path[] = "/sd0/docs/books/german/goethe";

static void gal_line(const ugui_surface_t *surf, int x, int y, const char *text, bool muted) {
    ugui_text_serif(surf, x, y, text, (uint32_t)strlen(text), false, muted, false);
}

static void launcher_draw_gallery(const ugui_surface_t *surf, int w, int h) {
    const int s = surf->scale;
    ugui_ctx_t *ctx = &s_launcher.ui_ctx;
    int pad = 12 * s, col = w - 2 * pad > 440 * s ? 440 * s : w - 2 * pad, row = 32 * s, y = pad;
    ugui_tab_bar(surf, ctx, (ugui_rect_t){ (int16_t)pad, (int16_t)y, (int16_t)(w - 2 * pad - 90 * s), (int16_t)(26 * s) },
                 k_gal_tabs, 4, s_launcher.gal.tab, GAL_ID_TAB);
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
        gal_line(surf, pad, y, "Tap the field (or Tab to it) and type: the on-screen or a USB keyboard;", true);
        y += ugui_serif_line_h(surf);
        gal_line(surf, pad, y, "Shift+arrows select, Cmd+C/X/V copy, cut, paste; Enter submits, Esc cancels.", true);
        y += ugui_serif_line_h(surf);
        gal_line(surf, pad, y, "A long press or a right click: Cut, Copy, Paste, Select All.", true);
        y += ugui_serif_line_h(surf) + 8 * s;
    } else if (s_launcher.gal.tab == 3) {
        /* The built-in icons as an icon grid (the Shortcuts face's): a tap
         * selects one, a long press or a right click names it. */
        ugui_grid_item_t items[48];
        int n = 0;
        for (unsigned i = 0; icon_at(i) && n < 48; i++, n++) {
            const icon_t *ic = icon_at(i);
            items[n].scale = 1;
            items[n].img = icon_image(ic, 32 * s, &items[n].scale);
            items[n].label = ic->name;
        }
        int cell_w = 96 * s, cell_h = 32 * s + ugui_serif_line_h(surf) + 14 * s;
        int lh = ugui_serif_line_h(surf) + 4 * s;
        ugui_rect_t grid = { (int16_t)pad, (int16_t)y, (int16_t)(w - 2 * pad), (int16_t)(h - y - lh - pad) };
        (void)ugui_icon_grid(surf, ctx, grid, items, n, cell_w, cell_h, s_launcher.gal.icon_sel, GAL_ID_ICONS);
        if (s_launcher.gal.result[0]) gal_line(surf, pad, h - pad - lh, s_launcher.gal.result, false);
        return;
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
    else if (s_launcher.gal.edit)
        (void)ugui_action_sheet(surf, ctx, "Edit", k_gal_edit, 5, GAL_ID_EDIT);
}

/* A gallery control acted on (a tap at x, or Enter: x < 0). */
static bool launcher_gallery_act(uint16_t id, int16_t idx, int x) {
    char *res = s_launcher.gal.result;
    if (id >= GAL_ID_TAB && id < GAL_ID_TAB + 4) { s_launcher.gal.tab = id - GAL_ID_TAB; res[0] = '\0'; }
    else if (id == GAL_ID_DONE) { s_launcher.gal.on = false; s_launcher.gal.dialog = s_launcher.gal.sheet = s_launcher.gal.edit = false; }
    else if (id >= GAL_ID_EDIT && id < GAL_ID_EDIT + 5) {
        static const char k_edit_keys[4] = { 'x', 'c', 'v', 'a' };     /* Cut, Copy, Paste, Select All */
        s_launcher.gal.edit = false;
        if (id < GAL_ID_EDIT + 4) (void)ugui_text_key(&s_launcher.gal.field, (uint32_t)k_edit_keys[id - GAL_ID_EDIT], KMOD_SUPER);
        ugui_set_focus(&s_launcher.ui_ctx, GAL_ID_TEXT, 0);
    }
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
    else if (id == GAL_ID_ICONS && icon_at((unsigned)idx)) {
        s_launcher.gal.icon_sel = idx;
        ksnprintf(res, 64, "Icon: %s", icon_at((unsigned)idx)->name);
    }
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
    if (k.key == 0x1b && (s_launcher.gal.dialog || s_launcher.gal.sheet || s_launcher.gal.edit)) {
        s_launcher.gal.dialog = s_launcher.gal.sheet = s_launcher.gal.edit = false;
        ksnprintf(s_launcher.gal.result, 64, "Cancelled");
        launcher_update();
        return true;
    }
    if (s_launcher.gal.tab == 1 && ctx->focus_id == GAL_ID_TEXT && !s_launcher.gal.dialog && !s_launcher.gal.sheet &&
        !s_launcher.gal.edit) {
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
 * views task may be redrawing: the hit-boxes are a bounded array, and a
 * stale answer costs one contact taken as a gesture or a drag. */
bool launcher_drag_target(int client_x, int client_y) {
    return s_launcher.active && ugui_drag_at(&s_launcher.ui_ctx, client_x, client_y);
}

/* A captured finger at (client_x, client_y): the armed slider follows it. */
void launcher_pointer_move(int client_x, int client_y) {
    if (s_launcher.active && s_launcher.ui_ctx.armed_id == LAUNCHER_ID_SCROLLBAR) {
        launcher_scrollbar_to(client_y);
        return;
    }
    if (!s_launcher.active || !s_launcher.gal.on || s_launcher.ui_ctx.armed_id != GAL_ID_SLIDER) return;
    (void)launcher_gallery_act(GAL_ID_SLIDER, 0, client_x);   /* sets it from x, draws */
}

void launcher_gallery(bool on) {
    if (on && !s_launcher.gal.field.buf) {
        strncpy(s_launcher.gal.text, "notes.md", sizeof s_launcher.gal.text);
        ugui_text_init(&s_launcher.gal.field, s_launcher.gal.text, (uint16_t)sizeof s_launcher.gal.text);
        s_launcher.gal.slider = 40;
        s_launcher.gal.icon_sel = -1;
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
        launcher_draw_alert(&surf);
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

    if (s_launcher.sheet_doc > 0 && s_launcher.sheet_doc <= s_launcher.doc_count)
        (void)ugui_action_sheet(&surf, &s_launcher.ui_ctx, s_launcher.docs[s_launcher.sheet_doc - 1].label,
                                s_launcher.sheet_items, 3, LAUNCHER_ID_SHEET);
    launcher_draw_alert(&surf);
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
    if (ctx->focus_shown || s_launcher.alert_title[0] || s_launcher.list_avail <= 0 || (!back && !s_launcher.aa_buf)) {
        launcher_update();
        return;
    }
    ugui_surface_t surf = {
        .c1 = back, .aa_fb = s_launcher.aa_buf, .aa_stride = s_launcher.aa_stride,
        .w = s_launcher.win_w, .h = s_launcher.win_h, .scale = CHROME_SCALE,
    };
    int keep = 0;
    for (int i = 0; i < ctx->count; i++)
        if (ctx->hits[i].id != LAUNCHER_ID_DOC_BASE && ctx->hits[i].id != LAUNCHER_ID_SCROLLBAR) ctx->hits[keep++] = ctx->hits[i];
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

    /* A text field with the focus and nothing over it: the on-screen
     * keyboard under it (where there is touch and no keyboard). */
    bool want = s_launcher.gal.on && s_launcher.gal.tab == 1 && s_launcher.ui_ctx.focus_id == GAL_ID_TEXT &&
                !s_launcher.gal.dialog && !s_launcher.gal.sheet && !s_launcher.gal.edit && !s_launcher.alert_title[0];
    if (want != s_launcher.osk_want) {
        s_launcher.osk_want = want;
        console_osk_for_view(RIBBON_WIN_LAUNCHER, s_launcher.slot_id, want);
    }
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
    s_launcher.sheet_doc = 0;
    s_launcher.ui_ctx.hover_id = 0;
    if (s_launcher.osk_want) {
        s_launcher.osk_want = false;
        console_osk_for_view(RIBBON_WIN_LAUNCHER, s_launcher.slot_id, false);
    }
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
    if (s_launcher.ui_ctx.armed_id == LAUNCHER_ID_SCROLLBAR) launcher_scrollbar_to(client_y);
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
    int windows = 1, canvases = 1;
    if ((a->caps & APP_WINDOW) && lcdterm_room(&windows, &canvases) && (windows < 1 || canvases < 1)) {
        launcher_alert("No room for another window.",
                       "Close a window first: its close box, or Cmd-W.");
        return false;
    }
    char cmd[sizeof s_pending_cmd];
    if (!app_command(a, path, cmd, sizeof cmd)) return false;
    uintptr_t f = spin_lock_irqsave(&s_pending_lock);
    memcpy(s_pending_cmd, cmd, sizeof cmd);
    spin_unlock_irqrestore(&s_pending_lock, f);
    return true;
}
static bool launcher_activate(uint16_t id, int16_t hit_idx);
static void launcher_sheet_answer(int choice);

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
    if (s_launcher.alert_title[0]) {                     /* modal: its buttons only */
        if (id >= LAUNCHER_ID_ALERT_OK && id < LAUNCHER_ID_ALERT_OK + s_launcher.alert_count)
            launcher_alert_answer(id - LAUNCHER_ID_ALERT_OK);
        return id != 0;
    }
    if (s_launcher.gal.on) return id && launcher_gallery_act(id, hit_idx, client_x);
    if (s_launcher.sheet_doc > 0) {                      /* modal: a choice, or away */
        launcher_sheet_answer(id >= LAUNCHER_ID_SHEET && id < LAUNCHER_ID_SHEET + 3 ? id - LAUNCHER_ID_SHEET : 2);
        return true;
    }
    return launcher_activate(id, hit_idx);
}

/* A document's other application: the editor for what the reader shows,
 * the reader for the rest. */
static const char *other_app(const char *path) {
    const app_desc_t *a = app_for_path(path);
    return a && strcmp(a->id, "reader") == 0 ? "editor" : "reader";
}

/* The context menu (a right click, a long press) at (x, y): a document's
 * action sheet; nothing elsewhere yet. */
static void launcher_context(int client_x, int client_y) {
    if (!s_launcher.active || s_launcher.alert_title[0] || s_launcher.sheet_doc > 0) return;
    int16_t hit_idx = -1;
    uint16_t id = ugui_hit_test(&s_launcher.ui_ctx, client_x, client_y, &hit_idx);
    if (s_launcher.gal.on) {                             /* the text field's edit menu; an icon's name */
        if (id == GAL_ID_ICONS && icon_at((unsigned)hit_idx)) {
            ksnprintf(s_launcher.gal.result, 64, "Long press: %s", icon_at((unsigned)hit_idx)->name);
            (void)ugui_disarm(&s_launcher.ui_ctx);
            launcher_update();
            return;
        }
        if (id != GAL_ID_TEXT || s_launcher.gal.dialog || s_launcher.gal.sheet || s_launcher.gal.edit) return;
        ugui_set_focus(&s_launcher.ui_ctx, GAL_ID_TEXT, 0);
        s_launcher.gal.edit = true;
        (void)ugui_disarm(&s_launcher.ui_ctx);
        launcher_update();
        return;
    }
    if (id != LAUNCHER_ID_DOC_BASE || hit_idx < 0 || hit_idx >= s_launcher.doc_count) return;
    s_launcher.sheet_items[0] = "Open";
    s_launcher.sheet_items[1] = strcmp(other_app(s_launcher.docs[hit_idx].path), "editor") == 0 ? "Open in Editor" : "Open in Reader";
    s_launcher.sheet_items[2] = "Cancel";
    s_launcher.sheet_doc = hit_idx + 1;
    (void)ugui_disarm(&s_launcher.ui_ctx);
    launcher_update();
}

/* The sheet's item `choice`: gone, then done. */
static void launcher_sheet_answer(int choice) {
    int i = s_launcher.sheet_doc - 1;
    s_launcher.sheet_doc = 0;
    launcher_update();
    if (i < 0 || i >= s_launcher.doc_count) return;
    const char *path = s_launcher.docs[i].path;
    if (choice == 0) {
        (void)launcher_activate(LAUNCHER_ID_DOC_BASE, (int16_t)i);
    } else if (choice == 1) {
        if (strcmp(other_app(path), "reader") == 0) open_doc_in_reader(path);
        else (void)launch_app("editor", path);
    }
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
        int windows = 1;
        if (lcdterm_room(&windows, NULL) && windows < 1) {
            launcher_alert("No room for another window.", "Close a window first: its close box, or Cmd-W.");
            return false;
        }
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
        hit_idx = (int16_t)(id - LAUNCHER_ID_ORIENT_SEG);   /* landscape, portrait, auto: display.orient's */
        (void)settings_set("display.orient", hit_idx);
        if (hit_idx == 2) launcher_update();             /* a rotation redraws it anyway */
        return true;
    }
#endif
    if (id == LAUNCHER_ID_SETTINGS) {
        if (!settings_view_open())
            launcher_alert("Settings did not open.", "There is no room for another window:\nclose one first (its close box, or Cmd-W).");
        return true;
    }
    if (id == LAUNCHER_ID_SCROLLBAR) return true;     /* launcher_press() moved it already */
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
    if (s_launcher.alert_title[0]) {                     /* Esc: its cancel; Enter: the focused button, or the default */
        if (k.key == 0x1b) { launcher_alert_answer(s_launcher.alert_cancel); return; }
        uint16_t id = 0;
        int16_t idx = 0;
        ugui_nav_t nav = ugui_nav(&s_launcher.ui_ctx, k.key, k.mods, &id, &idx);
        if (nav == UGUI_NAV_ACTIVATE && id >= LAUNCHER_ID_ALERT_OK && id < LAUNCHER_ID_ALERT_OK + s_launcher.alert_count)
            launcher_alert_answer(id - LAUNCHER_ID_ALERT_OK);
        else if ((k.key == '\r' || k.key == '\n') && nav != UGUI_NAV_ACTIVATE)
            launcher_alert_answer(s_launcher.alert_count - 1);
        else if (nav == UGUI_NAV_MOVED)
            launcher_update();
        return;
    }
    if (s_launcher.sheet_doc > 0) {                      /* Esc: Cancel; Enter: the focused item */
        if (k.key == 0x1b) { launcher_sheet_answer(2); return; }
        uint16_t id = 0;
        int16_t idx = 0;
        ugui_nav_t nav = ugui_nav(&s_launcher.ui_ctx, k.key, k.mods, &id, &idx);
        if (nav == UGUI_NAV_ACTIVATE && id >= LAUNCHER_ID_SHEET && id < LAUNCHER_ID_SHEET + 3)
            launcher_sheet_answer(id - LAUNCHER_ID_SHEET);
        else if (nav == UGUI_NAV_MOVED)
            launcher_update();
        return;
    }
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

static bool launcher_view_active(uint8_t inst) {
    (void)inst;
    return launcher_is_active();
}

static bool launcher_view_captures(uint8_t inst, int x, int y) {
    (void)inst;
    return launcher_drag_target(x, y);
}

/* The launcher as a view (drivers/view.h): each event onto what it did. */
static void launcher_view_event(uint8_t inst, const win_event_t *ev) {
    (void)inst;
    if (!s_launcher.active) return;
    switch (ev->type) {
    case WIN_EV_POINTER_DOWN:
        if (ev->button == 1 && (ev->flags & WIN_EVF_PRESS)) launcher_press(ev->x, ev->y);
        return;
    case WIN_EV_POINTER_UP:
        if (ev->button == 2) { if (ev->flags & WIN_EVF_TAP) launcher_context(ev->x, ev->y); }
        else if (ev->flags & WIN_EVF_TAP) (void)launcher_tap(ev->x, ev->y);
        else if (ev->flags & WIN_EVF_PRESS) launcher_release();
        return;
    case WIN_EV_POINTER_MOVE:
        if (ev->button == 0) { if (ugui_hover(&s_launcher.ui_ctx, ev->x, ev->y)) launcher_update(); }
        else launcher_pointer_move(ev->x, ev->y);
        return;
    case WIN_EV_DRAG:
        launcher_drag(ev->dy);
        return;
    case WIN_EV_DRAG_END:
        launcher_drag_end(ev->dy, ev->key);
        return;
    case WIN_EV_SCROLL:
        if (ev->flags & WIN_EVF_WHEEL)                   /* a step a notch, a few at the most */
            for (int n = ev->dy < 0 ? -ev->dy : ev->dy, i = 0; i < n && i < 4; i++) launcher_scroll(ev->dy > 0 ? -1 : 1);
        else
            launcher_scroll(ev->dy > 0 ? -1 : 1);
        return;
    case WIN_EV_KEY_DOWN: {
        key_event_t k = { ev->key, ev->mods };
        launcher_key(k);
        return;
    }
    default:
        return;
    }
}

static void launcher_view_resize(uint8_t inst, int w, int h) {
    if (launcher_set_target(inst, w, h)) launcher_update();
}

static void launcher_view_close(uint8_t inst) {
    (void)inst;
    launcher_close();
}

static uint8_t *launcher_view_surface(uint8_t inst, uint32_t *stride, int *w, int *h) {
    (void)inst;
    return launcher_get_aa_backing(stride, w, h);
}

static uint32_t launcher_view_seq(uint8_t inst) {
    (void)inst;
    return launcher_get_seq();
}

const view_ops_t g_launcher_view = {
    .name = "launcher",
    .active = launcher_view_active,
    .event = launcher_view_event,
    .captures = launcher_view_captures,
    .resize = launcher_view_resize,
    .close = launcher_view_close,
    .surface = launcher_view_surface,
    .seq = launcher_view_seq,
};

bool launcher_poll_exec(char *out_cmd, size_t max_len) {
    if (!out_cmd || max_len == 0) return false;
    uintptr_t f = spin_lock_irqsave(&s_pending_lock);
    bool got = s_pending_cmd[0] != '\0';
    if (got) {
        strncpy(out_cmd, s_pending_cmd, max_len - 1);
        out_cmd[max_len - 1] = '\0';
        s_pending_cmd[0] = '\0';
    }
    spin_unlock_irqrestore(&s_pending_lock, f);
    return got;
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
    if (s_launcher.alert_title[0]) cprintf("  alert: %s\n", s_launcher.alert_title);
    if (s_launcher.sheet_doc > 0 && s_launcher.sheet_doc <= s_launcher.doc_count)
        cprintf("  sheet: %s\n", s_launcher.docs[s_launcher.sheet_doc - 1].label);
    if (s_launcher.ui_ctx.hover_id)
        cprintf("  hover: id %u index %d\n", (unsigned)s_launcher.ui_ctx.hover_id, (int)s_launcher.ui_ctx.hover_index);
    if (s_launcher.gal.on && s_launcher.gal.field.buf)
        cprintf("  gallery field: \"%s\" caret %u anchor %u%s%s\n", s_launcher.gal.text, (unsigned)s_launcher.gal.field.caret,
                (unsigned)s_launcher.gal.field.anchor, s_launcher.ui_ctx.focus_id == GAL_ID_TEXT ? ", focused" : "",
                s_launcher.gal.edit ? ", edit menu up" : "");
    winclose_report();
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
