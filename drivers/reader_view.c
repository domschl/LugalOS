/* drivers/reader_view.c — Ribbon Reader Window Engine with Proportional Typography
 *
 * Implements a dedicated long-form reading window for the horizontal window ribbon:
 * - Proportional serif typography (font_serif) with word-wrapping.
 * - Monospace fallback (font8x16) for preformatted blocks (``` ... ```).
 * - Touch hit-testing: tapping links navigates to targets.
 * - Smooth vertical scrolling & page panning with GT911 gesture support.
 * - Freestanding C, zero heap allocations, zero FPU in M-mode.
 */

#include "kernel/orient.h"
#include "drivers/reader_view.h"
#include "drivers/view.h"
#include "kernel/keyseq.h"
#include "drivers/ugui.h"
#include "drivers/font_serif.h"
#include "drivers/font_noto_serif.h"
#include "drivers/font_noto_mono.h"
#include "drivers/font8x16.h"
#include "drivers/canvas1.h"
#include "drivers/lcdterm_attr.h"
#include "drivers/lcdterm.h"
#include "drivers/ramscreen.h"
#include "drivers/screen.h"
#include "kernel/console.h"
#include "kernel/settings.h"
#include "drivers/touch.h"
#include "fs/gemtext.h"
#include "fs/markdown.h"
#include "fs/latex_math.h"
#include "drivers/latex_layout.h"
#include "fs/vfs.h"
#include "net/spartan.h"
#include "kernel/vterm.h"
#include "kernel/palloc.h"
#include "kernel/time.h"
#include <string.h>

#if defined(CONFIG_PSRAM_BYTES) || defined(CONFIG_BOARD_VISIONFIVE2)
#define READER_EPUB_SUPPORT 1
#include "fs/epub.h"
#else
#define READER_EPUB_SUPPORT 0
#endif

#if defined(CONFIG_BOARD_ESP32P4) && (defined(CONFIG_DSI_LCD_RST_GPIO) || defined(CONFIG_BOARD_TAB5))
#include "drivers/dsi_esp32p4.h"
#endif

/* Without a panel (the C6, an RP2350 without the LCD) the one instance is for a
 * help page at most: smaller tables, ~2 KB instead of ~7.5 KB of static RAM. */
#if defined(CONFIG_BOARD_ESP32C6) || (defined(CONFIG_BOARD_RP2350) && !defined(CONFIG_LCD_PCLK_GPIO))
#define READER_LINKS_MAX   8
#define READER_TOC_MAX     8
#define READER_STACK_MAX   2
#define READER_CP_MAX      1           /* a help page at most: always the full pass */
#else
#define READER_LINKS_MAX   32
#define READER_TOC_MAX     32
#define READER_STACK_MAX   8
#if defined(CONFIG_PSRAM_BYTES)
#define READER_CP_MAX      512         /* 6 KB a reader, in PSRAM with the rest of it */
#else
#define READER_CP_MAX      32          /* no PSRAM: the state is low RAM */
#endif
#endif

#if defined(CONFIG_FONT_HIDPI) || (defined(CONFIG_FONT_CELL_H) && (CONFIG_FONT_CELL_H == 32))
#define READER_IS_HIDPI    1
#define READER_MARGIN_X    36
#define READER_LINE_H_BASE 30
#else
#define READER_IS_HIDPI    0
#define READER_MARGIN_X    24
#define READER_LINE_H_BASE 18
#endif

/* The text size (`ui.text_size`, drivers/ugui.h's faces): the face the body
 * text is drawn in (NULL: the board's own) and its line height, scaled from
 * the board's own in proportion to the face. Read again at each full draw;
 * a layout's checkpoints are for the size they were laid out at
 * (cp_tsize). single task: views. */
#if defined(UGUI_TEXT_SIZES)
static struct {
    const ugui_face_t *face;
    int                line_h;  /* 0: not read yet, the board's own */
    int                size;
} s_rm BULK_BSS;
#define READER_LINE_H (s_rm.line_h ? s_rm.line_h : READER_LINE_H_BASE)

static void reader_metrics(void) {
    int fpx = settings_get("ui.font_size");
    int size = settings_get("ui.text_size");
    if (size < 0) size = 1;
    const ugui_face_t *f = (fpx >= 16) ? ugui_face_px(fpx) : NULL;
    if (!f) f = ugui_face(size);
    int base_h = READER_IS_HIDPI ? 32 : 16;
    s_rm.face = f;
    s_rm.size = f ? (f->h + (size << 8)) : size;
    s_rm.line_h = f ? (f->h * READER_LINE_H_BASE + base_h / 2) / base_h : READER_LINE_H_BASE;
}
#define READER_FACE  (s_rm.face)
#define READER_CP_TSIZE_OK(r) ((r)->cp_tsize == s_rm.size)
#define READER_CP_TSIZE_SET(r) ((r)->cp_tsize = s_rm.size)
#else
#define READER_LINE_H READER_LINE_H_BASE
#define READER_FACE  ((const ugui_face_t *)0)
#define READER_CP_TSIZE_OK(r) true
#define READER_CP_TSIZE_SET(r) ((void)(r))
static void reader_metrics(void) {}
#endif

#define READER_BG_GRAY8          250   /* Soft book-grade paper/cream (0xFA) */
#define READER_FG_GRAY8          10    /* Deep charcoal dark slate ink (0x0A) */
#define READER_RULE_GRAY8        70    /* Heading underline rule */
#define READER_QUOTE_BAR_GRAY8   180   /* Blockquote left vertical bar */
#define READER_LINK_LINE_GRAY8   120   /* Hyperlink underline */
#define READER_SB_TRACK_GRAY8    230   /* Scrollbar track */
#define READER_SB_THUMB_GRAY8    90    /* Scrollbar thumb */

typedef struct {
    int16_t x0, y0, x1, y1;  /* Exact 2D bounding box */
    int     num;             /* 1-based index (e.g. 1..9) */
    char    url[128];
} reader_link_t;

typedef struct {
    char path[128];
    int  scroll_y;
} reader_hist_entry_t;

typedef struct {
    char    title[48];
    uint8_t level;
    int     doc_y;
} reader_toc_entry_t;

/* A place to resume the layout from: the start of a source line, where the
 * document's y and everything the parser carries from line to line are
 * known. Lines are laid out independently given in_code / in_math, and links
 * are numbered through the document, so these four values are all of it. */
typedef struct {
    uint32_t off;               /* byte offset of the line's first byte */
    int32_t  doc_y;
    uint16_t link_idx;
    uint8_t  flags;             /* READER_CP_* */
} reader_cp_t;
#define READER_CP_CODE 0x1u
#define READER_CP_MATH 0x2u

typedef struct {
    bool                active;
    uint8_t             target_cid;
    /* Checkpoints from the last full layout at width cp_w, every cp_step
     * px of the document at least; valid until the document or the width
     * changes. A draw then lays out only from the checkpoint above the
     * view to its bottom, instead of the whole file. */
    reader_cp_t         cp[READER_CP_MAX];
    int                 cp_count;
    int                 cp_step;
    int                 cp_w;
#if defined(UGUI_TEXT_SIZES)
    int                 cp_tsize;   /* the text size it was laid out at */
#endif
    uint32_t            cp_size;    /* the file's size at that layout */
    bool                cp_valid;
    int                 drawn_scroll_y; /* where the surfaces show the document, -1 unknown */
    bool                strip_failed;   /* the last strip draw could not lay out from checkpoints */
    int                 strip_diff_y0, strip_diff_y1; /* reader_strip_check()'s differing GRAY8 rows */
    int                 win_w, win_h;
    char                path[128];
    char                title[64];
    int                 scroll_y;
    int                 total_h;
    reader_link_t       links[READER_LINKS_MAX];
    int                 link_count;
    reader_hist_entry_t history[READER_STACK_MAX];
    int                 history_depth;
    reader_toc_entry_t  toc[READER_TOC_MAX];
    int                 toc_count;
    bool                toc_mode;
    bool                fullscreen;
    uint64_t            last_page_tap_ms;
    ugui_ctx_t          ui_ctx;
    uint8_t            *aa_buf;        /* Anti-aliased 8-bpp (GRAY8) surface in PSRAM */
    uint32_t            aa_stride;
    int                 aa_w, aa_h;
    uint32_t            aa_pages;
#if READER_EPUB_SUPPORT
    bool                is_epub;
    epub_book_t        *epub_book;
    uint32_t            epub_book_pages;
    int                 epub_chapter;
    char               *epub_md;
    uint32_t            epub_md_len;
    uint32_t            epub_md_pages;
    char                epub_err[96];
#endif
} reader_state_t;

/* single task: views (drivers/view.h) -- every change and every draw. Also
 * under console_lock: opening and closing an instance (the surface and the
 * active flag), which the compositor reads under it. accepted: the compositor
 * can show a frame mid-draw (the reader draws into its live surface). */
/* BULK_BSS: link tables, history and contents, touched at the speed of a person -- PSRAM where there
 * is one, which returns about 30 KB of low RAM on the P4 boards. */
static reader_state_t s_readers[READER_MAX_INSTANCES] BULK_BSS;
static uint8_t        s_active_reader_rid = 0;
static uint32_t       s_reader_seq[READER_MAX_INSTANCES];

static bool reader_scroll_step(uint8_t rid, const canvas1_t *c, int w, int h);

static bool resolve_doc_path(const char *topic, char *out_path, size_t out_sz) {
    if (!topic || !*topic) return false;

    if (strncmp(topic, "spartan://", 10) == 0 || strncmp(topic, "SPARTAN://", 10) == 0) {
        strncpy(out_path, topic, out_sz - 1);
        out_path[out_sz - 1] = '\0';
        return true;
    }

    if (topic[0] == '/' || topic[0] == '.') {
        int fd = vfs_open(topic, VFS_O_READ);
        if (fd >= 0) {
            vfs_close(fd);
            strncpy(out_path, topic, out_sz - 1);
            out_path[out_sz - 1] = '\0';
            return true;
        }
    }

    char clean[64];
    strncpy(clean, topic, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    char *dot = NULL;
    for (char *p = clean; *p; p++) {
        if (*p == '.') dot = p;
    }
    if (dot && (strcmp(dot, ".md") == 0 || strcmp(dot, ".gmi") == 0 || strcmp(dot, ".txt") == 0)) {
        *dot = '\0';
    }

    /* 0. Try .md then .gmi on /nvme0/help */
    if (vfs_volume_mounted("nvme0")) {
        ksnprintf(out_path, out_sz, "/nvme0/help/%s.md", clean);
        int fd = vfs_open(out_path, VFS_O_READ);
        if (fd >= 0) { vfs_close(fd); return true; }

        ksnprintf(out_path, out_sz, "/nvme0/help/%s.gmi", clean);
        fd = vfs_open(out_path, VFS_O_READ);
        if (fd >= 0) { vfs_close(fd); return true; }
    }

    /* 1. Try .md then .gmi on /sd0/help */
    ksnprintf(out_path, out_sz, "/sd0/help/%s.md", clean);
    int fd = vfs_open(out_path, VFS_O_READ);
    if (fd >= 0) { vfs_close(fd); return true; }

    ksnprintf(out_path, out_sz, "/sd0/help/%s.gmi", clean);
    fd = vfs_open(out_path, VFS_O_READ);
    if (fd >= 0) { vfs_close(fd); return true; }

    /* 2. Try .md then .gmi on /flash0/help */
    ksnprintf(out_path, out_sz, "/flash0/help/%s.md", clean);
    fd = vfs_open(out_path, VFS_O_READ);
    if (fd >= 0) { vfs_close(fd); return true; }

    ksnprintf(out_path, out_sz, "/flash0/help/%s.gmi", clean);
    fd = vfs_open(out_path, VFS_O_READ);
    if (fd >= 0) { vfs_close(fd); return true; }

    /* 3. Try /nvme0/pub/ */
    if (vfs_volume_mounted("nvme0")) {
        ksnprintf(out_path, out_sz, "/nvme0/pub/%s.md", clean);
        fd = vfs_open(out_path, VFS_O_READ);
        if (fd >= 0) { vfs_close(fd); return true; }

        ksnprintf(out_path, out_sz, "/nvme0/pub/%s.gmi", clean);
        fd = vfs_open(out_path, VFS_O_READ);
        if (fd >= 0) { vfs_close(fd); return true; }
    }

    /* 4. Try /sd0/pub/ */
    ksnprintf(out_path, out_sz, "/sd0/pub/%s.md", clean);
    fd = vfs_open(out_path, VFS_O_READ);
    if (fd >= 0) { vfs_close(fd); return true; }

    ksnprintf(out_path, out_sz, "/sd0/pub/%s.gmi", clean);
    fd = vfs_open(out_path, VFS_O_READ);
    if (fd >= 0) { vfs_close(fd); return true; }

    return false;
}

#if READER_EPUB_SUPPORT
static bool has_epub_ext(const char *p) {
    if (!p) return false;
    size_t n = strlen(p);
    if (n < 5) return false;
    const char *e = p + n - 5;
    return (e[0] == '.' &&
            (e[1] == 'e' || e[1] == 'E') &&
            (e[2] == 'p' || e[2] == 'P') &&
            (e[3] == 'u' || e[3] == 'U') &&
            (e[4] == 'b' || e[4] == 'B'));
}

static int epub_vfs_read_cb(void *ctx, uint32_t off, void *dst, uint32_t len) {
    int fd = *(int *)ctx;
    uint8_t *out = (uint8_t *)dst;
    uint32_t done = 0;
    while (done < len) {
        int n = vfs_pread(fd, out + done, len - done, (uint64_t)off + done);
        if (n <= 0) return -1;
        done += (uint32_t)n;
    }
    return 0;
}

static void reader_free_epub(reader_state_t *r) {
    if (!r) return;
    r->is_epub = false;
    r->epub_chapter = 0;
    r->epub_md_len = 0;
    r->epub_err[0] = '\0';
    if (r->epub_book) {
        palloc_free(r->epub_book, r->epub_book_pages);
        r->epub_book = NULL;
        r->epub_book_pages = 0;
    }
    if (r->epub_md) {
        palloc_free(r->epub_md, r->epub_md_pages);
        r->epub_md = NULL;
        r->epub_md_pages = 0;
    }
}

static uint8_t *reader_alloc_epub_scratch(uint32_t *out_pages) {
    static const uint32_t k_tries[] = { 512u, 256u, 128u };
    for (size_t i = 0; i < sizeof(k_tries) / sizeof(k_tries[0]); i++) {
        uint8_t *p = (uint8_t *)palloc_pages_bulk(k_tries[i]);
        if (p) {
            *out_pages = k_tries[i];
            return p;
        }
    }
    *out_pages = 0;
    return NULL;
}

static bool reader_load_epub_chapter(reader_state_t *r, int ch) {
    if (!r || !r->is_epub || !r->epub_book || !r->epub_md) return false;
    if (ch < 0 || ch >= r->epub_book->spine_count) return false;

    int fd = vfs_open(r->path, VFS_O_READ);
    if (fd < 0) return false;

    uint32_t scratch_pages = 0;
    uint8_t *scratch = reader_alloc_epub_scratch(&scratch_pages);
    if (!scratch) {
        vfs_close(fd);
        return false;
    }

    r->epub_book->read_fn = epub_vfs_read_cb;
    r->epub_book->read_ctx = &fd;
    uint32_t md_cap = r->epub_md_pages * 4096u;
    uint32_t out_len = 0;
    epub_err_t err = epub_load_chapter_md(r->epub_book, ch, scratch, scratch_pages * 4096u,
                                          r->epub_md, md_cap, &out_len);
    palloc_free(scratch, scratch_pages);
    vfs_close(fd);
    if (err != EPUB_OK) return false;

    r->epub_chapter = ch;
    r->epub_md_len = out_len;
    r->scroll_y = 0;
    r->cp_valid = false;
    r->drawn_scroll_y = -1;
    return true;
}

static bool reader_open_epub(reader_state_t *r, const char *path) {
    vfs_stat_t st;
    if (vfs_stat(path, &st) != 0 || st.is_dir || st.size < 22u) return false;
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return false;

    uint32_t book_pages = (uint32_t)((sizeof(epub_book_t) + 4095u) / 4096u);
    uint32_t md_pages = 128u; /* 512 KB chapter Markdown buffer */
    uint32_t scratch_pages = 0;

    epub_book_t *book = (epub_book_t *)palloc_pages_bulk(book_pages);
    char *md = (char *)palloc_pages_bulk(md_pages);
    if (!md) {
        md_pages = 64u;
        md = (char *)palloc_pages_bulk(md_pages);
    }
    uint8_t *scratch = reader_alloc_epub_scratch(&scratch_pages);
    if (!book || !md || !scratch) {
        if (book) palloc_free(book, book_pages);
        if (md) palloc_free(md, md_pages);
        if (scratch) palloc_free(scratch, scratch_pages);
        vfs_close(fd);
        return false;
    }

    epub_err_t err = epub_open(book, epub_vfs_read_cb, &fd, st.size,
                               scratch, scratch_pages * 4096u);
    uint32_t out_len = 0;
    int init_ch = 0;
    if (err == EPUB_OK) {
        bool found_text = false;
        for (int c = 0; c < book->spine_count; c++) {
            uint32_t clen = 0;
            epub_err_t cerr = epub_load_chapter_md(book, c, scratch, scratch_pages * 4096u,
                                                   md, md_pages * 4096u, &clen);
            if (cerr != EPUB_OK) {
                err = cerr;
                break;
            }
            uint32_t nonws = 0;
            for (uint32_t k = 0; k < clen && nonws < 16u; k++) {
                if ((uint8_t)md[k] > ' ') nonws++;
            }
            init_ch = c;
            out_len = clen;
            if (nonws >= 16u) {
                found_text = true;
                break;
            }
        }
        if (err == EPUB_OK && !found_text && init_ch != 0) {
            init_ch = 0;
            err = epub_load_chapter_md(book, 0, scratch, scratch_pages * 4096u,
                                       md, md_pages * 4096u, &out_len);
        }
    }
    palloc_free(scratch, scratch_pages);
    vfs_close(fd);

    if (err != EPUB_OK) {
        palloc_free(book, book_pages);
        palloc_free(md, md_pages);
        if (err == EPUB_ERR_DRM) {
            strncpy(r->epub_err, "DRM-protected EPUB is not supported.", sizeof(r->epub_err) - 1);
        } else {
            ksnprintf(r->epub_err, sizeof(r->epub_err), "Cannot open EPUB (err %d).", (int)err);
        }
        return false;
    }

    reader_free_epub(r);
    r->is_epub = true;
    r->epub_book = book;
    r->epub_book_pages = book_pages;
    r->epub_chapter = init_ch;
    r->epub_md = md;
    r->epub_md_pages = md_pages;
    r->epub_md_len = out_len;
#if defined(TOUCH_PRESENT)
    r->fullscreen = true; /* epub opens directly in distraction-free fullscreen on touch devices */
    if (!lcdterm_fullscreen()) console_hotkey(CONSOLE_HOTKEY_FULLSCREEN);
#else
    r->fullscreen = false;
#endif
    if (book->title[0]) {
        strncpy(r->title, book->title, sizeof(r->title) - 1);
        r->title[sizeof(r->title) - 1] = '\0';
    }
    return true;
}
#endif

static canvas1_t *get_reader_canvas(uint8_t rid);

static bool reader_open_internal(uint8_t rid, const char *resource, bool push_history) {
    if (!resource || !*resource || rid >= READER_MAX_INSTANCES) return false;
    reader_state_t *r = &s_readers[rid];

    char resolved[128];
    if (!resolve_doc_path(resource, resolved, sizeof(resolved))) {
        strncpy(resolved, resource, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }

    if (push_history && r->path[0] && r->history_depth < READER_STACK_MAX) {
        strncpy(r->history[r->history_depth].path, r->path, sizeof(r->history[0].path) - 1);
        r->history[r->history_depth].path[sizeof(r->history[0].path) - 1] = '\0';
        r->history[r->history_depth].scroll_y = r->scroll_y;
        r->history_depth++;
    }

    bool is_spartan = (strncmp(resolved, "spartan://", 10) == 0 || strncmp(resolved, "SPARTAN://", 10) == 0);
    if (is_spartan) {
        char redir[128] = {0};
        char err[128] = {0};
        char tmp_path[32];
        ksnprintf(tmp_path, sizeof(tmp_path), "/ram0/spartan%d.gmi", (int)rid);
        int rc = spartan_fetch(resolved, tmp_path, redir, sizeof(redir), err, sizeof(err));
        if (rc == SPARTAN_REDIRECT && redir[0]) {
            rc = spartan_fetch(redir, tmp_path, NULL, 0, err, sizeof(err));
        }
        if (rc != SPARTAN_OK) {
            return false;
        }
    }

    /* From here the instance changes under what the compositor reads it
     * with; the fetch above is not, it may take seconds. */
    console_lock();
#if READER_EPUB_SUPPORT
    reader_free_epub(r);
#endif
    r->fullscreen = false;
    strncpy(r->path, resolved, sizeof(r->path) - 1);
    r->path[sizeof(r->path) - 1] = '\0';

    /* Set default title from filename */
    const char *slash = NULL;
    for (const char *sp = resolved; *sp; sp++) {
        if (*sp == '/') slash = sp;
    }
    const char *name = slash ? slash + 1 : resolved;
    strncpy(r->title, name, sizeof(r->title) - 1);
    r->title[sizeof(r->title) - 1] = '\0';

    r->scroll_y = 0;
    r->total_h = 0;
    r->link_count = 0;
    r->cp_valid = false;
    r->drawn_scroll_y = -1;
    r->active = true;

#if READER_EPUB_SUPPORT
    if (has_epub_ext(resolved)) {
        (void)reader_open_epub(r, resolved);
    }
#endif

    if (!r->aa_buf) {
        canvas1_t *c = get_reader_canvas(rid);
        uint32_t aa_w = (c && c->w > 1280u) ? c->w : 1280u;
        uint32_t aa_h = (c && c->h > 1280u) ? c->h : 1280u;
        uint32_t pages = (aa_w * aa_h + 4095u) / 4096u;
        r->aa_buf = (uint8_t *)palloc_pages_bulk(pages);
        if (r->aa_buf) {
            r->aa_pages = pages;
            r->aa_stride = aa_w;
            r->aa_w = aa_w;
            r->aa_h = aa_h;
        }
    }
    console_unlock();
    return true;
}

void reader_init(void) {
    memset(s_readers, 0, sizeof(s_readers));
    s_active_reader_rid = 0;
}

/* Focuses reader rid's window on the ribbon. */
static bool reader_focus_window(uint8_t rid) {
    uint8_t rreq[2] = { 'R', 0 }, rreply[SCREEN_REPLY_LEN];
    if (!console_canvas(rreq, 1, rreply) || rreply[0] != 0) return false;
    uint8_t count = rreply[1];
    for (uint8_t w = 0; w < count; w++) {
        rreq[1] = w;
        if (!console_canvas(rreq, 2, rreply) || rreply[0] != 0) continue;
        if (rreply[1] != 3 /* RIBBON_WIN_READER */ || rreply[2] != rid) continue;
        uint8_t jreq[2] = { 'J', w };
        (void)console_canvas(jreq, 2, rreply);
        reader_set_active_instance(rid);
        console_set_active_win_type(3 /* RIBBON_WIN_READER */);
        return true;
    }
    return false;
}

int reader_open_window(const char *path, bool *reused) {
    if (reused) *reused = false;
    if (!path || !*path) return -1;
    uint8_t req[128];
    req[0] = 'O';
    req[1] = 0;                                          /* the default width */
    uint32_t len = (uint32_t)strlen(path);
    if (len > sizeof(req) - 3) len = sizeof(req) - 3;
    memcpy(req + 2, path, len);
    req[2 + len] = '\0';
    uint8_t reply[SCREEN_REPLY_LEN];
    if (console_canvas(req, 3 + len, reply) && reply[0] == 0) {
        uint8_t rid = reply[1];
        int w = (int)(reply[2] | (reply[3] << 8));
        int h = (int)(reply[4] | (reply[5] << 8));
        if (reader_open_instance(rid, path) < 0) return -1;
        reader_set_target(rid, w, h);
        reader_update_instance(rid);
        reader_set_active_instance(rid);
        console_set_active_win_type(3 /* RIBBON_WIN_READER */);
        return rid;
    }
    /* No room for another: the focused reader, or the first open one,
     * shows it instead (Back returns). */
    int rid = reader_instance_is_active(s_active_reader_rid) ? (int)s_active_reader_rid : -1;
    for (uint8_t i = 0; rid < 0 && i < READER_MAX_INSTANCES; i++)
        if (s_readers[i].active && s_readers[i].win_w > 0) rid = i;
    if (rid < 0 || !reader_focus_window((uint8_t)rid)) return -1;
    if (reader_open_instance((uint8_t)rid, path) < 0) return -1;
    reader_update_instance((uint8_t)rid);
    if (reused) *reused = true;
    return rid;
}

int reader_open_instance(uint8_t rid, const char *resource) {
    if (rid >= READER_MAX_INSTANCES) return -1;
    if (reader_open_internal(rid, resource, true)) {
        s_active_reader_rid = rid;
        return (int)rid;
    }
    return -1;
}

bool reader_open(const char *resource) {
    return reader_open_instance(s_active_reader_rid, resource) >= 0;
}

/* Under console_lock and the panel's frame lock: the conversion composites
 * from aa_buf under the frame lock, and the page must not be freed in the
 * middle of that. */
void reader_close_instance(uint8_t rid) {
    if (rid < READER_MAX_INSTANCES) {
        console_lock();
        s_readers[rid].active = false;
        s_readers[rid].fullscreen = false;
        s_readers[rid].path[0] = '\0';
        s_readers[rid].title[0] = '\0';
#if READER_EPUB_SUPPORT
        reader_free_epub(&s_readers[rid]);
#endif
        if (s_readers[rid].aa_buf) {
            console_frame_lock();
            palloc_free(s_readers[rid].aa_buf, s_readers[rid].aa_pages);
            s_readers[rid].aa_buf = NULL;
            s_readers[rid].aa_pages = 0;
            console_frame_unlock();
        }
        console_unlock();
    }
}

void reader_close(void) {
    reader_close_instance(s_active_reader_rid);
}

uint8_t *reader_get_aa_backing(uint8_t rid, uint32_t *out_stride, int *out_w, int *out_h) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active || !s_readers[rid].aa_buf)
        return NULL;
    if (out_stride) *out_stride = s_readers[rid].aa_stride;
    if (out_w) *out_w = s_readers[rid].win_w > 0 ? s_readers[rid].win_w : s_readers[rid].aa_w;
    if (out_h) *out_h = s_readers[rid].win_h > 0 ? s_readers[rid].win_h : s_readers[rid].aa_h;
    return s_readers[rid].aa_buf;
}

static canvas1_t *get_reader_canvas(uint8_t rid) {
    if (rid >= READER_MAX_INSTANCES) return 0;
#if defined(LCDTERM_PRESENT)
    canvas1_t *c = lcdterm_get_reader_backing(rid);
    if (c) return c;
#endif
    return ramscreen_get_reader_backing(rid);
}

bool reader_set_target(uint8_t rid, int w, int h) {
    if (rid >= READER_MAX_INSTANCES) return false;
    bool changed = false;
    if (w > 0 && s_readers[rid].win_w != w) {
        s_readers[rid].win_w = w;
        s_readers[rid].cp_valid = false;
        changed = true;
    }
    if (h > 0 && s_readers[rid].win_h != h) {
        s_readers[rid].win_h = h;
        changed = true;
    }
    return changed;
}

/* The reader's 1-bpp backing, drawn, into its tile on the screen (the
 * screen's 'W'): only that tile, not the whole screen repainted. */
static void reader_show(uint8_t rid) {
    uint8_t req[2] = { 'W', rid }, reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 2, reply);
}

void reader_update_instance(uint8_t rid) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return;
    canvas1_t *c = get_reader_canvas(rid);
    if (!c) return;
    int w = s_readers[rid].win_w > 0 ? s_readers[rid].win_w : (int)c->w;
    int h = s_readers[rid].win_h > 0 ? s_readers[rid].win_h : (int)c->h;
    reader_draw(rid, c, w, h);

    /* Update title on ribbon reader window */
    const char *title = reader_get_title_instance(rid);
    uint8_t req[SCREEN_CTITLE_MAX + 2];
    req[0] = 'T';
    uint32_t tl = (uint32_t)strlen(title);
    if (tl > sizeof(req) - 2) tl = sizeof(req) - 2;
    memcpy(req + 1, title, tl);
    req[1 + tl] = '\0';
    uint8_t reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 2 + tl, reply);
    reader_show(rid);
}

/* FNV-1a over the reader's surfaces as drawn: its GRAY8 surface where it has
 * one, and its 1-bpp backing. */
static uint64_t reader_surface_hash(uint8_t rid) {
    reader_state_t *r = &s_readers[rid];
    uint64_t hsh = 0xcbf29ce484222325ull;
    int w = r->win_w, h = r->win_h;
    if (r->aa_buf && w > 0 && h > 0 && w <= r->aa_w && h <= r->aa_h) {
        for (int y = 0; y < h; y++) {
            const uint8_t *row = r->aa_buf + (uint32_t)y * r->aa_stride;
            for (int x = 0; x < w; x++) { hsh ^= row[x]; hsh *= 0x100000001b3ull; }
        }
    }
    /* The 1-bpp backing is screen-sized; only the window's part is shown (the
     * screen's 'W' copies w x h of it), and a full draw lets a last line run
     * past the window's bottom into rows a strip draw clips. */
    canvas1_t *c = get_reader_canvas(rid);
    if (c && w > 0 && h > 0) {
        for (int y = 0; y < h && y < (int)c->h; y++) {
            const uint8_t *row = c->fb + (uint32_t)y * c->stride;
            for (int b = 0; b < w / 8; b++) { hsh ^= row[b]; hsh *= 0x100000001b3ull; }
            for (int x = (w / 8) * 8; x < w; x++) { hsh ^= (row[x / 8] >> (x % 8)) & 1u; hsh *= 0x100000001b3ull; }
        }
    }
    return hsh;
}

int reader_layout_check(uint8_t rid) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return -1;
    reader_state_t *r = &s_readers[rid];
    bool had = r->cp_valid;
    reader_update_instance(rid);               /* from the checkpoints, if it has them */
    uint64_t a = reader_surface_hash(rid);
    r->cp_valid = false;
    reader_update_instance(rid);               /* the whole file */
    uint64_t b = reader_surface_hash(rid);
    return !had ? 2 : (a == b ? 1 : 0);
}

int reader_strip_check(uint8_t rid, int dy) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return -1;
    reader_state_t *r = &s_readers[rid];
    canvas1_t *c = get_reader_canvas(rid);
    int w = r->win_w, h = r->win_h;
    if (!c || w <= 0 || h <= 0) return -1;
    int max_scroll = r->total_h - 120;
    int start = r->scroll_y;
    int target = start + dy;
    if (target < 0) target = 0;
    if (target > max_scroll) target = max_scroll > 0 ? max_scroll : 0;
    reader_update_instance(rid);               /* the surfaces at start, whole */
    r->scroll_y = target;
    if (!reader_scroll_step(rid, c, w, h)) { reader_update_instance(rid); return 2; }
    uint64_t a = reader_surface_hash(rid);
    /* Row hashes of the stepped GRAY8 surface, to say where it differs. */
    uint32_t pages = ((uint32_t)h * 8u + PAGE_SIZE - 1u) / PAGE_SIZE;
    uint64_t *rows = r->aa_buf ? (uint64_t *)palloc_pages_bulk(pages) : NULL;
    if (rows)
        for (int y = 0; y < h; y++) {
            uint64_t hh = 0xcbf29ce484222325ull;
            const uint8_t *p = r->aa_buf + (uint32_t)y * r->aa_stride;
            for (int x = 0; x < w; x++) { hh ^= p[x]; hh *= 0x100000001b3ull; }
            rows[y] = hh;
        }
    reader_update_instance(rid);               /* the same place, drawn whole */
    uint64_t b = reader_surface_hash(rid);
    r->strip_diff_y0 = r->strip_diff_y1 = -1;
    if (rows) {
        for (int y = 0; y < h; y++) {
            uint64_t hh = 0xcbf29ce484222325ull;
            const uint8_t *p = r->aa_buf + (uint32_t)y * r->aa_stride;
            for (int x = 0; x < w; x++) { hh ^= p[x]; hh *= 0x100000001b3ull; }
            if (hh != rows[y]) { if (r->strip_diff_y0 < 0) r->strip_diff_y0 = y; r->strip_diff_y1 = y; }
        }
        palloc_free(rows, pages);
    }
    return a == b ? 1 : 0;
}

void reader_strip_diff(uint8_t rid, int *y0, int *y1) {
    *y0 = rid < READER_MAX_INSTANCES ? s_readers[rid].strip_diff_y0 : -1;
    *y1 = rid < READER_MAX_INSTANCES ? s_readers[rid].strip_diff_y1 : -1;
}

void reader_update(void) {
    reader_update_instance(s_active_reader_rid);
}

void reader_scroll_instance(uint8_t rid, int delta_y) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active || delta_y == 0) return;
    int start_y = s_readers[rid].scroll_y;
    int target_y = start_y + delta_y;
    if (target_y < 0) target_y = 0;
    int max_scroll = s_readers[rid].total_h - 120;
    if (max_scroll < 0) max_scroll = 0;
    if (target_y > max_scroll) target_y = max_scroll;
    if (target_y == start_y) return;
#if defined(LCDTERM_PRESENT)
    int diff = target_y - start_y;
    canvas1_t *c = get_reader_canvas(rid);
    int w = s_readers[rid].win_w > 0 ? s_readers[rid].win_w : (c ? (int)c->w : 0);
    int h = s_readers[rid].win_h > 0 ? s_readers[rid].win_h : (c ? (int)c->h : 0);
    int abs_diff = diff < 0 ? -diff : diff;

    if (c && w > 0 && h > 0 && abs_diff > 16) {
        /* Smooth ease-out scrolling */
        const int steps = abs_diff <= 48 ? 3 : 5;
        static const int weights3[3] = { 450, 800, 1000 };
        static const int weights5[5] = { 280, 540, 750, 900, 1000 };
        const int *w_table = (steps == 3) ? weights3 : weights5;

        for (int s = 0; s < steps; s++) {
            s_readers[rid].scroll_y = start_y + (diff * w_table[s]) / 1000;
            if (!reader_scroll_step(rid, c, w, h)) reader_draw(rid, c, w, h);
            lcdterm_sync_post();
            if (view_pending()) break;                   /* a key or a touch for a view: it goes first */
        }
    }
#endif

    s_readers[rid].scroll_y = target_y;
    reader_update_instance(rid);
}

void reader_scroll(int delta_y) {
    reader_scroll_instance(s_active_reader_rid, delta_y);
}

void reader_touch_fling_instance(uint8_t rid, int dy, uint32_t ms) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return;
    if (ms < 10) ms = 10;
    if (ms > 500) ms = 500;

    int max_scroll = s_readers[rid].total_h - 120;
    if (max_scroll < 0) max_scroll = 0;
    if (max_scroll == 0) return;

    canvas1_t *c = get_reader_canvas(rid);
    int w = s_readers[rid].win_w > 0 ? s_readers[rid].win_w : (c ? (int)c->w : 0);
    int h = s_readers[rid].win_h > 0 ? s_readers[rid].win_h : (c ? (int)c->h : 0);
    if (!c || w <= 0 || h <= 0) return;

    /* Moving finger up (dy < 0) scrolls down (positive scroll_y change).
     * Scaled for responsive, rapid fling speed with smooth inertia. */
#if READER_IS_HIDPI
    int v = -(dy * 110) / (int)ms;
    if (v > 360) v = 360;
    if (v < -360) v = -360;
    if (v >= 0 && v < 24) v = 36;
    if (v < 0 && v > -24) v = -36;
#else
    int v = -(dy * 70) / (int)ms;
    if (v > 220) v = 220;
    if (v < -220) v = -220;
    if (v >= 0 && v < 16) v = 24;
    if (v < 0 && v > -16) v = -24;
#endif

    uint32_t start_presses = touch_press_count();

    for (int frame = 0; frame < 120; frame++) {
        int next_y = s_readers[rid].scroll_y + v;
        if (next_y < 0) {
            s_readers[rid].scroll_y = 0;
            break;
        }
        if (next_y > max_scroll) {
            s_readers[rid].scroll_y = max_scroll;
            break;
        }
        s_readers[rid].scroll_y = next_y;
        if (!reader_scroll_step(rid, c, w, h)) reader_draw(rid, c, w, h);
#if defined(LCDTERM_PRESENT)
        lcdterm_sync_post();
#endif
        /* Low friction decay: ~5% loss per frame at 60 Hz */
        v = (v * 95) / 100;
        int abs_v = v < 0 ? -v : v;
        if (abs_v <= 1) break;

        /* Stopped by a finger on the screen, or anything else for a view */
        if (view_pending() || touch_press_count() != start_presses) break;
    }

    reader_update_instance(rid);
}

void reader_touch_fling(int dy, uint32_t ms) {
    reader_touch_fling_instance(s_active_reader_rid, dy, ms);
}

void reader_drag(int dy) {
    uint8_t rid = s_active_reader_rid;
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active || dy == 0) return;
    reader_state_t *r = &s_readers[rid];
    canvas1_t *c = get_reader_canvas(rid);
    int w = r->win_w > 0 ? r->win_w : (c ? (int)c->w : 0);
    int h = r->win_h > 0 ? r->win_h : (c ? (int)c->h : 0);
    if (!c || w <= 0 || h <= 0) return;
    int max_scroll = r->total_h - 120;
    if (max_scroll < 0) max_scroll = 0;
    int target = r->scroll_y - dy;            /* the finger down pulls the document down */
    if (target < 0) target = 0;
    if (target > max_scroll) target = max_scroll;
    if (target == r->scroll_y) return;
    r->scroll_y = target;
    if (!reader_scroll_step(rid, c, w, h)) reader_draw(rid, c, w, h);
#if defined(LCDTERM_PRESENT)
    lcdterm_sync_post();
#endif
}

void reader_drag_end(int dy, uint32_t ms) {
    uint8_t rid = s_active_reader_rid;
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return;
    /* Still moving at least 3 mm over the last ~100 ms: a fling from there. */
    int fling_px = ugui_mm_px(30);
    if (fling_px <= 0) fling_px = 16;
    if (ms > 0 && (dy >= fling_px || dy <= -fling_px)) reader_touch_fling_instance(rid, dy, ms);
    else reader_update_instance(rid);
}

static bool reader_jump_anchor(uint8_t rid, const char *anchor) {
    if (rid >= READER_MAX_INSTANCES || !anchor || anchor[0] != '#') return false;
    reader_state_t *r = &s_readers[rid];
    const char *target = anchor + 1;

    /* Check if target matches footnote #fn:id */
    bool is_footnote = (strncmp(target, "fn:", 3) == 0);
    const char *fn_id = is_footnote ? target + 3 : NULL;

    for (int i = 0; i < r->toc_count; i++) {
        char slug[64];
        md_heading_slug(r->toc[i].title, slug, sizeof(slug));
        if (strcmp(slug, target) == 0) {
            if (r->history_depth < READER_STACK_MAX) {
                strncpy(r->history[r->history_depth].path, r->path, sizeof(r->history[0].path) - 1);
                r->history[r->history_depth].path[sizeof(r->history[0].path) - 1] = '\0';
                r->history[r->history_depth].scroll_y = r->scroll_y;
                r->history_depth++;
            }
            int target_y = r->toc[i].doc_y - 20;
            if (target_y < 0) target_y = 0;
            r->scroll_y = target_y;
            reader_update_instance(rid);
            return true;
        }
    }
    (void)fn_id;
    return false;
}

bool reader_back_instance(uint8_t rid) {
    if (rid >= READER_MAX_INSTANCES) return false;
    reader_state_t *r = &s_readers[rid];
    if (r->toc_mode) {
        r->toc_mode = false;
        reader_update_instance(rid);
        return true;
    }
    if (r->history_depth > 0) {
        r->history_depth--;
        reader_hist_entry_t entry = r->history[r->history_depth];
        if (strcmp(entry.path, r->path) == 0) {
            r->scroll_y = entry.scroll_y;
            reader_update_instance(rid);
            return true;
        }
        bool ok = reader_open_internal(rid, entry.path, false);
        if (ok) {
            r->scroll_y = entry.scroll_y;
            reader_update_instance(rid);
        }
        return ok;
    }
    return false;
}

bool reader_back(void) {
    return reader_back_instance(s_active_reader_rid);
}

/* The action bar's height. Its buttons are touched over more than they
 * show (ugui_set_min_target()), not drawn bigger. */
static int reader_bar_h(void) {
    return 26 * (int)CHROME_SCALE;
}

static int reader_effective_bar_h(const reader_state_t *r) {
    return (r && r->fullscreen && !r->toc_mode) ? 0 : reader_bar_h();
}

enum {
    READER_UI_BACK     = 1,
    READER_UI_CONTENTS = 2,
    READER_UI_ROTATE   = 3,
    READER_UI_TOP      = 4,
    READER_UI_FULL     = 5,
    READER_UI_SEPIA    = 6,
};

static bool reader_tap_act(uint8_t rid, int client_x, int client_y);

static void reader_redraw_bar(uint8_t rid);

uint8_t reader_active_instance(void) {
    return s_active_reader_rid;
}

static uint16_t reader_hit(const reader_state_t *r, int x, int y, int *link);

/* Arms what a tap there would act on (reader_hit()): nothing on a link. */
void reader_press(uint8_t rid, int client_x, int client_y) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return;
    reader_state_t *r = &s_readers[rid];
    int link;
    bool changed = reader_hit(r, client_x, client_y, &link) ? ugui_arm(&r->ui_ctx, client_x, client_y)
                                                            : ugui_disarm(&r->ui_ctx);
    if (changed) reader_redraw_bar(rid);
}

void reader_release(void) {
    for (uint8_t rid = 0; rid < READER_MAX_INSTANCES; rid++)
        if (s_readers[rid].active && ugui_disarm(&s_readers[rid].ui_ctx)) reader_redraw_bar(rid);
}

/* A tap: the pressed button lets go first, then acts; the bar is drawn
 * again only if the action did not draw the reader. */
bool reader_tap_instance(uint8_t rid, int client_x, int client_y) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return false;
    bool was_armed = ugui_disarm(&s_readers[rid].ui_ctx);
    uint32_t seq = s_reader_seq[rid];
    bool acted = reader_tap_act(rid, client_x, client_y);
    if (was_armed && s_readers[rid].active && s_reader_seq[rid] == seq) reader_redraw_bar(rid);
    return acted;
}

/* The link under (x, y) -- its words, 4 units taller -- or -1. Below the
 * action bar only. */
static int reader_link_at(const reader_state_t *r, int x, int y) {
    if (y < reader_effective_bar_h(r)) return -1;
    int pad_y = 4 * (int)CHROME_SCALE;
    for (int i = 0; i < r->link_count; i++)
        if (x >= r->links[i].x0 && x <= r->links[i].x1 &&
            y >= r->links[i].y0 - pad_y && y <= r->links[i].y1 + pad_y) return i;
    return -1;
}

/* What is under a finger at (x, y), in this order: a button as drawn, a
 * link as drawn, a button's grown touch area. So a button is easy to hit
 * where nothing else is, and a link just under the bar stays the link's. */
static uint16_t reader_hit(const reader_state_t *r, int x, int y, int *link) {
    uint16_t hit = ugui_hit_test_drawn(&r->ui_ctx, x, y, NULL);
    *link = -1;
    if (hit) return hit;
    *link = reader_link_at(r, x, y);
    return *link >= 0 ? 0 : ugui_hit_test(&r->ui_ctx, x, y, NULL);
}

static bool reader_page_step(uint8_t rid, int dir) {
    reader_state_t *r = &s_readers[rid];
    int win_h = r->win_h > 0 ? r->win_h : 400;
    int step = win_h - READER_LINE_H;
    if (step < 64) step = 64;
#if READER_EPUB_SUPPORT
    if (r->is_epub && r->epub_book) {
        int max_scroll = r->total_h > win_h ? (r->total_h - win_h) : 0;
        if (dir > 0 && r->scroll_y >= max_scroll &&
            r->epub_chapter + 1 < r->epub_book->spine_count) {
            if (reader_load_epub_chapter(r, r->epub_chapter + 1)) {
                reader_update_instance(rid);
                return true;
            }
        } else if (dir < 0 && r->scroll_y <= 0 && r->epub_chapter > 0) {
            if (reader_load_epub_chapter(r, r->epub_chapter - 1)) {
                reader_update_instance(rid);
                return true;
            }
        }
    }
#endif
    int target_y = r->scroll_y + dir * step;
    int max_scroll = r->total_h - 120;
    if (max_scroll < 0) max_scroll = 0;
    if (target_y < 0) target_y = 0;
    if (target_y > max_scroll) target_y = max_scroll;
    if (target_y != r->scroll_y) {
        r->scroll_y = target_y;
        reader_update_instance(rid);
    }
    return true;
}

static void reader_set_fullscreen(uint8_t rid, bool on) {
    reader_state_t *r = &s_readers[rid];
    r->fullscreen = on;
    r->toc_mode = false;
    r->cp_valid = false;
    if (lcdterm_fullscreen() != on) {
        console_hotkey(on ? CONSOLE_HOTKEY_FULLSCREEN : CONSOLE_HOTKEY_FS_LEAVE);
    }
    reader_update_instance(rid);
}

static bool reader_follow_target(uint8_t rid, const char *target) {
    reader_state_t *r = &s_readers[rid];
    if (r->toc_mode) r->toc_mode = false;

#if READER_EPUB_SUPPORT
    if (r->is_epub && r->epub_book) {
        if (strncmp(target, "#epub:", 6) == 0) {
            int ch = 0;
            for (const char *p = target + 6; *p >= '0' && *p <= '9'; p++) {
                ch = ch * 10 + (*p - '0');
            }
            if (reader_load_epub_chapter(r, ch)) {
                reader_update_instance(rid);
                return true;
            }
        }
        /* Relative in-book link (e.g. part0005.html#anchor): resolve inside EPUB spine. */
        if (target[0] != '#' && target[0] != '/' && !strstr(target, "://")) {
            int ch = epub_find_spine_href(r->epub_book, r->epub_chapter, target);
            if (ch >= 0 && reader_load_epub_chapter(r, ch)) {
                reader_update_instance(rid);
                return true;
            }
        }
    }
#endif

    if (target[0] == '#') {
        if (reader_jump_anchor(rid, target)) return true;
    }

    char resolved[128];
    bool ok = false;
    if (md_resolve_url(r->path, target, resolved, sizeof(resolved)) ||
        resolve_doc_path(target, resolved, sizeof(resolved))) {
        ok = reader_open_internal(rid, resolved, true);
    } else {
        ok = reader_open_internal(rid, target, true);
    }
    if (ok) reader_update_instance(rid);
    return ok;
}

static bool reader_tap_act(uint8_t rid, int client_x, int client_y) {
    reader_state_t *r = &s_readers[rid];

    int link = -1;
    uint16_t hit = reader_hit(r, client_x, client_y, &link);
    if (hit == READER_UI_BACK) {
        (void)reader_back_instance(rid);
        return true;
    }
    if (hit == READER_UI_CONTENTS) {
        r->toc_mode = !r->toc_mode;
        reader_update_instance(rid);
        return true;
    }
    if (hit == READER_UI_ROTATE) {
        console_hotkey(CONSOLE_HOTKEY_ORIENT);
        return true;
    }
    if (hit == READER_UI_TOP) {
        r->scroll_y = 0;
        r->toc_mode = false;
        reader_update_instance(rid);
        return true;
    }
    if (hit == READER_UI_FULL) {
        reader_set_fullscreen(rid, !r->fullscreen);
        return true;
    }
    if (hit == READER_UI_SEPIA) {
        bool is_sepia = settings_get("display.warmth") >= 5;
        if (is_sepia) {
            (void)settings_set("display.warmth", 0);
        } else {
            (void)settings_set("display.warm_ink", 1);
            (void)settings_set("display.warmth", 8);
        }
        reader_redraw_bar(rid);
        return true;
    }

    if (link < 0) {
        if (r->fullscreen && !r->toc_mode) {
            int w = r->win_w > 0 ? r->win_w : 480;
            if (client_x < w / 3 || client_x > (2 * w) / 3) {
                uint64_t now = time_get_ms();
                if (r->last_page_tap_ms && (now - r->last_page_tap_ms) < 300u) return true;
                r->last_page_tap_ms = now;
                return reader_page_step(rid, client_x < w / 3 ? -1 : 1);
            }
            reader_set_fullscreen(rid, false);
            return true;
        }
        return false;
    }

    char target[128];
    strncpy(target, r->links[link].url, sizeof(target) - 1);
    target[sizeof(target) - 1] = '\0';
    return reader_follow_target(rid, target);
}

bool reader_tap(int client_x, int client_y) {
    return reader_tap_instance(s_active_reader_rid, client_x, client_y);
}

bool reader_follow_link_num_instance(uint8_t rid, int num) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return false;
    reader_state_t *r = &s_readers[rid];
    for (int i = 0; i < r->link_count; i++) {
        if (r->links[i].num == num) {
            char target[128];
            strncpy(target, r->links[i].url, sizeof(target) - 1);
            target[sizeof(target) - 1] = '\0';
            return reader_follow_target(rid, target);
        }
    }
    return false;
}

bool reader_follow_link_num(int num) {
    return reader_follow_link_num_instance(s_active_reader_rid, num);
}

/* A key for the active reader (kernel/keyseq.h's codes). Vi- and
 * Emacs-style: j / k / Ctrl-N / Ctrl-P / arrows a line, Space / Ctrl-V /
 * PgDn / Right a page down, Alt-V / PgUp a page up, d / u half a page, g /
 * Home and G / End the ends, b / Backspace / Left back, t the contents, 1-9
 * a link, r reload, o rotate, ? / h / Ctrl-H help, q / Ctrl-C close. The
 * system's Cmd shortcuts never come here: the console takes them. */
static void reader_key(uint8_t rid, uint32_t key, uint8_t mods) {
    if (rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return;
    reader_state_t *r = &s_readers[rid];
    int win_h = r->win_h > 0 ? r->win_h : 480;
    int page_dy = win_h - 40;
    if (page_dy < 40) page_dy = 40;

    if (mods & KMOD_ALT) {
        if (key == 'v' || key == 'V') reader_scroll_instance(rid, -page_dy);   /* Emacs page up */
        return;
    }
    switch (key) {
    case 0x08: case '?': case 'h':
        reader_open_instance(rid, vfs_volume_mounted("nvme0") ? "/nvme0/help/reader.md" : "/sd0/help/reader.md");
        reader_update_instance(rid);
        return;
    case 'q': case 0x03: {
        if (r->toc_mode) {
            r->toc_mode = false;
            reader_update_instance(rid);
            return;
        }
#if defined(CONFIG_BOARD_TAB5)
        int rot = dsi_lcd_get_orientation();
        if (rot == 0 || rot == 180) {
            reader_close_instance(rid);
            orient_manual();
            dsi_lcd_set_orientation(270);
            console_set_active_win_type(1 /* RIBBON_WIN_TERM */);
            return;
        }
#endif
        console_hotkey(CONSOLE_HOTKEY_CLOSE);           /* as Cmd-W: the focus moves on properly */
        return;
    }
    case 'b': case 0x7f: case KEY_LEFT:
        reader_back_instance(rid);
        return;
    case ' ': case 0x16: case KEY_PGDN: case KEY_RIGHT:
        reader_scroll_instance(rid, page_dy);
        return;
    case KEY_PGUP:
        reader_scroll_instance(rid, -page_dy);
        return;
    case 'j': case 0x0e: case '\r': case '\n': case KEY_DOWN:
        reader_scroll_instance(rid, 32);
        return;
    case 'k': case 0x10: case KEY_UP:
        reader_scroll_instance(rid, -32);
        return;
    case 'd': case 0x04:
        reader_scroll_instance(rid, page_dy / 2);
        return;
    case 'u': case 0x15:
        reader_scroll_instance(rid, -page_dy / 2);
        return;
    case 'g': case KEY_HOME:
        r->scroll_y = 0;
        reader_update_instance(rid);
        return;
    case 'G': case KEY_END: {
        int max_scroll = r->total_h - 120;
        r->scroll_y = max_scroll > 0 ? max_scroll : 0;
        reader_update_instance(rid);
        return;
    }
    case 'r': case 0x12:
        r->cp_valid = false;                             /* the file may have changed under it */
        reader_update_instance(rid);
        return;
    case 'o': case 'O':
        console_hotkey(CONSOLE_HOTKEY_ORIENT);
        return;
    case 't': case 'T':
        r->toc_mode = !r->toc_mode;
        reader_update_instance(rid);
        return;
    case 'f': case 'F':
        reader_set_fullscreen(rid, !r->fullscreen);
        return;
#if READER_EPUB_SUPPORT
    case 'n': case 'N':
        if (r->is_epub && r->epub_book &&
            r->epub_chapter + 1 < r->epub_book->spine_count &&
            reader_load_epub_chapter(r, r->epub_chapter + 1)) {
            reader_update_instance(rid);
        }
        return;
    case 'p': case 'P':
        if (r->is_epub && r->epub_book && r->epub_chapter > 0 &&
            reader_load_epub_chapter(r, r->epub_chapter - 1)) {
            reader_update_instance(rid);
        }
        return;
#endif
    default:
        if (key >= '1' && key <= '9') reader_follow_link_num_instance(rid, (int)(key - '0'));
        return;
    }
}


/* `ui.text_size` changed (setting index `key`): laid out again in the new
 * face, at about the same place -- the scroll scaled by the line heights. */
static void reader_text_size_changed(uint8_t rid, uint32_t key) {
    const setting_def_t *d = settings_at(key);
    if (!d || (strcmp(d->key, "ui.text_size") != 0 && strcmp(d->key, "ui.font_size") != 0) ||
        rid >= READER_MAX_INSTANCES || !s_readers[rid].active) return;
    reader_state_t *r = &s_readers[rid];
    int old_h = READER_LINE_H;
    reader_metrics();
    r->scroll_y = (int)((long)r->scroll_y * READER_LINE_H / old_h);
    r->cp_valid = false;
    reader_update_instance(rid);
}

/* The reader as a view (drivers/view.h): each event onto what it did before. */
static void reader_view_event(uint8_t rid, const win_event_t *ev) {
    switch (ev->type) {
    case WIN_EV_POINTER_DOWN:
        if (ev->button == 1 && (ev->flags & WIN_EVF_PRESS)) reader_press(rid, ev->x, ev->y);
        return;
    case WIN_EV_POINTER_UP:
        if (ev->button == 2) return;                     /* no context menu in the reader yet */
        if (ev->flags & WIN_EVF_TAP) (void)reader_tap_instance(rid, ev->x, ev->y);
        else if (ev->flags & WIN_EVF_PRESS) reader_release();
        return;
    case WIN_EV_POINTER_MOVE:
        if (ev->button == 0 && rid < READER_MAX_INSTANCES && s_readers[rid].active &&
            ugui_hover(&s_readers[rid].ui_ctx, ev->x, ev->y))
            reader_redraw_bar(rid);
        return;
    case WIN_EV_DRAG:
        reader_drag(ev->dy);
        return;
    case WIN_EV_DRAG_END:
        if (rid < READER_MAX_INSTANCES && s_readers[rid].fullscreen &&
            !s_readers[rid].toc_mode && ev->dy > 80) {
            reader_set_fullscreen(rid, false);
            return;
        }
        reader_drag_end(ev->dy, ev->key);
        return;
    case WIN_EV_SCROLL:
        if (rid < READER_MAX_INSTANCES && s_readers[rid].fullscreen &&
            !s_readers[rid].toc_mode && !(ev->flags & WIN_EVF_WHEEL) && ev->dy > 60) {
            reader_set_fullscreen(rid, false);
            return;
        }
        if (ev->flags & WIN_EVF_WHEEL) reader_scroll_instance(rid, -(int)ev->dy * 3 * READER_LINE_H);   /* three lines a notch */
        else if (ev->key > 0 && ev->dy != 0) reader_touch_fling(ev->dy, ev->key);
        else reader_scroll(ev->dy > 0 ? -160 : 160);
        return;
    case WIN_EV_KEY_DOWN:
        reader_key(rid, ev->key, ev->mods);
        return;
    case WIN_EV_SETTINGS:
        reader_text_size_changed(rid, ev->key);
        return;
    default:
        return;
    }
}

static void reader_view_resize(uint8_t rid, int w, int h) {
    bool changed = reader_set_target(rid, w, h);
    if (rid < READER_MAX_INSTANCES) {
        bool os_fs = lcdterm_fullscreen();
        if ((rid == s_active_reader_rid || !os_fs) && s_readers[rid].fullscreen != os_fs) {
            s_readers[rid].fullscreen = os_fs;
            s_readers[rid].cp_valid = false;
            changed = true;
        }
    }
    if (changed) reader_update_instance(rid);
}

const view_ops_t g_reader_view = {
    .name = "reader",
    .active = reader_instance_is_active,
    .event = reader_view_event,
    .captures = NULL,
    .resize = reader_view_resize,
    .close = reader_close_instance,
    .surface = reader_get_aa_backing,
    .seq = reader_get_seq,
};

const char *reader_get_title_instance(uint8_t rid) {
    if (rid >= READER_MAX_INSTANCES) return "Reader";
    return s_readers[rid].title[0] ? s_readers[rid].title : "Reader";
}

const char *reader_get_title(void) {
    return reader_get_title_instance(s_active_reader_rid);
}

const char *reader_get_path_instance(uint8_t rid) {
    if (rid >= READER_MAX_INSTANCES) return "";
    return s_readers[rid].path;
}

int reader_get_scroll_instance(uint8_t rid) {
    return rid < READER_MAX_INSTANCES ? s_readers[rid].scroll_y : 0;
}

int reader_get_height_instance(uint8_t rid) {
    return rid < READER_MAX_INSTANCES ? s_readers[rid].total_h : 0;
}

const char *reader_get_path(void) {
    return reader_get_path_instance(s_active_reader_rid);
}

bool reader_instance_is_active(uint8_t rid) {
    return (rid < READER_MAX_INSTANCES && s_readers[rid].active);
}

bool reader_is_active(void) {
    return reader_instance_is_active(s_active_reader_rid);
}

uint8_t reader_get_active_instance(void) {
    return s_active_reader_rid;
}

void reader_set_active_instance(uint8_t rid) {
    if (rid < READER_MAX_INSTANCES) {
        s_active_reader_rid = rid;
    }
}

int reader_find_by_path(const char *path) {
    if (!path || !*path) return -1;
    char resolved[128];
    if (!resolve_doc_path(path, resolved, sizeof(resolved))) {
        strncpy(resolved, path, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }
    for (int i = 0; i < READER_MAX_INSTANCES; i++) {
        if (s_readers[i].active && (strcmp(s_readers[i].path, path) == 0 || strcmp(s_readers[i].path, resolved) == 0)) {
            return i;
        }
    }
    return -1;
}

/* The reader draws through ugui's surface and primitives (drivers/ugui.h):
 * the same ink and paper (READER_FG_GRAY8 / READER_BG_GRAY8 are ugui's), the
 * same fonts at the same scale. These keep the reader's inclusive corners
 * and its monospace limit. */
static void rdraw_fill(const ugui_surface_t *rd, int x0, int y0, int x1, int y1, uint8_t gray, unsigned pat1) {
    if (x1 < x0 || y1 < y0) return;
    ugui_fill(rd, (ugui_rect_t){ (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0 + 1), (int16_t)(y1 - y0 + 1) }, gray, pat1);
}

static void rdraw_hline(const ugui_surface_t *rd, int x0, int x1, int y, uint8_t gray, unsigned pat1) {
    ugui_hline(rd, x0, x1, y, 1, gray, pat1);
}

static int rdraw_serif_text(const ugui_surface_t *rd, int x, int y, const char *s, uint32_t len, bool bold, bool italic) {
    return ugui_face_text(rd, READER_FACE, x, y, s, len, bold, italic, false);
}

static int rdraw_mono_cell_w(const ugui_surface_t *rd) {
    if (READER_FACE && rd && rd->aa_fb) {
        int cw = (READER_FACE->h + 1) / 2;
        return cw > 0 ? cw : (READER_IS_HIDPI ? 16 : 8);
    }
    return READER_IS_HIDPI ? 16 : 8;
}

static int reader_math_scale(void) {
#if defined(UGUI_TEXT_SIZES)
    if (READER_FACE) {
        int s = (READER_FACE->h + 8) / 16;
        return s > 1 ? s : 1;
    }
#endif
    return READER_IS_HIDPI ? 2 : 1;
}

static uint8_t noto_mono_aa_sample(const uint8_t *g, int sx, int sy) {
    uint8_t pair = g[(sy * 8) + (sx >> 1)];
    return (sx & 1) ? (uint8_t)(pair >> 4) : (uint8_t)(pair & 0x0Fu);
}

static void rdraw_mono_text(const ugui_surface_t *rd, int x, int y, const char *s, uint32_t len, int max_chars) {
    uint32_t n = 0;
    while (n < len && s[n] && (int)n < max_chars) n++;
    if (READER_FACE && rd && rd->aa_fb) {
        int ch = READER_FACE->h;
        int cw = rdraw_mono_cell_w(rd);
        uint32_t stride = rd->aa_stride;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t code = (uint8_t)s[i];
            if (code == ' ') continue;
            const uint8_t *g = font_noto_mono_get_aa_glyph(code);
            int gx = x + (int)i * cw;
            for (int dy = 0; dy < ch; dy++) {
                int py = y + dy;
                if (py < 0 || py >= rd->h) continue;
                uint8_t *dst = rd->aa_fb + (uint32_t)py * stride;
                uint32_t sy_q8 = (uint32_t)(dy * FONT_NOTO_MONO_H * 256) / (uint32_t)ch;
                int sy0 = (int)(sy_q8 >> 8);
                uint32_t fy = sy_q8 & 0xFFu;
                int sy1 = (sy0 + 1 < FONT_NOTO_MONO_H) ? sy0 + 1 : sy0;
                for (int dx = 0; dx < cw; dx++) {
                    int px = gx + dx;
                    if (px < 0 || px >= rd->w) continue;
                    uint32_t sx_q8 = (uint32_t)(dx * FONT_NOTO_MONO_W * 256) / (uint32_t)cw;
                    int sx0 = (int)(sx_q8 >> 8);
                    uint32_t fx = sx_q8 & 0xFFu;
                    int sx1 = (sx0 + 1 < FONT_NOTO_MONO_W) ? sx0 + 1 : sx0;
                    uint32_t a00 = noto_mono_aa_sample(g, sx0, sy0);
                    uint32_t a10 = noto_mono_aa_sample(g, sx1, sy0);
                    uint32_t a01 = noto_mono_aa_sample(g, sx0, sy1);
                    uint32_t a11 = noto_mono_aa_sample(g, sx1, sy1);
                    uint32_t a_top = a00 * (256u - fx) + a10 * fx;
                    uint32_t a_bot = a01 * (256u - fx) + a11 * fx;
                    uint32_t a_q16 = a_top * (256u - fy) + a_bot * fy;
                    uint32_t cov = (a_q16 + 15u * 128u) / (15u * 256u);
                    if (cov >= 255u) {
                        dst[px] = READER_FG_GRAY8;
                    } else if (cov > 0u) {
                        uint32_t bg = dst[px];
                        dst[px] = (uint8_t)((READER_FG_GRAY8 * cov + bg * (255u - cov) + 127u) / 255u);
                    }
                }
            }
        }
        return;
    }
    (void)ugui_text_mono(rd, x, y, s, n, false, false);
}


static bool span_has_non_ascii(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if ((uint8_t)s[i] >= 0x80u) return true;
    }
    return false;
}

/* Word wrapping and rich proportional / monospace span rendering */
static int render_wrapped_spans(reader_state_t *r, ugui_surface_t *rd, int x0, int max_w,
                                const char *text, int line_h, uint16_t extra_style,
                                int scroll_y, int win_h, int *out_doc_y,
                                int *inout_link_idx) {
    int cur_x = x0;
    int cur_y = *out_doc_y;

    md_span_iter_t iter;
    md_span_init(&iter, text);
    md_span_t span;

    while (md_span_next(&iter, &span)) {
        int link_num = 0;
        char target_url[128] = {0};
        bool is_link = false;

        if (span.style & MD_STYLE_LINK) {
            is_link = true;
            link_num = ++(*inout_link_idx);
            strncpy(target_url, span.link_url, sizeof(target_url) - 1);
        } else if (span.style & MD_STYLE_FOOTNOTE) {
            is_link = true;
            link_num = ++(*inout_link_idx);
            ksnprintf(target_url, sizeof(target_url), "#fn:%s", span.footnote_id);
        } else if (span.style & MD_STYLE_MATH) {
            char mbuf[128];
            size_t mlen = span.len < sizeof(mbuf) - 1 ? span.len : sizeof(mbuf) - 1;
            memcpy(mbuf, span.text, mlen);
            mbuf[mlen] = '\0';

            uint8_t arena_buf[2048];
            math_arena_t arena;
            math_arena_init(&arena, arena_buf, sizeof(arena_buf));
            math_box_t *box = latex_parse(mbuf, STYLE_TEXT, &arena);
            if (box) {
                int base_scale = reader_math_scale();
                latex_layout(box, base_scale);
                int math_w = box->w;

                if (cur_x + math_w > x0 + max_w && cur_x > x0) {
                    cur_x = x0;
                    cur_y += line_h;
                }

                int screen_y = cur_y - scroll_y;
                if (screen_y + line_h >= 0 && screen_y < win_h) {
                    int baseline = screen_y + line_h - 4 * base_scale;
                    if (rd->c1) {
                        latex_render_canvas1(box, cur_x, baseline, rd->c1, 1);
                    }
                    if (rd->aa_fb) {
                        latex_render_gray8(box, cur_x, baseline, rd->aa_fb, rd->aa_stride, rd->w, rd->h, READER_FG_GRAY8, READER_BG_GRAY8);
                    }
                }
                cur_x += math_w + 4 * base_scale;
            }
            continue;
        }

        /* Iterate through words inside the span */
        const char *p = span.text;
        size_t span_remaining = span.len;

        while (span_remaining > 0 && *p) {
            while (span_remaining > 0 && *p == ' ') {
                p++;
                span_remaining--;
            }
            if (span_remaining == 0 || !*p) break;

            const char *word_start = p;
            size_t word_len = 0;
            bool ended_at_tsheg = false;
            while (span_remaining > 0 && *p && *p != ' ') {
                /* Tibetan tsheg (U+0F0B, UTF-8 E0 BC 8B) is a visible syllable
                 * separator where lines may break without adding extra space. */
                if (span_remaining >= 3 &&
                    (uint8_t)p[0] == 0xE0 && (uint8_t)p[1] == 0xBC && (uint8_t)p[2] == 0x8B) {
                    word_len += 3;
                    p += 3;
                    span_remaining -= 3;
                    /* Keep an immediately following shad (U+0F0D/U+0F0E) attached. */
                    if (span_remaining >= 3 && (uint8_t)p[0] == 0xE0 && (uint8_t)p[1] == 0xBC &&
                        ((uint8_t)p[2] == 0x8D || (uint8_t)p[2] == 0x8E)) {
                        continue;
                    }
                    if (span_remaining > 0 && *p != ' ') {
                        ended_at_tsheg = true;
                    }
                    break;
                }
                word_len++;
                p++;
                span_remaining--;
            }

            unsigned word_w = 0;
            unsigned space_w = 0;
            bool is_code_mono = ((span.style & MD_STYLE_CODE) != 0) &&
                                !(READER_FACE && span_has_non_ascii(word_start, word_len));
            if (is_code_mono) {
                unsigned cw = (unsigned)rdraw_mono_cell_w(rd);
                word_w = (unsigned)word_len * cw;
                space_w = cw;
            } else if (READER_FACE) {
                word_w = (unsigned)ugui_face_width(rd, READER_FACE, word_start, (uint32_t)word_len);
                space_w = (unsigned)ugui_face_width(rd, READER_FACE, " ", 1);
            } else {
#if READER_IS_HIDPI
                word_w = font_noto_serif_text_width(word_start, (uint32_t)word_len);
                space_w = font_noto_serif_char_width(' ');
#else
                word_w = font_serif_text_width(word_start, (uint32_t)word_len);
                space_w = font_serif_char_width(' ');
#endif
            }

            if (cur_x + (int)word_w > x0 + max_w && cur_x > x0) {
                cur_x = x0;
                cur_y += line_h;
            }

            int screen_y = cur_y - scroll_y;
            if (screen_y + line_h >= 0 && screen_y < win_h) {
                if (is_code_mono) {
                    /* Monospace code pill with subtle shaded background */
                    rdraw_fill(rd, cur_x - 2, screen_y + 1, cur_x + (int)word_w + 1, screen_y + line_h - 3, 236, CANVAS1_GREY);
                    rdraw_mono_text(rd, cur_x, screen_y + 1, word_start, (uint32_t)word_len, (int)word_len);
                } else {
                    bool bold = ((span.style | extra_style) & MD_STYLE_BOLD) != 0;
                    bool italic = ((span.style | extra_style) & MD_STYLE_ITALIC) != 0;
                    rdraw_serif_text(rd, cur_x, screen_y, word_start, (uint32_t)word_len, bold, italic);
                    if (is_link) {
                        rdraw_hline(rd, cur_x, cur_x + (int)word_w, screen_y + line_h - 2, READER_LINK_LINE_GRAY8, CANVAS1_BLACK);
                    }
                }

                /* Register 2D bounding box for touch and numbered keys */
                if (is_link && r && r->link_count < READER_LINKS_MAX) {
                    reader_link_t *lnk = &r->links[r->link_count++];
                    lnk->x0 = (int16_t)cur_x;
                    lnk->y0 = (int16_t)screen_y;
                    lnk->x1 = (int16_t)(cur_x + (int)word_w);
                    lnk->y1 = (int16_t)(screen_y + line_h);
                    lnk->num = link_num;
                    strncpy(lnk->url, target_url, sizeof(lnk->url) - 1);
                    lnk->url[sizeof(lnk->url) - 1] = '\0';
                }
            }
            cur_x += (int)word_w + (ended_at_tsheg ? 0 : (int)space_w);
        }
    }

    cur_y += line_h;
    *out_doc_y = cur_y;
    return cur_y;
}

static void draw_reader_action_bar(reader_state_t *r, ugui_surface_t *rd, int w, int h) {
    if (r->fullscreen && !r->toc_mode) return;
    int sc = (int)CHROME_SCALE;
    int bar_h = reader_bar_h();
    ugui_surface_t surf = {
        .c1 = rd->c1,
        .aa_fb = rd->aa_fb,
        .aa_stride = rd->aa_stride,
        .w = w,
        .h = h,
        .scale = sc,
    };

    ugui_rect_t bar_rect = { .x = 0, .y = 0, .w = (int16_t)w, .h = (int16_t)bar_h };
    ugui_fill(&surf, bar_rect, UGUI_GRAY_TOOLBAR, CANVAS1_WHITE);
    ugui_hline(&surf, 0, w - 1, bar_h - sc, sc, UGUI_GRAY_BORDER, CANVAS1_BLACK);

    int btn_h = 18 * sc;
    int btn_y = (bar_h - btn_h - sc) / 2;
    int pad_x = 8 * sc;
    int gap = 8 * sc;
    /* The buttons' touch areas grow to a target, past the bar into the
     * page: a link drawn there still wins (reader_tap_act()). */
    ugui_set_min_target(&r->ui_ctx, ugui_target_px());

    /* Dimmed when they cannot act: Back with no history and no contents
     * view open, Top at the top. Contents is not a lit toggle (inverted is
     * the finger's): while the contents view is open it says Close. */
    ugui_rect_t b_back = { .x = (int16_t)pad_x, .y = (int16_t)btn_y, .w = (int16_t)(58 * sc), .h = (int16_t)btn_h };
    if (!r->toc_mode && r->history_depth <= 0) ugui_disable_next(&r->ui_ctx, 1);
    ugui_toolbar_button(&surf, &r->ui_ctx, b_back, "< Back", READER_UI_BACK, false);

    ugui_rect_t b_toc = {
        .x = (int16_t)(b_back.x + b_back.w + gap),
        .y = (int16_t)btn_y,
        .w = (int16_t)(72 * sc),
        .h = (int16_t)btn_h,
    };
    ugui_toolbar_button(&surf, &r->ui_ctx, b_toc, r->toc_mode ? "Close" : "Contents", READER_UI_CONTENTS, false);

    static const uint16_t k_icon_sepia[16] = {
        0x0180, 0x0180, 0x1008, 0x0810, 0x03C0, 0x0420, 0x0810, 0x781E,
        0x781E, 0x0810, 0x0420, 0x03C0, 0x0810, 0x1008, 0x0180, 0x0180,
    };
#if defined(CONFIG_BOARD_TAB5)
    static const uint16_t k_icon_rotate[16] = {
        0x0000, 0x07C0, 0x1FF0, 0x3838, 0x601C, 0x603E, 0xC01C, 0xC000,
        0xC000, 0xC000, 0x6000, 0x6004, 0x381C, 0x1FF8, 0x07E0, 0x0000,
    };
#endif

    ugui_rect_t b_top = {
        .x = (int16_t)(w - pad_x - 42 * sc),
        .y = (int16_t)btn_y,
        .w = (int16_t)(42 * sc),
        .h = (int16_t)btn_h,
    };
    if (!r->toc_mode && r->scroll_y <= 0) ugui_disable_next(&r->ui_ctx, 1);
    ugui_toolbar_button(&surf, &r->ui_ctx, b_top, "Top", READER_UI_TOP, false);

    int right_x = b_top.x;
#if defined(CONFIG_BOARD_TAB5)
    ugui_rect_t b_rot = {
        .x = (int16_t)(b_top.x - gap - 26 * sc),
        .y = (int16_t)btn_y,
        .w = (int16_t)(26 * sc),
        .h = (int16_t)btn_h,
    };
    if (b_rot.x > b_toc.x + b_toc.w + gap) {
        ugui_toolbar_button(&surf, &r->ui_ctx, b_rot, "", READER_UI_ROTATE, false);
        ugui_icon16(&surf, b_rot.x + (b_rot.w - 16 * sc) / 2, b_rot.y + (b_rot.h - 16 * sc) / 2,
                    k_icon_rotate, 1, false);
        right_x = b_rot.x;
    }
#endif

    ugui_rect_t b_full = {
        .x = (int16_t)(b_toc.x + b_toc.w + gap),
        .y = (int16_t)btn_y,
        .w = (int16_t)(44 * sc),
        .h = (int16_t)btn_h,
    };
    if (!r->toc_mode && (b_full.x + b_full.w + gap <= right_x)) {
        ugui_toolbar_button(&surf, &r->ui_ctx, b_full, "Full", READER_UI_FULL, false);
        ugui_rect_t b_sepia = {
            .x = (int16_t)(b_full.x + b_full.w + gap),
            .y = (int16_t)btn_y,
            .w = (int16_t)(26 * sc),
            .h = (int16_t)btn_h,
        };
        if (b_sepia.x + b_sepia.w + gap <= right_x) {
            bool sepia_on = settings_get("display.warmth") >= 5;
            ugui_toolbar_button(&surf, &r->ui_ctx, b_sepia, "", READER_UI_SEPIA, sepia_on);
            ugui_icon16(&surf, b_sepia.x + (b_sepia.w - 16 * sc) / 2, b_sepia.y + (b_sepia.h - 16 * sc) / 2,
                        k_icon_sepia, 1, sepia_on);
        }
    }
    ugui_draw_hover(&surf, &r->ui_ctx);
}

/* The action bar alone, drawn again over the page and shown: its buttons
 * are the reader's only hit-boxes, so they are registered afresh. */
static void reader_redraw_bar(uint8_t rid) {
    reader_state_t *r = &s_readers[rid];
    canvas1_t *c = get_reader_canvas(rid);
    if (!c) return;
    int w = r->win_w > 0 ? r->win_w : (int)c->w;
    int h = r->win_h > 0 ? r->win_h : (int)c->h;
    ugui_surface_t rd = { .c1 = c, .aa_fb = r->aa_buf, .aa_stride = r->aa_stride, .w = w, .h = h, .scale = (int)CHROME_SCALE };
    ugui_begin(&r->ui_ctx);
    draw_reader_action_bar(r, &rd, w, h);
    reader_show(rid);
}

/* The scrollbar along the right edge, over the whole height. */
static void draw_reader_scrollbar(reader_state_t *r, ugui_surface_t *rd, int w, int h) {
    if (r->total_h <= h) return;
    int sc = (int)CHROME_SCALE;
    int bar_h = reader_bar_h();
    int sb_w = 4 * sc;
    ugui_surface_t surf = {
        .c1 = rd->c1,
        .aa_fb = rd->aa_fb,
        .aa_stride = rd->aa_stride,
        .w = w,
        .h = h,
        .scale = sc,
    };
    ugui_rect_t track = {
        .x = (int16_t)(w - sb_w - 2 * sc),
        .y = (int16_t)bar_h,
        .w = (int16_t)sb_w,
        .h = (int16_t)(h - bar_h),
    };
    ugui_scrollbar(&surf, track, r->scroll_y, r->total_h, h);
}

/* The reader's view of rows clip_y0..clip_y1-1 of its w x full_h surfaces;
 * the whole of it, chrome included, when that is all rows. A strip (an
 * animation step: reader_scroll_step()) is drawn into the surfaces from its
 * first row as if it were a view that tall, further down the document, and
 * leaves the hit-boxes, links and chrome to the full draw that follows; it
 * lays out from checkpoints only, and sets strip_failed when it cannot. */
static void reader_draw_view(uint8_t rid, const canvas1_t *c, int w, int full_h, int clip_y0, int clip_y1) {
    if (w <= 0 || full_h <= 0 || rid >= READER_MAX_INSTANCES) return;
    reader_state_t *r = &s_readers[rid];
    int sc = (int)CHROME_SCALE;
    int bar_h = reader_effective_bar_h(r);
    bool strip = clip_y0 > 0 || clip_y1 < full_h;
    if (!strip) reader_metrics();
    int h = strip ? clip_y1 - clip_y0 : full_h;
    if (h <= 0) return;

    canvas1_t sub;
    if (strip && c) canvas1_init(&sub, c->fb + (uint32_t)clip_y0 * c->stride, c->stride, c->w, (unsigned)h);
    if (!strip) ugui_begin(&r->ui_ctx);

    ugui_surface_t rd = {
        .scale = (int)CHROME_SCALE,
        .c1 = strip && c ? &sub : c,
        .aa_fb = r->aa_buf ? r->aa_buf + (uint32_t)(strip ? clip_y0 : 0) * r->aa_stride : NULL,
        .aa_stride = r->aa_stride,
        .w = w,
        .h = h,
    };
    int links_before = r->link_count;

    /* Fill background */
    rdraw_fill(&rd, 0, 0, w - 1, h - 1, READER_BG_GRAY8, CANVAS1_WHITE);

    if (strip && (!r->active || !r->path[0])) { r->strip_failed = true; return; }
    if (!r->active || !r->path[0]) {
        rdraw_serif_text(&rd, READER_MARGIN_X, bar_h + 16 * sc, "No document loaded. Use 'reader <file.md>' to open.", 51, false, false);
        draw_reader_action_bar(r, &rd, w, h);
        return;
    }

#if READER_EPUB_SUPPORT
    if (r->epub_err[0]) {
        if (strip) { r->strip_failed = true; return; }
        rdraw_serif_text(&rd, READER_MARGIN_X, bar_h + 16 * sc, r->epub_err, strlen(r->epub_err), false, false);
        draw_reader_action_bar(r, &rd, w, h);
        return;
    }
#endif

    const char *file_to_open = r->path;
    char spartan_tmp[32];
    if (strncmp(r->path, "spartan://", 10) == 0 || strncmp(r->path, "SPARTAN://", 10) == 0) {
        ksnprintf(spartan_tmp, sizeof(spartan_tmp), "/ram0/spartan%d.gmi", (int)rid);
        file_to_open = spartan_tmp;
    }

    int fd = -1;
    uint32_t file_size = 0;
#if READER_EPUB_SUPPORT
    if (r->is_epub) {
        file_size = (uint32_t)r->epub_md_len;
    } else
#endif
    {
        fd = vfs_open(file_to_open, VFS_O_READ);
        if (fd < 0 && strip) { r->strip_failed = true; return; }
        if (fd < 0) {
            char err[128];
            ksnprintf(err, sizeof(err), "Cannot open document: %s", file_to_open);
            rdraw_serif_text(&rd, READER_MARGIN_X, bar_h + 16 * sc, err, strlen(err), false, false);
            draw_reader_action_bar(r, &rd, w, h);
            return;
        }
        vfs_stat_t st;
        file_size = vfs_fstat(fd, &st) == 0 ? (uint32_t)st.size : 0xffffffffu;
    }

    int margin_x = READER_MARGIN_X;
    int max_w = w - 2 * margin_x;
    if (max_w < 60) max_w = 60;

    int doc_y = bar_h + 12 * sc;
    int scroll_y = r->scroll_y + (strip ? clip_y0 : 0);
    int link_idx = 0;
    r->link_count = 0;

    uint64_t file_offset = 0;
    char read_buf[512];
    char line_fallback[MD_LINE_MAX];
    int line_pos = 0;
    bool in_code = false;
    bool in_math = false;

    /* With checkpoints for this width, from the last one at or above the
     * view, and only down to its bottom: the contents, the links' numbering
     * and the total height are the full pass's. Otherwise the whole file, and
     * checkpoints on the way (the contents view always takes the whole file). */
    bool partial = r->cp_valid && r->cp_w == w && r->cp_count > 0 && !r->toc_mode &&
                   READER_CP_TSIZE_OK(r) &&
                   r->cp_size == file_size;     /* rewritten since (editor, a new Spartan page) */
    if (strip && !partial) {
        r->strip_failed = true;
        r->link_count = links_before;
        if (fd >= 0) vfs_close(fd);
        return;
    }

    /* EPUB paragraphs can be 20+ KB single lines; allocate a 32 KB line buffer
     * off the task stack so long paragraphs do not truncate mid-UTF-8 or span. */
    uint32_t line_pages = 8u;
    char *line_buf = (char *)palloc_pages_bulk(line_pages);
    int line_cap = line_buf ? (int)(line_pages * 4096u) : (int)sizeof(line_fallback);
    if (!line_buf) {
        line_buf = line_fallback;
        line_pages = 0;
    }
    if (partial) {
        int lo = 0, hi = r->cp_count - 1;
        while (lo < hi) {                     /* the last checkpoint with doc_y <= scroll_y */
            int mid = (lo + hi + 1) / 2;
            if (r->cp[mid].doc_y <= scroll_y) lo = mid; else hi = mid - 1;
        }
        const reader_cp_t *cp = &r->cp[lo];
        file_offset = cp->off;
        doc_y = cp->doc_y;
        link_idx = cp->link_idx;
        in_code = (cp->flags & READER_CP_CODE) != 0;
        in_math = (cp->flags & READER_CP_MATH) != 0;
    } else {
        r->toc_count = 0;
        r->cp_count = 0;
        r->cp_step = h > 0 ? h / 2 : 300;
        r->cp_w = w;
        READER_CP_TSIZE_SET(r);
        r->cp_size = file_size;
    }
    int next_cp_y = doc_y;
    uint64_t line_off = file_offset;          /* where the line in line_buf began */
    bool past_view = false;

    /* Parse all document lines */
    for (;;) {
        int n = 0;
#if READER_EPUB_SUPPORT
        if (r->is_epub) {
            if (r->epub_md && file_offset < r->epub_md_len) {
                size_t rem = r->epub_md_len - (size_t)file_offset;
                n = (int)(rem > sizeof(read_buf) ? sizeof(read_buf) : rem);
                memcpy(read_buf, r->epub_md + file_offset, (size_t)n);
            }
        } else
#endif
        {
            n = vfs_pread(fd, read_buf, sizeof(read_buf), file_offset);
        }
        if (n <= 0) {
            if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                line_pos = 0;
                md_line_t md;
                if (md_parse_line(line_buf, &in_code, &in_math, &md)) {
                    if (md.type == MD_LINE_TEXT) {
                        render_wrapped_spans(r, &rd, margin_x, max_w, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
                        doc_y += 4;
                    }
                }
            }
            break;
        }

        for (int i = 0; i < n; i++) {
            char ch = read_buf[i];
            if (ch == '\r') continue;
            if (ch == '\n') {
                line_buf[line_pos] = '\0';
                line_pos = 0;
                uint64_t this_off = line_off;
                line_off = file_offset + (uint64_t)i + 1u;

                if (partial) {
                    if (doc_y > scroll_y + h) { past_view = true; break; }
                } else if (doc_y >= next_cp_y) {
                    if (r->cp_count == READER_CP_MAX) {
                        /* Full: every other one kept, twice the spacing. */
                        for (int k = 0; k < READER_CP_MAX / 2; k++) r->cp[k] = r->cp[2 * k];
                        r->cp_count = READER_CP_MAX / 2;
                        r->cp_step *= 2;
                    }
                    reader_cp_t *cp = &r->cp[r->cp_count++];
                    cp->off = (uint32_t)this_off;
                    cp->doc_y = doc_y;
                    cp->link_idx = (uint16_t)link_idx;
                    cp->flags = (uint8_t)((in_code ? READER_CP_CODE : 0u) | (in_math ? READER_CP_MATH : 0u));
                    next_cp_y = doc_y + r->cp_step;
                }

                md_line_t md;
                if (!md_parse_line(line_buf, &in_code, &in_math, &md)) continue;

                /* Collect headings into Table of Contents */
                if (md.type == MD_LINE_H1 || md.type == MD_LINE_H2 ||
                    md.type == MD_LINE_H3 || md.type == MD_LINE_H4) {
                    if (r->toc_count < READER_TOC_MAX) {
                        reader_toc_entry_t *entry = &r->toc[r->toc_count++];
                        md_heading_clean_title(md.text, entry->title, sizeof(entry->title));
                        entry->level = (md.type == MD_LINE_H1) ? 1 :
                                       (md.type == MD_LINE_H2) ? 2 :
                                       (md.type == MD_LINE_H3) ? 3 : 4;
                        entry->doc_y = doc_y;
                    }
                }

                /* If TOC mode is active, skip body rendering */
                if (r->toc_mode) {
                    continue;
                }

                int screen_y = doc_y - scroll_y;

                switch (md.type) {
                case MD_LINE_H1:
                    {
                        int h1_line_h = READER_LINE_H + (READER_IS_HIDPI ? 6 : 4);
                        render_wrapped_spans(r, &rd, margin_x, max_w, md.text, h1_line_h, MD_STYLE_BOLD, scroll_y, h, &doc_y, &link_idx);
                        int rule_sy = doc_y - scroll_y - (READER_IS_HIDPI ? 6 : 3);
                        if (rule_sy >= 0 && rule_sy < h) {
                            rdraw_hline(&rd, margin_x, margin_x + max_w, rule_sy, READER_RULE_GRAY8, CANVAS1_BLACK);
                        }
                        doc_y += (READER_IS_HIDPI ? 8 : 4);
                    }
                    break;

                case MD_LINE_H2:
                    {
                        int h2_line_h = READER_LINE_H + (READER_IS_HIDPI ? 4 : 2);
                        render_wrapped_spans(r, &rd, margin_x, max_w, md.text, h2_line_h, MD_STYLE_BOLD, scroll_y, h, &doc_y, &link_idx);
                        doc_y += (READER_IS_HIDPI ? 8 : 4);
                    }
                    break;

                case MD_LINE_H3:
                    render_wrapped_spans(r, &rd, margin_x, max_w, md.text, READER_LINE_H, MD_STYLE_BOLD, scroll_y, h, &doc_y, &link_idx);
                    doc_y += (READER_IS_HIDPI ? 6 : 4);
                    break;

                case MD_LINE_H4:
                    render_wrapped_spans(r, &rd, margin_x, max_w, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
                    doc_y += (READER_IS_HIDPI ? 4 : 2);
                    break;

                case MD_LINE_BLANK:
#if READER_IS_HIDPI
                    doc_y += 14;
#else
                    doc_y += 12;
#endif
                    break;

                case MD_LINE_ULIST:
                    {
                        int indent_px = md.indent_level * 20;
#if READER_IS_HIDPI
                        if (screen_y + 30 >= 0 && screen_y < h) {
                            rdraw_fill(&rd, margin_x + indent_px + 8, screen_y + 12, margin_x + indent_px + 14, screen_y + 18, READER_FG_GRAY8, CANVAS1_BLACK);
                        }
                        render_wrapped_spans(r, &rd, margin_x + indent_px + 24, max_w - indent_px - 24, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
#else
                        if (screen_y + 16 >= 0 && screen_y < h) {
                            rdraw_fill(&rd, margin_x + indent_px + 6, screen_y + 6, margin_x + indent_px + 9, screen_y + 9, READER_FG_GRAY8, CANVAS1_BLACK);
                        }
                        render_wrapped_spans(r, &rd, margin_x + indent_px + 16, max_w - indent_px - 16, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
#endif
                        doc_y += 4;
                    }
                    break;

                case MD_LINE_OLIST:
                    {
                        int indent_px = md.indent_level * 20;
                        char num_str[16];
                        ksnprintf(num_str, sizeof(num_str), "%d.", md.order_num);
                        int pfx_w = (READER_IS_HIDPI ? 32 : 20);
                        if (screen_y + READER_LINE_H >= 0 && screen_y < h) {
                            rdraw_serif_text(&rd, margin_x + indent_px, screen_y, num_str, strlen(num_str), true, false);
                        }
                        render_wrapped_spans(r, &rd, margin_x + indent_px + pfx_w, max_w - indent_px - pfx_w, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
                        doc_y += 4;
                    }
                    break;

                case MD_LINE_QUOTE:
                    {
                        int q_start = doc_y;
#if READER_IS_HIDPI
                        render_wrapped_spans(r, &rd, margin_x + 24, max_w - 24, md.text, READER_LINE_H, MD_STYLE_ITALIC, scroll_y, h, &doc_y, &link_idx);
                        int q_h = doc_y - q_start;
                        int q_sy = q_start - scroll_y;
                        if (q_sy + q_h >= 0 && q_sy < h) {
                            rdraw_fill(&rd, margin_x + 6, q_sy, margin_x + 10, q_sy + q_h - 4, READER_QUOTE_BAR_GRAY8, CANVAS1_GREY);
                        }
                        doc_y += 10;
#else
                        render_wrapped_spans(r, &rd, margin_x + 16, max_w - 16, md.text, READER_LINE_H, MD_STYLE_ITALIC, scroll_y, h, &doc_y, &link_idx);
                        int q_h = doc_y - q_start;
                        int q_sy = q_start - scroll_y;
                        if (q_sy + q_h >= 0 && q_sy < h) {
                            rdraw_fill(&rd, margin_x + 4, q_sy, margin_x + 6, q_sy + q_h - 2, READER_QUOTE_BAR_GRAY8, CANVAS1_GREY);
                        }
                        doc_y += 6;
#endif
                    }
                    break;

                case MD_LINE_ALERT:
                    {
                        const char *badge = "NOTE";
                        uint8_t bar_gray = 120;
                        if (md.alert_type == MD_ALERT_TIP) { badge = "TIP"; bar_gray = 90; }
                        else if (md.alert_type == MD_ALERT_WARNING) { badge = "WARNING"; bar_gray = 40; }
                        else if (md.alert_type == MD_ALERT_IMPORTANT) { badge = "IMPORTANT"; bar_gray = 50; }
                        else if (md.alert_type == MD_ALERT_CAUTION) { badge = "CAUTION"; bar_gray = 20; }

                        int q_start = doc_y;
                        char full_badge[32];
                        ksnprintf(full_badge, sizeof(full_badge), "[%s]", badge);
                        if (screen_y + READER_LINE_H >= 0 && screen_y < h) {
                            rdraw_serif_text(&rd, margin_x + 16, screen_y, full_badge, strlen(full_badge), true, false);
                        }
                        doc_y += READER_LINE_H;
                        render_wrapped_spans(r, &rd, margin_x + 16, max_w - 16, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
                        int q_h = doc_y - q_start;
                        int q_sy = q_start - scroll_y;
                        if (q_sy + q_h >= 0 && q_sy < h) {
                            rdraw_fill(&rd, margin_x + 4, q_sy, margin_x + 8, q_sy + q_h - 2, bar_gray, CANVAS1_BLACK);
                        }
                        doc_y += 8;
                    }
                    break;

                case MD_LINE_HRULE:
                    if (screen_y + 12 >= 0 && screen_y < h) {
                        rdraw_hline(&rd, margin_x, margin_x + max_w, screen_y + 6, READER_RULE_GRAY8, CANVAS1_BLACK);
                    }
                    doc_y += 18;
                    break;

                case MD_LINE_TABLE_HDR:
                case MD_LINE_TABLE_ROW:
                    {
                        char cells[MD_TABLE_MAX_COLS][MD_CELL_MAX];
                        int cols = md_parse_table_cells(line_buf, cells);
                        if (cols > 0) {
                            int cell_w = max_w / cols;
                            int row_h = READER_LINE_H + 4;
                            if (screen_y + row_h >= 0 && screen_y < h) {
                                bool is_hdr = (md.type == MD_LINE_TABLE_HDR);
                                uint8_t bg_gray = is_hdr ? 238 : 250;
                                rdraw_fill(&rd, margin_x, screen_y, margin_x + max_w, screen_y + row_h - 1, bg_gray, CANVAS1_GREY);
                                for (int c = 0; c < cols; c++) {
                                    int cx = margin_x + c * cell_w + 6;
                                    rdraw_serif_text(&rd, cx, screen_y + 2, cells[c], strlen(cells[c]), is_hdr, false);
                                    if (c > 0) {
                                        rdraw_fill(&rd, margin_x + c * cell_w, screen_y, margin_x + c * cell_w, screen_y + row_h - 1, 210, CANVAS1_BLACK);
                                    }
                                }
                                rdraw_hline(&rd, margin_x, margin_x + max_w, screen_y + row_h - 1, is_hdr ? READER_RULE_GRAY8 : 210, CANVAS1_BLACK);
                            }
                            doc_y += row_h;
                        }
                    }
                    break;

                case MD_LINE_TABLE_SEP:
                    /* Handled by table header bottom border */
                    break;

                case MD_LINE_FOOTNOTE_DEF:
                    {
                        char badge[32];
                        ksnprintf(badge, sizeof(badge), "[^%s] ", md.footnote_id);
                        int pfx_w = (READER_IS_HIDPI ? 48 : 32);
                        if (screen_y + READER_LINE_H >= 0 && screen_y < h) {
                            rdraw_serif_text(&rd, margin_x, screen_y, badge, strlen(badge), true, false);
                        }
                        render_wrapped_spans(r, &rd, margin_x + pfx_w, max_w - pfx_w, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
                        doc_y += 6;
                    }
                    break;

                case MD_LINE_MATH_TOGGLE:
                    doc_y += 8;
                    break;

                case MD_LINE_MATH_TEXT:
                    {
                        uint8_t arena_buf[4096];
                        math_arena_t arena;
                        math_arena_init(&arena, arena_buf, sizeof(arena_buf));
                        math_box_t *box = latex_parse(md.text, STYLE_DISPLAY, &arena);
                        int base_scale = reader_math_scale();
                        if (box) {
                            latex_layout(box, base_scale);
                            int math_w = box->w;
                            int total_h = box->h + box->d + 16 * base_scale;
                            int cx = margin_x + (max_w - math_w) / 2;
                            if (cx < margin_x) cx = margin_x;

                            int my = doc_y - scroll_y;
                            if (my + total_h >= 0 && my < h) {
                                int baseline = my + box->h + 8 * base_scale;
                                if (rd.c1) {
                                    latex_render_canvas1(box, cx, baseline, rd.c1, 1);
                                }
                                if (rd.aa_fb) {
                                    latex_render_gray8(box, cx, baseline, rd.aa_fb, rd.aa_stride, rd.w, rd.h, READER_FG_GRAY8, READER_BG_GRAY8);
                                }
                            }
                            doc_y += total_h;
                        } else {
                            int line_len = strlen(md.text);
                            int cw = rdraw_mono_cell_w(&rd);
                            int txt_w = line_len * cw;
                            int cx = margin_x + (max_w - txt_w) / 2;
                            if (cx < margin_x) cx = margin_x;
                            if (screen_y + READER_LINE_H >= 0 && screen_y < h) {
                                rdraw_fill(&rd, cx - 8, screen_y - 2, cx + txt_w + 8, screen_y + READER_LINE_H, 244, CANVAS1_GREY);
                                rdraw_mono_text(&rd, cx, screen_y, md.text, line_len, max_w / cw);
                            }
                            doc_y += READER_LINE_H + 8;
                        }
                    }
                    break;

                case MD_LINE_CODE_TOGGLE:
                    doc_y += (READER_FACE && rd.aa_fb) ? (READER_LINE_H / 2) : (READER_IS_HIDPI ? 16 : 8);
                    break;

                case MD_LINE_CODE_TEXT: {
                    int cw = rdraw_mono_cell_w(&rd);
                    int pad_x = READER_IS_HIDPI ? 16 : 12;
                    int code_h = (READER_FACE && rd.aa_fb) ? READER_LINE_H : (READER_IS_HIDPI ? 34 : 16);
                    if (screen_y + code_h >= 0 && screen_y < h) {
                        rdraw_mono_text(&rd, margin_x + pad_x, screen_y, md.text, strlen(md.text), (max_w - pad_x) / cw);
                    }
                    doc_y += code_h;
                    break;
                }

                case MD_LINE_GEM_LINK:
                    link_idx++;
#if READER_IS_HIDPI
                    if (screen_y + 36 >= 0 && screen_y < h) {
                        if (r->link_count < READER_LINKS_MAX) {
                            r->links[r->link_count].x0 = (int16_t)margin_x;
                            r->links[r->link_count].y0 = (int16_t)screen_y;
                            r->links[r->link_count].x1 = (int16_t)(margin_x + max_w);
                            r->links[r->link_count].y1 = (int16_t)(screen_y + 36);
                            r->links[r->link_count].num = link_idx;
                            strncpy(r->links[r->link_count].url, md.url, sizeof(r->links[0].url) - 1);
                            r->links[r->link_count].url[sizeof(r->links[0].url) - 1] = '\0';
                            r->link_count++;
                        }

                        char prefix[16];
                        ksnprintf(prefix, sizeof(prefix), "[%d] ", link_idx);
                        int px_w = rdraw_serif_text(&rd, margin_x, screen_y, prefix, strlen(prefix), true, false);
                        const char *lbl = md.label[0] ? md.label : md.url;
                        int lbl_w = rdraw_serif_text(&rd, margin_x + px_w, screen_y, lbl, strlen(lbl), true, false);
                        rdraw_hline(&rd, margin_x + px_w, margin_x + px_w + lbl_w, screen_y + 33, READER_LINK_LINE_GRAY8, CANVAS1_BLACK);
                    }
                    doc_y += 42;
#else
                    if (screen_y + 20 >= 0 && screen_y < h) {
                        if (r->link_count < READER_LINKS_MAX) {
                            r->links[r->link_count].x0 = (int16_t)margin_x;
                            r->links[r->link_count].y0 = (int16_t)screen_y;
                            r->links[r->link_count].x1 = (int16_t)(margin_x + max_w);
                            r->links[r->link_count].y1 = (int16_t)(screen_y + 18);
                            r->links[r->link_count].num = link_idx;
                            strncpy(r->links[r->link_count].url, md.url, sizeof(r->links[0].url) - 1);
                            r->links[r->link_count].url[sizeof(r->links[0].url) - 1] = '\0';
                            r->link_count++;
                        }

                        char prefix[16];
                        ksnprintf(prefix, sizeof(prefix), "[%d] ", link_idx);
                        int px_w = rdraw_serif_text(&rd, margin_x, screen_y, prefix, strlen(prefix), true, false);
                        const char *lbl = md.label[0] ? md.label : md.url;
                        int lbl_w = rdraw_serif_text(&rd, margin_x + px_w, screen_y, lbl, strlen(lbl), true, false);
                        rdraw_hline(&rd, margin_x + px_w, margin_x + px_w + lbl_w, screen_y + 16, READER_LINK_LINE_GRAY8, CANVAS1_BLACK);
                    }
                    doc_y += 22;
#endif
                    break;

                case MD_LINE_TOC:
                    doc_y += 8;
                    break;

                case MD_LINE_TEXT:
                default:
                    render_wrapped_spans(r, &rd, margin_x, max_w, md.text, READER_LINE_H, 0, scroll_y, h, &doc_y, &link_idx);
                    doc_y += 4;
                    break;
                }
            } else if (line_pos < line_cap - 1) {
                line_buf[line_pos++] = ch;
            }
        }
        if (past_view) break;
        file_offset += (uint64_t)n;
    }

    if (line_pages > 0) palloc_free(line_buf, line_pages);
    if (fd >= 0) vfs_close(fd);
    if (!partial) {
        r->total_h = doc_y + 32;
        r->cp_valid = !r->toc_mode;
    }

    /* Render interactive Table of Contents overlay if in TOC mode */
    if (r->toc_mode) {
        rdraw_fill(&rd, 0, 0, w - 1, h - 1, READER_BG_GRAY8, CANVAS1_WHITE);
        int ty = bar_h + 12 * sc;
        rdraw_serif_text(&rd, margin_x, ty, "Table of Contents (tap heading or 'Contents' to return)", 55, true, false);
        rdraw_hline(&rd, margin_x, margin_x + max_w, ty + READER_LINE_H + 2 * sc, READER_RULE_GRAY8, CANVAS1_BLACK);
        ty += READER_LINE_H + 12 * sc;

        r->link_count = 0;
#if READER_EPUB_SUPPORT
        if (r->is_epub && r->epub_book) {
            for (int i = 0; i < r->epub_book->spine_count; i++) {
                const epub_spine_item_t *sp = &r->epub_book->spine[i];
                char label[96];
                if (sp->title[0]) {
                    ksnprintf(label, sizeof(label), "[%d] %s", i + 1, sp->title);
                } else {
                    ksnprintf(label, sizeof(label), "[%d] Section %d", i + 1, i + 1);
                }
                bool cur = (i == r->epub_chapter);
                (void)rdraw_serif_text(&rd, margin_x, ty, label, strlen(label), cur, false);
                if (r->link_count < READER_LINKS_MAX) {
                    reader_link_t *lnk = &r->links[r->link_count++];
                    lnk->x0 = (int16_t)margin_x;
                    lnk->y0 = (int16_t)ty;
                    lnk->x1 = (int16_t)(margin_x + max_w);
                    lnk->y1 = (int16_t)(ty + READER_LINE_H);
                    lnk->num = i + 1;
                    ksnprintf(lnk->url, sizeof(lnk->url), "#epub:%d", i);
                }
                ty += READER_LINE_H + 4 * sc;
                if (ty >= h - 16 * sc) break;
            }
        } else
#endif
        if (r->toc_count == 0) {
            rdraw_serif_text(&rd, margin_x, ty, "No headings found in document.", 30, false, false);
        } else {
            for (int i = 0; i < r->toc_count && i < READER_TOC_MAX; i++) {
                reader_toc_entry_t *entry = &r->toc[i];
                int indent_px = (entry->level > 1) ? (entry->level - 1) * 16 * sc : 0;
                int entry_x = margin_x + indent_px;
                char num_prefix[16];
                ksnprintf(num_prefix, sizeof(num_prefix), "[%d] ", i + 1);
                int pfx_w = rdraw_serif_text(&rd, entry_x, ty, num_prefix, strlen(num_prefix), true, false);
                (void)rdraw_serif_text(&rd, entry_x + pfx_w, ty, entry->title, strlen(entry->title), false, false);

                if (r->link_count < READER_LINKS_MAX) {
                    reader_link_t *lnk = &r->links[r->link_count++];
                    lnk->x0 = (int16_t)margin_x;
                    lnk->y0 = (int16_t)ty;
                    lnk->x1 = (int16_t)(margin_x + max_w);
                    lnk->y1 = (int16_t)(ty + READER_LINE_H);
                    lnk->num = i + 1;
                    char slug[64];
                    md_heading_slug(entry->title, slug, sizeof(slug));
                    ksnprintf(lnk->url, sizeof(lnk->url), "#%s", slug);
                }
                ty += READER_LINE_H + 4 * sc;
                if (ty >= h - 16 * sc) break;
            }
        }
        draw_reader_action_bar(r, &rd, w, h);
        s_reader_seq[rid]++;
        return;
    }

    if (strip) {
        r->link_count = links_before;          /* those found were in strip coordinates */
        s_reader_seq[rid]++;
        return;
    }
    if (!r->fullscreen) {
        draw_reader_scrollbar(r, &rd, w, h);
        draw_reader_action_bar(r, &rd, w, h);
    }
    r->drawn_scroll_y = r->scroll_y;
    s_reader_seq[rid]++;
}

void reader_draw(uint8_t rid, const canvas1_t *c, int w, int h) {
    reader_draw_view(rid, c, w, h, 0, h);
}

/* An animation step from where the surfaces show the document to scroll_y:
 * the content below the action bar moved in both surfaces, only the rows
 * that came into view laid out and drawn, and the scrollbar. False when that
 * cannot be done (no checkpoints, the contents view, a step as tall as the
 * view): the caller draws it whole. */
static bool reader_scroll_step(uint8_t rid, const canvas1_t *c, int w, int h) {
    reader_state_t *r = &s_readers[rid];
    int bar_h = reader_effective_bar_h(r);
    int dy = r->scroll_y - r->drawn_scroll_y;
    int area = h - bar_h;
    /* The 1-bpp grey pattern alternates by row (canvas1.c: x + y odd), so a
     * move by an odd number of rows would put moved content out of phase
     * with freshly drawn content: an odd step goes one row less (the next
     * step or the full draw after the animation catches up). */
    if (r->drawn_scroll_y >= 0 && (dy & 1)) {
        r->scroll_y -= dy > 0 ? 1 : -1;
        dy = r->scroll_y - r->drawn_scroll_y;
    }
    if (dy == 0) return true;
    if (r->drawn_scroll_y < 0 || !r->cp_valid || r->cp_w != w || !READER_CP_TSIZE_OK(r) || r->toc_mode || !c ||
        dy >= area || -dy >= area || (r->aa_buf && (w > r->aa_w || h > r->aa_h)))
        return false;
    /* Rows y <- y + dy, front to back for dy > 0 and back to front otherwise. */
    for (int k = 0; k < area - (dy > 0 ? dy : -dy); k++) {
        int y = dy > 0 ? bar_h + k : h - 1 - k;
        if (r->aa_buf)
            memcpy(r->aa_buf + (uint32_t)y * r->aa_stride, r->aa_buf + (uint32_t)(y + dy) * r->aa_stride, (size_t)w);
        memcpy(c->fb + (uint32_t)y * c->stride, c->fb + (uint32_t)(y + dy) * c->stride, c->stride);
    }
    r->strip_failed = false;
    /* A strip starts on an even row, for the same pattern (bar_h is even). */
    if (dy > 0) reader_draw_view(rid, c, w, h, (h - dy) & ~1, h);
    else reader_draw_view(rid, c, w, h, bar_h, bar_h - dy);
    if (r->strip_failed) return false;
    ugui_surface_t rd = { .c1 = c, .aa_fb = r->aa_buf, .aa_stride = r->aa_stride, .w = w, .h = h, .scale = (int)CHROME_SCALE };
    if (!r->fullscreen) draw_reader_scrollbar(r, &rd, w, h);
    r->drawn_scroll_y = r->scroll_y;
    return true;
}

uint32_t reader_get_seq(uint8_t rid) {
    if (rid >= READER_MAX_INSTANCES) return 0;
    return s_reader_seq[rid];
}
