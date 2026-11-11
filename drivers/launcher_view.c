/* drivers/launcher_view.c -- the launcher (RIBBON_WIN_LAUNCHER), the home
 * view, drawn with ugui. Two faces, a tab bar between them (launcher.face):
 *
 * - Shortcuts: an icon grid -- a list in a narrow window -- of the shortcuts
 *   (drivers/shortcuts.h): applications, documents, folders. A tap opens
 *   one; a long press, a right click, Shift+Enter or `m` gives Open, Rename,
 *   Remove, Move Left / Right.
 * - Files: the mounted volumes, and the directories under them: a path bar
 *   of tappable segments over a list (dirs first, then by name; a Markdown
 *   file by its first heading). A tap opens by association; the sheet adds
 *   Open in Editor / Reader, Add to Shortcuts, Info and Delete (confirmed);
 *   Shift+Enter opens it. Backspace, Esc, or Left on a row goes up; a letter
 *   jumps to the next entry with it.
 *
 * Ctrl+Tab switches the faces (Cmd+1..9 are the window manager's). Also the
 * alerts over it (launcher_ask(): the not-responding alert's), and the
 * control gallery (`launcher gallery`). */

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
bool launcher_bench_show(int face, const char *dir, int *was_face, char *was_dir, unsigned cap) {
    (void)face; (void)dir; (void)was_face; (void)was_dir; (void)cap;
    return false;
}
int launcher_bench_files(void) { return 0; }

#else

#include "drivers/ugui.h"
#include "drivers/reader_view.h"
#include "drivers/settings_view.h"
#include "drivers/shortcuts.h"
#include "drivers/ribbon.h"
#include "drivers/screen.h"
#include "drivers/lcdterm.h"
#include "drivers/ramscreen.h"
#include "drivers/battery.h"
#include "drivers/usbkbd_task.h"
#include "drivers/uxbench.h"
#include "kernel/time.h"
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

#define LAUNCHER_FILES_MAX      512     /* a directory's entries shown (more: the first ones, and said so) */
#define LAUNCHER_NAME_MAX       96      /* a long name, as far as a row shows it */
#define LAUNCHER_TITLES_MAX     64      /* Markdown files a listing reads a heading from */

#define LAUNCHER_ID_TAB          1      /* + face */
#define LAUNCHER_ID_SHORTCUT     10     /* index: the shortcut */
#define LAUNCHER_ID_PATH         20     /* index: the path bar's segment */
#define LAUNCHER_ID_FILE         30     /* index: the row */
#define LAUNCHER_ID_SCROLLBAR    41     /* the list's: press or drag to scroll there */
#define LAUNCHER_ID_RENAME       42     /* the rename dialog's text field */
#define LAUNCHER_ID_RENAME_BTN   43     /* + 0 Cancel, + 1 Rename */
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
#define LAUNCHER_ID_SHEET    550     /* + item: an action sheet's */
#define LAUNCHER_ALERT_MAX   4
#define LAUNCHER_SHEET_MAX   8

enum { FACE_SHORTCUTS, FACE_FILES };
enum { ENTRY_FILE, ENTRY_DIR, ENTRY_ROOT };
/* What an action sheet's item does. */
enum { ACT_OPEN, ACT_OPEN_OTHER, ACT_RENAME, ACT_REMOVE, ACT_MOVE_LEFT, ACT_MOVE_RIGHT, ACT_ADD_SHORTCUT,
       ACT_INFO, ACT_DELETE, ACT_CANCEL };
enum { SHEET_NONE, SHEET_SHORTCUT, SHEET_FILE };

typedef struct {
    char     name[LAUNCHER_NAME_MAX];  /* its name in the directory; a volume's name for a root */
    char     title[48];                /* a Markdown file's first "# " heading, a root's label; "" none */
    uint32_t size;
    uint8_t  kind;                     /* ENTRY_* */
} launcher_entry_t;

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
    int            face;           /* FACE_*: launcher.face */
    /* The Shortcuts face: its scroll, and its grid's place as last drawn. */
    int            sc_scroll, sc_content_h;
    ugui_rect_t    sc_view;
    /* The Files face: the directory shown ("" the roots), its entries in
     * order (dirs first, then by name), the list's scroll and geometry. */
    char           cwd[VFS_PATH_MAX];
    launcher_entry_t entries[LAUNCHER_FILES_MAX];
    int            n_entries;
    bool           truncated;      /* the directory had more than LAUNCHER_FILES_MAX */
    bool           files_loaded;
    int            scroll_px;      /* the list's scroll, pixels */
    int            item_h, view_h; /* its row and viewport heights, as last drawn */
    int            list_y;         /* its viewport's top, as last drawn */
    int            sb_y, sb_h;     /* its scrollbar's track, as last drawn (sb_h 0: none) */
    /* An action sheet over either face. */
    int            sheet;          /* SHEET_* */
    int            sheet_idx;      /* the shortcut's or the entry's index */
    int            sheet_n;
    const char    *sheet_items[LAUNCHER_SHEET_MAX];
    uint8_t        sheet_act[LAUNCHER_SHEET_MAX];
    char           sheet_title[LAUNCHER_NAME_MAX];
    /* Renaming a shortcut: a dialog with a text field. */
    bool           renaming;
    int            rename_idx;
    char           rename_buf[SHORTCUT_LABEL];
    ugui_text_t    rename_field;
    int            pending_delete; /* the entry a Delete confirmation is about, -1 none */
    int            pending_remove; /* the shortcut a Remove confirmation is about, -1 none */
    char           info[200];      /* the text of the alert up (Info, Remove, Delete): one at a time */
    bool           osk_want;       /* a text field has the focus: the on-screen keyboard asked for */
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
static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool same_name(const char *a, const char *b) {
    for (;; a++, b++) {
        if (lower(*a) != lower(*b)) return false;
        if (!*a) return true;
    }
}

static bool str_ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s);
    size_t xl = strlen(suffix);
    if (sl < xl) return false;
    return same_name(s + sl - xl, suffix);
}


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
    s_launcher.pending_delete = -1;
    s_launcher.pending_remove = -1;
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

/* --- The Files face's model -------------------------------------------------- */

/* path + "/" + name into out; false when it does not fit (a path cut short
 * would open another file, or none). */
static bool join_path(char *out, unsigned cap, const char *dir, const char *name) {
    unsigned dl = (unsigned)strlen(dir), nl = (unsigned)strlen(name);
    if (dl + 1u + nl + 1u > cap) return false;
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1, name, nl + 1);
    return true;
}

/* An entry's path ("/sd0" for a root). */
static bool entry_path(int i, char *out, unsigned cap) {
    const launcher_entry_t *e = &s_launcher.entries[i];
    if (e->kind == ENTRY_ROOT) return join_path(out, cap, "", e->name);
    return join_path(out, cap, s_launcher.cwd, e->name);
}

/* A Markdown file's first "# " heading, from its first 256 bytes. */
static void read_title(const char *path, char *out, unsigned cap) {
    out[0] = '\0';
    char buf[257];
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return;
    int n = vfs_pread(fd, buf, sizeof buf - 1, 0);
    vfs_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    for (char *p = buf; *p;) {
        while (*p == '\n' || *p == '\r') p++;
        if (p[0] == '#' && p[1] == ' ') {
            p += 2;
            unsigned k = 0;
            while (p[k] && p[k] != '\n' && p[k] != '\r' && k + 1 < cap) { out[k] = p[k]; k++; }
            out[k] = '\0';
            return;
        }
        while (*p && *p != '\n') p++;
    }
}

/* Directories first, then by name without case, a run of digits as its
 * number: file2 before file10. */
static bool entry_before(const launcher_entry_t *a, const launcher_entry_t *b) {
    if ((a->kind == ENTRY_DIR) != (b->kind == ENTRY_DIR)) return a->kind == ENTRY_DIR;
    const char *x = a->name, *y = b->name;
    for (;;) {
        if (*x >= '0' && *x <= '9' && *y >= '0' && *y <= '9') {
            while (*x == '0') x++;                       /* leading zeros do not count */
            while (*y == '0') y++;
            const char *xe = x, *ye = y;
            while (*xe >= '0' && *xe <= '9') xe++;
            while (*ye >= '0' && *ye <= '9') ye++;
            if (xe - x != ye - y) return xe - x < ye - y;   /* fewer digits: the smaller number */
            for (; x < xe; x++, y++)
                if (*x != *y) return *x < *y;
            continue;
        }
        char cx = lower(*x), cy = lower(*y);
        if (cx != cy) return cx < cy;
        if (!cx) return false;
        x++;
        y++;
    }
}

/* Shell sort on the entries in place: a directory of 500 sorts in a few
 * thousand moves, where an insertion sort would make a hundred thousand. */
static void sort_entries(void) {
    launcher_entry_t *e = s_launcher.entries;
    int n = s_launcher.n_entries;
    static const int k_gaps[] = { 301, 132, 57, 23, 10, 4, 1 };
    for (unsigned g = 0; g < sizeof k_gaps / sizeof k_gaps[0]; g++) {
        int gap = k_gaps[g];
        for (int i = gap; i < n; i++) {
            launcher_entry_t t = e[i];
            int j = i;
            for (; j >= gap && entry_before(&t, &e[j - gap]); j -= gap) e[j] = e[j - gap];
            e[j] = t;
        }
    }
}

/* The roots: every mounted storage volume. */
static void load_roots(void) {
    s_launcher.n_entries = 0;
    s_launcher.truncated = false;
    char name[LAUNCHER_NAME_MAX], label[48];
    int kind;
    for (unsigned i = 0; (kind = vfs_root_at(i, name, sizeof name, label, sizeof label)) >= 0; i++) {
        if (s_launcher.n_entries >= LAUNCHER_FILES_MAX) break;
        launcher_entry_t *e = &s_launcher.entries[s_launcher.n_entries++];
        strncpy(e->name, name, sizeof e->name - 1);
        e->name[sizeof e->name - 1] = '\0';
        strncpy(e->title, label, sizeof e->title - 1);
        e->title[sizeof e->title - 1] = '\0';
        e->size = 0;
        e->kind = ENTRY_ROOT;
    }
}

/* The directory in cwd read again; false when it cannot be opened. */
static bool load_dir(void) {
    s_launcher.files_loaded = true;
    s_launcher.n_entries = 0;
    s_launcher.truncated = false;
    if (!s_launcher.cwd[0]) {
        load_roots();
        return true;
    }
    int fd = vfs_open(s_launcher.cwd, VFS_O_READ);
    if (fd < 0) return false;
    char name[VFS_NAME_MAX];
    vfs_stat_t st;
    for (uint32_t idx = 0;; idx++) {
        if (vfs_readdir(fd, idx, name, sizeof name, &st) < 0) break;
        if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) continue;
        if (strlen(name) >= LAUNCHER_NAME_MAX) continue;          /* not shown cut short: it would not open */
        if (s_launcher.n_entries >= LAUNCHER_FILES_MAX) {
            s_launcher.truncated = true;
            break;
        }
        launcher_entry_t *e = &s_launcher.entries[s_launcher.n_entries++];
        memcpy(e->name, name, strlen(name) + 1);
        e->title[0] = '\0';
        e->size = st.size;
        e->kind = st.is_dir ? ENTRY_DIR : ENTRY_FILE;
    }
    vfs_close(fd);
    sort_entries();
    int titles = 0;
    for (int i = 0; i < s_launcher.n_entries && titles < LAUNCHER_TITLES_MAX; i++) {
        launcher_entry_t *e = &s_launcher.entries[i];
        char path[VFS_PATH_MAX];
        if (e->kind != ENTRY_FILE || !str_ends_with(e->name, ".md") || !entry_path(i, path, sizeof path)) continue;
        read_title(path, e->title, sizeof e->title);
        titles++;
    }
    return true;
}

/* cwd becomes path ("" the roots), listed from the top; on the keyboard's
 * side the focus goes to its first row. */
static void files_go(const char *path) {
    char was[VFS_PATH_MAX];
    strncpy(was, s_launcher.cwd, sizeof was - 1);
    was[sizeof was - 1] = '\0';
    strncpy(s_launcher.cwd, path, sizeof s_launcher.cwd - 1);
    s_launcher.cwd[sizeof s_launcher.cwd - 1] = '\0';
    if (!load_dir()) {
        strncpy(s_launcher.cwd, was, sizeof s_launcher.cwd);
        (void)load_dir();
        launcher_alert("The folder did not open.", "It may be gone, or its volume unmounted.");
        return;
    }
    s_launcher.scroll_px = 0;
    if (s_launcher.n_entries > 0) ugui_set_focus(&s_launcher.ui_ctx, LAUNCHER_ID_FILE, 0);
}

/* Up one directory; from a volume's top, to the roots. */
static void files_up(void) {
    if (!s_launcher.cwd[0]) return;
    unsigned n = 0;                                      /* up to the last '/', not the leading one */
    for (unsigned i = 1; s_launcher.cwd[i]; i++)
        if (s_launcher.cwd[i] == '/') n = i;
    char up[VFS_PATH_MAX];
    memcpy(up, s_launcher.cwd, n);
    up[n] = '\0';
    files_go(up);
}

/* --- Drawing -------------------------------------------------------------------- */

static const char *const k_faces[2] = { "Shortcuts", "Files" };

/* A size as people read it: bytes, KB, MB. */
static void size_text(uint32_t b, char *out, unsigned cap) {
    if (b < 1024u) ksnprintf(out, cap, "%lu B", (unsigned long)b);
    else if (b < 1024u * 1024u) ksnprintf(out, cap, "%lu KB", (unsigned long)((b + 512u) / 1024u));
    else ksnprintf(out, cap, "%lu.%lu MB", (unsigned long)(b >> 20), (unsigned long)(((b & 0xFFFFFu) * 10u) >> 20));
}

/* What a row shows: the heading or the name, and on its right the name (when
 * a heading took its place), the size, "folder", or a volume's label. */
static void entry_texts(const launcher_entry_t *e, const char **main, char *meta, unsigned cap) {
    *main = e->title[0] && e->kind == ENTRY_FILE ? e->title : e->name;
    if (e->kind == ENTRY_ROOT) ksnprintf(meta, cap, "%s", e->title);
    else if (e->kind == ENTRY_DIR) ksnprintf(meta, cap, "folder");
    else if (e->title[0]) ksnprintf(meta, cap, "%s", e->name);
    else size_text(e->size, meta, cap);
}

/* An icon (its 32-unit form) at x, y, drawn as on an armed row when inverted. */
static void draw_icon(const ugui_surface_t *s, int x, int y, const char *name, const char *fallback, bool inverted) {
    int sc = 1;
    ugui_image_t img = icon_image(icon_or(name, fallback), 32 * s->scale, &sc);
    if (img.px) ugui_image4(s, x, y, &img, sc, inverted);
}

static void file_row(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t row, int i, void *user) {
    (void)user;
    const launcher_entry_t *e = &s_launcher.entries[i];
    const int sc = s->scale;
    bool armed = ugui_is_armed(ctx, LAUNCHER_ID_FILE, (int16_t)i);
    ugui_fill(s, row, armed ? UGUI_GRAY_INK : UGUI_GRAY_CARD, armed ? CANVAS1_BLACK : CANVAS1_WHITE);
    ugui_hline(s, row.x, row.x + row.w - 1, row.y + row.h - 1, 1, UGUI_GRAY_TRACK, CANVAS1_GREY);
    int icon = 32 * sc;
    draw_icon(s, row.x + 6 * sc, row.y + (row.h - icon) / 2,
              e->kind == ENTRY_ROOT ? "files" : shortcuts_path_icon(e->name, e->kind == ENTRY_DIR), "doc", armed);
    const char *main;
    char meta[LAUNCHER_NAME_MAX];
    entry_texts(e, &main, meta, sizeof meta);
    int lh = ugui_serif_line_h(s), ty = row.y + (row.h - lh) / 2;
    int mw = ugui_text_serif_width(s, meta, (uint32_t)strlen(meta)) + 3 * sc;   /* the italic's slant past its advance */
    int text_x = row.x + icon + 14 * sc, right = row.x + row.w - 8 * sc;
    if (mw > (right - text_x) / 2) mw = (right - text_x) / 2;              /* the name keeps half the row */
    ugui_rect_t name_r = { (int16_t)text_x, row.y, (int16_t)(right - mw - 12 * sc - text_x), row.h };
    if (name_r.w > 0) {
        ugui_surface_t ns = ugui_clip(s, name_r);
        ugui_text_serif(&ns, text_x, ty, main, (uint32_t)strlen(main), e->kind != ENTRY_FILE, false, armed);
    }
    ugui_surface_t ms = ugui_clip(s, (ugui_rect_t){ (int16_t)(right - mw), row.y, (int16_t)mw, row.h });
    ugui_text_serif(&ms, right - mw, ty, meta, (uint32_t)strlen(meta), false, true, armed);
    ugui_add_hit(ctx, row, LAUNCHER_ID_FILE, (int16_t)i);
}

static void draw_files(const ugui_surface_t *surf, ugui_rect_t r) {
    const int s = surf->scale;
    ugui_ctx_t *ctx = &s_launcher.ui_ctx;
    if (!s_launcher.files_loaded) (void)load_dir();
    ugui_path_bar(surf, ctx, ugui_cut_top(&r, 26 * s), s_launcher.cwd[0] ? s_launcher.cwd : "/", LAUNCHER_ID_PATH);
    (void)ugui_cut_top(&r, 6 * s);
    if (s_launcher.truncated) {
        char t[64];
        ksnprintf(t, sizeof t, "The first %d entries", s_launcher.n_entries);
        ugui_rect_t note = ugui_cut_bottom(&r, ugui_serif_line_h(surf) + 4 * s);
        ugui_text_serif(surf, note.x, note.y + 2 * s, t, (uint32_t)strlen(t), false, true, false);
    }
    int sb_w = 10 * s, item_h = 40 * s;
    ugui_rect_t track = ugui_cut_right(&r, sb_w);
    (void)ugui_cut_right(&r, 4 * s);
    s_launcher.item_h = item_h;
    s_launcher.view_h = r.h;
    s_launcher.list_y = r.y;
    if (s_launcher.n_entries == 0) {
        const char *t = s_launcher.cwd[0] ? "This folder is empty." : "No volumes are mounted.";
        ugui_text_serif(surf, r.x + 8 * s, r.y + 8 * s, t, (uint32_t)strlen(t), false, true, false);
        s_launcher.sb_h = 0;
        return;
    }
    ugui_vlist(surf, ctx, r, s_launcher.n_entries, item_h, &s_launcher.scroll_px, file_row, NULL);
    int total = s_launcher.n_entries * item_h;
    s_launcher.sb_y = track.y;
    s_launcher.sb_h = total > r.h ? track.h : 0;
    if (s_launcher.sb_h) {
        ugui_scrollbar(surf, track, s_launcher.scroll_px, total, r.h);
        ugui_add_drag_hit(ctx, track, LAUNCHER_ID_SCROLLBAR);
    }
}

/* The Shortcuts face: an icon grid, or -- in a window narrower than two
 * cells -- a list; scrolled when it is taller than the window. */
static void draw_shortcuts(const ugui_surface_t *surf, ugui_rect_t r) {
    const int s = surf->scale;
    ugui_ctx_t *ctx = &s_launcher.ui_ctx;
    int n = shortcuts_count();
    s_launcher.sc_view = r;
    if (n == 0) {
        const char *t = "No shortcuts. Add one from Files: a long press on it, Add to Shortcuts.";
        ugui_text_serif(surf, r.x + 8 * s, r.y + 8 * s, t, (uint32_t)strlen(t), false, true, false);
        return;
    }
    int cell_w = 96 * s, cell_h = 32 * s + ugui_serif_line_h(surf) + 14 * s;
    bool grid = r.w >= 2 * cell_w;
    int cols = grid ? r.w / cell_w : 1, rows = (n + cols - 1) / cols;
    int row_h = grid ? cell_h : 40 * s;
    s_launcher.sc_content_h = rows * row_h;
    int most = s_launcher.sc_content_h - r.h;
    if (s_launcher.sc_scroll > most) s_launcher.sc_scroll = most > 0 ? most : 0;
    if (s_launcher.sc_scroll < 0) s_launcher.sc_scroll = 0;
    ugui_surface_t cs = ugui_clip(surf, r);
    if (grid) {
        ugui_grid_item_t items[SHORTCUT_MAX];
        for (int i = 0; i < n; i++) {
            const shortcut_t *sc = shortcuts_at(i);
            items[i].scale = 1;
            items[i].img = icon_image(icon_or(shortcuts_icon(sc), "app"), 32 * s, &items[i].scale);
            items[i].label = sc->label;
        }
        ugui_rect_t g = { r.x, (int16_t)(r.y - s_launcher.sc_scroll), (int16_t)(cols * cell_w), (int16_t)(rows * cell_h) };
        (void)ugui_icon_grid(&cs, ctx, g, items, n, cell_w, cell_h, -1, LAUNCHER_ID_SHORTCUT);
        return;
    }
    for (int i = 0; i < n; i++) {
        const shortcut_t *sc = shortcuts_at(i);
        ugui_rect_t row = { r.x, (int16_t)(r.y - s_launcher.sc_scroll + i * row_h), r.w, (int16_t)row_h };
        bool armed = ugui_is_armed(ctx, LAUNCHER_ID_SHORTCUT, (int16_t)i);
        ugui_fill(&cs, row, armed ? UGUI_GRAY_INK : UGUI_GRAY_CARD, armed ? CANVAS1_BLACK : CANVAS1_WHITE);
        draw_icon(&cs, row.x + 6 * s, row.y + (row.h - 32 * s) / 2, shortcuts_icon(sc), "app", armed);
        ugui_text_serif(&cs, row.x + 46 * s, row.y + (row.h - ugui_serif_line_h(surf)) / 2, sc->label,
                        (uint32_t)strlen(sc->label), false, false, armed);
        ugui_add_hit(ctx, row, LAUNCHER_ID_SHORTCUT, (int16_t)i);
    }
}

static const char *const k_rename_buttons[2] = { "Cancel", "Rename" };

/* The rename dialog: a dialog's frame and buttons, the text field in it. */
static void draw_rename(const ugui_surface_t *surf) {
    const int s = surf->scale;
    ugui_ctx_t *ctx = &s_launcher.ui_ctx;
    ugui_rect_t d = ugui_dialog(surf, ctx, "Rename the shortcut", "\n\n", k_rename_buttons, 2, LAUNCHER_ID_RENAME_BTN);
    ugui_rect_t f = { (int16_t)(d.x + 16 * s), (int16_t)(d.y + 26 * s + ugui_serif_line_h(surf)), (int16_t)(d.w - 32 * s),
                      (int16_t)(28 * s) };
    ugui_textfield(surf, ctx, f, &s_launcher.rename_field, LAUNCHER_ID_RENAME);
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

static void launcher_scrollbar_to(int y);

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

    ugui_rect_t area = ugui_inset((ugui_rect_t){ 0, 0, (int16_t)w, (int16_t)h }, 10 * s, 8 * s);
    ugui_tab_bar(&surf, &s_launcher.ui_ctx, ugui_cut_top(&area, 26 * s), k_faces, 2, s_launcher.face, LAUNCHER_ID_TAB);
    (void)ugui_cut_top(&area, 8 * s);
    if (s_launcher.face == FACE_FILES) draw_files(&surf, area);
    else draw_shortcuts(&surf, area);

    if (s_launcher.sheet != SHEET_NONE)
        (void)ugui_action_sheet(&surf, &s_launcher.ui_ctx, s_launcher.sheet_title, s_launcher.sheet_items,
                                s_launcher.sheet_n, LAUNCHER_ID_SHEET);
    else if (s_launcher.renaming)
        draw_rename(&surf);
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
    uint64_t t_wait = time_get_us();
    if (s_launcher.aa_back) {
        console_lock();
        console_frame_lock();
        console_frame_unlock();
        console_unlock();
        s_launcher.aa_target = s_launcher.aa_back;
    }
    uint64_t t_draw = time_get_us();
    launcher_draw(back, s_launcher.win_w, s_launcher.win_h);
    uint64_t t_copy = time_get_us();
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
    uxbench_view_note(t_wait, t_draw, t_copy, time_get_us());

    /* A text field with the focus and nothing over it: the on-screen
     * keyboard under it (where there is touch and no keyboard). */
    bool want = (s_launcher.gal.on && s_launcher.gal.tab == 1 && s_launcher.ui_ctx.focus_id == GAL_ID_TEXT &&
                 !s_launcher.gal.dialog && !s_launcher.gal.sheet && !s_launcher.gal.edit && !s_launcher.alert_title[0]) ||
                (s_launcher.renaming && s_launcher.ui_ctx.focus_id == LAUNCHER_ID_RENAME && !s_launcher.alert_title[0]);
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

    s_launcher.face = settings_get("launcher.face") == FACE_FILES ? FACE_FILES : FACE_SHORTCUTS;
    shortcuts_load();                                    /* the file may have changed (`shortcuts`, 9P) */
    if (s_launcher.files_loaded) (void)load_dir();
    if (s_launcher.face == FACE_FILES && !s_launcher.files_loaded) (void)load_dir();
    /* Opened afresh (not brought to the front): the keyboard's focus on
     * the face's first item, its ring hidden until a key. */
    if (!s_launcher.active) {
        if (s_launcher.face == FACE_FILES && s_launcher.n_entries > 0) ugui_set_focus(&s_launcher.ui_ctx, LAUNCHER_ID_FILE, 0);
        else if (s_launcher.face == FACE_SHORTCUTS && shortcuts_count() > 0) ugui_set_focus(&s_launcher.ui_ctx, LAUNCHER_ID_SHORTCUT, 0);
        s_launcher.ui_ctx.focus_shown = false;
    }

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
    s_launcher.sheet = SHEET_NONE;
    s_launcher.renaming = false;
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

/* The list's scrollbar pressed or dragged at y: the thumb's middle there. */
static void launcher_scrollbar_to(int y) {
    if (s_launcher.sb_h <= 0 || s_launcher.item_h <= 0) return;
    int total = s_launcher.n_entries * s_launcher.item_h, view = s_launcher.view_h;
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
    launcher_update();
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
 * left for the shell (shell_check_launcher_exec()). The Settings app, a view
 * of the kernel's, opens directly. */
static bool launch_app(const char *id, const char *path) {
    if (strcmp(id, "settings") == 0) {
        if (!settings_view_open())
            launcher_alert("Settings did not open.", "There is no room for another window:\nclose one first (its close box, or Cmd-W).");
        return true;
    }
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
    if (strcmp(id, "term") == 0) {                       /* beside the launcher, focused */
        if (lcdterm_room(&windows, NULL) && windows < 1) {
            launcher_alert("No room for another window.", "Close a window first: its close box, or Cmd-W.");
            return false;
        }
        console_hotkey(CONSOLE_HOTKEY_NEW_TERM);
        return true;
    }
    char cmd[sizeof s_pending_cmd];
    if (!app_command(a, path, cmd, sizeof cmd)) return false;
    uintptr_t f = spin_lock_irqsave(&s_pending_lock);
    memcpy(s_pending_cmd, cmd, sizeof cmd);
    spin_unlock_irqrestore(&s_pending_lock, f);
    return true;
}

/* A document's other application: the editor for what the reader shows,
 * the reader for the rest. */
static const char *other_app(const char *path) {
    const app_desc_t *a = app_for_path(path);
    return a && strcmp(a->id, "reader") == 0 ? "editor" : "reader";
}

/* A file opened by its association: the reader in the ribbon for what it
 * shows, the application for the rest; an alert when nothing opens it. */
static void open_path(const char *path) {
    const app_desc_t *a = app_for_path(path);
    if (a && strcmp(a->id, "reader") == 0) {
        open_doc_in_reader(path);
        return;
    }
    if (!a || !launch_app(a->id, path))
        launcher_alert("Nothing opens this file.", "No application is associated with its type:\nOpen With... offers the editor.");
}

static void open_shortcut(int i) {
    const shortcut_t *sc = shortcuts_at(i);
    if (!sc) return;
    switch (sc->type) {
    case SHORTCUT_APP:
        if (!launch_app(sc->target, NULL)) launcher_alert("The application is not on this board.", sc->target);
        return;
    case SHORTCUT_DIR:
        s_launcher.face = FACE_FILES;
        (void)settings_set("launcher.face", FACE_FILES);
        files_go(sc->target);
        launcher_update();
        return;
    default:                                             /* a document, a book */
        open_path(sc->target);
        return;
    }
}

static void open_entry(int i) {
    if (i < 0 || i >= s_launcher.n_entries) return;
    char path[VFS_PATH_MAX];
    if (!entry_path(i, path, sizeof path)) {
        launcher_alert("The path is too long.", "It does not fit the paths this board takes.");
        return;
    }
    if (s_launcher.entries[i].kind == ENTRY_FILE) {
        open_path(path);
        return;
    }
    files_go(path);
    launcher_update();
}

static void set_face(int face) {
    if (face == s_launcher.face) return;
    s_launcher.face = face;
    (void)settings_set("launcher.face", face);
    if (face == FACE_FILES && !s_launcher.files_loaded) (void)load_dir();
    /* The keyboard's focus on the face's first item (shown as it was). */
    if (face == FACE_FILES && s_launcher.n_entries > 0) ugui_set_focus(&s_launcher.ui_ctx, LAUNCHER_ID_FILE, 0);
    else if (face == FACE_SHORTCUTS && shortcuts_count() > 0) ugui_set_focus(&s_launcher.ui_ctx, LAUNCHER_ID_SHORTCUT, 0);
    else ugui_set_focus(&s_launcher.ui_ctx, LAUNCHER_ID_TAB + face, (int16_t)face);
    launcher_update();
}

bool launcher_bench_show(int face, const char *dir, int *was_face, char *was_dir, unsigned cap) {
    if (was_face) *was_face = s_launcher.face;
    if (was_dir && cap) {
        strncpy(was_dir, s_launcher.cwd, cap - 1);
        was_dir[cap - 1] = '\0';
    }
    if (dir) {
        char was[VFS_PATH_MAX];
        memcpy(was, s_launcher.cwd, sizeof was);
        strncpy(s_launcher.cwd, dir, sizeof s_launcher.cwd - 1);
        s_launcher.cwd[sizeof s_launcher.cwd - 1] = '\0';
        if (!load_dir()) {
            memcpy(s_launcher.cwd, was, sizeof s_launcher.cwd);
            (void)load_dir();
            return false;
        }
        s_launcher.scroll_px = 0;
    }
    s_launcher.face = face == FACE_FILES ? FACE_FILES : FACE_SHORTCUTS;
    if (s_launcher.face == FACE_FILES && !s_launcher.files_loaded) (void)load_dir();
    return true;
}

int launcher_bench_files(void) {
    return s_launcher.n_entries;
}

/* --- Action sheets, confirmations, rename --- */

static void sheet_add(const char *label, uint8_t act) {
    if (s_launcher.sheet_n >= LAUNCHER_SHEET_MAX) return;
    s_launcher.sheet_items[s_launcher.sheet_n] = label;
    s_launcher.sheet_act[s_launcher.sheet_n++] = act;
}

static void sheet_for_shortcut(int i) {
    const shortcut_t *sc = shortcuts_at(i);
    if (!sc) return;
    s_launcher.sheet = SHEET_SHORTCUT;
    s_launcher.sheet_idx = i;
    s_launcher.sheet_n = 0;
    strncpy(s_launcher.sheet_title, sc->label, sizeof s_launcher.sheet_title - 1);
    s_launcher.sheet_title[sizeof s_launcher.sheet_title - 1] = '\0';
    sheet_add("Open", ACT_OPEN);
    sheet_add("Rename", ACT_RENAME);
    sheet_add("Remove", ACT_REMOVE);
    if (i > 0) sheet_add("Move Left", ACT_MOVE_LEFT);
    if (i + 1 < shortcuts_count()) sheet_add("Move Right", ACT_MOVE_RIGHT);
    sheet_add("Cancel", ACT_CANCEL);
    ugui_set_focus(&s_launcher.ui_ctx, LAUNCHER_ID_SHEET, 0);
}

static void sheet_for_entry(int i) {
    if (i < 0 || i >= s_launcher.n_entries) return;
    const launcher_entry_t *e = &s_launcher.entries[i];
    s_launcher.sheet = SHEET_FILE;
    s_launcher.sheet_idx = i;
    s_launcher.sheet_n = 0;
    strncpy(s_launcher.sheet_title, e->name, sizeof s_launcher.sheet_title - 1);
    s_launcher.sheet_title[sizeof s_launcher.sheet_title - 1] = '\0';
    sheet_add("Open", ACT_OPEN);
    if (e->kind == ENTRY_FILE)
        sheet_add(strcmp(other_app(e->name), "editor") == 0 ? "Open in Editor" : "Open in Reader", ACT_OPEN_OTHER);
    if (e->kind != ENTRY_ROOT) sheet_add("Add to Shortcuts", ACT_ADD_SHORTCUT);
    sheet_add("Info", ACT_INFO);
    if (e->kind == ENTRY_FILE) sheet_add("Delete", ACT_DELETE);
    sheet_add("Cancel", ACT_CANCEL);
    ugui_set_focus(&s_launcher.ui_ctx, LAUNCHER_ID_SHEET, 0);
}

static const char *const k_remove_buttons[2] = { "Cancel", "Remove" };
static const char *const k_delete_buttons[2] = { "Cancel", "Delete" };

static void remove_answer(int choice, void *arg) {
    (void)arg;
    int i = s_launcher.pending_remove;
    s_launcher.pending_remove = -1;
    if (choice == 1 && !shortcuts_remove(i)) launcher_alert("The shortcut was not removed.", "Its file could not be written.");
    launcher_update();
}

static void delete_answer(int choice, void *arg) {
    (void)arg;
    int i = s_launcher.pending_delete;
    s_launcher.pending_delete = -1;
    char path[VFS_PATH_MAX];
    if (choice != 1 || i < 0 || i >= s_launcher.n_entries || !entry_path(i, path, sizeof path)) return;
    if (vfs_remove(path) < 0) launcher_alert("The file was not deleted.", "The volume may be read-only.");
    (void)load_dir();
    launcher_update();
}

static void entry_info(int i) {
    const launcher_entry_t *e = &s_launcher.entries[i];
    char path[VFS_PATH_MAX], sz[24];
    if (!entry_path(i, path, sizeof path)) path[0] = '\0';
    size_text(e->size, sz, sizeof sz);
    if (e->kind == ENTRY_ROOT) {
        ksnprintf(s_launcher.info, sizeof s_launcher.info, "%s\n%s", path, e->title);
    } else if (e->kind == ENTRY_DIR) {
        ksnprintf(s_launcher.info, sizeof s_launcher.info, "%s\nA folder", path);
    } else {
        const app_desc_t *a = app_for_path(e->name);
        ksnprintf(s_launcher.info, sizeof s_launcher.info, "%s\n%s%s%s\nOpens in %s", path, sz,
                  e->title[0] ? ", titled " : "", e->title, a ? a->title : "nothing (no association)");
    }
    launcher_ask("doc", e->name, s_launcher.info, NULL, 1, 0, NULL, NULL);
}

/* The sheet's item `choice`: gone, then done. */
static void launcher_sheet_answer(int choice) {
    int kind = s_launcher.sheet, i = s_launcher.sheet_idx;
    int act = choice >= 0 && choice < s_launcher.sheet_n ? s_launcher.sheet_act[choice] : ACT_CANCEL;
    s_launcher.sheet = SHEET_NONE;
    launcher_update();
    if (kind == SHEET_SHORTCUT) {
        const shortcut_t *sc = shortcuts_at(i);
        if (!sc) return;
        switch (act) {
        case ACT_OPEN: open_shortcut(i); return;
        case ACT_RENAME:
            s_launcher.renaming = true;
            s_launcher.rename_idx = i;
            strncpy(s_launcher.rename_buf, sc->label, sizeof s_launcher.rename_buf - 1);
            s_launcher.rename_buf[sizeof s_launcher.rename_buf - 1] = '\0';
            ugui_text_init(&s_launcher.rename_field, s_launcher.rename_buf, (uint16_t)sizeof s_launcher.rename_buf);
            s_launcher.rename_field.len = (uint16_t)strlen(s_launcher.rename_buf);
            s_launcher.rename_field.anchor = 0;                  /* all of it selected: typing replaces it */
            s_launcher.rename_field.caret = s_launcher.rename_field.len;
            ugui_set_focus(&s_launcher.ui_ctx, LAUNCHER_ID_RENAME, 0);
            launcher_update();
            return;
        case ACT_REMOVE: {
            ksnprintf(s_launcher.info, sizeof s_launcher.info, "\"%s\" leaves the Shortcuts.\nWhat it opens stays where it is.",
                      sc->label);
            s_launcher.pending_remove = i;
            launcher_ask(NULL, "Remove the shortcut?", s_launcher.info, k_remove_buttons, 2, 0, remove_answer, NULL);
            return;
        }
        case ACT_MOVE_LEFT:
        case ACT_MOVE_RIGHT:
            (void)shortcuts_move(i, act == ACT_MOVE_LEFT ? -1 : 1);
            ugui_set_focus(&s_launcher.ui_ctx, LAUNCHER_ID_SHORTCUT, (int16_t)(i + (act == ACT_MOVE_LEFT ? -1 : 1)));
            launcher_update();
            return;
        default:
            return;
        }
    }
    if (kind != SHEET_FILE || i < 0 || i >= s_launcher.n_entries) return;
    const launcher_entry_t *e = &s_launcher.entries[i];
    char path[VFS_PATH_MAX];
    if (!entry_path(i, path, sizeof path)) return;
    switch (act) {
    case ACT_OPEN: open_entry(i); return;
    case ACT_OPEN_OTHER:
        if (strcmp(other_app(path), "reader") == 0) open_doc_in_reader(path);
        else (void)launch_app("editor", path);
        return;
    case ACT_ADD_SHORTCUT: {
        const char *label = e->title[0] && e->kind == ENTRY_FILE ? e->title : e->name;
        shortcut_type_t t = e->kind == ENTRY_DIR ? SHORTCUT_DIR : SHORTCUT_DOC;
        if (!shortcuts_add(t, path, label, NULL)) launcher_alert("The shortcut was not added.", "The list is full, or its file could not be written.");
        return;
    }
    case ACT_INFO: entry_info(i); return;
    case ACT_DELETE: {
        ksnprintf(s_launcher.info, sizeof s_launcher.info, "\"%s\" will be removed from %s.\nThis cannot be undone.", e->name,
                  s_launcher.cwd);
        s_launcher.pending_delete = i;
        launcher_ask("bomb", "Delete the file?", s_launcher.info, k_delete_buttons, 2, 0, delete_answer, NULL);
        return;
    }
    default:
        return;
    }
}

static void rename_done(bool ok) {
    if (ok && s_launcher.rename_buf[0] && !shortcuts_rename(s_launcher.rename_idx, s_launcher.rename_buf))
        launcher_alert("The shortcut was not renamed.", "Its file could not be written.");
    s_launcher.renaming = false;
    ugui_set_focus(&s_launcher.ui_ctx, LAUNCHER_ID_SHORTCUT, (int16_t)s_launcher.rename_idx);
    launcher_update();
}

/* The context menu (a right click, a long press, `m`) on (id, index). */
static void launcher_context_on(uint16_t id, int16_t idx) {
    if (id == LAUNCHER_ID_SHORTCUT) sheet_for_shortcut(idx);
    else if (id == LAUNCHER_ID_FILE) sheet_for_entry(idx);
    else return;
    (void)ugui_disarm(&s_launcher.ui_ctx);
    launcher_update();
}

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

static bool launcher_activate(uint16_t id, int16_t hit_idx);

static bool launcher_tap_act(int client_x, int client_y) {
    int16_t hit_idx = -1;
    uint16_t id = ugui_hit_test(&s_launcher.ui_ctx, client_x, client_y, &hit_idx);
    if (s_launcher.alert_title[0]) {                     /* modal: its buttons only */
        if (id >= LAUNCHER_ID_ALERT_OK && id < LAUNCHER_ID_ALERT_OK + s_launcher.alert_count)
            launcher_alert_answer(id - LAUNCHER_ID_ALERT_OK);
        return id != 0;
    }
    if (s_launcher.gal.on) return id && launcher_gallery_act(id, hit_idx, client_x);
    if (s_launcher.sheet != SHEET_NONE) {                /* modal: a choice, or away */
        launcher_sheet_answer(id >= LAUNCHER_ID_SHEET && id < LAUNCHER_ID_SHEET + s_launcher.sheet_n ? id - LAUNCHER_ID_SHEET : -1);
        return true;
    }
    return launcher_activate(id, hit_idx);
}

static void launcher_context(int client_x, int client_y) {
    if (!s_launcher.active || s_launcher.alert_title[0] || s_launcher.sheet != SHEET_NONE || s_launcher.renaming) return;
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
    launcher_context_on(id, hit_idx);
}

/* What a tap on widget id (hit_idx: its index) does; the keyboard's Enter
 * does the same. */
static bool launcher_activate(uint16_t id, int16_t hit_idx) {
    if (id == 0) return false;
    if (s_launcher.renaming) {
        if (id == LAUNCHER_ID_RENAME) ugui_set_focus(&s_launcher.ui_ctx, LAUNCHER_ID_RENAME, 0);
        else if (id == LAUNCHER_ID_RENAME_BTN || id == LAUNCHER_ID_RENAME_BTN + 1) rename_done(id == LAUNCHER_ID_RENAME_BTN + 1);
        else return false;
        launcher_update();
        return true;
    }
    if (id >= LAUNCHER_ID_TAB && id < LAUNCHER_ID_TAB + 2) {
        set_face(id - LAUNCHER_ID_TAB);
        return true;
    }
    if (id == LAUNCHER_ID_SHORTCUT) {
        open_shortcut(hit_idx);
        return true;
    }
    if (id == LAUNCHER_ID_FILE) {
        open_entry(hit_idx);
        return true;
    }
    if (id == LAUNCHER_ID_PATH) {                        /* segment 0 is the roots */
        if (hit_idx <= 0) {
            files_go("");
        } else {
            char pre[VFS_PATH_MAX];
            int n = ugui_path_prefix(s_launcher.cwd, hit_idx);
            memcpy(pre, s_launcher.cwd, (size_t)n);
            pre[n] = '\0';
            files_go(pre);
        }
        launcher_update();
        return true;
    }
    if (id == LAUNCHER_ID_SCROLLBAR) return true;        /* launcher_press() moved it already */
    return false;
}

void launcher_scroll(int delta_y) {
    if (!s_launcher.active || s_launcher.gal.on) return;
    if (s_launcher.face == FACE_FILES) {
        s_launcher.scroll_px += (delta_y > 0 ? 2 : -2) * (s_launcher.item_h > 0 ? s_launcher.item_h : 40);
        if (s_launcher.scroll_px < 0) s_launcher.scroll_px = 0;
    } else {
        s_launcher.sc_scroll += (delta_y > 0 ? 1 : -1) * 64 * (int)CHROME_SCALE;
    }
    launcher_update();
}

/* A finger dragging dy px (down positive): the face's content follows it. */
void launcher_drag(int dy) {
    if (!s_launcher.active || dy == 0 || s_launcher.gal.on) return;
    if (s_launcher.face == FACE_FILES) {
        s_launcher.scroll_px -= dy;
        if (s_launcher.scroll_px < 0) s_launcher.scroll_px = 0;
    } else {
        s_launcher.sc_scroll -= dy;
    }
    launcher_update();
}

/* The drag's end: a finger still moving (dy over its last ms) glides on,
 * its speed halving at each of up to four steps -- in whole pixels, drawn
 * as it goes. */
void launcher_drag_end(int dy, uint32_t ms) {
    if (!s_launcher.active || ms == 0 || dy == 0) return;
    int step = (int)((long)dy * 48 / (long)ms);          /* px per ~48 ms */
    for (int i = 0; i < 4 && (step > 1 || step < -1); i++) {
        int before = s_launcher.face == FACE_FILES ? s_launcher.scroll_px : s_launcher.sc_scroll;
        launcher_drag(step);
        if ((s_launcher.face == FACE_FILES ? s_launcher.scroll_px : s_launcher.sc_scroll) == before) break;
        step /= 2;
    }
}

/* The file list under the keyboard: Up / Down from a row moves to the row
 * before / after it, scrolling it into view, so the focus walks the whole
 * list rather than jumping out of it at the visible edge. */
static bool launcher_row_step(uint32_t key) {
    ugui_ctx_t *ctx = &s_launcher.ui_ctx;
    if (!ctx->focus_shown || ctx->focus_id != LAUNCHER_ID_FILE) return false;
    int to = ctx->focus_index + (key == KEY_DOWN ? 1 : -1);
    if (to < 0 || to >= s_launcher.n_entries) return false;
    ctx->focus_index = (int16_t)to;
    s_launcher.scroll_px = ugui_vlist_reveal(s_launcher.scroll_px, s_launcher.view_h, s_launcher.item_h, to);
    return true;
}

/* Type-ahead in the file list: a letter moves the focus to the next entry
 * whose name (or heading) starts with it. */
static bool launcher_type_ahead(uint32_t key) {
    if (s_launcher.face != FACE_FILES || s_launcher.n_entries == 0) return false;
    char c = lower((char)key);
    ugui_ctx_t *ctx = &s_launcher.ui_ctx;
    int from = ctx->focus_id == LAUNCHER_ID_FILE ? ctx->focus_index : -1;
    for (int k = 1; k <= s_launcher.n_entries; k++) {
        int i = (from + k) % s_launcher.n_entries;
        const launcher_entry_t *e = &s_launcher.entries[i];
        const char *main = e->title[0] && e->kind == ENTRY_FILE ? e->title : e->name;
        if (lower(main[0]) == c || lower(e->name[0]) == c) {
            ugui_set_focus(ctx, LAUNCHER_ID_FILE, (int16_t)i);
            ctx->focus_shown = true;
            s_launcher.scroll_px = ugui_vlist_reveal(s_launcher.scroll_px, s_launcher.view_h, s_launcher.item_h, i);
            return true;
        }
    }
    return false;
}

static void launcher_key(key_event_t k) {
    ugui_ctx_t *ctx = &s_launcher.ui_ctx;
    if (s_launcher.alert_title[0]) {                     /* Esc: its cancel; Enter: the focused button, or the default */
        if (k.key == 0x1b) { launcher_alert_answer(s_launcher.alert_cancel); return; }
        uint16_t id = 0;
        int16_t idx = 0;
        ugui_nav_t nav = ugui_nav(ctx, k.key, k.mods, &id, &idx);
        if (nav == UGUI_NAV_ACTIVATE && id >= LAUNCHER_ID_ALERT_OK && id < LAUNCHER_ID_ALERT_OK + s_launcher.alert_count)
            launcher_alert_answer(id - LAUNCHER_ID_ALERT_OK);
        else if ((k.key == '\r' || k.key == '\n') && nav != UGUI_NAV_ACTIVATE)
            launcher_alert_answer(s_launcher.alert_count - 1);
        else if (nav == UGUI_NAV_MOVED)
            launcher_update();
        return;
    }
    if (s_launcher.sheet != SHEET_NONE) {                /* Esc: Cancel; Enter: the focused item */
        if (k.key == 0x1b) { launcher_sheet_answer(-1); return; }
        uint16_t id = 0;
        int16_t idx = 0;
        ugui_nav_t nav = ugui_nav(ctx, k.key, k.mods, &id, &idx);
        if (nav == UGUI_NAV_ACTIVATE && id >= LAUNCHER_ID_SHEET && id < LAUNCHER_ID_SHEET + s_launcher.sheet_n)
            launcher_sheet_answer(id - LAUNCHER_ID_SHEET);
        else if (nav == UGUI_NAV_MOVED)
            launcher_update();
        return;
    }
    if (s_launcher.gal.on && launcher_gallery_key(k)) return;
    if (s_launcher.renaming) {                           /* the field takes its keys; Esc / Enter end it */
        if (ctx->focus_id == LAUNCHER_ID_RENAME) {
            ugui_text_ev_t e = ugui_text_key(&s_launcher.rename_field, k.key, k.mods);
            if (e == UGUI_TEXT_SUBMIT || e == UGUI_TEXT_CANCEL) { rename_done(e == UGUI_TEXT_SUBMIT); return; }
            if (e != UGUI_TEXT_NONE) { launcher_update(); return; }
        }
        if (k.key == 0x1b) { rename_done(false); return; }
    }
    if (!s_launcher.gal.on && !s_launcher.renaming) {
        if (k.key == '\t' && (k.mods & KMOD_CTRL)) {     /* Ctrl+Tab, Ctrl+Shift+Tab: the other face */
            set_face(s_launcher.face == FACE_FILES ? FACE_SHORTCUTS : FACE_FILES);
            return;
        }
        /* The focused item's action sheet: Shift+Enter, and `m` where letters
         * are free (Files takes them for type-ahead). */
        bool sheet_key = ((k.key == '\r' || k.key == '\n') && k.mods == KMOD_SHIFT) ||
                         (k.key == 'm' && !k.mods && s_launcher.face == FACE_SHORTCUTS);
        if (sheet_key && ctx->focus_shown) {
            launcher_context_on(ctx->focus_id, ctx->focus_index);
            return;
        }
        if (s_launcher.face == FACE_FILES) {
            bool on_row = ctx->focus_id == LAUNCHER_ID_FILE && ctx->focus_shown;
            if (k.key == 0x7f || k.key == 0x08 || (k.key == 0x1b && s_launcher.cwd[0]) || (k.key == KEY_LEFT && on_row)) {
                files_up();
                launcher_update();
                return;
            }
            if ((k.key == KEY_UP || k.key == KEY_DOWN) && launcher_row_step(k.key)) {
                launcher_update();
                return;
            }
            /* A page, the first, the last row: the focus goes there (shown),
             * the list follows it. */
            if ((k.key == KEY_PGDN || k.key == KEY_PGUP || k.key == KEY_HOME || k.key == KEY_END) && s_launcher.n_entries > 0) {
                int page = s_launcher.item_h > 0 ? s_launcher.view_h / s_launcher.item_h : 1;
                int at = ctx->focus_id == LAUNCHER_ID_FILE ? ctx->focus_index : 0;
                int to = k.key == KEY_HOME ? 0 : k.key == KEY_END ? s_launcher.n_entries - 1
                       : at + (k.key == KEY_PGDN ? page : -page);
                if (to < 0) to = 0;
                if (to >= s_launcher.n_entries) to = s_launcher.n_entries - 1;
                ugui_set_focus(ctx, LAUNCHER_ID_FILE, (int16_t)to);
                ctx->focus_shown = true;
                s_launcher.scroll_px = ugui_vlist_reveal(s_launcher.scroll_px, s_launcher.view_h, s_launcher.item_h, to);
                launcher_update();
                return;
            }
            if (!k.mods && k.key > ' ' && k.key < 0x7f && launcher_type_ahead(k.key)) {
                launcher_update();
                return;
            }
        }
    }
    uint16_t id = 0;
    int16_t idx = 0;
    switch (ugui_nav(ctx, k.key, k.mods, &id, &idx)) {
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
        return;
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
    cprintf("launcher: %s (slot=%u, size=%dx%d, face %s, shortcuts=%d, widgets=%d)\n",
            s_launcher.active ? "open" : "closed",
            (unsigned)s_launcher.slot_id,
            s_launcher.win_w, s_launcher.win_h, k_faces[s_launcher.face],
            shortcuts_count(), s_launcher.ui_ctx.count);
    cprintf("  files: \"%s\", %d entries%s, scrolled %d px\n", s_launcher.cwd[0] ? s_launcher.cwd : "/",
            s_launcher.n_entries, s_launcher.truncated ? " (more not shown)" : "", s_launcher.scroll_px);
    const ugui_ctx_t *fc = &s_launcher.ui_ctx;
    if (fc->focus_id == LAUNCHER_ID_FILE && fc->focus_index >= 0 && fc->focus_index < s_launcher.n_entries)
        cprintf("  focus: file %d \"%s\"%s\n", fc->focus_index, s_launcher.entries[fc->focus_index].name,
                fc->focus_shown ? "" : " (not shown)");
    else if (fc->focus_id == LAUNCHER_ID_SHORTCUT && shortcuts_at(fc->focus_index))
        cprintf("  focus: shortcut %d \"%s\"%s\n", fc->focus_index, shortcuts_at(fc->focus_index)->label,
                fc->focus_shown ? "" : " (not shown)");
    else if (fc->focus_id)
        cprintf("  focus: id %u index %d%s\n", (unsigned)fc->focus_id, (int)fc->focus_index, fc->focus_shown ? "" : " (not shown)");
    if (s_launcher.alert_title[0]) cprintf("  alert: %s\n", s_launcher.alert_title);
    if (s_launcher.sheet != SHEET_NONE) {
        cprintf("  sheet: %s:", s_launcher.sheet_title);
        for (int i = 0; i < s_launcher.sheet_n; i++) cprintf(" [%d] %s", i, s_launcher.sheet_items[i]);
        cprintf("\n");
    }
    if (s_launcher.renaming) cprintf("  renaming shortcut %d: \"%s\"\n", s_launcher.rename_idx, s_launcher.rename_buf);
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
        /* A row's or a tile's name after it, for tests that pick one. */
        const char *what = h->id == LAUNCHER_ID_FILE && h->index >= 0 && h->index < s_launcher.n_entries
                               ? s_launcher.entries[h->index].name
                         : h->id == LAUNCHER_ID_SHORTCUT && shortcuts_at(h->index) ? shortcuts_at(h->index)->label : NULL;
        cprintf("  hit id %u index %d: x %d..%d y %d..%d, touched over %dx%d px%s%s%s%s\n", (unsigned)h->id, (int)h->index,
                (int)h->rect.x, (int)(h->rect.x + h->rect.w - 1), (int)h->rect.y, (int)(h->rect.y + h->rect.h - 1),
                (int)t.w, (int)t.h, (h->flags & UGUI_HIT_DISABLED) ? " (dimmed)" : "", what ? " \"" : "", what ? what : "",
                what ? "\"" : "");
    }
}

#endif
